#include "HostedPluginBlockProcessor.h"
#include "BlockKinds.h"

#include <cmath>
#include <cstring>

namespace {

std::atomic<juce::uint32> attention{0};

// Where the last reference to an instance sends it. Always a later turn of
// the message loop, never inline: JUCE requires VST3 and AU instances to be
// destroyed on the message thread, and the last reference can be dropped
// anywhere (on a loader thread, or with the chain lock held while a restore
// retires a block), where tearing a plugin down would also stall the audio
// thread. Only a process with no message loop left deletes in place.
void destroyOnMessageThread(HostedPluginInstance* instance) {
  if (juce::MessageManager::getInstanceWithoutCreating() != nullptr &&
      juce::MessageManager::callAsync([instance] { delete instance; }))
    return;
  delete instance;
}

bool tryLayout(juce::AudioPluginInstance& plugin, const juce::AudioChannelSet& in,
               const juce::AudioChannelSet& out, bool disableOtherBuses) {
  auto layout = plugin.getBusesLayout();
  if (layout.inputBuses.isEmpty() || layout.outputBuses.isEmpty())
    return false;
  layout.inputBuses.getReference(0) = in;
  layout.outputBuses.getReference(0) = out;
  if (disableOtherBuses) {
    // Sidechains and extra outputs: nothing in a lane feeds or reads them.
    for (int i = 1; i < layout.inputBuses.size(); ++i)
      layout.inputBuses.getReference(i) = juce::AudioChannelSet::disabled();
    for (int i = 1; i < layout.outputBuses.size(); ++i)
      layout.outputBuses.getReference(i) = juce::AudioChannelSet::disabled();
  }
  return plugin.checkBusesLayoutSupported(layout) && plugin.setBusesLayout(layout);
}

}  // namespace

// ####################
// INSTANCE
// ####################
HostedPluginInstance::HostedPluginInstance(std::unique_ptr<juce::AudioPluginInstance> instance,
                                           const HostedPluginIdentity& identity)
    : plugin(std::move(instance)), pluginIdentity(identity),
      messageThread(std::this_thread::get_id()) {
  plugin->setPlayHead(&playHead);
  plugin->addListener(this);
}

HostedPluginInstance::~HostedPluginInstance() {
  plugin->removeListener(this);
  plugin->setPlayHead(nullptr);
  if (preparedBlock > 0)
    plugin->releaseResources();
  plugin.reset();
}

juce::uint32 HostedPluginInstance::attentionCount() noexcept { return attention.load(); }
void HostedPluginInstance::raiseAttention() noexcept { attention.fetch_add(1); }

bool HostedPluginInstance::negotiateLayout(juce::String& error) {
  const auto mono = juce::AudioChannelSet::mono();
  const auto stereo = juce::AudioChannelSet::stereo();
  const std::pair<juce::AudioChannelSet, juce::AudioChannelSet> wanted[] = {
      {stereo, stereo}, {mono, stereo}, {mono, mono}};

  // Without the other buses first; some plugins refuse to switch a
  // sidechain off, so then with whatever they have.
  bool negotiated = false;
  for (const bool disableOtherBuses : {true, false}) {
    for (const auto& [in, out] : wanted)
      if (tryLayout(*plugin, in, out, disableOtherBuses)) {
        negotiated = true;
        break;
      }
    if (negotiated)
      break;
  }

  inChannels = plugin->getMainBusNumInputChannels();
  outChannels = plugin->getMainBusNumOutputChannels();
  // A plugin that takes none of the three but already sits in a mono or
  // stereo layout of its own is still usable as it stands.
  if (inChannels < 1 || inChannels > 2 || outChannels < 1 || outChannels > 2) {
    error = "no mono or stereo layout (" + juce::String(inChannels) + " in, " +
            juce::String(outChannels) + " out)";
    return false;
  }
  bufferChannels = juce::jmax(2, plugin->getTotalNumInputChannels(),
                              plugin->getTotalNumOutputChannels());
  // The audio thread wraps the scratch channels in a juce::AudioBuffer per
  // block, which only stays allocation-free up to its built-in channel list.
  if (bufferChannels > 32) {
    error = "too many channels across its buses (" + juce::String(bufferChannels) + ")";
    return false;
  }
  return true;
}

void HostedPluginInstance::buildParameterTable() {
  const auto& all = plugin->getParameters();
  rowOfPluginParameter.assign(static_cast<size_t>(all.size()), -1);

  for (int i = 0; i < all.size(); ++i) {
    juce::AudioProcessorParameter* parameter = all.getUnchecked(i);
    if (parameter == nullptr || !parameter->isAutomatable())
      continue;
    // The plugin's own bypass switch is not offered: the block has one
    // (enabled), which also keeps the latency compensation in place.
    if (parameter == plugin->getBypassParameter())
      continue;
    if (static_cast<int>(table.size()) >= HostedPluginBlockProcessor::kMaxParameters)
      break;

    // The plugin's own id where the format has one: it survives the plugin
    // adding or reordering parameters, which an index does not.
    juce::String id;
    if (auto* hostedParameter = dynamic_cast<juce::HostedAudioProcessorParameter*>(parameter))
      id = hostedParameter->getParameterID();
    const auto taken = [&](const juce::String& candidate) {
      return candidate.isEmpty() || findBlockParam(commonBlockParams(), candidate) >= 0 ||
             findBlockParam(table, candidate) >= 0;
    };
    if (taken(id))
      id = juce::String(i);
    if (taken(id))
      id = "param" + juce::String(i);

    BlockParamSpec spec = BlockParamSpec::floatParam(id, parameter->getName(64),
                                                     parameter->getLabel(), 0.0f, 1.0f,
                                                     parameter->getDefaultValue());
    if (parameter->isBoolean()) {
      spec.type = BlockParamType::Bool;
      spec.step = 1.0f;
    } else if (parameter->isDiscrete()) {
      const int steps = parameter->getNumSteps();
      // A grid for real switches and selectors; a "discrete" parameter
      // with more steps than this is continuous for every purpose here.
      if (steps >= 2 && steps <= 16384)
        spec.step = 1.0f / static_cast<float>(steps - 1);
    }
    spec.defaultValue = spec.clamp(spec.defaultValue);

    rowOfPluginParameter[static_cast<size_t>(i)] = static_cast<int>(table.size());
    table.push_back(spec);
    exposed.push_back(parameter);
  }

  const size_t rows = juce::jmax<size_t>(1, table.size());
  mirror = std::make_unique<std::atomic<float>[]>(rows);
  pending = std::make_unique<std::atomic<float>[]>(rows);
  dirty = std::make_unique<std::atomic<bool>[]>(rows);
  for (size_t i = 0; i < rows; ++i) {
    mirror[i].store(0.0f);
    pending[i].store(0.0f);
    dirty[i].store(false);
  }
  refreshMirrors();
}

void HostedPluginInstance::refreshMirrors() {
  for (size_t i = 0; i < exposed.size(); ++i)
    mirror[i].store(exposed[i]->getValue(), std::memory_order_relaxed);
}

void HostedPluginInstance::refreshTail() {
  double seconds = plugin->getTailLengthSeconds();
  // "Infinite" (a freeze, a feedback loop) and absurd reports alike.
  if (!std::isfinite(seconds) || seconds > HostedPluginBlockProcessor::kMaxTailSeconds)
    seconds = HostedPluginBlockProcessor::kMaxTailSeconds;
  tail.store(juce::jmax(0.0, seconds));
}

bool HostedPluginInstance::isPreparedFor(double sampleRate, int maxBlockSize) const noexcept {
  return prepared.load() && juce::exactlyEqual(preparedRate, sampleRate) &&
         preparedBlock == maxBlockSize;
}

void HostedPluginInstance::prepare(double sampleRate, int maxBlockSize) {
  maxBlockSize = juce::jmax(1, maxBlockSize);

  if (!onMessageThread()) {
    // Nothing may be called on the plugin from here. A plugin that already
    // runs at these settings keeps running (it only misses a reset);
    // otherwise it steps aside until the message thread has prepared it.
    if (isPreparedFor(sampleRate, maxBlockSize))
      return;
    prepared.store(false);
    prepareWanted.store(true);
    raiseAttention();
    return;
  }

  prepareWanted.store(false);
  try {
    if (isPreparedFor(sampleRate, maxBlockSize)) {
      plugin->reset();
    } else {
      prepared.store(false);
      if (preparedBlock > 0)
        plugin->releaseResources();
      plugin->setRateAndBufferSizeDetails(sampleRate, maxBlockSize);
      plugin->prepareToPlay(sampleRate, maxBlockSize);
      scratch.setSize(bufferChannels, maxBlockSize, false, false, true);
      midi.ensureSize(2048);
      preparedRate = sampleRate;
      preparedBlock = maxBlockSize;
    }
    failed.store(false);
  } catch (...) {
    failed.store(true);
  }

  // prepare() latches (see BlockProcessor::latencySamples): every caller
  // refreshes the chain's compensation before audio runs again.
  reported.store(juce::jmax(0, plugin->getLatencySamples()));
  latched.store(reported.load());
  refreshTail();
  prepared.store(true);
}

void HostedPluginInstance::applyState(const juce::MemoryBlock& chunk) {
  if (!onMessageThread())
    return;
  // Whatever was set and not yet applied belongs to the state being replaced.
  for (size_t i = 0; i < exposed.size(); ++i)
    dirty[i].store(false);
  try {
    plugin->setStateInformation(chunk.getData(), static_cast<int>(chunk.getSize()));
  } catch (...) {
    juce::Logger::writeToLog("[Plugin] " + pluginIdentity.name + " threw while restoring its state");
  }
  refreshMirrors();
  {
    juce::ScopedLock scoped(stateLock);
    chunkCache = chunk;
  }
  // The plugin reports the state it was just given as a change; the cache
  // already holds it.
  stateDirty.store(false);
  reported.store(juce::jmax(0, plugin->getLatencySamples()));
  refreshTail();
}

void HostedPluginInstance::captureState() {
  juce::MemoryBlock fresh;
  try {
    plugin->getStateInformation(fresh);
  } catch (...) {
    juce::Logger::writeToLog("[Plugin] " + pluginIdentity.name + " threw while saving its state");
    return;
  }
  juce::ScopedLock scoped(stateLock);
  chunkCache = std::move(fresh);
}

juce::MemoryBlock HostedPluginInstance::cachedState() const {
  juce::ScopedLock scoped(stateLock);
  return chunkCache;
}

HostedPluginInstance::ServiceResult
HostedPluginInstance::serviceOnMessageThread(bool forceStateRefresh) {
  ServiceResult result;
  if (!onMessageThread())
    return result;

  // Sets made off this thread are normally applied by the next audio block;
  // with no audio running this is what applies them.
  if (anyDirty.exchange(false))
    flushPendingParameters();

  if (stateDirty.exchange(false) || forceStateRefresh) {
    const double tailBefore = tail.load();
    refreshMirrors();
    captureState();
    refreshTail();
    result.tailChanged = !juce::exactlyEqual(tail.load(), tailBefore);
  }
  result.latencyPending = latencyChangePending();
  result.needsPrepare = prepareWanted.load();
  return result;
}

std::unique_ptr<juce::AudioProcessorEditor> HostedPluginInstance::createEditor() {
  if (!onMessageThread() || plugin == nullptr || failed.load())
    return nullptr;
  // One editor at a time: JUCE keeps the active one on the processor, and
  // createEditorIfNeeded would hand that same object to a second owner.
  if (plugin->getActiveEditor() != nullptr)
    return nullptr;
  try {
    if (!plugin->hasEditor())
      return nullptr;
    return std::unique_ptr<juce::AudioProcessorEditor>(plugin->createEditorIfNeeded());
  } catch (...) {
    return nullptr;
  }
}

juce::String HostedPluginInstance::parameterText(int index) const {
  // The VST3 wrapper takes the message-manager lock to ask the plugin.
  if (!onMessageThread() || index < 0 || index >= static_cast<int>(exposed.size()))
    return {};
  juce::AudioProcessorParameter* parameter = exposed[static_cast<size_t>(index)];
  try {
    return parameter->getText(getParameter(index), 64);
  } catch (...) {
    return {};
  }
}

float HostedPluginInstance::getParameter(int index) const noexcept {
  if (index < 0 || index >= static_cast<int>(exposed.size()))
    return 0.0f;
  return mirror[static_cast<size_t>(index)].load(std::memory_order_relaxed);
}

void HostedPluginInstance::setParameter(int index, float value) noexcept {
  if (index < 0 || index >= static_cast<int>(exposed.size()))
    return;
  const auto row = static_cast<size_t>(index);
  const float clamped = table[row].clamp(value);
  mirror[row].store(clamped, std::memory_order_relaxed);

  if (onMessageThread()) {
    // Straight to the plugin, as a host's UI thread would: the plugin hears
    // of it now, also when no audio is running, and anything it changes in
    // response (its latency) is reported before this returns.
    dirty[row].store(false);
    try {
      exposed[row]->setValue(clamped);
    } catch (...) {
    }
  } else {
    // Any other thread, the audio thread included: two stores, and the next
    // audio block (or the message thread, whichever comes first) applies it.
    pending[row].store(clamped, std::memory_order_relaxed);
    dirty[row].store(true, std::memory_order_release);
    anyDirty.store(true, std::memory_order_release);
  }
  stateDirty.store(true, std::memory_order_relaxed);
  raiseAttention();
}

void HostedPluginInstance::flushPendingParameters() {
  for (size_t i = 0; i < exposed.size(); ++i)
    if (dirty[i].exchange(false, std::memory_order_acquire)) {
      try {
        exposed[i]->setValue(pending[i].load(std::memory_order_relaxed));
      } catch (...) {
      }
    }
}

void HostedPluginInstance::audioProcessorParameterChanged(juce::AudioProcessor*,
                                                          int parameterIndex, float newValue) {
  if (parameterIndex < 0 || parameterIndex >= static_cast<int>(rowOfPluginParameter.size()))
    return;
  const int row = rowOfPluginParameter[static_cast<size_t>(parameterIndex)];
  if (row >= 0)
    mirror[static_cast<size_t>(row)].store(newValue, std::memory_order_relaxed);
}

void HostedPluginInstance::audioProcessorChanged(juce::AudioProcessor* processor,
                                                 const ChangeDetails& details) {
  if (details.latencyChanged)
    reported.store(juce::jmax(0, processor->getLatencySamples()), std::memory_order_relaxed);
  if (details.programChanged || details.nonParameterStateChanged || details.parameterInfoChanged)
    stateDirty.store(true, std::memory_order_relaxed);
  raiseAttention();
}

void HostedPluginInstance::audioProcessorParameterChangeGestureEnd(juce::AudioProcessor*, int) {
  stateDirty.store(true, std::memory_order_relaxed);
  raiseAttention();
}

BlockProcessResult HostedPluginInstance::process(juce::AudioBuffer<float>& buffer,
                                                 const BlockTransport& transport) {
  if (failed.load(std::memory_order_relaxed))
    return BlockProcessResult::Failed;
  if (!prepared.load(std::memory_order_acquire))
    return BlockProcessResult::Idle;

  const int laneChannels = buffer.getNumChannels();
  const int numSamples = buffer.getNumSamples();
  if (laneChannels < 1 || numSamples < 1)
    return BlockProcessResult::Processed;

  try {
    if (anyDirty.exchange(false, std::memory_order_acquire))
      flushPendingParameters();

    {
      juce::AudioPlayHead::PositionInfo position;
      position.setBpm(transport.bpm);
      position.setTimeSignature(juce::AudioPlayHead::TimeSignature{transport.timeSigNumerator,
                                                                   transport.timeSigDenominator});
      position.setIsPlaying(transport.playing);
      position.setTimeInSeconds(transport.timeSeconds);
      position.setTimeInSamples(
          static_cast<juce::int64>(std::llround(transport.timeSeconds * preparedRate)));
      position.setPpqPosition(transport.ppqPosition);
      position.setPpqPositionOfLastBarStart(transport.ppqLastBarStart);
      playHead.position = position;
    }

    float* const* work = scratch.getArrayOfWritePointers();

    // The chain never hands a base-rate kind more than the block it was
    // prepared for; slicing keeps a plugin inside its promised maximum even
    // if that ever stops being true.
    for (int offset = 0; offset < numSamples; offset += preparedBlock) {
      const int count = juce::jmin(preparedBlock, numSamples - offset);
      const auto bytes = sizeof(float) * static_cast<size_t>(count);

      // Lane -> plugin input. A one-channel lane feeds both sides of a
      // stereo input; a two-channel lane folds into a mono input.
      const float* laneLeft = buffer.getReadPointer(0) + offset;
      const float* laneRight = laneChannels > 1 ? buffer.getReadPointer(1) + offset : laneLeft;
      if (inChannels >= 2) {
        std::memcpy(work[0], laneLeft, bytes);
        std::memcpy(work[1], laneRight, bytes);
      } else if (laneChannels > 1) {
        for (int i = 0; i < count; ++i)
          work[0][i] = 0.5f * (laneLeft[i] + laneRight[i]);
      } else {
        std::memcpy(work[0], laneLeft, bytes);
      }
      for (int channel = inChannels; channel < bufferChannels; ++channel)
        std::memset(work[channel], 0, bytes);

      juce::AudioBuffer<float> view(work, bufferChannels, count);
      midi.clear();
      plugin->processBlock(view, midi);

      // Plugin output -> lane. A stereo output folds to (L + R) / 2 for a
      // one-channel lane; a mono output goes to every lane channel.
      float* outLeft = buffer.getWritePointer(0) + offset;
      float* outRight = laneChannels > 1 ? buffer.getWritePointer(1) + offset : nullptr;
      if (outChannels >= 2) {
        if (outRight != nullptr) {
          std::memcpy(outLeft, work[0], bytes);
          std::memcpy(outRight, work[1], bytes);
        } else {
          for (int i = 0; i < count; ++i)
            outLeft[i] = 0.5f * (work[0][i] + work[1][i]);
        }
      } else {
        std::memcpy(outLeft, work[0], bytes);
        if (outRight != nullptr)
          std::memcpy(outRight, work[0], bytes);
      }
    }
  } catch (...) {
    // Out for good (until something prepares it again): a plugin that threw
    // once is not asked a second time on the audio thread. The caller
    // restores the dry signal and flags the block; nothing is logged here.
    failed.store(true, std::memory_order_relaxed);
    return BlockProcessResult::Failed;
  }
  return BlockProcessResult::Processed;
}

// ####################
// BLOCK PROCESSOR
// ####################
HostedPluginBlockProcessor::HostedPluginBlockProcessor(
    std::shared_ptr<HostedPluginInstance> instanceIn)
    : hosted(std::move(instanceIn)) {}

std::unique_ptr<HostedPluginBlockProcessor>
HostedPluginBlockProcessor::create(std::unique_ptr<juce::AudioPluginInstance> instance,
                                   const juce::PluginDescription& description,
                                   const juce::MemoryBlock* chunk, juce::String& error) {
  if (instance == nullptr) {
    if (error.isEmpty())
      error = "the plugin could not be instantiated";
    return nullptr;
  }

  std::shared_ptr<HostedPluginInstance> hosted(
      new HostedPluginInstance(std::move(instance),
                               HostedPluginIdentity::fromDescription(description)),
      destroyOnMessageThread);
  if (!hosted->negotiateLayout(error))
    return nullptr;
  hosted->buildParameterTable();
  // The saved state goes in before anything runs. A plugin added fresh has
  // none, so its own starting state is what the block saves until it changes.
  if (chunk != nullptr && !chunk->isEmpty())
    hosted->applyState(*chunk);
  else
    hosted->captureState();
  return std::unique_ptr<HostedPluginBlockProcessor>(
      new HostedPluginBlockProcessor(std::move(hosted)));
}

HostedPluginBlockProcessor* HostedPluginBlockProcessor::from(BlockProcessor* processor) noexcept {
  // kindId() is the class's own literal, so comparing the text is exact; no
  // RTTI on a path the chain walks under its lock.
  if (processor == nullptr || processor->kind() != ChainBlockType::KIND ||
      std::strcmp(processor->kindId(), kKindId) != 0)
    return nullptr;
  return static_cast<HostedPluginBlockProcessor*>(processor);
}

juce::ValueTree HostedPluginBlockProcessor::makeKindState(const HostedPluginIdentity& identity) {
  juce::ValueTree state("KindState");
  state.setProperty("kind", juce::String(kKindId), nullptr);
  state.setProperty("version", 1, nullptr);
  state.setProperty("name", identity.name, nullptr);
  state.appendChild(identity.toValueTree(), nullptr);
  return state;
}

HostedPluginIdentity HostedPluginBlockProcessor::identityOf(const juce::ValueTree& kindState) {
  HostedPluginIdentity identity =
      HostedPluginIdentity::fromValueTree(kindState.getChildWithName("Plugin"));
  if (identity.name.isEmpty())
    identity.name = kindState.getProperty("name").toString();
  return identity;
}

void HostedPluginBlockProcessor::prepare(const BlockPrepareSpec& spec) {
  hosted->prepare(spec.runSampleRate(true), spec.runBlockSize(true));
}

void HostedPluginBlockProcessor::prepareForInstall(const BlockPrepareSpec& spec) {
  // Prepared on the message thread just before; only a domain that moved
  // since is worth another prepare (which, off the message thread, is
  // deferred: see HostedPluginInstance::prepare).
  if (hosted->isPreparedFor(spec.runSampleRate(true), juce::jmax(1, spec.runBlockSize(true))))
    return;
  prepare(spec);
}

BlockProcessResult HostedPluginBlockProcessor::process(juce::AudioBuffer<float>& buffer,
                                                       const BlockProcessContext& ctx) {
  return hosted->process(buffer, ctx.transport);
}

void HostedPluginBlockProcessor::appendKindState(juce::ValueTree& kindState) const {
  kindState.appendChild(hosted->identity().toValueTree(), nullptr);
}

void HostedPluginBlockProcessor::setKindState(const juce::ValueTree& kindState) {
  if (const auto* chunk = kindState.getProperty("chunk").getBinaryData())
    hosted->applyState(*chunk);
}

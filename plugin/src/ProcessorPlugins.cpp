#include "Processor.h"

#include <algorithm>
#include <optional>

// #############################
// HOSTED PLUGINS (kind "plugin")
// #############################
//
// The chain side of HostedPluginBlockProcessor: how a hosted block comes to
// be, comes back from a saved node, and is looked after while it runs.
//
// The rule everything here is built around is JUCE's: a VST3 or Audio Unit
// instance is created, prepared, asked for its state and destroyed on the
// message thread. So unlike every other kind, whose processor is built on a
// loader thread, a hosted block's build is a chain of message-thread turns:
//
//   queueHostedPluginBuild   any thread, under chainMutex: the block shows as
//                            loading and the build is queued
//   createHostedPlugin       message thread: creates the instance (an AU v3
//                            is asked for and arrives a turn later)
//   finishHostedPlugin       message thread: layout, saved chunk, prepare, all
//                            off the chain lock; then the same fade + install
//                            under the lock that every other kind uses
//
// Nothing waits across threads, so there is nothing to deadlock on when the
// message thread is itself waiting for the rig to load: that waiter runs the
// queue itself (serviceHostedPlugins), which is all a VST3 or an Audio Unit
// v2 needs. Only an Audio Unit v3, whose instance arrives in a later message,
// cannot land while the message thread is blocked.

namespace {

constexpr int kHostedServiceIntervalMs = 100;
// A plugin that keeps flagging changes has its state read at most this often
// by the looking-only callers (see serviceHostedPlugins).
constexpr int kHostedStateCaptureIntervalMs = 500;

bool isHostedPluginNode(const juce::ValueTree& blockNode) {
  return blockNode.getChildWithName("KindState").getProperty("kind").toString() ==
         HostedPluginBlockProcessor::kKindId;
}

}  // namespace

void TONE3000Processor::HostedPluginTimer::timerCallback() {
  processor.serviceHostedPlugins(false, /*throttled=*/true);

  // Nothing left to watch: no hosted block, and no block waiting for a
  // plugin a later scan might find.
  bool needed = false;
  {
    juce::ScopedLock lock(processor.chainMutex);
    for (const auto& l : processor.lanes)
      for (const auto& block : l)
        if (HostedPluginBlockProcessor::from(block->processor.get()) != nullptr ||
            (block->type == ChainBlockType::KIND &&
             block->kindId == HostedPluginBlockProcessor::kKindId))
          needed = true;
  }
  if (!needed)
    stopTimer();
}

void TONE3000Processor::applyHostedPluginState(ChainBlock& block,
                                               const juce::ValueTree& blockState,
                                               const juce::ValueTree& kindState, int serial) {
  const HostedPluginIdentity identity = HostedPluginBlockProcessor::identityOf(kindState);
  block.kindId = HostedPluginBlockProcessor::kKindId;
  block.kindName = identity.name.isNotEmpty() ? identity.name : juce::String("Plugin");

  // The watcher has to run for a placeholder too (a later scan may find the
  // plugin), and this can be any thread.
  juce::MessageManager::callAsync([link = hostedPluginLink] {
    if (TONE3000Processor* self = link->processor.load())
      if (!self->hostedPluginTimer.isTimerRunning())
        self->hostedPluginTimer.startTimer(kHostedServiceIntervalMs);
  });

  // Already hosting this plugin (an undo over a knob move, a preset that
  // keeps the block): the instance stays and only its state moves. That is
  // a call into the plugin, so it needs the message thread; and it is only
  // made when the node's chunk is not what the plugin already has, which is
  // most restores (an undo of an edit elsewhere in the chain).
  // A block whose plugin failed while processing is not reused: a fresh
  // instance is the only honest way to bring it back.
  if (HostedPluginBlockProcessor* live = HostedPluginBlockProcessor::from(block.processor.get());
      live != nullptr && live->identity().samePluginAs(identity) && !block.processingFailed) {
    const juce::MemoryBlock* chunk = kindState.getProperty("chunk").getBinaryData();
    const bool sameState = chunk == nullptr || (live->instance()->cacheIsCurrent() &&
                                                live->instance()->cachedState() == *chunk);
    if (sameState || juce::MessageManager::existsAndIsCurrentThread()) {
      if (!sameState) {
        live->setKindState(kindState);
        // Every caller holds the chain lock and the chain-edit fade, and
        // refreshes the compensation before audio resumes.
        live->commitLatency();
      }
      block.modelLoading = false;
      block.loaded = true;
      return;
    }
  }

  // From here the block gets a new instance or none. The one it holds, if
  // any, goes now: its state is no longer the block's, and its latency must
  // not linger in the lane. Dropping it under the lock is fine for this
  // kind alone: the instance is only handed to the message thread here, and
  // torn down there (see HostedPluginBlockProcessor::create).
  if (HostedPluginBlockProcessor::from(block.processor.get()) != nullptr)
    block.processor.reset();
  block.processingFailed = false;

  const std::optional<juce::PluginDescription> description =
      PluginScanner::instance().findEffect(identity);
  if (!description.has_value()) {
    // Not installed here (or not scanned yet, or blacklisted). The block
    // stays where it is as a bypassed placeholder under the plugin's name,
    // and keeps its whole node so the next save writes it back untouched;
    // a later scan that finds the plugin revives it.
    juce::Logger::writeToLog("[Restore] Block " + juce::String(block.id) + " hosts the plugin '" +
                             identity.name + "' (" + identity.format +
                             "), which is not available; kept as a bypassed placeholder");
    block.missingKindNode = blockState.createCopy();
    block.loaded = false;
    block.modelLoading = false;
    return;
  }

  queueHostedPluginBuild(block, *description, kindState, blockState, serial);
}

void TONE3000Processor::queueHostedPluginBuild(ChainBlock& block,
                                               const juce::PluginDescription& description,
                                               const juce::ValueTree& kindState,
                                               const juce::ValueTree& savedNode, int serial) {
  // Loading, like a tone block waiting for its model; the node it is being
  // built from is kept so a save in the meantime still writes the block
  // whole, and a restore's mute holds until the plugin has landed.
  block.pendingKindState = kindState.createCopy();
  block.loaded = false;
  block.modelLoading = true;

  // Queued, and the message thread nudged. The closure owns its trees: a
  // ValueTree shared with the chain is not safe to read from another thread.
  {
    const std::lock_guard<std::mutex> guard(hostedPluginLink->mutex);
    hostedPluginLink->queue.push_back(
        [blockId = block.id, description, state = kindState.createCopy(),
         node = savedNode.isValid() ? savedNode.createCopy() : juce::ValueTree(),
         serial](TONE3000Processor& self) {
          self.createHostedPlugin(blockId, description, state, node, serial);
        });
  }
  // (A post can only fail while the process is shutting down; the build
  // then stays queued for a service call that will not come, which is as
  // good as any other answer at that point.)
  juce::MessageManager::callAsync([link = hostedPluginLink] {
    if (TONE3000Processor* self = link->processor.load())
      self->runQueuedHostedPluginWork();
  });
}

void TONE3000Processor::runQueuedHostedPluginWork() {
  if (!juce::MessageManager::existsAndIsCurrentThread())
    return;
  // Taken out under the lock, run after it: a build installs under the
  // chain lock, and whatever queues the next one holds that lock too.
  std::vector<std::function<void(TONE3000Processor&)>> work;
  {
    const std::lock_guard<std::mutex> guard(hostedPluginLink->mutex);
    work.swap(hostedPluginLink->queue);
  }
  for (const auto& task : work)
    task(*this);
}

void TONE3000Processor::createHostedPlugin(const std::string& blockId,
                                           const juce::PluginDescription& description,
                                           const juce::ValueTree& kindState,
                                           const juce::ValueTree& savedNode, int serial) {
  if (!hostedPluginTimer.isTimerRunning())
    hostedPluginTimer.startTimer(kHostedServiceIntervalMs);

  {
    // Gone, or waiting for a newer build: do not load a plugin for nothing.
    juce::ScopedLock lock(chainMutex);
    const ChainBlock* block = findBlockById(blockId);
    if (block == nullptr || block->type != ChainBlockType::KIND ||
        block->kindBuildSerial != serial)
      return;
  }

  // Created right here when the format allows it (VST3, Audio Unit v2), so
  // a caller that is waiting on this thread gets its plugin without a
  // message having to be delivered.
  const BlockPrepareSpec spec = blockPrepareSpecForId(blockId);
  juce::String error;
  std::unique_ptr<juce::AudioPluginInstance> instance = hostedPluginFormats().createPluginInstance(
      description, spec.runSampleRate(true), spec.runBlockSize(true), error);
  if (instance != nullptr || description.pluginFormatName != "AudioUnit") {
    finishHostedPlugin(std::move(instance), error, blockId, description, kindState, savedNode,
                       serial);
    return;
  }

  // An Audio Unit that did not come up synchronously may be a v3 unit, which
  // JUCE refuses to make on a blocked message thread. Those are asked for
  // asynchronously and arrive in a later message (a unit that is simply
  // broken fails again there, with its real error).
  hostedPluginFormats().createPluginInstanceAsync(
      description, spec.runSampleRate(true), spec.runBlockSize(true),
      [link = hostedPluginLink, blockId, description, kindState, savedNode,
       serial](std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& error) {
        // With the processor gone the instance dies right here, which is
        // the message thread.
        if (TONE3000Processor* self = link->processor.load())
          self->finishHostedPlugin(std::move(instance), error, blockId, description, kindState,
                                   savedNode, serial);
      });
}

void TONE3000Processor::finishHostedPlugin(std::unique_ptr<juce::AudioPluginInstance> instance,
                                           const juce::String& creationError,
                                           const std::string& blockId,
                                           const juce::PluginDescription& description,
                                           const juce::ValueTree& kindState,
                                           const juce::ValueTree& savedNode, int serial) {
  // Layout, saved state, prepare: all calls into the plugin, all out here on
  // the message thread and off the chain lock. The chunk goes in before the
  // prepare, so the plugin never runs a block on anything but its saved
  // state and reports the latency that state implies.
  PreparedBlockModel prepared;
  juce::String error = creationError;
  if (instance != nullptr) {
    prepared.processor = HostedPluginBlockProcessor::create(
        std::move(instance), description, kindState.getProperty("chunk").getBinaryData(), error);
    if (prepared.processor != nullptr) {
      prepared.processor->prepareForChain(blockPrepareSpecForId(blockId));
      prepared.success = true;
    }
  }

  // A plugin with latency moves its lane in time as it lands, so the chain
  // is muted around the install. Unless it already is: a restore holds its
  // mute until this very build settles, and owns the release.
  std::unique_ptr<ChainEditFade> editFade;
  if (prepared.success && prepared.processor->chainLatencySamples() > 0 &&
      !chainEditFadePending.load())
    editFade = std::make_unique<ChainEditFade>(*this);

  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  // Gone, turned into something else, or waiting for a newer build. The
  // instance just made is let go (and destroyed a message-loop turn later).
  if (block == nullptr || block->type != ChainBlockType::KIND ||
      block->kindBuildSerial != serial)
    return;

  if (!prepared.success) {
    // The scanner knew the plugin and it still would not come up (the file
    // changed, a licence check failed, no usable layout). Same answer as a
    // plugin that is not installed: a bypassed placeholder that keeps the
    // state, never a failed block in the way of the rest of the rig.
    juce::Logger::writeToLog("[Plugin] Could not load '" + description.name + "' for block " +
                             juce::String(blockId) + ": " + error +
                             "; kept as a bypassed placeholder");
    block->missingKindNode =
        savedNode.isValid() ? savedNode.createCopy() : serializeBlockSettings(*block);
    block->pendingKindState = {};
    block->loaded = false;
    block->modelLoading = false;
    block->loadFailed = false;
    block->swapFadePending.store(false);
    refreshLatencyCompensation();
    bumpChainRevision();
    return;
  }

  applyPreparedModelToChainBlock(*block, ChainBlockType::KIND, prepared);
}

juce::var TONE3000Processor::addPluginBlock(const juce::String& pluginKey,
                                            const std::string& targetId) {
  const auto failure = [](const juce::String& message) {
    juce::DynamicObject::Ptr result = new juce::DynamicObject();
    result->setProperty("error", message);
    return juce::var(result.get());
  };

  juce::String lookupError;
  const std::optional<juce::PluginDescription> description =
      PluginScanner::instance().findEffect(pluginKey, &lookupError);
  if (!description.has_value())
    return failure(lookupError);

  const juce::ValueTree kindState = HostedPluginBlockProcessor::makeKindState(
      HostedPluginIdentity::fromDescription(*description));

  juce::ScopedLock lock(chainMutex);

  if (!targetId.empty()) {
    const ChainBlock* target = findBlockById(targetId);
    if (target == nullptr)
      return failure("no block with id '" + juce::String(targetId) + "'");
    if (target->type != ChainBlockType::INSERT)
      return failure("block '" + juce::String(targetId) +
                     "' is not an empty slot; a plugin is added to an empty slot");
  }

  // The same slot resolution as addKindBlock: the named insert, else the
  // active lane's first one, else the lane's end (a grid row never grows:
  // without an empty slot there is nowhere to put it).
  Lane* targetLane = nullptr;
  size_t slotIndex = 0;
  if (!findLandingSlot(targetId, targetLane, slotIndex))
    return failure("the grid has no empty slot");

  pushChainHistory();
  auto fresh = std::make_unique<ChainBlock>(juce::Uuid().toString().toStdString(),
                                            ChainBlockType::KIND);
  prepareBlockForChainRate(*fresh);
  ChainBlock* block = fresh.get();

  if (slotIndex < targetLane->size())
    (*targetLane)[slotIndex] = std::move(fresh);
  else
    targetLane->push_back(std::move(fresh));

  block->kindId = HostedPluginBlockProcessor::kKindId;
  block->kindName = description->name;
  const int serial = ++block->kindBuildSerial;
  queueHostedPluginBuild(*block, *description, kindState, {}, serial);

  rebuildTopology();
  bumpChainRevision();

  juce::DynamicObject::Ptr result = new juce::DynamicObject();
  result->setProperty("blockId", juce::String(block->id));
  return juce::var(result.get());
}

void TONE3000Processor::reviveMissingPluginBlocks() {
  // Which placeholders name a plugin the scanner can now offer. The
  // scanner is asked off the chain lock: its first answer reads the cache
  // file, which is no business of the audio thread's.
  std::vector<std::pair<std::string, HostedPluginIdentity>> waiting;
  {
    juce::ScopedLock lock(chainMutex);
    for (const auto& l : lanes)
      for (const auto& block : l)
        // (A placeholder for a missing asset is not waiting for a plugin: a
        // scan cannot bring its state back, see applyKindState.)
        if (block->isMissingKind() && isHostedPluginNode(block->missingKindNode) &&
            !t3k::presetjson::hasMissingAsset(block->missingKindNode))
          waiting.emplace_back(block->id,
                               HostedPluginBlockProcessor::identityOf(
                                   block->missingKindNode.getChildWithName("KindState")));
  }
  std::vector<std::string> revivable;
  for (const auto& [id, identity] : waiting)
    if (PluginScanner::instance().findEffect(identity).has_value())
      revivable.push_back(id);
  if (revivable.empty())
    return;

  // Like a restore of those blocks: muted until their plugins have landed.
  // Not an undo step: nothing the user did, and the rig's saved content is
  // the same before and after.
  ChainEditFade editFade(*this);
  {
    juce::ScopedLock lock(chainMutex);
    for (const auto& id : revivable)
      if (ChainBlock* block = findBlockById(id); block != nullptr && block->isMissingKind()) {
        juce::Logger::writeToLog("[Plugin] Plugin for block " + juce::String(id) +
                                 " is available again; loading it");
        const juce::ValueTree node = block->missingKindNode;
        applyBlockSettings(*block, node);
      }
    bumpChainRevision();
  }
  editFade.releaseWhenChainLoadsSettle();
}

void TONE3000Processor::serviceHostedPlugins(bool forceStateRefresh, bool throttled) {
  if (!juce::MessageManager::existsAndIsCurrentThread() || servicingHostedPlugins)
    return;
  const juce::ScopedValueSetter<bool> guard(servicingHostedPlugins, true);

  // Builds that are waiting for this thread (see HostedPluginLink).
  runQueuedHostedPluginWork();

  // A scan ended since the last look: plugins may have appeared.
  if (const juce::uint32 generation = PluginScanner::instance().generation();
      generation != scannerGenerationSeen) {
    scannerGenerationSeen = generation;
    reviveMissingPluginBlocks();
  }

  // One process-wide counter says whether any instance has work; without a
  // change (and without a save asking for fresh chunks) this is all it costs.
  const juce::uint32 attention = HostedPluginInstance::attentionCount();
  if (attention == hostedAttentionSeen && !forceStateRefresh)
    return;
  hostedAttentionSeen = attention;

  // The instances are collected under the lock and serviced after it: the
  // shared_ptr keeps each alive, and the calls into the plugin (its state
  // above all) must not hold the audio thread up.
  std::vector<std::shared_ptr<HostedPluginInstance>> instances;
  {
    juce::ScopedLock lock(chainMutex);
    for (const auto& l : lanes)
      for (const auto& block : l)
        if (const auto* hosted = HostedPluginBlockProcessor::from(block->processor.get()))
          instances.push_back(hosted->instance());
  }

  bool refreshWanted = false;
  bool prepareWanted = false;
  bool captureDeferred = false;
  for (const auto& instance : instances) {
    const auto result = instance->serviceOnMessageThread(
        forceStateRefresh, throttled ? kHostedStateCaptureIntervalMs : 0);
    captureDeferred = captureDeferred || result.captureDeferred;
    refreshWanted = refreshWanted || result.latencyPending || result.tailChanged;
    prepareWanted = prepareWanted || result.needsPrepare;
  }

  // A change left for later: make the next call look again (the timer, 100
  // ms on, is that call).
  if (captureDeferred)
    hostedAttentionSeen = attention - 1;

  if (prepareWanted) {
    // A prepare reached these instances on a thread that may not touch a
    // plugin (a host calling prepareToPlay off the message thread). They
    // have been passing audio through since; prepare them here, muted,
    // since a latent plugin coming back moves its lane in time.
    ChainEditFade editFade(*this);
    juce::ScopedLock lock(chainMutex);
    for (int row = 0; row < kNumLanes; ++row) {
      const BlockPrepareSpec spec = blockPrepareSpecFor(row);
      for (const auto& block : lanes[static_cast<size_t>(row)])
        if (const auto* hosted = HostedPluginBlockProcessor::from(block->processor.get());
            hosted != nullptr && hosted->instance()->prepareIsWanted())
          block->processor->prepareForChain(spec);
    }
    refreshBlockTailLength();
    refreshLatencyCompensation();
  }

  // A latency or tail the plugin changed by itself goes the way a parameter
  // edit's does: the deferred refresh, which mutes, commits and re-reports.
  if (refreshWanted) {
    blockRefreshPending.store(true);
    startTimer(static_cast<int>(kGestureSettleMs));
  }
}

std::shared_ptr<HostedPluginInstance> TONE3000Processor::hostedPluginInstance(const std::string& blockId) {
  juce::ScopedLock lock(chainMutex);
  if (const ChainBlock* block = findBlockById(blockId))
    if (const auto* hosted = HostedPluginBlockProcessor::from(block->processor.get()))
      return hosted->instance();
  return nullptr;
}

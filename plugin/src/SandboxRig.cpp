#include "SandboxRig.h"

// Processor.h first: it pulls in the JUCE module headers, which must come
// before anything that drags in <windows.h>.
#include "Processor.h"

#include "EngineCommands.h"
#include "ImageDeck.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

// Same value the render tool uses, so a sandbox render of a rig with Spread
// or Align wobble matches a PedaleiraRender of the same rig.
constexpr std::int64_t kWobbleSeed = 0x47524944;

// The wobble noise seed is process wide and read in prepare (see DeckWobble):
// held fixed while a sandbox prepares, so its renders repeat, and put back
// before the rig being played prepares anything again.
struct ScopedWobbleSeed {
  ScopedWobbleSeed()
      : wasSeeded(DeckWobble::seeded.load()), previousSeed(DeckWobble::seed.load()) {
    DeckWobble::setSeedOverride(kWobbleSeed);
  }
  ~ScopedWobbleSeed() {
    if (wasSeeded)
      DeckWobble::setSeedOverride(previousSeed);
    else
      DeckWobble::clearSeedOverride();
  }
  bool wasSeeded;
  std::int64_t previousSeed;
};

}  // namespace

SandboxRig::SandboxRig(const juce::String& rigId, TONE3000Processor& live)
    : id(rigId), created(juce::Time::getCurrentTime()) {
  JUCE_ASSERT_MESSAGE_THREAD
  rig = std::make_unique<TONE3000Processor>();
  // One thread: a render must not depend on, or compete with, the realtime
  // workers of the rig being played.
  rig->setMultiCoreEnabled(false, /*persist=*/false);
  rig->sharePresetStoreWith(live);
  const double rate = live.getSampleRate() > 0.0 ? live.getSampleRate() : 48000.0;
  rig->setPlayConfigDetails(2, 2, rate, OfflineRender::kBlockSize);
  rig->prepareToPlay(rate, OfflineRender::kBlockSize);
  rigCommands = std::make_unique<EngineCommands>(*rig, EngineCommands::Scope::rig);
}

SandboxRig::~SandboxRig() {
  JUCE_ASSERT_MESSAGE_THREAD
  // A processor unsets the process's logger as it goes (it assumes it is the
  // only one). The rig being played keeps logging.
  juce::Logger* const logger = juce::Logger::getCurrentLogger();
  rigCommands.reset();
  rig.reset();
  juce::Logger::setCurrentLogger(logger);
}

void SandboxRig::copyStateFrom(TONE3000Processor& source) {
  juce::MemoryBlock state;
  source.getStateInformation(state);
  rig->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
}

bool SandboxRig::applyTo(TONE3000Processor& target) { return applySnapshot(capture(), target); }

SandboxRig::Snapshot SandboxRig::capture() {
  const juce::var preset = rig->getChainState(-1)["preset"];
  Snapshot snapshot;
  // A tree of its own (captureRigTree builds one each time): nothing the
  // sandbox does afterwards reaches it.
  snapshot.tree = rig->captureRigTree();
  snapshot.presetId = preset["id"].toString();
  snapshot.presetName = preset["name"].toString();
  snapshot.edited = rig->hasUnsavedChanges();
  snapshot.revision = rig->getCurrentChainRevision();
  return snapshot;
}

bool SandboxRig::applySnapshot(const Snapshot& snapshot, TONE3000Processor& target) {
  // Whether what arrives is the preset as its file has it: not when the
  // sandbox was edited since it loaded or saved it, and not when the target
  // carried unsaved changes of the same preset (a copy of the live rig starts
  // from those, and counts them as its own baseline).
  const bool unsaved =
      snapshot.edited ||
      (snapshot.presetId.isNotEmpty() &&
       target.getChainState(-1)["preset"]["id"].toString() == snapshot.presetId &&
       target.hasUnsavedChanges());
  return target.applyRigTree(snapshot.tree, snapshot.presetId, snapshot.presetName, unsaved);
}

namespace OfflineRender {

Result render(TONE3000Processor& rig, const juce::AudioBuffer<float>& input, double sampleRate,
              double tailSeconds, const std::atomic<bool>& cancel) {
  Result result;
  const int inChannels = juce::jlimit(1, 2, input.getNumChannels());
  const int outChannels = 2;
  const int inFrames = input.getNumSamples();
  if (inFrames <= 0 || sampleRate < 8000.0 || sampleRate > 192000.0) {
    result.error = "nothing to render";
    return result;
  }

  const auto onMessageThread = [&](std::function<void()> call) {
    return CommandRegistry::callOnMessageThread(std::move(call), kLoadTimeoutMs, &cancel);
  };
  const auto waitLoaded = [&]() -> juce::String {
    const CommandResult loaded = EngineCommands::waitUntilLoaded(rig, kLoadTimeoutMs, &cancel);
    if (!loaded.ok)
      return loaded.errorMessage;
    if (!static_cast<bool>(loaded.value["loaded"]))
      return "a block failed to load: " +
             juce::JSON::toString(loaded.value["failedBlockIds"], true);
    return {};
  };

  // The same sequence as PedaleiraRender: prepare, let the loads land, prepare
  // again with the rig in place (like a host starting transport after a
  // session load, so fades and the image matrix start from the loaded rig),
  // then render. Preparing also clears every block's history, which is what
  // makes the same call give the same samples.
  bool layoutTaken = true;
  const auto prepare = [&] {
    const ScopedWobbleSeed seed;
    rig.setPlayConfigDetails(inChannels, outChannels, sampleRate, kBlockSize);
    layoutTaken = rig.getTotalNumInputChannels() == inChannels &&
                  rig.getTotalNumOutputChannels() == outChannels;
    if (layoutTaken)
      rig.prepareToPlay(sampleRate, kBlockSize);
  };
  for (int pass = 0; pass < 2; ++pass) {
    if (!onMessageThread(prepare)) {
      result.error = "the render was cancelled or the application was busy";
      return result;
    }
    if (!layoutTaken) {
      result.error = "the rig does not take " + juce::String(inChannels) + " input channel(s)";
      return result;
    }
    if (const juce::String problem = waitLoaded(); problem.isNotEmpty()) {
      result.error = problem;
      return result;
    }
  }

  int latency = 0;
  if (!onMessageThread([&] { latency = rig.getLatencySamples(); })) {
    result.error = "the render was cancelled or the application was busy";
    return result;
  }
  result.latencySamples = latency;

  // `latency` extra frames are rendered and that many leading ones dropped,
  // so the result lines up sample for sample with the input.
  const int tailFrames =
      static_cast<int>(std::llround(juce::jlimit(0.0, 60.0, tailSeconds) * sampleRate));
  const int renderedFrames = inFrames + tailFrames;
  const std::int64_t total = static_cast<std::int64_t>(renderedFrames) + latency;
  const int numBlocks = static_cast<int>((total + kBlockSize - 1) / kBlockSize);

  result.audio.setSize(outChannels, renderedFrames);
  result.audio.clear();
  juce::AudioBuffer<float> block(std::max(inChannels, outChannels), kBlockSize);
  juce::MidiBuffer midi;
  for (int b = 0; b < numBlocks; ++b) {
    if (cancel.load()) {
      result.error = "the render was cancelled";
      result.audio.setSize(0, 0);
      return result;
    }
    const int start = b * kBlockSize;
    block.clear();
    const int fromInput = juce::jlimit(0, kBlockSize, inFrames - start);
    for (int ch = 0; ch < inChannels && fromInput > 0; ++ch)
      block.copyFrom(ch, 0, input, ch, start, fromInput);

    midi.clear();
    rig.processBlock(block, midi);

    // Output frame n is processed frame n + latency.
    const int dest = std::max(0, start - latency);
    const int skip = dest + latency - start;
    const int count = std::min(kBlockSize - skip, renderedFrames - dest);
    for (int ch = 0; ch < outChannels && count > 0; ++ch)
      result.audio.copyFrom(ch, dest, block, ch, skip, count);
  }
  return result;
}

}  // namespace OfflineRender

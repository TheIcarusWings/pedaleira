// cab.model: a guitar cab and a mic, modelled from physics (docs/cab-framework.md).
//
//   in -> [convolver with the cab's impulse response] -> level
//
// The block has no impulse response file: it computes one (cab/CabResponse.h)
// from its speaker (a description, resources/cabs/*.json), its box, its mic
// and the mic's place, and plays it through the convolution engine the IR
// blocks use (juce::dsp::Convolution, uniform partitions, zero latency; see
// IrBlockProcessor.h).
//
// A change of any control but the level asks for a new response. It is built
// off the audio thread, by the shared builder thread (dsp/CabIrWorker.h), into
// the block's second convolver, which is idle; the audio thread then runs both
// convolvers for a moment, first with the new one silent while it fills its
// history (45 ms: all of the response but its faint end, so it then plays what
// it would have, had it always been there), then crossfading to it. The old convolver goes back to the builder. Nothing
// on the audio thread allocates, locks or waits: the hand-off is two atomics.
//
//   handoff 0  the idle convolver belongs to the builder
//   handoff 1  the builder has loaded it: it belongs to the audio thread, which
//              primes it, fades to it, makes it the live one and sets 0 again
//
// prepareEffect builds the response for the settings of the moment on the
// calling thread, so a block always starts on the sound its parameters ask
// for, and renders made after a prepare are repeatable.
//
// A speaker of one's own: block.loadDescription gives the block a speaker
// description file, which becomes the last entry of the speaker choice,
// "Custom". Like amp.custom, the text travels in the block's saved state.
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "IrBlockProcessor.h"
#include "cab/CabResponse.h"
#include "dsp/CabIrWorker.h"
#include "effects/EffectDsp.h"
#include "effects/cab/BuiltinCabs.h"

namespace {

// The new response is silent for this long after it arrives while its
// convolver fills its history (most of the response's length: what is left
// is 40 dB and more down), then fades in over this long, equal gain (the two
// responses are close relatives, their outputs correlated).
constexpr double kPrimeSeconds = 0.045;
constexpr double kFadeSeconds = 0.025;

// The convolvers' own loader queue: they are loaded by the builder thread one
// at a time, so a few entries are plenty.
constexpr int kConvolverQueueEntries = 16;

/** Everything the impulse response depends on: the parameters but the level,
    and which custom speaker is loaded. Compared as a whole. */
struct CabSettingsKey {
  float values[10]{};
  int speakerGeneration{0};

  bool operator==(const CabSettingsKey& other) const noexcept {
    // The values as stored: any change at all asks for a new response.
    return std::memcmp(values, other.values, sizeof values) == 0 && speakerGeneration == other.speakerGeneration;
  }
};

class CabModelProcessor final : public ParameterisedBlockProcessor, private cabfx::CabIrClient {
public:
  static constexpr const char* kKindId = "cab.model";

  enum Param {
    kSpeaker,
    kCabSize,
    kBack,
    kMic,
    kMicPosition,
    kMicDistanceCm,
    kMicAngleDeg,
    kRoom,
    kLowCutHz,
    kHighCutHz,
    kLevelDb,
  };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      juce::StringArray speakers;
      for (const char* file : cabfx::kSpeakerFiles) {
        const auto speaker = cabfx::builtinSpeaker(file);
        speakers.add(speaker != nullptr ? juce::String(speaker->name) : juce::String(file));
      }
      // A speaker loaded from a description file (block.loadDescription).
      speakers.add("Custom");
      BlockParamTable t;
      t.push_back(BlockParamSpec::choiceParam("speaker", "Speaker", speakers, 0));
      t.push_back(BlockParamSpec::choiceParam("cabSize", "Cab", {"1x12", "2x12", "4x12", "1x10", "2x10"}, 2));
      t.push_back(BlockParamSpec::choiceParam("back", "Back", {"Closed", "Open"}, 0));
      t.push_back(BlockParamSpec::choiceParam("mic", "Mic", {"Dynamic", "Ribbon", "Condenser"}, 0));
      t.push_back(BlockParamSpec::floatParam("micPosition", "Position", "%", 0.0f, 100.0f, 35.0f));
      t.push_back(BlockParamSpec::floatParam("micDistanceCm", "Distance", "cm", 0.5f, 100.0f, 2.5f).withSkew(0.3f));
      t.push_back(BlockParamSpec::floatParam("micAngleDeg", "Angle", "deg", 0.0f, 60.0f, 0.0f));
      t.push_back(BlockParamSpec::floatParam("room", "Room", "%", 0.0f, 100.0f, 10.0f));
      t.push_back(BlockParamSpec::floatParam("lowCutHz", "Low cut", "Hz", 20.0f, 400.0f, 30.0f).withSkew(0.4f));
      t.push_back(
          BlockParamSpec::floatParam("highCutHz", "High cut", "Hz", 2000.0f, 20000.0f, 16000.0f).withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("levelDb", "Level", "dB", -24.0f, 12.0f, 0.0f));
      return t;
    }();
    return table;
  }

  CabModelProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()), worker(cabfx::CabIrWorker::acquire()) {
    worker->addClient(this);
  }

  ~CabModelProcessor() override {
    // Waits for a build of this block to finish; nothing reaches the block
    // from the builder afterwards.
    worker->removeClient(this);
  }

  CabModelProcessor(const CabModelProcessor&) = delete;
  CabModelProcessor& operator=(const CabModelProcessor&) = delete;

  // The convolvers are zero latency; the tail is the response.
  int latencySamples() const noexcept override { return 0; }
  double tailSeconds() const noexcept override {
    return static_cast<double>(cab::cabImpulseLength(kChainBaseSampleRate)) / kChainBaseSampleRate;
  }
  float costSeed() const noexcept override { return 0.006f; }

  // ---- A speaker of one's own ------------------------------------------------

  bool takesDescription() const noexcept override { return true; }

  bool checkDescription(const juce::String& text, juce::String& error) const override {
    cab::SpeakerDescription scratch;
    std::string message;
    if (cab::parseSpeakerDescription(text.toStdString(), scratch, message))
      return true;
    error = juce::String(message);
    return false;
  }

  bool loadDescription(const juce::String& text, juce::String& error) override {
    auto parsed = std::make_shared<cab::SpeakerDescription>();
    std::string message;
    const std::string utf8 = text.toStdString();
    if (!cab::parseSpeakerDescription(utf8, *parsed, message)) {
      error = juce::String(message);
      return false;
    }
    setCustomSpeaker(std::move(parsed), utf8);
    // The builder notices the new speaker on its next visit (the chain lock is
    // held here, so it is not woken: that would wait on its thread).
    setParameter(kSpeaker, static_cast<float>(cabfx::kBuiltinSpeakers));
    return true;
  }

  void getStateChunk(juce::MemoryBlock& chunk) const override {
    const std::lock_guard<std::mutex> lock(speakerMutex);
    if (!customText.empty())
      chunk.append(customText.data(), customText.size());
  }

  void setStateChunk(const juce::MemoryBlock& chunk, int) override {
    auto parsed = std::make_shared<cab::SpeakerDescription>();
    std::string message;
    const std::string utf8(static_cast<const char*>(chunk.getData()), chunk.getSize());
    if (cab::parseSpeakerDescription(utf8, *parsed, message)) {
      setCustomSpeaker(std::move(parsed), utf8);
    } else {
      juce::Logger::writeToLog("[cab.model] saved speaker description does not load: " + juce::String(message));
      setCustomSpeaker(nullptr, {});
    }
  }

  void setKindState(const juce::ValueTree& kindState) override {
    BlockProcessor::setKindState(kindState);
    // No chunk in the node: the block has no speaker of its own.
    if (kindState.getProperty("chunk").getBinaryData() == nullptr)
      setCustomSpeaker(nullptr, {});
  }

  ImpulseResponseJob impulseResponseJob() const override {
    const CabSettingsKey key = currentKey();
    const auto speaker = speakerFor(key);
    return [speaker, key](double sampleRate) { return impulseFor(*speaker, key, sampleRate); };
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    const std::lock_guard<std::mutex> lock(buildMutex);
    builtSpec = spec;
    const int channels = juce::jmax(2, spec.numChannels);
    inputCopy.setSize(channels, juce::jmax(1, spec.maxBlockSize));
    level.prepare(spec.sampleRate, 0.02);
    primeSamples = static_cast<int>(std::lround(kPrimeSeconds * spec.sampleRate));
    fadeSamples = juce::jmax(1, static_cast<int>(std::lround(kFadeSeconds * spec.sampleRate)));

    for (auto& slot : slots)
      if (slot.convolver == nullptr) {
        slot.queue = std::make_unique<juce::dsp::ConvolutionMessageQueue>(kConvolverQueueEntries);
        slot.convolver = std::make_unique<juce::dsp::Convolution>(juce::dsp::Convolution::Latency{0}, *slot.queue);
      }

    // The sound the parameters ask for, now, on this thread.
    const CabSettingsKey key = currentKey();
    load(slots[0], key);
    live.store(0, std::memory_order_relaxed);
    fading = false;
    fadePosition = 0;
    handoff.store(0, std::memory_order_release);
    prepared = true;
    resetEffect();
  }

  void resetEffect() noexcept override {
    // A fade in progress ends here: the new response is the one wanted.
    if (fading)
      finishFade();
    if (prepared)
      slots[live.load(std::memory_order_relaxed)].convolver->reset();
    level.snapOnNextTarget();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext&) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    if (!prepared || numSamples == 0)
      return;

    // A new response is ready: start priming it.
    if (!fading && handoff.load(std::memory_order_acquire) == 1) {
      fading = true;
      fadePosition = 0;
    }

    const int current = live.load(std::memory_order_relaxed);
    if (fading) {
      for (int ch = 0; ch < numChannels; ++ch)
        inputCopy.copyFrom(ch, 0, buffer, ch, 0, numSamples);
    }
    convolve(*slots[current].convolver, buffer, numChannels, numSamples);

    if (fading) {
      juce::AudioBuffer<float> incoming(inputCopy.getArrayOfWritePointers(), numChannels, numSamples);
      convolve(*slots[1 - current].convolver, incoming, numChannels, numSamples);
      for (int i = 0; i < numSamples; ++i) {
        const int position = fadePosition + i - primeSamples;
        const float g = position <= 0 ? 0.0f
                        : position >= fadeSamples
                            ? 1.0f
                            : static_cast<float>(position) / static_cast<float>(fadeSamples);
        for (int ch = 0; ch < numChannels; ++ch) {
          float* out = buffer.getWritePointer(ch);
          out[i] += g * (incoming.getReadPointer(ch)[i] - out[i]);
        }
      }
      fadePosition += numSamples;
      if (fadePosition >= primeSamples + fadeSamples)
        finishFade();
    }

    level.setTarget(fx::dbToGain(param(kLevelDb)));
    for (int i = 0; i < numSamples; ++i) {
      const float g = level.next();
      for (int ch = 0; ch < numChannels; ++ch)
        buffer.getWritePointer(ch)[i] *= g;
    }
  }

private:
  struct Slot {
    std::unique_ptr<juce::dsp::ConvolutionMessageQueue> queue;
    std::unique_ptr<juce::dsp::Convolution> convolver;
    CabSettingsKey key;
  };

  static void convolve(juce::dsp::Convolution& convolver, juce::AudioBuffer<float>& buffer, int numChannels,
                       int numSamples) {
    juce::dsp::AudioBlock<float> block(buffer.getArrayOfWritePointers(), static_cast<size_t>(numChannels),
                                       static_cast<size_t>(numSamples));
    processConvolverInChunks(convolver, block);
  }

  /** Audio thread (or prepare, with the audio thread out): the faded-to
      convolver becomes the live one and the other goes back to the
      builder. */
  void finishFade() noexcept {
    fading = false;
    fadePosition = 0;
    live.store(1 - live.load(std::memory_order_relaxed), std::memory_order_relaxed);
    handoff.store(0, std::memory_order_release);
  }

  CabSettingsKey currentKey() const noexcept {
    CabSettingsKey key;
    for (int i = 0; i < 10; ++i)
      key.values[i] = param(i);
    key.speakerGeneration = speakerGeneration.load(std::memory_order_acquire);
    return key;
  }

  std::shared_ptr<const cab::SpeakerDescription> speakerFor(const CabSettingsKey& key) const {
    const int index = static_cast<int>(std::lround(key.values[kSpeaker]));
    if (index >= cabfx::kBuiltinSpeakers) {
      const std::lock_guard<std::mutex> lock(speakerMutex);
      if (custom != nullptr)
        return custom;
    }
    // Custom with no speaker loaded plays the first shipped one.
    if (auto builtin = cabfx::builtinSpeakerAt(index >= cabfx::kBuiltinSpeakers ? 0 : index))
      return builtin;
    // Only if a shipped file failed to parse (the tests load them all): a
    // plain speaker rather than none.
    static const auto fallback = std::make_shared<const cab::SpeakerDescription>();
    return fallback;
  }

  static cab::CabSettings settingsFor(const CabSettingsKey& key) noexcept {
    cab::CabSettings settings;
    settings.size = static_cast<cab::CabSize>(std::lround(key.values[kCabSize]));
    settings.back = static_cast<cab::CabBack>(std::lround(key.values[kBack]));
    settings.mic = static_cast<cab::MicType>(std::lround(key.values[kMic]));
    settings.position = key.values[kMicPosition] / 100.0;
    settings.distanceCm = key.values[kMicDistanceCm];
    settings.angleDeg = key.values[kMicAngleDeg];
    settings.room = key.values[kRoom] / 100.0;
    settings.lowCutHz = key.values[kLowCutHz];
    settings.highCutHz = key.values[kHighCutHz];
    return settings;
  }

  static std::vector<float> impulseFor(const cab::SpeakerDescription& speaker, const CabSettingsKey& key,
                                       double sampleRate) {
    return cab::cabImpulseResponse(speaker, settingsFor(key), sampleRate);
  }

  /** Not on the audio thread, with buildMutex held and `slot` not in use by
      the audio thread: computes the response for `key` and gives it to the
      slot's convolver, fully installed. */
  void load(Slot& slot, const CabSettingsKey& key) {
    const auto speaker = speakerFor(key);
    std::vector<float> ir = impulseFor(*speaker, key, builtSpec.sampleRate);
    juce::AudioBuffer<float> buffer(1, static_cast<int>(ir.size()));
    buffer.copyFrom(0, 0, ir.data(), static_cast<int>(ir.size()));

    const int blockSize = irConvolverBlockSizeFor(builtSpec.maxBlockSize);
    const juce::dsp::ProcessSpec spec{builtSpec.sampleRate, static_cast<juce::uint32>(blockSize),
                                      static_cast<juce::uint32>(juce::jmax(2, builtSpec.numChannels))};
    // Load before prepare: prepare runs the convolver's queue on this thread,
    // so the engine is built and installed here (as the IR loader does it).
    slot.convolver->loadImpulseResponse(std::move(buffer), builtSpec.sampleRate, juce::dsp::Convolution::Stereo::no,
                                        juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
    slot.convolver->prepare(spec);
    // Should the convolver's own thread have taken the engine first, it
    // installs on the first process call behind a 50 ms fade of its own: run
    // it here, through silence, so it is over before the audio thread sees
    // the convolver (as ProcessorModelLoader does for IR files).
    juce::AudioBuffer<float> silence(static_cast<int>(spec.numChannels), blockSize);
    silence.clear();
    const int warmup = static_cast<int>(0.15 * builtSpec.sampleRate);
    for (int done = 0; done < warmup; done += blockSize) {
      juce::dsp::AudioBlock<float> block(silence);
      slot.convolver->process(juce::dsp::ProcessContextReplacing<float>(block));
    }
    slot.convolver->reset();
    slot.key = key;
  }

  /** The builder thread. */
  void serviceBuild() override {
    std::unique_lock<std::mutex> lock(buildMutex, std::try_to_lock);
    if (!lock.owns_lock() || !prepared)
      return;
    // A hand-off in flight: the idle convolver is the audio thread's.
    if (handoff.load(std::memory_order_acquire) != 0)
      return;
    const int current = live.load(std::memory_order_relaxed);
    const CabSettingsKey key = currentKey();
    if (key == slots[current].key)
      return;
    load(slots[1 - current], key);
    handoff.store(1, std::memory_order_release);
  }

  void setCustomSpeaker(std::shared_ptr<const cab::SpeakerDescription> speaker, std::string text) {
    {
      const std::lock_guard<std::mutex> lock(speakerMutex);
      if (custom == nullptr && speaker == nullptr)
        return;
      custom = std::move(speaker);
      customText = std::move(text);
    }
    speakerGeneration.fetch_add(1, std::memory_order_acq_rel);
  }

  // Declared first, destroyed last: the clients go before the worker.
  std::shared_ptr<cabfx::CabIrWorker> worker;

  // ---- Shared between the builder and prepare (buildMutex) -------------------
  std::mutex buildMutex;
  EffectSpec builtSpec;
  bool prepared{false};
  Slot slots[2];

  // ---- The hand-off ------------------------------------------------------------
  std::atomic<int> live{0};
  std::atomic<int> handoff{0};

  // ---- Audio thread ------------------------------------------------------------
  bool fading{false};
  int fadePosition{0};
  int primeSamples{0};
  int fadeSamples{1};
  juce::AudioBuffer<float> inputCopy;
  fx::SmoothedParam level;

  // ---- The custom speaker ------------------------------------------------------
  mutable std::mutex speakerMutex;
  std::shared_ptr<const cab::SpeakerDescription> custom;
  std::string customText;
  std::atomic<int> speakerGeneration{0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(CabModel) {
  addEffectKind<CabModelProcessor>(registry, "Cab", BlockCategory::Cab);
}

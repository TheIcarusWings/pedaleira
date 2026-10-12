// pitch.octaver: sub-octave voices under the played note, in the spirit of an
// analog octaver (octave down, two octaves down, optionally one up, blended
// with the direct signal), built on the polyphonic engine in PitchShift.h.
// A divide-by-two circuit needs a single clean note; this engine does not, so
// chords work (monophonic tracking is deliberately not offered).
//
// This kind is a BLEND effect: its wet output already contains the direct
// signal (`dryLevelDb`, delayed by the reported latency so it lines up with
// the voices) next to the octaves. The common mix knob is therefore not the
// balance between guitar and octave, it only crossfades this blend against
// the chain's own copy of the dry signal; left at its default of 100 % the
// block sounds like the pedal it models, with its own three level knobs
// setting the balance. The Crossfade law is the right one for the same
// reason: the wet signal replaces the dry one, it is not heard next to it.
//
// The defaults sound like a classic octaver: direct at unity, one octave down
// at about -6 dB, the rest off. The sub voices go through a low-pass for the
// smooth synth-bass tone (the engine's splice artifacts and the guitar's
// pick noise live above it); the octave up is left bright, as it is an
// accent rather than a bass.
#include <algorithm>
#include <cmath>

#include "effects/EffectDsp.h"
#include "detail/PitchVoice.h"

namespace {

class OctaverProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "pitch.octaver";

  enum Param { kDryDb = 0, kDown1Db, kDown2Db, kUp1Db, kSubToneHz, kWindow };

  // The level knobs run to this floor, which means "voice off": a real -60 dB
  // voice would still cost an engine, and nobody wants a -60 dB octave.
  static constexpr float kOffDb = -60.0f;

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(
          BlockParamSpec::floatParam("dryLevelDb", "Direct", "dB", kOffDb, 6.0f, 0.0f).withStep(0.5f));
      t.push_back(BlockParamSpec::floatParam("octDown1Db", "Octave -1", "dB", kOffDb, 6.0f, -6.0f)
                      .withStep(0.5f));
      t.push_back(BlockParamSpec::floatParam("octDown2Db", "Octave -2", "dB", kOffDb, 6.0f, kOffDb)
                      .withStep(0.5f));
      t.push_back(BlockParamSpec::floatParam("octUp1Db", "Octave +1", "dB", kOffDb, 6.0f, kOffDb)
                      .withStep(0.5f));
      // The low-pass on the two sub voices.
      t.push_back(BlockParamSpec::floatParam("subToneHz", "Sub Tone", "Hz", 100.0f, 4000.0f, 700.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::choiceParam("window", "Window", pitchfx::windowLabels(),
                                              static_cast<int>(PitchShift::kDefaultWindow))
                      .withAffectsLatency()
                      .withAffectsTail()
                      .notAutomatable());
      return t;
    }();
    return table;
  }

  OctaverProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  int latencySamples() const noexcept override { return latch.latencySamples(); }
  bool latencyChangePending() const noexcept override { return latch.pending(param(kWindow)); }
  void commitLatency() override { latch.commit(param(kWindow)); }
  double tailSeconds() const noexcept override { return latch.tailSeconds(); }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    maxBlock = juce::jmax(1, spec.maxBlockSize);
    latch.prepare(sampleRate, param(kWindow));
    for (auto& voice : voices)
      voice.prepare(sampleRate, maxBlock, latch.current());
    for (auto& copy : scratch)
      copy.setSize(2, maxBlock, false, true, false);
    // The longest window's latency, with room to spare.
    dryLine.prepare(2, PitchShift::latencySamples(PitchShift::Window::ms60, sampleRate) + 16);
    for (auto& smoother : levels)
      smoother.prepare(sampleRate, kLevelRampSeconds);
    cutoff.prepare(sampleRate, kLevelRampSeconds);
    clearState();
  }

  void resetEffect() noexcept override {
    for (auto& voice : voices)
      voice.markCold();
    clearState();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), 2);
    if (numChannels < 1 || numSamples < 1)
      return;
    const bool dual = ctx.dualMono && numChannels == 2;

    // Level targets (a knob at the floor is silence) and the voices that are
    // worth running: one whose level is off and has finished fading out is
    // not run at all, and starts from rest when it comes back.
    const float dbs[kNumLevels] = {param(kDryDb), param(kDown1Db), param(kDown2Db), param(kUp1Db)};
    for (int i = 0; i < kNumLevels; ++i)
      levels[i].setTarget(dbs[i] <= kOffDb + 0.01f ? 0.0f : fx::dbToGain(dbs[i]));
    cutoff.setTarget(param(kSubToneHz));
    bool running[kNumVoices];
    for (int v = 0; v < kNumVoices; ++v) {
      const SmoothedLevel& level = levels[v + 1];
      running[v] = level.target() > 0.0f || level.isSmoothing();
      if (running[v])
        copyInput(buffer, scratch[v], numChannels, numSamples);
      else
        voices[v].markCold();
    }

    const PitchShift::Window window = latch.current();
    const int delay = latch.latencySamples();
    float* io[2] = {buffer.getWritePointer(0), numChannels > 1 ? buffer.getWritePointer(1) : nullptr};

    grid.run(numSamples, [&](int start, int length, bool boundary) {
      if (boundary)
        for (int ch = 0; ch < numChannels; ++ch)
          subFilter[ch].set(cutoff.current(), kSubQ, sampleRate);
      for (int v = 0; v < kNumVoices; ++v) {
        if (!running[v])
          continue;
        float* pointers[2] = {scratch[v].getWritePointer(0),
                              numChannels > 1 ? scratch[v].getWritePointer(1) : nullptr};
        const float semitones[pitchfx::kMaxLanes] = {kSemitones[v], kSemitones[v]};
        voices[v].process(pointers, numChannels, dual, start, length, semitones, window);
      }
      for (int i = start; i < start + length; ++i) {
        const float direct = levels[0].next();
        const float down1 = levels[1].next();
        const float down2 = levels[2].next();
        const float up1 = levels[3].next();
        cutoff.next();
        for (int ch = 0; ch < numChannels; ++ch) {
          // The direct signal, as late as the voices are on average. The
          // per-channel lines, filters and engines keep two instruments in
          // dual mono apart.
          const float in = io[ch][i];
          const float aligned = dryLine.read(ch, delay);
          dryLine.write(ch, in);
          float sub = 0.0f;
          if (running[0])
            sub += down1 * scratch[0].getReadPointer(ch)[i];
          if (running[1])
            sub += down2 * scratch[1].getReadPointer(ch)[i];
          float out = direct * aligned + subFilter[ch].lowpass(sub);
          if (running[2])
            out += up1 * scratch[2].getReadPointer(ch)[i];
          io[ch][i] = out;
        }
      }
    });
  }

private:
  void clearState() noexcept {
    dryLine.clear();
    for (auto& filter : subFilter)
      filter.reset();
    for (auto& smoother : levels)
      smoother.snapOnNextTarget();
    cutoff.snapOnNextTarget();
    grid.reset();
  }

  using SmoothedLevel = fx::SmoothedParam;
  static constexpr int kNumVoices = 3;
  static constexpr int kNumLevels = kNumVoices + 1;  // the direct signal and the voices
  static constexpr float kSemitones[kNumVoices] = {-12.0f, -24.0f, 12.0f};
  static constexpr double kLevelRampSeconds = 0.02;
  static constexpr double kSubQ = 0.7071;  // Butterworth: a smooth tone, no peak

  static void copyInput(const juce::AudioBuffer<float>& from, juce::AudioBuffer<float>& to,
                        int numChannels, int numSamples) noexcept {
    for (int ch = 0; ch < numChannels; ++ch)
      std::copy_n(from.getReadPointer(ch), numSamples, to.getWritePointer(ch));
  }

  pitchfx::PitchVoice voices[kNumVoices];
  juce::AudioBuffer<float> scratch[kNumVoices];
  fx::DelayLine dryLine;
  fx::StateVariableFilter subFilter[2];
  SmoothedLevel levels[kNumLevels];
  SmoothedLevel cutoff;
  pitchfx::ControlGrid grid;
  pitchfx::WindowLatch latch;
  double sampleRate{48000.0};
  int maxBlock{0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(Octaver) {
  // A blend with its own direct level (see the top of the file): the common
  // mix stays at 100 % and replaces the dry signal with this blend.
  addEffectKind<OctaverProcessor>(registry, "Octaver", BlockCategory::Pitch, 1.0f,
                                  BlockMixLaw::Crossfade);
}

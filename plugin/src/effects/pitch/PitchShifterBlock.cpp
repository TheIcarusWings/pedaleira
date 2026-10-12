// pitch.shifter: the input stage's polyphonic pitch shifter (PitchShift.h,
// plugin/docs/pitch-shift.md) as a chain block: a whammy or drop-tune that can
// sit anywhere in the rig, before an amp or after it. The engine is reused
// through its public interface; this file adds what a block needs around it
// (see detail/PitchVoice.h): a wet-only start, the latched window, a pitch
// glide, controls on a fixed grid and one engine per signal in dual mono.
//
// The wet signal replaces the dry one (the Crossfade mix law, default 100 %),
// which is how a drop-tune or a whammy is used. At a lower mix the dry is
// delayed by the block's latency, as for every latent kind.
//
// That latency is the engine's MEAN tap delay, (floor + window) / 2, which is
// what the main plugin reports for the same engine. A shifter reads its
// signal through a tap that drifts between the floor (2 ms) and the window and
// jumps back, so a held note wanders over that range and only a pick attack is
// pulled back to the floor, a few ms behind the input; the delay of a shifted
// note is therefore an average, not a constant, and no dry delay lines a
// shifted note up with the dry one anyway (the pitches differ). The tests
// measure the average and the spread.
//
// The one setting where the delay IS a constant is no shift at all. The engine
// parks its tap at the floor there, 14 ms earlier than the dry that the chain
// delays by the mean, and a mix below 100 % would comb the two. So at exactly
// 0 st and 0 ct the block plays the dry signal delayed by the reported latency
// itself, crossfading to and from the engine, which keeps running either way.
// Unison is then transparent and its latency exact.
#include <cmath>

#include "effects/EffectDsp.h"
#include "detail/PitchVoice.h"

namespace {

class PitchShifterProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "pitch.shifter";

  enum Param { kSemitones = 0, kCents, kWindow };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      // The interval in whole semitones (a transpose or a drop-tune), and a
      // fine detune on top. A stepped interval plus continuous cents is how a
      // whammy-style pedal is automated too: the sum is what the engine gets.
      t.push_back(BlockParamSpec::floatParam("semitones", "Interval", "st", -24.0f, 24.0f, 0.0f)
                      .withStep(1.0f));
      t.push_back(BlockParamSpec::floatParam("cents", "Fine", "ct", -50.0f, 50.0f, 0.0f)
                      .withStep(1.0f));
      // The engine's buffer: a longer window tracks low notes more cleanly
      // and costs latency (11 / 16 / 21 / 31 ms reported). Changing it moves
      // the latency, which the framework splices in under its edit fade.
      t.push_back(BlockParamSpec::choiceParam("window", "Window", pitchfx::windowLabels(),
                                              static_cast<int>(PitchShift::kDefaultWindow))
                      .withAffectsLatency()
                      .withAffectsTail()
                      .notAutomatable());
      return t;
    }();
    return table;
  }

  PitchShifterProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // Latched with the window, see WindowLatch.
  int latencySamples() const noexcept override { return latch.latencySamples(); }
  bool latencyChangePending() const noexcept override { return latch.pending(param(kWindow)); }
  void commitLatency() override { latch.commit(param(kWindow)); }
  double tailSeconds() const noexcept override { return latch.tailSeconds(); }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    latch.prepare(spec.sampleRate, param(kWindow));
    voice.prepare(spec.sampleRate, spec.maxBlockSize, latch.current());
    glide.prepare(spec.sampleRate, kGlideSeconds);
    unison.prepare(spec.sampleRate, kUnisonSeconds);
    // Room for the longest window's latency.
    dryLine.prepare(2, PitchShift::latencySamples(PitchShift::Window::ms60, spec.sampleRate) + 16);
    aligned.setSize(2, juce::jmax(1, spec.maxBlockSize), false, true, false);
    clearState();
  }

  void resetEffect() noexcept override {
    // Whatever the engine holds is from before the bypass: it starts from
    // rest on the next block that plays (lazily, so a reset stays cheap).
    voice.markCold();
    clearState();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), 2);
    if (numChannels < 1 || numSamples < 1)
      return;
    // Dual mono is two instruments: each gets its own engine and its own
    // delay line, and nothing is shared between them. Nothing here depends on
    // the stereo image.
    const bool dual = ctx.dualMono && numChannels == 2;

    const float semitones = param(kSemitones) + param(kCents) * 0.01f;
    glide.setTarget(semitones);
    unison.setTarget(std::abs(semitones) < kUnisonEpsilon ? 1.0f : 0.0f);
    float* channels[2] = {buffer.getWritePointer(0),
                          numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    const PitchShift::Window window = latch.current();

    // The dry signal as late as the block reports, before the engine
    // overwrites the buffer.
    const int delay = latch.latencySamples();
    for (int ch = 0; ch < numChannels; ++ch) {
      float* late = aligned.getWritePointer(ch);
      for (int i = 0; i < numSamples; ++i) {
        late[i] = dryLine.read(ch, delay);
        dryLine.write(ch, channels[ch][i]);
      }
    }

    grid.run(numSamples, [&](int start, int length, bool boundary) {
      if (boundary)
        glide.tick();
      const float laneSemitones[pitchfx::kMaxLanes] = {glide.value(), glide.value()};
      voice.process(channels, numChannels, dual, start, length, laneSemitones, window);
    });

    for (int i = 0; i < numSamples; ++i) {
      const float toDry = unison.next();
      // This form is exact at both ends (the delayed dry signal, bit for bit,
      // at unison); the other, x + u * (a - x), rounds.
      for (int ch = 0; ch < numChannels; ++ch)
        channels[ch][i] = channels[ch][i] * (1.0f - toDry) + aligned.getReadPointer(ch)[i] * toDry;
    }
  }

private:
  void clearState() noexcept {
    glide.snap();
    unison.snapOnNextTarget();
    dryLine.clear();
    grid.reset();
  }

  // A stepped interval change bends over this long rather than jumping.
  static constexpr double kGlideSeconds = 0.015;
  // The crossfade between the engine and the delayed dry at unison, as long
  // as the engine's own power blend.
  static constexpr double kUnisonSeconds = 0.025;
  // Both knobs are on grids of whole units, so anything but exactly zero is
  // far above this.
  static constexpr float kUnisonEpsilon = 1.0e-4f;

  pitchfx::PitchVoice voice;
  pitchfx::Glide glide;
  pitchfx::ControlGrid grid;
  pitchfx::WindowLatch latch;
  fx::SmoothedParam unison;
  fx::DelayLine dryLine;
  juce::AudioBuffer<float> aligned;
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(PitchShifterBlock) {
  // The shifted signal replaces the dry one, 100 % by default: a drop-tune or
  // a whammy is heard alone. Pulling the mix back crossfades to the dry.
  addEffectKind<PitchShifterProcessor>(registry, "Pitch Shifter", BlockCategory::Pitch, 1.0f,
                                       BlockMixLaw::Crossfade);
}

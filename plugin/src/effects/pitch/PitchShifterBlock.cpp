// pitch.shifter: the input stage's polyphonic pitch shifter (PitchShift.h,
// plugin/docs/pitch-shift.md) as a chain block: a whammy or drop-tune that can
// sit anywhere in the rig, before an amp or after it. The engine is reused
// through its public interface; this file adds what a block needs around it
// (see detail/PitchVoice.h and detail/ShiftPath.h): a wet-only start, the
// latched window, a pitch glide, controls on a fixed grid and one engine per
// signal in dual mono.
//
// The wet signal replaces the dry one (the Crossfade mix law, default 100 %),
// which is how a drop-tune or a whammy is used. At a lower mix the dry is
// delayed by the block's latency, as for every latent kind.
//
// Latency. A shifter reads its signal through a tap that drifts between a
// floor (2 ms) and the window and jumps back, so a held note wanders over that
// range and has no constant delay (a shifted note IS a drifting delay). What
// can be constant is the delay of an event, and the engine's constant-latency
// mode makes it so: every pick attack (and an impulse, and the start of a
// stream) is placed so that it arrives exactly `latencySamples()` after it
// went in, at every interval, on every window. That figure is what the block
// reports (about 9 to 10 ms on the default 30 ms window; the window moves it
// a little) and it does not depend on the interval, so moving the interval
// never moves it. A note that starts without an attack the engine recognises
// arrives wherever the tap is, as before.
//
// At exactly 0 st and 0 ct the block plays the dry signal delayed by the
// reported latency itself, crossfading to and from the engine, which keeps
// running either way. Unison is then transparent and its latency exact for
// the whole signal, not just the attacks.
#include <cmath>

#include "effects/EffectDsp.h"
#include "detail/ShiftPath.h"

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
      // and costs latency. Changing it moves the latency, which the framework
      // splices in under its edit fade.
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
    latch.prepare(spec.sampleRate, param(kWindow), kMaxSemitones);
    path.prepare(spec.sampleRate, spec.maxBlockSize, latch.current(), latch.latencySamples(),
                 kGlideSeconds, kUnisonSeconds);
  }

  // Whatever the engine holds is from before the bypass: it starts from rest
  // on the next block that plays (lazily, so a reset stays cheap).
  void resetEffect() noexcept override { path.reset(); }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    // Dual mono is two instruments: each gets its own engine and its own delay
    // line, and nothing is shared between them.
    path.process(buffer, ctx.dualMono, param(kSemitones) + param(kCents) * 0.01f, latch.current(),
                 latch.latencySamples());
  }

private:
  // A stepped interval change bends over this long rather than jumping.
  static constexpr double kGlideSeconds = 0.015;
  // The crossfade between the engine and the delayed dry at unison, as long
  // as the engine's own power blend.
  static constexpr double kUnisonSeconds = 0.025;
  static constexpr double kMaxSemitones = 24.0;

  pitchfx::ShiftPath path;
  pitchfx::WindowLatch latch;
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(PitchShifterBlock) {
  // The shifted signal replaces the dry one, 100 % by default: a drop-tune or
  // a whammy is heard alone. Pulling the mix back crossfades to the dry.
  addEffectKind<PitchShifterProcessor>(registry, "Pitch Shifter", BlockCategory::Pitch, 1.0f,
                                       BlockMixLaw::Crossfade);
}

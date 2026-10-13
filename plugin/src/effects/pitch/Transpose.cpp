// pitch.transpose "Transpose": a polyphonic transpose for tuning a guitar down
// or up, a whole instrument at a time. -12 to +12 semitones in semitone steps
// plus a fine setting in cents, always fully wet (the mix is 100 % when the
// block is added), meant to sit first in a chain, before the drive and the amp.
//
// It is the pitch family's shifter (detail/ShiftPath.h) with a narrower range,
// which helps the latency a little: the constant attack latency the engine can
// promise (see PitchShift.h) is set by the hardest interval it has to land at.
// The hardest is a small up-shift (about +1 st), which this block has too, so
// on the 30 ms window it reports the same 9.3 ms as the wide shifter; on the
// 20 ms "Low latency" window the narrower range pays: 6.2 ms against 8.0 ms.
// The quality choice is the engine's buffer: a longer one splices less often
// and tracks low notes more cleanly, and costs latency (12.4 and 18.3 ms).
//
// At 0 st and 0 ct the block is an exact delay of its reported latency, bit for
// bit: no shifter is in the path. Every other setting is polyphonic (a chord
// comes out as the chord, not as one note), because the engine splices the
// signal and never analyses it for a pitch.
#include <cmath>

#include "effects/EffectDsp.h"
#include "detail/ShiftPath.h"

namespace {

class TransposeProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "pitch.transpose";

  enum Param { kSemitones = 0, kCents, kQuality };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::floatParam("semitones", "Transpose", "st", -12.0f, 12.0f, 0.0f)
                      .withStep(1.0f));
      t.push_back(BlockParamSpec::floatParam("cents", "Fine", "ct", -50.0f, 50.0f, 0.0f).withStep(1.0f));
      // The engine's window, in the order of PitchShift::Window.
      t.push_back(BlockParamSpec::choiceParam(
                      "quality", "Quality", {"Low latency", "Standard", "Smooth", "Smoothest"},
                      static_cast<int>(PitchShift::kDefaultWindow))
                      .withAffectsLatency()
                      .withAffectsTail()
                      .notAutomatable());
      return t;
    }();
    return table;
  }

  TransposeProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  int latencySamples() const noexcept override { return latch.latencySamples(); }
  bool latencyChangePending() const noexcept override { return latch.pending(param(kQuality)); }
  void commitLatency() override { latch.commit(param(kQuality)); }
  double tailSeconds() const noexcept override { return latch.tailSeconds(); }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    latch.prepare(spec.sampleRate, param(kQuality), kMaxSemitones);
    path.prepare(spec.sampleRate, spec.maxBlockSize, latch.current(), latch.latencySamples(),
                 kGlideSeconds, kUnisonSeconds);
  }

  void resetEffect() noexcept override { path.reset(); }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const float semitones =
        juce::jlimit(-static_cast<float>(kMaxSemitones), static_cast<float>(kMaxSemitones),
                     param(kSemitones) + param(kCents) * 0.01f);
    path.process(buffer, ctx.dualMono, semitones, latch.current(), latch.latencySamples());
  }

private:
  static constexpr double kGlideSeconds = 0.015;
  static constexpr double kUnisonSeconds = 0.025;
  // 12 st and 50 ct on top of it are one setting; the range the latency has to
  // cover is the sum, which processEffect keeps to 12 st.
  static constexpr double kMaxSemitones = 12.0;

  pitchfx::ShiftPath path;
  pitchfx::WindowLatch latch;
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(Transpose) {
  addEffectKind<TransposeProcessor>(registry, "Transpose", BlockCategory::Pitch, 1.0f,
                                    BlockMixLaw::Crossfade);
}

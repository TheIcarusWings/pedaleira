// reverb.shimmer: the hall's feedback delay network with a pitch shifter in
// its feedback path. Some of the sixteen lines read a share of their signal
// an octave (or a fifth) up, so every trip round the network lifts part of
// the tail by that interval: the octaves pile up over the first seconds and
// climb until the damping takes them.
//
// The shifter sits INSIDE the loop, as a convex share of a line's read, and
// not in a send from the output back to the input: that way the loop can
// only lose energy (see FdnTank::readShifted) and the reverb is stable at
// full shimmer and the longest decay without a limiter. What goes up is
// taken from the note it came from, so the decay time is the RT60 with the
// shimmer at 0 and an upper bound otherwise.
#include <cmath>

#include "effects/EffectDsp.h"
#include "effects/dsp/Ducker.h"
#include "effects/reverb/FdnReverbEngine.h"
#include "effects/reverb/ReverbDuck.h"

namespace {

using fx::reverb::FdnReverbEngine;

class ShimmerReverbProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "reverb.shimmer";

  enum Param { kDecayS = 0, kShimmer, kInterval, kDamping, kPreDelayMs, kModulation, kDuckDb, kDuckReleaseMs,
         kSpillover };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::floatParam("decayS", "Decay", "s", 1.0f, 20.0f, 5.0f)
                      .withSkew(0.5f)
                      .withAffectsTail());
      // Brings the four shifters in one after another (octave, interval,
      // octave, interval), a quarter of the travel each.
      t.push_back(BlockParamSpec::floatParam("shimmer", "Shimmer", "%", 0.0f, 100.0f, 40.0f));
      // The second and fourth of the four shifters play the fifth instead
      // of the octave. Each takes up a new interval at its own grain seam, so
      // the switch is click-free and may be automated.
      t.push_back(BlockParamSpec::choiceParam("interval", "Interval",
                                              juce::StringArray{"Octave", "Fifth + octave"}, 0));
      t.push_back(BlockParamSpec::floatParam("damping", "Damping", "%", 0.0f, 100.0f, 35.0f));
      t.push_back(BlockParamSpec::floatParam("preDelayMs", "Pre-delay", "ms", 0.0f, 200.0f, 20.0f)
                      .withSkew(0.5f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("modulation", "Modulation", "%", 0.0f, 100.0f, 30.0f));
      fx::appendDuckParams(t);
      t.push_back(spilloverParam());
      return t;
    }();
    return table;
  }

  ShimmerReverbProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The RT60, after the pre-delay (plus its glide), the diffusers and the
  // longest line with a shifter grain on top. The shimmer only shortens it.
  double tailSeconds() const noexcept override {
    return static_cast<double>(param(kDecayS)) +
           static_cast<double>(param(kPreDelayMs)) * 0.001 +
           FdnReverbEngine::kPreDelayGlideSeconds + kStructureSeconds;
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    FdnReverbEngine::Design d;
    d.tank.lineSeconds = {0.0411, 0.0463, 0.0521, 0.0577, 0.0631, 0.0691, 0.0751, 0.0811,
                          0.0877, 0.0941, 0.1009, 0.1087, 0.1171, 0.1259, 0.1361, 0.1481};
    d.tank.maxModSeconds = kMaxModMs * 0.001;
    d.tank.modRatesHz = {0.47, 0.71};
    d.tank.dampingCornerHz = 4000.0;
    d.tank.numShifters = kShifters;
    // 60 ms grains: long enough to carry a low E without warbling, short
    // enough that the two taps do not read as an echo.
    d.tank.shifterGrainSeconds = 0.06;
    d.tank.shifterLowpassHz = 6500.0;
    d.diffuserStages = 4;
    d.diffuserSeconds = {0.0047, 0.0071, 0.0113, 0.0173};
    d.diffuserCoeffs = {0.7f, 0.7f, 0.6f, 0.6f};
    d.maxPreDelaySeconds = 0.2;
    engine.prepare(d, spec.sampleRate);
    ducking.prepare(spec.sampleRate);
    resetEffect();
  }

  void resetEffect() noexcept override {
    engine.reset();
    ducking.reset();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const bool stereo = blockHasStereoImage(buffer.getNumChannels(), ctx);
    ducking.run(buffer, stereo, param(kDuckDb), param(kDuckReleaseMs),
                [&](juce::AudioBuffer<float>& wet) { processWet(wet, stereo); });
  }

  void processWet(juce::AudioBuffer<float>& buffer, bool stereo) {
    engine.process(buffer, stereo, [this] {
      FdnReverbEngine::Controls c;
      c.rt60 = param(kDecayS);
      c.highRatio = fx::reverb::dampingToHighRatio(param(kDamping));
      c.preDelayMs = param(kPreDelayMs);
      c.modDepthMs = static_cast<float>(kMaxModMs) * param(kModulation) * 0.01f;
      c.lowCutHz = kLowCutHz;
      c.highCutHz = kHighCutHz;
      c.shimmerAmount = param(kShimmer) * 0.01f;
      // 3/2, the just fifth, not the tempered one: with it the grain windows
      // overlap to a constant (see FdnTank::readShifted), and against its own
      // root a pure fifth is the one that does not beat.
      c.shimmerInterval = param(kInterval) >= 0.5f ? 1.5 : 2.0;
      return c;
    });
  }

private:
  static constexpr int kShifters = 4;
  static constexpr double kMaxModMs = 0.6;
  // Fixed wet band: nothing useful below 60 Hz, and the top octave is where
  // the shifters' fold-over would collect.
  static constexpr float kLowCutHz = 60.0f;
  static constexpr float kHighCutHz = 11000.0f;
  static constexpr double kStructureSeconds = 0.27;

  FdnReverbEngine engine;
  fx::reverb::WetDucking ducking;
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(ShimmerReverb) {
  // Heard beside the dry signal: the dry stays at unity and a new block
  // brings the shimmer in at 28% under it.
  addEffectKind<ShimmerReverbProcessor>(registry, "Shimmer", BlockCategory::Reverb, 0.28f,
                                        BlockMixLaw::DryPreserving);
}

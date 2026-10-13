// reverb.modhall: a lush hall. The hall's sixteen-line feedback delay network
// and four allpass diffusers, with the chorus on four of its lines turned
// into a control: the speed of the two slow oscillators (0.1 to 5 Hz) and the
// depth of the excursion are the character of the effect. The depth is
// limited by the speed so the pitch drift of the moving lines stays under
// 0.8% (14 cents) however the two are set: slow and deep is a gentle swell,
// fast and deep stays a shimmer in the tail and never becomes a vibrato. The
// pieces and the decay law are in plugin/include/effects/reverb.
#include <cmath>

#include "effects/EffectDsp.h"
#include "effects/reverb/FdnReverbEngine.h"

namespace {

using fx::reverb::FdnReverbEngine;

class ModulatedHallReverbProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "reverb.modhall";

  enum Param { kDecayS = 0, kPreDelayMs, kModSpeedHz, kModDepth, kLowCutHz, kHighCutHz, kSpillover };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      // The RT60 below the damping corner.
      t.push_back(BlockParamSpec::floatParam("decayS", "Decay", "s", 1.0f, 10.0f, 4.0f)
                      .withSkew(0.55f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("preDelayMs", "Pre-delay", "ms", 0.0f, 200.0f, 20.0f)
                      .withSkew(0.5f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("modSpeedHz", "Mod speed", "Hz", 0.1f, 5.0f, 0.8f)
                      .withSkew(0.5f));
      t.push_back(BlockParamSpec::floatParam("modDepth", "Mod depth", "%", 0.0f, 100.0f, 50.0f));
      t.push_back(BlockParamSpec::floatParam("lowCutHz", "Low cut", "Hz", 20.0f, 1000.0f, 100.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("highCutHz", "High cut", "Hz", 1000.0f, 20000.0f, 9000.0f)
                      .withSkew(0.4f));
      t.push_back(spilloverParam());
      return t;
    }();
    return table;
  }

  ModulatedHallReverbProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The decay time is the RT60, and it starts after the pre-delay (plus its
  // glide), the diffusers and one trip through the longest line.
  double tailSeconds() const noexcept override {
    return static_cast<double>(param(kDecayS)) + static_cast<double>(param(kPreDelayMs)) * 0.001 +
           FdnReverbEngine::kPreDelayGlideSeconds + kStructureSeconds;
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    FdnReverbEngine::Design d;
    d.tank.lineSeconds = {0.0411, 0.0463, 0.0521, 0.0577, 0.0631, 0.0691, 0.0751, 0.0811,
                          0.0877, 0.0941, 0.1009, 0.1087, 0.1171, 0.1259, 0.1361, 0.1481};
    d.tank.maxScale = 1.0;
    d.tank.maxModSeconds = kMaxModMs * 0.001;
    d.tank.modRatesHz = {0.47, 0.71};
    d.tank.dampingCornerHz = 3800.0;
    d.diffuserStages = 4;
    d.diffuserSeconds = {0.0047, 0.0071, 0.0113, 0.0173};
    d.diffuserCoeffs = {0.7f, 0.7f, 0.6f, 0.6f};
    d.maxPreDelaySeconds = 0.2;
    engine.prepare(d, spec.sampleRate);
    resetEffect();
  }

  void resetEffect() noexcept override { engine.reset(); }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    engine.process(buffer, blockHasStereoImage(buffer.getNumChannels(), ctx), [this] {
      FdnReverbEngine::Controls c;
      c.rt60 = param(kDecayS);
      c.highRatio = fx::reverb::dampingToHighRatio(kDampingPercent);
      c.scale = 1.0;
      c.preDelayMs = param(kPreDelayMs);
      c.modRateHz = param(kModSpeedHz);
      c.modDepthMs = fx::reverb::modExcursionMs(param(kModDepth) * 0.01f, param(kModSpeedHz),
                                                kMaxModMs, kMaxDrift);
      c.lowCutHz = param(kLowCutHz);
      c.highCutHz = param(kHighCutHz);
      return c;
    });
  }

private:
  // 1.5 ms each way: at 0.8 Hz that is 0.75% of pitch drift at full depth.
  static constexpr double kMaxModMs = 1.5;
  static constexpr double kMaxDrift = 0.008;
  static constexpr double kDampingPercent = 40.0;
  static constexpr double kStructureSeconds = 0.2;

  FdnReverbEngine engine;
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(ModulatedHallReverb) {
  // Heard beside the dry signal: the dry stays at unity and a new block
  // brings the hall in at 30% under it.
  addEffectKind<ModulatedHallReverbProcessor>(registry, "Modulated Hall", BlockCategory::Reverb,
                                              0.3f, BlockMixLaw::DryPreserving);
}

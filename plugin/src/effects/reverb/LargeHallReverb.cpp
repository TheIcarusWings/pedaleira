// reverb.large: a large algorithmic hall with the controls of a studio unit.
// The family's sixteen-line feedback delay network, with everything that
// shapes a big room exposed:
//
//   decay        0.2 to 70 s, the RT60 of the middle of the spectrum
//   bass         the RT60 below the crossover is `bassMult` times the decay
//                (0.25 to 4): a second shelf per line, in the same loop
//   treble       above the shelf frequency the RT60 is the decay times the
//                shelf gain (-24 to 0 dB as a gain: -12 dB rings a quarter as
//                long)
//   size         the line lengths (40 to 120% of the base hall) and, with them,
//                the reflection times
//   diffusion    early: allpass smear on the eight reflections per side; late:
//                the coefficients of the four allpasses in front of the network
//   attack       the late field swells in instead of starting at once, over up
//                to a third of a second, while the reflections stay crisp
//   modulation   the speed and depth of the four moving lines
//
// Everything that touches the loop keeps it passive (see FdnTank.h), so the
// longest decay with the bass at 4 times is as stable as the shortest. The
// pieces and the decay law are in plugin/include/effects/reverb.
#include <cmath>

#include "effects/EffectDsp.h"
#include "effects/reverb/FdnReverbEngine.h"

namespace {

using fx::reverb::FdnReverbEngine;

class LargeHallReverbProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "reverb.large";

  enum Param {
    kDecayS = 0,
    kPreDelayMs,
    kSize,
    kBassMult,
    kBassFreqHz,
    kHShelfFreqHz,
    kHShelfDb,
    kEarlyDiffusion,
    kLateDiffusion,
    kAttack,
    kModRateHz,
    kModDepth,
    kLowCutHz,
    kHighCutHz,
    kSpillover
  };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::floatParam("decayS", "Decay", "s", 0.2f, 70.0f, 4.0f)
                      .withSkew(0.3f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("preDelayMs", "Pre-delay", "ms", 0.0f, 500.0f, 20.0f)
                      .withSkew(0.5f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("size", "Size", "%", 0.0f, 100.0f, 70.0f));
      // 1 in the middle of the knob's travel.
      t.push_back(BlockParamSpec::floatParam("bassMult", "Bass multiplier", "x", 0.25f, 4.0f, 1.0f)
                      .withSkew(0.43f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("bassFreqHz", "Bass frequency", "Hz", 100.0f, 10000.0f, 300.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("hShelfFreqHz", "High shelf frequency", "Hz", 100.0f,
                                             20000.0f, 4000.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("hShelfDb", "High shelf gain", "dB", -24.0f, 0.0f, -6.0f));
      t.push_back(BlockParamSpec::floatParam("earlyDiffusion", "Early diffusion", "%", 0.0f, 100.0f, 60.0f));
      t.push_back(BlockParamSpec::floatParam("lateDiffusion", "Late diffusion", "%", 0.0f, 100.0f, 80.0f));
      // 0: the reverb starts at once. 100: it swells in over a third of a second.
      t.push_back(BlockParamSpec::floatParam("attack", "Attack", "%", 0.0f, 100.0f, 0.0f));
      t.push_back(BlockParamSpec::floatParam("modRateHz", "Mod rate", "Hz", 0.1f, 10.0f, 0.6f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("modDepth", "Mod depth", "%", 0.0f, 100.0f, 30.0f));
      t.push_back(BlockParamSpec::floatParam("lowCutHz", "Low cut", "Hz", 10.0f, 1500.0f, 60.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("highCutHz", "High cut", "Hz", 100.0f, 20000.0f, 12000.0f)
                      .withSkew(0.4f));
      t.push_back(spilloverParam());
      return t;
    }();
    return table;
  }

  LargeHallReverbProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The RT60 of the slowest band (the bass, when it is stretched), after the
  // pre-delay (plus its glide), the swell of the attack and one trip through
  // the longest line.
  double tailSeconds() const noexcept override {
    return static_cast<double>(param(kDecayS)) * std::max(1.0, static_cast<double>(param(kBassMult))) +
           static_cast<double>(param(kPreDelayMs)) * 0.001 + FdnReverbEngine::kPreDelayGlideSeconds +
           kAttackSeconds + kStructureSeconds;
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    FdnReverbEngine::Design d;
    d.tank.lineSeconds = {0.0521, 0.0587, 0.0661, 0.0733, 0.0809, 0.0887, 0.0971, 0.1063,
                          0.1163, 0.1277, 0.1399, 0.1531, 0.1669, 0.1823, 0.1993, 0.2179};
    d.tank.maxScale = kMaxScale;
    d.tank.maxModSeconds = kMaxModMs * 0.001;
    d.tank.modRatesHz = {0.47, 0.71};
    d.tank.dampingCornerHz = 4000.0;
    d.tank.bassBand = true;
    d.diffuserStages = 4;
    d.diffuserSeconds = {0.0049, 0.0079, 0.0127, 0.0191};
    d.diffuserCoeffs = {0.75f, 0.75f, 0.65f, 0.65f};
    d.earlyDiffuser = true;
    // Reflections of a big hall, 8 to 98 ms at scale 1.
    d.earlyTaps = 8;
    d.earlyLeftSeconds = {0.0083, 0.0151, 0.0241, 0.0343, 0.0471, 0.0617, 0.0791, 0.0983};
    d.earlyRightSeconds = {0.0097, 0.0173, 0.0269, 0.0383, 0.0517, 0.0673, 0.0853, 0.1049};
    d.earlyLeftGains = {1.0f, -0.84f, 0.72f, -0.62f, 0.53f, 0.45f, -0.38f, 0.32f};
    d.earlyRightGains = {1.0f, 0.82f, -0.71f, 0.6f, -0.52f, -0.44f, 0.37f, -0.31f};
    d.earlyEnergy = 0.06;
    d.attackMaxSeconds = kAttackSeconds;
    d.maxPreDelaySeconds = 0.5;
    engine.prepare(d, spec.sampleRate);
    resetEffect();
  }

  void resetEffect() noexcept override { engine.reset(); }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    engine.process(buffer, blockHasStereoImage(buffer.getNumChannels(), ctx), [this] {
      FdnReverbEngine::Controls c;
      c.rt60 = param(kDecayS);
      // The shelf gain as the ratio of the high RT60 to the middle one.
      c.highRatio = std::max(0.02, std::pow(10.0, static_cast<double>(param(kHShelfDb)) * 0.05));
      c.dampCornerHz = param(kHShelfFreqHz);
      c.bassRatio = param(kBassMult);
      c.bassCornerHz = param(kBassFreqHz);
      c.scale = kMinScale + (kMaxScale - kMinScale) * static_cast<double>(param(kSize)) * 0.01;
      c.preDelayMs = param(kPreDelayMs);
      c.earlyDiffusion = static_cast<double>(param(kEarlyDiffusion)) * 0.01;
      c.lateDiffusion = static_cast<double>(param(kLateDiffusion)) * 0.01;
      c.attack = param(kAttack) * 0.01f;
      c.modRateHz = param(kModRateHz);
      c.modDepthMs = fx::reverb::modExcursionMs(param(kModDepth) * 0.01f, param(kModRateHz),
                                                kMaxModMs, kMaxDrift);
      c.lowCutHz = param(kLowCutHz);
      c.highCutHz = param(kHighCutHz);
      return c;
    });
  }

private:
  static constexpr double kMinScale = 0.4;
  static constexpr double kMaxScale = 1.2;
  // 2 ms each way, and never more than 0.6% of pitch drift: 1.6 ms at the
  // default 0.6 Hz, 0.1 ms at 10 Hz.
  static constexpr double kMaxModMs = 2.0;
  static constexpr double kMaxDrift = 0.006;
  static constexpr double kAttackSeconds = 0.35;
  static constexpr double kStructureSeconds = 0.3;

  FdnReverbEngine engine;
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(LargeHallReverb) {
  // Heard beside the dry signal: the dry stays at unity and a new block
  // brings the hall in at 28% under it.
  addEffectKind<LargeHallReverbProcessor>(registry, "Large Hall", BlockCategory::Reverb, 0.28f,
                                          BlockMixLaw::DryPreserving);
}

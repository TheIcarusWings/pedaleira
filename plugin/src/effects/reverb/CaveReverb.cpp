// reverb.cave: an enormous, dark space. The feedback delay network of the
// hall with lines three times as long (71 to 288 ms), so the first echoes are
// spread out the way they are in a cavern, a decay of 5 to 20 seconds and a
// damping that darkens the tail much further than the hall's: the shelves in
// the loops sit at 1.8 kHz and take the highs down to a 25th of the decay at
// the top of the knob. Two very slow oscillators move four lines by less than
// a millisecond, which keeps the long tail from ringing at the lines' modes.
//
// Stability at the longest decay is by construction, not by a limiter: the
// loop is an orthogonal matrix followed, per line, by a gain below one and a
// shelf whose gain never exceeds one (see FdnTank.h), so no setting grows. The
// pieces and the decay law are in plugin/include/effects/reverb.
#include <cmath>

#include "effects/EffectDsp.h"
#include "effects/reverb/FdnReverbEngine.h"

namespace {

using fx::reverb::FdnReverbEngine;

class CaveReverbProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "reverb.cave";

  enum Param { kDecayS = 0, kPreDelayMs, kDamping, kLowCutHz, kHighCutHz, kSpillover };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      // The RT60 below the damping corner.
      t.push_back(BlockParamSpec::floatParam("decayS", "Decay", "s", 5.0f, 20.0f, 10.0f)
                      .withSkew(0.6f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("preDelayMs", "Pre-delay", "ms", 0.0f, 200.0f, 30.0f)
                      .withSkew(0.5f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("damping", "Damping", "%", 0.0f, 100.0f, 60.0f));
      t.push_back(BlockParamSpec::floatParam("lowCutHz", "Low cut", "Hz", 20.0f, 1000.0f, 80.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("highCutHz", "High cut", "Hz", 800.0f, 8000.0f, 5000.0f)
                      .withSkew(0.5f));
      t.push_back(spilloverParam());
      return t;
    }();
    return table;
  }

  CaveReverbProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The decay time is the RT60, and it starts after the pre-delay (plus its
  // glide), the diffusers and one trip through the longest line.
  double tailSeconds() const noexcept override {
    return static_cast<double>(param(kDecayS)) + static_cast<double>(param(kPreDelayMs)) * 0.001 +
           FdnReverbEngine::kPreDelayGlideSeconds + kStructureSeconds;
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    FdnReverbEngine::Design d;
    d.tank.lineSeconds = {0.0711, 0.0801, 0.0897, 0.1003, 0.1117, 0.1229, 0.1361, 0.1493,
                          0.1633, 0.1789, 0.1949, 0.2113, 0.2287, 0.2467, 0.2663, 0.2879};
    d.tank.maxScale = 1.0;
    d.tank.maxModSeconds = kMaxModMs * 0.001;
    d.tank.modRatesHz = {0.31, 0.47};
    d.tank.dampingCornerHz = 1800.0;
    d.diffuserStages = 4;
    d.diffuserSeconds = {0.0071, 0.0113, 0.0179, 0.0277};
    d.diffuserCoeffs = {0.7f, 0.7f, 0.65f, 0.65f};
    d.maxPreDelaySeconds = 0.2;
    engine.prepare(d, spec.sampleRate);
    resetEffect();
  }

  void resetEffect() noexcept override { engine.reset(); }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    engine.process(buffer, blockHasStereoImage(buffer.getNumChannels(), ctx), [this] {
      FdnReverbEngine::Controls c;
      c.rt60 = param(kDecayS);
      // 1 at 0%, down to 1/25 at 100%, exponentially.
      c.highRatio = std::exp(-std::log(25.0) * static_cast<double>(param(kDamping)) * 0.01);
      c.scale = 1.0;
      c.preDelayMs = param(kPreDelayMs);
      c.modDepthMs = static_cast<float>(kMaxModMs);
      c.lowCutHz = param(kLowCutHz);
      c.highCutHz = param(kHighCutHz);
      return c;
    });
  }

private:
  // 0.8 ms each way at 0.31 and 0.47 Hz: under 0.25% of pitch drift.
  static constexpr double kMaxModMs = 0.8;
  static constexpr double kStructureSeconds = 0.4;

  FdnReverbEngine engine;
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(CaveReverb) {
  // Heard beside the dry signal: the dry stays at unity and a new block
  // brings the cave in at 25% under it.
  addEffectKind<CaveReverbProcessor>(registry, "Cave", BlockCategory::Reverb, 0.25f,
                                     BlockMixLaw::DryPreserving);
}

// reverb.room: a small to medium room. Eight early reflections per side off
// the pre-delay, then a short, dense late field from a sixteen-line feedback
// delay network behind three allpass diffusers. The pieces and the decay law
// are in plugin/include/effects/reverb.
#include <cmath>

#include "effects/EffectDsp.h"
#include "effects/reverb/FdnReverbEngine.h"

namespace {

using fx::reverb::FdnReverbEngine;

class RoomReverbProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "reverb.room";

  enum Param { kDecayS = 0, kSize, kPreDelayMs, kDamping, kLowCutHz, kHighCutHz, kSpillover };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      // The RT60 below the damping corner.
      t.push_back(BlockParamSpec::floatParam("decayS", "Decay", "s", 0.2f, 2.5f, 0.8f)
                      .withSkew(0.5f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("size", "Size", "%", 0.0f, 100.0f, 50.0f));
      t.push_back(BlockParamSpec::floatParam("preDelayMs", "Pre-delay", "ms", 0.0f, 200.0f, 10.0f)
                      .withSkew(0.5f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("damping", "Damping", "%", 0.0f, 100.0f, 40.0f));
      t.push_back(BlockParamSpec::floatParam("lowCutHz", "Low cut", "Hz", 20.0f, 1000.0f, 80.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("highCutHz", "High cut", "Hz", 1000.0f, 20000.0f, 9000.0f)
                      .withSkew(0.4f));
      t.push_back(spilloverParam());
      return t;
    }();
    return table;
  }

  RoomReverbProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The decay time is the RT60, and it starts after the pre-delay (plus its
  // glide), the last early reflection and one trip through the longest line.
  double tailSeconds() const noexcept override {
    return static_cast<double>(param(kDecayS)) +
           static_cast<double>(param(kPreDelayMs)) * 0.001 +
           FdnReverbEngine::kPreDelayGlideSeconds + kStructureSeconds;
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    FdnReverbEngine::Design d;
    // 9 to 45 ms: path lengths of 3 to 15 m, a medium room at full size.
    d.tank.lineSeconds = {0.00931, 0.01117, 0.01303, 0.01471, 0.01663, 0.01849, 0.02047, 0.02261,
                          0.02483, 0.02707, 0.02951, 0.03203, 0.03467, 0.03761, 0.04093, 0.04471};
    d.tank.maxScale = kMaxScale;
    // A room does not move, but sixteen short lines at a long decay have few
    // enough modes to ring: six hundredths of a millisecond of drift on four
    // of them (half a cent of pitch) keeps the modes from standing still.
    d.tank.maxModSeconds = 0.0001;
    d.tank.modRatesHz = {0.53, 0.81};
    d.tank.dampingCornerHz = 4500.0;
    d.diffuserStages = 3;
    d.diffuserSeconds = {0.00131, 0.00217, 0.00367, 0.0};
    d.diffuserCoeffs = {0.6f, 0.6f, 0.6f, 0.0f};
    d.earlyTaps = 8;
    d.earlyLeftSeconds = {0.0031, 0.0073, 0.0119, 0.0173, 0.0239, 0.0317, 0.0413, 0.0539};
    d.earlyRightSeconds = {0.0043, 0.0089, 0.0137, 0.0191, 0.0271, 0.0353, 0.0457, 0.0571};
    d.earlyLeftGains = {1.0f, -0.82f, 0.71f, -0.6f, 0.52f, 0.44f, -0.37f, 0.3f};
    d.earlyRightGains = {1.0f, 0.8f, -0.7f, 0.62f, -0.5f, -0.45f, 0.36f, -0.31f};
    // About 6 dB under the late field at the default decay: they place the
    // source in the room without turning into a slap.
    d.earlyEnergy = 0.08;
    d.maxPreDelaySeconds = 0.2;
    engine.prepare(d, spec.sampleRate);
    resetEffect();
  }

  void resetEffect() noexcept override { engine.reset(); }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    engine.process(buffer, blockHasStereoImage(buffer.getNumChannels(), ctx), [this] {
      FdnReverbEngine::Controls c;
      c.rt60 = param(kDecayS);
      c.highRatio = fx::reverb::dampingToHighRatio(param(kDamping));
      c.scale = kMinScale + (kMaxScale - kMinScale) * static_cast<double>(param(kSize)) * 0.01;
      c.preDelayMs = param(kPreDelayMs);
      c.modDepthMs = 0.06f;
      c.lowCutHz = param(kLowCutHz);
      c.highCutHz = param(kHighCutHz);
      return c;
    });
  }

private:
  static constexpr double kMinScale = 0.45;
  static constexpr double kMaxScale = 1.0;
  static constexpr double kStructureSeconds = 0.11;

  FdnReverbEngine engine;
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(RoomReverb) {
  // Heard beside the dry signal: the dry stays at unity and a new block
  // brings the room in at 25% under it.
  addEffectKind<RoomReverbProcessor>(registry, "Room", BlockCategory::Reverb, 0.25f,
                                     BlockMixLaw::DryPreserving);
}

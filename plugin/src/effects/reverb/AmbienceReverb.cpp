// reverb.ambience: a short space that puts the sound in a place without a
// tail. The reflections do the work: eight taps per side off the pre-delay,
// spread over the first 70 ms of a medium room and carrying more energy than
// the late field behind them, which is a sixteen-line feedback delay network
// at the shortest lengths of the family with a tail of a fraction of a
// second. Three sizes move the reflection times, the network's lines and the
// tail together (the early taps and the lines cross-fade to a new size). The
// pieces and the decay law are in plugin/include/effects/reverb.
#include <cmath>

#include "effects/EffectDsp.h"
#include "effects/reverb/FdnReverbEngine.h"

namespace {

using fx::reverb::FdnReverbEngine;

class AmbienceReverbProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "reverb.ambience";

  enum Param { kSize = 0, kPreDelayMs, kLowCutHz, kHighCutHz, kSpillover };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::choiceParam("size", "Size",
                                              juce::StringArray{"Small", "Medium", "Large"}, 1)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("preDelayMs", "Pre-delay", "ms", 0.0f, 100.0f, 0.0f)
                      .withSkew(0.6f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("lowCutHz", "Low cut", "Hz", 20.0f, 800.0f, 100.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("highCutHz", "High cut", "Hz", 800.0f, 12000.0f, 8000.0f)
                      .withSkew(0.4f));
      t.push_back(spilloverParam());
      return t;
    }();
    return table;
  }

  AmbienceReverbProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The decay time is the RT60 of the size, after the pre-delay (plus its
  // glide), the last reflection and one trip through the longest line.
  double tailSeconds() const noexcept override {
    return decayOf(sizeIndex()) + static_cast<double>(param(kPreDelayMs)) * 0.001 +
           FdnReverbEngine::kPreDelayGlideSeconds + kStructureSeconds;
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    FdnReverbEngine::Design d;
    // The room's lines (9 to 45 ms at scale 1); the sizes shrink them.
    d.tank.lineSeconds = {0.00931, 0.01117, 0.01303, 0.01471, 0.01663, 0.01849, 0.02047, 0.02261,
                          0.02483, 0.02707, 0.02951, 0.03203, 0.03467, 0.03761, 0.04093, 0.04471};
    d.tank.maxScale = kScales[2];
    d.tank.maxModSeconds = 0.0001;
    d.tank.modRatesHz = {0.53, 0.81};
    d.tank.dampingCornerHz = 4500.0;
    d.diffuserStages = 3;
    d.diffuserSeconds = {0.00131, 0.00217, 0.00367, 0.0};
    d.diffuserCoeffs = {0.6f, 0.6f, 0.6f, 0.0f};
    // Reflections of a medium room, 4 to 76 ms at scale 1, different on the
    // two sides so a stereo field is wide.
    d.earlyTaps = 8;
    d.earlyLeftSeconds = {0.0041, 0.0097, 0.0163, 0.0229, 0.0311, 0.0419, 0.0541, 0.0701};
    d.earlyRightSeconds = {0.0053, 0.0113, 0.0181, 0.0257, 0.0347, 0.0463, 0.0587, 0.0757};
    d.earlyLeftGains = {1.0f, -0.86f, 0.74f, -0.64f, 0.55f, 0.47f, -0.4f, 0.33f};
    d.earlyRightGains = {1.0f, 0.84f, -0.73f, 0.63f, -0.53f, -0.46f, 0.38f, -0.32f};
    // More than the late field carries at these decays: the reflections are
    // the sound of the effect.
    d.earlyEnergy = 0.4;
    d.maxPreDelaySeconds = 0.1;
    engine.prepare(d, spec.sampleRate);
    resetEffect();
  }

  void resetEffect() noexcept override { engine.reset(); }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    engine.process(buffer, blockHasStereoImage(buffer.getNumChannels(), ctx), [this] {
      const int size = sizeIndex();
      FdnReverbEngine::Controls c;
      c.rt60 = decayOf(size);
      c.highRatio = fx::reverb::dampingToHighRatio(kDampingPercent);
      c.scale = kScales[size];
      c.preDelayMs = param(kPreDelayMs);
      c.modDepthMs = 0.06f;
      c.lowCutHz = param(kLowCutHz);
      c.highCutHz = param(kHighCutHz);
      return c;
    });
  }

private:
  static constexpr double kScales[3] = {0.35, 0.6, 1.0};
  static constexpr double kDecays[3] = {0.18, 0.3, 0.5};
  static constexpr double kDampingPercent = 50.0;
  static constexpr double kStructureSeconds = 0.12;

  int sizeIndex() const noexcept { return juce::jlimit(0, 2, static_cast<int>(param(kSize) + 0.5f)); }
  static double decayOf(int size) noexcept { return kDecays[size]; }

  FdnReverbEngine engine;
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(AmbienceReverb) {
  // Heard beside the dry signal: the dry stays at unity and a new block
  // brings the space in at 30% under it.
  addEffectKind<AmbienceReverbProcessor>(registry, "Ambience", BlockCategory::Reverb, 0.3f,
                                         BlockMixLaw::DryPreserving);
}

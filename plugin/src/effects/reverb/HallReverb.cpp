// reverb.hall: a large hall. The same sixteen-line feedback delay network as
// the room, with lines four times as long (41 to 148 ms), four allpass
// diffusers in front and no early taps: nothing arrives before the first
// line has been crossed, so the sound builds over the first tenth of a
// second instead of starting at once, and four slowly modulated lines keep
// a tail of many seconds from ringing. The pieces and the decay law are in
// plugin/include/effects/reverb.
#include <cmath>

#include "effects/EffectDsp.h"
#include "effects/dsp/Ducker.h"
#include "effects/reverb/FdnReverbEngine.h"
#include "effects/reverb/ReverbDuck.h"

namespace {

using fx::reverb::FdnReverbEngine;

class HallReverbProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "reverb.hall";

  enum Param {
    kDecayS = 0,
    kSize,
    kPreDelayMs,
    kDamping,
    kLowCutHz,
    kHighCutHz,
    kModulation,
    kDuckDb,
    kDuckReleaseMs,
    kSpillover
  };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      // The RT60 below the damping corner.
      t.push_back(BlockParamSpec::floatParam("decayS", "Decay", "s", 1.0f, 12.0f, 3.0f)
                      .withSkew(0.5f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("size", "Size", "%", 0.0f, 100.0f, 60.0f));
      t.push_back(BlockParamSpec::floatParam("preDelayMs", "Pre-delay", "ms", 0.0f, 200.0f, 25.0f)
                      .withSkew(0.5f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("damping", "Damping", "%", 0.0f, 100.0f, 45.0f));
      t.push_back(BlockParamSpec::floatParam("lowCutHz", "Low cut", "Hz", 20.0f, 1000.0f, 100.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("highCutHz", "High cut", "Hz", 1000.0f, 20000.0f, 8000.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("modulation", "Modulation", "%", 0.0f, 100.0f, 30.0f));
      fx::appendDuckParams(t);
      t.push_back(spilloverParam());
      return t;
    }();
    return table;
  }

  HallReverbProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The decay time is the RT60, and it starts after the pre-delay (plus its
  // glide), the diffusers and one trip through the longest line.
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
    d.tank.maxScale = kMaxScale;
    d.tank.maxModSeconds = kMaxModMs * 0.001;
    d.tank.modRatesHz = {0.47, 0.71};
    d.tank.dampingCornerHz = 3500.0;
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
      c.scale = kMinScale + (kMaxScale - kMinScale) * static_cast<double>(param(kSize)) * 0.01;
      c.preDelayMs = param(kPreDelayMs);
      c.modDepthMs = static_cast<float>(kMaxModMs) * param(kModulation) * 0.01f;
      c.lowCutHz = param(kLowCutHz);
      c.highCutHz = param(kHighCutHz);
      return c;
    });
  }

private:
  static constexpr double kMinScale = 0.5;
  static constexpr double kMaxScale = 1.0;
  // 0.6 ms each way at 0.47 and 0.71 Hz: under five cents of pitch drift at
  // full depth, enough to smear the modes and not enough to hear as chorus.
  static constexpr double kMaxModMs = 0.6;
  static constexpr double kStructureSeconds = 0.2;

  FdnReverbEngine engine;
  fx::reverb::WetDucking ducking;
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(HallReverb) {
  // Heard beside the dry signal: the dry stays at unity and a new block
  // brings the hall in at 28% under it.
  addEffectKind<HallReverbProcessor>(registry, "Hall", BlockCategory::Reverb, 0.28f,
                                     BlockMixLaw::DryPreserving);
}

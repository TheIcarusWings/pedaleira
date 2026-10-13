// reverb.vintage: the sound of an early digital reverb. Three things make it:
//
//   band limit   those units ran at a low sample rate, so nothing above a few
//                kHz went in. An eighth-order low-pass at 7 kHz in front of
//                the network does that; the High cut after it is the knob.
//   grain        a sparser network than the hall (shorter lines, three light
//                diffusers, a touch of modulation) and an optional word
//                length: the wet signal is rounded to 14, 12 or 10 bits, so
//                the tail dissolves into grain as it dies instead of fading
//                smoothly.
//   freeze       while on, the loops are lossless and the input is shut, so
//                what is in the network when the switch goes down is held for
//                as long as it stays down (the damping shelves are out, and
//                the loop is an orthogonal matrix, so it neither decays nor
//                grows). Off, the tail takes the decay setting up again.
//
// The pieces and the decay law are in plugin/include/effects/reverb.
#include <cmath>

#include "effects/EffectDsp.h"
#include "effects/reverb/FdnReverbEngine.h"

namespace {

using fx::reverb::FdnReverbEngine;

class VintageDigitalReverbProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "reverb.vintage";

  enum Param {
    kSize = 0,
    kDecayS,
    kPreDelayMs,
    kDamping,
    kModulation,
    kFreeze,
    kWordLength,
    kLowCutHz,
    kHighCutHz,
    kSpillover
  };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::floatParam("size", "Size", "%", 0.0f, 100.0f, 50.0f));
      // The RT60 below the damping corner.
      t.push_back(BlockParamSpec::floatParam("decayS", "Decay", "s", 0.3f, 15.0f, 2.5f)
                      .withSkew(0.5f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("preDelayMs", "Pre-delay", "ms", 0.0f, 200.0f, 0.0f)
                      .withSkew(0.5f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("damping", "Damping", "%", 0.0f, 100.0f, 50.0f));
      t.push_back(BlockParamSpec::floatParam("modulation", "Modulation", "%", 0.0f, 100.0f, 30.0f));
      t.push_back(BlockParamSpec::boolParam("freeze", "Freeze", false).withAffectsTail());
      t.push_back(BlockParamSpec::choiceParam(
          "wordLength", "Word length", juce::StringArray{"Off", "14 bit", "12 bit", "10 bit"}, 1));
      t.push_back(BlockParamSpec::floatParam("lowCutHz", "Low cut", "Hz", 20.0f, 500.0f, 80.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("highCutHz", "High cut", "Hz", 1500.0f, 20000.0f, 9000.0f)
                      .withSkew(0.4f));
      t.push_back(spilloverParam());
      return t;
    }();
    return table;
  }

  VintageDigitalReverbProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The decay time is the RT60, after the pre-delay (plus its glide) and one
  // trip through the longest line. Frozen, the tail does not end (the chain
  // caps what is reported at its own limit).
  double tailSeconds() const noexcept override {
    if (param(kFreeze) >= 0.5f)
      return kTailForever;
    return static_cast<double>(param(kDecayS)) + static_cast<double>(param(kPreDelayMs)) * 0.001 +
           FdnReverbEngine::kPreDelayGlideSeconds + kStructureSeconds;
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    FdnReverbEngine::Design d;
    d.tank.lineSeconds = {0.0173, 0.0197, 0.0223, 0.0251, 0.0283, 0.0317, 0.0353, 0.0391,
                          0.0433, 0.0479, 0.0529, 0.0583, 0.0641, 0.0703, 0.0769, 0.0839};
    d.tank.maxScale = kMaxScale;
    d.tank.maxModSeconds = kMaxModMs * 0.001;
    d.tank.modRatesHz = {1.1, 1.7};
    d.tank.dampingCornerHz = 2800.0;
    d.diffuserStages = 3;
    d.diffuserSeconds = {0.0037, 0.0061, 0.0097, 0.0};
    d.diffuserCoeffs = {0.55f, 0.55f, 0.5f, 0.0f};
    d.maxPreDelaySeconds = 0.2;
    engine.prepare(d, sampleRate);
    // An eighth-order Butterworth low-pass as four biquads.
    constexpr double kQ[kBandSections] = {0.5098, 0.6013, 0.9000, 2.5629};
    for (int ch = 0; ch < 2; ++ch)
      for (int k = 0; k < kBandSections; ++k)
        bandLimit[ch][k].setCoeffs(fx::BiquadCoeffs::lowpass(kBandLimitHz, kQ[k], sampleRate));
    resetEffect();
  }

  void resetEffect() noexcept override {
    engine.reset();
    for (auto& channel : bandLimit)
      for (auto& section : channel)
        section.reset();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int channels = juce::jmin(buffer.getNumChannels(), 2);
    for (int ch = 0; ch < channels; ++ch) {
      float* samples = buffer.getWritePointer(ch);
      for (int i = 0; i < buffer.getNumSamples(); ++i) {
        float x = samples[i];
        for (auto& section : bandLimit[ch])
          x = section.process(x);
        samples[i] = x;
      }
    }
    engine.process(buffer, blockHasStereoImage(buffer.getNumChannels(), ctx), [this] {
      FdnReverbEngine::Controls c;
      c.rt60 = param(kDecayS);
      c.highRatio = fx::reverb::dampingToHighRatio(param(kDamping));
      c.scale = kMinScale + (kMaxScale - kMinScale) * static_cast<double>(param(kSize)) * 0.01;
      c.preDelayMs = param(kPreDelayMs);
      c.modDepthMs = static_cast<float>(kMaxModMs) * param(kModulation) * 0.01f;
      c.lowCutHz = param(kLowCutHz);
      c.highCutHz = param(kHighCutHz);
      c.hold = param(kFreeze) >= 0.5f;
      return c;
    });
    quantise(buffer);
  }

private:
  static constexpr double kMinScale = 0.5;
  static constexpr double kMaxScale = 1.0;
  static constexpr double kMaxModMs = 0.4;
  static constexpr int kBandSections = 4;
  static constexpr double kBandLimitHz = 7000.0;
  static constexpr double kStructureSeconds = 0.15;
  static constexpr double kTailForever = 1.0e6;

  /** Rounds the wet to the chosen word length (signed, full scale 1). */
  void quantise(juce::AudioBuffer<float>& buffer) noexcept {
    static constexpr float kStepsPerUnit[4] = {0.0f, 8192.0f, 2048.0f, 512.0f};
    const int choice = juce::jlimit(0, 3, static_cast<int>(param(kWordLength) + 0.5f));
    const float steps = kStepsPerUnit[choice];
    if (steps == 0.0f)
      return;
    const float inverse = 1.0f / steps;
    const int channels = juce::jmin(buffer.getNumChannels(), 2);
    for (int ch = 0; ch < channels; ++ch) {
      float* samples = buffer.getWritePointer(ch);
      for (int i = 0; i < buffer.getNumSamples(); ++i)
        samples[i] = std::nearbyint(samples[i] * steps) * inverse;
    }
  }

  FdnReverbEngine engine;
  fx::Biquad bandLimit[2][kBandSections];
  double sampleRate{48000.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(VintageDigitalReverb) {
  // Heard beside the dry signal: the dry stays at unity and a new block
  // brings the reverb in at 30% under it.
  addEffectKind<VintageDigitalReverbProcessor>(registry, "Vintage Digital", BlockCategory::Reverb,
                                               0.3f, BlockMixLaw::DryPreserving);
}

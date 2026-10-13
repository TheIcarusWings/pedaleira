// reverb.ducking: a bright, dense, plate-like reverb whose wet signal gets out
// of the way of the playing. While the input is loud the whole wet output is
// lowered by the set depth; when the playing stops it comes back over the
// release time, so the tail blooms in the gaps between phrases. The ducking
// is the one every reverb and delay has (plugin/include/effects/dsp/Ducker.h,
// plugin/include/effects/reverb/ReverbDuck.h): it acts on the output only,
// never on the loop, so the tail keeps circulating at full level underneath.
//
// The reverb itself is the family's sixteen-line network at plate-like
// lengths (8 to 46 ms, no early reflections), four allpass diffusers whose
// coefficients are the diffusion knob, little damping and a bass and a
// treble shelf on the wet signal. The pieces and the decay law are in
// plugin/include/effects/reverb.
#include <cmath>

#include "effects/EffectDsp.h"
#include "effects/dsp/Ducker.h"
#include "effects/reverb/FdnReverbEngine.h"
#include "effects/reverb/ReverbDuck.h"

namespace {

using fx::reverb::FdnReverbEngine;

class DuckingReverbProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "reverb.ducking";

  enum Param {
    kDecayS = 0,
    kPreDelayMs,
    kDiffusion,
    kBassDb,
    kTrebleDb,
    kModulation,
    kLowCutHz,
    kHighCutHz,
    kDuckDb,
    kDuckReleaseMs,
    kSpillover
  };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      // The RT60 below the damping corner.
      t.push_back(BlockParamSpec::floatParam("decayS", "Decay", "s", 0.3f, 20.0f, 2.5f)
                      .withSkew(0.5f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("preDelayMs", "Pre-delay", "ms", 0.0f, 500.0f, 0.0f)
                      .withSkew(0.5f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("diffusion", "Diffusion", "%", 0.0f, 100.0f, 85.0f));
      t.push_back(BlockParamSpec::floatParam("bassDb", "Bass", "dB", -18.0f, 12.0f, 0.0f));
      t.push_back(BlockParamSpec::floatParam("trebleDb", "Treble", "dB", -18.0f, 12.0f, 0.0f));
      t.push_back(BlockParamSpec::floatParam("modulation", "Modulation", "%", 0.0f, 100.0f, 20.0f));
      t.push_back(BlockParamSpec::floatParam("lowCutHz", "Low cut", "Hz", 20.0f, 800.0f, 80.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("highCutHz", "High cut", "Hz", 1000.0f, 20000.0f, 14000.0f)
                      .withSkew(0.4f));
      fx::appendDuckParams(t, kDefaultDuckDb);
      t.push_back(spilloverParam());
      return t;
    }();
    return table;
  }

  DuckingReverbProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The decay time is the RT60, after the pre-delay (plus its glide) and one
  // trip through the longest line. Ducking only lowers the output.
  double tailSeconds() const noexcept override {
    return static_cast<double>(param(kDecayS)) + static_cast<double>(param(kPreDelayMs)) * 0.001 +
           FdnReverbEngine::kPreDelayGlideSeconds + kStructureSeconds;
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    FdnReverbEngine::Design d;
    d.tank.lineSeconds = {0.0083, 0.0097, 0.0113, 0.0127, 0.0143, 0.0161, 0.0179, 0.0199,
                          0.0223, 0.0247, 0.0271, 0.0299, 0.0331, 0.0367, 0.0409, 0.0461};
    d.tank.maxScale = 1.0;
    d.tank.maxModSeconds = kMaxModMs * 0.001;
    d.tank.modRatesHz = {0.83, 1.07};
    d.tank.dampingCornerHz = 5000.0;
    d.diffuserStages = 4;
    d.diffuserSeconds = {0.0030, 0.0047, 0.0071, 0.0113};
    d.diffuserCoeffs = {0.75f, 0.75f, 0.65f, 0.65f};
    d.maxPreDelaySeconds = 0.5;
    engine.prepare(d, sampleRate);
    ducking.prepare(sampleRate);
    resetEffect();
  }

  void resetEffect() noexcept override {
    engine.reset();
    ducking.reset();
    for (auto& filter : bass)
      filter.reset();
    for (auto& filter : treble)
      filter.reset();
    appliedBassDb = appliedTrebleDb = 1.0e9f;
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const bool stereo = blockHasStereoImage(buffer.getNumChannels(), ctx);
    updateTone();
    ducking.run(buffer, stereo, param(kDuckDb), param(kDuckReleaseMs),
                [&](juce::AudioBuffer<float>& wet) {
                  engine.process(wet, stereo, [this] {
                    FdnReverbEngine::Controls c;
                    c.rt60 = param(kDecayS);
                    c.highRatio = fx::reverb::dampingToHighRatio(kDampingPercent);
                    c.scale = 1.0;
                    c.preDelayMs = param(kPreDelayMs);
                    c.lateDiffusion = static_cast<double>(param(kDiffusion)) * 0.01;
                    c.modDepthMs = static_cast<float>(kMaxModMs) * param(kModulation) * 0.01f;
                    c.lowCutHz = param(kLowCutHz);
                    c.highCutHz = param(kHighCutHz);
                    return c;
                  });
                  applyTone(wet);
                });
  }

private:
  static constexpr float kDefaultDuckDb = 12.0f;
  static constexpr double kDampingPercent = 20.0;
  // 0.3 ms each way at 0.83 and 1.07 Hz: under 0.2% of pitch drift.
  static constexpr double kMaxModMs = 0.3;
  static constexpr double kStructureSeconds = 0.1;
  static constexpr double kBassHz = 250.0;
  static constexpr double kTrebleHz = 3500.0;

  void updateTone() noexcept {
    const float bassDb = param(kBassDb), trebleDb = param(kTrebleDb);
    if (bassDb != appliedBassDb) {
      const auto coeffs = fx::BiquadCoeffs::lowShelf(kBassHz, 0.7071, bassDb, sampleRate);
      for (auto& filter : bass)
        filter.setCoeffs(coeffs);
      appliedBassDb = bassDb;
    }
    if (trebleDb != appliedTrebleDb) {
      const auto coeffs = fx::BiquadCoeffs::highShelf(kTrebleHz, 0.7071, trebleDb, sampleRate);
      for (auto& filter : treble)
        filter.setCoeffs(coeffs);
      appliedTrebleDb = trebleDb;
    }
  }

  void applyTone(juce::AudioBuffer<float>& wet) noexcept {
    const bool useBass = std::abs(appliedBassDb) >= 0.05f;
    const bool useTreble = std::abs(appliedTrebleDb) >= 0.05f;
    if (!useBass && !useTreble) {
      // Flat: out of the path, and rested so it starts clean when it returns.
      for (int c = 0; c < 2; ++c) {
        bass[c].reset();
        treble[c].reset();
      }
      return;
    }
    // Both run while either is in use (a flat shelf is the identity), so
    // switching one on never starts from stale state.
    const int channels = juce::jmin(wet.getNumChannels(), 2);
    for (int ch = 0; ch < channels; ++ch) {
      float* samples = wet.getWritePointer(ch);
      const size_t c = static_cast<size_t>(ch);
      for (int i = 0; i < wet.getNumSamples(); ++i)
        samples[i] = treble[c].process(bass[c].process(samples[i]));
    }
  }

  FdnReverbEngine engine;
  fx::reverb::WetDucking ducking;
  fx::Biquad bass[2];
  fx::Biquad treble[2];
  double sampleRate{48000.0};
  float appliedBassDb{1.0e9f};
  float appliedTrebleDb{1.0e9f};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(DuckingReverb) {
  // Heard beside the dry signal: the dry stays at unity and a new block
  // brings the plate in at 35% under it.
  addEffectKind<DuckingReverbProcessor>(registry, "Ducking Reverb", BlockCategory::Reverb, 0.35f,
                                        BlockMixLaw::DryPreserving);
}

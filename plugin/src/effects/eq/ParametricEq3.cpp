// eq.parametric3: three fully parametric bands and an output level.
//
// Each band has frequency (20 Hz to 20 kHz), gain (+-18 dB), Q (0.1 to 10)
// and a bypass. The outer two can be a peak or a shelf (band 1 a low shelf,
// band 3 a high shelf); the middle one is a peak. The engine is
// effects/dsp/EqBank.h: coefficients glide on a fixed 16-sample grid, and a
// flat or bypassed band is skipped, so the neutral setting is bit-exact.
#include "effects/EffectDsp.h"
#include "effects/dsp/EqParams.h"

namespace {

using fx::eq::BandTarget;
using fx::eq::BandType;

class ParametricEq3Processor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "eq.parametric3";
  static constexpr int kNumBands = 3;

  // Table layout: band 1 (type, freq, gain, Q, bypass), band 2 (freq, gain,
  // Q, bypass), band 3 (type, freq, gain, Q, bypass), then the level.
  static constexpr int kBandStart[kNumBands] = {0, 5, 9};
  static constexpr bool kHasType[kNumBands] = {true, false, true};
  static constexpr int kLevelIndex = 14;

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      fx::eq::addParametricBand(t, 1, juce::StringArray{"Peak", "Shelf"}, 120.0f);
      fx::eq::addParametricBand(t, 2, juce::StringArray(), 1000.0f);
      fx::eq::addParametricBand(t, 3, juce::StringArray{"Peak", "Shelf"}, 6000.0f);
      t.push_back(BlockParamSpec::floatParam("levelDb", "Output", "dB", -20.0f, 20.0f, 0.0f));
      return t;
    }();
    return table;
  }

  ParametricEq3Processor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  double tailSeconds() const noexcept override {
    BandTarget targets[kNumBands];
    readTargets(targets);
    return fx::eq::Bank<kNumBands>::tailSeconds(targets);
  }

protected:
  void prepareEffect(const EffectSpec& spec) override { bank.prepare(spec.sampleRate); }
  void resetEffect() noexcept override { bank.reset(); }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext&) override {
    BandTarget targets[kNumBands];
    readTargets(targets);
    bank.setTargets(targets, param(kLevelIndex));
    bank.process(buffer.getArrayOfWritePointers(), buffer.getNumChannels(),
                 buffer.getNumSamples());
  }

private:
  void readTargets(BandTarget* targets) const noexcept {
    for (int b = 0; b < kNumBands; ++b) {
      int at = kBandStart[b];
      BandType type = BandType::Peak;
      if (kHasType[b] && param(at++) >= 0.5f)
        type = b == 0 ? BandType::LowShelf : BandType::HighShelf;
      targets[b].type = type;
      targets[b].hz = param(at);
      targets[b].gainDb = param(at + 1);
      targets[b].q = param(at + 2);
      targets[b].active = param(at + 3) < 0.5f;
    }
  }

  fx::eq::Bank<kNumBands> bank;
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(ParametricEq3) {
  addEffectKind<ParametricEq3Processor>(registry, "Parametric EQ 3", BlockCategory::Eq);
}

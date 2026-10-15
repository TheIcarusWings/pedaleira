// eq.parametric8: eight fully parametric bands and an output level.
//
// Each band has frequency (20 Hz to 20 kHz), gain (+-18 dB), Q (0.1 to 10)
// and a bypass. Band 1 is a peak, a low shelf or a 12 dB/oct high-pass; band
// 8 a peak, a high shelf or a 12 dB/oct low-pass; bands 2 to 7 are peaks. A
// pass band takes no gain (its Q is the resonance at the corner). The engine
// is effects/dsp/EqBank.h.
#include "effects/EffectDsp.h"
#include "effects/dsp/EqParams.h"

namespace {

using fx::eq::BandTarget;
using fx::eq::BandType;

class ParametricEq8Processor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "eq.parametric8";
  static constexpr int kNumBands = 8;

  // Band 1 and band 8 carry a type (five entries), the others four.
  static constexpr int bandStart(int band) { return band == 0 ? 0 : 5 + (band - 1) * 4; }
  static constexpr int kLevelIndex = 5 + 6 * 4 + 5;
  // One shape control per band follows the level: Default keeps the type
  // control's meaning, the rest set the filter outright.
  static constexpr int kShapeStart = kLevelIndex + 1;

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      const float freqs[kNumBands] = {40.0f,   120.0f,  300.0f,  700.0f,
                                      1500.0f, 3200.0f, 6500.0f, 12000.0f};
      for (int b = 0; b < kNumBands; ++b) {
        juce::StringArray types;
        if (b == 0)
          types = juce::StringArray{"Peak", "Shelf", "High-pass"};
        else if (b == kNumBands - 1)
          types = juce::StringArray{"Peak", "Shelf", "Low-pass"};
        fx::eq::addParametricBand(t, b + 1, types, freqs[b]);
      }
      t.push_back(BlockParamSpec::floatParam("levelDb", "Output", "dB", -20.0f, 20.0f, 0.0f));
      fx::eq::addShapeParams(t, kNumBands);
      return t;
    }();
    return table;
  }

  ParametricEq8Processor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

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
      int at = bandStart(b);
      BandType type = BandType::Peak;
      if (b == 0 || b == kNumBands - 1) {
        const int choice = static_cast<int>(param(at++) + 0.5f);
        if (choice == 1)
          type = b == 0 ? BandType::LowShelf : BandType::HighShelf;
        else if (choice == 2)
          type = b == 0 ? BandType::HighPass2 : BandType::LowPass2;
      }
      fx::eq::applyShape(static_cast<int>(param(kShapeStart + b) + 0.5f), type);
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

PEDALEIRA_EFFECT_KINDS(ParametricEq8) {
  addEffectKind<ParametricEq8Processor>(registry, "Parametric EQ 8", BlockCategory::Eq);
}

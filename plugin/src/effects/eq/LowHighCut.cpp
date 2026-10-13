// eq.cut: a high-pass and a low-pass filter in one block, each with its own
// corner (20 Hz to 20 kHz) and slope (off, 6, 12, 18, 24, 36 or 48 dB per
// octave), and an output level.
//
// Each filter is a Butterworth cascade: a first-order section for an odd
// order plus second-order sections at the Butterworth Qs, so the corner is
// the -3 dB point at every slope. Slope changes cross-fade the sections in
// and out and glide their Q (see effects/dsp/EqBank.h), and a filter that is
// off is skipped, so with both off the block is bit-exact pass-through.
#include "effects/EffectDsp.h"
#include "effects/dsp/EqParams.h"

namespace {

using fx::eq::BandTarget;
using fx::eq::BandType;

class LowHighCutProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "eq.cut";

  enum Param { kHpSlope = 0, kHpFreq, kLpSlope, kLpFreq, kLevelDb };

  // Slope choice index to filter order (0 = off).
  static constexpr int kOrders[7] = {0, 1, 2, 3, 4, 6, 8};
  // One first-order section and up to four second-order ones per filter.
  static constexpr int kStagesPerFilter = 5;
  static constexpr int kNumStages = 2 * kStagesPerFilter;

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      const juce::StringArray slopes{"Off",       "6 dB/oct",  "12 dB/oct", "18 dB/oct",
                                     "24 dB/oct", "36 dB/oct", "48 dB/oct"};
      t.push_back(BlockParamSpec::choiceParam("hpSlope", "High-pass slope", slopes, 0)
                      .withAffectsTail());
      t.push_back(fx::eq::frequencyParam("hpFreq", "High-pass frequency", fx::eq::kFreqMinHz,
                                         fx::eq::kFreqMaxHz, 80.0f));
      t.push_back(BlockParamSpec::choiceParam("lpSlope", "Low-pass slope", slopes, 0)
                      .withAffectsTail());
      t.push_back(fx::eq::frequencyParam("lpFreq", "Low-pass frequency", fx::eq::kFreqMinHz,
                                         fx::eq::kFreqMaxHz, 12000.0f));
      t.push_back(BlockParamSpec::floatParam("levelDb", "Output", "dB", -20.0f, 20.0f, 0.0f));
      return t;
    }();
    return table;
  }

  LowHighCutProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  double tailSeconds() const noexcept override {
    BandTarget targets[kNumStages];
    readTargets(targets);
    return fx::eq::Bank<kNumStages>::tailSeconds(targets);
  }

protected:
  void prepareEffect(const EffectSpec& spec) override { bank.prepare(spec.sampleRate); }
  void resetEffect() noexcept override { bank.reset(); }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext&) override {
    BandTarget targets[kNumStages];
    readTargets(targets);
    bank.setTargets(targets, param(kLevelDb));
    bank.process(buffer.getArrayOfWritePointers(), buffer.getNumChannels(),
                 buffer.getNumSamples());
  }

private:
  // Stage 0 of a filter is its first-order section, stages 1 to 4 its
  // second-order ones. An unused stage keeps the plain Butterworth Q so its
  // fade-out does not also move it.
  static void fillFilter(BandTarget* stages, int slopeChoice, float hz, bool highPass) noexcept {
    const int order = kOrders[std::clamp(slopeChoice, 0, 6)];
    const int biquads = order / 2;
    stages[0].type = highPass ? BandType::HighPass1 : BandType::LowPass1;
    stages[0].hz = hz;
    stages[0].q = 0.5f;
    stages[0].active = (order % 2) == 1;
    for (int k = 0; k < kStagesPerFilter - 1; ++k) {
      BandTarget& stage = stages[1 + k];
      stage.type = highPass ? BandType::HighPass2 : BandType::LowPass2;
      stage.hz = hz;
      stage.active = k < biquads;
      stage.q = stage.active ? static_cast<float>(fx::eq::butterworthQ(order, k)) : 0.7071f;
    }
  }

  void readTargets(BandTarget* targets) const noexcept {
    fillFilter(targets, static_cast<int>(param(kHpSlope) + 0.5f), param(kHpFreq), true);
    fillFilter(targets + kStagesPerFilter, static_cast<int>(param(kLpSlope) + 0.5f),
               param(kLpFreq), false);
  }

  fx::eq::Bank<kNumStages> bank;
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(LowHighCut) {
  addEffectKind<LowHighCutProcessor>(registry, "Low and High Cut", BlockCategory::Eq);
}

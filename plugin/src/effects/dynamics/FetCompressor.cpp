// dynamics.fet: a fast studio compressor in the field-effect-transistor
// style. The threshold is fixed; the Input knob raises the level into it, so
// turning it up is what compresses more, and Output brings the level back.
//
//   in -> input gain -> detector (peak, linked) -> curve -> reduction
//      -> gain -> [colour] -> output gain
//
// - Attack 20 us to 800 us and release 50 ms to 1.1 s, both one-poles on the
//   gain reduction in dB.
// - Ratio is a choice: 4, 8, 12, 20, and "All", every button in: ratio 20 with
//   a quicker, more nervous release and twice the colour.
// - The release is mildly programme dependent: after long, deep reduction it
//   takes up to 1.5 times as long, after a short peak 0.7 of the set time.
// - Colour is a small amount of odd and even harmonics on the compressed
//   signal, growing with the level; off, the path is exactly linear.
//
// The gain reduction is reported as a reading (gainReductionDb).
#include <cmath>
#include "effects/EffectDsp.h"
#include "effects/dynamics/DynamicsCommon.h"

namespace {

using namespace dynfx;

class FetCompressorProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "dynamics.fet";

  enum Param { kInputDb = 0, kRatio, kAttackUs, kReleaseMs, kOutputDb, kColour };
  enum Reading { kGainReduction = 0 };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::floatParam("inputDb", "Input", "dB", 0.0f, 40.0f, 12.0f));
      t.push_back(BlockParamSpec::choiceParam("ratio", "Ratio", {"4", "8", "12", "20", "All"}, 0));
      t.push_back(BlockParamSpec::floatParam("attackUs", "Attack", "us", 20.0f, 800.0f, 100.0f)
                      .withSkew(0.5f));
      t.push_back(BlockParamSpec::floatParam("releaseMs", "Release", "ms", 50.0f, 1100.0f, 250.0f)
                      .withSkew(0.5f));
      t.push_back(BlockParamSpec::floatParam("outputDb", "Output", "dB", -36.0f, 12.0f, -9.0f));
      t.push_back(BlockParamSpec::boolParam("colour", "Colour", true));
      return t;
    }();
    return table;
  }

  static const BlockReadingTable& readingSpecs() {
    static const BlockReadingTable table = {
        {"gainReductionDb", "Gain reduction", "dB", 0.0f, 60.0f}};
    return table;
  }

  FetCompressorProcessor()
      : ParameterisedBlockProcessor(kKindId, parameterSpecs(), readingSpecs()) {}

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    inputGain.prepare(sampleRate, 0.02);
    outputGain.prepare(sampleRate, 0.02);
    colourAmount.prepare(sampleRate, 0.02);
    averageStep = stepFor(0.6, sampleRate);
    for (Lane& lane : lanes)
      lane.dcBlock.setCutoff(8.0, sampleRate);
    resetEffect();
  }

  void resetEffect() noexcept override {
    for (Lane& lane : lanes) {
      lane.detector.reset();
      lane.reduction.reset();
      lane.dcBlock.reset();
    }
    inputGain.snapOnNextTarget();
    outputGain.snapOnNextTarget();
    colourAmount.snapOnNextTarget();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const Lanes layout = lanesFor(buffer.getNumChannels(), ctx.dualMono);

    static constexpr float kRatios[] = {4.0f, 8.0f, 12.0f, 20.0f, 20.0f};
    const int ratioIndex = juce::jlimit(0, 4, static_cast<int>(param(kRatio) + 0.5f));
    const bool allButtons = ratioIndex == 4;
    const float slope = 1.0f - 1.0f / kRatios[ratioIndex];
    // A short, fixed knee: the character of the curve is its corner.
    const float knee = allButtons ? 0.0f : kKneeDb;
    const float kneeFloorLevel = fx::dbToGain(kThresholdDb - 0.5f * knee);

    const float attackStep = stepFor(1.0e-6 * static_cast<double>(param(kAttackUs)), sampleRate);
    // The set release is for the middle of the programme-dependent range.
    const float releaseSeconds = 0.001f * param(kReleaseMs) * (allButtons ? 0.6f : 1.0f);
    const float detectorKeep =
        static_cast<float>(std::exp(-1.0 / (kDetectorFallSeconds * sampleRate)));

    inputGain.setTarget(fx::dbToGain(param(kInputDb)));
    outputGain.setTarget(fx::dbToGain(param(kOutputDb)));
    colourAmount.setTarget(param(kColour) >= 0.5f ? (allButtons ? 2.0f : 1.0f) : 0.0f);

    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     layout.channels > 1 ? buffer.getWritePointer(1) : nullptr};
    float mostReduction = 0.0f;
    for (int i = 0; i < numSamples; ++i) {
      const float in = inputGain.next();
      const float out = outputGain.next();
      const float colour = colourAmount.next();
      for (int lane = 0; lane < layout.count; ++lane) {
        Lane& s = lanes[lane];
        float x0 = channels[lane][i] * in;
        float x1 = layout.linked ? channels[1][i] * in : 0.0f;

        float detected = std::abs(x0);
        if (layout.linked)
          detected = juce::jmax(detected, std::abs(x1));
        const float level = s.detector.process(detected, detectorKeep);

        float target = 0.0f;
        if (level > kneeFloorLevel)
          target = kneeReduction(levelToDb(level) - kThresholdDb, knee, slope);

        // Programme-dependent release: slower after sustained reduction.
        const float seconds = releaseSeconds * (0.7f + 0.8f * juce::jlimit(0.0f, 1.0f, s.reduction.average / 12.0f));
        const float reduction =
            s.reduction.next(target, attackStep, stepForSlow(seconds, sampleRate), averageStep);
        mostReduction = juce::jmax(mostReduction, reduction);

        const float gain = dbToGainFast(-reduction);
        x0 *= gain;
        if (colour > 0.0f)
          x0 = addColour(s, x0, colour);
        channels[lane][i] = x0 * out;
        if (layout.linked) {
          x1 *= gain;
          if (colour > 0.0f)
            x1 = addColour(lanes[1], x1, colour);
          channels[1][i] = x1 * out;
        }
      }
    }
    setReading(kGainReduction, mostReduction);
  }

private:
  static constexpr float kThresholdDb = -10.0f;
  static constexpr float kKneeDb = 3.0f;
  static constexpr double kDetectorFallSeconds = 0.015;

  struct Lane {
    PeakDetector detector;
    Reduction reduction;
    fx::OnePole dcBlock;
  };

  // A little third harmonic (the transistor leaning into its limit) and a
  // little second (the asymmetry), against the level: a few tenths of a
  // percent at -20 dBFS, a few percent near full scale. The square leaves a
  // DC term, which the high-pass takes out of the added part only.
  static float addColour(Lane& lane, float x, float amount) noexcept {
    const float c = juce::jlimit(-2.0f, 2.0f, x);
    const float even = 0.04f * c * c;
    const float odd = -0.15f * c * c * c;
    return x + amount * (lane.dcBlock.highpass(even) + odd);
  }

  Lane lanes[kMaxChannels];
  fx::SmoothedParam inputGain, outputGain, colourAmount;
  float averageStep{0.0f};
  double sampleRate{48000.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(FetCompressor) {
  addEffectKind<FetCompressorProcessor>(registry, "FET Compressor", BlockCategory::Dynamics);
}

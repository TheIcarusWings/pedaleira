// dynamics.bus: a glue compressor for the whole mix, in the style of a
// console bus compressor: stepped ratio, attack and release, a smooth knee
// and a detector shared by both channels.
//
// - Ratio 2, 4 or 10. Attack 0.1, 0.3, 1, 3, 10 or 30 ms. Release 0.1, 0.3,
//   0.6 or 1.2 s, or Auto, which runs from 0.1 s after a short peak up to
//   1.2 s after sustained reduction.
// - The knee is the one a feedback compressor has. There the detector listens
//   to the output, so the output sits (level - T) / ratio above the threshold
//   and the bend is wider and rounder than the same knee in a feed-forward
//   design. The reduction here is worked out from the input but follows that
//   curve exactly (solved in closed form, see feedbackReduction), without the
//   stability limit a real loop puts on fast attacks at high ratios.
// - A stereo pair is detected as one (the louder channel) and gets one gain;
//   dual mono is two guitars, each on its own.
//
// The gain reduction is reported as a reading (gainReductionDb).
#include <cmath>
#include "effects/EffectDsp.h"
#include "effects/dynamics/DynamicsCommon.h"

namespace {

using namespace dynfx;

class BusCompressorProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "dynamics.bus";

  enum Param { kThresholdDb = 0, kRatio, kAttack, kRelease, kMakeupDb };
  enum Reading { kGainReduction = 0 };

  static constexpr int kAutoRelease = 4;

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::floatParam("thresholdDb", "Threshold", "dB", -48.0f, 0.0f, -20.0f));
      t.push_back(BlockParamSpec::choiceParam("ratio", "Ratio", {"2:1", "4:1", "10:1"}, 1));
      t.push_back(BlockParamSpec::choiceParam(
          "attack", "Attack", {"0.1 ms", "0.3 ms", "1 ms", "3 ms", "10 ms", "30 ms"}, 3));
      t.push_back(BlockParamSpec::choiceParam(
          "release", "Release", {"0.1 s", "0.3 s", "0.6 s", "1.2 s", "Auto"}, 1));
      t.push_back(BlockParamSpec::floatParam("makeupDb", "Makeup", "dB", 0.0f, 24.0f, 0.0f));
      return t;
    }();
    return table;
  }

  static const BlockReadingTable& readingSpecs() {
    static const BlockReadingTable table = {
        {"gainReductionDb", "Gain reduction", "dB", 0.0f, 60.0f}};
    return table;
  }

  BusCompressorProcessor()
      : ParameterisedBlockProcessor(kKindId, parameterSpecs(), readingSpecs()) {}

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    makeup.prepare(sampleRate, 0.02);
    averageStep = stepFor(0.5, sampleRate);
    detectorKeep = static_cast<float>(std::exp(-1.0 / (kDetectorFallSeconds * sampleRate)));
    resetEffect();
  }

  void resetEffect() noexcept override {
    for (Lane& lane : lanes) {
      lane.detector.reset();
      lane.reduction.reset();
    }
    makeup.snapOnNextTarget();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const Lanes layout = lanesFor(buffer.getNumChannels(), ctx.dualMono);

    static constexpr float kRatios[] = {2.0f, 4.0f, 10.0f};
    static constexpr float kAttackMs[] = {0.1f, 0.3f, 1.0f, 3.0f, 10.0f, 30.0f};
    static constexpr float kReleaseSeconds[] = {0.1f, 0.3f, 0.6f, 1.2f};
    const float threshold = param(kThresholdDb);
    const float ratio = kRatios[juce::jlimit(0, 2, static_cast<int>(param(kRatio) + 0.5f))];
    const float attackStep = stepFor(
        0.001 * static_cast<double>(kAttackMs[juce::jlimit(0, 5, static_cast<int>(param(kAttack) + 0.5f))]),
        sampleRate);
    const int releaseIndex = juce::jlimit(0, 4, static_cast<int>(param(kRelease) + 0.5f));
    const bool autoRelease = releaseIndex == kAutoRelease;
    const float releaseSeconds = autoRelease ? 0.0f : kReleaseSeconds[releaseIndex];
    // Where the curve starts, so a quiet signal skips the logarithm.
    const float floorLevel = fx::dbToGain(threshold - 0.5f * kKneeDb);
    makeup.setTarget(fx::dbToGain(param(kMakeupDb)));

    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     layout.channels > 1 ? buffer.getWritePointer(1) : nullptr};
    float mostReduction = 0.0f;
    for (int i = 0; i < numSamples; ++i) {
      const float makeupGain = makeup.next();
      for (int lane = 0; lane < layout.count; ++lane) {
        Lane& s = lanes[lane];
        float detected = std::abs(channels[lane][i]);
        if (layout.linked)
          detected = juce::jmax(detected, std::abs(channels[1][i]));
        const float level = s.detector.process(detected, detectorKeep);

        float target = 0.0f;
        if (level > floorLevel)
          target = feedbackReduction(levelToDb(level) - threshold, ratio);

        const float seconds =
            autoRelease ? 0.1f + 1.1f * juce::jlimit(0.0f, 1.0f, s.reduction.average / 8.0f)
                        : releaseSeconds;
        const float reduction =
            s.reduction.next(target, attackStep, stepForSlow(seconds, sampleRate), averageStep);
        mostReduction = juce::jmax(mostReduction, reduction);

        const float gain = dbToGainFast(-reduction) * makeupGain;
        channels[lane][i] *= gain;
        if (layout.linked)
          channels[1][i] *= gain;
      }
    }
    setReading(kGainReduction, mostReduction);
  }

private:
  static constexpr float kKneeDb = 4.0f;
  static constexpr double kDetectorFallSeconds = 0.008;

  // The static curve of a feedback compressor with a quadratic knee of width
  // W on its output side: reduction k * f(d), with k = ratio - 1 and d the
  // output's distance from the threshold. Solving output = input - reduction
  // for u = input - threshold gives
  //   u <= -W/2          0
  //   -W/2 < u < R W/2   k e^2 / (2 W), e = W (sqrt(1 + 2k (W/2 + u) / W) - 1) / k
  //   u >= R W/2         k u / R          (a plain ratio of R)
  static float feedbackReduction(float u, float ratio) noexcept {
    const float k = ratio - 1.0f;
    const float w = kKneeDb;
    if (u <= -0.5f * w)
      return 0.0f;
    if (u >= 0.5f * ratio * w)
      return k * u / ratio;
    const float e = w * (std::sqrt(1.0f + 2.0f * k * (0.5f * w + u) / w) - 1.0f) / k;
    return k * e * e / (2.0f * w);
  }

  struct Lane {
    PeakDetector detector;
    Reduction reduction;
  };

  Lane lanes[kMaxChannels];
  fx::SmoothedParam makeup;
  float averageStep{0.0f};
  float detectorKeep{0.0f};
  double sampleRate{48000.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(BusCompressor) {
  addEffectKind<BusCompressorProcessor>(registry, "Bus Compressor", BlockCategory::Dynamics);
}

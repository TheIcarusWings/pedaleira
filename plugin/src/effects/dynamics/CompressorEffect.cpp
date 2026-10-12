// dynamics.compressor: a feed-forward compressor with a peak or RMS
// detector, a quadratic soft knee and attack/release ballistics on the gain
// reduction.
//
// Signal flow per sample: detector (linear level) -> level in dB -> static
// curve (threshold, ratio, knee) -> gain reduction in dB -> attack/release
// smoothing of the reduction -> gain. Smoothing the reduction rather than the
// level keeps the static curve exact for a steady tone whatever the attack
// is, and below the threshold the gain is exactly 1 (plus makeup).
//
// The gain reduction is reported as a reading (gainReductionDb), for a meter.
#include <cmath>
#include "effects/EffectDsp.h"

namespace {

class CompressorProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "dynamics.compressor";

  enum Param { kThresholdDb = 0, kRatio, kAttackMs, kReleaseMs, kKneeDb, kMakeupDb, kDetector };
  enum Reading { kGainReduction = 0 };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::floatParam("thresholdDb", "Threshold", "dB", -60.0f, 0.0f, -18.0f));
      t.push_back(BlockParamSpec::floatParam("ratio", "Ratio", "ratio", 1.0f, 20.0f, 4.0f)
                      .withSkew(0.5f));
      t.push_back(BlockParamSpec::floatParam("attackMs", "Attack", "ms", 0.1f, 200.0f, 10.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("releaseMs", "Release", "ms", 5.0f, 2000.0f, 150.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("kneeDb", "Knee", "dB", 0.0f, 24.0f, 6.0f));
      t.push_back(BlockParamSpec::floatParam("makeupDb", "Makeup", "dB", 0.0f, 24.0f, 0.0f));
      t.push_back(BlockParamSpec::choiceParam("detector", "Detector", {"Peak", "RMS"}, 0));
      return t;
    }();
    return table;
  }

  // How far the gain is pulled down: the most within each processed block.
  static const BlockReadingTable& readingSpecs() {
    static const BlockReadingTable table = {
        {"gainReductionDb", "Gain reduction", "dB", 0.0f, 60.0f}};
    return table;
  }

  CompressorProcessor()
      : ParameterisedBlockProcessor(kKindId, parameterSpecs(), readingSpecs()) {}

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    makeup.prepare(sampleRate, 0.02);
    rmsStep = static_cast<float>(-std::expm1(-1.0 / (kRmsWindowSeconds * sampleRate)));
    resetEffect();
  }

  void resetEffect() noexcept override {
    for (auto& lane : lanes)
      lane = Lane{};
    makeup.snapOnNextTarget();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxChannels);
    // One stereo signal shares one gain reduction (the image holds); dual
    // mono is two guitars, each compressed on its own.
    const bool linked = numChannels == 2 && !ctx.dualMono;
    const int numLanes = linked ? 1 : numChannels;

    const float threshold = param(kThresholdDb);
    const float knee = param(kKneeDb);
    // Slope of the output above the knee, as the change in gain per dB
    // (ratio 4: -0.75). Zero at ratio 1: no reduction anywhere.
    const float slope = 1.0f / param(kRatio) - 1.0f;
    // Below this level the curve is flat; skipping the log there is what
    // keeps a quiet signal cheap.
    const float kneeFloorLevel = fx::dbToGain(threshold - 0.5f * knee);
    const float attackKeep = static_cast<float>(std::exp(-1.0 / (0.001 * param(kAttackMs) * sampleRate)));
    const float releaseKeep = static_cast<float>(std::exp(-1.0 / (0.001 * param(kReleaseMs) * sampleRate)));
    // The peak detector falls back at a fraction of the release time, within
    // 2 to 30 ms: slow enough to ride over the half cycles of a low note
    // (which would otherwise be read as the level dipping twice a cycle),
    // fast enough never to dominate a short release.
    const float peakKeep = static_cast<float>(std::exp(
        -1.0 / (0.001 * juce::jlimit(2.0, 30.0, 0.5 * static_cast<double>(param(kReleaseMs))) * sampleRate)));
    const bool rms = param(kDetector) >= 0.5f;
    makeup.setTarget(param(kMakeupDb));

    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    float mostReduction = 0.0f;
    for (int i = 0; i < numSamples; ++i) {
      const float makeupDb = makeup.next();
      for (int lane = 0; lane < numLanes; ++lane) {
        Lane& s = lanes[lane];
        float input = std::abs(channels[lane][i]);
        if (linked)
          input = juce::jmax(input, std::abs(channels[1][i]));

        float level;
        if (rms) {
          // Mean square over about 20 ms; the root of it is scaled by sqrt(2)
          // so a sine reads at its peak level, as the peak detector reads
          // it, and one threshold means the same for both.
          s.meanSquare += rmsStep * (input * input - s.meanSquare);
          level = 1.41421356f * std::sqrt(s.meanSquare);
        } else {
          s.peak = input > s.peak ? input : input + peakKeep * (s.peak - input);
          level = s.peak;
        }

        // The static curve: how many dB the level is pulled down.
        float target = 0.0f;
        if (level > kneeFloorLevel) {
          const float over = 8.68588964f * std::log(level) - threshold;
          if (knee > 0.0f && 2.0f * over > -knee && 2.0f * over < knee) {
            const float t = over + 0.5f * knee;
            target = -slope * t * t / (2.0f * knee);
          } else if (over > 0.0f) {
            target = -slope * over;
          }
        }

        // Ballistics on the reduction: fast when it grows, slow when it
        // relaxes. Both are one-poles, so the times are time constants
        // (about 63% of a step).
        const float keep = target > s.reduction ? attackKeep : releaseKeep;
        s.reduction = target + keep * (s.reduction - target);
        if (s.reduction < 1.0e-6f && target == 0.0f)
          s.reduction = 0.0f;
        mostReduction = juce::jmax(mostReduction, s.reduction);

        const float gainDb = makeupDb - s.reduction;
        const float gain = gainDb == 0.0f ? 1.0f : std::exp(0.115129255f * gainDb);
        channels[lane][i] *= gain;
        if (linked)
          channels[1][i] *= gain;
      }
    }
    setReading(kGainReduction, mostReduction);
  }

private:
  static constexpr int kMaxChannels = 2;
  static constexpr double kRmsWindowSeconds = 0.02;

  struct Lane {
    float peak{0.0f};
    float meanSquare{0.0f};
    float reduction{0.0f};  // dB, >= 0
  };

  Lane lanes[kMaxChannels];
  fx::SmoothedParam makeup;
  float rmsStep{0.001f};
  double sampleRate{48000.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(CompressorEffect) {
  addEffectKind<CompressorProcessor>(registry, "Compressor", BlockCategory::Dynamics);
}

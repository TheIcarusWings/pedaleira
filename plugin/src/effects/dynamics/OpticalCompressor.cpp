// dynamics.opto: a levelling compressor built around a light-sensitive cell,
// as in the classic tube-era leveller. One knob (Peak Reduction) sets how
// hard the signal drives the cell, Gain brings the level back, a switch picks
// compress (about 3:1, a wide soft knee) or limit (about 12:1, narrower).
//
// The cell is modelled as two parts that share the gain reduction:
//   fast part (60%)  attack 4 to 40 ms, quicker the harder it is driven;
//                    release about 60 ms
//   slow part (40%)  attack about 300 ms; release 1.2 to 3.2 s, longer the
//                    more it has been charged
// so a loud note is caught within tens of milliseconds, keeps tightening for a
// second, and on release first lets go of about half of the reduction quickly
// and then recovers over seconds, which is what an optical cell does.
//
// The gain reduction is reported as a reading (gainReductionDb).
#include <cmath>
#include "effects/EffectDsp.h"
#include "effects/dynamics/DynamicsCommon.h"

namespace {

using namespace dynfx;

class OpticalCompressorProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "dynamics.opto";

  enum Param { kPeakReduction = 0, kGainDb, kMode };
  enum Reading { kGainReduction = 0 };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::floatParam("peakReduction", "Peak Reduction", "%", 0.0f, 100.0f, 50.0f));
      t.push_back(BlockParamSpec::floatParam("gainDb", "Gain", "dB", -12.0f, 24.0f, 5.0f));
      t.push_back(BlockParamSpec::choiceParam("mode", "Mode", {"Compress", "Limit"}, 0));
      return t;
    }();
    return table;
  }

  static const BlockReadingTable& readingSpecs() {
    static const BlockReadingTable table = {
        {"gainReductionDb", "Gain reduction", "dB", 0.0f, 60.0f}};
    return table;
  }

  OpticalCompressorProcessor()
      : ParameterisedBlockProcessor(kKindId, parameterSpecs(), readingSpecs()) {}

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    gain.prepare(sampleRate, 0.02);
    detectorKeep = static_cast<float>(std::exp(-1.0 / (kDetectorFallSeconds * sampleRate)));
    fastReleaseStep = stepFor(0.06, sampleRate);
    slowAttackStep = stepFor(0.3, sampleRate);
    resetEffect();
  }

  void resetEffect() noexcept override {
    for (Lane& lane : lanes)
      lane = Lane{};
    gain.snapOnNextTarget();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const Lanes layout = lanesFor(buffer.getNumChannels(), ctx.dualMono);

    const bool limit = param(kMode) >= 0.5f;
    const float slope = 1.0f - 1.0f / (limit ? 12.0f : 3.0f);
    const float knee = limit ? 6.0f : 12.0f;
    const float threshold = -2.0f - 0.5f * param(kPeakReduction);
    const float floorLevel = fx::dbToGain(threshold - 0.5f * knee);
    gain.setTarget(param(kGainDb));
    const float attackScale = limit ? 1.5f : 1.0f;

    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     layout.channels > 1 ? buffer.getWritePointer(1) : nullptr};
    float mostReduction = 0.0f;
    for (int i = 0; i < numSamples; ++i) {
      const float gainDb = gain.next();
      for (int lane = 0; lane < layout.count; ++lane) {
        Lane& s = lanes[lane];
        float detected = std::abs(channels[lane][i]);
        if (layout.linked)
          detected = juce::jmax(detected, std::abs(channels[1][i]));
        const float level = s.detector.process(detected, detectorKeep);

        float target = 0.0f;
        if (level > floorLevel)
          target = kneeReduction(levelToDb(level) - threshold, knee, slope);

        // Each part chases its share of the target.
        const float fastTarget = kFastShare * target;
        const float slowTarget = target - fastTarget;
        // Program-dependent attack: the harder the drive, the quicker.
        const float fastAttack = juce::jmin(0.2f, attackScale * (1.0f + target / 6.0f) *
                                                      stepForSlow(0.04f, sampleRate));
        const float slowRelease =
            stepForSlow(1.2f + 2.0f * juce::jlimit(0.0f, 1.0f, s.slow / 8.0f), sampleRate);
        s.fast += (fastTarget > s.fast ? fastAttack : fastReleaseStep) * (fastTarget - s.fast);
        s.slow += (slowTarget > s.slow ? slowAttackStep : slowRelease) * (slowTarget - s.slow);
        if (target == 0.0f) {
          if (s.fast < 1.0e-6f)
            s.fast = 0.0f;
          if (s.slow < 1.0e-6f)
            s.slow = 0.0f;
        }
        const float reduction = s.fast + s.slow;
        mostReduction = juce::jmax(mostReduction, reduction);

        const float g = dbToGainFast(gainDb - reduction);
        channels[lane][i] *= g;
        if (layout.linked)
          channels[1][i] *= g;
      }
    }
    setReading(kGainReduction, mostReduction);
  }

private:
  static constexpr float kFastShare = 0.6f;
  static constexpr double kDetectorFallSeconds = 0.01;

  struct Lane {
    PeakDetector detector;
    float fast{0.0f};
    float slow{0.0f};
  };

  Lane lanes[kMaxChannels];
  fx::SmoothedParam gain;
  float detectorKeep{0.0f};
  float fastReleaseStep{0.0f};
  float slowAttackStep{0.0f};
  double sampleRate{48000.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(OpticalCompressor) {
  addEffectKind<OpticalCompressorProcessor>(registry, "Optical Compressor", BlockCategory::Dynamics);
}

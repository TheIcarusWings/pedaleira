// dynamics.sustainer: the sustain pedal. A heavy compressor with a threshold
// far down and automatic makeup, so a note keeps nearly the same level as it
// dies away.
//
// - Sustain (0 to 100%) lowers the threshold from -10 to -55 dBFS and raises
//   the ratio from 6:1 to 20:1. The makeup is worked out from the curve so a
//   -18 dBFS signal comes out at -18 dBFS at any setting; Level trims that.
// - Attack (0 to 100%) is how much of the pick attack passes: 0 catches it in
//   0.5 ms, 100 lets it through for 40 ms before the gain comes down.
// - Release follows the playing: 120 ms after a short note up to 620 ms after
//   long, deep reduction, so a chord decays smoothly and fast runs are not
//   pumped.
// - Tone tilts the highs of the output by +-9 dB around 2 kHz.
// - The knee is 12 dB, soft, so the pedal fades into compression.
//
// The gain reduction is reported as a reading (gainReductionDb). Like the
// pedal, it also raises the noise floor as Sustain goes up.
#include <cmath>
#include "effects/EffectDsp.h"
#include "effects/dynamics/DynamicsCommon.h"

namespace {

using namespace dynfx;

class SustainerProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "dynamics.sustainer";

  enum Param { kSustain = 0, kAttack, kTone, kLevelDb };
  enum Reading { kGainReduction = 0 };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::floatParam("sustain", "Sustain", "%", 0.0f, 100.0f, 50.0f));
      t.push_back(BlockParamSpec::floatParam("attack", "Attack", "%", 0.0f, 100.0f, 50.0f));
      t.push_back(BlockParamSpec::floatParam("tone", "Tone", "%", 0.0f, 100.0f, 50.0f));
      t.push_back(BlockParamSpec::floatParam("levelDb", "Level", "dB", -18.0f, 12.0f, 0.0f));
      return t;
    }();
    return table;
  }

  static const BlockReadingTable& readingSpecs() {
    static const BlockReadingTable table = {
        {"gainReductionDb", "Gain reduction", "dB", 0.0f, 60.0f}};
    return table;
  }

  SustainerProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs(), readingSpecs()) {}

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    makeup.prepare(sampleRate, 0.05);
    toneDb.prepare(sampleRate, 0.05);
    averageStep = stepFor(0.5, sampleRate);
    detectorKeep = static_cast<float>(std::exp(-1.0 / (kDetectorFallSeconds * sampleRate)));
    resetEffect();
  }

  void resetEffect() noexcept override {
    for (Lane& lane : lanes) {
      lane.detector.reset();
      lane.reduction.reset();
      lane.tone.reset();
    }
    makeup.snapOnNextTarget();
    toneDb.snapOnNextTarget();
    appliedToneDb = 1000.0f;  // forces the first coefficient set
    gridCount = 0;
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const Lanes layout = lanesFor(buffer.getNumChannels(), ctx.dualMono);

    const float sustain = 0.01f * param(kSustain);
    const float threshold = -10.0f - 45.0f * sustain;
    const float ratio = 6.0f + 14.0f * sustain;
    const float slope = 1.0f - 1.0f / ratio;
    const float floorLevel = fx::dbToGain(threshold - 0.5f * kKneeDb);
    // The makeup that puts a -18 dBFS signal back at -18 dBFS: what the
    // curve takes off there, capped so the top of the range stays usable.
    const float autoMakeupDb = juce::jmin(kMaxMakeupDb, slope * juce::jmax(0.0f, -threshold - 18.0f));
    makeup.setTarget(autoMakeupDb + param(kLevelDb));
    toneDb.setTarget(0.18f * (param(kTone) - 50.0f));

    const float attackMs = 0.5f * std::pow(80.0f, 0.01f * param(kAttack));
    const float attackStep = stepFor(0.001 * static_cast<double>(attackMs), sampleRate);

    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     layout.channels > 1 ? buffer.getWritePointer(1) : nullptr};
    float mostReduction = 0.0f;
    for (int i = 0; i < numSamples; ++i) {
      const float makeupDb = makeup.next();
      const float tilt = toneDb.next();
      // The tone filter is retuned on a grid counted across blocks, and only
      // when the knob has moved.
      if (--gridCount <= 0) {
        gridCount = kControlGrid;
        if (tilt != appliedToneDb) {
          appliedToneDb = tilt;
          const fx::BiquadCoeffs c = fx::BiquadCoeffs::highShelf(2000.0, 0.7071, tilt, sampleRate);
          for (Lane& lane : lanes)
            lane.tone.setCoeffs(c);
        }
      }
      for (int lane = 0; lane < layout.count; ++lane) {
        Lane& s = lanes[lane];
        float detected = std::abs(channels[lane][i]);
        if (layout.linked)
          detected = juce::jmax(detected, std::abs(channels[1][i]));
        const float level = s.detector.process(detected, detectorKeep);

        float target = 0.0f;
        if (level > floorLevel)
          target = kneeReduction(levelToDb(level) - threshold, kKneeDb, slope);

        const float seconds = 0.12f + 0.5f * juce::jlimit(0.0f, 1.0f, s.reduction.average / 15.0f);
        const float reduction =
            s.reduction.next(target, attackStep, stepForSlow(seconds, sampleRate), averageStep);
        mostReduction = juce::jmax(mostReduction, reduction);

        const float gain = dbToGainFast(makeupDb - reduction);
        channels[lane][i] = s.tone.process(channels[lane][i] * gain);
        if (layout.linked)
          channels[1][i] = lanes[1].tone.process(channels[1][i] * gain);
      }
    }
    setReading(kGainReduction, mostReduction);
  }

private:
  static constexpr float kKneeDb = 12.0f;
  static constexpr float kMaxMakeupDb = 36.0f;
  static constexpr int kControlGrid = 16;
  static constexpr double kDetectorFallSeconds = 0.008;

  struct Lane {
    PeakDetector detector;
    Reduction reduction;
    fx::Biquad tone;
  };

  Lane lanes[kMaxChannels];
  fx::SmoothedParam makeup, toneDb;
  float averageStep{0.0f};
  float detectorKeep{0.0f};
  float appliedToneDb{1000.0f};
  int gridCount{0};
  double sampleRate{48000.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(Sustainer) {
  addEffectKind<SustainerProcessor>(registry, "Sustainer", BlockCategory::Dynamics);
}

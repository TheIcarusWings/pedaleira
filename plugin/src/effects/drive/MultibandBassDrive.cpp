// drive.multibandbass: a bass drive in three bands, each with its own drive
// and level. Two Linkwitz-Riley crossovers split the input into low, mid and
// high bands; the low band also goes through the all-pass of the upper
// crossover, so the three bands add up to a flat magnitude (an all-pass in
// phase) whenever every band is clean:
//
//   L = AP(lowpass4(x, f1), f2)
//   M = lowpass4(highpass4(x, f1), f2)
//   H = highpass4(highpass4(x, f1), f2)
//   L + M + H = AP(AP(x, f1), f2)
//
// Each band then has its own character:
//
//   low   a gentle, symmetric soft saturation with a low gain range: it
//         rounds the peaks of the lowest notes and keeps their weight
//   mid   the main grind: a high gain, lopsided clip (even harmonics), its
//         DC removed in the band
//   high  presence and fizz: a hard clip whose fizz is softened by a one
//         pole low-pass, so the top can be driven without turning harsh
//
// Drive at 0 leaves a band untouched; the first tenth of the knob fades the
// shaper in at its lowest gain, so the band is either exactly clean or
// driven, never a step between the two. Blend mixes the sum of the clean
// bands (the same all-pass, so no notch at the crossovers) with the sum of
// the driven ones; Level comes last.
//
//   in -> [4x: split -> drive each band -> band levels -> sum, blend] -> level
#include "effects/drive/DriveCommon.h"

namespace {

using namespace drivefx;

class MultibandBassDriveProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "drive.multibandbass";

  enum Param {
    kLowDrivePercent = 0,
    kLowLevelDb,
    kMidDrivePercent,
    kMidLevelDb,
    kHighDrivePercent,
    kHighLevelDb,
    kLowCrossoverHz,
    kHighCrossoverHz,
    kBlendPercent,
    kLevelDb
  };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::floatParam("lowDrivePercent", "Low Drive", "%", 0.0f, 100.0f, 30.0f));
      t.push_back(BlockParamSpec::floatParam("lowLevelDb", "Low Level", "dB", -24.0f, 12.0f, 0.0f));
      t.push_back(BlockParamSpec::floatParam("midDrivePercent", "Mid Drive", "%", 0.0f, 100.0f, 50.0f));
      t.push_back(BlockParamSpec::floatParam("midLevelDb", "Mid Level", "dB", -24.0f, 12.0f, 0.0f));
      t.push_back(BlockParamSpec::floatParam("highDrivePercent", "High Drive", "%", 0.0f, 100.0f, 30.0f));
      t.push_back(BlockParamSpec::floatParam("highLevelDb", "High Level", "dB", -24.0f, 12.0f, 0.0f));
      t.push_back(BlockParamSpec::floatParam("lowCrossoverHz", "Low Crossover", "Hz", 50.0f, 500.0f, 150.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("highCrossoverHz", "High Crossover", "Hz", 500.0f, 5000.0f, 1500.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("blendPercent", "Blend", "%", 0.0f, 100.0f, 100.0f));
      t.push_back(BlockParamSpec::floatParam("levelDb", "Level", "dB", -48.0f, 18.0f, 0.0f));
      return t;
    }();
    return table;
  }

  MultibandBassDriveProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The half-band filters of the 4x oversampler delay every path by a few
  // samples (drive/DriveCommon.h).
  int latencySamples() const noexcept override { return kLatencySamples; }

  // The lowest crossover (50 Hz, fourth order) is the longest memory.
  double tailSeconds() const noexcept override { return 0.15; }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    oversampler.prepare(spec.maxBlockSize);
    const double osRate = sampleRate * kOversampleFactor;
    lowCrossover.prepare(osRate, 0.05);
    highCrossover.prepare(osRate, 0.05);
    for (Band& band : bands) {
      band.drive.prepare(osRate, 0.02);
      band.level.prepare(osRate, 0.02);
    }
    blend.prepare(osRate, 0.02);
    level.prepare(sampleRate, 0.02);
    for (Lane& lane : lanes) {
      lane.midDc.setCutoff(kMidDcHz, osRate);
      lane.fizz.setCutoff(kFizzHz, osRate);
    }
    midOffset = hardSat(kMidBias);
    resetEffect();
  }

  void resetEffect() noexcept override {
    oversampler.reset();
    for (Lane& lane : lanes) {
      lane.lowA.reset();
      lane.lowB.reset();
      lane.restA.reset();
      lane.restB.reset();
      lane.midA.reset();
      lane.midB.reset();
      lane.highA.reset();
      lane.highB.reset();
      lane.lowAllpass.reset();
      lane.midDc.reset();
      lane.fizz.reset();
    }
    tick.reset();
    lowCrossover.snapOnNextTarget();
    highCrossover.snapOnNextTarget();
    for (Band& band : bands) {
      band.drive.snapOnNextTarget();
      band.level.snapOnNextTarget();
    }
    blend.snapOnNextTarget();
    level.snapOnNextTarget();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext&) override {
    const int numSamples = buffer.getNumSamples();
    const int numLanes = juce::jmin(buffer.getNumChannels(), kMaxLanes);
    lowCrossover.setTarget(param(kLowCrossoverHz));
    highCrossover.setTarget(param(kHighCrossoverHz));
    for (int b = 0; b < kNumBands; ++b) {
      bands[b].drive.setTarget(param(kLowDrivePercent + 2 * b) * 0.01f);
      bands[b].level.setTarget(fx::dbToGain(param(kLowLevelDb + 2 * b)));
    }
    blend.setTarget(param(kBlendPercent) * 0.01f);
    level.setTarget(fx::dbToGain(param(kLevelDb)));

    const double osRate = sampleRate * kOversampleFactor;
    oversampler.run(buffer, [&](float* const* io, int frames, int numReal) {
      for (int i = 0; i < frames; ++i) {
        const float f1 = lowCrossover.next();
        const float f2 = highCrossover.next();
        if (tick.step())
          for (Lane& lane : lanes) {
            lane.lowA.set(f1, kButterworthQ, osRate);
            lane.lowB.set(f1, kButterworthQ, osRate);
            lane.restA.set(f1, kButterworthQ, osRate);
            lane.restB.set(f1, kButterworthQ, osRate);
            lane.midA.set(f2, kButterworthQ, osRate);
            lane.midB.set(f2, kButterworthQ, osRate);
            lane.highA.set(f2, kButterworthQ, osRate);
            lane.highB.set(f2, kButterworthQ, osRate);
            lane.lowAllpass.set(f2, kButterworthQ, osRate);
          }

        // Per band: how far the shaper is faded in, its gain and the level.
        float fade[kNumBands], gain[kNumBands], bandLevel[kNumBands];
        for (int b = 0; b < kNumBands; ++b) {
          const float d = bands[b].drive.next();
          fade[b] = juce::jmin(1.0f, d * kFadeIn);
          gain[b] = 1.0f + kGainSpan[b] * d * d;
          bandLevel[b] = bands[b].level.next();
        }
        const float wet = blend.next();

        for (int ch = 0; ch < numReal; ++ch) {
          Lane& lane = lanes[ch];
          const float x = io[ch][i];
          const fx::StateVariableFilter::Outputs split = lane.lowAllpass.process(
              lane.lowB.lowpass(lane.lowA.lowpass(x)));
          const float low = split.low + split.high - kAllpassK * split.band;
          const float rest = lane.restB.highpass(lane.restA.highpass(x));
          const float mid = lane.midB.lowpass(lane.midA.lowpass(rest));
          const float high = lane.highB.highpass(lane.highA.highpass(rest));

          const float lowShaped = kLowClip * softSat(gain[0] * low * (1.0f / kLowClip));
          const float midShaped = lane.midDc.process(
              kMidClip * (hardSat(gain[1] * mid * (1.0f / kMidClip) + kMidBias) - midOffset));
          const float highShaped =
              lane.fizz.lowpass(kHighClip * hardSat(gain[2] * high * (1.0f / kHighClip)));

          const float driven = bandLevel[0] * (low + fade[0] * (lowShaped - low)) +
                               bandLevel[1] * (mid + fade[1] * (midShaped - mid)) +
                               bandLevel[2] * (high + fade[2] * (highShaped - high));
          const float clean = low + mid + high;
          io[ch][i] = clean + wet * (driven - clean);
        }
      }
    });

    for (int i = 0; i < numSamples; ++i) {
      const float g = level.next();
      for (int ch = 0; ch < numLanes; ++ch)
        buffer.getWritePointer(ch)[i] *= g;
    }
  }

private:
  static constexpr int kNumBands = 3;
  static constexpr double kButterworthQ = 0.7071067811865476;
  // The all-pass of a Butterworth state-variable stage: low + high - k band
  // with k = 1 / Q, which is what the two halves of the crossover add up to.
  static constexpr float kAllpassK = 1.4142135623730951f;
  // The first tenth of a Drive knob fades its shaper in.
  static constexpr float kFadeIn = 10.0f;
  // Gain into each shaper is 1 + span * drive^2 (low, mid, high).
  static constexpr float kGainSpan[kNumBands] = {5.0f, 79.0f, 29.0f};
  // Where each band's shaper tops out, in full-scale units.
  static constexpr float kLowClip = 0.35f;
  static constexpr float kMidClip = 0.095f;
  static constexpr float kHighClip = 0.08f;
  // The lopsidedness of the mid clip, in units of its level.
  static constexpr float kMidBias = 0.2f;
  static constexpr double kMidDcHz = 10.0;
  static constexpr double kFizzHz = 7000.0;

  struct Band {
    fx::SmoothedParam drive, level;
  };

  struct Lane {
    fx::StateVariableFilter lowA, lowB, restA, restB, midA, midB, highA, highB, lowAllpass;
    DcBlocker midDc;
    fx::OnePole fizz;
  };

  Lane lanes[kMaxLanes];
  Band bands[kNumBands];
  Oversampler4x oversampler;
  ControlTick tick{kControlGridOs};
  fx::SmoothedParam lowCrossover, highCrossover, blend, level;
  float midOffset{0.0f};
  double sampleRate{48000.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(MultibandBassDrive) {
  addEffectKind<MultibandBassDriveProcessor>(registry, "Multiband Bass Drive", BlockCategory::Drive);
}

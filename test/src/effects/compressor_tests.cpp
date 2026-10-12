// dynamics.compressor (plugin/src/effects/dynamics/CompressorEffect.cpp): the
// conformance test, then the compressor's own behaviour: the static curve
// (threshold, ratio, soft knee, makeup) at several input levels for both
// detectors, unity below the threshold, attack and release times, and the
// stereo link.
#include "effects/effect_test_helpers.h"

#include <gtest/gtest.h>

using namespace fxtest;

namespace {

constexpr const char* kKind = "dynamics.compressor";
constexpr double kRate = 48000.0;

float amp(double levelDb) { return static_cast<float>(std::pow(10.0, levelDb / 20.0)); }

// Steady-state gain in dB of a 1 kHz sine of peak level `levelDb` dBFS.
double staticGainDb(const Params& params, double levelDb) {
  EffectRig rig(kKind, params);
  return sineProbeGainDb(rig, 1000.0, amp(levelDb), 2.0, 1.5);
}

struct Segment {
  double seconds;
  double levelDb;
};

std::vector<float> steppedSine(double freq, const std::vector<Segment>& segments) {
  std::vector<float> x;
  for (const Segment& s : segments) {
    const int n = static_cast<int>(std::lround(s.seconds * kRate));
    const float a = amp(s.levelDb);
    for (int i = 0; i < n; ++i)
      x.push_back(a * static_cast<float>(std::sin(2.0 * kPi * freq * static_cast<double>(x.size()) / kRate)));
  }
  return x;
}

// Gain reduction in dB against time: the level of `in` over the level of
// `out` in 1 ms windows (4 kHz tone: whole periods).
std::vector<double> reductionDb(const std::vector<float>& in, const std::vector<float>& out) {
  const size_t window = 48;
  std::vector<double> reduction;
  for (size_t i = 0; i + window <= in.size(); i += window)
    reduction.push_back(toDb(rmsOf(in, i, i + window)) - toDb(rmsOf(out, i, i + window)));
  return reduction;
}

}  // namespace

TEST(CompressorEffectTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"thresholdDb", -30.0}, {"ratio", 6.0}, {"attackMs", 5.0}, {"makeupDb", 3.0}};
  options.stateParams = {{"thresholdDb", -24.0}, {"ratio", 8.0},    {"attackMs", 2.0},
                         {"releaseMs", 300.0},   {"kneeDb", 12.0},  {"makeupDb", 6.0},
                         {"detector", 1.0}};
  // Makeup of up to 24 dB on noise at 0.25.
  options.sweepBound = 6.0f;
  options.cpuAverageBudgetNs = 2000.0;
  options.cpuWorstBudgetNs = 150000.0;
  expectKindConforms("dynamics.compressor", options);
}

TEST(CompressorEffectTest, StaticCurveMatchesThresholdAndRatio) {
  // Threshold -30 dB, 4:1, hard knee: nothing below the threshold, and above
  // it the output rises 1 dB for every 4 dB in. At -10 dB the output is at
  // -30 + 20/4 = -25 dB, a gain of -15 dB.
  for (const double detector : {0.0, 1.0}) {
    SCOPED_TRACE(detector == 0.0 ? "peak" : "rms");
    const Params params = {{"thresholdDb", -30.0}, {"ratio", 4.0},  {"kneeDb", 0.0},
                           {"attackMs", 1.0},      {"releaseMs", 50.0}, {"detector", detector},
                           {"mix", 1.0}};
    EXPECT_NEAR(staticGainDb(params, -50.0), 0.0, 0.2);
    EXPECT_NEAR(staticGainDb(params, -30.0), 0.0, 0.3);
    EXPECT_NEAR(staticGainDb(params, -20.0), -7.5, 0.5);
    EXPECT_NEAR(staticGainDb(params, -10.0), -15.0, 0.5);
  }
}

TEST(CompressorEffectTest, RatioAndThresholdMoveTheCurve) {
  const Params base = {{"thresholdDb", -24.0}, {"ratio", 2.0}, {"kneeDb", 0.0}, {"attackMs", 1.0}, {"mix", 1.0}};
  // 2:1 at 12 dB over: 6 dB of reduction.
  EXPECT_NEAR(staticGainDb(base, -12.0), -6.0, 0.5);
  Params ratio20 = base;
  ratio20.emplace_back("ratio", 20.0);
  // 20:1 at 12 dB over: 12 * 0.95 = 11.4 dB.
  EXPECT_NEAR(staticGainDb(ratio20, -12.0), -11.4, 0.5);
  Params ratio1 = base;
  ratio1.emplace_back("ratio", 1.0);
  EXPECT_NEAR(staticGainDb(ratio1, -6.0), 0.0, 0.05) << "ratio 1 is no compression";
}

TEST(CompressorEffectTest, SoftKneeIsSmoothAndHalfwayThroughAtTheThreshold) {
  // 10 dB knee at -30: the curve bends between -35 and -25. At the threshold
  // the reduction is (1 - 1/R) * (W/2)^2 / (2W) = 0.75 * 25 / 20 = 0.94 dB.
  const Params params = {{"thresholdDb", -30.0}, {"ratio", 4.0}, {"kneeDb", 10.0},
                         {"attackMs", 1.0},      {"mix", 1.0}};
  EXPECT_NEAR(staticGainDb(params, -36.0), 0.0, 0.1) << "reduction before the knee";
  EXPECT_NEAR(staticGainDb(params, -30.0), -0.94, 0.25);
  // At the top edge of the knee the soft and the hard curve meet: 0.75 * 5.
  EXPECT_NEAR(staticGainDb(params, -25.0), -3.75, 0.4);
  EXPECT_NEAR(staticGainDb(params, -10.0), -15.0, 0.5);

  // Monotonic and continuous through the knee: each step of 1 dB in changes
  // the reduction by less than the full slope, never jumps.
  double previous = staticGainDb(params, -37.0);
  for (double level = -36.0; level <= -22.0; level += 1.0) {
    const double gain = staticGainDb(params, level);
    EXPECT_LE(gain, previous + 0.05) << level;
    EXPECT_GE(gain, previous - 0.8) << level;
    previous = gain;
  }
}

TEST(CompressorEffectTest, UnityGainBelowTheThresholdAndMakeupAddsGain) {
  // Alone, with nothing around it: below the threshold the compressor does
  // not touch the signal at all.
  {
    DirectKind kind(kKind, {{"thresholdDb", -20.0}, {"kneeDb", 6.0}});
    const std::vector<float> in = sine(1.0, 440.0, amp(-40.0), kRate);
    EXPECT_EQ(kind.render(in), in);
  }
  // Makeup is a plain gain on top.
  const Params params = {{"thresholdDb", -10.0}, {"makeupDb", 6.0}, {"mix", 1.0}};
  EXPECT_NEAR(staticGainDb(params, -40.0), 6.0, 0.1);
}

TEST(CompressorEffectTest, AttackAndReleaseTimesAreInTheRightBallpark) {
  // A quiet tone, a loud one 20 dB over the threshold, then quiet again. The
  // gain reduction moves with the attack time constant on the way in and the
  // release time constant on the way out (63% of the step).
  struct Case {
    double attack;
    double release;
  };
  for (const Case& c : {Case{20.0, 200.0}, Case{5.0, 80.0}}) {
    SCOPED_TRACE(c.attack);
    EffectRig rig(kKind, {{"thresholdDb", -30.0}, {"ratio", 10.0}, {"kneeDb", 0.0},
                          {"attackMs", c.attack}, {"releaseMs", c.release}, {"mix", 1.0}});
    EffectRig reference{juce::String()};
    const std::vector<float> in =
        steppedSine(4000.0, {{0.3, -60.0}, {1.2, -10.0}, {1.5, -60.0}});
    const std::vector<double> reduction = reductionDb(reference.render(in), rig.render(in));
    // 1 ms per entry. The loud part starts at entry 300 and ends at 1500.
    const auto at = [&](size_t ms) { return reduction[ms]; };
    const double finalReduction = at(1400);  // settled at the end of the loud part
    ASSERT_GT(finalReduction, 10.0);

    size_t attackReached = 0;
    for (size_t t = 300; t < 1500; ++t)
      if (at(t) >= 0.63 * finalReduction) {
        attackReached = t - 300;
        break;
      }
    EXPECT_GT(attackReached, 0.4 * c.attack);
    EXPECT_LT(attackReached, 2.0 * c.attack + 2.0);

    const double startOfRelease = at(1499);
    size_t releaseReached = 0;
    for (size_t t = 1500; t < reduction.size(); ++t)
      if (at(t) <= 0.37 * startOfRelease) {
        releaseReached = t - 1500;
        break;
      }
    EXPECT_GT(releaseReached, 0.4 * c.release);
    EXPECT_LT(releaseReached, 2.0 * c.release + 20.0);
  }
}

TEST(CompressorEffectTest, TheTwoDetectorsAgreeOnASteadyTone) {
  const Params peak = {{"thresholdDb", -30.0}, {"ratio", 4.0}, {"kneeDb", 6.0}, {"detector", 0.0}, {"mix", 1.0}};
  const Params rms = {{"thresholdDb", -30.0}, {"ratio", 4.0}, {"kneeDb", 6.0}, {"detector", 1.0}, {"mix", 1.0}};
  EXPECT_NEAR(staticGainDb(peak, -15.0), staticGainDb(rms, -15.0), 0.3);
}

TEST(CompressorEffectTest, StereoPairIsLinkedAndDualMonoIsNot) {
  // A loud tone on the left, a quiet one on the right (under the threshold).
  const Params params = {{"thresholdDb", -30.0}, {"ratio", 10.0}, {"kneeDb", 0.0}, {"attackMs", 1.0}};
  const Stereo in{steppedSine(1000.0, {{1.5, -10.0}}), steppedSine(1500.0, {{1.5, -50.0}})};
  const size_t from = static_cast<size_t>(1.0 * kRate);
  const auto gain = [&](const std::vector<float>& out, const std::vector<float>& source) {
    return toDb(rmsOf(out, from)) - toDb(rmsOf(source, from));
  };

  DirectKind linked(kKind, params, Layout::Stereo);
  const Stereo outLinked = linked.render(in);
  EXPECT_LT(gain(outLinked.left, in.left), -10.0);
  EXPECT_NEAR(gain(outLinked.right, in.right), gain(outLinked.left, in.left), 0.2)
      << "a stereo pair shares one gain reduction";

  DirectKind dual(kKind, params, Layout::DualMono);
  const Stereo outDual = dual.render(in);
  EXPECT_LT(gain(outDual.left, in.left), -10.0);
  EXPECT_NEAR(gain(outDual.right, in.right), 0.0, 0.05)
      << "in dual mono the quiet guitar must not be pulled down by the loud one";
}

TEST(CompressorEffectTest, ReportsNoTailAndNoLatency) {
  EffectRig rig(kKind);
  EXPECT_EQ(rig.tailSeconds(), 0.0);
  EXPECT_EQ(rig.latencySamples(), 0);
}

TEST(CompressorEffectTest, GainReductionIsReportedAsAReading) {
  // Threshold -30 dB, 4:1, hard knee: a steady -10 dB tone is pulled down
  // 15 dB (see StaticCurveMatchesThresholdAndRatio), and the reading says so.
  DirectKind kind(kKind, {{"thresholdDb", -30.0}, {"ratio", 4.0}, {"kneeDb", 0.0}, {"attackMs", 1.0}});
  ASSERT_TRUE(kind.valid());
  ASSERT_EQ(kind.engine().readingTable().size(), 1u);
  EXPECT_EQ(kind.engine().readingTable()[0].id, juce::String("gainReductionDb"));
  EXPECT_FLOAT_EQ(kind.engine().getReading(0), 0.0f) << "nothing processed yet";

  kind.render(sine(1.0, 1000.0, amp(-10.0), kRate));
  EXPECT_NEAR(kind.engine().getReading(0), 15.0, 0.5);

  // A reset (the block stops being processed) puts the meter back to rest.
  kind.engine().reset();
  EXPECT_FLOAT_EQ(kind.engine().getReading(0), 0.0f);

  // Below the threshold nothing is pulled down.
  kind.render(sine(1.0, 1000.0, amp(-50.0), kRate));
  EXPECT_FLOAT_EQ(kind.engine().getReading(0), 0.0f);
}

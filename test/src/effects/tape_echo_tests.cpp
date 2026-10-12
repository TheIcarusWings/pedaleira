// delay.tape (plugin/src/effects/delay/TapeEcho.cpp): the conformance test,
// then what makes it a tape echo: the time, wow and flutter that move the
// pitch at their set rates and are absent at depth 0, band-limiting that
// worsens with `age`, saturation, a pitch bend on a change of time, and a
// loop that can be pushed to self-oscillation and still cannot run away.
#include "effects/delay_test_support.h"
#include "effects/dsp/TempoSync.h"

#include <gtest/gtest.h>

using namespace delaytest;

namespace {

constexpr const char* kKind = "delay.tape";

// What the heads' filters add to a delay as the peak of a cross-correlation
// sees it: their group delay, a few samples.
constexpr int kFilterLagSamples = 8;

// A setting where the wobble is off, the saturator is at unity and the tape
// is fresh.
const Params kSteady = {{"timeMs", 250.0}, {"feedback", 60.0}, {"wow", 0.0},
                        {"flutter", 0.0},  {"saturation", 0.0}, {"age", 0.0},
                        {"mix", 1.0}};

}  // namespace

TEST(TapeEchoTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"timeMs", 120.0}, {"feedback", 45.0}, {"wow", 50.0},
                    {"flutter", 40.0}, {"saturation", 50.0}, {"age", 50.0}};
  options.stateParams = {{"sync", 1.0},          {"division", fx::kDivEighthDotted},
                         {"timeMs", 123.0},      {"feedback", 55.0},
                         {"wow", 70.0},          {"wowRateHz", 1.5},
                         {"flutter", 60.0},      {"flutterRateHz", 9.0},
                         {"saturation", 80.0},   {"age", 80.0},
                         {"spillover", 0.0}};
  options.latency = LatencyCheck::AtMostOnset;
  options.cpuAverageBudgetNs = 10000.0;
  options.cpuWorstBudgetNs = 80000.0;
  expectKindConforms("delay.tape", options);
}

TEST(TapeEchoTest, DelayTimeMatchesTheParameter) {
  for (const double timeMs : {40.0, 300.0, 1100.0}) {
    SCOPED_TRACE(timeMs);
    EXPECT_NEAR(measuredDelaySamples(kKind, withParams(kSteady, {{"timeMs", timeMs}, {"feedback", 0.0}}),
                                     ms(timeMs)),
                ms(timeMs), kFilterLagSamples);
  }
}

TEST(TapeEchoTest, SyncedTimeFollowsTheTempoAtTwoTempos) {
  struct Case {
    double bpm;
    int division;
    double expectedMs;
  };
  for (const Case& c : {Case{100.0, fx::kDivEighth, 300.0}, Case{150.0, fx::kDivEighthDotted, 300.0},
                        Case{150.0, fx::kDivQuarter, 400.0}}) {
    SCOPED_TRACE(c.bpm);
    RigOptions options;
    options.bpm = c.bpm;
    const Params params = withParams(kSteady, {{"sync", 1.0},
                                               {"division", static_cast<double>(c.division)},
                                               {"timeMs", 77.0},
                                               {"feedback", 0.0}});
    EXPECT_NEAR(measuredDelaySamples(kKind, params, ms(c.expectedMs), options), ms(c.expectedMs),
                kFilterLagSamples);
  }
}

TEST(TapeEchoTest, RepeatsFallByTheFeedbackSetting) {
  for (const double feedback : {30.0, 60.0, 85.0}) {
    SCOPED_TRACE(feedback);
    EffectRig rig(kKind, withParams(kSteady, {{"feedback", feedback}}));
    const std::vector<float> out = rig.render(toneBurst(0.04, 500.0, 0.25f, 3.0));
    double previous = rmsWindow(out, 250.0, 60.0);
    EXPECT_GT(previous, 0.01);
    for (int k = 2; k <= 4; ++k) {
      const double level = rmsWindow(out, 250.0 * k, 60.0);
      EXPECT_NEAR(level / previous, feedback * 0.01, 0.04) << "repeat " << k;
      previous = level;
    }
  }
}

TEST(TapeEchoTest, RepeatsGetDarkerEachPassAndAgeNarrowsTheBand) {
  const auto render = [](double age) {
    EffectRig rig(kKind, withParams(kSteady, {{"timeMs", 200.0}, {"feedback", 85.0}, {"age", age}}));
    return rig.render(noiseBurst(0.08, 8u, 0.4f, 1.5));
  };
  const std::vector<float> fresh = render(0.0);
  const std::vector<float> worn = render(100.0);

  for (const std::vector<float>* out : {&fresh, &worn}) {
    SCOPED_TRACE(out == &fresh ? "fresh" : "worn");
    std::vector<double> centroid;
    for (int k = 1; k <= 4; ++k)
      centroid.push_back(spectralCentroidHz(*out, static_cast<size_t>(ms(200.0 * k))));
    for (size_t k = 1; k < centroid.size(); ++k)
      EXPECT_LT(centroid[k], centroid[k - 1] * 0.95) << "repeat " << k + 1 << " is not darker";
    EXPECT_LT(centroid[3], centroid[0] * 0.8);
  }

  // Worn tape is narrower at both ends from the very first repeat.
  const size_t first = static_cast<size_t>(ms(200.0));
  EXPECT_LT(spectralCentroidHz(worn, first), spectralCentroidHz(fresh, first) * 0.7);
  EXPECT_LT(bandPower(worn, first, 4000.0, 24000.0), bandPower(fresh, first, 4000.0, 24000.0) * 0.1);
  EXPECT_LT(bandPower(worn, first, 30.0, 120.0), bandPower(fresh, first, 30.0, 120.0) * 0.35);
}

TEST(TapeEchoTest, SaturationAddsHarmonics) {
  const std::vector<float> in = sine(3.0, 220.0, 0.3f, kRate);
  const auto thdAt = [&](double saturation) {
    EffectRig rig(kKind, withParams(kSteady, {{"timeMs", 100.0}, {"feedback", 0.0},
                                              {"saturation", saturation}}));
    return thdPercent(rig.render(in), 220.0, kRate, static_cast<size_t>(ms(1000.0)));
  };
  const double clean = thdAt(0.0);
  const double dirty = thdAt(100.0);
  EXPECT_LT(clean, 1.0);
  EXPECT_GT(dirty, 3.0);
  EXPECT_GT(dirty, clean * 5.0);
}

TEST(TapeEchoTest, WowAndFlutterMovePitchAtTheirRatesAndNotAtDepthZero) {
  // A steady 800 Hz sine, one echo: the output is the echo.
  const std::vector<float> in = sine(10.0, 800.0, 0.25f, kRate);
  const size_t skip = static_cast<size_t>(ms(800.0));
  const Params base = withParams(kSteady, {{"timeMs", 250.0}, {"feedback", 0.0}});

  // Off: no pitch movement at all, whatever the rates.
  {
    EffectRig rig(kKind, withParams(base, {{"wowRateHz", 2.5}, {"flutterRateHz", 9.0}}));
    const PitchModulation none = analysePitchModulation(rig.render(in), skip, 0.3, 20.0);
    EXPECT_NEAR(none.carrierHz, 800.0, 0.5);
    EXPECT_LT(none.worstDeviation, 1.0e-4) << "the pitch moves with wow and flutter at zero";
  }
  // Wow alone, at two rates: the strongest pitch component sits at the rate
  // and swings by the stated 1.2% at 100% depth.
  for (const double rate : {1.3, 2.5}) {
    SCOPED_TRACE(rate);
    EffectRig rig(kKind, withParams(base, {{"wow", 100.0}, {"wowRateHz", rate}}));
    const PitchModulation m = analysePitchModulation(rig.render(in), skip, 0.3, 20.0);
    EXPECT_NEAR(m.rateHz, rate, 0.15);
    EXPECT_NEAR(m.deviation, 0.012, 0.012 * 0.3);
  }
  // Half the depth, half the swing.
  {
    EffectRig rig(kKind, withParams(base, {{"wow", 50.0}, {"wowRateHz", 2.5}}));
    EXPECT_NEAR(analysePitchModulation(rig.render(in), skip, 0.3, 20.0).deviation, 0.006, 0.006 * 0.35);
  }
  // Flutter alone: faster and half as deep.
  for (const double rate : {6.0, 11.0}) {
    SCOPED_TRACE(rate);
    EffectRig rig(kKind, withParams(base, {{"flutter", 100.0}, {"flutterRateHz", rate}}));
    const PitchModulation m = analysePitchModulation(rig.render(in), skip, 0.3, 20.0);
    EXPECT_NEAR(m.rateHz, rate, 0.2);
    EXPECT_NEAR(m.deviation, 0.006, 0.006 * 0.3);
  }
}

TEST(TapeEchoTest, TheDriftIsRandomButRepeatable) {
  // Wow brings a slow random wander with it: the pitch is not a pure sine of
  // the wow rate, but two runs are the same bits.
  const std::vector<float> in = sine(6.0, 800.0, 0.25f, kRate);
  const Params params = withParams(kSteady, {{"timeMs", 250.0}, {"feedback", 0.0}, {"wow", 100.0},
                                             {"wowRateHz", 2.5}});
  EffectRig a(kKind, params);
  EffectRig b(kKind, params);
  const std::vector<float> first = a.render(in);
  EXPECT_EQ(first, b.render(in));
  a.restart();
  EXPECT_EQ(first, a.render(in));
  // The wander shows as pitch movement away from the wow rate.
  const PitchModulation m =
      analysePitchModulation(first, static_cast<size_t>(ms(800.0)), 0.1, 0.9);
  EXPECT_GT(m.deviation, 5.0e-4);
}

TEST(TapeEchoTest, ATimeChangeBendsThePitchWithoutAStep) {
  const std::vector<float> in = sine(8.0, 300.0, 0.25f, kRate);
  for (const double to : {700.0, 120.0}) {
    SCOPED_TRACE(to);
    EffectRig rig(kKind, withParams(kSteady, {{"timeMs", 400.0}, {"feedback", 0.0}}));
    std::vector<float> out = rig.render(std::vector<float>(in.begin(), in.begin() + ms(2000.0)));
    const float steady = maxStep(out, static_cast<size_t>(ms(1000.0)), out.size());
    rig.set("timeMs", to);
    const std::vector<float> after = rig.render(std::vector<float>(in.begin() + ms(2000.0), in.end()));
    out.insert(out.end(), after.begin(), after.end());
    expectCleanSignal(out, 1.0f, "time change");
    // The pointer moves at no more than 0.4 samples per sample: 60% speed
    // while the delay grows, 140% while it shrinks, with no step.
    EXPECT_LT(maxStep(out, static_cast<size_t>(ms(1900.0)), out.size()), steady * 1.5f + 0.002f)
        << "the change of time stepped the waveform";
    const PitchModulation during = analysePitchModulation(out, static_cast<size_t>(ms(2150.0)), 0.5,
                                                          5.0, static_cast<size_t>(ms(2500.0)));
    if (to > 400.0)
      EXPECT_NEAR(during.carrierHz, 300.0 * 0.6, 300.0 * 0.05) << "no pitch bend while the delay grows";
    else
      EXPECT_NEAR(during.carrierHz, 300.0 * 1.4, 300.0 * 0.05) << "no pitch bend while the delay shrinks";
    const PitchModulation settled = analysePitchModulation(out, static_cast<size_t>(ms(6500.0)), 0.5, 5.0);
    EXPECT_NEAR(settled.carrierHz, 300.0, 0.6);
    EXPECT_NEAR(measuredDelaySamples(kKind, withParams(kSteady, {{"timeMs", to}, {"feedback", 0.0}}),
                                     ms(to)),
                ms(to), kFilterLagSamples);
  }
}

TEST(TapeEchoTest, OutputStaysBoundedAtMaximumFeedbackWithAHotInput) {
  // Fresh tape (the bump pushes the loop above unity) and worn tape, short
  // and long.
  expectBoundedAtMaximumFeedback(kKind, {{"timeMs", 90.0}, {"age", 0.0}, {"saturation", 0.0}}, 2.0f);
  expectBoundedAtMaximumFeedback(kKind, {{"timeMs", 1500.0}, {"age", 0.0}, {"saturation", 100.0}}, 2.0f);
  expectBoundedAtMaximumFeedback(kKind, {{"timeMs", 400.0}, {"age", 100.0}}, 2.0f);
}

TEST(TapeEchoTest, FreshTapeAtHighFeedbackSelfOscillatesAndWornTapeDecays) {
  // A burst, then 25 s of silence in. The fresh tape's head bump lifts the
  // loop above unity near 90 Hz, so it keeps singing; the worn tape has no
  // bump and a lower gain, and dies away.
  const std::vector<float> burst = withSilence(noise(0.5, 13u, 0.25f, kRate), ms(25000.0));
  const auto lastSeconds = [&](double feedback, double age) {
    EffectRig rig(kKind, withParams(kSteady, {{"timeMs", 400.0}, {"feedback", feedback},
                                              {"age", age}, {"saturation", 35.0}}));
    const std::vector<float> out = rig.render(burst);
    expectCleanSignal(out, 2.0f, "self-oscillation");
    return rmsOf(out, out.size() - static_cast<size_t>(5 * kRate));
  };
  EXPECT_GT(lastSeconds(95.0, 0.0), 0.02) << "the loop does not reach self-oscillation";
  EXPECT_LT(lastSeconds(80.0, 100.0), 1.0e-3) << "worn tape at 80% should have died away";
}

TEST(TapeEchoTest, TailFollowsTimeFeedbackAndAge) {
  EffectRig rig(kKind, withParams(kSteady, {{"timeMs", 200.0}, {"feedback", 0.0}, {"age", 100.0}}));
  EXPECT_NEAR(rig.tailSeconds(), 0.2 + 0.05, 0.003);
  rig.set("feedback", 50.0);
  EXPECT_NEAR(rig.tailSeconds(), 0.2 * 11.0 + 0.05, 0.02);
  // Fresh tape's bump raises the worst-case loop gain, so the same feedback
  // rings longer; at 80% it never falls (0.8 * 1.26 > 1) and the cap is
  // reported.
  rig.set("age", 0.0);
  const double fresh = rig.tailSeconds();
  EXPECT_GT(fresh, 0.2 * 11.0 + 0.05);
  rig.set("feedback", 80.0);
  EXPECT_DOUBLE_EQ(rig.tailSeconds(), kMaxBlockTailSeconds);
  rig.set("age", 100.0);
  EXPECT_LT(rig.tailSeconds(), kMaxBlockTailSeconds);
}

TEST(TapeEchoTest, SpilloverRingsOutAfterBypassAndStopsAtTheReportedTail) {
  expectSpilloverBehaviour(kKind, withParams(kSteady, {{"timeMs", 200.0}, {"feedback", 70.0},
                                                       {"age", 50.0}, {"wow", 25.0}, {"flutter", 15.0}}),
                           350.0, 550.0);
}

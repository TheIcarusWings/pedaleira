// delay.analog (plugin/src/effects/delay/AnalogDelay.cpp): the conformance
// test, then what makes it a bucket-brigade echo: the time (free and synced,
// to within the group delay of its low-pass pair), repeats that get darker
// and dirtier each pass, a low-pass that tightens with longer times, a
// pitch bend on a change of time without a step in the waveform, a loop that
// cannot run away, and a tail that rings out after a bypass.
#include "effects/delay_test_support.h"
#include "effects/dsp/TempoSync.h"

#include <gtest/gtest.h>

using namespace delaytest;

namespace {

constexpr const char* kKind = "delay.analog";

// What the reconstruction low-pass pair adds to a delay, as the peak of a
// cross-correlation sees it: its group delay, a few samples at 4-5 kHz.
constexpr int kFilterLagSamples = 8;

// A setting where the low-pass is wide open and the saturator is off.
const Params kOpen = {{"timeMs", 250.0}, {"feedback", 60.0}, {"toneHz", 12000.0}, {"grit", 0.0},
                      {"mix", 1.0}};

}  // namespace

TEST(AnalogDelayTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"timeMs", 120.0}, {"feedback", 40.0}, {"toneHz", 3000.0}, {"grit", 60.0}};
  options.stateParams = {{"sync", 1.0},      {"division", fx::kDivEighthDotted},
                         {"timeMs", 123.0},  {"feedback", 55.0},
                         {"toneHz", 2500.0}, {"grit", 80.0},
                         {"spillover", 0.0}};
  options.latency = LatencyCheck::AtMostOnset;
  options.cpuAverageBudgetNs = 4000.0;
  options.cpuWorstBudgetNs = 50000.0;
  expectKindConforms("delay.analog", options);
}

TEST(AnalogDelayTest, DelayTimeMatchesTheParameter) {
  for (const double timeMs : {40.0, 300.0, 1100.0}) {
    SCOPED_TRACE(timeMs);
    EXPECT_NEAR(measuredDelaySamples(kKind, withParams(kOpen, {{"timeMs", timeMs}, {"feedback", 0.0}}),
                                     ms(timeMs)),
                ms(timeMs), kFilterLagSamples);
  }
}

TEST(AnalogDelayTest, SyncedTimeFollowsTheTempoAtTwoTempos) {
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
    const Params params = withParams(kOpen, {{"sync", 1.0},
                                             {"division", static_cast<double>(c.division)},
                                             {"timeMs", 77.0},
                                             {"feedback", 0.0}});
    EXPECT_NEAR(measuredDelaySamples(kKind, params, ms(c.expectedMs), options), ms(c.expectedMs),
                kFilterLagSamples);
  }
}

TEST(AnalogDelayTest, RepeatsFallByTheFeedbackSetting) {
  for (const double feedback : {30.0, 60.0, 85.0}) {
    SCOPED_TRACE(feedback);
    EffectRig rig(kKind, withParams(kOpen, {{"feedback", feedback}}));
    const std::vector<float> out = rig.render(toneBurst(0.04, 300.0, 0.25f, 3.0));
    double previous = rmsWindow(out, 250.0, 60.0);
    EXPECT_GT(previous, 0.01);
    for (int k = 2; k <= 4; ++k) {
      const double level = rmsWindow(out, 250.0 * k, 60.0);
      // A little under the setting: the saturator at unity drive and the
      // low-pass take a percent or two each pass.
      EXPECT_NEAR(level / previous, feedback * 0.01, 0.04) << "repeat " << k;
      previous = level;
    }
  }
}

TEST(AnalogDelayTest, RepeatsGetDarkerEachPass) {
  EffectRig rig(kKind, withParams(kOpen, {{"timeMs", 200.0}, {"feedback", 85.0}, {"toneHz", 4000.0},
                                          {"grit", 30.0}}));
  const std::vector<float> out = rig.render(noiseBurst(0.08, 8u, 0.4f, 1.5));
  std::vector<double> centroid;
  for (int k = 1; k <= 4; ++k)
    centroid.push_back(spectralCentroidHz(out, static_cast<size_t>(ms(200.0 * k))));
  for (size_t k = 1; k < centroid.size(); ++k)
    EXPECT_LT(centroid[k], centroid[k - 1] * 0.93) << "repeat " << k + 1 << " is not darker";
  EXPECT_LT(centroid[3], centroid[0] * 0.7);

  // The top end goes first: the share of power above 6 kHz in the fourth
  // repeat is a fraction of the first's.
  const double top1 = bandPower(out, static_cast<size_t>(ms(200.0)), 6000.0, 24000.0) /
                      bandPower(out, static_cast<size_t>(ms(200.0)), 100.0, 24000.0);
  const double top4 = bandPower(out, static_cast<size_t>(ms(800.0)), 6000.0, 24000.0) /
                      bandPower(out, static_cast<size_t>(ms(800.0)), 100.0, 24000.0);
  EXPECT_LT(top4, top1 * 0.25);
}

TEST(AnalogDelayTest, TheLowPassTightensWithLongerTimes) {
  const auto centroidAt = [](double timeMs) {
    EffectRig rig(kKind, withParams(kOpen, {{"timeMs", timeMs}, {"feedback", 0.0}, {"toneHz", 4000.0}}));
    const std::vector<float> out = rig.render(noiseBurst(0.08, 8u, 0.4f, 2.0));
    return spectralCentroidHz(out, static_cast<size_t>(ms(timeMs)));
  };
  const double short100 = centroidAt(100.0);
  const double mid300 = centroidAt(300.0);
  const double long1000 = centroidAt(1000.0);
  EXPECT_GT(short100, mid300 * 1.1);
  EXPECT_GT(mid300, long1000 * 1.2);
}

TEST(AnalogDelayTest, GritAddsHarmonicsAndNoneBelowItsDrive) {
  // 220 Hz at 0.3 through the single echo; THD of the echo.
  const std::vector<float> in = sine(3.0, 220.0, 0.3f, kRate);
  const auto thdAt = [&](double grit) {
    EffectRig rig(kKind, withParams(kOpen, {{"timeMs", 100.0}, {"feedback", 0.0}, {"grit", grit}}));
    return thdPercent(rig.render(in), 220.0, kRate, static_cast<size_t>(ms(1000.0)));
  };
  const double clean = thdAt(0.0);
  const double dirty = thdAt(100.0);
  EXPECT_LT(clean, 1.0);
  EXPECT_GT(dirty, 3.0);
  EXPECT_GT(dirty, clean * 5.0);
}

TEST(AnalogDelayTest, ATimeChangeBendsThePitchWithoutAStep) {
  // 300 Hz at 0.25 moves 0.01 per sample.
  const std::vector<float> in = sine(8.0, 300.0, 0.25f, kRate);
  for (const double to : {700.0, 120.0}) {
    SCOPED_TRACE(to);
    EffectRig rig(kKind, withParams(kOpen, {{"timeMs", 400.0}, {"feedback", 0.0}}));
    std::vector<float> out = rig.render(std::vector<float>(in.begin(), in.begin() + ms(2000.0)));
    const float steady = maxStep(out, static_cast<size_t>(ms(1000.0)), out.size());
    rig.set("timeMs", to);
    const std::vector<float> after = rig.render(std::vector<float>(in.begin() + ms(2000.0), in.end()));
    out.insert(out.end(), after.begin(), after.end());
    expectCleanSignal(out, 1.0f, "time change");
    // The pointer chases the new time at no more than 0.3 samples per sample,
    // so the echo plays at 70% speed while the delay grows (130% while it
    // shrinks) and the waveform stays continuous.
    EXPECT_LT(maxStep(out, static_cast<size_t>(ms(1900.0)), out.size()), steady * 1.45f + 0.002f)
        << "the change of time stepped the waveform";
    const PitchModulation before = analysePitchModulation(out, static_cast<size_t>(ms(500.0)),
                                                          0.5, 5.0, static_cast<size_t>(ms(1900.0)));
    EXPECT_NEAR(before.carrierHz, 300.0, 0.6);
    const PitchModulation during = analysePitchModulation(out, static_cast<size_t>(ms(2300.0)), 0.5,
                                                          5.0, static_cast<size_t>(ms(2800.0)));
    if (to > 400.0)
      EXPECT_NEAR(during.carrierHz, 300.0 * 0.7, 300.0 * 0.05) << "no pitch bend while the delay grows";
    else
      EXPECT_NEAR(during.carrierHz, 300.0 * 1.3, 300.0 * 0.05) << "no pitch bend while the delay shrinks";
    // And it settles back to the pitch, at the new time.
    const PitchModulation settled = analysePitchModulation(out, static_cast<size_t>(ms(6500.0)), 0.5, 5.0);
    EXPECT_NEAR(settled.carrierHz, 300.0, 0.6);
    EXPECT_NEAR(measuredDelaySamples(kKind, withParams(kOpen, {{"timeMs", to}, {"feedback", 0.0}}),
                                     ms(to)),
                ms(to), kFilterLagSamples);
  }
}

TEST(AnalogDelayTest, OutputStaysBoundedAtMaximumFeedbackWithAHotInput) {
  expectBoundedAtMaximumFeedback(kKind, {{"timeMs", 90.0}, {"toneHz", 12000.0}, {"grit", 0.0}}, 1.2f);
  expectBoundedAtMaximumFeedback(kKind, {{"timeMs", 1200.0}, {"toneHz", 800.0}, {"grit", 100.0}}, 1.2f);
}

TEST(AnalogDelayTest, TailFollowsTimeAndFeedback) {
  EffectRig rig(kKind, {{"timeMs", 200.0}, {"feedback", 0.0}, {"toneHz", 12000.0}});
  EXPECT_NEAR(rig.tailSeconds(), 0.2 + 0.05, 0.003);
  rig.set("feedback", 50.0);
  EXPECT_NEAR(rig.tailSeconds(), 0.2 * 11.0 + 0.05, 0.01);
  // A longer time darkens the repeats, and the group delay of the darker
  // filter is part of the tail.
  rig.set("toneHz", 800.0);
  EXPECT_GT(rig.tailSeconds(), 0.2 * 11.0 + 0.05 + 0.003);
  rig.set("timeMs", 1200.0);
  rig.set("feedback", 95.0);
  EXPECT_DOUBLE_EQ(rig.tailSeconds(), kMaxBlockTailSeconds);
}

TEST(AnalogDelayTest, SpilloverRingsOutAfterBypassAndStopsAtTheReportedTail) {
  expectSpilloverBehaviour(kKind, {{"timeMs", 200.0}, {"feedback", 70.0}, {"toneHz", 12000.0},
                                   {"grit", 0.0}},
                           350.0, 550.0);
}

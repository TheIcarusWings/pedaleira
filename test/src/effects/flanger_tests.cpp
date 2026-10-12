// modulation.flanger (plugin/src/effects/modulation/Flanger.cpp): the
// conformance test, then what a flanger is for: a comb whose notches sit at
// odd multiples of 1 / (2 * delay), peaks and notches that swap with the sign
// of the feedback, a sweep at the stated rate (free and tempo-synced) and a
// loop that cannot run away.
#include "effects/effect_test_helpers.h"
#include "effects/modulation_test_helpers.h"

#include <gtest/gtest.h>

using namespace fxtest;
using namespace modtest;

namespace {

constexpr const char* kKind = "modulation.flanger";

// The flanger alone, fully wet, held still (no depth) at a delay of
// `manualMs` and `feedback` percent: its impulse response.
std::vector<float> staticResponse(double manualMs, double feedback, double seconds = 0.5) {
  EffectRig rig(kKind, {{"depth", 0.0}, {"manualMs", manualMs}, {"feedback", feedback},
                        {"mix", 1.0}});
  return captureImpulseResponse(rig, seconds);
}

// Rate of the notch sweep, read off the level of a 500 Hz probe: at a
// delay of 2.5 ms the probe sits on the flank of a notch, so the notch
// moving up and down the spectrum (10% depth: 15% either side) moves the
// level of the probe up and down once per cycle.
double measuredRate(Params params, double bpm = 120.0) {
  params.emplace_back("mix", 1.0);
  params.emplace_back("depth", 10.0);
  params.emplace_back("manualMs", 2.5);
  params.emplace_back("feedback", 0.0);
  RigOptions options;
  options.bpm = bpm;
  EffectRig rig(kKind, params, options);
  const std::vector<float> out = rig.render(sine(8.0, 500.0, 0.25f, kRate));
  return amplitudeModulationRateHz(out, kRate, static_cast<size_t>(samples(0.5)));
}

}  // namespace

TEST(FlangerTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"depth", 80.0}, {"rateHz", 1.0}, {"feedback", 60.0}};
  options.stateParams = {{"sync", 1.0},    {"division", fx::kDivEighth},
                         {"rateHz", 2.0},  {"depth", 30.0},
                         {"manualMs", 4.0}, {"feedback", -40.0},
                         {"width", 20.0}};
  options.cpuAverageBudgetNs = 10000.0;
  // About 50x the average: one block in thousands can be descheduled on a
  // busy machine, and that says nothing about the effect.
  options.cpuWorstBudgetNs = 150000.0;
  expectKindConforms("modulation.flanger", options);
}

TEST(FlangerTest, RegisteredAsACrossfadeWithTheCombInTheWet) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kKind);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->category, BlockCategory::Modulation);
  EXPECT_EQ(info->mixLaw, BlockMixLaw::Crossfade);
  // The wet starts with the input at once: no latency, no onset delay.
  EffectRig rig(kKind, {{"mix", 1.0}});
  EXPECT_EQ(onsetIndex(captureImpulseResponse(rig, 0.1)), 0);
}

TEST(FlangerTest, CombSpacingMatchesTheDelayAtAFrozenPhase) {
  for (const double delayMs : {2.0, 4.0}) {
    SCOPED_TRACE(delayMs);
    const std::vector<float> ir = staticResponse(delayMs, 0.0, 0.3);
    const std::vector<double> notches = responseNotches(ir, 100.0, 9000.0, 4.0, 25.0);
    const double spacing = 1000.0 / delayMs;  // Hz
    ASSERT_GE(notches.size(), 8u);
    // Odd multiples of half the spacing: a notch where the delayed copy
    // arrives in opposite phase.
    for (const double notch : notches) {
      const double k = std::round(notch / spacing - 0.5);
      EXPECT_NEAR(notch, (k + 0.5) * spacing, 0.02 * spacing) << notch << " Hz";
    }
    const double mean = (notches.back() - notches.front()) / static_cast<double>(notches.size() - 1);
    EXPECT_NEAR(mean, spacing, 0.01 * spacing);
  }
}

TEST(FlangerTest, NegativeFeedbackSwapsPeaksAndNotches) {
  // A 2 ms delay: peaks at multiples of 500 Hz, notches halfway between.
  const std::vector<double> peaks = {500.0, 1000.0, 1500.0, 2000.0, 2500.0, 3000.0};
  std::vector<double> between;
  for (const double f : peaks)
    between.push_back(f - 250.0);

  const auto levels = [&](double feedback) {
    const std::vector<float> ir = staticResponse(2.0, feedback);
    return std::pair{magnitudeResponseDb(ir, peaks, kRate), magnitudeResponseDb(ir, between, kRate)};
  };
  const auto [positivePeaks, positiveBetween] = levels(80.0);
  const auto [negativePeaks, negativeBetween] = levels(-80.0);
  for (size_t i = 0; i < peaks.size(); ++i) {
    SCOPED_TRACE(peaks[i]);
    EXPECT_GT(positivePeaks[i], positiveBetween[i] + 6.0) << "positive feedback peaks at k / D";
    EXPECT_GT(negativeBetween[i], negativePeaks[i] + 6.0) << "negative feedback peaks halfway";
  }
}

TEST(FlangerTest, SweepRunsAtTheRateParameter) {
  for (const double rateHz : {1.0, 3.0}) {
    SCOPED_TRACE(rateHz);
    EXPECT_NEAR(measuredRate({{"rateHz", rateHz}}), rateHz, 0.03 * rateHz);
  }
}

TEST(FlangerTest, SyncedRateFollowsTheTempoAtTwoTempos) {
  struct Case {
    double bpm;
    int division;
    double expectedHz;
  };
  for (const Case& c : {Case{120.0, fx::kDivQuarter, 2.0}, Case{90.0, fx::kDivQuarter, 1.5},
                        Case{90.0, fx::kDivEighth, 3.0}}) {
    SCOPED_TRACE(c.bpm);
    EXPECT_NEAR(measuredRate({{"sync", 1.0}, {"division", static_cast<double>(c.division)},
                              {"rateHz", 7.0}},
                             c.bpm),
                c.expectedHz, 0.03 * c.expectedHz);
  }
}

TEST(FlangerTest, DepthZeroIsTheStaticCombWhateverTheRate) {
  const std::vector<float> in = noise(2.0, 5u, 0.25f, kRate);
  const auto render = [&](double rateHz) {
    DirectKind kind(kKind, {{"depth", 0.0}, {"manualMs", 2.0}, {"feedback", 0.0}, {"rateHz", rateHz}});
    return kind.render(in);
  };
  const std::vector<float> slow = render(0.2);
  const std::vector<float> fast = render(9.0);
  EXPECT_LT(maxDifference(slow, fast), 1.0e-6f);

  // And it is what the comb says: (x[n] + x[n - D]) / sqrt(2) for a 2 ms
  // (96 sample) delay without feedback.
  std::vector<float> expected(in.size(), 0.0f);
  for (size_t i = 0; i < in.size(); ++i)
    expected[i] = (in[i] + (i >= 96 ? in[i - 96] : 0.0f)) * 0.70710678f;
  EXPECT_LT(maxDifference(slow, expected), 1.0e-5f);
}

TEST(FlangerTest, WetLevelFollowsTheInputWhateverTheFeedback) {
  for (const double feedback : {0.0, 60.0, -60.0}) {
    SCOPED_TRACE(feedback);
    EffectRig rig(kKind, {{"depth", 40.0}, {"feedback", feedback}, {"mix", 1.0}});
    const std::vector<float> in = noise(4.0, 9u, 0.1f, kRate);
    const std::vector<float> out = rig.render(in);
    EXPECT_NEAR(steadyStateGainDb(out, in, static_cast<size_t>(samples(0.5))), 0.0, 1.5);
  }
}

TEST(FlangerTest, TailFollowsFeedbackAndTheLongestDelay) {
  EffectRig rig(kKind, {{"depth", 0.0}, {"manualMs", 5.0}, {"feedback", 0.0}});
  EXPECT_NEAR(rig.tailSeconds(), 0.005 + 0.01, 1e-6);
  // 90%: the 66th pass is 60 dB down.
  rig.set("feedback", 90.0);
  EXPECT_NEAR(rig.tailSeconds(), 0.005 * 67.0 + 0.01, 1e-6);
  rig.set("feedback", -90.0);
  EXPECT_NEAR(rig.tailSeconds(), 0.005 * 67.0 + 0.01, 1e-6);
  // And it covers the ring that is measured (a loud burst, then silence).
  for (const double feedback : {90.0, -90.0}) {
    SCOPED_TRACE(feedback);
    const TailMeasurement tail = expectSilenceAfterReportedTail(
        kKind, {{"depth", 0.0}, {"manualMs", 5.0}, {"feedback", feedback}});
    EXPECT_GT(tail.measuredSeconds, 0.05) << "feedback should ring";
    EXPECT_FALSE(tail.capped);
  }
}

TEST(FlangerTest, SidesDifferOnlyInATrueStereoImage) {
  const std::vector<float> in = noise(2.0, 3u, 0.25f, kRate);
  {
    EffectRig rig(kKind, {{"width", 100.0}, {"depth", 100.0}, {"rateHz", 2.0}, {"mix", 1.0}});
    const Stereo out = rig.renderBoth(in);
    EXPECT_GT(relativeDifferenceDb(out.left, out.right, static_cast<size_t>(samples(0.2))), -12.0)
        << "no stereo spread at full width";
  }
  {
    EffectRig rig(kKind, {{"width", 0.0}, {"mix", 1.0}});
    const Stereo out = rig.renderBoth(in);
    EXPECT_LT(maxDifference(out.left, out.right), 1.0e-6f) << "width 0 must be a mono flanger";
  }
  {
    EffectRig rig(kKind, {{"width", 100.0}, {"mix", 1.0}}, rigFor(Layout::DualMono));
    const Stereo out = rig.renderBoth(in);
    EXPECT_LT(maxDifference(out.left, out.right), 1.0e-6f);
  }
  {
    DirectKind kind(kKind, {{"width", 100.0}}, Layout::Stereo);
    kind.ctx.nam.lastNamIndex = 3;
    const Stereo out = kind.render(Stereo{in, in});
    EXPECT_LT(maxDifference(out.left, out.right), 1.0e-6f) << "stereo before an amp is lost";
  }
}

TEST(FlangerTest, StaysFiniteAndBoundedAtMaximumFeedbackWithAHotInput) {
  // Twenty seconds of full-scale noise at 95% feedback of either sign, at
  // the shortest and the longest delay, with the sweep at its widest.
  for (const double feedback : {95.0, -95.0})
    for (const double manualMs : {0.5, 10.0}) {
      SCOPED_TRACE(feedback);
      SCOPED_TRACE(manualMs);
      DirectKind kind(kKind,
                      {{"feedback", feedback}, {"manualMs", manualMs}, {"depth", 100.0},
                       {"rateHz", 10.0}, {"width", 100.0}},
                      Layout::Stereo);
      const std::vector<float> hot = noise(20.0, 31u, 1.0f, kRate);
      const std::vector<float> other = noise(20.0, 32u, 1.0f, kRate);
      const Stereo out = kind.render(Stereo{hot, other});
      expectCleanSignal(out.left, 3.0f, "hot flanger, left");
      expectCleanSignal(out.right, 3.0f, "hot flanger, right");
      // It rings (not clamped to silence).
      EXPECT_GT(rmsOf(out.left), 0.1);
    }
}

// modulation.vibrato (plugin/src/effects/modulation/Vibrato.cpp): the
// conformance test, then what a vibrato is for: pure pitch modulation whose
// peak deviation is the depth setting in cents at any rate, a wobble at the
// stated rate (free and tempo-synced), a rise after the block is switched on,
// and, at depth 0, a plain short delay.
#include "effects/effect_test_helpers.h"
#include "effects/modulation_test_helpers.h"

#include <gtest/gtest.h>

using namespace fxtest;
using namespace modtest;

namespace {

constexpr const char* kKind = "modulation.vibrato";
constexpr double kCarrierHz = 1000.0;

// A 1 kHz sine through the fully wet vibrato on a rig.
std::vector<float> render(Params params, double seconds, double bpm = 120.0) {
  params.emplace_back("mix", 1.0);
  RigOptions options;
  options.bpm = bpm;
  EffectRig rig(kKind, params, options);
  return rig.render(sine(seconds, kCarrierHz, 0.25f, kRate));
}

double measuredRate(Params params, double bpm = 120.0) {
  params.emplace_back("riseMs", 0.0);
  return frequencyModulationRateHz(render(params, 8.0, bpm), kRate,
                                   static_cast<size_t>(samples(0.5)));
}

}  // namespace

TEST(VibratoTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"depthCents", 40.0}, {"rateHz", 5.0}, {"riseMs", 0.0}};
  options.stateParams = {{"sync", 1.0},        {"division", fx::kDivQuarterTriplet},
                         {"rateHz", 3.0},      {"depthCents", 33.0},
                         {"riseMs", 800.0}};
  // The wet is a delayed copy (at least 0.5 ms): the effect, not latency.
  options.latency = LatencyCheck::AtMostOnset;
  options.cpuAverageBudgetNs = 4000.0;
  // About 50x the average: one block in thousands can be descheduled on a
  // busy machine, and that says nothing about the effect.
  options.cpuWorstBudgetNs = 150000.0;
  expectKindConforms("modulation.vibrato", options);
}

TEST(VibratoTest, RegisteredFullyWet) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kKind);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->category, BlockCategory::Modulation);
  EXPECT_FLOAT_EQ(info->defaultMix, 1.0f);
}

TEST(VibratoTest, PitchWobbleRunsAtTheRateParameter) {
  for (const double rateHz : {1.0, 3.0, 8.0}) {
    SCOPED_TRACE(rateHz);
    EXPECT_NEAR(measuredRate({{"depthCents", 40.0}, {"rateHz", rateHz}}), rateHz, 0.03 * rateHz);
  }
}

TEST(VibratoTest, SyncedRateFollowsTheTempoAtTwoTempos) {
  struct Case {
    double bpm;
    int division;
    double expectedHz;
  };
  for (const Case& c : {Case{120.0, fx::kDivQuarter, 2.0}, Case{90.0, fx::kDivQuarter, 1.5},
                        Case{90.0, fx::kDivEighth, 3.0}}) {
    SCOPED_TRACE(c.bpm);
    EXPECT_NEAR(measuredRate({{"depthCents", 40.0}, {"sync", 1.0},
                              {"division", static_cast<double>(c.division)}, {"rateHz", 7.0}},
                             c.bpm),
                c.expectedHz, 0.03 * c.expectedHz);
  }
}

TEST(VibratoTest, PeakPitchDeviationIsTheDepthSettingAtAnyRate) {
  for (const double depth : {10.0, 30.0, 60.0})
    for (const double rateHz : {2.0, 5.0, 8.0}) {
      SCOPED_TRACE(depth);
      SCOPED_TRACE(rateHz);
      const CycleFrequencies f = cycleFrequencies(
          render({{"depthCents", depth}, {"rateHz", rateHz}, {"riseMs", 0.0}}, 4.0), kRate,
          static_cast<size_t>(samples(0.3)));
      const Deviation d = deviationOf(f, kCarrierHz, 0.5, 4.0);
      // Up and down by the same number of cents, about the played pitch.
      EXPECT_NEAR(d.maxCents, depth, 0.1 * depth + 0.5);
      EXPECT_NEAR(d.minCents, -depth, 0.1 * depth + 0.5);
      // The mean over cycles (not over time) sits a little above the carrier
      // because faster cycles are more numerous: about 0.6 Hz at 60 cents.
      EXPECT_NEAR(d.meanHz, kCarrierHz, 1.0);
    }
}

TEST(VibratoTest, ModulationRisesAfterTheBlockIsSwitchedOn) {
  const Params params = {{"depthCents", 30.0}, {"rateHz", 5.0}, {"riseMs", 1000.0}, {"mix", 1.0}};
  const auto peaks = [](const std::vector<float>& out, double from, double to) {
    const CycleFrequencies f = cycleFrequencies(out, kRate);
    const Deviation d = deviationOf(f, kCarrierHz, from, to);
    return std::max(d.maxCents, -d.minCents);
  };
  EffectRig rig(kKind, params);
  const std::vector<float> in = sine(3.0, kCarrierHz, 0.25f, kRate);
  const std::vector<float> out = rig.render(in);
  // A fifth of the way in the sweep is a fifth of the way up.
  EXPECT_LT(peaks(out, 0.01, 0.25), 0.35 * 30.0);
  EXPECT_GT(peaks(out, 0.01, 0.25), 0.02 * 30.0);
  EXPECT_NEAR(peaks(out, 1.4, 3.0), 30.0, 3.0);

  // Switched off and on again, the rise starts over.
  rig.setEnabled(false);
  rig.render(std::vector<float>(static_cast<size_t>(samples(0.5)), 0.0f));
  rig.setEnabled(true);
  const std::vector<float> again = rig.render(in);
  EXPECT_LT(peaks(again, 0.01, 0.25), 0.35 * 30.0);
  EXPECT_NEAR(peaks(again, 1.4, 3.0), 30.0, 3.0);
}

TEST(VibratoTest, DepthZeroIsAPlainShortDelay) {
  const std::vector<float> in = noise(2.0, 5u, 0.25f, kRate);
  const auto run = [&](double rateHz) {
    DirectKind kind(kKind, {{"depthCents", 0.0}, {"rateHz", rateHz}, {"riseMs", 0.0}});
    return kind.render(in);
  };
  const std::vector<float> slow = run(0.7);
  EXPECT_LT(maxDifference(slow, run(9.0)), 1.0e-7f);
  // 0.5 ms, 24 samples, nothing else.
  std::vector<float> expected(in.size(), 0.0f);
  for (size_t i = 24; i < in.size(); ++i)
    expected[i] = in[i - 24];
  EXPECT_LT(maxDifference(slow, expected), 1.0e-6f);
}

TEST(VibratoTest, SlowSyncedTemposShrinkTheDepthInsteadOfOverflowingTheLine) {
  // A whole note per cycle at 30 bpm is 0.125 Hz: 100 cents would need a
  // 76 ms sweep. The sweep is held to what the line has, 25 ms, which at
  // that rate bends the pitch by 2 pi * 0.125 * 0.025 = 1.96%, 34 cents.
  DirectKind kind(kKind,
                  {{"sync", 1.0}, {"division", static_cast<double>(fx::kDivWhole)},
                   {"depthCents", 100.0}, {"riseMs", 0.0}},
                  Layout::Mono, 256, 30.0);
  const std::vector<float> out = kind.render(sine(6.0, kCarrierHz, 0.25f, kRate));
  expectCleanSignal(out, 0.3f, "slow synced vibrato");
  const Deviation d = deviationOf(cycleFrequencies(out, kRate, static_cast<size_t>(samples(0.3))),
                                  kCarrierHz, 0.5, 6.0);
  EXPECT_NEAR(d.maxCents, 1200.0 * std::log2(1.0 + 2.0 * kPi * 0.125 * 0.025), 1.5);
  EXPECT_GT(rmsOf(out), 0.15);
}

TEST(VibratoTest, HasNoStereoBehaviour) {
  // Pitch modulation moves both sides together: a true stereo buffer comes
  // out as two identical sides, and two independent signals stay apart.
  const std::vector<float> in = noise(1.0, 3u, 0.25f, kRate);
  {
    EffectRig rig(kKind, {{"depthCents", 60.0}, {"mix", 1.0}});
    const Stereo out = rig.renderBoth(in);
    EXPECT_EQ(out.left, out.right);
  }
  {
    EffectRig rig(kKind, {{"depthCents", 60.0}, {"mix", 1.0}}, rigFor(Layout::DualMono));
    const Stereo out = rig.renderBoth(in);
    EXPECT_EQ(out.left, out.right);
  }
}

TEST(VibratoTest, StaysBoundedAtTheExtremes) {
  for (const Layout layout : {Layout::Mono, Layout::Stereo}) {
    DirectKind kind(kKind, {{"depthCents", 100.0}, {"rateHz", 0.5}, {"riseMs", 0.0}}, layout);
    const std::vector<float> hot = noise(5.0, 17u, 1.0f, kRate);
    const Stereo out = kind.render(Stereo{hot, hot});
    // A delay line has no gain: Hermite may overshoot a hard edge a little.
    expectCleanSignal(out.left, 1.5f, "extreme vibrato, left");
    expectCleanSignal(out.right, 1.5f, "extreme vibrato, right");
  }
}

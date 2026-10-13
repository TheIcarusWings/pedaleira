// modulation.vibe (plugin/src/effects/modulation/Vibe.cpp): the conformance
// test, then what a photocell phase shifter is for: four staggered all-pass
// stages whose corners sit where the lamp and cell model puts them, a sweep
// that is lopsided (quick to rise, slow to fall, more so the faster it runs),
// a speed that follows the stated rate (free and tempo-synced), notches in
// chorus mode and none in vibrato mode, and a right side behind the left in
// a true stereo image only.
//
// The corner of the first stage is read from the kind's `cornerHz` reading
// after every 32-sample block, which gives the sweep as a series.
#include "effects/effect_test_helpers.h"
#include "effects/modulation_test_helpers.h"

#include <gtest/gtest.h>

#include <array>

using namespace fxtest;
using namespace modtest;

namespace {

constexpr const char* kKind = "modulation.vibe";
constexpr int kBlock = 32;
constexpr double kSeriesRate = kRate / kBlock;

// The corner of the first stage over `seconds`, one value per block.
std::vector<double> cornerSeries(const Params& params, double seconds, double bpm = 120.0) {
  DirectKind kind(kKind, params, Layout::Mono, kBlock, bpm);
  std::vector<double> series;
  const std::vector<float> silence(static_cast<size_t>(kBlock), 0.0f);
  for (int block = 0; block < static_cast<int>(seconds * kSeriesRate); ++block) {
    kind.render(silence);
    series.push_back(kind.engine().getReading(0));
  }
  return series;
}

// Share of each cycle spent rising from its lowest corner to its highest,
// averaged over the whole cycles after `skipCycles`.
double riseShare(const std::vector<double>& series, double rateHz, int skipCycles, int cycles) {
  const double period = kSeriesRate / rateHz;
  double sum = 0.0;
  for (int c = skipCycles; c < skipCycles + cycles; ++c) {
    const size_t begin = static_cast<size_t>(std::lround(c * period));
    const size_t end = static_cast<size_t>(std::lround((c + 1) * period));
    size_t lo = begin, hi = begin;
    for (size_t i = begin; i < end; ++i) {
      if (series[i] < series[lo])
        lo = i;
      if (series[i] > series[hi])
        hi = i;
    }
    const double rise = hi >= lo ? static_cast<double>(hi - lo) : static_cast<double>(hi + static_cast<size_t>(period) - lo);
    sum += rise / period;
  }
  return sum / cycles;
}

// Corners of the four stages with the cell at conductance `g` (the kind's
// staggered corners and its tenfold sweep).
std::array<double, 4> cornersAt(double g) {
  const double scale = 1.0 + 9.0 * g;
  return {450.0 * scale, 250.0 * scale, 135.0 * scale, 72.0 * scale};
}

// |1 + chain| / sqrt(2) in dB, or |chain| for vibrato mode, at `hz`.
double expectedDb(const std::array<double, 4>& corners, double hz, bool chorus) {
  double phase = 0.0;
  for (const double corner : corners)
    phase += -2.0 * std::atan(std::tan(kPi * hz / kRate) / std::tan(kPi * corner / kRate));
  return chorus ? toDb(0.70710678 * 2.0 * std::abs(std::cos(0.5 * phase))) : 0.0;
}

}  // namespace

TEST(VibeTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"rateHz", 2.0}, {"intensity", 90.0}, {"width", 60.0}};
  options.stateParams = {{"sync", 1.0},        {"division", fx::kDivQuarterTriplet},
                         {"rateHz", 3.0},      {"intensity", 40.0},
                         {"mode", 1.0},        {"width", 80.0}};
  options.cpuAverageBudgetNs = 15000.0;
  options.cpuWorstBudgetNs = 200000.0;
  expectKindConforms("modulation.vibe", options);
}

TEST(VibeTest, RegisteredFullyWetWithItsReading) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kKind);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->category, BlockCategory::Modulation);
  EXPECT_EQ(info->mixLaw, BlockMixLaw::Crossfade);
  EXPECT_FLOAT_EQ(info->defaultMix, 1.0f);
  EXPECT_EQ(info->name, juce::String("Vibe"));
  ASSERT_EQ(info->readings.size(), 1u);
  EXPECT_EQ(info->readings[0].id, juce::String("cornerHz"));
}

TEST(VibeTest, StagesSitWhereTheLampAndCellModelPutThem) {
  // Intensity 0 leaves the lamp at half drive: light 0.25, and a cell
  // conductance of 0.25^0.7 = 0.379, so the corners are 4.41 times their dark
  // values. The first stage is read out directly...
  const std::vector<double> corner = cornerSeries({{"intensity", 0.0}, {"rateHz", 3.0}}, 0.5);
  EXPECT_NEAR(corner.back(), cornersAt(std::pow(0.25, 0.7))[0], 0.5);
  EXPECT_NEAR(corner.front(), corner.back(), 0.01) << "nothing moves at intensity 0";

  // ... and all four are heard in the notches of chorus mode: 1 + chain is
  // 0 where the four stages are 180 or 540 degrees behind.
  EffectRig rig(kKind, {{"intensity", 0.0}, {"mix", 1.0}});
  const std::vector<float> ir = captureImpulseResponse(rig, 0.4);
  const std::array<double, 4> corners = cornersAt(std::pow(0.25, 0.7));
  const std::vector<double> freqs = {120.0, 300.0, 700.0, 1500.0, 3000.0, 6000.0, 12000.0};
  const std::vector<double> measured = magnitudeResponseDb(ir, freqs, kRate);
  for (size_t i = 0; i < freqs.size(); ++i)
    EXPECT_NEAR(measured[i], expectedDb(corners, freqs[i], true), 0.35) << freqs[i];
  EXPECT_EQ(responseNotches(ir, 40.0, 20000.0, 5.0, 18.0).size(), 2u);
}

TEST(VibeTest, VibratoModeIsAllPassWithNoNotches) {
  EffectRig rig(kKind, {{"intensity", 0.0}, {"mode", 1.0}, {"mix", 1.0}});
  const std::vector<float> ir = captureImpulseResponse(rig, 0.4);
  for (const double hz : {80.0, 400.0, 1500.0, 5000.0, 12000.0})
    EXPECT_NEAR(magnitudeResponseDb(ir, {hz}, kRate).front(), 0.0, 0.15) << hz;
  EXPECT_TRUE(responseNotches(ir, 40.0, 20000.0, 5.0, 3.0).empty());
}

TEST(VibeTest, SweepRisesFasterThanItFallsAndMoreSoTheFasterItRuns) {
  // A cell that brightens in 6 ms and lets go over 50 ms peaks soon after
  // the lamp and bottoms out late: the rise takes less than half the cycle.
  const auto share = [](double rateHz) {
    const std::vector<double> series =
        cornerSeries({{"rateHz", rateHz}, {"intensity", 100.0}}, 12.0 / rateHz + 2.0);
    return riseShare(series, rateHz, 2, 8);
  };
  const double slow = share(0.3);
  const double medium = share(2.0);
  const double fast = share(5.0);
  EXPECT_NEAR(slow, 0.5, 0.03) << "slow enough to look symmetric";
  EXPECT_LT(medium, slow - 0.03);
  EXPECT_LT(fast, medium - 0.02);
  EXPECT_GT(fast, 0.2) << "still a sweep";
}

TEST(VibeTest, SweepReachesFurtherUpwardThanTheLampAloneWouldTake) {
  // Intensity 100 swings the lamp from dark to full: the first stage's
  // corner moves over most of its tenfold range at a slow speed, and
  // spends longer in the low half (the resistance curve is not linear).
  const std::vector<double> series = cornerSeries({{"rateHz", 0.5}, {"intensity", 100.0}}, 6.0);
  const double lowest = *std::min_element(series.begin() + 3 * 1500, series.end());
  const double highest = *std::max_element(series.begin() + 3 * 1500, series.end());
  EXPECT_NEAR(lowest, 450.0, 25.0);
  EXPECT_NEAR(highest, 4500.0, 150.0);
  int below = 0, total = 0;
  for (size_t i = 3 * 1500; i < series.size(); ++i, ++total)
    if (series[i] < 0.5 * (lowest + highest))
      ++below;
  EXPECT_GT(static_cast<double>(below) / total, 0.5);
}

TEST(VibeTest, IntensityScalesTheSweep) {
  const auto range = [](double intensity) {
    const std::vector<double> series = cornerSeries({{"rateHz", 0.5}, {"intensity", intensity}}, 6.0);
    const double lowest = *std::min_element(series.begin() + 3 * 1500, series.end());
    const double highest = *std::max_element(series.begin() + 3 * 1500, series.end());
    return highest / lowest;
  };
  EXPECT_NEAR(range(0.0), 1.0, 0.001);
  EXPECT_LT(range(25.0), range(50.0));
  EXPECT_LT(range(50.0), range(100.0));
  EXPECT_GT(range(100.0), 8.0);
}

TEST(VibeTest, SpeedRunsAtTheRateParameter) {
  for (const double rateHz : {0.5, 1.5, 4.0}) {
    SCOPED_TRACE(rateHz);
    const std::vector<double> series = cornerSeries({{"rateHz", rateHz}, {"intensity", 100.0}}, 10.0);
    EXPECT_NEAR(oscillationRateHz(std::vector<double>(series.begin() + 1500, series.end()), kSeriesRate, 0.1),
                rateHz, 0.03 * rateHz);
  }
}

TEST(VibeTest, SyncedSpeedFollowsTheTempoAtTwoTempos) {
  struct Case {
    double bpm;
    int division;
    double expectedHz;
  };
  for (const Case& c : {Case{120.0, fx::kDivQuarter, 2.0}, Case{90.0, fx::kDivQuarter, 1.5},
                        Case{90.0, fx::kDivEighth, 3.0}}) {
    SCOPED_TRACE(c.bpm);
    const std::vector<double> series =
        cornerSeries({{"sync", 1.0}, {"division", static_cast<double>(c.division)}, {"rateHz", 7.0},
                      {"intensity", 100.0}},
                     8.0, c.bpm);
    EXPECT_NEAR(oscillationRateHz(std::vector<double>(series.begin() + 1500, series.end()), kSeriesRate, 0.1),
                c.expectedHz, 0.03 * c.expectedHz);
  }
}

TEST(VibeTest, ChorusModeSweepsNotchesAndVibratoModeBendsThePitch) {
  // A 1 kHz sine at full intensity and 4 Hz. Chorus: the level swings as the
  // notches pass through it (twice a cycle). Vibrato: the level holds and the phase bends the
  // pitch.
  const std::vector<float> in = sine(6.0, 1000.0, 0.25f, kRate);
  const size_t skip = static_cast<size_t>(samples(0.5));
  {
    EffectRig rig(kKind, {{"rateHz", 4.0}, {"intensity", 100.0}, {"mode", 0.0}, {"mix", 1.0}});
    const std::vector<float> out = rig.render(in);
    // Two notches pass through the tone in every cycle of the sweep.
    EXPECT_NEAR(amplitudeModulationRateHz(out, kRate, skip, 0.2), 8.0, 0.4);
  }
  {
    EffectRig rig(kKind, {{"rateHz", 4.0}, {"intensity", 100.0}, {"mode", 1.0}, {"mix", 1.0}});
    const std::vector<float> out = rig.render(in);
    EXPECT_EQ(amplitudeModulationRateHz(out, kRate, skip, 0.05), 0.0) << "the level must hold";
    const Deviation d = deviationOf(cycleFrequencies(out, kRate, skip), 1000.0, 1.0, 5.5);
    EXPECT_GT(d.maxCents - d.minCents, 3.0) << "a phase shifter bends the pitch a little";
    EXPECT_NEAR(frequencyModulationRateHz(out, kRate, skip, 0.0005), 4.0, 0.2);
  }
}

TEST(VibeTest, SidesDifferOnlyInATrueStereoImage) {
  const std::vector<float> in = noise(2.0, 3u, 0.25f, kRate);
  {
    EffectRig rig(kKind, {{"width", 100.0}, {"intensity", 100.0}, {"rateHz", 2.0}, {"mix", 1.0}});
    const Stereo out = rig.renderBoth(in);
    EXPECT_GT(relativeDifferenceDb(out.left, out.right, static_cast<size_t>(samples(0.2))), -12.0)
        << "no stereo spread at full width";
  }
  {
    EffectRig rig(kKind, {{"width", 0.0}, {"mix", 1.0}});
    const Stereo out = rig.renderBoth(in);
    EXPECT_LT(maxDifference(out.left, out.right), 1.0e-6f) << "width 0 must be mono";
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

TEST(VibeTest, TailCoversTheRingOfTheChain) {
  for (const double mode : {0.0, 1.0}) {
    SCOPED_TRACE(mode);
    const TailMeasurement tail =
        expectSilenceAfterReportedTail(kKind, {{"intensity", 0.0}, {"mode", mode}});
    EXPECT_FALSE(tail.capped);
    EXPECT_GT(tail.measuredSeconds, 0.002);
  }
}

TEST(VibeTest, StaysBoundedAtTheExtremes) {
  for (const Layout layout : {Layout::Mono, Layout::Stereo}) {
    DirectKind kind(kKind, {{"rateHz", 10.0}, {"intensity", 100.0}, {"width", 100.0}}, layout);
    const std::vector<float> hot = noise(5.0, 17u, 1.0f, kRate);
    const Stereo out = kind.render(Stereo{hot, hot});
    expectCleanSignal(out.left, 4.0f, "extreme vibe, left");
    expectCleanSignal(out.right, 4.0f, "extreme vibe, right");
  }
}

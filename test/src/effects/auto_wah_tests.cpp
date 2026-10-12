// filter.autowah (plugin/src/effects/filter/AutoWahEffect.cpp): the
// conformance test, then what an auto wah is for: the sweep runs at the rate
// parameter (free, and locked to the tempo when synced), its depth and centre
// do what they say, and a stereo pair is swept as one.
#include "effects/dsp/TempoSync.h"
#include "effects/effect_test_helpers.h"

#include <gtest/gtest.h>

using namespace fxtest;

namespace {

constexpr const char* kKind = "filter.autowah";
constexpr double kRate = 48000.0;

// A steady 330 Hz tone sits below the lowest cutoff the sweep reaches at full
// depth around 1 kHz (353 Hz), so its level through the filter rises once per
// LFO cycle, when the cutoff comes down to meet it: one swell per cycle, at
// the LFO's own rate.
double sweepRateHz(const Params& params, double bpm = 120.0, double seconds = 5.0) {
  RigOptions options;
  options.bpm = bpm;
  EffectRig rig(kKind, params, options);
  const std::vector<float> out = rig.render(sine(seconds, 330.0, 0.25f, kRate));
  return amplitudeModulationRateHz(out, kRate, static_cast<size_t>(0.5 * kRate));
}

Params sweepParams(double rateHz) {
  return {{"rateHz", rateHz}, {"depthPct", 100.0}, {"centreHz", 1000.0}, {"resonancePct", 50.0},
          {"mix", 1.0}};
}

// How far the level of a tone swings through the sweep: trough to peak over
// mean of the smoothed envelope, in a window.
double swingOf(const std::vector<float>& out, size_t from) {
  const double coeff = 1.0 - std::exp(-2.0 * kPi * 40.0 / kRate);
  double first = 0.0, second = 0.0, lo = 1e9, hi = 0.0, sum = 0.0;
  size_t n = 0;
  for (size_t i = 0; i < out.size(); ++i) {
    first += coeff * (std::abs(static_cast<double>(out[i])) - first);
    second += coeff * (first - second);
    if (i >= from) {
      lo = std::min(lo, second);
      hi = std::max(hi, second);
      sum += second;
      ++n;
    }
  }
  return (hi - lo) / (sum / static_cast<double>(n));
}

}  // namespace

TEST(AutoWahEffectTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"rateHz", 3.0}, {"depthPct", 90.0}, {"resonancePct", 70.0}};
  options.stateParams = {{"sync", 1.0},          {"division", fx::kDivEighthTriplet},
                         {"rateHz", 4.5},        {"depthPct", 35.0},
                         {"centreHz", 1500.0},   {"resonancePct", 20.0},
                         {"shape", 1.0},         {"levelDb", -4.0}};
  options.cpuAverageBudgetNs = 3000.0;
  options.cpuWorstBudgetNs = 150000.0;
  expectKindConforms("filter.autowah", options);
}

TEST(AutoWahEffectTest, SweepRateMatchesTheRateParameter) {
  for (const double rate : {0.8, 1.5, 3.0}) {
    SCOPED_TRACE(rate);
    EXPECT_NEAR(sweepRateHz(sweepParams(rate)), rate, 0.05 * rate);
  }
  // And with the triangle shape.
  Params triangle = sweepParams(2.0);
  triangle.emplace_back("shape", 1.0);
  EXPECT_NEAR(sweepRateHz(triangle), 2.0, 0.1);
}

TEST(AutoWahEffectTest, SyncedRateFollowsTheTempoAndTheDivision) {
  struct Case {
    double bpm;
    int division;
    double expectedHz;
  };
  // A quarter note at 120 bpm is 2 Hz; at 90 bpm 1.5 Hz; an eighth at 120
  // is 4 Hz; a dotted quarter at 120 is 1.333 Hz.
  for (const Case& c : {Case{120.0, fx::kDivQuarter, 2.0}, Case{90.0, fx::kDivQuarter, 1.5},
                        Case{120.0, fx::kDivEighth, 4.0}, Case{120.0, fx::kDivQuarterDotted, 4.0 / 3.0}}) {
    SCOPED_TRACE(c.bpm);
    Params params = sweepParams(7.0);  // the free rate must be ignored
    params.emplace_back("sync", 1.0);
    params.emplace_back("division", static_cast<double>(c.division));
    EXPECT_NEAR(sweepRateHz(params, c.bpm), c.expectedHz, 0.05 * c.expectedHz);
  }
}

TEST(AutoWahEffectTest, DepthSetsHowFarTheSweepTravelsAndZeroHoldsTheCentre) {
  RigOptions options;
  double previous = -1.0;
  for (const double depth : {0.0, 25.0, 60.0, 100.0}) {
    EffectRig rig(kKind, {{"rateHz", 2.0}, {"depthPct", depth}, {"centreHz", 1000.0}, {"mix", 1.0}}, options);
    const double swing = swingOf(rig.render(sine(4.0, 330.0, 0.25f, kRate)), static_cast<size_t>(0.5 * kRate));
    if (depth == 0.0)
      EXPECT_LT(swing, 0.02) << "no depth, no sweep";
    EXPECT_GT(swing, previous - 1.0e-9) << depth;
    previous = swing;
  }
  EXPECT_GT(previous, 0.5);
}

TEST(AutoWahEffectTest, WithNoDepthTheCentreIsWhereTheFilterSits) {
  for (const double centre : {400.0, 900.0, 2000.0}) {
    SCOPED_TRACE(centre);
    DirectKind kind(kKind, {{"depthPct", 0.0}, {"centreHz", centre}});
    const std::vector<float> ir = kind.render(impulse(24000));
    std::vector<double> grid;
    for (double hz = 100.0; hz <= 6000.0; hz *= std::pow(2.0, 1.0 / 48.0))
      grid.push_back(hz);
    const std::vector<double> response = magnitudeResponseDb(ir, grid, kRate);
    size_t best = 0;
    for (size_t i = 0; i < grid.size(); ++i)
      if (response[i] > response[best])
        best = i;
    EXPECT_NEAR(std::log2(grid[best] / centre), 0.0, 0.05);
  }
}

TEST(AutoWahEffectTest, TheSweepIsOneForBothSidesOfAStereoPair) {
  // The same swept filter on both channels of a true stereo buffer, so the
  // image stays put; in dual mono each side equals a mono rig (conformance).
  DirectKind kind(kKind, {{"rateHz", 2.0}, {"depthPct", 100.0}}, Layout::Stereo);
  const std::vector<float> tone = sine(1.0, 700.0, 0.25f, kRate);
  const Stereo out = kind.render(Stereo{tone, tone});
  EXPECT_EQ(out.left, out.right);
}

TEST(AutoWahEffectTest, TailFollowsTheLowestCutoffOfTheSweep) {
  EffectRig rig(kKind, {{"resonancePct", 100.0}, {"depthPct", 0.0}, {"centreHz", 1000.0}});
  const double shallow = rig.tailSeconds();
  rig.set("depthPct", 100.0);
  EXPECT_GT(rig.tailSeconds(), 1.5 * shallow) << "the sweep reaches lower and rings longer";
  EXPECT_EQ(rig.latencySamples(), 0);
}

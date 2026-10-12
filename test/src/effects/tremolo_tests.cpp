// modulation.tremolo (plugin/src/effects/modulation/Tremolo.cpp): the
// conformance test, then what a tremolo is for: a gain that swings between 1
// and 1 - depth at the stated rate (free and tempo-synced) in the stated
// shape, square edges that ramp instead of click, and, in a true stereo
// image only, a right side that runs out of phase with the left.
//
// The gain itself is measured with a steady 1.0 as input on the kind alone
// (DirectKind has no DC blocker around it), so what comes out is the gain.
#include "effects/effect_test_helpers.h"
#include "effects/modulation_test_helpers.h"

#include <gtest/gtest.h>

using namespace fxtest;
using namespace modtest;

namespace {

constexpr const char* kKind = "modulation.tremolo";

std::vector<float> steady(double seconds) {
  return std::vector<float>(static_cast<size_t>(samples(seconds)), 1.0f);
}

// The gain of the kind alone over `seconds`.
std::vector<float> gainOf(const Params& params, double seconds, Layout layout = Layout::Mono) {
  DirectKind kind(kKind, params, layout);
  return kind.render(Stereo{steady(seconds), steady(seconds)}).left;
}

// Rate of the amplitude modulation of a 1 kHz sine.
double measuredRate(Params params, double bpm = 120.0) {
  params.emplace_back("mix", 1.0);
  RigOptions options;
  options.bpm = bpm;
  EffectRig rig(kKind, params, options);
  const std::vector<float> out = rig.render(sine(8.0, 1000.0, 0.25f, kRate));
  return amplitudeModulationRateHz(out, kRate, static_cast<size_t>(samples(0.5)));
}

}  // namespace

TEST(TremoloTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"depth", 80.0}, {"rateHz", 4.0}, {"shape", 1.0}, {"pan", 60.0}};
  options.stateParams = {{"sync", 1.0},   {"division", fx::kDivEighth}, {"rateHz", 3.3},
                         {"depth", 40.0}, {"shape", 2.0},               {"pan", 70.0}};
  options.cpuAverageBudgetNs = 3000.0;
  // About 50x the average: one block in thousands can be descheduled on a
  // busy machine, and that says nothing about the effect.
  options.cpuWorstBudgetNs = 150000.0;
  expectKindConforms("modulation.tremolo", options);
}

TEST(TremoloTest, RegisteredFullyWet) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kKind);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->category, BlockCategory::Modulation);
  EXPECT_FLOAT_EQ(info->defaultMix, 1.0f);
  EXPECT_EQ(info->mixLaw, BlockMixLaw::Crossfade);
}

TEST(TremoloTest, ModulationRunsAtTheRateParameterInEveryShape) {
  for (const double shape : {0.0, 1.0, 2.0})
    for (const double rateHz : {1.0, 3.0, 12.0}) {
      SCOPED_TRACE(shape);
      SCOPED_TRACE(rateHz);
      EXPECT_NEAR(measuredRate({{"depth", 80.0}, {"shape", shape}, {"rateHz", rateHz}}), rateHz,
                  0.03 * rateHz);
    }
}

TEST(TremoloTest, SyncedRateFollowsTheTempoAtTwoTempos) {
  struct Case {
    double bpm;
    int division;
    double expectedHz;
  };
  for (const Case& c : {Case{120.0, fx::kDivQuarter, 2.0}, Case{90.0, fx::kDivQuarter, 1.5},
                        Case{90.0, fx::kDivEighth, 3.0}, Case{140.0, fx::kDivEighthTriplet, 7.0}}) {
    SCOPED_TRACE(c.bpm);
    EXPECT_NEAR(measuredRate({{"depth", 80.0}, {"sync", 1.0},
                              {"division", static_cast<double>(c.division)}, {"rateHz", 9.0}},
                             c.bpm),
                c.expectedHz, 0.03 * c.expectedHz);
  }
}

TEST(TremoloTest, GainSwingsBetweenOneAndOneMinusDepth) {
  for (const double shape : {0.0, 1.0, 2.0})
    for (const double depth : {25.0, 60.0, 100.0}) {
      SCOPED_TRACE(shape);
      SCOPED_TRACE(depth);
      const std::vector<float> gain = gainOf({{"depth", depth}, {"shape", shape}, {"rateHz", 5.0}}, 1.0);
      float lowest = 2.0f, highest = -1.0f;
      for (size_t i = static_cast<size_t>(samples(0.1)); i < gain.size(); ++i) {
        lowest = std::min(lowest, gain[i]);
        highest = std::max(highest, gain[i]);
      }
      EXPECT_NEAR(highest, 1.0f, 0.005f);
      EXPECT_NEAR(lowest, 1.0f - static_cast<float>(depth) * 0.01f, 0.01f);
    }
}

TEST(TremoloTest, DepthZeroPassesTheSignalUntouched) {
  const std::vector<float> in = noise(1.0, 3u, 0.25f, kRate);
  for (const double shape : {0.0, 1.0, 2.0}) {
    DirectKind kind(kKind, {{"depth", 0.0}, {"shape", shape}, {"rateHz", 9.0}});
    EXPECT_EQ(kind.render(in), in) << shape;
  }
  // And through the rig, a mix at 100% is the dry rig.
  EffectRig rig(kKind, {{"depth", 0.0}, {"mix", 1.0}});
  EffectRig reference{juce::String()};
  EXPECT_LT(maxDifference(rig.render(in), reference.render(in)), 1.0e-6f);
}

TEST(TremoloTest, SineAndTriangleFollowTheirFormula) {
  const double rateHz = 5.0, depth = 0.7;
  for (const double shape : {0.0, 1.0}) {
    SCOPED_TRACE(shape);
    const std::vector<float> gain =
        gainOf({{"depth", depth * 100.0}, {"shape", shape}, {"rateHz", rateHz}}, 0.8);
    std::vector<float> expected(gain.size());
    for (size_t i = 0; i < gain.size(); ++i) {
      const double phase = rateHz * static_cast<double>(i) / kRate;
      double wave;
      if (shape == 0.0) {
        wave = std::sin(2.0 * kPi * phase);
      } else {
        double q = phase + 0.25;
        q -= std::floor(q);
        wave = 1.0 - 4.0 * std::abs(q - 0.5);
      }
      expected[i] = static_cast<float>(1.0 - depth * 0.5 * (1.0 - wave));
    }
    // The two quick stages that keep a shape change from clicking lag the
    // wave by 0.2 ms.
    EXPECT_LT(maxDifference(gain, expected), 0.005f);
  }
}

TEST(TremoloTest, SquareHasEvenHalvesAndEdgesThatRamp) {
  const double rateHz = 4.0, depth = 1.0;
  const std::vector<float> gain =
      gainOf({{"depth", depth * 100.0}, {"shape", 2.0}, {"rateHz", rateHz}}, 2.0);
  // Half the time at the top, half at the bottom (plateaus judged at 10% of
  // the swing: an edge takes about 3.9 ms of each half to leave or reach
  // them, so 0.486 of the time each).
  size_t high = 0, low = 0;
  for (size_t i = static_cast<size_t>(samples(0.25)); i < gain.size(); ++i) {
    high += gain[i] > 0.9f;
    low += gain[i] < 0.1f;
  }
  const double total = static_cast<double>(gain.size() - static_cast<size_t>(samples(0.25)));
  EXPECT_NEAR(static_cast<double>(high) / total, 0.486, 0.01);
  EXPECT_NEAR(static_cast<double>(low) / total, 0.486, 0.01);

  // No step between samples above 1% of the swing (an edge is a ramp of a
  // few milliseconds, not a click), and there are edges at all (above 0.4%).
  const float step = maxStep(gain, 1, gain.size());
  EXPECT_LE(step, 0.01f * static_cast<float>(depth));
  EXPECT_GT(step, 0.004f);

  // A rising edge takes between 2 and 6 ms from 10% to 90% of the swing.
  size_t from = 0;
  while (from < gain.size() && gain[from] < 0.5f)  // wait for the first low half
    ++from;
  size_t lowAt = from;
  while (lowAt < gain.size() && gain[lowAt] > 0.1f)
    ++lowAt;
  size_t tenAt = lowAt;
  while (tenAt < gain.size() && gain[tenAt] < 0.1f + 1.0e-6f)
    ++tenAt;
  size_t ninetyAt = tenAt;
  while (ninetyAt < gain.size() && gain[ninetyAt] < 0.9f)
    ++ninetyAt;
  ASSERT_LT(ninetyAt, gain.size());
  const double riseMs = 1000.0 * static_cast<double>(ninetyAt - tenAt) / kRate;
  EXPECT_GT(riseMs, 2.0);
  EXPECT_LT(riseMs, 6.0);
}

TEST(TremoloTest, ChangingTheShapeDoesNotClick) {
  DirectKind kind(kKind, {{"depth", 100.0}, {"rateHz", 3.0}});
  std::vector<float> out;
  for (const double shape : {0.0, 2.0, 1.0, 0.0, 1.0}) {
    setKindParam(kind, "shape", shape);
    const std::vector<float> part = kind.render(steady(0.37));
    out.insert(out.end(), part.begin(), part.end());
  }
  // The jump in the target is at most a fifth of the swing, and the soft
  // shapes take it through two quick stages.
  EXPECT_LT(maxStep(out, 1, out.size()), 0.05f);
}

TEST(TremoloTest, PanRunsTheSidesOutOfPhaseInATrueStereoImage) {
  const auto render = [](double pan, Layout layout = Layout::Stereo) {
    DirectKind kind(kKind, {{"depth", 100.0}, {"rateHz", 4.0}, {"pan", pan}}, layout);
    return kind.render(Stereo{steady(1.0), steady(1.0)});
  };
  {
    // Opposite phase: when one side is down the other is up, and the two
    // gains always add up to 2 - depth.
    const Stereo out = render(100.0);
    float sumError = 0.0f, spread = 0.0f;
    for (size_t i = static_cast<size_t>(samples(0.1)); i < out.size(); ++i) {
      sumError = std::max(sumError, std::abs(out.left[i] + out.right[i] - 1.0f));
      spread = std::max(spread, std::abs(out.left[i] - out.right[i]));
    }
    EXPECT_LT(sumError, 0.01f);
    EXPECT_GT(spread, 0.9f);
  }
  {
    // Quarter of the way: a quarter turn apart, the sides differ but do not
    // mirror each other.
    const Stereo out = render(50.0);
    const double difference = relativeDifferenceDb(out.left, out.right, static_cast<size_t>(samples(0.1)));
    EXPECT_GT(difference, -12.0);
    EXPECT_GT(maxDifference(out.left, out.right), 0.3f);
  }
  {
    const Stereo out = render(0.0);
    EXPECT_EQ(out.left, out.right) << "pan 0 is a plain tremolo on both sides";
  }
  // Nowhere else is there a stereo image to pan: one channel, two
  // independent channels, and a stereo buffer in front of an amp model all
  // run the plain tremolo on every side.
  const std::vector<float> plain = render(0.0).left;
  EXPECT_EQ(render(100.0, Layout::Mono).left, plain);
  const Stereo dual = render(100.0, Layout::DualMono);
  EXPECT_EQ(dual.left, plain);
  EXPECT_EQ(dual.right, plain);
  DirectKind beforeAmp(kKind, {{"depth", 100.0}, {"rateHz", 4.0}, {"pan", 100.0}}, Layout::Stereo);
  beforeAmp.ctx.nam.lastNamIndex = 2;
  const Stereo lost = beforeAmp.render(Stereo{steady(1.0), steady(1.0)});
  EXPECT_EQ(lost.left, plain);
  EXPECT_EQ(lost.right, plain);
}

TEST(TremoloTest, DualMonoNeverCrossFeeds) {
  DirectKind kind(kKind, {{"depth", 100.0}, {"rateHz", 4.0}, {"pan", 100.0}}, Layout::DualMono);
  const std::vector<float> silence(static_cast<size_t>(samples(1.0)), 0.0f);
  const Stereo out = kind.render(Stereo{noise(1.0, 3u, 0.25f, kRate), silence});
  EXPECT_GT(peakOf(out.left), 0.05f);
  EXPECT_EQ(peakOf(out.right), 0.0f);
}

TEST(TremoloTest, NeverLouderThanTheInputAtTheExtremes) {
  for (const Layout layout : {Layout::Mono, Layout::Stereo, Layout::DualMono}) {
    DirectKind kind(kKind, {{"depth", 100.0}, {"rateHz", 20.0}, {"shape", 2.0}, {"pan", 100.0}},
                    layout);
    const std::vector<float> hot = noise(3.0, 7u, 1.0f, kRate);
    const Stereo out = kind.render(Stereo{hot, hot});
    expectCleanSignal(out.left, 1.0f, "extreme tremolo, left");
    expectCleanSignal(out.right, 1.0f, "extreme tremolo, right");
  }
}

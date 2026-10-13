// modulation.dualchorus (plugin/src/effects/modulation/DualChorus.cpp): the
// conformance test, then what two independent chorus voices are for: each at
// its own delay, rate (free and tempo-synced) and depth in cents, spread
// across a true stereo image by gains alone so that the sides add back to
// the same two voices.
//
// The voices are measured one at a time through the spread: at 100% the left
// side is voice A alone and the right side voice B alone.
#include "effects/effect_test_helpers.h"
#include "effects/modulation_test_helpers.h"

#include <gtest/gtest.h>

using namespace fxtest;
using namespace modtest;

namespace {

constexpr const char* kKind = "modulation.dualchorus";

// Params that isolate the voices and open the high cut.
Params isolated(Params extra) {
  Params params = {{"spread", 100.0}, {"toneHz", 20000.0}, {"mix", 1.0}};
  params.insert(params.end(), extra.begin(), extra.end());
  return params;
}

// Peak-to-peak and mean pitch of a 1 kHz sine, per side, in cents.
struct VoicePitch {
  double swingCents{0.0};
  double rateHz{0.0};
};

VoicePitch measure(const std::vector<float>& side) {
  VoicePitch result;
  const CycleFrequencies f = cycleFrequencies(side, kRate, static_cast<size_t>(samples(0.5)));
  const Deviation d = deviationOf(f, 1000.0, 0.8, 7.5);
  result.swingCents = d.maxCents - d.minCents;
  result.rateHz = frequencyModulationRateHz(side, kRate, static_cast<size_t>(samples(0.5)), 0.0005);
  return result;
}

}  // namespace

TEST(DualChorusTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"aDepth", 80.0}, {"bDepth", 60.0}, {"aRateHz", 1.5}, {"bRateHz", 2.3},
                    {"spread", 80.0}};
  options.stateParams = {{"aSync", 1.0},      {"aDivision", fx::kDivEighthDotted},
                         {"aRateHz", 2.2},    {"aDepth", 33.0},
                         {"aDelayMs", 9.0},   {"bSync", 1.0},
                         {"bDivision", fx::kDivQuarterTriplet}, {"bRateHz", 3.1},
                         {"bDepth", 44.0},    {"bDelayMs", 22.0},
                         {"spread", 40.0},    {"toneHz", 5000.0}};
  // The wet is the voices alone, each a delay of 1 ms or more: the effect,
  // not latency. The kind reports none.
  options.latency = LatencyCheck::AtMostOnset;
  options.cpuAverageBudgetNs = 20000.0;
  options.cpuWorstBudgetNs = 300000.0;
  expectKindConforms("modulation.dualchorus", options);
}

TEST(DualChorusTest, RegisteredAsACrossfadeAtFiftyPercent) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kKind);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->category, BlockCategory::Modulation);
  EXPECT_EQ(info->mixLaw, BlockMixLaw::Crossfade);
  EXPECT_FLOAT_EQ(info->defaultMix, 0.5f);
  EXPECT_EQ(info->name, juce::String("Dual Chorus"));
}

TEST(DualChorusTest, EachVoiceSitsAtItsOwnDelay) {
  // Depth 0 freezes the voices. Spread 100 puts A on the left and B on the
  // right, so each side's impulse response is one voice.
  EffectRig rig(kKind, isolated({{"aDepth", 0.0}, {"bDepth", 0.0}, {"aDelayMs", 8.0},
                                 {"bDelayMs", 21.0}}));
  EffectRig reference{juce::String()};
  const int origin = peakIndexOf(captureImpulseResponse(reference, 0.05));
  rig.restart();
  const Stereo ir = rig.renderBoth(impulse(samples(0.1)));
  EXPECT_NEAR(peakIndexOf(ir.left) - origin, ms(8.0), 2);
  EXPECT_NEAR(peakIndexOf(ir.right) - origin, ms(21.0), 2);
  // One voice at unity per side, less the sliver of the other and the 20 kHz
  // high cut.
  EXPECT_NEAR(peakOf(ir.left), 1.0f, 0.12f);
  EXPECT_NEAR(peakOf(ir.right), 1.0f, 0.12f);
  EXPECT_LT(peakOf(ir.left, static_cast<size_t>(ms(10.0))), 0.02f) << "no B voice on the left";
  EXPECT_LT(peakOf(ir.right, 0, static_cast<size_t>(ms(19.0))), 0.02f) << "no A voice on the right";
}

TEST(DualChorusTest, VoicesRunAtTheirOwnRates) {
  // A at 1 Hz and B at 3 Hz, side by side in one render.
  EffectRig rig(kKind, isolated({{"aRateHz", 1.0}, {"bRateHz", 3.0}, {"aDepth", 100.0},
                                 {"bDepth", 100.0}, {"aDelayMs", 9.0}, {"bDelayMs", 19.0}}));
  const Stereo out = rig.renderBoth(sine(8.0, 1000.0, 0.25f, kRate));
  EXPECT_NEAR(measure(out.left).rateHz, 1.0, 0.04);
  EXPECT_NEAR(measure(out.right).rateHz, 3.0, 0.1);
}

TEST(DualChorusTest, SyncedRatesFollowTheTempoAtTwoTempos) {
  struct Case {
    double bpm;
    int divisionA;
    double expectedA;
    int divisionB;
    double expectedB;
  };
  for (const Case& c : {Case{120.0, fx::kDivQuarter, 2.0, fx::kDivEighth, 4.0},
                        Case{90.0, fx::kDivQuarter, 1.5, fx::kDivEighth, 3.0}}) {
    SCOPED_TRACE(c.bpm);
    RigOptions options;
    options.bpm = c.bpm;
    EffectRig rig(kKind,
                  isolated({{"aSync", 1.0}, {"aDivision", static_cast<double>(c.divisionA)},
                            {"aRateHz", 7.0}, {"bSync", 1.0},
                            {"bDivision", static_cast<double>(c.divisionB)}, {"bRateHz", 9.0},
                            {"aDepth", 100.0}, {"bDepth", 100.0}}),
                  options);
    const Stereo out = rig.renderBoth(sine(8.0, 1000.0, 0.25f, kRate));
    EXPECT_NEAR(measure(out.left).rateHz, c.expectedA, 0.04 * c.expectedA);
    EXPECT_NEAR(measure(out.right).rateHz, c.expectedB, 0.04 * c.expectedB);
  }
}

TEST(DualChorusTest, DepthIsAPitchSwingInCentsAtEachVoice) {
  // A delay swinging by +-s bends the pitch by up to 2 pi r s: at 1 Hz and
  // 3 ms per 100% depth, 32 cents either side, 65 peak to peak. Voice B is
  // set differently each time and must not matter.
  for (const double depth : {25.0, 50.0, 100.0}) {
    SCOPED_TRACE(depth);
    EffectRig rig(kKind, isolated({{"aRateHz", 1.0}, {"bRateHz", 1.0}, {"aDepth", depth},
                                   {"bDepth", 100.0 - depth}, {"aDelayMs", 12.0},
                                   {"bDelayMs", 25.0}}));
    const Stereo out = rig.renderBoth(sine(8.0, 1000.0, 0.25f, kRate));
    const double peak = 1200.0 * std::log2(1.0 + 2.0 * kPi * 1.0 * 0.003 * depth * 0.01);
    EXPECT_NEAR(measure(out.left).swingCents, 2.0 * peak, 0.1 * 2.0 * peak + 0.5);
    const double peakB = 1200.0 * std::log2(1.0 + 2.0 * kPi * 1.0 * 0.003 * (100.0 - depth) * 0.01);
    EXPECT_NEAR(measure(out.right).swingCents, 2.0 * peakB, 0.1 * 2.0 * peakB + 0.5);
  }
}

TEST(DualChorusTest, SpreadMovesOnlyGainsSoTheSidesAddBackToTheSameVoices) {
  const std::vector<float> in = noise(3.0, 7u, 0.25f, kRate);
  const auto sumOf = [&](double spread) {
    EffectRig rig(kKind, {{"spread", spread}, {"mix", 1.0}, {"toneHz", 20000.0}});
    const Stereo out = rig.renderBoth(in);
    std::vector<float> sum(out.size());
    for (size_t i = 0; i < sum.size(); ++i)
      sum[i] = out.left[i] + out.right[i];
    return std::make_pair(out, sum);
  };
  const auto centred = sumOf(0.0);
  const auto wide = sumOf(100.0);
  // Centred, both sides are 0.707 (A + B), so the sum is 1.414 (A + B); wide,
  // the sides are A and B, so the sum is A + B: the same waveform.
  float worst = 0.0f;
  for (size_t i = static_cast<size_t>(samples(0.2)); i < in.size(); ++i)
    worst = std::max(worst, std::abs(centred.second[i] - 1.41421356f * wide.second[i]));
  EXPECT_LT(worst, 2.0e-4f) << "the mono sum must not depend on the spread beyond a gain";
  // And the spread is audible: wide sides differ, centred sides do not.
  EXPECT_LT(maxDifference(centred.first.left, centred.first.right), 1.0e-6f);
  EXPECT_GT(relativeDifferenceDb(wide.first.left, wide.first.right, static_cast<size_t>(samples(0.2))),
            -6.0);
  // Each side keeps its level as the spread moves (the voices are
  // decorrelated).
  EXPECT_NEAR(toDb(rmsOf(wide.first.left, static_cast<size_t>(samples(0.2)))),
              toDb(rmsOf(centred.first.left, static_cast<size_t>(samples(0.2)))), 1.2);
}

TEST(DualChorusTest, MonoSumHasNoCombFilteringFromTheSpread) {
  // Mono fold of the wide pair against the centred one, as a spectrum: no
  // band of it is more than a hair different from the centred sound.
  const std::vector<float> in = noise(6.0, 9u, 0.25f, kRate);
  const auto folded = [&](double spread) {
    EffectRig rig(kKind, {{"spread", spread}, {"mix", 1.0}, {"toneHz", 20000.0}});
    const Stereo out = rig.renderBoth(in);
    std::vector<float> sum(out.size());
    for (size_t i = 0; i < sum.size(); ++i)
      sum[i] = 0.5f * (out.left[i] + out.right[i]);
    return sum;
  };
  const std::vector<float> wide = folded(100.0);
  const std::vector<float> centred = folded(0.0);
  for (const double hz : {200.0, 800.0, 3000.0, 9000.0}) {
    const size_t skip = static_cast<size_t>(samples(0.5));
    const double a = db(goertzelPower(wide.data() + skip, wide.size() - skip, hz, kRate));
    const double b = db(goertzelPower(centred.data() + skip, centred.size() - skip, hz, kRate));
    EXPECT_NEAR(a - b, -3.0, 0.5) << hz;
  }
}

TEST(DualChorusTest, SidesDifferOnlyInATrueStereoImage) {
  const std::vector<float> in = noise(2.0, 3u, 0.25f, kRate);
  {
    EffectRig rig(kKind, {{"spread", 100.0}, {"mix", 1.0}}, rigFor(Layout::DualMono));
    const Stereo out = rig.renderBoth(in);
    EXPECT_LT(maxDifference(out.left, out.right), 1.0e-6f) << "dual mono must not be spread";
  }
  {
    // An amp model later in the lane throws the image away: both sides get
    // the same centred pair.
    DirectKind kind(kKind, {{"spread", 100.0}}, Layout::Stereo);
    kind.ctx.nam.lastNamIndex = 3;
    const Stereo out = kind.render(Stereo{in, in});
    EXPECT_LT(maxDifference(out.left, out.right), 1.0e-6f);
  }
  {
    EffectRig rig(kKind, {{"spread", 0.0}, {"mix", 1.0}});
    const Stereo out = rig.renderBoth(in);
    EXPECT_LT(maxDifference(out.left, out.right), 1.0e-6f) << "spread 0 must be centred";
  }
}

TEST(DualChorusTest, DepthZeroOnBothVoicesIsAStaticFilterWhateverTheRates) {
  const std::vector<float> in = noise(2.0, 5u, 0.25f, kRate);
  const auto render = [&](double rateHz) {
    EffectRig rig(kKind, {{"aDepth", 0.0}, {"bDepth", 0.0}, {"aRateHz", rateHz},
                          {"bRateHz", rateHz * 0.7}, {"mix", 1.0}});
    return rig.render(in);
  };
  EXPECT_LT(maxDifference(render(0.2), render(9.0)), 1.0e-6f);
}

TEST(DualChorusTest, ToneIsAHighCutOnTheWet) {
  const auto gainAt8k = [](double toneHz) {
    EffectRig rig(kKind, {{"aDepth", 0.0}, {"bDepth", 0.0}, {"toneHz", toneHz}, {"mix", 1.0}});
    return sineProbeGainDb(rig, 8000.0);
  };
  EXPECT_LT(gainAt8k(1000.0), gainAt8k(20000.0) - 14.0);
}

TEST(DualChorusTest, StaysBoundedAtTheExtremes) {
  for (const Layout layout : {Layout::Mono, Layout::Stereo}) {
    DirectKind kind(kKind,
                    {{"aDepth", 100.0}, {"bDepth", 100.0}, {"aRateHz", 10.0}, {"bRateHz", 9.0},
                     {"aDelayMs", 4.0}, {"bDelayMs", 30.0}, {"toneHz", 20000.0}},
                    layout);
    const std::vector<float> hot = noise(5.0, 17u, 1.0f, kRate);
    const Stereo out = kind.render(Stereo{hot, hot});
    expectCleanSignal(out.left, 3.0f, "extreme dual chorus, left");
    expectCleanSignal(out.right, 3.0f, "extreme dual chorus, right");
  }
}

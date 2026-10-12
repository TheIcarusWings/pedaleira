// modulation.chorus (plugin/src/effects/modulation/Chorus.cpp): the
// conformance test, then what a chorus is for: voices at their delays, a
// pitch wobble at the stated rate (free and tempo-synced), nothing moving at
// depth 0, a tone control on the wet, and a stereo spread only where there
// is a stereo image.
#include "effects/effect_test_helpers.h"
#include "effects/modulation_test_helpers.h"

#include <gtest/gtest.h>

using namespace fxtest;
using namespace modtest;

namespace {

constexpr const char* kKind = "modulation.chorus";

// Pitch-wobble rate of the wet. The carrier is 400 Hz: the two voices are
// 5 ms apart, exactly two cycles of it, so they add in phase at rest, and at
// 20% depth they never swing far enough apart to cancel (a higher carrier or
// a deeper sweep would drive the sum through nulls and the zero crossings
// would no longer follow one pitch).
double measuredRate(Params params, double bpm = 120.0) {
  params.emplace_back("mix", 1.0);
  params.emplace_back("depth", 20.0);
  params.emplace_back("toneHz", 20000.0);
  RigOptions options;
  options.bpm = bpm;
  EffectRig rig(kKind, params, options);
  const std::vector<float> out = rig.render(sine(8.0, 400.0, 0.25f, kRate));
  return frequencyModulationRateHz(out, kRate, static_cast<size_t>(samples(0.5)));
}

}  // namespace

TEST(ChorusTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"depth", 80.0}, {"rateHz", 1.5}};
  options.stateParams = {{"sync", 1.0},      {"division", fx::kDivEighthDotted},
                         {"rateHz", 2.2},    {"depth", 33.0},
                         {"voices", 1.0},    {"toneHz", 5000.0},
                         {"width", 40.0}};
  // The wet is the voices alone, each a delay of 7 ms or more: the effect,
  // not latency. The kind reports none.
  options.latency = LatencyCheck::AtMostOnset;
  options.cpuAverageBudgetNs = 12000.0;
  // About 50x the average: one block in thousands can be descheduled on a
  // busy machine, and that says nothing about the effect.
  options.cpuWorstBudgetNs = 150000.0;
  expectKindConforms("modulation.chorus", options);
}

TEST(ChorusTest, RegisteredAsACrossfadeAtFiftyPercent) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kKind);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->category, BlockCategory::Modulation);
  EXPECT_EQ(info->mixLaw, BlockMixLaw::Crossfade);
  EXPECT_FLOAT_EQ(info->defaultMix, 0.5f);
}

TEST(ChorusTest, VoicesSitAtTheirDelaysWhenNothingMoves) {
  // Depth 0 freezes the voices at 10 and 15 ms (the third at 21 ms).
  EffectRig rig(kKind, {{"depth", 0.0}, {"toneHz", 20000.0}, {"mix", 1.0}});
  EffectRig reference{juce::String()};
  const int origin = peakIndexOf(captureImpulseResponse(reference, 0.05));
  const std::vector<float> two = captureImpulseResponse(rig, 0.1);
  EXPECT_NEAR(onsetIndex(two, 0.3f) - origin, ms(10.0), 2);
  const int first = peakIndexOf(std::vector<float>(two.begin(), two.begin() + ms(12.0)));
  EXPECT_NEAR(first - origin, ms(10.0), 2);
  const std::vector<float> secondHalf(two.begin() + ms(12.0), two.end());
  EXPECT_NEAR(peakIndexOf(secondHalf) + ms(12.0) - origin, ms(15.0), 2);
  // Two voices at 1 / sqrt(2) each, less the 20 kHz high cut's share.
  EXPECT_NEAR(peakOf(two), 0.66f, 0.08f);
  EXPECT_LT(peakOf(two, static_cast<size_t>(ms(16.0))), 0.02f) << "no third voice yet";

  rig.set("voices", 1.0);
  const std::vector<float> three = captureImpulseResponse(rig, 0.1);
  const std::vector<float> thirdVoice(three.begin() + ms(17.0), three.end());
  EXPECT_NEAR(peakIndexOf(thirdVoice) + ms(17.0) - origin, ms(21.0), 2);
  // Three voices at 1 / sqrt(3) each.
  EXPECT_NEAR(peakOf(thirdVoice), 0.54f, 0.08f);
}

TEST(ChorusTest, PitchWobbleRunsAtTheRateParameter) {
  for (const double rateHz : {1.0, 3.0}) {
    SCOPED_TRACE(rateHz);
    EXPECT_NEAR(measuredRate({{"rateHz", rateHz}}), rateHz, 0.03 * rateHz);
  }
}

TEST(ChorusTest, SyncedRateFollowsTheTempoAtTwoTempos) {
  struct Case {
    double bpm;
    int division;
    double expectedHz;
  };
  // A quarter note per cycle at 120 and 90 bpm, and an eighth at 90.
  for (const Case& c : {Case{120.0, fx::kDivQuarter, 2.0}, Case{90.0, fx::kDivQuarter, 1.5},
                        Case{90.0, fx::kDivEighth, 3.0}}) {
    SCOPED_TRACE(c.bpm);
    EXPECT_NEAR(measuredRate({{"sync", 1.0}, {"division", static_cast<double>(c.division)},
                              {"rateHz", 7.0}},
                             c.bpm),
                c.expectedHz, 0.03 * c.expectedHz);
  }
}

TEST(ChorusTest, DepthZeroIsAStaticFilterWhateverTheRate) {
  const std::vector<float> in = noise(2.0, 5u, 0.25f, kRate);
  const auto render = [&](double rateHz) {
    EffectRig rig(kKind, {{"depth", 0.0}, {"rateHz", rateHz}, {"mix", 1.0}});
    return rig.render(in);
  };
  const std::vector<float> slow = render(0.2);
  const std::vector<float> fast = render(9.0);
  EXPECT_LT(maxDifference(slow, fast), 1.0e-6f);
  EXPECT_GT(rmsOf(slow), 0.05);
}

TEST(ChorusTest, PitchDeviationGrowsWithDepth) {
  // A 400 Hz sine at 2 Hz: the swing of the pitch scales with the depth (see
  // measuredRate for why the sweep stays shallow).
  const auto swingCents = [](double depth) {
    EffectRig rig(kKind, {{"depth", depth}, {"rateHz", 2.0}, {"toneHz", 20000.0}, {"mix", 1.0}});
    const CycleFrequencies f = cycleFrequencies(rig.render(sine(4.0, 400.0, 0.25f, kRate)), kRate,
                                                static_cast<size_t>(samples(0.5)));
    const Deviation d = deviationOf(f, 400.0, 0.5, 4.0);
    return d.maxCents - d.minCents;
  };
  const double smallSwing = swingCents(10.0);
  const double large = swingCents(20.0);
  EXPECT_GT(smallSwing, 5.0);
  EXPECT_NEAR(large / smallSwing, 2.0, 0.3);
}

TEST(ChorusTest, ToneIsAHighCutOnTheWet) {
  const auto gainAt8k = [](double toneHz) {
    EffectRig rig(kKind, {{"depth", 0.0}, {"toneHz", toneHz}, {"mix", 1.0}});
    return sineProbeGainDb(rig, 8000.0);
  };
  const double open = gainAt8k(20000.0);
  const double dark = gainAt8k(1000.0);
  // One pole at 1 kHz is about 18 dB down at 8 kHz.
  EXPECT_LT(dark, open - 14.0);
  EXPECT_GT(open, -4.0);
}

TEST(ChorusTest, WetLevelFollowsTheInput) {
  EffectRig rig(kKind, {{"toneHz", 20000.0}, {"mix", 1.0}});
  const std::vector<float> in = noise(4.0, 9u, 0.25f, kRate);
  const std::vector<float> out = rig.render(in);
  // Two uncorrelated voices at 1 / sqrt(2) each are as loud as the input;
  // the 20 kHz high cut takes about 1.3 dB off white noise.
  EXPECT_NEAR(steadyStateGainDb(out, in, static_cast<size_t>(samples(0.5))), -1.3, 0.7);
}

TEST(ChorusTest, SidesDifferOnlyInATrueStereoImage) {
  const std::vector<float> in = noise(2.0, 3u, 0.25f, kRate);
  {
    EffectRig rig(kKind, {{"width", 100.0}, {"mix", 1.0}});
    const Stereo out = rig.renderBoth(in);
    EXPECT_GT(relativeDifferenceDb(out.left, out.right, static_cast<size_t>(samples(0.2))), -12.0)
        << "no stereo spread at full width";
    // Neither side is reduced to silence or doubled.
    EXPECT_NEAR(toDb(rmsOf(out.left)), toDb(rmsOf(out.right)), 1.0);
  }
  {
    EffectRig rig(kKind, {{"width", 0.0}, {"mix", 1.0}});
    const Stereo out = rig.renderBoth(in);
    EXPECT_LT(maxDifference(out.left, out.right), 1.0e-6f) << "width 0 must be a mono chorus";
  }
  {
    // Two independent signals: the same chorus on each, whatever the width.
    EffectRig rig(kKind, {{"width", 100.0}, {"mix", 1.0}}, rigFor(Layout::DualMono));
    const Stereo out = rig.renderBoth(in);
    EXPECT_LT(maxDifference(out.left, out.right), 1.0e-6f);
  }
  {
    // An amp model later in the lane throws the image away: mono chorus.
    DirectKind kind(kKind, {{"width", 100.0}}, Layout::Stereo);
    kind.ctx.blockIndex = 0;
    kind.ctx.nam.lastNamIndex = 3;
    const Stereo out = kind.render(Stereo{in, in});
    EXPECT_LT(maxDifference(out.left, out.right), 1.0e-6f);
  }
}

TEST(ChorusTest, StaysBoundedAtTheExtremes) {
  for (const Layout layout : {Layout::Mono, Layout::Stereo}) {
    DirectKind kind(kKind,
                    {{"depth", 100.0}, {"rateHz", 10.0}, {"voices", 1.0}, {"toneHz", 20000.0}},
                    layout);
    const std::vector<float> hot = noise(5.0, 17u, 1.0f, kRate);
    const Stereo out = kind.render(Stereo{hot, hot});
    expectCleanSignal(out.left, 3.0f, "extreme chorus, left");
    expectCleanSignal(out.right, 3.0f, "extreme chorus, right");
  }
}

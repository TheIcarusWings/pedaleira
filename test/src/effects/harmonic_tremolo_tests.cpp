// modulation.harmonictrem (plugin/src/effects/modulation/HarmonicTremolo.cpp):
// the conformance test, then what a harmonic tremolo is for: a signal split at
// the crossover, the lows and the highs swinging in opposite phase by the
// stated depth at the stated rate (free and tempo-synced), a flat sound with
// no depth, and a right side half a cycle behind the left in a true stereo
// image only.
#include "effects/effect_test_helpers.h"
#include "effects/modulation_test_helpers.h"

#include <gtest/gtest.h>

using namespace fxtest;
using namespace modtest;

namespace {

constexpr const char* kKind = "modulation.harmonictrem";

// The level of a rendered sine over time: RMS of 10 ms windows (a whole cycle
// of the lowest tone used), from `from`
// seconds on.
std::vector<double> envelopeOf(const std::vector<float>& x, double from) {
  std::vector<double> out;
  const size_t window = static_cast<size_t>(samples(0.01));
  for (size_t begin = static_cast<size_t>(samples(from)); begin + window <= x.size(); begin += window)
    out.push_back(rmsOf(x, begin, begin + window));
  return out;
}

double correlation(const std::vector<double>& a, const std::vector<double>& b) {
  const size_t n = std::min(a.size(), b.size());
  double ma = 0.0, mb = 0.0;
  for (size_t i = 0; i < n; ++i) {
    ma += a[i];
    mb += b[i];
  }
  ma /= static_cast<double>(n);
  mb /= static_cast<double>(n);
  double ab = 0.0, aa = 0.0, bb = 0.0;
  for (size_t i = 0; i < n; ++i) {
    ab += (a[i] - ma) * (b[i] - mb);
    aa += (a[i] - ma) * (a[i] - ma);
    bb += (b[i] - mb) * (b[i] - mb);
  }
  return ab / std::sqrt(aa * bb);
}

// A sine of `hz` through the fully wet kind alone (no DC blocker, no mix).
std::vector<float> toneThrough(Params params, double hz, double seconds, Layout layout = Layout::Mono) {
  DirectKind kind(kKind, params, layout);
  const std::vector<float> in = sine(seconds, hz, 0.25f, kRate);
  return kind.render(Stereo{in, in}).left;
}

// Index in `series` of its highest and lowest value.
size_t argMax(const std::vector<double>& s) { return std::max_element(s.begin(), s.end()) - s.begin(); }
size_t argMin(const std::vector<double>& s) { return std::min_element(s.begin(), s.end()) - s.begin(); }

}  // namespace

TEST(HarmonicTremoloTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"depth", 80.0}, {"rateHz", 4.0}, {"crossoverHz", 600.0}, {"width", 50.0}};
  options.stateParams = {{"sync", 1.0},          {"division", fx::kDivEighth},
                         {"rateHz", 3.3},        {"depth", 40.0},
                         {"crossoverHz", 1200.0}, {"toneHz", 6000.0},
                         {"width", 70.0}};
  options.cpuAverageBudgetNs = 14000.0;
  options.cpuWorstBudgetNs = 150000.0;
  expectKindConforms("modulation.harmonictrem", options);
}

TEST(HarmonicTremoloTest, RegisteredFullyWet) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kKind);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->category, BlockCategory::Modulation);
  EXPECT_EQ(info->mixLaw, BlockMixLaw::Crossfade);
  EXPECT_FLOAT_EQ(info->defaultMix, 1.0f);
  EXPECT_EQ(info->name, juce::String("Harmonic Tremolo"));
}

TEST(HarmonicTremoloTest, LowsAndHighsSwingInOppositePhase) {
  // Crossover 800 Hz, 2 Hz, depth 80%: a 100 Hz tone is wholly in the lows and
  // a 4 kHz tone wholly in the highs. The wave starts at zero, rising: the
  // lows reach their top a quarter cycle (125 ms) in and their bottom at
  // 375 ms, the highs the other way round.
  const Params params = {{"depth", 80.0}, {"rateHz", 2.0}, {"crossoverHz", 800.0},
                         {"toneHz", 20000.0}};
  const std::vector<double> lows = envelopeOf(toneThrough(params, 100.0, 4.0), 0.5);
  const std::vector<double> highs = envelopeOf(toneThrough(params, 4000.0, 4.0), 0.5);
  EXPECT_LT(correlation(lows, highs), -0.97);

  // The first 500 ms of a render show where the extremes fall.
  const std::vector<double> lowsEarly = envelopeOf(toneThrough(params, 100.0, 0.5), 0.0);
  const std::vector<double> highsEarly = envelopeOf(toneThrough(params, 4000.0, 0.5), 0.0);
  const double step = 0.01;
  EXPECT_NEAR((static_cast<double>(argMax(lowsEarly)) + 0.5) * step, 0.125, 0.02) << "lows top";
  EXPECT_NEAR((static_cast<double>(argMin(highsEarly)) + 0.5) * step, 0.125, 0.02) << "highs bottom";
  EXPECT_NEAR((static_cast<double>(argMin(lowsEarly)) + 0.5) * step, 0.375, 0.02) << "lows bottom";
  EXPECT_NEAR((static_cast<double>(argMax(highsEarly)) + 0.5) * step, 0.375, 0.02) << "highs top";
}

TEST(HarmonicTremoloTest, EachBandSwingsBetweenOneAndOneMinusDepth) {
  for (const double depth : {30.0, 60.0, 100.0}) {
    SCOPED_TRACE(depth);
    const Params params = {{"depth", depth}, {"rateHz", 2.0}, {"toneHz", 20000.0}};
    for (const double hz : {100.0, 4000.0}) {
      const std::vector<double> env = envelopeOf(toneThrough(params, hz, 3.0), 0.5);
      const double top = *std::max_element(env.begin(), env.end());
      const double bottom = *std::min_element(env.begin(), env.end());
      // The carrier's RMS is 0.25 / sqrt 2; the gain swings 1 to 1 - depth.
      EXPECT_NEAR(top / (0.25 / std::sqrt(2.0)), 1.0, 0.05) << hz;
      EXPECT_NEAR(bottom / top, 1.0 - depth * 0.01, 0.03 + 0.01) << hz;
    }
  }
}

TEST(HarmonicTremoloTest, CrossoverDecidesWhichBandAToneBelongsTo) {
  // The same 600 Hz tone moves with the lows when the crossover sits well
  // above it and with the highs when it sits well below.
  const Params base = {{"depth", 80.0}, {"rateHz", 2.0}, {"toneHz", 20000.0}};
  const auto envelopeAt = [&](double crossoverHz, double hz) {
    Params params = base;
    params.emplace_back("crossoverHz", crossoverHz);
    return envelopeOf(toneThrough(params, hz, 4.0), 0.5);
  };
  const std::vector<double> lowReference = envelopeAt(800.0, 100.0);
  const std::vector<double> highReference = envelopeAt(800.0, 4000.0);
  EXPECT_GT(correlation(envelopeAt(3000.0, 600.0), lowReference), 0.9);
  EXPECT_GT(correlation(envelopeAt(200.0, 600.0), highReference), 0.9);
}

TEST(HarmonicTremoloTest, ModulationRunsAtTheRateParameter) {
  for (const double rateHz : {1.0, 3.0, 9.0}) {
    SCOPED_TRACE(rateHz);
    EffectRig rig(kKind, {{"depth", 80.0}, {"rateHz", rateHz}, {"crossoverHz", 400.0}, {"mix", 1.0}});
    const std::vector<float> out = rig.render(sine(8.0, 4000.0, 0.25f, kRate));
    EXPECT_NEAR(amplitudeModulationRateHz(out, kRate, static_cast<size_t>(samples(0.5))), rateHz,
                0.03 * rateHz);
  }
}

TEST(HarmonicTremoloTest, SyncedRateFollowsTheTempoAtTwoTempos) {
  struct Case {
    double bpm;
    int division;
    double expectedHz;
  };
  for (const Case& c : {Case{120.0, fx::kDivQuarter, 2.0}, Case{90.0, fx::kDivQuarter, 1.5},
                        Case{90.0, fx::kDivEighth, 3.0}}) {
    SCOPED_TRACE(c.bpm);
    RigOptions options;
    options.bpm = c.bpm;
    EffectRig rig(kKind,
                  {{"depth", 80.0}, {"sync", 1.0}, {"division", static_cast<double>(c.division)},
                   {"rateHz", 7.0}, {"crossoverHz", 400.0}, {"mix", 1.0}},
                  options);
    const std::vector<float> out = rig.render(sine(8.0, 4000.0, 0.25f, kRate));
    EXPECT_NEAR(amplitudeModulationRateHz(out, kRate, static_cast<size_t>(samples(0.5))),
                c.expectedHz, 0.03 * c.expectedHz);
  }
}

TEST(HarmonicTremoloTest, WithoutDepthTheSplitAddsBackFlat) {
  // Fourth-order Linkwitz-Riley halves sum to an all-pass: the level of a
  // tone anywhere is the input's.
  for (const double crossoverHz : {300.0, 800.0, 2500.0}) {
    EffectRig rig(kKind, {{"depth", 0.0}, {"crossoverHz", crossoverHz}, {"toneHz", 20000.0},
                          {"mix", 1.0}});
    for (const double hz : {60.0, 250.0, 800.0, 2000.0, 5000.0})
      EXPECT_NEAR(sineProbeGainDb(rig, hz), 0.0, 0.35) << crossoverHz << " Hz crossover, " << hz;
  }
}

TEST(HarmonicTremoloTest, ToneIsAHighCutOnTheOutput) {
  const auto gainAt8k = [](double toneHz) {
    EffectRig rig(kKind, {{"depth", 0.0}, {"toneHz", toneHz}, {"mix", 1.0}});
    return sineProbeGainDb(rig, 8000.0);
  };
  EXPECT_LT(gainAt8k(1000.0), gainAt8k(20000.0) - 14.0);
}

TEST(HarmonicTremoloTest, ReadingsReportBothBandGains) {
  // The two gains always add to 2 - depth: where one is up the other is down.
  DirectKind kind(kKind, {{"depth", 80.0}, {"rateHz", 3.0}}, Layout::Mono, 64);
  ASSERT_EQ(kind.engine().readingTable().size(), 2u);
  EXPECT_EQ(kind.engine().readingTable()[0].id, juce::String("lowGain"));
  EXPECT_EQ(kind.engine().readingTable()[1].id, juce::String("highGain"));
  float lowest = 1.0f, highest = 0.0f;
  const std::vector<float> in = sine(1.0, 1000.0, 0.25f, kRate);
  for (size_t at = 0; at + 64 <= in.size(); at += 64) {
    kind.render(std::vector<float>(in.begin() + at, in.begin() + at + 64));
    const float low = kind.engine().getReading(0);
    const float high = kind.engine().getReading(1);
    EXPECT_NEAR(low + high, 2.0f - 0.8f, 0.002f);
    lowest = std::min(lowest, low);
    highest = std::max(highest, low);
  }
  EXPECT_NEAR(lowest, 0.2f, 0.01f);
  EXPECT_NEAR(highest, 1.0f, 0.01f);
}

TEST(HarmonicTremoloTest, WidthPutsTheRightSideHalfACycleBehindOnlyInATrueStereoImage) {
  const std::vector<float> lowTone = sine(3.0, 100.0, 0.25f, kRate);
  {
    EffectRig rig(kKind, {{"depth", 80.0}, {"rateHz", 2.0}, {"crossoverHz", 800.0},
                          {"width", 100.0}, {"toneHz", 20000.0}, {"mix", 1.0}});
    const Stereo out = rig.renderBoth(lowTone);
    EXPECT_LT(correlation(envelopeOf(out.left, 0.5), envelopeOf(out.right, 0.5)), -0.97);
  }
  {
    EffectRig rig(kKind, {{"width", 0.0}, {"mix", 1.0}});
    const Stereo out = rig.renderBoth(lowTone);
    EXPECT_LT(maxDifference(out.left, out.right), 1.0e-6f) << "width 0 must be centred";
  }
  {
    EffectRig rig(kKind, {{"width", 100.0}, {"mix", 1.0}}, rigFor(Layout::DualMono));
    const Stereo out = rig.renderBoth(lowTone);
    EXPECT_LT(maxDifference(out.left, out.right), 1.0e-6f);
  }
  {
    DirectKind kind(kKind, {{"width", 100.0}}, Layout::Stereo);
    kind.ctx.nam.lastNamIndex = 3;
    const Stereo out = kind.render(Stereo{lowTone, lowTone});
    EXPECT_LT(maxDifference(out.left, out.right), 1.0e-6f) << "stereo before an amp is lost";
  }
}

TEST(HarmonicTremoloTest, TailCoversTheRingOfTheCrossover) {
  for (const double crossoverHz : {200.0, 3000.0}) {
    SCOPED_TRACE(crossoverHz);
    const TailMeasurement tail =
        expectSilenceAfterReportedTail(kKind, {{"depth", 0.0}, {"crossoverHz", crossoverHz}});
    EXPECT_FALSE(tail.capped);
    EXPECT_GT(tail.measuredSeconds, 0.0002);
  }
}

TEST(HarmonicTremoloTest, StaysBoundedAtTheExtremes) {
  for (const Layout layout : {Layout::Mono, Layout::Stereo}) {
    DirectKind kind(kKind,
                    {{"depth", 100.0}, {"rateHz", 20.0}, {"crossoverHz", 200.0}, {"width", 100.0}},
                    layout);
    const std::vector<float> hot = noise(5.0, 17u, 1.0f, kRate);
    const Stereo out = kind.render(Stereo{hot, hot});
    // The LR4 halves overshoot a hard edge a little.
    expectCleanSignal(out.left, 3.0f, "extreme harmonic tremolo, left");
    expectCleanSignal(out.right, 3.0f, "extreme harmonic tremolo, right");
  }
}

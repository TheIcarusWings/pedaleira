// modulation.patterntrem (plugin/src/effects/modulation/PatternTremolo.cpp):
// the conformance test, then what a step tremolo is for: steps that start on
// the clock (free, and one note value at a set tempo), a level of their own
// per step, 4 to 16 of them, an edge that goes from hard to a full-step glide
// with the shape, preset patterns that adapt to the step count, a clock that
// sits where the song position says while the host rolls, and a depth that
// shallows the whole pattern.
//
// The gain is measured with a steady 1.0 as input on the kind alone
// (DirectKind has no DC blocker around it), so what comes out is the gain.
#include "effects/effect_test_helpers.h"
#include "effects/modulation_test_helpers.h"

#include <gtest/gtest.h>

using namespace fxtest;
using namespace modtest;

namespace {

constexpr const char* kKind = "modulation.patterntrem";

std::vector<float> steady(int frames) { return std::vector<float>(static_cast<size_t>(frames), 1.0f); }

// The gain of the kind alone over `frames`.
std::vector<float> gainOf(const Params& params, int frames, double bpm = 120.0) {
  DirectKind kind(kKind, params, Layout::Mono, 256, bpm);
  return kind.render(Stereo{steady(frames), steady(frames)}).left;
}

// The gain at the middle of step `k` (a step being `stepFrames` long).
float plateau(const std::vector<float>& gain, int k, int stepFrames) {
  return gain[static_cast<size_t>(k * stepFrames + stepFrames / 2)];
}

// Frames at which the gain crosses `level` going either way, in order.
std::vector<int> crossings(const std::vector<float>& gain, float level) {
  std::vector<int> at;
  for (size_t i = 1; i < gain.size(); ++i)
    if ((gain[i - 1] < level) != (gain[i] < level))
      at.push_back(static_cast<int>(i));
  return at;
}

// Eight steps at 120 bpm of 1/16 notes: 125 ms, 6000 frames each.
const Params kSyncedSixteenths = {{"sync", 1.0}, {"division", fx::kDivSixteenth}};
constexpr int kStepFrames = 6000;

Params with(Params base, const Params& more) {
  base.insert(base.end(), more.begin(), more.end());
  return base;
}

}  // namespace

TEST(PatternTremoloTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"pattern", 0.0}, {"rateHz", 6.0}, {"shape", 30.0}, {"depth", 90.0},
                    {"level1", 20.0}, {"level2", 60.0}, {"level3", 0.0}, {"steps", 4.0}};
  options.stateParams = {{"sync", 1.0},     {"division", fx::kDivEighthTriplet},
                         {"rateHz", 3.0},   {"steps", 9.0},
                         {"pattern", 5.0},  {"shape", 45.0},
                         {"depth", 55.0},   {"level1", 10.0},
                         {"level16", 35.0}};
  options.cpuAverageBudgetNs = 3000.0;
  options.cpuWorstBudgetNs = 150000.0;
  expectKindConforms("modulation.patterntrem", options);
}

TEST(PatternTremoloTest, RegisteredFullyWetWithItsPatternsAndSteps) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kKind);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->category, BlockCategory::Modulation);
  EXPECT_EQ(info->mixLaw, BlockMixLaw::Crossfade);
  EXPECT_FLOAT_EQ(info->defaultMix, 1.0f);
  EXPECT_EQ(info->name, juce::String("Pattern Tremolo"));
  const int steps = findBlockParam(info->params, "steps");
  ASSERT_GE(steps, 0);
  EXPECT_EQ(info->params[static_cast<size_t>(steps)].choices[0], juce::String("4"));
  EXPECT_EQ(info->params[static_cast<size_t>(steps)].choices[12], juce::String("16"));
  for (int s = 1; s <= 16; ++s)
    EXPECT_GE(findBlockParam(info->params, "level" + juce::String(s)), 0) << s;
}

TEST(PatternTremoloTest, StepsStartOnTheClockAtASetTempo) {
  // A pulse (on, off, on, ...) at full depth, hard edges. 120 bpm, 1/16:
  // every 6000 frames the gain flips, within a millisecond.
  const std::vector<float> gain = gainOf(with(kSyncedSixteenths, {{"pattern", 1.0}, {"depth", 100.0}, {"shape", 0.0}}),
                                         kStepFrames * 16);
  const std::vector<int> edges = crossings(gain, 0.5f);
  ASSERT_GE(edges.size(), 15u);
  for (size_t k = 0; k < 15; ++k)
    EXPECT_NEAR(edges[k], static_cast<int>(k + 1) * kStepFrames, 48) << "edge " << k;
  EXPECT_NEAR(plateau(gain, 0, kStepFrames), 1.0f, 0.001f);
  EXPECT_NEAR(plateau(gain, 1, kStepFrames), 0.0f, 0.001f);
}

TEST(PatternTremoloTest, StepLengthFollowsTheTempoAndTheNoteValue) {
  struct Case {
    double bpm;
    int division;
    int stepFrames;
  };
  // 90 bpm: a beat is 32000 frames; an eighth is 16000, a dotted eighth
  // 24000. 140 bpm 1/16 triplet: 60/140 / 6 s = 3428.6 frames.
  for (const Case& c : {Case{90.0, fx::kDivEighth, 16000}, Case{90.0, fx::kDivEighthDotted, 24000},
                        Case{140.0, fx::kDivSixteenthTriplet, 3429}}) {
    SCOPED_TRACE(c.bpm);
    SCOPED_TRACE(c.division);
    const std::vector<float> gain =
        gainOf({{"sync", 1.0}, {"division", static_cast<double>(c.division)}, {"rateHz", 20.0},
                {"pattern", 1.0}, {"depth", 100.0}, {"shape", 0.0}},
               c.stepFrames * 8, c.bpm);
    const std::vector<int> edges = crossings(gain, 0.5f);
    ASSERT_GE(edges.size(), 7u);
    // The span of seven edges is seven steps, however the individual
    // edges fall on the sample grid.
    EXPECT_NEAR(static_cast<double>(edges[6] - edges[0]) / 6.0, c.bpm == 140.0 ? 3428.57 : c.stepFrames,
                2.0);
    EXPECT_NEAR(edges[0], c.stepFrames, 48);
  }
}

TEST(PatternTremoloTest, FreeRateIsStepsPerSecond) {
  for (const double rateHz : {2.0, 5.0, 10.0}) {
    SCOPED_TRACE(rateHz);
    const int stepFrames = static_cast<int>(std::lround(kRate / rateHz));
    const std::vector<float> gain =
        gainOf({{"rateHz", rateHz}, {"pattern", 1.0}, {"depth", 100.0}, {"shape", 0.0}}, stepFrames * 10);
    const std::vector<int> edges = crossings(gain, 0.5f);
    ASSERT_GE(edges.size(), 9u);
    EXPECT_NEAR(static_cast<double>(edges[8] - edges[0]) / 8.0, kRate / rateHz, 1.5);
  }
}

TEST(PatternTremoloTest, EveryStepHasALevelOfItsOwn) {
  // Sixteen distinct levels; depth 100 so the gain is the level itself.
  Params params = {{"pattern", 0.0}, {"depth", 100.0}, {"shape", 0.0}, {"steps", 12.0}};
  for (int s = 1; s <= 16; ++s)
    params.emplace_back("level" + juce::String(s), 5.0 * s);
  const std::vector<float> gain = gainOf(with(kSyncedSixteenths, params), kStepFrames * 34);
  for (int k = 0; k < 34; ++k)
    EXPECT_NEAR(plateau(gain, k, kStepFrames), 0.05f * static_cast<float>(k % 16 + 1), 0.002f) << k;
}

TEST(PatternTremoloTest, TheStepCountSetsTheLengthOfThePattern) {
  // Levels 5 %, 10 %, ... : with N steps the pattern repeats after N.
  for (const int steps : {4, 5, 7, 11, 16}) {
    SCOPED_TRACE(steps);
    Params params = {{"pattern", 0.0}, {"depth", 100.0}, {"shape", 0.0},
                     {"steps", static_cast<double>(steps - 4)}};
    for (int s = 1; s <= 16; ++s)
      params.emplace_back("level" + juce::String(s), 5.0 * s);
    const std::vector<float> gain = gainOf(with(kSyncedSixteenths, params), kStepFrames * 3 * steps);
    for (int k = 0; k < 3 * steps; ++k)
      EXPECT_NEAR(plateau(gain, k, kStepFrames), 0.05f * static_cast<float>(k % steps + 1), 0.002f) << k;
  }
}

TEST(PatternTremoloTest, DepthShallowsTheWholePattern) {
  // Gain = 1 - depth (1 - level): a 40 % step at 50 % depth is 0.7.
  Params params = {{"pattern", 0.0}, {"depth", 50.0}, {"shape", 0.0}, {"level1", 40.0}, {"level2", 100.0},
                   {"level3", 0.0}, {"steps", 0.0}};
  const std::vector<float> gain = gainOf(with(kSyncedSixteenths, params), kStepFrames * 4);
  EXPECT_NEAR(plateau(gain, 0, kStepFrames), 0.7f, 0.002f);
  EXPECT_NEAR(plateau(gain, 1, kStepFrames), 1.0f, 0.002f);
  EXPECT_NEAR(plateau(gain, 2, kStepFrames), 0.5f, 0.002f);
}

TEST(PatternTremoloTest, DepthZeroPassesTheSignalOnBitForBit) {
  const std::vector<float> in = noise(1.0, 3u, 0.25f, kRate);
  DirectKind kind(kKind, {{"depth", 0.0}, {"pattern", 1.0}}, Layout::Mono);
  EXPECT_EQ(kind.render(in), in);
}

TEST(PatternTremoloTest, ShapeGoesFromAHardEdgeToAGlideOverTheWholeStep) {
  // The 10 % to 90 % time of the edge from a 0 level to a 100 % level.
  const auto edgeFrames = [](double shape) {
    Params params = {{"pattern", 0.0}, {"depth", 100.0}, {"shape", shape}, {"steps", 0.0},
                     {"level1", 0.0}, {"level2", 100.0}, {"level3", 100.0}, {"level4", 100.0}};
    // Four steps: 0, 100, 100, 100. The edge of interest is into step 2.
    const std::vector<float> gain = gainOf(with(kSyncedSixteenths, params), kStepFrames * 3);
    int from = -1, to = -1;
    for (int i = kStepFrames; i < 2 * kStepFrames; ++i) {
      if (from < 0 && gain[static_cast<size_t>(i)] >= 0.1f)
        from = i;
      if (to < 0 && gain[static_cast<size_t>(i)] >= 0.9f)
        to = i;
    }
    return std::make_pair(from, to - from);
  };
  const auto hard = edgeFrames(0.0);
  const auto half = edgeFrames(50.0);
  const auto full = edgeFrames(100.0);
  // Hard: an edge of about a millisecond (a raised cosine of 0.7 ms plus a
  // 0.2 ms smoother), starting at the step.
  EXPECT_NEAR(hard.first, kStepFrames, 30);
  EXPECT_LT(hard.second, ms(1.0));
  EXPECT_GT(hard.second, 5);
  // A raised cosine takes 0.59 of its length from 10 % to 90 %. Half shape:
  // an edge of half a step; full: a whole step.
  EXPECT_NEAR(half.second, 0.59 * 0.5 * kStepFrames, 0.06 * 0.59 * 0.5 * kStepFrames + 40);
  EXPECT_NEAR(full.second, 0.59 * kStepFrames, 0.06 * 0.59 * kStepFrames);
  EXPECT_GT(full.second, 20 * hard.second);
}

TEST(PatternTremoloTest, HardEdgesDoNotClick) {
  // At the sharpest the gain moves by well under a tenth a sample.
  const std::vector<float> gain =
      gainOf(with(kSyncedSixteenths, {{"pattern", 1.0}, {"depth", 100.0}, {"shape", 0.0}}), kStepFrames * 6);
  EXPECT_LT(maxStep(gain, 0, gain.size()), 0.1f);
  EXPECT_GT(maxStep(gain, 0, gain.size()), 0.02f);
}

TEST(PatternTremoloTest, PresetsAreDrawnOverTheStepsInUse) {
  struct Case {
    const char* name;
    int pattern;
    int steps;
    std::vector<float> expected;
  };
  const std::vector<Case> cases = {
      {"pulse", 1, 8, {1, 0, 1, 0, 1, 0, 1, 0}},
      {"gallop", 2, 8, {1, 1, 0, 1, 1, 0, 1, 1}},
      {"ramp up", 3, 8, {0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f, 0.875f, 1.0f}},
      {"ramp down", 4, 4, {1.0f, 0.75f, 0.5f, 0.25f}},
      {"ramp up, 12 steps", 3, 12, {1 / 12.f, 2 / 12.f, 3 / 12.f, 4 / 12.f, 5 / 12.f, 6 / 12.f, 7 / 12.f, 8 / 12.f, 9 / 12.f, 10 / 12.f, 11 / 12.f, 1.0f}},
      {"stutter", 6, 8, {1, 0.25f, 0.25f, 0.25f, 1, 0.25f, 0.25f, 0.25f}},
      {"syncopated", 7, 8, {1, 0, 0.6f, 0, 1, 0.3f, 0.6f, 0}},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    const std::vector<float> gain =
        gainOf(with(kSyncedSixteenths, {{"pattern", static_cast<double>(c.pattern)}, {"depth", 100.0},
                                        {"shape", 0.0}, {"steps", static_cast<double>(c.steps - 4)}}),
               kStepFrames * c.steps * 2);
    for (int k = 0; k < 2 * c.steps; ++k)
      EXPECT_NEAR(plateau(gain, k, kStepFrames), c.expected[static_cast<size_t>(k % c.steps)], 0.002f) << k;
  }
  // The swell peaks in the middle of its steps and never reaches zero.
  const std::vector<float> swell = gainOf(
      with(kSyncedSixteenths, {{"pattern", 5.0}, {"depth", 100.0}, {"shape", 0.0}, {"steps", 4.0}}),
      kStepFrames * 8);
  EXPECT_GT(plateau(swell, 3, kStepFrames), plateau(swell, 0, kStepFrames) + 0.3f);
  EXPECT_GT(plateau(swell, 0, kStepFrames), 0.1f);
}

TEST(PatternTremoloTest, FollowsTheSongPositionWhileTheHostRolls) {
  // Pulse over 8 steps of 1/16 at 120 bpm: a step is a quarter of a beat.
  // At a position of 2.125 beats the clock is half way through step 8 (an
  // "on" step, being step 0 of the next lap); at 2.375 half way through step
  // 9 (off). Two blocks, same params, only the host position differs.
  const auto gainAtPosition = [](double ppq) {
    DirectKind kind(kKind,
                    with(kSyncedSixteenths, {{"pattern", 1.0}, {"depth", 100.0}, {"shape", 0.0}}),
                    Layout::Mono, 4096);
    kind.ctx.transport.playing = true;
    kind.ctx.transport.ppqPosition = ppq;
    return kind.render(steady(1000)).front();
  };
  EXPECT_NEAR(gainAtPosition(2.125), 1.0f, 0.001f);
  EXPECT_NEAR(gainAtPosition(2.375), 0.0f, 0.001f);
  EXPECT_NEAR(gainAtPosition(3.125), 1.0f, 0.001f);
  EXPECT_NEAR(gainAtPosition(3.375), 0.0f, 0.001f);
  // Free-running (the host stopped), the clock starts at its own step 0.
  DirectKind stopped(kKind, with(kSyncedSixteenths, {{"pattern", 1.0}, {"depth", 100.0}, {"shape", 0.0}}),
                     Layout::Mono, 4096);
  stopped.ctx.transport.ppqPosition = 2.375;
  EXPECT_NEAR(stopped.render(steady(1000)).front(), 1.0f, 0.001f);
}

TEST(PatternTremoloTest, ReadingFollowsTheCurrentStep) {
  DirectKind kind(kKind, with(kSyncedSixteenths, {{"pattern", 1.0}, {"steps", 4.0}}), Layout::Mono, 64);
  ASSERT_EQ(kind.engine().readingTable().size(), 1u);
  EXPECT_EQ(kind.engine().readingTable()[0].id, juce::String("step"));
  std::vector<int> seen;
  for (int block = 0; block < kStepFrames * 14 / 64; ++block) {
    kind.render(steady(64));
    seen.push_back(static_cast<int>(std::lround(kind.engine().getReading(0))));
  }
  // 8 steps in use: the reading counts 0 to 7 and wraps, one step per 125 ms.
  const auto stepAt = [&](int frame) { return seen[static_cast<size_t>(frame / 64 - 1)]; };
  EXPECT_EQ(stepAt(3000), 0);
  EXPECT_EQ(stepAt(kStepFrames + 3000), 1);
  EXPECT_EQ(stepAt(7 * kStepFrames + 3000), 7);
  EXPECT_EQ(stepAt(8 * kStepFrames + 3000), 0);
  EXPECT_EQ(stepAt(13 * kStepFrames + 3000), 5);
}

TEST(PatternTremoloTest, SameGainOnBothChannelsAndNothingStereo) {
  const std::vector<float> in = noise(2.0, 3u, 0.25f, kRate);
  EffectRig rig(kKind, {{"pattern", 2.0}, {"mix", 1.0}});
  const Stereo out = rig.renderBoth(in);
  EXPECT_EQ(out.left, out.right);
}

TEST(PatternTremoloTest, StaysBoundedAtTheExtremes) {
  for (const Layout layout : {Layout::Mono, Layout::Stereo}) {
    DirectKind kind(kKind, {{"rateHz", 32.0}, {"steps", 12.0}, {"pattern", 7.0}, {"shape", 100.0}}, layout);
    const std::vector<float> hot = noise(5.0, 17u, 1.0f, kRate);
    const Stereo out = kind.render(Stereo{hot, hot});
    expectCleanSignal(out.left, 1.0f, "extreme pattern tremolo, left");
    expectCleanSignal(out.right, 1.0f, "extreme pattern tremolo, right");
  }
}

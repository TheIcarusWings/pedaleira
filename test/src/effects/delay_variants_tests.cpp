// The delay variants (plugin/src/effects/delay): slapback, dual, reverse, dual
// reverse and multi-tap, and the ducking every delay shares (DelayCommon.h). Each new kind
// gets the conformance test and then what it is for: an exact time, left and
// right times and a cross-feedback that moves the repeats across, a reverse
// that really plays backwards with no click at the chunk joins, taps where the
// pattern puts them. The ducking is tested across all eleven delays at once:
// it lowers the wet by the set depth while the input is loud, lets go within
// the release, leaves the loop alone, and at 0 dB changes nothing.
#include "effects/delay_test_support.h"
#include "effects/dsp/TempoSync.h"

#include <gtest/gtest.h>

#include <cstring>
#include <numeric>
#include <tuple>

using namespace delaytest;

namespace {

// The wet output of a kind on its own (no mix, no input stage).
Stereo wetOf(const char* kind, const Params& params, const Stereo& in,
             Layout layout = Layout::Mono, double bpm = 120.0) {
  DirectKind direct(kind, params, layout, 256, bpm);
  return direct.render(in);
}

std::vector<float> wetOf(const char* kind, const Params& params, const std::vector<float>& in,
                         Layout layout = Layout::Mono) {
  return wetOf(kind, params, Stereo{in, in}, layout).left;
}

// Where an impulse's echoes are: local maxima above `threshold`, merged when
// closer than 3 samples.
std::vector<int> peaksOf(const std::vector<float>& x, float threshold) {
  std::vector<int> found;
  for (size_t i = 1; i + 1 < x.size(); ++i) {
    if (x[i] > threshold && x[i] > x[i - 1] && x[i] >= x[i + 1]) {
      if (!found.empty() && static_cast<int>(i) - found.back() < 3)
        continue;
      found.push_back(static_cast<int>(i));
    }
  }
  return found;
}

double windowRms(const std::vector<float>& x, double fromSeconds, double lengthSeconds) {
  const size_t begin = static_cast<size_t>(std::lround(fromSeconds * kRate));
  return rmsOf(x, begin, begin + static_cast<size_t>(std::lround(lengthSeconds * kRate)));
}

const BlockParamSpec* specOf(const char* kind, const char* id) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kind);
  if (info == nullptr)
    return nullptr;
  const int index = findBlockParam(info->params, id);
  return index < 0 ? nullptr : &info->params[static_cast<size_t>(index)];
}

}  // namespace

// ---------------------------------------------------------------------------
// Slapback
// ---------------------------------------------------------------------------

namespace {
const Params kSlapClean = {{"saturation", 0.0}, {"wobble", 0.0}, {"toneHz", 16000.0},
                           {"feedback", 0.0}};
}

TEST(SlapbackDelayTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"timeMs", 120.0}, {"feedback", 35.0}, {"wobble", 40.0}, {"saturation", 50.0}};
  options.stateParams = {{"sync", 1.0},         {"division", fx::kDivSixteenthDotted},
                         {"timeMs", 77.0},      {"feedback", 45.0},
                         {"toneHz", 3000.0},    {"saturation", 60.0},
                         {"wobble", 30.0},      {"duckDb", 9.0},
                         {"duckReleaseMs", 400.0}, {"spillover", 0.0}};
  options.latency = LatencyCheck::AtMostOnset;
  options.cpuAverageBudgetNs = 8000.0;
  options.cpuWorstBudgetNs = 60000.0;
  expectKindConforms("delay.slapback", options);
}

TEST(SlapbackDelayTest, ParametersAreTheDocumentedRanges) {
  const BlockParamSpec* time = specOf("delay.slapback", "timeMs");
  ASSERT_NE(time, nullptr);
  EXPECT_FLOAT_EQ(time->min, 40.0f);
  EXPECT_FLOAT_EQ(time->max, 200.0f);
  EXPECT_FLOAT_EQ(time->defaultValue, 110.0f);
  const BlockParamSpec* feedback = specOf("delay.slapback", "feedback");
  ASSERT_NE(feedback, nullptr);
  EXPECT_FLOAT_EQ(feedback->min, 0.0f);
  EXPECT_FLOAT_EQ(feedback->max, 60.0f);
  EXPECT_NE(specOf("delay.slapback", "toneHz"), nullptr);
  EXPECT_NE(specOf("delay.slapback", "sync"), nullptr);
  EXPECT_NE(specOf("delay.slapback", "division"), nullptr);
}

TEST(SlapbackDelayTest, TimeMatchesTheParameterAcrossTheRange) {
  for (const double timeMs : {40.0, 110.0, 200.0}) {
    SCOPED_TRACE(timeMs);
    const std::vector<float> in = withSilence(noise(0.25, 3u, 0.25f, kRate), ms(timeMs) + ms(100.0));
    const std::vector<float> out = wetOf("delay.slapback", withParams(kSlapClean, {{"timeMs", timeMs}}), in);
    // The tone filter (two poles at 16 kHz) is about one sample of delay.
    EXPECT_NEAR(estimateDelaySamples(out, in, ms(timeMs) - ms(30.0), ms(timeMs) + ms(30.0)), ms(timeMs), 2);
  }
}

TEST(SlapbackDelayTest, SyncedTimeFollowsTheTempo) {
  const std::vector<float> in = withSilence(noise(0.25, 3u, 0.25f, kRate), ms(300.0));
  const Params params = withParams(kSlapClean, {{"sync", 1.0}, {"division", fx::kDivSixteenth}});
  // A sixteenth at 120 BPM is 125 ms, at 100 BPM 150 ms.
  const std::vector<std::pair<double, double>> tempos = {{120.0, 125.0}, {100.0, 150.0}};
  for (const auto& [bpm, expectedMs] : tempos) {
    SCOPED_TRACE(bpm);
    const std::vector<float> out = wetOf("delay.slapback", params, Stereo{in, in}, Layout::Mono, bpm).left;
    EXPECT_NEAR(estimateDelaySamples(out, in, ms(expectedMs) - ms(30.0), ms(expectedMs) + ms(30.0)),
                ms(expectedMs), 2);
  }
}

TEST(SlapbackDelayTest, FeedbackSetsTheRepeatsAndStaysBelowTheCeiling) {
  // 100 ms, 50% feedback: the second repeat is half the first (a low tone,
  // so the tone filter leaves it alone).
  const std::vector<float> in = toneBurst(0.04, 200.0, 0.2f, 0.8);
  const std::vector<float> out =
      wetOf("delay.slapback", withParams(kSlapClean, {{"timeMs", 100.0}, {"feedback", 50.0}}), in);
  const double first = rmsWindow(out, 95.0, 60.0);
  const double second = rmsWindow(out, 195.0, 60.0);
  ASSERT_GT(first, 0.01);
  EXPECT_NEAR(second / first, 0.5, 0.06);

  // Asking for more than the range clamps at 60%.
  DirectKind direct("delay.slapback", {{"feedback", 95.0}});
  const int index = direct.engine().findParameter("feedback");
  EXPECT_FLOAT_EQ(direct.engine().getParameter(index), 60.0f);
}

TEST(SlapbackDelayTest, ToneIsALowPassSweep) {
  const std::vector<float> in = withSilence(noiseBurst(0.3, 9u, 0.3f, 0.5), ms(400.0));
  const auto highShare = [&](double toneHz) {
    const std::vector<float> out =
        wetOf("delay.slapback", withParams(kSlapClean, {{"timeMs", 100.0}, {"toneHz", toneHz}}), in);
    const size_t from = static_cast<size_t>(ms(150.0));
    return bandPower(out, from, 6000.0, 12000.0) / bandPower(out, from, 100.0, 12000.0);
  };
  EXPECT_LT(highShare(1000.0), highShare(16000.0) * 0.05);
  EXPECT_LT(highShare(1000.0), highShare(4000.0));
}

TEST(SlapbackDelayTest, SaturationAddsHarmonicsAndWobbleMovesThePitch) {
  const std::vector<float> loud = sine(1.0, 500.0, 0.8f, kRate);
  const auto thd = [&](double saturation) {
    const std::vector<float> out = wetOf(
        "delay.slapback", withParams(kSlapClean, {{"timeMs", 100.0}, {"saturation", saturation}}), loud);
    return thdPercent(out, 500.0, kRate, static_cast<size_t>(ms(300.0)), 8);
  };
  EXPECT_GT(thd(100.0), thd(0.0) + 1.0);

  const std::vector<float> steady = sine(6.0, 800.0, 0.25f, kRate);
  const size_t skip = static_cast<size_t>(ms(800.0));
  const PitchModulation none = analysePitchModulation(
      wetOf("delay.slapback", withParams(kSlapClean, {{"timeMs", 120.0}, {"wobble", 0.0}}), steady),
      skip, 0.3, 9.0);
  EXPECT_LT(none.worstDeviation, 1.0e-4);
  const PitchModulation wobbly = analysePitchModulation(
      wetOf("delay.slapback", withParams(kSlapClean, {{"timeMs", 120.0}, {"wobble", 100.0}}), steady),
      skip, 0.3, 9.0);
  EXPECT_GT(wobbly.worstDeviation, 0.003);
  EXPECT_LT(wobbly.worstDeviation, 0.02) << "a wobble, not a vibrato";
}

TEST(SlapbackDelayTest, SpilloverRingsOutAfterBypassAndStopsAtTheReportedTail) {
  expectSpilloverBehaviour("delay.slapback",
                           {{"timeMs", 200.0}, {"feedback", 60.0}, {"wobble", 0.0}, {"toneHz", 16000.0}},
                           250.0, 450.0);
}

// ---------------------------------------------------------------------------
// Dual delay
// ---------------------------------------------------------------------------

namespace {
const Params kDualClean = {{"leftTimeMs", 100.0}, {"rightTimeMs", 150.0}, {"leftFeedback", 0.0},
                           {"rightFeedback", 0.0}, {"crossFeedback", 0.0}, {"lowCutHz", 20.0},
                           {"highCutHz", 16000.0}};
}

TEST(DualDelayTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"leftTimeMs", 120.0},   {"rightTimeMs", 180.0}, {"leftFeedback", 40.0},
                    {"rightFeedback", 30.0}, {"crossFeedback", 50.0}};
  options.stateParams = {{"leftSync", 1.0},        {"leftDivision", fx::kDivEighth},
                         {"leftTimeMs", 123.0},    {"rightSync", 1.0},
                         {"rightDivision", fx::kDivQuarterDotted}, {"rightTimeMs", 456.0},
                         {"leftFeedback", 55.0},   {"rightFeedback", 45.0},
                         {"crossFeedback", 35.0},  {"lowCutHz", 150.0},
                         {"highCutHz", 5000.0},    {"duckDb", 6.0},
                         {"duckReleaseMs", 300.0}, {"spillover", 0.0}};
  options.latency = LatencyCheck::AtMostOnset;
  options.cpuAverageBudgetNs = 12000.0;
  options.cpuWorstBudgetNs = 80000.0;
  expectKindConforms("delay.dual", options);
}

TEST(DualDelayTest, ParametersAreTheDocumentedRanges) {
  for (const char* side : {"left", "right"}) {
    const std::string prefix = side;
    const BlockParamSpec* time = specOf("delay.dual", (prefix + "TimeMs").c_str());
    ASSERT_NE(time, nullptr) << side;
    EXPECT_FLOAT_EQ(time->min, 7.0f);
    EXPECT_FLOAT_EQ(time->max, 6000.0f);
    EXPECT_TRUE(time->tempoSync);
    EXPECT_NE(specOf("delay.dual", (prefix + "Sync").c_str()), nullptr) << side;
    EXPECT_NE(specOf("delay.dual", (prefix + "Division").c_str()), nullptr) << side;
    EXPECT_NE(specOf("delay.dual", (prefix + "Feedback").c_str()), nullptr) << side;
  }
  const BlockParamSpec* cross = specOf("delay.dual", "crossFeedback");
  ASSERT_NE(cross, nullptr);
  EXPECT_FLOAT_EQ(cross->max, 100.0f);
  EXPECT_FLOAT_EQ(specOf("delay.dual", "lowCutHz")->min, 20.0f);
  EXPECT_FLOAT_EQ(specOf("delay.dual", "highCutHz")->max, 16000.0f);
}

TEST(DualDelayTest, EachSideHasItsOwnTimeInTrueStereo) {
  const std::vector<std::pair<double, double>> times = {{100.0, 150.0}, {7.0, 333.0}, {2000.0, 500.0}};
  for (const auto& [leftMs, rightMs] : times) {
    SCOPED_TRACE(leftMs);
    const Stereo out = wetOf("delay.dual", withParams(kDualClean, {{"leftTimeMs", leftMs},
                                                                    {"rightTimeMs", rightMs}}),
                             Stereo{impulse(ms(2500.0), 0, 0.5f), impulse(ms(2500.0), 0, 0.5f)},
                             Layout::Stereo);
    EXPECT_NEAR(peakIndexOf(out.left), ms(leftMs), 1);
    EXPECT_NEAR(peakIndexOf(out.right), ms(rightMs), 1);
    EXPECT_NEAR(peakOf(out.left), 0.5f, 1.0e-4f) << "the first echo is the clean copy";
  }
}

TEST(DualDelayTest, EachSideTakesItsOwnInput) {
  const Stereo out = wetOf("delay.dual", kDualClean,
                           Stereo{impulse(ms(500.0), 0, 0.5f), std::vector<float>(static_cast<size_t>(ms(500.0)), 0.0f)},
                           Layout::Stereo);
  EXPECT_NEAR(peakIndexOf(out.left), ms(100.0), 1);
  EXPECT_EQ(peakOf(out.right), 0.0f) << "the right side heard nothing";
}

TEST(DualDelayTest, SyncedTimesFollowTheTempo) {
  const Params params = withParams(kDualClean, {{"leftSync", 1.0},
                                                {"leftDivision", fx::kDivQuarter},
                                                {"rightSync", 1.0},
                                                {"rightDivision", fx::kDivEighthDotted}});
  const std::vector<std::tuple<double, double, double>> tempos = {{120.0, 500.0, 375.0}, {90.0, 666.6667, 500.0}};
  for (const auto& [bpm, leftMs, rightMs] : tempos) {
    SCOPED_TRACE(bpm);
    const Stereo out = wetOf("delay.dual", params,
                             Stereo{impulse(ms(1500.0), 0, 0.5f), impulse(ms(1500.0), 0, 0.5f)},
                             Layout::Stereo, bpm);
    EXPECT_NEAR(peakIndexOf(out.left), ms(leftMs), 1);
    EXPECT_NEAR(peakIndexOf(out.right), ms(rightMs), 1);
  }
}

TEST(DualDelayTest, EachSideHasItsOwnFeedback) {
  const Params params = withParams(kDualClean, {{"leftFeedback", 50.0}, {"rightFeedback", 0.0}});
  const Stereo out = wetOf("delay.dual", params,
                           Stereo{impulse(ms(1200.0), 0, 0.5f), impulse(ms(1200.0), 0, 0.5f)},
                           Layout::Stereo);
  const float first = peakOf(out.left, static_cast<size_t>(ms(95.0)), static_cast<size_t>(ms(105.0)));
  const float repeat = peakOf(out.left, static_cast<size_t>(ms(195.0)), static_cast<size_t>(ms(205.0)));
  EXPECT_NEAR(repeat / first, 0.5 * 0.77, 0.08) << "50% of the first, less the high cut's rounding";
  EXPECT_LT(peakOf(out.right, static_cast<size_t>(ms(160.0))), 1.0e-6f) << "the right side has no repeats";
}

TEST(DualDelayTest, CrossFeedbackSendsTheRepeatsToTheOtherSide) {
  // A click on the left only. At 0% cross-feedback the left repeats on the
  // left (100, 200, 300 ms) and the right stays silent. At 100% the repeat
  // goes over: the left echo at 100 ms feeds the right line, which plays it
  // 150 ms later, at 250 ms, on the right.
  const Stereo in{impulse(ms(1500.0), 0, 0.5f), std::vector<float>(static_cast<size_t>(ms(1500.0)), 0.0f)};
  const Params base = withParams(kDualClean, {{"leftFeedback", 60.0}, {"rightFeedback", 60.0}});

  const Stereo apart = wetOf("delay.dual", withParams(base, {{"crossFeedback", 0.0}}), in, Layout::Stereo);
  EXPECT_GT(peakOf(apart.left, static_cast<size_t>(ms(195.0)), static_cast<size_t>(ms(205.0))), 0.1f);
  EXPECT_EQ(peakOf(apart.right), 0.0f);

  const Stereo crossed = wetOf("delay.dual", withParams(base, {{"crossFeedback", 100.0}}), in, Layout::Stereo);
  EXPECT_GT(peakOf(crossed.left, static_cast<size_t>(ms(95.0)), static_cast<size_t>(ms(105.0))), 0.3f);
  EXPECT_LT(peakOf(crossed.left, static_cast<size_t>(ms(150.0)), static_cast<size_t>(ms(300.0))), 1.0e-4f)
      << "the left repeat at 200 ms is gone (the next one is at 350 ms, back from the right)";
  const float over = peakOf(crossed.right, static_cast<size_t>(ms(245.0)), static_cast<size_t>(ms(255.0)));
  EXPECT_NEAR(over / 0.5f, 0.6f * 0.77f, 0.08f) << "the repeat arrives on the right, 60% down";

  // Halfway, both.
  const Stereo half = wetOf("delay.dual", withParams(base, {{"crossFeedback", 50.0}}), in, Layout::Stereo);
  EXPECT_GT(peakOf(half.left, static_cast<size_t>(ms(195.0)), static_cast<size_t>(ms(205.0))), 0.05f);
  EXPECT_GT(peakOf(half.right, static_cast<size_t>(ms(245.0)), static_cast<size_t>(ms(255.0))), 0.05f);
}

TEST(DualDelayTest, TheFiltersShapeOnlyTheRepeats) {
  // A bright burst: the repeat through a 1 kHz high cut is far darker, the
  // first echo untouched.
  const std::vector<float> burst = withSilence(noiseBurst(0.1, 5u, 0.3f, 0.1), ms(900.0));
  const auto repeatHigh = [&](double highCut) {
    const Stereo out = wetOf("delay.dual",
                             withParams(kDualClean, {{"leftTimeMs", 200.0}, {"rightTimeMs", 200.0},
                                                     {"leftFeedback", 70.0}, {"rightFeedback", 70.0},
                                                     {"highCutHz", highCut}}),
                             Stereo{burst, burst}, Layout::Stereo);
    // The first echo starts at 200 ms and the second 200 ms later.
    return std::make_pair(bandPower(out.left, static_cast<size_t>(ms(410.0)), 4000.0, 12000.0),
                          bandPower(out.left, static_cast<size_t>(ms(210.0)), 4000.0, 12000.0));
  };
  const auto dark = repeatHigh(1000.0);
  const auto bright = repeatHigh(16000.0);
  ASSERT_GT(bright.first, 0.0);
  EXPECT_LT(dark.first, bright.first * 0.01);
  EXPECT_NEAR(dark.second / bright.second, 1.0, 0.05) << "the first echo does not depend on the cut";
}

TEST(DualDelayTest, AMonoRowSumsBothDelaysIntoOneSignal) {
  // Both times equal: the same level as a single delay.
  const Stereo same = wetOf("delay.dual",
                            withParams(kDualClean, {{"leftTimeMs", 100.0}, {"rightTimeMs", 100.0}}),
                            Stereo{impulse(ms(500.0), 0, 0.5f), impulse(ms(500.0), 0, 0.5f)}, Layout::Mono);
  EXPECT_NEAR(peakOf(same.left), 0.5f, 1.0e-4f);
  EXPECT_EQ(same.left, same.right) << "a mono row has one signal";

  // Different times: both taps are there, at half level each.
  const Stereo out = wetOf("delay.dual", kDualClean,
                           Stereo{impulse(ms(500.0), 0, 0.5f), impulse(ms(500.0), 0, 0.5f)}, Layout::Mono);
  EXPECT_NEAR(out.left[static_cast<size_t>(ms(100.0))], 0.25f, 1.0e-4f);
  EXPECT_NEAR(out.left[static_cast<size_t>(ms(150.0))], 0.25f, 1.0e-4f);
}

TEST(DualDelayTest, DualMonoKeepsTheTwoSidesApart) {
  const Stereo out = wetOf("delay.dual", withParams(kDualClean, {{"leftFeedback", 50.0}, {"crossFeedback", 100.0}}),
                           Stereo{impulse(ms(800.0), 0, 0.5f), std::vector<float>(static_cast<size_t>(ms(800.0)), 0.0f)},
                           Layout::DualMono);
  EXPECT_GT(peakOf(out.left), 0.1f);
  EXPECT_EQ(peakOf(out.right), 0.0f) << "nothing crossed into the other channel";
}

TEST(DualDelayTest, SpilloverRingsOutAfterBypassAndStopsAtTheReportedTail) {
  expectSpilloverBehaviour("delay.dual",
                           {{"leftTimeMs", 200.0}, {"rightTimeMs", 300.0}, {"leftFeedback", 60.0},
                            {"rightFeedback", 60.0}, {"crossFeedback", 40.0}, {"lowCutHz", 20.0},
                            {"highCutHz", 16000.0}},
                           350.0, 650.0);
}

// ---------------------------------------------------------------------------
// Reverse delay
// ---------------------------------------------------------------------------

namespace {
const Params kReverseClean = {{"feedback", 0.0}, {"toneHz", 20000.0}};
}

TEST(ReverseDelayTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"timeMs", 150.0}, {"feedback", 40.0}, {"toneHz", 6000.0}};
  options.stateParams = {{"sync", 1.0},          {"division", fx::kDivEighth},
                         {"timeMs", 333.0},      {"feedback", 55.0},
                         {"toneHz", 3000.0},     {"duckDb", 6.0},
                         {"duckReleaseMs", 300.0}, {"spillover", 0.0}};
  options.latency = LatencyCheck::AtMostOnset;
  // The conformance signal is a few steady tones, and the reversed echo of a
  // tone is a phase-locked copy of it: beside the dry signal (and, fed back,
  // beside the live input) the sum's level depends on where the host's
  // resampling latency leaves each tone's phase against the chunk grid, up to
  // 6 dB at 44.1 kHz against 48 kHz. That is the nature of a reverse delay,
  // not a rate dependence of the effect (wet only, the levels match exactly).
  options.rateToleranceDb = 8.0;
  options.cpuAverageBudgetNs = 10000.0;
  options.cpuWorstBudgetNs = 70000.0;
  expectKindConforms("delay.reverse", options);
}

TEST(ReverseDelayTest, ParametersAreTheDocumentedRanges) {
  const BlockParamSpec* time = specOf("delay.reverse", "timeMs");
  ASSERT_NE(time, nullptr);
  EXPECT_FLOAT_EQ(time->min, 100.0f);
  EXPECT_FLOAT_EQ(time->max, 2000.0f);
  EXPECT_TRUE(time->tempoSync);
  EXPECT_NE(specOf("delay.reverse", "sync"), nullptr);
  EXPECT_NE(specOf("delay.reverse", "feedback"), nullptr);
  EXPECT_NE(specOf("delay.reverse", "toneHz"), nullptr);
}

TEST(ReverseDelayTest, ARampComesBackReversed) {
  // A ramp 0 to 1 over one chunk (200 ms). The chunk that follows plays it
  // from its newest sample to its oldest: a ramp from 1 down to 0, sample for
  // sample, once the fade at the start of the voice is over.
  const int chunk = ms(200.0);
  std::vector<float> in(static_cast<size_t>(ms(800.0)), 0.0f);
  for (int n = 0; n < chunk; ++n)
    in[static_cast<size_t>(n)] = 0.8f * static_cast<float>(n) / static_cast<float>(chunk);
  const std::vector<float> out = wetOf("delay.reverse", withParams(kReverseClean, {{"timeMs", 200.0}}), in);

  const int fade = ms(50.0);  // a quarter of the chunk
  int checked = 0;
  for (int n = fade + 10; n < chunk - 10; n += 7) {
    const float expected = in[static_cast<size_t>(chunk - 1 - n)];
    ASSERT_NEAR(out[static_cast<size_t>(chunk + n)], expected, 0.01f) << "n = " << n;
    ++checked;
  }
  EXPECT_GT(checked, 900);
  // And it really is falling, where the input rose.
  EXPECT_GT(out[static_cast<size_t>(chunk + fade + 50)], out[static_cast<size_t>(chunk + chunk - 60)] + 0.3f);
}

TEST(ReverseDelayTest, ANoteSwellsInBackwards) {
  // A decaying 300 Hz burst: the reversed copy grows instead of dying away.
  const int chunk = ms(400.0);
  std::vector<float> in(static_cast<size_t>(ms(1500.0)), 0.0f);
  for (int n = 0; n < chunk; ++n)
    in[static_cast<size_t>(n)] = 0.6f * std::exp(-6.0f * static_cast<float>(n) / static_cast<float>(chunk)) *
                                 std::sin(2.0f * 3.14159265f * 300.0f * static_cast<float>(n) / 48000.0f);
  const std::vector<float> out = wetOf("delay.reverse", withParams(kReverseClean, {{"timeMs", 400.0}}), in);
  const double early = rmsOf(out, static_cast<size_t>(chunk + ms(60.0)), static_cast<size_t>(chunk + ms(120.0)));
  const double late = rmsOf(out, static_cast<size_t>(chunk + ms(280.0)), static_cast<size_t>(chunk + ms(340.0)));
  EXPECT_GT(late, early * 4.0);
}

TEST(ReverseDelayTest, ChunkEdgesAreCrossfadedWithNoClick) {
  for (const double timeMs : {100.0, 500.0}) {
    SCOPED_TRACE(timeMs);
    // A steady sine has a known largest step; the reversed copy of it, joined
    // chunk to chunk, must not exceed it by more than the fade's own slope.
    const std::vector<float> in = sine(6.0, 440.0, 0.5f, kRate);
    const std::vector<float> out =
        wetOf("delay.reverse", withParams(kReverseClean, {{"timeMs", timeMs}}), in);
    const float steady = maxStep(in, in.size() / 2, in.size());
    const float worst = maxStep(out, static_cast<size_t>(ms(timeMs * 2.0 + 100.0)), out.size());
    EXPECT_LT(worst, steady * 1.7f);
    expectCleanSignal(out, 1.1f, "reverse delay on a sine");
  }
}

TEST(ReverseDelayTest, TheJoinsKeepTheirLevelOnNoise) {
  // Chunks are not correlated, so an equal-power crossfade keeps the level
  // through every join: no 10 ms window of the steady part falls well below
  // the average.
  const std::vector<float> in = noise(6.0, 21u, 0.25f, kRate);
  const std::vector<float> out = wetOf("delay.reverse", withParams(kReverseClean, {{"timeMs", 300.0}}), in);
  std::vector<double> windows;
  for (double t = 1.0; t < 5.5; t += 0.01)
    windows.push_back(windowRms(out, t, 0.01));
  const double average = std::accumulate(windows.begin(), windows.end(), 0.0) / static_cast<double>(windows.size());
  const double lowest = *std::min_element(windows.begin(), windows.end());
  const double highest = *std::max_element(windows.begin(), windows.end());
  EXPECT_GT(lowest, average * 0.8);
  EXPECT_LT(highest, average * 1.25);
}

TEST(ReverseDelayTest, SyncedChunkFollowsTheTempo) {
  // A quarter note at 120 BPM is 500 ms: the ramp's reversed copy starts there.
  const int chunk = ms(500.0);
  std::vector<float> in(static_cast<size_t>(ms(1200.0)), 0.0f);
  for (int n = 0; n < chunk; ++n)
    in[static_cast<size_t>(n)] = 0.8f * static_cast<float>(n) / static_cast<float>(chunk);
  const std::vector<float> out = wetOf("delay.reverse", withParams(kReverseClean, {{"sync", 1.0}, {"division", fx::kDivQuarter}}),
                                       Stereo{in, in}, Layout::Mono, 120.0).left;
  EXPECT_NEAR(out[static_cast<size_t>(chunk + ms(200.0))], in[static_cast<size_t>(chunk - 1 - ms(200.0))], 0.01f);
}

TEST(ReverseDelayTest, FeedbackBringsTheChunkBackForward) {
  // With feedback, the reversed chunk is recorded again and read backwards
  // again: a second, forward pass of the ramp, quieter by the feedback.
  const int chunk = ms(200.0);
  std::vector<float> in(static_cast<size_t>(ms(2000.0)), 0.0f);
  for (int n = 0; n < chunk; ++n)
    in[static_cast<size_t>(n)] = 0.8f * static_cast<float>(n) / static_cast<float>(chunk);
  const std::vector<float> out = wetOf("delay.reverse", withParams(kReverseClean, {{"timeMs", 200.0}, {"feedback", 50.0}}), in);
  const double first = rmsOf(out, static_cast<size_t>(chunk), static_cast<size_t>(2 * chunk));
  double later = 0.0;
  for (size_t at = static_cast<size_t>(2 * chunk); at < out.size() - static_cast<size_t>(chunk); at += static_cast<size_t>(chunk))
    later = std::max(later, rmsOf(out, at, at + static_cast<size_t>(chunk)));
  EXPECT_GT(later, first * 0.1) << "a repeat follows";
  EXPECT_LT(later, first) << "and it is quieter";
}

TEST(ReverseDelayTest, SpilloverRingsOutAfterBypassAndStopsAtTheReportedTail) {
  // The wet is a reversed chunk of the noise burst; the residue the rig's DC
  // blocker leaves after a cut is the burst's local mean, so the bound is looser
  // than the default (still about 1% of the signal).
  expectSpilloverBehaviour("delay.reverse", {{"timeMs", 200.0}, {"feedback", 0.0}, {"toneHz", 20000.0}},
                           100.0, 400.0, 6.0e-3f);
}

// ---------------------------------------------------------------------------
// Reverse delay: the engine shared with the dual reverse
// ---------------------------------------------------------------------------

namespace {

// FNV-1a over the bits of every sample, so the check is on the exact output.
uint64_t hashOf(const Stereo& s) {
  uint64_t h = 1469598103934665603ull;
  for (const std::vector<float>* channel : {&s.left, &s.right})
    for (const float v : *channel) {
      uint32_t bits;
      std::memcpy(&bits, &v, sizeof(bits));
      h = (h ^ bits) * 1099511628211ull;
    }
  return h;
}

}  // namespace

TEST(ReverseDelayTest, OutputIsBitIdenticalToTheVersionBeforeTheEngineWasShared) {
  // Hashes taken from the delay.reverse that had the voices inline (before
  // ReverseWindow.h): any change to the shared engine that moves one bit of
  // the reverse delay's output fails here. Ducking, tone, feedback, a synced
  // time and every layout are covered.
  const std::vector<float> in = noise(4.0, 33u, 0.4f, kRate);
  const std::vector<float> other = noise(4.0, 34u, 0.4f, kRate);
  const std::vector<std::pair<Params, Layout>> cases = {
      {{{"timeMs", 230.0}, {"feedback", 55.0}, {"toneHz", 5000.0}}, Layout::Mono},
      {{{"timeMs", 100.0}, {"feedback", 90.0}, {"duckDb", 9.0}}, Layout::Stereo},
      {{{"sync", 1.0}, {"division", fx::kDivEighthDotted}, {"feedback", 40.0}}, Layout::DualMono},
      {{{"timeMs", 2000.0}, {"feedback", 20.0}, {"toneHz", 800.0}}, Layout::Stereo},
  };
  const uint64_t golden[] = {17033785850139335071ull, 5427004574353486415ull,
                              4982159417271987979ull, 6777288674567911168ull};
  for (size_t n = 0; n < cases.size(); ++n) {
    SCOPED_TRACE(n);
    const uint64_t hash = hashOf(wetOf("delay.reverse", cases[n].first, Stereo{in, other}, cases[n].second));
    EXPECT_EQ(hash, golden[n]) << "hash " << hash << "ull";
  }
}

// ---------------------------------------------------------------------------
// Dual reverse delay
// ---------------------------------------------------------------------------

namespace {

const Params kDualReverseClean = {{"leftTimeMs", 200.0}, {"rightTimeMs", 300.0},
                                  {"leftFeedback", 0.0}, {"rightFeedback", 0.0},
                                  {"crossFeedback", 0.0}, {"toneHz", 20000.0},
                                  {"lowCutHz", 20.0}};

// A 300 Hz tone that grows over `chunkMs`, then silence up to `totalMs`: a
// shape that is not symmetrical, so its reversal is told from a copy.
std::vector<float> growingTone(double chunkMs, double totalMs) {
  std::vector<float> x(static_cast<size_t>(ms(totalMs)), 0.0f);
  const int chunk = ms(chunkMs);
  for (int n = 0; n < chunk; ++n)
    x[static_cast<size_t>(n)] = 0.6f * (static_cast<float>(n) / static_cast<float>(chunk)) *
                                std::sin(2.0f * 3.14159265f * 300.0f * static_cast<float>(n) / 48000.0f);
  return x;
}

// How well `out`, in the chunk after the first `chunk` samples, follows `in`
// played backwards from its sample `chunk - 1` (correlation coefficient, past
// the fade at the start of the voice).
double reversedMatch(const std::vector<float>& out, const std::vector<float>& in, int chunk, int length) {
  double xy = 0.0, xx = 0.0, yy = 0.0;
  const int fade = std::min(ms(60.0), length / 4);
  for (int n = fade + 10; n < length - 10; ++n) {
    const double a = out[static_cast<size_t>(chunk + n)];
    const double b = in[static_cast<size_t>(chunk - 1 - n)];
    xy += a * b;
    xx += a * a;
    yy += b * b;
  }
  return xy / std::sqrt(xx * yy + 1.0e-30);
}

}  // namespace

TEST(DualReverseDelayTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"leftTimeMs", 150.0},   {"rightTimeMs", 210.0}, {"leftFeedback", 40.0},
                    {"rightFeedback", 30.0}, {"crossFeedback", 50.0}, {"toneHz", 6000.0}};
  options.stateParams = {{"leftSync", 1.0},        {"leftDivision", fx::kDivEighth},
                         {"leftTimeMs", 333.0},    {"rightSync", 1.0},
                         {"rightDivision", fx::kDivQuarterDotted}, {"rightTimeMs", 456.0},
                         {"leftFeedback", 55.0},   {"rightFeedback", 45.0},
                         {"crossFeedback", 35.0},  {"toneHz", 3000.0},
                         {"lowCutHz", 150.0},      {"duckDb", 6.0},
                         {"duckReleaseMs", 300.0}, {"spillover", 0.0}};
  options.latency = LatencyCheck::AtMostOnset;
  // See ReverseDelayTest.Conforms: the reversed echo of a steady tone is a
  // phase-locked copy of it, so the sum's level depends on resampling phase.
  options.rateToleranceDb = 8.0;
  options.cpuAverageBudgetNs = 20000.0;
  options.cpuWorstBudgetNs = 120000.0;
  expectKindConforms("delay.dualreverse", options);
}

TEST(DualReverseDelayTest, ParametersAreTheDocumentedRanges) {
  for (const char* side : {"left", "right"}) {
    const std::string prefix = side;
    const BlockParamSpec* time = specOf("delay.dualreverse", (prefix + "TimeMs").c_str());
    ASSERT_NE(time, nullptr) << side;
    EXPECT_FLOAT_EQ(time->min, 100.0f) << "the range of delay.reverse";
    EXPECT_FLOAT_EQ(time->max, 2000.0f);
    EXPECT_TRUE(time->tempoSync);
    EXPECT_NE(specOf("delay.dualreverse", (prefix + "Sync").c_str()), nullptr) << side;
    EXPECT_NE(specOf("delay.dualreverse", (prefix + "Division").c_str()), nullptr) << side;
    EXPECT_NE(specOf("delay.dualreverse", (prefix + "Feedback").c_str()), nullptr) << side;
  }
  const BlockParamSpec* cross = specOf("delay.dualreverse", "crossFeedback");
  ASSERT_NE(cross, nullptr);
  EXPECT_FLOAT_EQ(cross->min, 0.0f);
  EXPECT_FLOAT_EQ(cross->max, 100.0f);
  EXPECT_NE(specOf("delay.dualreverse", "toneHz"), nullptr);
  EXPECT_NE(specOf("delay.dualreverse", "lowCutHz"), nullptr);
}

TEST(DualReverseDelayTest, EachSideReversesItsOwnWindowAtItsOwnTime) {
  // Left hears a tone that grows over 200 ms, right one that grows over 300
  // ms. Each comes back reversed (shrinking) right after its own chunk.
  const std::vector<float> left = growingTone(200.0, 1500.0);
  const std::vector<float> right = growingTone(300.0, 1500.0);
  const Stereo out = wetOf("delay.dualreverse", kDualReverseClean, Stereo{left, right}, Layout::Stereo);
  EXPECT_GT(reversedMatch(out.left, left, ms(200.0), ms(200.0)), 0.95);
  EXPECT_GT(reversedMatch(out.right, right, ms(300.0), ms(300.0)), 0.95);
  // The left line is not on the right's schedule: read at the other chunk
  // length the match falls apart.
  EXPECT_LT(std::abs(reversedMatch(out.left, left, ms(300.0), ms(200.0))), 0.5);
  // And nothing of either input is heard before its chunk is over.
  EXPECT_LT(windowRms(out.right, 0.0, 0.25), 1.0e-4);
}

TEST(DualReverseDelayTest, EachSideTakesItsOwnInput) {
  const std::vector<float> tone = growingTone(200.0, 1000.0);
  const Stereo out = wetOf("delay.dualreverse", kDualReverseClean,
                           Stereo{tone, std::vector<float>(tone.size(), 0.0f)}, Layout::Stereo);
  EXPECT_GT(windowRms(out.left, 0.2, 0.2), 0.05);
  EXPECT_EQ(peakOf(out.right), 0.0f) << "the right line heard nothing";
}

TEST(DualReverseDelayTest, AMonoInputFeedsBothLinesAndAMonoRowHearsTheirSum) {
  const std::vector<float> in = growingTone(300.0, 1500.0);
  // The one input goes to both sides: the same chunk length on both gives
  // two equal outputs.
  const Params same = withParams(kDualReverseClean, {{"leftTimeMs", 300.0}, {"rightTimeMs", 300.0}});
  const Stereo image = wetOf("delay.dualreverse", same, Stereo{in, in}, Layout::Stereo);
  EXPECT_EQ(image.left, image.right);
  // One channel: the sum of the two lines (different times and feedback, so
  // the check is not about equal lines), sample for sample.
  const Params mixed = withParams(kDualReverseClean, {{"leftTimeMs", 200.0}, {"rightTimeMs", 350.0},
                                                      {"leftFeedback", 50.0}, {"rightFeedback", 40.0},
                                                      {"crossFeedback", 40.0}});
  const Stereo pair = wetOf("delay.dualreverse", mixed, Stereo{in, in}, Layout::Stereo);
  const std::vector<float> mono = wetOf("delay.dualreverse", mixed, in, Layout::Mono);
  ASSERT_EQ(mono.size(), pair.left.size());
  float worst = 0.0f;
  for (size_t i = 0; i < mono.size(); ++i)
    worst = std::max(worst, std::abs(mono[i] - (pair.left[i] + pair.right[i])));
  EXPECT_LT(worst, 1.0e-5f);
  EXPECT_GT(peakOf(mono), 0.05f);
}

TEST(DualReverseDelayTest, SyncedTimesFollowTheTempo) {
  // A quarter note at 120 BPM is 500 ms on the left, a dotted eighth 375 ms on
  // the right: each reversed chunk starts at its own time.
  const Params params = withParams(kDualReverseClean, {{"leftSync", 1.0},
                                                       {"leftDivision", fx::kDivQuarter},
                                                       {"rightSync", 1.0},
                                                       {"rightDivision", fx::kDivEighthDotted}});
  const std::vector<float> left = growingTone(500.0, 2000.0);
  const std::vector<float> right = growingTone(375.0, 2000.0);
  const Stereo out = wetOf("delay.dualreverse", params, Stereo{left, right}, Layout::Stereo, 120.0);
  EXPECT_GT(reversedMatch(out.left, left, ms(500.0), ms(500.0)), 0.95);
  EXPECT_GT(reversedMatch(out.right, right, ms(375.0), ms(375.0)), 0.95);
}

TEST(DualReverseDelayTest, CrossFeedbackMovesEnergyToTheOtherSide) {
  // Noise on the left only, equal times. Apart, the right stays silent. With
  // all the feedback crossing, the left's reversed chunk is recorded into the
  // right line and comes out of the right, and the left line no longer
  // repeats itself.
  const std::vector<float> burst = noise(0.2, 8u, 0.4f, kRate);
  std::vector<float> in(static_cast<size_t>(ms(3000.0)), 0.0f);
  std::copy(burst.begin(), burst.end(), in.begin());
  const Stereo input{in, std::vector<float>(in.size(), 0.0f)};
  const Params base = withParams(kDualReverseClean, {{"leftTimeMs", 200.0}, {"rightTimeMs", 200.0},
                                                     {"leftFeedback", 70.0}, {"rightFeedback", 70.0}});

  const Stereo apart = wetOf("delay.dualreverse", withParams(base, {{"crossFeedback", 0.0}}), input, Layout::Stereo);
  EXPECT_EQ(peakOf(apart.right), 0.0f);
  const double firstEcho = windowRms(apart.left, 0.2, 0.2);
  ASSERT_GT(firstEcho, 0.02);
  const double ownRepeat = windowRms(apart.left, 0.4, 0.2);

  const Stereo crossed = wetOf("delay.dualreverse", withParams(base, {{"crossFeedback", 100.0}}), input, Layout::Stereo);
  EXPECT_NEAR(windowRms(crossed.left, 0.2, 0.2), firstEcho, firstEcho * 0.01) << "the first echo is the same";
  EXPECT_GT(windowRms(crossed.right, 0.4, 0.2), firstEcho * 0.3) << "the repeat arrives on the right";
  EXPECT_LT(windowRms(crossed.left, 0.4, 0.2), ownRepeat * 0.2) << "and the left no longer repeats itself";
}

TEST(DualReverseDelayTest, TheTailCoversTheDecayOfBothSides) {
  // A burst, then silence, with feedback on both sides and half of it
  // crossing: the level falls to -60 dB inside the reported tail, which is
  // not a blanket maximum.
  const Params params = withParams(kDualReverseClean, {{"leftTimeMs", 300.0}, {"rightTimeMs", 450.0},
                                                       {"leftFeedback", 60.0}, {"rightFeedback", 60.0},
                                                       {"crossFeedback", 50.0}});
  const std::vector<float> burst = noise(0.3, 12u, 0.4f, kRate);
  std::vector<float> in(static_cast<size_t>(ms(20000.0)), 0.0f);
  std::copy(burst.begin(), burst.end(), in.begin());
  DirectKind direct("delay.dualreverse", params, Layout::Stereo);
  const Stereo out = direct.render(Stereo{in, in});
  const float peak = std::max(peakOf(out.left), peakOf(out.right));
  const double decay = std::max(decayTimeSeconds(out.left, burst.size(), peak, kRate),
                                decayTimeSeconds(out.right, burst.size(), peak, kRate));
  EXPECT_GT(decay, 2.0) << "the feedback does ring";
  EXPECT_LE(decay, direct.tailSeconds() + 0.1);
  expectCleanSignal(out.left, 1.5f, "dual reverse tail");

  // No feedback, a short tail; a longer line or more feedback, a longer one.
  const double dry = DirectKind("delay.dualreverse", withParams(params, {{"leftFeedback", 0.0}, {"rightFeedback", 0.0}}))
                         .tailSeconds();
  EXPECT_LT(dry, 3.0);
  EXPECT_GT(direct.tailSeconds(), dry);
  EXPECT_GT(DirectKind("delay.dualreverse", withParams(params, {{"rightTimeMs", 1500.0}})).tailSeconds(),
            direct.tailSeconds());
}

TEST(DualReverseDelayTest, FeedbackAtItsMaximumStaysBoundedAndFreeOfDenormals) {
  const Params params = withParams(kDualReverseClean, {{"leftFeedback", 90.0}, {"rightFeedback", 90.0},
                                                       {"crossFeedback", 100.0}});
  const std::vector<float> in = noise(2.0, 3u, 1.0f, kRate);
  const Stereo out = wetOf("delay.dualreverse", params, Stereo{in, in}, Layout::Stereo);
  expectCleanSignal(out.left, 1.5f, "dual reverse at full feedback");
  expectCleanSignal(out.right, 1.5f, "dual reverse at full feedback");
}

TEST(DualReverseDelayTest, SpilloverRingsOutAfterBypassAndStopsAtTheReportedTail) {
  expectSpilloverBehaviour("delay.dualreverse",
                           {{"leftTimeMs", 200.0}, {"rightTimeMs", 300.0}, {"leftFeedback", 35.0},
                            {"rightFeedback", 35.0}, {"crossFeedback", 40.0}, {"toneHz", 20000.0},
                            {"lowCutHz", 20.0}},
                           100.0, 400.0, 1.2e-2f);
}

// ---------------------------------------------------------------------------
// Multi-tap delay
// ---------------------------------------------------------------------------

namespace {

const Params kTapsClean = {{"feedback", 0.0}, {"diffusion", 0.0}, {"lowCutHz", 20.0},
                           {"highCutHz", 16000.0}};

// The level of the tap near sample `at`: the sum of the samples around it.
// A tap between two samples is shared between them, and the interpolation's
// weights sum to 1, so the sum is the tap's gain whatever the fraction.
float tapLevel(const std::vector<float>& x, int at) {
  float sum = 0.0f;
  for (int i = at - 3; i <= at + 3; ++i)
    sum += x[static_cast<size_t>(i)];
  return sum;
}

// The fractions of the time the taps sit at, by the pattern's definition.
std::vector<double> expectedPositions(int pattern, int count) {
  std::vector<double> gaps;
  for (int k = 0; k < count; ++k) {
    switch (pattern) {
      case 1: gaps.push_back(std::pow(0.72, k)); break;
      case 2: gaps.push_back(std::pow(0.72, count - 1 - k)); break;
      case 3: gaps.push_back(k % 2 == 0 ? 2.0 : 1.0); break;
      default: gaps.push_back(1.0); break;
    }
  }
  const double total = std::accumulate(gaps.begin(), gaps.end(), 0.0);
  std::vector<double> positions;
  double run = 0.0;
  for (const double gap : gaps) {
    run += gap;
    positions.push_back(run / total);
  }
  return positions;
}

}  // namespace

TEST(MultiTapDelayTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"timeMs", 400.0}, {"taps", 6.0},      {"pattern", 3.0},
                    {"diffusion", 50.0}, {"feedback", 40.0}};
  options.stateParams = {{"sync", 1.0},          {"division", fx::kDivEighthDotted},
                         {"timeMs", 321.0},      {"taps", 8.0},
                         {"pattern", 2.0},       {"diffusion", 70.0},
                         {"feedback", 55.0},     {"lowCutHz", 150.0},
                         {"highCutHz", 5000.0},  {"duckDb", 6.0},
                         {"duckReleaseMs", 300.0}, {"spillover", 0.0}};
  options.latency = LatencyCheck::AtMostOnset;
  options.cpuAverageBudgetNs = 25000.0;
  options.cpuWorstBudgetNs = 120000.0;
  expectKindConforms("delay.multitap", options);
}

TEST(MultiTapDelayTest, ParametersAreTheDocumentedRanges) {
  const BlockParamSpec* taps = specOf("delay.multitap", "taps");
  ASSERT_NE(taps, nullptr);
  EXPECT_FLOAT_EQ(taps->min, 4.0f);
  EXPECT_FLOAT_EQ(taps->max, 8.0f);
  const BlockParamSpec* pattern = specOf("delay.multitap", "pattern");
  ASSERT_NE(pattern, nullptr);
  EXPECT_EQ(pattern->choices.size(), 4);
  for (const char* id : {"diffusion", "feedback", "lowCutHz", "highCutHz", "sync", "division", "timeMs"})
    EXPECT_NE(specOf("delay.multitap", id), nullptr) << id;
}

TEST(MultiTapDelayTest, TapCountAndPatternTimingMatchTheDefinition) {
  const double timeMs = 600.0;
  for (const int pattern : {0, 1, 2, 3}) {
    for (const int count : {4, 5, 8}) {
      SCOPED_TRACE(pattern * 10 + count);
      const std::vector<float> out = wetOf(
          "delay.multitap",
          withParams(kTapsClean, {{"timeMs", timeMs}, {"taps", static_cast<double>(count)},
                                  {"pattern", static_cast<double>(pattern)}}),
          impulse(ms(900.0), 0, 0.5f));
      const float top = peakOf(out);
      ASSERT_GT(top, 0.05f);
      const std::vector<int> found = peaksOf(out, top * 0.04f);
      const std::vector<double> wanted = expectedPositions(pattern, count);
      ASSERT_EQ(found.size(), wanted.size()) << "tap count";
      for (size_t k = 0; k < wanted.size(); ++k)
        EXPECT_NEAR(found[k], wanted[k] * ms(timeMs), 2.0) << "tap " << k;
      // Each tap a little quieter than the one before: 0.8 of it (the sum of a
      // few samples around the tap is its level whatever the fraction).
      for (size_t k = 1; k < found.size(); ++k)
        EXPECT_NEAR(tapLevel(out, found[k]) / tapLevel(out, found[k - 1]), 0.8, 0.03) << "tap " << k;
    }
  }
}

TEST(MultiTapDelayTest, PatternsDifferInTheirSpacing) {
  const auto gapsOf = [&](int pattern) {
    const std::vector<float> out = wetOf(
        "delay.multitap",
        withParams(kTapsClean, {{"timeMs", 600.0}, {"taps", 6.0}, {"pattern", static_cast<double>(pattern)}}),
        impulse(ms(900.0), 0, 0.5f));
    const std::vector<int> found = peaksOf(out, peakOf(out) * 0.04f);
    std::vector<int> gaps;
    for (size_t k = 1; k < found.size(); ++k)
      gaps.push_back(found[k] - found[k - 1]);
    return gaps;
  };
  const std::vector<int> even = gapsOf(0);
  const std::vector<int> accelerating = gapsOf(1);
  const std::vector<int> decelerating = gapsOf(2);
  const std::vector<int> swing = gapsOf(3);
  ASSERT_EQ(even.size(), 5u);
  EXPECT_NEAR(even.front(), even.back(), 3);
  EXPECT_GT(accelerating.front(), accelerating.back() * 2);
  EXPECT_LT(decelerating.front() * 2, decelerating.back());
  // The first tap sits one long gap in; after it short, long, short, long.
  EXPECT_NEAR(swing[1], 2 * swing[0], 4) << "short, long, short, long";
  EXPECT_NEAR(swing[3], 2 * swing[2], 4);
}

TEST(MultiTapDelayTest, SyncedTimeFollowsTheTempo) {
  const std::vector<float> out = wetOf(
      "delay.multitap", withParams(kTapsClean, {{"sync", 1.0}, {"division", fx::kDivQuarter}, {"taps", 4.0}}),
      Stereo{impulse(ms(900.0), 0, 0.5f), impulse(ms(900.0), 0, 0.5f)}, Layout::Mono, 100.0).left;
  const std::vector<int> found = peaksOf(out, peakOf(out) * 0.04f);
  ASSERT_EQ(found.size(), 4u);
  EXPECT_NEAR(found.back(), ms(600.0), 2) << "a quarter at 100 BPM, the last tap at the full time";
}

TEST(MultiTapDelayTest, FeedbackRepeatsTheWholeClusterEveryTime) {
  const double timeMs = 400.0;
  const std::vector<float> out = wetOf(
      "delay.multitap",
      withParams(kTapsClean, {{"timeMs", timeMs}, {"taps", 4.0}, {"feedback", 50.0}}),
      impulse(ms(1500.0), 0, 0.5f));
  const std::vector<int> found = peaksOf(out, peakOf(out) * 0.04f);
  ASSERT_GE(found.size(), 8u);
  const std::vector<double> wanted = expectedPositions(0, 4);
  // The second cluster sits one time later, the same four taps.
  for (size_t k = 0; k < 4; ++k)
    EXPECT_NEAR(found[4 + k], (1.0 + wanted[k]) * ms(timeMs), 3.0) << "repeat tap " << k;
  EXPECT_LT(tapLevel(out, found[7]), tapLevel(out, found[3])) << "the repeat is quieter";
}

TEST(MultiTapDelayTest, DiffusionSmearsTheRepeatsButNotTheFirstCluster) {
  const std::vector<float> in = impulse(ms(1500.0), 0, 0.5f);
  const Params base = withParams(kTapsClean, {{"timeMs", 400.0}, {"taps", 4.0}, {"feedback", 60.0}});
  const std::vector<float> sharp = wetOf("delay.multitap", withParams(base, {{"diffusion", 0.0}}), in);
  const std::vector<float> smeared = wetOf("delay.multitap", withParams(base, {{"diffusion", 100.0}}), in);

  // The first cluster is identical: nothing has been through the diffuser yet.
  for (size_t i = 0; i < static_cast<size_t>(ms(400.0)); ++i)
    ASSERT_EQ(sharp[i], smeared[i]) << i;

  // A repeat tap is lower and wider: its peak falls, and the energy around it
  // spreads over many more samples.
  const size_t from = static_cast<size_t>(ms(400.0 + 80.0));
  const size_t to = static_cast<size_t>(ms(400.0 + 120.0));
  EXPECT_LT(peakOf(smeared, from, to), peakOf(sharp, from, to) * 0.5f);
  // The energy is kept (an allpass): the same RMS over a window that holds
  // the whole burst, give or take what the window cuts off.
  const size_t wideFrom = static_cast<size_t>(ms(400.0 + 60.0));
  const size_t wideTo = static_cast<size_t>(ms(400.0 + 150.0));
  EXPECT_NEAR(rmsOf(smeared, wideFrom, wideTo) / rmsOf(sharp, wideFrom, wideTo), 1.0, 0.2);
}

TEST(MultiTapDelayTest, HighCutDarkensTheRepeats) {
  const std::vector<float> burst = withSilence(noiseBurst(0.05, 5u, 0.3f, 0.05), ms(1500.0));
  const auto repeatHigh = [&](double highCut) {
    const std::vector<float> out = wetOf(
        "delay.multitap",
        withParams(kTapsClean, {{"timeMs", 200.0}, {"taps", 4.0}, {"feedback", 70.0}, {"highCutHz", highCut}}),
        burst);
    return bandPower(out, static_cast<size_t>(ms(250.0)), 4000.0, 12000.0);
  };
  EXPECT_LT(repeatHigh(1000.0), repeatHigh(16000.0) * 0.05);
}

TEST(MultiTapDelayTest, SpilloverRingsOutAfterBypassAndStopsAtTheReportedTail) {
  expectSpilloverBehaviour("delay.multitap",
                           {{"timeMs", 200.0}, {"taps", 4.0}, {"feedback", 60.0}, {"diffusion", 30.0},
                            {"lowCutHz", 20.0}, {"highCutHz", 16000.0}},
                           250.0, 450.0, 6.0e-3f);
}

// ---------------------------------------------------------------------------
// All the new kinds: reset
// ---------------------------------------------------------------------------

TEST(DelayVariantsTest, ResetClearsEveryRepeatAndRestoresTheFirstRender) {
  const std::vector<float> in = noise(1.5, 7u, 0.3f, kRate);
  const std::vector<std::pair<const char*, Params>> kinds = {
      {"delay.slapback", {{"timeMs", 150.0}, {"feedback", 50.0}, {"wobble", 50.0}, {"duckDb", 6.0}}},
      {"delay.dual", {{"leftTimeMs", 150.0}, {"rightTimeMs", 210.0}, {"leftFeedback", 50.0},
                      {"rightFeedback", 50.0}, {"crossFeedback", 50.0}, {"duckDb", 6.0}}},
      {"delay.reverse", {{"timeMs", 150.0}, {"feedback", 50.0}, {"duckDb", 6.0}}},
      {"delay.dualreverse", {{"leftTimeMs", 150.0}, {"rightTimeMs", 210.0}, {"leftFeedback", 50.0},
                             {"rightFeedback", 50.0}, {"crossFeedback", 50.0}, {"duckDb", 6.0}}},
      {"delay.multitap", {{"timeMs", 150.0}, {"feedback", 50.0}, {"diffusion", 50.0}, {"duckDb", 6.0}}},
  };
  for (const auto& [kind, params] : kinds) {
    SCOPED_TRACE(kind);
    for (const Layout layout : {Layout::Mono, Layout::Stereo, Layout::DualMono}) {
      SCOPED_TRACE(layoutName(layout));
      DirectKind direct(kind, params, layout);
      const Stereo first = direct.render(Stereo{in, in});
      direct.engine().reset();
      const Stereo again = direct.render(Stereo{in, in});
      EXPECT_EQ(first.left, again.left);
      EXPECT_EQ(first.right, again.right);
      // And silence in gives silence out once reset (nothing left in a line).
      direct.engine().reset();
      const Stereo quiet = direct.render(Stereo{std::vector<float>(48000, 0.0f), std::vector<float>(48000, 0.0f)});
      EXPECT_EQ(peakOf(quiet.left), 0.0f);
      EXPECT_EQ(peakOf(quiet.right), 0.0f);
    }
  }
}

// ---------------------------------------------------------------------------
// Ducking, common to every delay
// ---------------------------------------------------------------------------

namespace {

struct DelayCase {
  const char* id;
  // A setting with dense, lasting repeats: the loop is audible for a second
  // after the input stops.
  Params params;
};

std::vector<DelayCase> allDelays() {
  return {
      {"delay.digital", {{"timeMs", 120.0}, {"feedback", 80.0}, {"modDepth", 0.0}}},
      {"delay.analog", {{"timeMs", 120.0}, {"feedback", 80.0}}},
      {"delay.tape", {{"timeMs", 120.0}, {"feedback", 80.0}}},
      {"delay.simple", {{"timeMs", 120.0}, {"feedback", 80.0}}},
      {"delay.pingpong", {{"timeMs", 120.0}, {"feedback", 80.0}}},
      {"delay.slapback", {{"timeMs", 200.0}, {"feedback", 60.0}}},
      {"delay.dual",
       {{"leftTimeMs", 120.0}, {"rightTimeMs", 160.0}, {"leftFeedback", 80.0}, {"rightFeedback", 80.0}}},
      {"delay.reverse", {{"timeMs", 200.0}, {"feedback", 80.0}}},
      {"delay.dualreverse",
       {{"leftTimeMs", 200.0}, {"rightTimeMs", 250.0}, {"leftFeedback", 80.0}, {"rightFeedback", 80.0}}},
      {"delay.multitap", {{"timeMs", 150.0}, {"feedback", 80.0}}},
      {"delay.crystal", {{"timeMs", 120.0}, {"feedback", 80.0}, {"pitch", -5.0}, {"lowCutHz", 20.0}, {"modDepth", 0.0}}},
  };
}

}  // namespace

TEST(DelayDuckingTest, EveryDelayHasTheSameTwoParametersBeforeSpillover) {
  for (const DelayCase& c : allDelays()) {
    SCOPED_TRACE(c.id);
    const BlockKindInfo* info = BlockKindRegistry::instance().find(c.id);
    ASSERT_NE(info, nullptr);
    const int duck = findBlockParam(info->params, "duckDb");
    const int release = findBlockParam(info->params, "duckReleaseMs");
    ASSERT_GE(duck, 0);
    ASSERT_GE(release, 0);
    EXPECT_EQ(release, duck + 1);
    EXPECT_FLOAT_EQ(info->params[static_cast<size_t>(duck)].min, 0.0f);
    EXPECT_FLOAT_EQ(info->params[static_cast<size_t>(duck)].max, 24.0f);
    EXPECT_FLOAT_EQ(info->params[static_cast<size_t>(duck)].defaultValue, 0.0f) << "off by default";
    EXPECT_FLOAT_EQ(info->params[static_cast<size_t>(release)].min, 50.0f);
    EXPECT_FLOAT_EQ(info->params[static_cast<size_t>(release)].max, 1000.0f);
    EXPECT_FLOAT_EQ(info->params[static_cast<size_t>(release)].defaultValue, 250.0f);
    EXPECT_EQ(info->params.back().id, kSpilloverParamId);
    EXPECT_EQ(release + 2, static_cast<int>(info->params.size()));
  }
}

TEST(DelayDuckingTest, AtZeroDbTheWetPathIsBitIdentical) {
  // The digital delay at no feedback and no modulation is the input delayed by
  // a whole number of samples, exactly: the ducking at 0 dB must leave that
  // bit for bit, whatever its release is set to.
  const std::vector<float> in = noise(1.0, 11u, 0.25f, kRate);
  const int delay = ms(250.0);
  for (const double release : {50.0, 250.0, 1000.0}) {
    const std::vector<float> out =
        wetOf("delay.digital",
              {{"timeMs", 250.0}, {"feedback", 0.0}, {"modDepth", 0.0}, {"duckDb", 0.0}, {"duckReleaseMs", release}},
              in);
    for (size_t i = 0; i < in.size(); ++i) {
      const float expected = i >= static_cast<size_t>(delay) ? in[i - static_cast<size_t>(delay)] : 0.0f;
      ASSERT_EQ(out[i], expected) << "sample " << i;
    }
  }

  // Every delay: 0 dB ignores the release, and any depth changes the output.
  const std::vector<float> loud = noise(2.0, 12u, 0.25f, kRate);
  for (const DelayCase& c : allDelays()) {
    SCOPED_TRACE(c.id);
    for (const Layout layout : {Layout::Mono, Layout::Stereo, Layout::DualMono}) {
      SCOPED_TRACE(layoutName(layout));
      const Stereo a = wetOf(c.id, withParams(c.params, {{"duckDb", 0.0}, {"duckReleaseMs", 50.0}}), Stereo{loud, loud}, layout);
      const Stereo b = wetOf(c.id, withParams(c.params, {{"duckDb", 0.0}, {"duckReleaseMs", 1000.0}}), Stereo{loud, loud}, layout);
      EXPECT_EQ(a.left, b.left);
      EXPECT_EQ(a.right, b.right);
      const Stereo ducked = wetOf(c.id, withParams(c.params, {{"duckDb", 6.0}}), Stereo{loud, loud}, layout);
      EXPECT_NE(a.left, ducked.left);
    }
  }
}

TEST(DelayDuckingTest, LowersTheWetByTheSetDepthWhileTheInputIsLoud) {
  const std::vector<float> loud = noise(2.5, 13u, 0.25f, kRate);
  for (const DelayCase& c : allDelays()) {
    for (const Layout layout : {Layout::Mono, Layout::Stereo, Layout::DualMono}) {
      for (const double depth : {6.0, 12.0, 24.0}) {
        SCOPED_TRACE(std::string(c.id) + " " + layoutName(layout) + " " + std::to_string(depth) + " dB");
        const Stereo reference = wetOf(c.id, c.params, Stereo{loud, loud}, layout);
        const Stereo ducked = wetOf(c.id, withParams(c.params, {{"duckDb", depth}}), Stereo{loud, loud}, layout);
        const std::vector<float>* const sides[2][2] = {{&reference.left, &ducked.left},
                                                       {&reference.right, &ducked.right}};
        for (const auto& side : sides) {
          const double before = windowRms(*side[0], 1.5, 1.0);
          ASSERT_GT(before, 0.01);
          const double after = windowRms(*side[1], 1.5, 1.0);
          EXPECT_NEAR(toDb(after / before), -depth, 1.0);
        }
      }
    }
  }
}

TEST(DelayDuckingTest, LetsGoWithinTheReleaseAndLeavesTheLoopAlone) {
  // 1.5 s of loud noise, then silence: the repeats go on circulating. While
  // the burst plays and for a moment after, the wet is held down; by the
  // release (plus the detector's fall) it is back where the unducked delay
  // has it, which also shows the loop itself was never ducked.
  std::vector<float> in = noise(1.5, 14u, 0.25f, kRate);
  in.resize(static_cast<size_t>(4.0 * kRate), 0.0f);
  const double end = 1.5;
  for (const DelayCase& c : allDelays()) {
    for (const double release : {100.0, 250.0, 600.0}) {
      SCOPED_TRACE(std::string(c.id) + " release " + std::to_string(release));
      const std::vector<float> reference = wetOf(c.id, c.params, in);
      const std::vector<float> ducked =
          wetOf(c.id, withParams(c.params, {{"duckDb", 12.0}, {"duckReleaseMs", release}}), in);

      const double heldRef = windowRms(reference, end - 0.1, 0.1);
      ASSERT_GT(heldRef, 0.01);
      EXPECT_LT(toDb(windowRms(ducked, end - 0.1, 0.1) / heldRef), -10.0) << "ducked while loud";

      const double at = end + release * 0.001 + 0.15;
      const double lateRef = windowRms(reference, at, 0.05);
      ASSERT_GT(lateRef, 0.003);
      EXPECT_GT(toDb(windowRms(ducked, at, 0.05) / lateRef), -1.0) << "back within the release";
      EXPECT_LT(toDb(windowRms(ducked, at, 0.05) / lateRef), 1.0);
    }
  }
}

TEST(DelayDuckingTest, QuietPlayingDoesNotDuck) {
  // 40 dB under full scale is below the detector's range: nothing is lowered.
  const std::vector<float> quiet = noise(2.0, 15u, 0.004f, kRate);
  for (const DelayCase& c : allDelays()) {
    SCOPED_TRACE(c.id);
    const std::vector<float> reference = wetOf(c.id, c.params, quiet);
    const std::vector<float> ducked = wetOf(c.id, withParams(c.params, {{"duckDb", 24.0}}), quiet);
    const double before = windowRms(reference, 1.0, 1.0);
    ASSERT_GT(before, 1.0e-4);
    EXPECT_NEAR(toDb(windowRms(ducked, 1.0, 1.0) / before), 0.0, 0.5);
  }
}

TEST(DelayDuckingTest, ABounceIsDuckedAsOneSoTheImageDoesNotLean) {
  // A ping pong in true stereo: both sides get the same gain.
  const std::vector<float> loud = noise(2.0, 16u, 0.25f, kRate);
  const Params params = {{"timeMs", 120.0}, {"feedback", 60.0}, {"spread", 100.0}};
  const Stereo reference = wetOf("delay.pingpong", params, Stereo{loud, loud}, Layout::Stereo);
  const Stereo ducked = wetOf("delay.pingpong", withParams(params, {{"duckDb", 12.0}}), Stereo{loud, loud}, Layout::Stereo);
  size_t compared = 0;
  for (size_t i = static_cast<size_t>(1.0 * kRate); i < loud.size(); ++i) {
    if (std::abs(reference.left[i]) < 0.02f || std::abs(reference.right[i]) < 0.02f)
      continue;
    const double leftGain = static_cast<double>(ducked.left[i]) / static_cast<double>(reference.left[i]);
    const double rightGain = static_cast<double>(ducked.right[i]) / static_cast<double>(reference.right[i]);
    ASSERT_NEAR(leftGain, rightGain, 1.0e-4) << i;
    ++compared;
  }
  EXPECT_GT(compared, 1000u);
}

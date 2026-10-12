// delay.pingpong (plugin/src/effects/delay/PingPongDelay.cpp): the conformance
// test, then what it is for: repeats that alternate between the sides in a
// true stereo image (and only there), a spread that runs from independent
// delays to the full bounce, a ratio that shortens the right line, and a
// plain mono delay per channel in mono and in dual mono, with nothing
// crossing between channels.
#include "effects/delay_test_support.h"
#include "effects/dsp/TempoSync.h"

#include <gtest/gtest.h>

using namespace delaytest;

namespace {

constexpr const char* kKind = "delay.pingpong";

// Full bounce, the tone wide open, 60% feedback.
const Params kBounce = {{"timeMs", 250.0}, {"feedback", 60.0}, {"spread", 100.0}, {"ratio", 0.0},
                        {"toneHz", 20000.0}, {"mix", 1.0}};

// The level of a burst repeat on each side, `windowMs` wide at `atMs`.
struct Sides {
  double left;
  double right;
};
Sides sidesAt(const Stereo& out, double atMs, double windowMs = 60.0) {
  const size_t begin = static_cast<size_t>(ms(atMs));
  const size_t end = begin + static_cast<size_t>(ms(windowMs));
  return {rmsOf(out.left, begin, end), rmsOf(out.right, begin, end)};
}

}  // namespace

TEST(PingPongDelayTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"timeMs", 120.0}, {"feedback", 40.0}, {"spread", 80.0}, {"ratio", 1.0},
                    {"toneHz", 6000.0}};
  options.stateParams = {{"sync", 1.0},      {"division", fx::kDivEighthDotted},
                         {"timeMs", 123.0},  {"feedback", 55.0},
                         {"spread", 60.0},   {"ratio", 2.0},
                         {"toneHz", 2500.0}, {"spillover", 0.0}};
  options.latency = LatencyCheck::AtMostOnset;
  options.cpuAverageBudgetNs = 4000.0;
  options.cpuWorstBudgetNs = 50000.0;
  expectKindConforms("delay.pingpong", options);
}

TEST(PingPongDelayTest, DelayTimeMatchesTheParameterInEveryLayout) {
  for (const Layout layout : {Layout::Stereo, Layout::Mono, Layout::DualMono}) {
    SCOPED_TRACE(layoutName(layout));
    for (const double timeMs : {25.0, 250.0, 1234.0}) {
      SCOPED_TRACE(timeMs);
      // The left time, whatever the ratio: the ratio only shapes the bounce.
      const Params params = withParams(kBounce, {{"timeMs", timeMs}, {"feedback", 0.0}, {"ratio", 1.0}});
      EXPECT_NEAR(measuredDelaySamples(kKind, params, ms(timeMs), rigFor(layout)), ms(timeMs), 1);
    }
  }
}

TEST(PingPongDelayTest, SyncedTimeFollowsTheTempoAtTwoTempos) {
  struct Case {
    double bpm;
    int division;
    double expectedMs;
  };
  for (const Case& c : {Case{100.0, fx::kDivEighth, 300.0}, Case{150.0, fx::kDivEighthDotted, 300.0},
                        Case{150.0, fx::kDivQuarter, 400.0}}) {
    SCOPED_TRACE(c.bpm);
    RigOptions options;
    options.bpm = c.bpm;
    const Params params = withParams(kBounce, {{"sync", 1.0},
                                               {"division", static_cast<double>(c.division)},
                                               {"timeMs", 77.0},
                                               {"feedback", 0.0}});
    EXPECT_NEAR(measuredDelaySamples(kKind, params, ms(c.expectedMs), options), ms(c.expectedMs), 1);
  }
}

TEST(PingPongDelayTest, RepeatsAlternateSidesInTrueStereoAtFullSpread) {
  EffectRig rig(kKind, kBounce);
  const Stereo out = rig.renderBoth(toneBurst(0.04, 440.0, 0.25f, 3.0));
  const double input = rmsWindow(toneBurst(0.04, 440.0, 0.25f, 3.0), 0.0, 60.0);
  ASSERT_GT(input, 0.01);

  // Repeat k lands at k * 250 ms, on the left for odd k and the right for
  // even k, `feedback` quieter each hop; nothing of it on the other side.
  double expected = input;
  for (int k = 1; k <= 5; ++k) {
    const Sides s = sidesAt(out, 250.0 * k);
    const double wanted = k % 2 == 1 ? s.left : s.right;
    const double other = k % 2 == 1 ? s.right : s.left;
    EXPECT_NEAR(wanted, expected, expected * 0.06 + 0.001) << "repeat " << k;
    EXPECT_LT(other, input * 0.01) << "repeat " << k << " leaks to the other side";
    expected *= 0.6;
  }
  // Odd repeats' energy is all on the left, even repeats' all on the right.
  double leftOdd = 0.0, rightOdd = 0.0, leftEven = 0.0, rightEven = 0.0;
  for (int k = 1; k <= 8; ++k) {
    const Sides s = sidesAt(out, 250.0 * k);
    (k % 2 == 1 ? leftOdd : leftEven) += s.left * s.left;
    (k % 2 == 1 ? rightOdd : rightEven) += s.right * s.right;
  }
  EXPECT_GT(leftOdd, rightOdd * 1000.0);
  EXPECT_GT(rightEven, leftEven * 1000.0);
}

TEST(PingPongDelayTest, SpreadBlendsFromIndependentSidesToTheBounce) {
  const std::vector<float> in = toneBurst(0.04, 440.0, 0.25f, 3.0);
  const double input = rmsWindow(in, 0.0, 60.0);

  // 0: both sides are the same ordinary delay, so they are the same signal
  // (the input is the same on both), a repeat every 250 ms at the feedback.
  {
    EffectRig rig(kKind, withParams(kBounce, {{"spread", 0.0}}));
    const Stereo out = rig.renderBoth(in);
    EXPECT_EQ(out.left, out.right);
    double previous = input;
    for (int k = 1; k <= 4; ++k) {
      const double level = sidesAt(out, 250.0 * k).left;
      EXPECT_NEAR(level, previous * (k == 1 ? 1.0 : 0.6), previous * 0.06) << "repeat " << k;
      previous = level;
    }
  }
  // 50%: the first repeat is on both sides, the right at half the left.
  {
    EffectRig rig(kKind, withParams(kBounce, {{"spread", 50.0}}));
    const Stereo out = rig.renderBoth(in);
    const Sides first = sidesAt(out, 250.0);
    EXPECT_NEAR(first.left, input, input * 0.06);
    EXPECT_NEAR(first.right / first.left, 0.5, 0.04);
  }
  // 100%: nothing of the first repeat on the right (see the test above).
  {
    EffectRig rig(kKind, kBounce);
    const Sides first = sidesAt(rig.renderBoth(in), 250.0);
    EXPECT_LT(first.right, input * 0.01);
  }
}

TEST(PingPongDelayTest, RatioShortensTheRightLine) {
  const std::vector<float> in = toneBurst(0.02, 440.0, 0.25f, 4.0);
  const double input = rmsWindow(in, 0.0, 40.0);
  struct Case {
    double ratio;
    double right;  // the right line, as a share of the left
  };
  for (const Case& c : {Case{0.0, 1.0}, Case{1.0, 0.75}, Case{2.0, 0.618034}}) {
    SCOPED_TRACE(c.ratio);
    const double d = 400.0;
    EffectRig rig(kKind, withParams(kBounce, {{"timeMs", d}, {"ratio", c.ratio}}));
    const Stereo out = rig.renderBoth(in);
    // Hop times: input -> left d, left -> right d * share, right -> left d, ...
    const double t1 = d;
    const double t2 = t1 + d * c.right;
    const double t3 = t2 + d;
    const double t4 = t3 + d * c.right;
    const double window = 40.0;
    const Sides s1 = sidesAt(out, t1 - 5.0, window);
    const Sides s2 = sidesAt(out, t2 - 5.0, window);
    const Sides s3 = sidesAt(out, t3 - 5.0, window);
    const Sides s4 = sidesAt(out, t4 - 5.0, window);
    EXPECT_NEAR(s1.left, input, input * 0.1);
    EXPECT_LT(s1.right, input * 0.02);
    EXPECT_NEAR(s2.right, input * 0.6, input * 0.06);
    EXPECT_LT(s2.left, input * 0.02);
    EXPECT_NEAR(s3.left, input * 0.36, input * 0.04);
    EXPECT_LT(s3.right, input * 0.02);
    EXPECT_NEAR(s4.right, input * 0.216, input * 0.03);
    EXPECT_LT(s4.left, input * 0.02);
  }
}

TEST(PingPongDelayTest, MonoBufferIsAPlainDelayAtTheLeftTime) {
  const std::vector<float> in = toneBurst(0.04, 440.0, 0.25f, 3.0);
  const double input = rmsWindow(in, 0.0, 60.0);
  for (const double ratio : {0.0, 1.0, 2.0}) {
    SCOPED_TRACE(ratio);
    EffectRig rig(kKind, withParams(kBounce, {{"ratio", ratio}}), rigFor(Layout::Mono));
    const std::vector<float> out = rig.render(in);
    double expected = input;
    for (int k = 1; k <= 4; ++k) {
      EXPECT_NEAR(rmsWindow(out, 250.0 * k, 60.0), expected, expected * 0.06) << "repeat " << k;
      expected *= 0.6;
    }
    // And nothing in between: the dotted or golden right line is not there.
    EXPECT_LT(rmsWindow(out, 250.0 * 1.5, 60.0), input * 0.01);
  }
}

TEST(PingPongDelayTest, DualMonoNeverCrossFeeds) {
  const RigOptions dual = rigFor(Layout::DualMono);
  const std::vector<float> a = toneBurst(0.04, 440.0, 0.25f, 3.0);
  const std::vector<float> b = toneBurst(0.04, 1100.0, 0.25f, 3.0);
  const std::vector<float> silence(a.size(), 0.0f);
  const double input = rmsWindow(a, 0.0, 60.0);

  // Audio on the left only: the right output is the empty rig's.
  {
    EffectRig rig(kKind, withParams(kBounce, {{"ratio", 1.0}}), dual);
    EffectRig reference(juce::String(), {}, dual);
    const Stereo out = rig.render(Stereo{a, silence});
    const Stereo dry = reference.render(Stereo{a, silence});
    float worst = 0.0f;
    for (size_t i = 0; i < a.size(); ++i)
      worst = std::max(worst, std::abs(out.right[i] - dry.right[i]));
    EXPECT_LE(worst, 1.0e-7f);
    // The left is an ordinary delay: every repeat on the left, at the left time.
    double expected = input;
    for (int k = 1; k <= 4; ++k) {
      EXPECT_NEAR(rmsWindow(out.left, 250.0 * k, 60.0), expected, expected * 0.06) << "repeat " << k;
      expected *= 0.6;
    }
  }
  // Two different signals: each side is what a mono rig makes of it.
  {
    EffectRig rig(kKind, kBounce, dual);
    EffectRig monoA(kKind, kBounce, rigFor(Layout::Mono));
    EffectRig monoB(kKind, kBounce, rigFor(Layout::Mono));
    const Stereo out = rig.render(Stereo{a, b});
    const std::vector<float> wantLeft = monoA.render(a);
    const std::vector<float> wantRight = monoB.render(b);
    for (size_t i = 0; i < a.size(); ++i) {
      ASSERT_NEAR(out.left[i], wantLeft[i], 1.0e-6f) << i;
      ASSERT_NEAR(out.right[i], wantRight[i], 1.0e-6f) << i;
    }
  }
}

TEST(PingPongDelayTest, ATimeChangeWhileAudioRunsNeverSteps) {
  for (const double to : {400.0, 90.0}) {
    SCOPED_TRACE(to);
    EffectRig rig(kKind, withParams(kBounce, {{"timeMs", 250.0}, {"feedback", 50.0}}));
    const std::vector<float> in = sine(5.0, 220.0, 0.25f, kRate);
    Stereo out = rig.render(Stereo{std::vector<float>(in.begin(), in.begin() + ms(2000.0)),
                                   std::vector<float>(in.begin(), in.begin() + ms(2000.0))});
    const float steady = std::max(maxStep(out.left, static_cast<size_t>(ms(1000.0)), out.size()),
                                  maxStep(out.right, static_cast<size_t>(ms(1000.0)), out.size()));
    rig.set("timeMs", to);
    const std::vector<float> rest(in.begin() + ms(2000.0), in.end());
    const Stereo after = rig.render(Stereo{rest, rest});
    out.left.insert(out.left.end(), after.left.begin(), after.left.end());
    out.right.insert(out.right.end(), after.right.begin(), after.right.end());
    for (const std::vector<float>* side : {&out.left, &out.right}) {
      expectCleanSignal(*side, 2.0f, "time change");
      EXPECT_LT(maxStep(*side, static_cast<size_t>(ms(1900.0)), side->size()), steady * 1.5f + 0.004f)
          << "the change of time stepped the waveform";
    }
  }
}

TEST(PingPongDelayTest, OutputStaysBoundedAtMaximumFeedbackWithAHotInput) {
  expectBoundedAtMaximumFeedback(kKind, {{"timeMs", 90.0}, {"toneHz", 20000.0}}, 1.5f);
  expectBoundedAtMaximumFeedback(kKind, {{"timeMs", 1500.0}, {"spread", 50.0}, {"ratio", 2.0}}, 1.5f);
}

TEST(PingPongDelayTest, TailFollowsTimeAndFeedbackWhateverTheSpread) {
  EffectRig rig(kKind, {{"timeMs", 200.0}, {"feedback", 0.0}, {"toneHz", 20000.0}});
  EXPECT_NEAR(rig.tailSeconds(), 0.2 + 0.05, 0.003);
  rig.set("feedback", 50.0);
  EXPECT_NEAR(rig.tailSeconds(), 0.2 * 11.0 + 0.05, 0.01);
  // The bound is the left time, which no ratio exceeds.
  const double bound = rig.tailSeconds();
  rig.set("ratio", 2.0);
  EXPECT_DOUBLE_EQ(rig.tailSeconds(), bound);
  rig.set("timeMs", 2000.0);
  rig.set("feedback", 95.0);
  EXPECT_DOUBLE_EQ(rig.tailSeconds(), kMaxBlockTailSeconds);
}

TEST(PingPongDelayTest, SpilloverRingsOutAfterBypassAndStopsAtTheReportedTail) {
  // The left output alone is measured; at full spread its repeats come every
  // other hop, so the window is the one the second left repeat lands in.
  expectSpilloverBehaviour(kKind, {{"timeMs", 200.0}, {"feedback", 70.0}, {"spread", 100.0},
                                   {"toneHz", 20000.0}},
                           350.0, 550.0);
  expectSpilloverBehaviour(kKind, {{"timeMs", 200.0}, {"feedback", 70.0}, {"spread", 0.0},
                                   {"toneHz", 20000.0}},
                           350.0, 550.0);
}

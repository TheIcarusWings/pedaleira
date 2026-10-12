// dynamics.gate (plugin/src/effects/dynamics/NoiseGateEffect.cpp): the
// conformance test, then what a gate is for: it attenuates by exactly its
// range below the threshold, passes the signal above it, opens and closes
// without clicks, holds and has hysteresis, and links a stereo pair while
// leaving dual mono alone.
#include "effects/effect_test_helpers.h"

#include <gtest/gtest.h>

using namespace fxtest;

namespace {

constexpr const char* kKind = "dynamics.gate";
constexpr double kRate = 48000.0;

float amp(double levelDb) { return static_cast<float>(std::pow(10.0, levelDb / 20.0)); }

int ms(double milliseconds) { return static_cast<int>(std::lround(milliseconds * 0.001 * kRate)); }

// A sine whose amplitude steps from segment to segment without breaking the
// phase, so the only event at a step is the level change.
struct Segment {
  double seconds;
  double levelDb;  // peak level of the sine, dBFS; -200 for silence
};

std::vector<float> steppedSine(double freq, const std::vector<Segment>& segments) {
  std::vector<float> x;
  for (const Segment& s : segments) {
    const int n = static_cast<int>(std::lround(s.seconds * kRate));
    const float a = s.levelDb <= -200.0 ? 0.0f : amp(s.levelDb);
    for (int i = 0; i < n; ++i)
      x.push_back(a * static_cast<float>(std::sin(2.0 * kPi * freq * static_cast<double>(x.size()) / kRate)));
  }
  return x;
}

// Gain in dB of `out` against `in` over [from, to) samples.
double gainDbOver(const std::vector<float>& out, const std::vector<float>& in, size_t from, size_t to) {
  return toDb(rmsOf(out, from, to)) - toDb(rmsOf(in, from, to));
}

}  // namespace

TEST(NoiseGateEffectTest, Conforms) {
  ConformanceOptions options;
  // A gate that closes during the quiet part of the test signal.
  options.params = {{"thresholdDb", -24.0}, {"rangeDb", -30.0}, {"holdMs", 20.0}};
  options.stateParams = {{"thresholdDb", -33.0}, {"attackMs", 3.0},     {"holdMs", 80.0},
                         {"releaseMs", 300.0},   {"rangeDb", -45.0},    {"hysteresisDb", 6.0}};
  options.cpuAverageBudgetNs = 1500.0;
  options.cpuWorstBudgetNs = 150000.0;
  expectKindConforms("dynamics.gate", options);
}

TEST(NoiseGateEffectTest, AttenuatesByTheRangeBelowTheThreshold) {
  for (const double range : {-20.0, -40.0, -60.0}) {
    SCOPED_TRACE(range);
    EffectRig rig(kKind, {{"thresholdDb", -30.0}, {"rangeDb", range}, {"mix", 1.0}});
    EffectRig reference{juce::String()};
    // A tone 20 dB under the threshold, for long enough that the gate has
    // settled on its closed gain.
    const std::vector<float> in = sine(2.0, 1000.0, amp(-50.0), kRate);
    const std::vector<float> out = rig.render(in);
    const std::vector<float> dry = reference.render(in);
    const size_t from = static_cast<size_t>(1.5 * kRate);
    EXPECT_NEAR(gainDbOver(out, dry, from, out.size()), range, 0.5);
  }
}

TEST(NoiseGateEffectTest, PassesSignalAboveTheThresholdAtUnity) {
  EffectRig rig(kKind, {{"thresholdDb", -30.0}, {"rangeDb", -60.0}, {"mix", 1.0}});
  EffectRig reference{juce::String()};
  const std::vector<float> in = sine(1.0, 1000.0, amp(-10.0), kRate);
  const std::vector<float> out = rig.render(in);
  const std::vector<float> dry = reference.render(in);
  EXPECT_NEAR(gainDbOver(out, dry, ms(100.0), out.size()), 0.0, 0.05);
}

TEST(NoiseGateEffectTest, HysteresisKeepsAnOpenGateOpenBetweenTheThresholds) {
  // Opens at -30, closes at -36. A tone at -33 sits between the two: it
  // keeps an open gate open, and does not open a closed one.
  const Params params = {{"thresholdDb", -30.0}, {"hysteresisDb", 6.0}, {"rangeDb", -40.0},
                         {"holdMs", 0.0},        {"releaseMs", 30.0},    {"mix", 1.0}};
  const double fs = kRate;
  {
    EffectRig rig(kKind, params);
    const std::vector<float> in = steppedSine(1000.0, {{0.3, -10.0}, {1.0, -33.0}});
    const std::vector<float> out = rig.render(in);
    EXPECT_NEAR(gainDbOver(out, in, static_cast<size_t>(0.8 * fs), in.size()), 0.0, 0.5)
        << "the gate closed on a level above its closing threshold";
  }
  {
    EffectRig rig(kKind, params);
    const std::vector<float> in = steppedSine(1000.0, {{1.3, -33.0}});
    const std::vector<float> out = rig.render(in);
    EXPECT_NEAR(gainDbOver(out, in, static_cast<size_t>(0.8 * fs), in.size()), -40.0, 1.0)
        << "a level under the opening threshold opened the gate";
  }
}

TEST(NoiseGateEffectTest, HoldKeepsTheGateOpenAfterTheSignalDrops) {
  // A loud note, then a tone far under the threshold, with a 200 ms hold.
  EffectRig rig(kKind, {{"thresholdDb", -30.0}, {"hysteresisDb", 0.0}, {"holdMs", 200.0},
                        {"releaseMs", 20.0}, {"rangeDb", -40.0}, {"mix", 1.0}});
  const std::vector<float> in = steppedSine(1000.0, {{0.3, -10.0}, {1.2, -50.0}});
  const std::vector<float> out = rig.render(in);
  const size_t end = static_cast<size_t>(0.3 * kRate);
  // 50 to 150 ms into the quiet part the gate is still open ...
  EXPECT_NEAR(gainDbOver(out, in, end + static_cast<size_t>(ms(50.0)), end + static_cast<size_t>(ms(150.0))),
              0.0, 1.0);
  // ... and 500 ms in it has closed.
  EXPECT_NEAR(gainDbOver(out, in, end + static_cast<size_t>(ms(500.0)), end + static_cast<size_t>(ms(900.0))),
              -40.0, 1.0);

  // No hold: the same signal closes at once (after the detector's fall).
  EffectRig quick(kKind, {{"thresholdDb", -30.0}, {"hysteresisDb", 0.0}, {"holdMs", 0.0},
                          {"releaseMs", 20.0}, {"rangeDb", -40.0}, {"mix", 1.0}});
  const std::vector<float> outQuick = quick.render(in);
  EXPECT_LT(gainDbOver(outQuick, in, end + static_cast<size_t>(ms(100.0)), end + static_cast<size_t>(ms(150.0))),
            -30.0);
}

TEST(NoiseGateEffectTest, OpensAlongTheAttackTime) {
  // A closed gate, then a loud tone: the output swells to full level with
  // the time constant of the attack, 63% of the way after about `attack` ms.
  for (const double attack : {2.0, 10.0}) {
    SCOPED_TRACE(attack);
    EffectRig rig(kKind, {{"thresholdDb", -30.0}, {"rangeDb", -80.0}, {"attackMs", attack},
                          {"mix", 1.0}});
    const std::vector<float> in = steppedSine(4000.0, {{0.1, -200.0}, {0.3, -10.0}});
    const std::vector<float> out = rig.render(in);
    const size_t start = static_cast<size_t>(ms(100.0));
    const float full = amp(-10.0);
    // First 4 kHz period (12 samples) whose peak is above 63% of full level.
    int reached = -1;
    for (size_t i = start; i + 12 < out.size(); ++i)
      if (peakOf(out, i, i + 12) >= 0.63f * full) {
        reached = static_cast<int>(i - start);
        break;
      }
    ASSERT_GE(reached, 0);
    const double reachedMs = 1000.0 * reached / kRate;
    EXPECT_GT(reachedMs, 0.5 * attack);
    EXPECT_LT(reachedMs, 1.8 * attack + 0.5);
  }
}

TEST(NoiseGateEffectTest, OpeningAndClosingDoNotClick) {
  // A 200 Hz tone at half scale switches on after silence and, later, drops
  // to a level under the threshold. The tone itself moves at most 0.0131 per
  // sample; the gain ramps may add a little to that, a hard switch would add
  // the whole amplitude.
  EffectRig rig(kKind, {{"thresholdDb", -30.0}, {"rangeDb", -60.0}, {"attackMs", 5.0},
                        {"holdMs", 0.0}, {"releaseMs", 50.0}, {"mix", 1.0}});
  const std::vector<float> in = steppedSine(200.0, {{0.2, -200.0}, {0.5, -6.0}, {0.5, -45.0}});
  const std::vector<float> out = rig.render(in);
  expectCleanSignal(out, 1.0f, "gate");
  EXPECT_LT(maxStep(out, 0, out.size()), 0.02f);
}

TEST(NoiseGateEffectTest, LinkedInTrueStereoIndependentInDualMono) {
  // A loud tone on the left, a quiet one on the right (under the threshold).
  const Params params = {{"thresholdDb", -30.0}, {"rangeDb", -40.0}, {"holdMs", 0.0}};
  const Stereo in{steppedSine(1000.0, {{1.0, -10.0}}), steppedSine(1500.0, {{1.0, -50.0}})};
  const size_t from = static_cast<size_t>(0.6 * kRate);

  DirectKind linked(kKind, params, Layout::Stereo);
  const Stereo outLinked = linked.render(in);
  EXPECT_NEAR(gainDbOver(outLinked.right, in.right, from, in.size()), 0.0, 0.5)
      << "the quiet side of a stereo pair was gated on its own";

  DirectKind dual(kKind, params, Layout::DualMono);
  const Stereo outDual = dual.render(in);
  EXPECT_NEAR(gainDbOver(outDual.right, in.right, from, in.size()), -40.0, 1.0)
      << "in dual mono the quiet guitar must be gated by its own level";
  EXPECT_NEAR(gainDbOver(outDual.left, in.left, from, in.size()), 0.0, 0.5);
}

TEST(NoiseGateEffectTest, ReportsNoTailAndNoLatency) {
  // The wet path is the input times a gain: nothing rings after the input
  // stops, so the reported tail is zero and no latency is added.
  EffectRig rig(kKind);
  EXPECT_EQ(rig.tailSeconds(), 0.0);
  EXPECT_EQ(rig.latencySamples(), 0);
}

TEST(NoiseGateEffectTest, GainReductionIsReportedAsAReading) {
  DirectKind kind(kKind, {{"thresholdDb", -30.0}, {"rangeDb", -60.0}, {"holdMs", 0.0}});
  ASSERT_TRUE(kind.valid());
  ASSERT_EQ(kind.engine().readingTable().size(), 1u);
  // Closed on silence: the whole range.
  kind.render(std::vector<float>(48000, 0.0f));
  EXPECT_NEAR(kind.engine().getReading(0), 60.0, 0.5);
  // Open on a loud tone: nothing, once the attack has finished.
  kind.render(sine(0.5, 1000.0, 0.5f, 48000.0));
  EXPECT_NEAR(kind.engine().getReading(0), 0.0, 0.1);
}

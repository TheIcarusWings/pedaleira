// The utility basics (plugin/src/effects/utility and GainBlockProcessor):
// gain with pan and polarity, the volume pedal, the doubler and the phase
// align block. Each kind goes through the conformance test of the framework;
// then what each one is for: the balance law and the polarity switch of the
// gain, the volume pedal's tapers, floor and smoothing, a doubled copy that
// arrives inside the spread and is decorrelated on a stereo signal, and a
// path delay that lands within a sample of what was asked for.
#include "effects/effect_test_helpers.h"

#include <gtest/gtest.h>

using namespace fxtest;

namespace {

constexpr double kRate = 48000.0;

// A kind run on its own on the same signal on both sides; the left output.
std::vector<float> run(const char* id, const Params& params, const std::vector<float>& in,
                       Layout layout = Layout::Mono) {
  DirectKind kind(id, params, layout);
  return kind.render(in);
}

double dbOf(double linear) { return toDb(linear); }

// Signed centre of mass of a response to an impulse at `at`, in samples after
// it: for an interpolating delay this is the delay itself.
double centroid(const std::vector<float>& out, int at) {
  double weighted = 0.0, total = 0.0;
  for (size_t i = 0; i < out.size(); ++i) {
    weighted += static_cast<double>(out[i]) * (static_cast<double>(i) - at);
    total += out[i];
  }
  return total != 0.0 ? weighted / total : 0.0;
}

void setParam(DirectKind& kind, const char* id, double value) {
  const int index = kind.engine().findParameter(id);
  ASSERT_GE(index, 0) << id;
  kind.engine().setParameter(index, static_cast<float>(value));
}

}  // namespace

// ---------------------------------------------------------------------------
// Conformance
// ---------------------------------------------------------------------------

TEST(UtilityEffectsTest, GainConforms) {
  ConformanceOptions options;
  options.params = {{"gainDb", 6.0}, {"pan", 0.4}, {"phaseInvert", 1.0}};
  options.stateParams = {{"gainDb", -9.0}, {"pan", -0.6}, {"phaseInvert", 1.0}};
  // +24 dB on the sweep's noise.
  options.sweepBound = 8.0f;
  expectKindConforms("utility.gain", options);
}

TEST(UtilityEffectsTest, VolumePedalConforms) {
  ConformanceOptions options;
  options.params = {{"positionPercent", 40.0}, {"curve", 1.0}, {"minimumDb", -50.0}};
  options.stateParams = {{"positionPercent", 25.0}, {"curve", 0.0}, {"minimumDb", -20.0}};
  expectKindConforms("utility.volume", options);
}

TEST(UtilityEffectsTest, DoublerConforms) {
  ConformanceOptions options;
  options.params = {{"spreadMs", 30.0}, {"dryDb", 0.0}, {"doubledDb", 0.0}};
  options.stateParams = {{"spreadMs", 12.0}, {"dryDb", -6.0}, {"doubledDb", 3.0}};
  expectKindConforms("utility.doubler", options);
}

TEST(UtilityEffectsTest, PhaseAlignConforms) {
  ConformanceOptions options;
  options.params = {{"polarity", 1.0}, {"delayMs", 1.5}};
  options.stateParams = {{"polarity", 1.0}, {"delayMs", 3.2}};
  // The path delay is the effect, not latency: the output may start later
  // than the reported latency (0) but never earlier.
  options.latency = LatencyCheck::AtMostOnset;
  expectKindConforms("utility.phase", options);
}

TEST(UtilityEffectsTest, TheNewKindsAreUtilities) {
  for (const char* id : {"utility.gain", "utility.volume", "utility.doubler", "utility.phase"}) {
    const BlockKindInfo* info = BlockKindRegistry::instance().find(id);
    ASSERT_NE(info, nullptr) << id;
    EXPECT_EQ(info->category, BlockCategory::Utility) << id;
    EXPECT_TRUE(info->addable) << id;
    EXPECT_FLOAT_EQ(info->defaultMix, 1.0f) << id;
    DirectKind kind(id);
    EXPECT_EQ(kind.latencySamples(), 0) << id;
  }
}

// ---------------------------------------------------------------------------
// Gain: pan (a balance on a stereo signal) and polarity
// ---------------------------------------------------------------------------

TEST(UtilityEffectsTest, GainDefaultsAreABitExactPassThrough) {
  const std::vector<float> in = noise(0.5, 3, 0.5f, kRate);
  for (const Layout layout : {Layout::Mono, Layout::Stereo, Layout::DualMono}) {
    SCOPED_TRACE(layoutName(layout));
    EXPECT_EQ(run("utility.gain", {}, in, layout), in);
  }
}

TEST(UtilityEffectsTest, GainPanIsABalanceOnStereoAndNothingOnMono) {
  const std::vector<float> in = sine(0.5, 440.0, 0.5f, kRate);
  const size_t skip = 2000;

  // Centre leaves both sides alone; to one side the other is turned down
  // and the near side keeps its level.
  {
    DirectKind kind("utility.gain", {{"pan", 0.5}}, Layout::Stereo);
    const Stereo out = kind.render(Stereo{in, in});
    EXPECT_NEAR(dbOf(rmsOf(out.left, skip)) - dbOf(rmsOf(in, skip)), -6.02, 0.05);
    EXPECT_NEAR(dbOf(rmsOf(out.right, skip)) - dbOf(rmsOf(in, skip)), 0.0, 0.05);
  }
  {
    DirectKind kind("utility.gain", {{"pan", -1.0}}, Layout::Stereo);
    const Stereo out = kind.render(Stereo{in, in});
    EXPECT_LT(peakOf(out.right), 1.0e-6f) << "hard left silences the right side";
    EXPECT_NEAR(rmsOf(out.left, skip), rmsOf(in, skip), 1.0e-4);
  }
  // One channel has nothing to balance against.
  EXPECT_EQ(run("utility.gain", {{"pan", 1.0}}, in, Layout::Mono), in);
}

TEST(UtilityEffectsTest, GainPolarityInvertsExactly) {
  const std::vector<float> in = noise(0.3, 5, 0.5f, kRate);
  for (const Layout layout : {Layout::Mono, Layout::Stereo}) {
    const std::vector<float> out = run("utility.gain", {{"phaseInvert", 1.0}}, in, layout);
    ASSERT_EQ(out.size(), in.size());
    for (size_t i = 0; i < in.size(); ++i)
      ASSERT_EQ(out[i], -in[i]) << i;
  }
}

// ---------------------------------------------------------------------------
// Volume pedal
// ---------------------------------------------------------------------------

TEST(UtilityEffectsTest, VolumePedalDefaultIsFullyOpenAndExact) {
  const std::vector<float> in = noise(0.3, 9, 0.5f, kRate);
  EXPECT_EQ(run("utility.volume", {}, in), in);
}

TEST(UtilityEffectsTest, VolumePedalTapers) {
  const std::vector<float> in = sine(1.0, 1000.0, 0.5f, kRate);
  const size_t skip = static_cast<size_t>(0.3 * kRate);
  const auto gainDb = [&](double position, double curve, double minimumDb) {
    const std::vector<float> out = run(
        "utility.volume",
        {{"positionPercent", position}, {"curve", curve}, {"minimumDb", minimumDb}}, in);
    return steadyStateGainDb(out, in, skip);
  };

  // Audio taper: the level in dB is a straight line from the floor to 0.
  EXPECT_NEAR(gainDb(50.0, 1.0, -60.0), -30.0, 0.05);
  EXPECT_NEAR(gainDb(75.0, 1.0, -60.0), -15.0, 0.05);
  EXPECT_NEAR(gainDb(50.0, 1.0, -20.0), -10.0, 0.05);
  // Linear: the amplitude is a straight line (to silence at the lowest floor).
  EXPECT_NEAR(gainDb(50.0, 0.0, -60.0), -6.02, 0.05);
  EXPECT_NEAR(gainDb(25.0, 0.0, -60.0), -12.04, 0.05);
  // Both reach 0 dB at the toe.
  EXPECT_NEAR(gainDb(100.0, 0.0, -30.0), 0.0, 1.0e-4);
  EXPECT_NEAR(gainDb(100.0, 1.0, -30.0), 0.0, 1.0e-4);
}

TEST(UtilityEffectsTest, VolumePedalFloor) {
  const std::vector<float> in = sine(1.0, 1000.0, 0.5f, kRate);
  const size_t skip = static_cast<size_t>(0.3 * kRate);
  for (const double curve : {0.0, 1.0}) {
    SCOPED_TRACE(curve);
    // The lowest floor is silence at the heel...
    const std::vector<float> silent =
        run("utility.volume", {{"positionPercent", 0.0}, {"curve", curve}, {"minimumDb", -60.0}}, in);
    EXPECT_EQ(peakOf(silent, skip), 0.0f);
    // ...and any other floor is where the heel sits.
    const std::vector<float> floored =
        run("utility.volume", {{"positionPercent", 0.0}, {"curve", curve}, {"minimumDb", -40.0}}, in);
    EXPECT_NEAR(steadyStateGainDb(floored, in, skip), -40.0, 0.05);
  }
}

TEST(UtilityEffectsTest, VolumePedalMovesWithoutZipper) {
  // A constant signal, so every step in the output is the gain's.
  DirectKind kind("utility.volume", {{"positionPercent", 100.0}});
  const std::vector<float> ones(4800, 1.0f);
  std::vector<float> all = kind.render(ones);
  setParam(kind, "positionPercent", 0.0);
  const std::vector<float> down = kind.render(std::vector<float>(9600, 1.0f));
  all.insert(all.end(), down.begin(), down.end());
  setParam(kind, "positionPercent", 100.0);
  const std::vector<float> up = kind.render(std::vector<float>(9600, 1.0f));
  all.insert(all.end(), up.begin(), up.end());

  // 20 ms ramp: at most 1 / 960 per sample, with a little margin.
  float biggest = 0.0f;
  for (size_t i = 1; i < all.size(); ++i)
    biggest = std::max(biggest, std::abs(all[i] - all[i - 1]));
  EXPECT_LT(biggest, 1.0f / 960.0f + 1.0e-4f);
  EXPECT_FLOAT_EQ(all[4800 + 9600 - 1], 0.0f) << "settled at the heel";
  EXPECT_FLOAT_EQ(all.back(), 1.0f) << "and back at the toe";
  // It really did move (not one jump hidden in a block edge).
  EXPECT_GT(all[4800 + 100], 0.0f);
  EXPECT_LT(all[4800 + 100], 1.0f);
}

// ---------------------------------------------------------------------------
// Doubler
// ---------------------------------------------------------------------------

TEST(UtilityEffectsTest, DoublerCopyArrivesInsideTheSpread) {
  for (const double spreadMs : {3.0, 10.0, 25.0, 60.0}) {
    SCOPED_TRACE(spreadMs);
    // The dry is nearly off so the copy is the peak.
    const std::vector<float> out =
        run("utility.doubler", {{"spreadMs", spreadMs}, {"dryDb", -40.0}, {"doubledDb", 0.0}},
            impulse(8000, 0));
    const int index = peakIndexOf(out);
    const double samples = spreadMs * 0.001 * kRate;
    EXPECT_GE(index, static_cast<int>(0.8 * samples) - 2);
    EXPECT_LE(index, static_cast<int>(samples) + 2);
    EXPECT_GT(std::abs(out[static_cast<size_t>(index)]), 0.6f) << "a copy of the input, not smear";
    // Nothing later than the spread.
    EXPECT_LT(peakOf(out, static_cast<size_t>(samples) + 8), 1.0e-6f);
  }
}

TEST(UtilityEffectsTest, DoublerLevels) {
  const std::vector<float> in = noise(1.0, 11, 0.3f, kRate);
  const size_t skip = static_cast<size_t>(0.2 * kRate);
  // Dry alone is the input; the copy alone is the same level (a delayed
  // noise has the level of the noise).
  const auto level = [&](double dry, double doubled) {
    return steadyStateGainDb(
        run("utility.doubler", {{"dryDb", dry}, {"doubledDb", doubled}}, in), in, skip);
  };
  EXPECT_NEAR(level(0.0, -40.0), 0.0, 0.1);
  EXPECT_NEAR(level(-40.0, 0.0), 0.0, 1.2) << "interpolation takes a little off white noise";
  EXPECT_NEAR(level(6.0, -40.0), 6.0, 0.1);
  EXPECT_NEAR(level(-40.0, -6.0), -6.0, 1.2);
  // Both at 0 dB, uncorrelated at this delay: +3 dB.
  EXPECT_NEAR(level(0.0, 0.0), 3.0, 1.0);
}

TEST(UtilityEffectsTest, DoublerWidensAStereoSignalOnly) {
  const std::vector<float> in = noise(1.0, 13, 0.3f, kRate);
  const size_t skip = static_cast<size_t>(0.2 * kRate);

  // Identical on both sides in: different on the way out, so it is wide.
  DirectKind wide("utility.doubler", {}, Layout::Stereo);
  const Stereo out = wide.render(Stereo{in, in});
  double side = 0.0, mid = 0.0;
  for (size_t i = skip; i < out.size(); ++i) {
    const double d = out.left[i] - out.right[i];
    const double s = out.left[i] + out.right[i];
    side += d * d;
    mid += s * s;
  }
  EXPECT_GT(side / mid, 0.02) << "the two sides are decorrelated";

  // Mono in a one-channel bus and dual mono: no widening, and each channel
  // is what a mono rig makes of it.
  DirectKind dual("utility.doubler", {}, Layout::DualMono);
  const Stereo twin = dual.render(Stereo{in, in});
  EXPECT_EQ(twin.left, twin.right);
  EXPECT_EQ(twin.left, run("utility.doubler", {}, in));
}

// ---------------------------------------------------------------------------
// Phase align
// ---------------------------------------------------------------------------

TEST(UtilityEffectsTest, PhaseAlignDefaultIsABitExactPassThrough) {
  const std::vector<float> in = noise(0.3, 17, 0.5f, kRate);
  EXPECT_EQ(run("utility.phase", {}, in), in);
}

TEST(UtilityEffectsTest, PhaseAlignPolarityInvertsExactly) {
  const std::vector<float> in = noise(0.3, 19, 0.5f, kRate);
  const std::vector<float> out = run("utility.phase", {{"polarity", 1.0}}, in);
  for (size_t i = 0; i < in.size(); ++i)
    ASSERT_EQ(out[i], -in[i]) << i;
}

TEST(UtilityEffectsTest, PhaseAlignDelayIsAccurateToAFractionOfASample) {
  // Whole samples, fractions, below one sample, and the longest.
  for (const double delayMs : {0.01, 0.05, 0.3, 1.0, 1.2345, 2.5, 5.0}) {
    SCOPED_TRACE(delayMs);
    const double expected = delayMs * 0.001 * kRate;
    const std::vector<float> out = run("utility.phase", {{"delayMs", delayMs}}, impulse(1000, 100));
    EXPECT_NEAR(centroid(out, 100), expected, 0.02);
    // The peak is at the nearest whole sample.
    EXPECT_NEAR(peakIndexOf(out) - 100, expected, 1.0);
  }
}

TEST(UtilityEffectsTest, PhaseAlignDelaysBothSidesAndInverts) {
  DirectKind kind("utility.phase", {{"polarity", 1.0}, {"delayMs", 2.0}}, Layout::Stereo);
  const Stereo out = kind.render(Stereo{impulse(1000, 50), impulse(1000, 50)});
  EXPECT_EQ(peakIndexOf(out.left), 50 + 96);
  EXPECT_EQ(peakIndexOf(out.right), 50 + 96);
  EXPECT_FLOAT_EQ(out.left[50 + 96], -1.0f);
  EXPECT_FLOAT_EQ(out.right[50 + 96], -1.0f);
}

TEST(UtilityEffectsTest, PhaseAlignMovesWithoutAClick) {
  // A sine through a delay that is changed mid-stream: the output stays a
  // smooth wave (no sample-to-sample jump beyond what the sine itself makes
  // plus the pitch bend of the glide).
  DirectKind kind("utility.phase", {{"delayMs", 0.5}});
  const std::vector<float> in = sine(0.5, 200.0, 0.5f, kRate);
  std::vector<float> out = kind.render(std::vector<float>(in.begin(), in.begin() + 12000));
  setParam(kind, "delayMs", 4.0);
  const std::vector<float> rest = kind.render(std::vector<float>(in.begin() + 12000, in.end()));
  out.insert(out.end(), rest.begin(), rest.end());
  float biggest = 0.0f;
  for (size_t i = 1; i < out.size(); ++i)
    biggest = std::max(biggest, std::abs(out[i] - out[i - 1]));
  // 200 Hz at 0.5 peaks at 2 pi 200 / 48000 * 0.5 = 0.013 per sample.
  EXPECT_LT(biggest, 0.03f);
}

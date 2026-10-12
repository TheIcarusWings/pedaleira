// dynamics.limiter (plugin/src/effects/dynamics/LimiterEffect.cpp): the
// conformance test, then the limiter's contract: it reports its 2 ms
// look-ahead as latency, nothing it outputs exceeds the ceiling, a signal
// under the ceiling passes untouched (only delayed), the gain comes down
// before a peak arrives and recovers with the release time.
#include "effects/effect_test_helpers.h"

#include <gtest/gtest.h>

using namespace fxtest;

namespace {

constexpr const char* kKind = "dynamics.limiter";
constexpr double kRate = 48000.0;
constexpr int kLookahead = 96;  // 2 ms at 48 kHz

float amp(double levelDb) { return static_cast<float>(std::pow(10.0, levelDb / 20.0)); }

// The worst case for a limiter: abrupt square-ish bursts and a sine that
// switches on at full scale, mixed with noise.
std::vector<float> nastySignal() {
  std::vector<float> x(static_cast<size_t>(1.6 * kRate), 0.0f);
  const std::vector<float> grit = noise(1.6, 31u, 0.25f, kRate);
  for (size_t i = 0; i < x.size(); ++i) {
    const double t = static_cast<double>(i) / kRate;
    float v = grit[i];
    if (t >= 0.2 && t < 0.5)
      v += (std::fmod(t * 150.0, 1.0) < 0.5 ? 0.5f : -0.5f);  // 150 Hz square
    if (t >= 0.7 && t < 0.9)
      v += 0.9f * static_cast<float>(std::sin(2.0 * kPi * 1000.0 * t));
    if (t >= 1.2 && t < 1.205)
      v += (i % 2 == 0 ? 1.0f : -1.0f);  // a 5 ms burst at Nyquist/2
    x[i] = v;
  }
  return x;
}

}  // namespace

TEST(LimiterEffectTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"ceilingDb", -6.0}, {"inputGainDb", 6.0}, {"releaseMs", 60.0}};
  options.stateParams = {{"ceilingDb", -3.0}, {"releaseMs", 250.0}, {"inputGainDb", 9.0}};
  options.cpuAverageBudgetNs = 3000.0;
  options.cpuWorstBudgetNs = 150000.0;
  expectKindConforms("dynamics.limiter", options);
}

TEST(LimiterEffectTest, ReportsTwoMillisecondsOfLookaheadAsLatency) {
  EffectRig rig(kKind);
  EXPECT_EQ(rig.latencySamples(), kLookahead);
  DirectKind alone(kKind);
  EXPECT_EQ(alone.latencySamples(), kLookahead);
  EXPECT_NEAR(alone.tailSeconds(), 0.002, 1e-9);

  // The wet path really is that late: an impulse under the ceiling comes out
  // exactly `lookahead` samples after it went in, untouched.
  DirectKind kind(kKind, {{"ceilingDb", 0.0}});
  const std::vector<float> out = kind.render(impulse(2048, 10, 0.5f));
  EXPECT_EQ(peakIndexOf(out), 10 + kLookahead);
  EXPECT_FLOAT_EQ(out[static_cast<size_t>(10 + kLookahead)], 0.5f);

  // And through the whole rig the dry share of the mix is delayed by the
  // same amount (the chain compensates), so a half mix of a loud-enough
  // burst does not comb: the dry and wet impulses coincide.
  EffectRig half(kKind, {{"ceilingDb", -1.0}, {"mix", 0.5}});
  EffectRig reference{juce::String()};
  const std::vector<float> in = impulse(4096, 100, 0.8f);
  const std::vector<float> mixed = half.render(in);
  const std::vector<float> dry = reference.render(in);
  EXPECT_EQ(peakIndexOf(mixed), peakIndexOf(dry) + kLookahead);
  EXPECT_GT(mixed[static_cast<size_t>(peakIndexOf(dry) + kLookahead)], 0.7f);
}

TEST(LimiterEffectTest, NothingExceedsTheCeilingEvenOnAbruptBursts) {
  const std::vector<float> in = nastySignal();
  for (const double ceilingDb : {-1.0, -6.0, -12.0, -20.0}) {
    for (const double gainDb : {0.0, 12.0, 24.0}) {
      SCOPED_TRACE(testing::Message() << "ceiling " << ceilingDb << " dB, input gain " << gainDb);
      const float ceiling = amp(ceilingDb);
      DirectKind kind(kKind, {{"ceilingDb", ceilingDb}, {"inputGainDb", gainDb}, {"releaseMs", 50.0}});
      const std::vector<float> out = kind.render(in);
      expectCleanSignal(out, ceiling, "limiter");
      // Not muted: with this much drive the loudest passage sits at the
      // ceiling.
      if (gainDb >= 12.0)
        EXPECT_GT(peakOf(out), 0.97f * ceiling);

      // The same through the rig. Its 5 Hz DC blocker follows the chain and
      // overshoots a little on the abrupt square burst (the block's own
      // output, above, is exact), so the bound here is a few percent wider.
      EffectRig rig(kKind, {{"ceilingDb", ceilingDb}, {"inputGainDb", gainDb}, {"releaseMs", 50.0},
                            {"mix", 1.0}});
      EXPECT_LE(peakOf(rig.render(in)), ceiling * 1.08f);
    }
  }
}

TEST(LimiterEffectTest, PassesASignalUnderTheCeilingUntouched) {
  DirectKind kind(kKind, {{"ceilingDb", -6.0}});
  const std::vector<float> in = sine(0.5, 440.0, amp(-12.0), kRate);
  const std::vector<float> out = kind.render(in);
  for (size_t i = 0; i + kLookahead < in.size(); ++i)
    ASSERT_EQ(out[i + kLookahead], in[i]) << "sample " << i;
}

TEST(LimiterEffectTest, GainComesDownBeforeThePeakArrives) {
  // A quiet tone (0.2 peak, under the 0.5 ceiling) that jumps to full scale
  // at sample t0 without a break in its phase. The limiter sees it
  // `lookahead` samples early: the output ramps down across the 2 ms before
  // the loud part comes out, so it never needs to clip a sample.
  const double ceilingDb = -6.0;
  const size_t t0 = 6000;
  std::vector<float> in(12000);
  for (size_t i = 0; i < in.size(); ++i)
    in[i] = (i < t0 ? 0.2f : 1.0f) * static_cast<float>(std::sin(2.0 * kPi * 440.0 * static_cast<double>(i) / kRate));
  DirectKind kind(kKind, {{"ceilingDb", ceilingDb}, {"releaseMs", 100.0}});
  const std::vector<float> out = kind.render(in);

  // The quiet tone before the ramp is untouched.
  const size_t arrival = t0 + kLookahead;
  EXPECT_FLOAT_EQ(out[arrival - kLookahead - 200], in[arrival - kLookahead - 200 - kLookahead]);
  // Over the last millisecond before the loud part reaches the output the
  // quiet tone is already turned down: a plain clipper would leave it alone.
  const float quietPeakBefore = 0.2f;
  EXPECT_LT(peakOf(out, arrival - 24, arrival), 0.85f * quietPeakBefore);
  // And the loud part comes out at the ceiling, no higher.
  EXPECT_LE(peakOf(out, arrival, out.size()), amp(ceilingDb) * 1.000001f);
  EXPECT_GT(peakOf(out, arrival + 1000, out.size()), 0.9f * amp(ceilingDb));
  // The ramp is smooth: no step beyond what a 440 Hz tone at the ceiling's
  // own level does, plus the ramp's share.
  const float naturalStep = amp(ceilingDb) * static_cast<float>(2.0 * kPi * 440.0 / kRate);
  EXPECT_LT(maxStep(out, arrival - kLookahead, arrival + 400), 2.0f * naturalStep);
}

TEST(LimiterEffectTest, GainRecoversWithTheReleaseTime) {
  // Full-scale tone, 100 ms, then a quiet one (0.1) that sits under the
  // ceiling. The gain starts at ceiling / 1.0 = 0.5 and returns to 1 along
  // 1 - 0.5 exp(-t / release).
  const double releaseMs = 120.0;
  std::vector<float> in(static_cast<size_t>(0.8 * kRate));
  const size_t loudEnd = static_cast<size_t>(0.1 * kRate);
  for (size_t i = 0; i < in.size(); ++i)
    in[i] = (i < loudEnd ? 1.0f : 0.1f) *
            static_cast<float>(std::sin(2.0 * kPi * 440.0 * static_cast<double>(i) / kRate));
  DirectKind kind(kKind, {{"ceilingDb", -6.0}, {"releaseMs", releaseMs}});
  const std::vector<float> out = kind.render(in);

  // Quiet-tone gain (output peak over 0.1) around a time after the loud part
  // has left the output.
  const auto gainAt = [&](double secondsAfterLoud) {
    const size_t centre = loudEnd + kLookahead + static_cast<size_t>(secondsAfterLoud * kRate);
    return peakOf(out, centre - 60, centre + 60) / 0.1f;
  };
  for (const double t : {0.06, 0.12, 0.25, 0.5}) {
    const double expected = 1.0 - 0.5 * std::exp(-t / (releaseMs * 0.001));
    EXPECT_NEAR(gainAt(t), expected, 0.07) << "after " << t << " s";
  }
}

TEST(LimiterEffectTest, InputGainDrivesTheLimiter) {
  // 12 dB of input gain on a -30 dB tone, ceiling 0 dB: the tone is just 12
  // dB louder.
  EffectRig rig(kKind, {{"inputGainDb", 12.0}, {"ceilingDb", 0.0}, {"mix", 1.0}});
  EXPECT_NEAR(sineProbeGainDb(rig, 1000.0, amp(-30.0), 1.0, 0.5), 12.0, 0.1);
}

TEST(LimiterEffectTest, StereoPairSharesOneGainAndDualMonoDoesNot) {
  // Loud left, quiet right. In true stereo the right side is turned down
  // with the left (the image holds); in dual mono it is left alone.
  const double ceilingDb = -6.0;
  const Params params = {{"ceilingDb", ceilingDb}, {"releaseMs", 500.0}};
  std::vector<float> loud = sine(1.0, 1000.0, 1.0f, kRate);
  std::vector<float> quiet = sine(1.0, 1500.0, 0.1f, kRate);
  const Stereo in{loud, quiet};
  const size_t from = static_cast<size_t>(0.5 * kRate);

  DirectKind linked(kKind, params, Layout::Stereo);
  const Stereo outLinked = linked.render(in);
  const double linkedGain = peakOf(outLinked.right, from) / peakOf(in.right, from);
  EXPECT_NEAR(linkedGain, 0.5, 0.03);
  EXPECT_LE(peakOf(outLinked.left), amp(ceilingDb) * 1.000001f);

  DirectKind dual(kKind, params, Layout::DualMono);
  const Stereo outDual = dual.render(in);
  EXPECT_NEAR(peakOf(outDual.right, from) / peakOf(in.right, from), 1.0, 0.001);
  EXPECT_LE(peakOf(outDual.left), amp(ceilingDb) * 1.000001f);
}

TEST(LimiterEffectTest, GainReductionIsReportedAsAReading) {
  DirectKind kind(kKind, {{"ceilingDb", -6.0}});
  ASSERT_TRUE(kind.valid());
  ASSERT_EQ(kind.engine().readingTable().size(), 1u);
  // A 0 dB tone under a -6 dB ceiling: about 6 dB of reduction.
  kind.render(sine(0.5, 1000.0, 1.0f, 48000.0));
  EXPECT_NEAR(kind.engine().getReading(0), 6.0, 0.5);
  // Well under the ceiling: next to none once the release has let go (it
  // approaches unity, so a trace is left).
  kind.render(sine(1.0, 1000.0, 0.1f, 48000.0));
  EXPECT_NEAR(kind.engine().getReading(0), 0.0, 0.05);
}

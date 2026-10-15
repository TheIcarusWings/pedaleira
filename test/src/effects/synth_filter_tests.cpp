// filter.synth (plugin/src/effects/filter/SynthFilterEffect.cpp): the
// conformance test, then what a ladder filter is for: a 24 dB per octave low
// pass whose resonance peak grows until it sings, the other three modes, a
// signal that stays bounded at full resonance and full drive, and a cutoff
// that follows how hard you play.
#include "effects/effect_test_helpers.h"

#include <gtest/gtest.h>

using namespace fxtest;

namespace {

constexpr const char* kKind = "filter.synth";
constexpr double kRate = 48000.0;

// Gain in dB of a quiet sine (the ladder is linear enough at this level).
double gainAt(const Params& params, double hz) {
  EffectRig rig(kKind, params);
  return sineProbeGainDb(rig, hz, 0.01f, 1.0, 0.5);
}

Params with(Params params, const char* id, double value) {
  params.emplace_back(id, value);
  return params;
}

// How bright a stretch of white noise came out: the energy of its first
// difference over its energy, which grows with the filter's cutoff.
double brightness(const std::vector<float>& x, size_t from) {
  double diff = 0.0, energy = 0.0;
  for (size_t i = std::max<size_t>(from, 1); i < x.size(); ++i) {
    const double d = static_cast<double>(x[i]) - static_cast<double>(x[i - 1]);
    diff += d * d;
    energy += static_cast<double>(x[i]) * static_cast<double>(x[i]);
  }
  return std::sqrt(diff / std::max(energy, 1.0e-30));
}

double brightnessWith(const Params& params, float level) {
  EffectRig rig(kKind, params);
  const std::vector<float> out = rig.render(noise(1.5, 11u, level, kRate));
  return brightness(out, static_cast<size_t>(1.0 * kRate));
}

}  // namespace

TEST(SynthFilterEffectTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"cutoffHz", 800.0}, {"resonancePct", 70.0}, {"driveDb", 9.0},
                    {"envAmountPct", 60.0}};
  options.stateParams = {{"cutoffHz", 2500.0}, {"resonancePct", 45.0}, {"driveDb", 12.0},
                         {"envAmountPct", -50.0}, {"envAttackMs", 30.0}, {"envReleaseMs", 500.0},
                         {"mode", 2.0}};
  options.cpuAverageBudgetNs = 8000.0;
  options.cpuWorstBudgetNs = 200000.0;
  options.sweepBound = 20.0f;
  // Four poles take a few samples to rise to their peak; that is the filter,
  // not latency (the kind reports 0).
  options.latency = LatencyCheck::AtMostOnset;
  expectKindConforms("filter.synth", options);
}

TEST(SynthFilterEffectTest, LowPass24FallsTwentyFourDbPerOctave) {
  const Params base = {{"cutoffHz", 500.0}, {"resonancePct", 0.0}, {"mix", 1.0}, {"mode", 0.0}};
  EXPECT_NEAR(gainAt(base, 100.0), -0.7, 0.3) << "four poles, each a fraction of a dB down this far below the cutoff";
  // Each of the four poles is 3 dB down at the cutoff.
  EXPECT_NEAR(gainAt(base, 500.0), -12.0, 1.0);
  const double octaveA = gainAt(base, 2000.0) - gainAt(base, 4000.0);
  const double octaveB = gainAt(base, 3000.0) - gainAt(base, 6000.0);
  EXPECT_NEAR(octaveA, 24.0, 1.5);
  EXPECT_NEAR(octaveB, 24.0, 2.0);
}

TEST(SynthFilterEffectTest, LowPass12FallsTwelveDbPerOctave) {
  const Params base = {{"cutoffHz", 500.0}, {"resonancePct", 0.0}, {"mix", 1.0}, {"mode", 1.0}};
  const double octave = gainAt(base, 2000.0) - gainAt(base, 4000.0);
  EXPECT_NEAR(octave, 12.0, 1.0);
}

TEST(SynthFilterEffectTest, ResonancePeakGrowsWithResonance) {
  double previous = -100.0;
  for (const double resonance : {0.0, 40.0, 70.0, 90.0}) {
    SCOPED_TRACE(resonance);
    const Params params = {{"cutoffHz", 1000.0}, {"resonancePct", resonance}, {"mix", 1.0}};
    double peak = -100.0;
    for (const double hz : {700.0, 800.0, 900.0, 1000.0})
      peak = std::max(peak, gainAt(params, hz));
    EXPECT_GT(peak, previous + 1.0);
    previous = peak;
  }
  EXPECT_GT(previous, 6.0) << "high resonance should be a clear peak";
}

TEST(SynthFilterEffectTest, SelfOscillatesNearFullResonance) {
  EffectRig rig(kKind, {{"cutoffHz", 1000.0}, {"resonancePct", 100.0}, {"mix", 1.0}});
  const std::vector<float> out = rig.render(impulse(static_cast<int>(2.0 * kRate), 0, 0.5f));
  EXPECT_GT(peakOf(out, static_cast<size_t>(1.5 * kRate)), 0.05f) << "still ringing after 1.5 s";
  expectCleanSignal(out, 4.0f, "self-oscillation");
}

TEST(SynthFilterEffectTest, OtherModesShapeTheSpectrum) {
  const Params band = {{"cutoffHz", 1000.0}, {"resonancePct", 0.0}, {"mix", 1.0}, {"mode", 2.0}};
  EXPECT_LT(gainAt(band, 60.0), gainAt(band, 1000.0) - 10.0);
  EXPECT_LT(gainAt(band, 15000.0), gainAt(band, 1000.0) - 10.0);

  const Params high = {{"cutoffHz", 1000.0}, {"resonancePct", 0.0}, {"mix", 1.0}, {"mode", 3.0}};
  EXPECT_NEAR(gainAt(high, 12000.0), 0.0, 1.0);
  const double octave = gainAt(high, 250.0) - gainAt(high, 125.0);
  EXPECT_NEAR(octave, 24.0, 2.0);
}

TEST(SynthFilterEffectTest, StaysBoundedAtFullResonanceAndDriveOverASweep) {
  for (const double mode : {0.0, 1.0, 2.0, 3.0}) {
    SCOPED_TRACE(mode);
    EffectRig rig(kKind, {{"resonancePct", 100.0}, {"driveDb", 24.0}, {"mode", mode}, {"mix", 1.0}});
    const std::vector<float> in = noise(0.1, 21u, 0.5f, kRate);
    std::vector<float> all;
    // From the bottom of the range to the top and back, in steps.
    for (int step = 0; step < 40; ++step) {
      const double t = step < 20 ? step / 19.0 : (39 - step) / 19.0;
      rig.set("cutoffHz", 20.0 * std::pow(1000.0, t));
      const std::vector<float> out = rig.render(in);
      all.insert(all.end(), out.begin(), out.end());
    }
    expectCleanSignal(all, mode == 3.0 ? 40.0f : 6.0f, "full resonance and drive");
  }
}

TEST(SynthFilterEffectTest, EnvelopeMovesTheCutoffBothWays) {
  const Params base = {{"cutoffHz", 300.0}, {"resonancePct", 10.0}, {"mix", 1.0},
                       {"envAttackMs", 2.0}};
  const double still = brightnessWith(with(base, "envAmountPct", 0.0), 0.2f);
  const double opens = brightnessWith(with(base, "envAmountPct", 100.0), 0.2f);
  const double closes = brightnessWith(with(base, "envAmountPct", -100.0), 0.2f);
  EXPECT_GT(opens, 1.5 * still) << "positive amount opens the filter on a loud input";
  EXPECT_LT(closes, still) << "negative amount closes it";

  // A quiet input leaves the envelope near zero: the cutoff stays put.
  const double quietStill = brightnessWith(with(base, "envAmountPct", 0.0), 0.0005f);
  const double quietOpen = brightnessWith(with(base, "envAmountPct", 100.0), 0.0005f);
  EXPECT_NEAR(quietOpen, quietStill, 0.2 * quietStill);
}

TEST(SynthFilterEffectTest, EnvelopeReleaseSetsHowLongTheFilterStaysOpen) {
  const auto brightnessAfterBurst = [](double releaseMs) {
    EffectRig rig(kKind, {{"cutoffHz", 300.0}, {"envAmountPct", 100.0}, {"envAttackMs", 2.0},
                          {"envReleaseMs", releaseMs}, {"mix", 1.0}});
    std::vector<float> in = noise(0.4, 3u, 0.3f, kRate);
    const std::vector<float> quiet = noise(0.3, 4u, 0.002f, kRate);
    in.insert(in.end(), quiet.begin(), quiet.end());
    const std::vector<float> out = rig.render(in);
    const std::vector<float> tail(out.begin() + static_cast<long>(0.46 * kRate),
                                  out.begin() + static_cast<long>(0.6 * kRate));
    return brightness(tail, 0);
  };
  EXPECT_GT(brightnessAfterBurst(1500.0), 1.3 * brightnessAfterBurst(20.0));
}

TEST(SynthFilterEffectTest, ReportsALongerTailWithMoreResonance) {
  EffectRig low(kKind, {{"resonancePct", 0.0}, {"cutoffHz", 200.0}});
  EffectRig high(kKind, {{"resonancePct", 95.0}, {"cutoffHz", 200.0}});
  EXPECT_GT(high.tailSeconds(), low.tailSeconds());
  EXPECT_EQ(high.latencySamples(), 0);
}

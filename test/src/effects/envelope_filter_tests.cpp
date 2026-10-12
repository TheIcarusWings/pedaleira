// filter.envelope (plugin/src/effects/filter/EnvelopeFilterEffect.cpp): the
// conformance test, then what an envelope filter is for: the cutoff follows
// how hard you play, up or down, as fast as the attack and as slowly as the
// release say, over the range asked for.
#include "effects/effect_test_helpers.h"

#include <gtest/gtest.h>

using namespace fxtest;

namespace {

constexpr const char* kKind = "filter.envelope";
constexpr double kRate = 48000.0;

float amp(double levelDb) { return static_cast<float>(std::pow(10.0, levelDb / 20.0)); }

// How bright a stretch of white noise came out: the energy of its first
// difference over its energy, which grows with the filter's cutoff (white
// noise has nothing else to say about it).
double brightness(const std::vector<float>& x, size_t from, size_t to) {
  to = std::min(to, x.size());
  double diff = 0.0, energy = 0.0;
  for (size_t i = std::max<size_t>(from, 1); i < to; ++i) {
    const double d = static_cast<double>(x[i]) - static_cast<double>(x[i - 1]);
    diff += d * d;
    energy += static_cast<double>(x[i]) * static_cast<double>(x[i]);
  }
  return std::sqrt(diff / std::max(energy, 1.0e-30));
}

// Noise of the given peak level through the filter: its brightness once the
// envelope has settled.
double brightnessAtLevel(const Params& params, double levelDb) {
  EffectRig rig(kKind, params);
  const std::vector<float> out = rig.render(noise(1.5, 9u, amp(levelDb), kRate));
  return brightness(out, static_cast<size_t>(1.0 * kRate), out.size());
}

}  // namespace

TEST(EnvelopeFilterEffectTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"sensitivityPct", 70.0}, {"attackMs", 5.0}, {"resonancePct", 65.0}};
  options.stateParams = {{"sensitivityPct", 30.0}, {"attackMs", 20.0},   {"releaseMs", 600.0},
                         {"lowHz", 120.0},         {"rangeOct", 4.5},    {"direction", 1.0},
                         {"resonancePct", 25.0},   {"filterType", 1.0},  {"levelDb", 4.0}};
  options.cpuAverageBudgetNs = 3000.0;
  options.cpuWorstBudgetNs = 150000.0;
  expectKindConforms("filter.envelope", options);
}

TEST(EnvelopeFilterEffectTest, CutoffRisesWithInputLevelWhenGoingUp) {
  for (const double type : {0.0, 1.0}) {
    SCOPED_TRACE(type == 0.0 ? "low-pass" : "band-pass");
    const Params params = {{"direction", 0.0}, {"filterType", type}, {"mix", 1.0}};
    const double soft = brightnessAtLevel(params, -46.0);
    const double medium = brightnessAtLevel(params, -34.0);
    const double hard = brightnessAtLevel(params, -22.0);
    EXPECT_LT(soft, medium);
    EXPECT_LT(medium, hard);
    EXPECT_GT(hard, 1.5 * soft) << "the sweep is too small to hear";
  }
}

TEST(EnvelopeFilterEffectTest, CutoffFallsWithInputLevelWhenGoingDown) {
  for (const double type : {0.0, 1.0}) {
    SCOPED_TRACE(type == 0.0 ? "low-pass" : "band-pass");
    const Params params = {{"direction", 1.0}, {"filterType", type}, {"mix", 1.0}};
    const double soft = brightnessAtLevel(params, -46.0);
    const double medium = brightnessAtLevel(params, -34.0);
    const double hard = brightnessAtLevel(params, -22.0);
    EXPECT_GT(soft, medium);
    EXPECT_GT(medium, hard);
    EXPECT_GT(soft, 1.5 * hard);
  }
}

TEST(EnvelopeFilterEffectTest, SensitivityDecidesHowEasilyTheFilterOpens) {
  const Params base = {{"direction", 0.0}, {"mix", 1.0}};
  Params dull = base;
  dull.emplace_back("sensitivityPct", 10.0);
  Params keen = base;
  keen.emplace_back("sensitivityPct", 90.0);
  EXPECT_LT(brightnessAtLevel(dull, -36.0), brightnessAtLevel(keen, -36.0));
}

TEST(EnvelopeFilterEffectTest, RangeSetsHowFarTheFilterTravels) {
  Params narrow = {{"rangeOct", 1.0}, {"mix", 1.0}};
  Params wide = {{"rangeOct", 5.0}, {"mix", 1.0}};
  const double narrowSpan = brightnessAtLevel(narrow, -20.0) / brightnessAtLevel(narrow, -50.0);
  const double wideSpan = brightnessAtLevel(wide, -20.0) / brightnessAtLevel(wide, -50.0);
  EXPECT_GT(wideSpan, 1.5 * narrowSpan);
}

TEST(EnvelopeFilterEffectTest, ReleaseSetsHowLongTheFilterStaysOpen) {
  // Loud noise, then quiet noise: right after the burst the cutoff is still
  // up if the release is long, already down if it is short.
  const auto brightnessAfterBurst = [](double releaseMs) {
    EffectRig rig(kKind, {{"releaseMs", releaseMs}, {"attackMs", 2.0}, {"mix", 1.0}});
    std::vector<float> in = noise(0.4, 3u, amp(-18.0), kRate);
    const std::vector<float> quiet = noise(0.4, 4u, amp(-60.0), kRate);
    in.insert(in.end(), quiet.begin(), quiet.end());
    const std::vector<float> out = rig.render(in);
    const size_t burstEnd = static_cast<size_t>(0.4 * kRate);
    return brightness(out, burstEnd + static_cast<size_t>(0.08 * kRate), burstEnd + static_cast<size_t>(0.16 * kRate));
  };
  EXPECT_GT(brightnessAfterBurst(1500.0), 1.4 * brightnessAfterBurst(30.0));
}

TEST(EnvelopeFilterEffectTest, AttackSetsHowFastTheFilterOpens) {
  // Quiet noise, then loud: 20 ms after the burst starts the cutoff is up
  // with a fast attack, still on its way with a slow one.
  const auto brightnessAfterOnset = [](double attackMs) {
    EffectRig rig(kKind, {{"attackMs", attackMs}, {"releaseMs", 300.0}, {"mix", 1.0}});
    std::vector<float> in = noise(0.4, 5u, amp(-60.0), kRate);
    const std::vector<float> loud = noise(0.4, 6u, amp(-18.0), kRate);
    in.insert(in.end(), loud.begin(), loud.end());
    const std::vector<float> out = rig.render(in);
    const size_t onset = static_cast<size_t>(0.4 * kRate);
    return brightness(out, onset + static_cast<size_t>(0.012 * kRate), onset + static_cast<size_t>(0.03 * kRate));
  };
  EXPECT_GT(brightnessAfterOnset(1.0), 1.3 * brightnessAfterOnset(150.0));
}

TEST(EnvelopeFilterEffectTest, LowPassKeepsTheLowEndAndBandPassDoesNot) {
  // A 100 Hz tone sits under the lowest cutoff (200 Hz): the low-pass passes
  // it, the band-pass leaves it far below its +6 dB peak.
  const auto gainAt100 = [](double type) {
    EffectRig rig(kKind, {{"filterType", type}, {"resonancePct", 40.0}, {"mix", 1.0}});
    return sineProbeGainDb(rig, 100.0, amp(-40.0), 1.5, 1.0);
  };
  EXPECT_GT(gainAt100(0.0), -6.0);
  EXPECT_LT(gainAt100(1.0), gainAt100(0.0) - 6.0);
}

TEST(EnvelopeFilterEffectTest, StereoPairShareAnEnvelopeAndDualMonoDoNot) {
  // A loud noise on the left, quiet on the right. A pair is filtered with
  // one cutoff (equal brightness); two guitars each open on their own.
  const Params params = {{"direction", 0.0}};
  const Stereo in{noise(1.5, 11u, amp(-20.0), kRate), noise(1.5, 12u, amp(-50.0), kRate)};
  const size_t from = static_cast<size_t>(1.0 * kRate);

  DirectKind linked(kKind, params, Layout::Stereo);
  const Stereo outLinked = linked.render(in);
  EXPECT_NEAR(brightness(outLinked.left, from, in.size()), brightness(outLinked.right, from, in.size()),
              0.03 * brightness(outLinked.left, from, in.size()));

  DirectKind dual(kKind, params, Layout::DualMono);
  const Stereo outDual = dual.render(in);
  EXPECT_GT(brightness(outDual.left, from, in.size()), 1.5 * brightness(outDual.right, from, in.size()));
}

TEST(EnvelopeFilterEffectTest, TailFollowsTheResonanceAndTheLowestCutoff) {
  EffectRig rig(kKind, {{"resonancePct", 100.0}, {"lowHz", 100.0}});
  const double sharpLow = rig.tailSeconds();
  rig.set("lowHz", 800.0);
  EXPECT_LT(rig.tailSeconds(), sharpLow);
  rig.set("lowHz", 100.0);
  rig.set("resonancePct", 0.0);
  EXPECT_LT(rig.tailSeconds(), sharpLow);
  EXPECT_EQ(rig.latencySamples(), 0);
}

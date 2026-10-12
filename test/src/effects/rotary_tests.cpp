// modulation.rotary (plugin/src/effects/modulation/RotarySpeaker.cpp): the
// conformance test, then what a rotating speaker is for: a horn and a drum
// rotor whose speeds are the stated slow and fast rates, run up and down with
// inertia over the stated times (the horn quicker than the drum), a drive
// stage, a crossover that sums flat when nothing moves, and two microphones
// that hear different things in a true stereo image only.
#include "effects/effect_test_helpers.h"
#include "effects/modulation_test_helpers.h"

#include <gtest/gtest.h>

using namespace fxtest;
using namespace modtest;

namespace {

constexpr const char* kKind = "modulation.rotary";

// Probe tones: well inside the horn band and the drum band (crossover 800 Hz).
constexpr double kHornProbeHz = 3000.0;
constexpr double kDrumProbeHz = 300.0;

// The parameter table's speed choice.
constexpr double kSlow = 0.0, kFast = 1.0, kBrake = 2.0;

// The values the effect documents (RotarySpeaker.cpp).
constexpr double kFastHornHz = 6.7;
constexpr double kSlowShare = 0.12;
constexpr double kDrumShare = 0.855;
constexpr double kHornRampSeconds = 1.2;
constexpr double kDrumRampSeconds = 3.0;

// A mono rig, fully wet, the clean setting with the rotors at full depth.
EffectRig monoRig(const Params& params) {
  Params all = {{"mix", 1.0}, {"drive", 0.0}, {"depth", 100.0}};
  all.insert(all.end(), params.begin(), params.end());
  return EffectRig(kKind, all, rigFor(Layout::Mono));
}

// Rate of the amplitude modulation of a probe tone (0: none).
double measuredRate(const Params& params, double probeHz, double seconds, double bpm = 120.0) {
  Params all = {{"mix", 1.0}, {"drive", 0.0}, {"depth", 100.0}};
  all.insert(all.end(), params.begin(), params.end());
  RigOptions options = rigFor(Layout::Mono);
  options.bpm = bpm;
  EffectRig rig(kKind, all, options);
  const std::vector<float> out = rig.render(sine(seconds, probeHz, 0.25f, kRate));
  return amplitudeModulationRateHz(out, kRate, static_cast<size_t>(samples(0.5)));
}

// The level of a probe tone as it rotates: the same rectify-and-smooth
// envelope amplitudeModulationRateHz uses, one value per millisecond.
std::vector<double> envelopeOf(const std::vector<float>& out) {
  const double coeff = 1.0 - std::exp(-2.0 * kPi * 40.0 / kRate);
  const size_t hop = static_cast<size_t>(kRate / 1000.0);
  std::vector<double> envelope;
  double first = 0.0, second = 0.0;
  for (size_t i = 0; i < out.size(); ++i) {
    first += coeff * (std::abs(static_cast<double>(out[i])) - first);
    second += coeff * (first - second);
    if (i % hop == 0)
      envelope.push_back(second);
  }
  return envelope;
}

// Times in seconds at which the envelope rises through the middle of its
// swing, after `fromSeconds` (with hysteresis, so ripple does not count).
std::vector<double> risingCrossings(const std::vector<double>& envelope, double fromSeconds) {
  const size_t start = static_cast<size_t>(fromSeconds * 1000.0);
  double lo = 1.0e9, hi = -1.0e9;
  for (size_t i = start; i < envelope.size(); ++i) {
    lo = std::min(lo, envelope[i]);
    hi = std::max(hi, envelope[i]);
  }
  const double mid = 0.5 * (lo + hi);
  const double band = 0.1 * (hi - lo);
  std::vector<double> times;
  bool below = envelope[start] < mid;
  for (size_t i = start; i < envelope.size(); ++i) {
    if (below && envelope[i] > mid + band) {
      below = false;
      times.push_back(static_cast<double>(i) / 1000.0);
    } else if (!below && envelope[i] < mid - band) {
      below = true;
    }
  }
  return times;
}

// A rotor going from `from` to `to` turns per second, starting at `start`
// seconds, at a steady acceleration that takes `rampSeconds`: its angle in
// turns at time t.
struct Rotor {
  double from, to, start, rampSeconds;
  double turns(double t) const {
    const double u = t - start;
    const double travelled = u <= 0.0 ? 0.0
                             : u < rampSeconds ? u * u / (2.0 * rampSeconds)
                                               : u - 0.5 * rampSeconds;
    return from * t + (to - from) * travelled;
  }
};

// Runs the kind alone at `before` for `lead` seconds, switches the speed,
// runs `after` seconds more and returns the probe tone it makes.
std::vector<float> speedChange(double probeHz, double before, double after, double lead,
                               double afterSeconds) {
  DirectKind kind(kKind, {{"depth", 100.0}, {"drive", 0.0}, {"rateHz", kFastHornHz},
                          {"speed", before}});
  std::vector<float> out = kind.render(sine(lead, probeHz, 0.25f, kRate));
  setKindParam(kind, "speed", after);
  const std::vector<float> rest = kind.render(sine(afterSeconds, probeHz, 0.25f, kRate));
  out.insert(out.end(), rest.begin(), rest.end());
  return out;
}

// Each revolution the envelope shows (rising through the middle) must be one
// turn of the rotor in the model: every pair of neighbouring crossings, the
// model's angle advances by one.
void expectTurnsMatch(const std::vector<float>& out, const Rotor& rotor, int minRevolutions) {
  const std::vector<double> envelope = envelopeOf(out);
  // The envelope trails the signal by the two smoothing stages (about 8 ms).
  const double lag = 0.008;
  const std::vector<double> crossings = risingCrossings(envelope, 0.3);
  ASSERT_GE(static_cast<int>(crossings.size()), minRevolutions + 1);
  for (size_t i = 0; i + 1 < crossings.size(); ++i) {
    const double advance = rotor.turns(crossings[i + 1] - lag) - rotor.turns(crossings[i] - lag);
    EXPECT_NEAR(advance, 1.0, 0.07) << "revolution " << i << " at " << crossings[i] << " s";
  }
}

}  // namespace

TEST(RotaryTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"speed", kFast}, {"depth", 80.0}, {"drive", 40.0}, {"rateHz", 5.0}};
  options.stateParams = {{"sync", 1.0},   {"division", fx::kDivEighth}, {"rateHz", 4.0},
                         {"speed", kBrake}, {"depth", 50.0},            {"drive", 60.0},
                         {"hornDb", 3.0}, {"width", 30.0}};
  // The wet is a delayed copy (about 0.5 to 1.5 ms): the effect, not latency.
  options.latency = LatencyCheck::AtMostOnset;
  options.cpuAverageBudgetNs = 18000.0;
  // About 50x the average: one block in thousands can be descheduled on a
  // busy machine, and that says nothing about the effect.
  options.cpuWorstBudgetNs = 150000.0;
  expectKindConforms("modulation.rotary", options);
}

TEST(RotaryTest, RegisteredFullyWet) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kKind);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->category, BlockCategory::Modulation);
  EXPECT_FLOAT_EQ(info->defaultMix, 1.0f);
}

TEST(RotaryTest, FastAndSlowRatesMatchTheSettingsForHornAndDrum) {
  // Fast horn 6.7 Hz, drum 85.5% of that; slow is 12% of fast.
  const Params fast = {{"speed", kFast}, {"rateHz", kFastHornHz}};
  const Params slow = {{"speed", kSlow}, {"rateHz", kFastHornHz}};
  EXPECT_NEAR(measuredRate(fast, kHornProbeHz, 5.0), kFastHornHz, 0.03 * kFastHornHz);
  EXPECT_NEAR(measuredRate(fast, kDrumProbeHz, 5.0), kFastHornHz * kDrumShare,
              0.03 * kFastHornHz * kDrumShare);
  EXPECT_NEAR(measuredRate(slow, kHornProbeHz, 14.0), kFastHornHz * kSlowShare,
              0.03 * kFastHornHz * kSlowShare);
  EXPECT_NEAR(measuredRate(slow, kDrumProbeHz, 14.0), kFastHornHz * kDrumShare * kSlowShare,
              0.04 * kFastHornHz * kDrumShare * kSlowShare);
  // The rate parameter scales both.
  const Params other = {{"speed", kFast}, {"rateHz", 3.0}};
  EXPECT_NEAR(measuredRate(other, kHornProbeHz, 6.0), 3.0, 0.09);
}

TEST(RotaryTest, SyncedRateFollowsTheTempoAtTwoTempos) {
  for (const double bpm : {120.0, 90.0}) {
    SCOPED_TRACE(bpm);
    const Params synced = {{"speed", kFast}, {"sync", 1.0},
                           {"division", static_cast<double>(fx::kDivQuarter)}, {"rateHz", 9.0}};
    const double hornHz = bpm / 60.0;
    EXPECT_NEAR(measuredRate(synced, kHornProbeHz, 8.0, bpm), hornHz, 0.03 * hornHz);
    EXPECT_NEAR(measuredRate(synced, kDrumProbeHz, 8.0, bpm), hornHz * kDrumShare,
                0.03 * hornHz * kDrumShare);
  }
}

TEST(RotaryTest, HornSpeedsUpOverItsRampTime) {
  // Slow for two seconds, then fast: every revolution the level shows
  // must fit a rotor that takes 1.2 s to get there.
  const Rotor rotor{kFastHornHz * kSlowShare, kFastHornHz, 2.0, kHornRampSeconds};
  const std::vector<float> out = speedChange(kHornProbeHz, kSlow, kFast, 2.0, 5.0);
  expectTurnsMatch(out, rotor, 20);
}

TEST(RotaryTest, DrumSpeedsUpOverItsLongerRampTime) {
  const double slowHz = kFastHornHz * kDrumShare * kSlowShare;
  const Rotor rotor{slowHz, kFastHornHz * kDrumShare, 2.0, kDrumRampSeconds};
  const std::vector<float> out = speedChange(kDrumProbeHz, kSlow, kFast, 2.0, 8.0);
  expectTurnsMatch(out, rotor, 25);
}

TEST(RotaryTest, BrakeRunsTheRotorsDownAndStopsThem) {
  // At the speed-up acceleration, so from fast to rest takes fast / accel.
  const double hornFast = kFastHornHz;
  const double hornAccel = hornFast * (1.0 - kSlowShare) / kHornRampSeconds;
  const Rotor horn{hornFast, 0.0, 2.0, hornFast / hornAccel};
  const std::vector<float> hornOut = speedChange(kHornProbeHz, kFast, kBrake, 2.0, 5.0);
  expectTurnsMatch(hornOut, horn, 10);
  // Stopped: the level holds still.
  EXPECT_EQ(amplitudeModulationRateHz(
                std::vector<float>(hornOut.begin() + samples(4.0), hornOut.end()), kRate), 0.0);

  const double drumFast = kFastHornHz * kDrumShare;
  const double drumAccel = drumFast * (1.0 - kSlowShare) / kDrumRampSeconds;
  const Rotor drum{drumFast, 0.0, 2.0, drumFast / drumAccel};
  const std::vector<float> drumOut = speedChange(kDrumProbeHz, kFast, kBrake, 2.0, 8.0);
  expectTurnsMatch(drumOut, drum, 10);
  EXPECT_EQ(amplitudeModulationRateHz(
                std::vector<float>(drumOut.begin() + samples(7.0), drumOut.end()), kRate), 0.0);
}

TEST(RotaryTest, TheHornGetsToSpeedBeforeTheDrum) {
  // Both rotors stop (brake) from fast: the horn is still after about 1.4 s
  // and the drum after about 3.4 s.
  const std::vector<float> hornOut = speedChange(kHornProbeHz, kFast, kBrake, 1.0, 5.0);
  const std::vector<float> drumOut = speedChange(kDrumProbeHz, kFast, kBrake, 1.0, 5.0);
  const auto stillAfter = [](const std::vector<float>& out, double seconds) {
    return amplitudeModulationRateHz(std::vector<float>(out.begin() + samples(seconds), out.end()),
                                     kRate) == 0.0;
  };
  EXPECT_TRUE(stillAfter(hornOut, 1.0 + kHornRampSeconds * 1.2));
  EXPECT_FALSE(stillAfter(drumOut, 1.0 + kHornRampSeconds * 1.2));
  EXPECT_TRUE(stillAfter(drumOut, 1.0 + kDrumRampSeconds * 1.4));
}

TEST(RotaryTest, DepthZeroSumsFlatAcrossTheCrossover) {
  EffectRig rig = monoRig({{"speed", kBrake}, {"depth", 0.0}});
  const std::vector<float> ir = captureImpulseResponse(rig, 0.5);
  const std::vector<double> freqs = {100.0, 200.0, 400.0, 700.0, 800.0, 900.0, 1600.0, 3200.0, 6400.0};
  const std::vector<double> response = magnitudeResponseDb(ir, freqs, kRate);
  for (size_t i = 0; i < freqs.size(); ++i)
    EXPECT_NEAR(response[i], 0.0, 0.5) << freqs[i];
}

TEST(RotaryTest, DriveAddsHarmonics) {
  const auto thd = [](double drive) {
    EffectRig rig(kKind, {{"speed", kBrake}, {"depth", 0.0}, {"drive", drive}, {"mix", 1.0}},
                  rigFor(Layout::Mono));
    const std::vector<float> in = sine(1.5, 300.0, 0.3f, kRate);
    return thdPercent(rig.render(in), 300.0, kRate, static_cast<size_t>(samples(0.5)));
  };
  EXPECT_LT(thd(0.0), 0.5);
  EXPECT_GT(thd(100.0), 8.0);
  EXPECT_GT(thd(50.0), thd(20.0));
}

TEST(RotaryTest, HornLevelBalancesTheHornAgainstTheDrum) {
  const auto gain = [](double hornDb, double probeHz) {
    EffectRig rig = monoRig({{"speed", kBrake}, {"depth", 0.0}, {"hornDb", hornDb}});
    return sineProbeGainDb(rig, probeHz);
  };
  EXPECT_NEAR(gain(12.0, kHornProbeHz) - gain(0.0, kHornProbeHz), 12.0, 0.5);
  EXPECT_NEAR(gain(12.0, kDrumProbeHz) - gain(0.0, kDrumProbeHz), 0.0, 0.5);
  EXPECT_NEAR(gain(-12.0, kHornProbeHz) - gain(0.0, kHornProbeHz), -12.0, 0.5);
}

TEST(RotaryTest, MicrophonesDifferOnlyInATrueStereoImage) {
  const std::vector<float> in = sine(2.0, kHornProbeHz, 0.25f, kRate);
  const Params wide = {{"speed", kFast}, {"depth", 100.0}, {"width", 100.0}, {"mix", 1.0}};
  {
    EffectRig rig(kKind, wide);
    const Stereo out = rig.renderBoth(in);
    // Two microphones half a turn apart: when one is facing the horn the
    // other is not.
    EXPECT_GT(relativeDifferenceDb(out.left, out.right, static_cast<size_t>(samples(0.3))), -6.0);
    EXPECT_NEAR(toDb(rmsOf(out.left)), toDb(rmsOf(out.right)), 1.0);
  }
  {
    EffectRig rig(kKind, {{"speed", kFast}, {"width", 0.0}, {"mix", 1.0}});
    const Stereo out = rig.renderBoth(in);
    EXPECT_LT(maxDifference(out.left, out.right), 1.0e-6f) << "width 0 is one microphone";
  }
  {
    // Two independent signals: a cabinet each, the same whatever the width.
    EffectRig rig(kKind, wide, rigFor(Layout::DualMono));
    const Stereo out = rig.renderBoth(in);
    EXPECT_LT(maxDifference(out.left, out.right), 1.0e-6f);
    EffectRig mono(kKind, wide, rigFor(Layout::Mono));
    EXPECT_LT(maxDifference(out.left, mono.render(in)), 1.0e-6f);
  }
  {
    // An amp model later in the lane throws the image away.
    DirectKind kind(kKind, {{"speed", kFast}, {"width", 100.0}}, Layout::Stereo);
    kind.ctx.nam.lastNamIndex = 2;
    const Stereo out = kind.render(Stereo{in, in});
    EXPECT_LT(maxDifference(out.left, out.right), 1.0e-6f);
  }
  {
    // And the right cabinet never hears the left guitar.
    DirectKind kind(kKind, {{"speed", kFast}, {"width", 100.0}}, Layout::DualMono);
    const Stereo out = kind.render(Stereo{in, std::vector<float>(in.size(), 0.0f)});
    EXPECT_GT(peakOf(out.left), 0.05f);
    EXPECT_EQ(peakOf(out.right), 0.0f);
  }
}

TEST(RotaryTest, StaysFiniteAndBoundedAtTheExtremes) {
  for (const Layout layout : {Layout::Mono, Layout::Stereo, Layout::DualMono}) {
    DirectKind kind(kKind,
                    {{"speed", kFast}, {"rateHz", 12.0}, {"depth", 100.0}, {"drive", 100.0},
                     {"hornDb", 12.0}, {"width", 100.0}},
                    layout);
    const std::vector<float> hot = noise(10.0, 17u, 1.0f, kRate);
    const Stereo out = kind.render(Stereo{hot, hot});
    expectCleanSignal(out.left, 6.0f, "extreme rotary, left");
    expectCleanSignal(out.right, 6.0f, "extreme rotary, right");
  }
}

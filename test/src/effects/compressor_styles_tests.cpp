// The compressor styles (plugin/src/effects/dynamics): dynamics.fet,
// dynamics.bus, dynamics.opto and dynamics.sustainer. Each goes through the
// conformance test of the framework (mono, stereo and dual mono); then what
// each one is for: the static curve, the attack and release times measured on
// step tests, the programme-dependent behaviour (the FET release, the bus auto
// release, the optical cell's two-stage release and attack), the sustainer
// holding a decaying note up, and for all of them the gain reduction reading
// against the reduction measured on the audio, a level near unity at the
// defaults on a -18 dBFS guitar-like signal, and no latency.
//
// Everything that is not about the chain runs a kind on its own (DirectKind):
// the audio is exactly what the kind produced.
#include "effects/effect_test_helpers.h"

#include <gtest/gtest.h>

using namespace fxtest;

namespace {

constexpr double kRate = 48000.0;

float amp(double levelDb) { return static_cast<float>(std::pow(10.0, levelDb / 20.0)); }

// Steady-state gain in dB of a sine, from the fundamental alone.
double sineGainDb(const char* kind, const Params& params, double levelDb, double seconds = 2.0,
                  double settleSeconds = 1.5, double freq = 1000.0) {
  DirectKind k(kind, params);
  const std::vector<float> in = sine(seconds, freq, amp(levelDb), kRate);
  const std::vector<float> out = k.render(in);
  const size_t skip = static_cast<size_t>(settleSeconds * kRate);
  return db(goertzelPower(out.data() + skip, out.size() - skip, freq, kRate)) -
         db(goertzelPower(in.data() + skip, in.size() - skip, freq, kRate));
}

struct Segment {
  double seconds;
  double levelDb;
};

std::vector<float> steppedSine(double freq, const std::vector<Segment>& segments) {
  std::vector<float> x;
  for (const Segment& s : segments) {
    const int n = static_cast<int>(std::lround(s.seconds * kRate));
    const float a = amp(s.levelDb);
    for (int i = 0; i < n; ++i)
      x.push_back(a * static_cast<float>(std::sin(2.0 * kPi * freq * static_cast<double>(x.size()) / kRate)));
  }
  return x;
}

// Gain reduction in dB against time: the level of `in` over the level of
// `out` in windows of `window` samples (a 4 kHz tone: whole periods), plus the
// gain the kind has with no reduction at all (`netGainDb`).
std::vector<double> reductionDb(const std::vector<float>& in, const std::vector<float>& out,
                                size_t window, double netGainDb = 0.0) {
  std::vector<double> reduction;
  for (size_t i = 0; i + window <= in.size(); i += window)
    reduction.push_back(netGainDb + toDb(rmsOf(in, i, i + window)) - toDb(rmsOf(out, i, i + window)));
  return reduction;
}

// The first index at or after `from` where the series reaches `target`
// going up (or down); -1 if it never does.
long firstAtOrAbove(const std::vector<double>& x, size_t from, double target) {
  for (size_t i = from; i < x.size(); ++i)
    if (x[i] >= target)
      return static_cast<long>(i - from);
  return -1;
}

long firstAtOrBelow(const std::vector<double>& x, size_t from, double target) {
  for (size_t i = from; i < x.size(); ++i)
    if (x[i] <= target)
      return static_cast<long>(i - from);
  return -1;
}

// A steady loud tone for `loudSeconds` between two quiet ones; the reduction
// series in windows of `window`, with the loud part starting at entry
// `loudStart` and ending at `loudEnd`.
struct StepRun {
  std::vector<double> reduction;
  size_t loudStart{0};
  size_t loudEnd{0};
};

StepRun stepRun(const char* kind, const Params& params, double quietDb, double loudDb,
                double loudSeconds, double netGainDb, size_t window, double tailSeconds = 1.5) {
  DirectKind k(kind, params);
  const std::vector<float> in =
      steppedSine(4000.0, {{0.3, quietDb}, {loudSeconds, loudDb}, {tailSeconds, quietDb}});
  StepRun run;
  run.reduction = reductionDb(in, k.render(in), window, netGainDb);
  run.loudStart = static_cast<size_t>(0.3 * kRate) / window;
  run.loudEnd = static_cast<size_t>((0.3 + loudSeconds) * kRate) / window;
  return run;
}

// Milliseconds from the end of the loud part until the reduction has fallen to
// 37% of what it was.
double releaseMs(const StepRun& run, size_t window) {
  const double start = run.reduction[run.loudEnd - 1];
  const long entries = firstAtOrBelow(run.reduction, run.loudEnd, 0.37 * start);
  return entries < 0 ? -1.0 : 1000.0 * static_cast<double>(entries * static_cast<long>(window)) / kRate;
}

// Milliseconds from the start of the loud part until the reduction reaches
// 63% of `final`.
double attackMs(const StepRun& run, size_t window, double final) {
  const long entries = firstAtOrAbove(run.reduction, run.loudStart, 0.63 * final);
  return entries < 0 ? -1.0 : 1000.0 * static_cast<double>(entries * static_cast<long>(window)) / kRate;
}

// The reduction measured on a long steady tone, and the reading the kind
// reports for the same audio.
struct ReadingCheck {
  double measured;
  double reported;
};

ReadingCheck readingAgainstAudio(const char* kindId, const Params& params, double levelDb,
                                 double netGainDb, double seconds = 3.0) {
  DirectKind kind(kindId, params);
  const std::vector<float> in = sine(seconds, 1000.0, amp(levelDb), kRate);
  const std::vector<float> out = kind.render(in);
  const size_t from = static_cast<size_t>((seconds - 0.4) * kRate);
  ReadingCheck check;
  check.measured = netGainDb + toDb(rmsOf(in, from)) - toDb(rmsOf(out, from));
  check.reported = kind.engine().getReading(0);
  return check;
}

// A loud tone on the left and a quiet one on the right, rendered as a linked
// stereo pair and as dual mono; gains of each side in dB.
struct LinkResult {
  double linkedLeft, linkedRight, dualLeft, dualRight;
};

LinkResult linkMeasurement(const char* kindId, const Params& params, double loudDb, double quietDb) {
  const Stereo in{sine(3.0, 1000.0, amp(loudDb), kRate), sine(3.0, 1500.0, amp(quietDb), kRate)};
  const size_t from = static_cast<size_t>(2.0 * kRate);
  const auto gain = [&](const std::vector<float>& out, const std::vector<float>& source) {
    return toDb(rmsOf(out, from)) - toDb(rmsOf(source, from));
  };
  LinkResult r{};
  DirectKind linked(kindId, params, Layout::Stereo);
  const Stereo a = linked.render(in);
  r.linkedLeft = gain(a.left, in.left);
  r.linkedRight = gain(a.right, in.right);
  DirectKind dual(kindId, params, Layout::DualMono);
  const Stereo b = dual.render(in);
  r.dualLeft = gain(b.left, in.left);
  r.dualRight = gain(b.right, in.right);
  return r;
}

// The musical test signal (five partials under a swell, a crest factor near
// 10 dB like a plucked chord) scaled to peak at -18 dBFS.
std::vector<float> guitarLikeSignal(double seconds) {
  std::vector<float> x = musicalSignal(seconds, kRate, 1.0f);
  const float scale = amp(-18.0) / peakOf(x);
  for (float& v : x)
    v *= scale;
  return x;
}

// At the defaults the level stays near what it was on a guitar-like signal
// that peaks at -18 dBFS: a sine and a chord-like signal, within 3 dB (RMS in
// against RMS out). Peak is the reference because the heavy styles hold the
// peaks, not the average.
void expectNearUnityAtDefaults(const char* kindId) {
  SCOPED_TRACE(kindId);
  EXPECT_NEAR(sineGainDb(kindId, {}, -18.0, 4.0, 3.0), 0.0, 3.0) << "sine at -18 dBFS peak";
  DirectKind kind(kindId);
  const std::vector<float> in = guitarLikeSignal(5.0);
  const std::vector<float> out = kind.render(in);
  const size_t skip = static_cast<size_t>(1.5 * kRate);
  EXPECT_NEAR(steadyStateGainDb(out, in, skip), 0.0, 3.0) << "guitar-like signal peaking at -18 dBFS";
}

void expectNoLatencyNoTail(const char* kindId) {
  SCOPED_TRACE(kindId);
  EffectRig rig(kindId);
  EXPECT_EQ(rig.latencySamples(), 0);
  EXPECT_EQ(rig.tailSeconds(), 0.0);
  DirectKind alone(kindId);
  EXPECT_EQ(alone.latencySamples(), 0);
  EXPECT_EQ(alone.tailSeconds(), 0.0);
}

void expectGainReductionReading(const char* kindId) {
  SCOPED_TRACE(kindId);
  DirectKind kind(kindId);
  ASSERT_TRUE(kind.valid());
  ASSERT_EQ(kind.engine().readingTable().size(), 1u);
  EXPECT_EQ(kind.engine().readingTable()[0].id, juce::String("gainReductionDb"));
  EXPECT_FLOAT_EQ(kind.engine().getReading(0), 0.0f) << "nothing processed yet";
  // Something loud enough to be reduced at the defaults.
  kind.render(sine(2.0, 1000.0, amp(-3.0), kRate));
  EXPECT_GT(kind.engine().getReading(0), 1.0f);
  kind.engine().reset();
  EXPECT_FLOAT_EQ(kind.engine().getReading(0), 0.0f) << "a reset puts the meter back to rest";
  // Reset clears the state too: the same audio again gives the same output.
  DirectKind fresh(kindId);
  const std::vector<float> burst = sine(0.3, 1000.0, amp(-3.0), kRate);
  kind.render(burst);
  kind.engine().reset();
  EXPECT_EQ(kind.render(burst), fresh.render(burst)) << "reset leaves no state behind";
}

}  // namespace

// ===========================================================================
// FET compressor
// ===========================================================================

TEST(FetCompressorTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"inputDb", 20.0}, {"ratio", 1.0}, {"attackUs", 50.0}, {"outputDb", -12.0}};
  options.stateParams = {{"inputDb", 18.0},   {"ratio", 3.0},     {"attackUs", 300.0},
                         {"releaseMs", 400.0}, {"outputDb", -4.0}, {"colour", 0.0}};
  // Up to +40 dB in and +12 dB out on noise at 0.25, with the ratio on top.
  options.sweepBound = 24.0f;
  expectKindConforms("dynamics.fet", options);
}

TEST(FetCompressorTest, StaticCurveFollowsTheRatioButtons) {
  // The threshold is fixed at -10 dBFS inside the unit and Input lifts the
  // signal into it. With 20 dB of input, a sine at -25 dBFS is 5 dB over and
  // one at -5 dBFS 25 dB over: 20 dB more in must give 20 / ratio dB more out.
  const double ratios[] = {4.0, 8.0, 12.0, 20.0};
  for (int button = 0; button < 4; ++button) {
    SCOPED_TRACE(ratios[button]);
    const Params params = {{"inputDb", 20.0}, {"outputDb", 0.0}, {"ratio", button},
                           {"colour", 0.0},   {"releaseMs", 50.0}};
    const double low = -25.0 + sineGainDb("dynamics.fet", params, -25.0);
    const double high = -5.0 + sineGainDb("dynamics.fet", params, -5.0);
    const double measuredRatio = 20.0 / (high - low);
    EXPECT_NEAR(measuredRatio, ratios[button], 0.1 * ratios[button]);
  }
  // Well under the threshold the unit is linear: just input and output.
  const Params quiet = {{"inputDb", 10.0}, {"outputDb", -4.0}, {"colour", 0.0}};
  EXPECT_NEAR(sineGainDb("dynamics.fet", quiet, -50.0), 6.0, 0.1);
}

TEST(FetCompressorTest, InputDrivesTheCompression) {
  // Same signal, more Input: more reduction, and the reading says so.
  double previous = -1.0;
  for (const double inputDb : {6.0, 14.0, 22.0, 30.0}) {
    DirectKind kind("dynamics.fet", {{"inputDb", inputDb}, {"colour", 0.0}});
    kind.render(sine(1.0, 1000.0, amp(-24.0), kRate));
    EXPECT_GT(kind.engine().getReading(0), previous) << inputDb;
    previous = kind.engine().getReading(0);
  }
}

TEST(FetCompressorTest, AttackIsFastAndReleaseFollowsTheKnob) {
  const size_t fine = 12;  // 250 us windows: whole periods of the 4 kHz tone
  const size_t coarse = 48;
  // Attack: 20 us is inside the first window, 800 us takes a few.
  {
    const Params fast = {{"inputDb", 20.0}, {"outputDb", -20.0}, {"ratio", 3.0},
                         {"attackUs", 20.0}, {"colour", 0.0}};
    const StepRun run = stepRun("dynamics.fet", fast, -60.0, -10.0, 1.0, 0.0, fine);
    const double final = run.reduction[run.loudEnd - 1];
    ASSERT_GT(final, 10.0);
    EXPECT_LT(attackMs(run, fine, final), 0.5);
  }
  {
    const Params slow = {{"inputDb", 20.0}, {"outputDb", -20.0}, {"ratio", 3.0},
                         {"attackUs", 800.0}, {"colour", 0.0}};
    const StepRun run = stepRun("dynamics.fet", slow, -60.0, -10.0, 1.0, 0.0, fine);
    const double final = run.reduction[run.loudEnd - 1];
    ASSERT_GT(final, 10.0);
    const double ms = attackMs(run, fine, final);
    EXPECT_GT(ms, 0.4 * 0.8);
    EXPECT_LT(ms, 2.0 * 0.8 + 0.5);
  }
  // Release: from the end of a long loud part, 63% of the reduction is gone
  // within 0.4 to 2 times the set time (the programme-dependent factor is
  // 0.8 to 1.5, and the detector holds a few ms).
  for (const double set : {50.0, 200.0, 800.0}) {
    SCOPED_TRACE(set);
    const Params params = {{"inputDb", 20.0}, {"outputDb", -20.0}, {"ratio", 3.0},
                           {"releaseMs", set}, {"colour", 0.0}};
    const StepRun run = stepRun("dynamics.fet", params, -60.0, -10.0, 1.5, 0.0, coarse, 3.0);
    ASSERT_GT(run.reduction[run.loudEnd - 1], 10.0);
    const double ms = releaseMs(run, coarse);
    EXPECT_GT(ms, 0.4 * set);
    EXPECT_LT(ms, 2.0 * set + 20.0);
  }
}

TEST(FetCompressorTest, ReleaseIsLongerAfterSustainedReduction) {
  // Mild programme dependence: the same release setting lets go sooner after
  // a short burst than after a long one.
  const Params params = {{"inputDb", 20.0}, {"outputDb", -20.0}, {"ratio", 3.0},
                         {"releaseMs", 300.0}, {"colour", 0.0}};
  const StepRun shortBurst = stepRun("dynamics.fet", params, -60.0, -10.0, 0.08, 0.0, 48, 3.0);
  const StepRun longHold = stepRun("dynamics.fet", params, -60.0, -10.0, 2.0, 0.0, 48, 3.0);
  const double shortMs = releaseMs(shortBurst, 48);
  const double longMs = releaseMs(longHold, 48);
  ASSERT_GT(shortMs, 0.0);
  ASSERT_GT(longMs, 0.0);
  EXPECT_GT(longMs, 1.25 * shortMs);
  EXPECT_LT(longMs, 2.5 * shortMs) << "mild: not a different release";
}

TEST(FetCompressorTest, ColourAddsALittleHarmonicContentAndCanBeSwitchedOff) {
  // A -30 dBFS tone with 12 dB of input stays under the threshold: any
  // distortion is the colour alone.
  const auto distortion = [](double colour, double ratioButton) {
    DirectKind kind("dynamics.fet", {{"inputDb", 12.0}, {"outputDb", -12.0}, {"colour", colour},
                                     {"ratio", ratioButton}});
    const std::vector<float> out = kind.render(sine(1.0, 1000.0, amp(-30.0), kRate));
    return thdPercent(out, 1000.0, kRate, static_cast<size_t>(0.5 * kRate));
  };
  EXPECT_LT(distortion(0.0, 0.0), 0.02) << "colour off: linear";
  const double on = distortion(1.0, 0.0);
  EXPECT_GT(on, 0.1);
  EXPECT_LT(on, 3.0) << "subtle";
  EXPECT_GT(distortion(1.0, 4.0), 1.5 * on) << "all the buttons in: more colour";
}

TEST(FetCompressorTest, LevelStaysNearUnityAtTheDefaults) { expectNearUnityAtDefaults("dynamics.fet"); }

TEST(FetCompressorTest, ReadingMatchesTheMeasuredReduction) {
  for (const double ratio : {0.0, 3.0}) {
    const Params params = {{"inputDb", 24.0}, {"outputDb", -24.0}, {"ratio", ratio}, {"colour", 0.0}};
    const ReadingCheck c = readingAgainstAudio("dynamics.fet", params, -20.0, 0.0);
    EXPECT_GT(c.measured, 6.0);
    EXPECT_NEAR(c.reported, c.measured, 1.0);
  }
  expectGainReductionReading("dynamics.fet");
}

TEST(FetCompressorTest, StereoPairIsLinkedAndDualMonoIsNot) {
  const Params params = {{"inputDb", 20.0}, {"outputDb", -20.0}, {"colour", 0.0}};
  const LinkResult r = linkMeasurement("dynamics.fet", params, -10.0, -50.0);
  EXPECT_LT(r.linkedLeft, -10.0);
  EXPECT_NEAR(r.linkedRight, r.linkedLeft, 0.3);
  EXPECT_LT(r.dualLeft, -10.0);
  EXPECT_NEAR(r.dualRight, 0.0, 0.1);
}

TEST(FetCompressorTest, ReportsNoLatency) { expectNoLatencyNoTail("dynamics.fet"); }

// ===========================================================================
// Bus compressor
// ===========================================================================

TEST(BusCompressorTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"thresholdDb", -30.0}, {"ratio", 2.0}, {"attack", 2.0}, {"release", 4.0},
                    {"makeupDb", 3.0}};
  options.stateParams = {{"thresholdDb", -24.0}, {"ratio", 0.0}, {"attack", 5.0},
                         {"release", 3.0},       {"makeupDb", 6.0}};
  // Up to 24 dB of makeup on noise at 0.25.
  options.sweepBound = 8.0f;
  expectKindConforms("dynamics.bus", options);
}

TEST(BusCompressorTest, StaticCurveFollowsTheRatioAboveTheKnee) {
  // The knee of a feedback compressor ends ratio * 2 dB above the threshold;
  // beyond it 16 dB more in gives 16 / ratio dB more out.
  const double ratios[] = {2.0, 4.0, 10.0};
  for (int choice = 0; choice < 3; ++choice) {
    SCOPED_TRACE(ratios[choice]);
    const double u1 = 2.0 * ratios[choice] + 6.0;
    const Params params = {{"thresholdDb", -48.0}, {"ratio", choice}, {"attack", 2.0},
                           {"release", 0.0}};
    const double low = -48.0 + u1 + sineGainDb("dynamics.bus", params, -48.0 + u1);
    const double high = -48.0 + u1 + 16.0 + sineGainDb("dynamics.bus", params, -48.0 + u1 + 16.0);
    EXPECT_NEAR(16.0 / (high - low), ratios[choice], 0.1 * ratios[choice]);
  }
}

TEST(BusCompressorTest, TheKneeIsSmoothAndMakeupIsAPlainGain) {
  // 4:1 with a 4 dB output-side knee: at the threshold the feedback curve
  // takes 0.67 dB (k e^2 / 2W with e = 4/3, k = 3), nothing 2 dB under it,
  // and it never jumps.
  const Params params = {{"thresholdDb", -30.0}, {"ratio", 1.0}, {"attack", 2.0}, {"release", 0.0}};
  EXPECT_NEAR(sineGainDb("dynamics.bus", params, -34.0), 0.0, 0.1);
  EXPECT_NEAR(sineGainDb("dynamics.bus", params, -30.0), -0.67, 0.25);
  double previous = sineGainDb("dynamics.bus", params, -36.0);
  for (double level = -35.0; level <= -12.0; level += 1.0) {
    const double gain = sineGainDb("dynamics.bus", params, level);
    EXPECT_LE(gain, previous + 0.05) << level;
    EXPECT_GE(gain, previous - 0.8) << level;
    previous = gain;
  }
  Params withMakeup = params;
  withMakeup.emplace_back("makeupDb", 6.0);
  EXPECT_NEAR(sineGainDb("dynamics.bus", withMakeup, -60.0), 6.0, 0.1);
}

TEST(BusCompressorTest, AttackAndReleaseChoicesGiveTheirTimes) {
  const size_t window = 48;  // 1 ms
  const double attacks[] = {0.1, 0.3, 1.0, 3.0, 10.0, 30.0};
  for (const int choice : {2, 4}) {
    SCOPED_TRACE(attacks[choice]);
    const Params params = {{"thresholdDb", -30.0}, {"ratio", 2.0}, {"attack", choice}, {"release", 1.0}};
    const StepRun run = stepRun("dynamics.bus", params, -60.0, -10.0, 1.0, 0.0, window);
    const double final = run.reduction[run.loudEnd - 1];
    ASSERT_GT(final, 10.0);
    const double ms = attackMs(run, window, final);
    EXPECT_GT(ms, 0.4 * attacks[choice]);
    EXPECT_LT(ms, 2.0 * attacks[choice] + 1.5);
  }
  const double releases[] = {0.1, 0.3, 0.6, 1.2};
  for (const int choice : {0, 1, 2}) {
    SCOPED_TRACE(releases[choice]);
    const Params params = {{"thresholdDb", -30.0}, {"ratio", 2.0}, {"attack", 2.0}, {"release", choice}};
    const StepRun run = stepRun("dynamics.bus", params, -60.0, -10.0, 1.5, 0.0, window, 4.0);
    ASSERT_GT(run.reduction[run.loudEnd - 1], 10.0);
    const double ms = releaseMs(run, window);
    EXPECT_GT(ms, 400.0 * releases[choice]);
    EXPECT_LT(ms, 2000.0 * releases[choice] + 20.0);
  }
}

TEST(BusCompressorTest, AutoReleaseFollowsTheProgramme) {
  const Params params = {{"thresholdDb", -30.0}, {"ratio", 2.0}, {"attack", 2.0}, {"release", 4.0}};
  const StepRun shortBurst = stepRun("dynamics.bus", params, -60.0, -10.0, 0.06, 0.0, 48, 4.0);
  const StepRun longHold = stepRun("dynamics.bus", params, -60.0, -10.0, 2.0, 0.0, 48, 5.0);
  const double shortMs = releaseMs(shortBurst, 48);
  const double longMs = releaseMs(longHold, 48);
  ASSERT_GT(shortMs, 0.0);
  ASSERT_GT(longMs, 0.0);
  EXPECT_LT(shortMs, 400.0) << "a short peak lets go fast";
  EXPECT_GT(longMs, 600.0) << "sustained reduction lets go slowly";
  EXPECT_GT(longMs, 2.5 * shortMs);
}

TEST(BusCompressorTest, LevelStaysNearUnityAtTheDefaults) { expectNearUnityAtDefaults("dynamics.bus"); }

TEST(BusCompressorTest, ReadingMatchesTheMeasuredReduction) {
  for (const double choice : {0.0, 1.0, 2.0}) {
    const Params params = {{"thresholdDb", -30.0}, {"ratio", choice}};
    const ReadingCheck c = readingAgainstAudio("dynamics.bus", params, -10.0, 0.0);
    EXPECT_GT(c.measured, 6.0);
    EXPECT_NEAR(c.reported, c.measured, 1.0);
  }
  expectGainReductionReading("dynamics.bus");
}

TEST(BusCompressorTest, StereoPairIsLinkedAndDualMonoIsNot) {
  const Params params = {{"thresholdDb", -30.0}, {"ratio", 1.0}, {"attack", 2.0}};
  const LinkResult r = linkMeasurement("dynamics.bus", params, -10.0, -50.0);
  EXPECT_LT(r.linkedLeft, -10.0);
  EXPECT_NEAR(r.linkedRight, r.linkedLeft, 0.3) << "the louder channel drives both";
  EXPECT_LT(r.dualLeft, -10.0);
  EXPECT_NEAR(r.dualRight, 0.0, 0.1);
}

TEST(BusCompressorTest, ReportsNoLatency) { expectNoLatencyNoTail("dynamics.bus"); }

// ===========================================================================
// Optical compressor
// ===========================================================================

TEST(OpticalCompressorTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"peakReduction", 70.0}, {"gainDb", 6.0}, {"mode", 1.0}};
  options.stateParams = {{"peakReduction", 30.0}, {"gainDb", 9.0}, {"mode", 1.0}};
  // Up to 24 dB of gain on noise at 0.25, after the reduction.
  options.sweepBound = 8.0f;
  expectKindConforms("dynamics.opto", options);
}

TEST(OpticalCompressorTest, ReleaseHasAFastPartAndASlowTail) {
  // After a long loud part: the first half of the reduction goes in a fraction
  // of a second, the rest takes seconds.
  const size_t window = 48;
  const Params params = {{"peakReduction", 50.0}, {"gainDb", 0.0}};
  const StepRun run = stepRun("dynamics.opto", params, -60.0, -10.0, 4.0, 0.0, window, 6.0);
  const double start = run.reduction[run.loudEnd - 1];
  ASSERT_GT(start, 6.0);
  const long half = firstAtOrBelow(run.reduction, run.loudEnd, 0.5 * start);
  const long tenth = firstAtOrBelow(run.reduction, run.loudEnd, 0.1 * start);
  ASSERT_GT(half, 0);
  ASSERT_GT(tenth, 0) << "the tail ends within the render";
  const double halfMs = 1000.0 * static_cast<double>(half * static_cast<long>(window)) / kRate;
  const double tenthMs = 1000.0 * static_cast<double>(tenth * static_cast<long>(window)) / kRate;
  EXPECT_LT(halfMs, 250.0) << "fast first half";
  EXPECT_GT(tenthMs - halfMs, 3.0 * halfMs) << "slow tail after it";
  EXPECT_GT(tenthMs, 1000.0);
}

TEST(OpticalCompressorTest, AttackIsSlowAndDependsOnTheProgramme) {
  const size_t window = 48;
  // Reduction 15 ms and 150 ms after the step, as a share of the 150 ms one:
  // the harder drive gets there sooner.
  const auto shareAt15 = [&](double loudDb) {
    const Params params = {{"peakReduction", 50.0}, {"gainDb", 0.0}};
    const StepRun run = stepRun("dynamics.opto", params, -80.0, loudDb, 3.0, 0.0, window);
    return run.reduction[run.loudStart + 15] / run.reduction[run.loudStart + 150];
  };
  const double gentle = shareAt15(-22.0);
  const double hard = shareAt15(-4.0);
  EXPECT_GT(hard, gentle + 0.08);

  // Slow: well under the final reduction in the first millisecond or two, and
  // it keeps tightening for a second as the slow part of the cell charges.
  const Params params = {{"peakReduction", 50.0}, {"gainDb", 0.0}};
  const StepRun run = stepRun("dynamics.opto", params, -80.0, -10.0, 3.0, 0.0, window);
  const double settled = run.reduction[run.loudEnd - 1];
  EXPECT_LT(run.reduction[run.loudStart + 2], 0.15 * settled);
  EXPECT_GT(run.reduction[run.loudStart + 2000], 1.2 * run.reduction[run.loudStart + 100]);
}

TEST(OpticalCompressorTest, PeakReductionAndTheLimitSwitchSetTheDepth) {
  const auto reductionAt = [](double peakReduction, double mode, double levelDb) {
    DirectKind kind("dynamics.opto", {{"peakReduction", peakReduction}, {"mode", mode}, {"gainDb", 0.0}});
    kind.render(sine(4.0, 1000.0, amp(levelDb), kRate));
    return static_cast<double>(kind.engine().getReading(0));
  };
  EXPECT_LT(reductionAt(0.0, 0.0, -12.0), 0.1);
  double previous = 0.0;
  for (const double knob : {20.0, 40.0, 60.0, 80.0, 100.0}) {
    const double r = reductionAt(knob, 0.0, -12.0);
    EXPECT_GT(r, previous) << knob;
    previous = r;
  }
  // Limit squeezes harder than compress once well over the threshold.
  EXPECT_GT(reductionAt(50.0, 1.0, -6.0), reductionAt(50.0, 0.0, -6.0) + 3.0);
}

TEST(OpticalCompressorTest, LevelStaysNearUnityAtTheDefaults) { expectNearUnityAtDefaults("dynamics.opto"); }

TEST(OpticalCompressorTest, ReadingMatchesTheMeasuredReduction) {
  for (const double mode : {0.0, 1.0}) {
    const Params params = {{"peakReduction", 60.0}, {"gainDb", 0.0}, {"mode", mode}};
    const ReadingCheck c = readingAgainstAudio("dynamics.opto", params, -12.0, 0.0, 6.0);
    EXPECT_GT(c.measured, 6.0);
    EXPECT_NEAR(c.reported, c.measured, 1.0);
  }
  expectGainReductionReading("dynamics.opto");
}

TEST(OpticalCompressorTest, StereoPairIsLinkedAndDualMonoIsNot) {
  const Params params = {{"peakReduction", 50.0}, {"gainDb", 0.0}};
  const LinkResult r = linkMeasurement("dynamics.opto", params, -10.0, -60.0);
  EXPECT_LT(r.linkedLeft, -6.0);
  EXPECT_NEAR(r.linkedRight, r.linkedLeft, 0.3);
  EXPECT_LT(r.dualLeft, -6.0);
  EXPECT_NEAR(r.dualRight, 0.0, 0.1);
}

TEST(OpticalCompressorTest, ReportsNoLatency) { expectNoLatencyNoTail("dynamics.opto"); }

// ===========================================================================
// Sustainer
// ===========================================================================

TEST(SustainerTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"sustain", 80.0}, {"attack", 30.0}, {"tone", 70.0}, {"levelDb", -3.0}};
  options.stateParams = {{"sustain", 30.0}, {"attack", 80.0}, {"tone", 20.0}, {"levelDb", 4.0}};
  // Sustain at 100% puts a lot of gain on the noise before it is compressed.
  options.sweepBound = 8.0f;
  expectKindConforms("dynamics.sustainer", options);
}

TEST(SustainerTest, EvensOutADecayingNote) {
  // A note that dies away by 40 dB in 2 s. The sustainer's output falls much
  // less than the input does.
  const double seconds = 2.0;
  const size_t n = static_cast<size_t>(seconds * kRate);
  std::vector<float> note(n);
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / kRate;
    note[i] = amp(-20.0 - 20.0 * t) * static_cast<float>(std::sin(2.0 * kPi * 1000.0 * t));
  }
  DirectKind kind("dynamics.sustainer", {{"sustain", 80.0}});
  const std::vector<float> out = kind.render(note);
  const size_t window = 4800;  // 100 ms: whole periods
  const auto levelAt = [&](const std::vector<float>& x, double t) {
    const size_t from = static_cast<size_t>(t * kRate);
    return toDb(rmsOf(x, from, from + window));
  };
  const double inFall = levelAt(note, 0.2) - levelAt(note, 1.8);
  const double outFall = levelAt(out, 0.2) - levelAt(out, 1.8);
  EXPECT_NEAR(inFall, 32.0, 0.5);
  EXPECT_LT(outFall, 0.5 * inFall);
  EXPECT_GT(outFall, 0.0) << "it is a compressor, not a limiter that holds it flat";
}

TEST(SustainerTest, SustainDeepensTheCompression) {
  double previous = -1.0;
  for (const double knob : {10.0, 30.0, 50.0, 70.0, 90.0}) {
    DirectKind kind("dynamics.sustainer", {{"sustain", knob}});
    kind.render(sine(3.0, 1000.0, amp(-12.0), kRate));
    EXPECT_GT(kind.engine().getReading(0), previous) << knob;
    previous = kind.engine().getReading(0);
  }
  // A heavy ratio: at full sustain 30 dB more in is well under 3 dB more out.
  const double low = -35.0 + sineGainDb("dynamics.sustainer", {{"sustain", 100.0}}, -35.0, 3.0, 2.5);
  const double high = -5.0 + sineGainDb("dynamics.sustainer", {{"sustain", 100.0}}, -5.0, 3.0, 2.5);
  EXPECT_LT(high - low, 3.0);
  EXPECT_GT(high - low, 0.0);
}

TEST(SustainerTest, AttackControlsHowMuchOfThePickPasses) {
  // A tone that starts at once. With Attack at 0 the gain is down within a
  // millisecond; at 100% the first milliseconds go through before it comes
  // down. Level of the first 5 ms against the settled level.
  const auto overshootDb = [](double attack) {
    DirectKind kind("dynamics.sustainer", {{"sustain", 60.0}, {"attack", attack}});
    std::vector<float> in(static_cast<size_t>(0.1 * kRate), 0.0f);
    const std::vector<float> note = sine(1.5, 1000.0, amp(-20.0), kRate);
    in.insert(in.end(), note.begin(), note.end());
    const std::vector<float> out = kind.render(in);
    const size_t start = static_cast<size_t>(0.1 * kRate);
    const size_t five = static_cast<size_t>(0.005 * kRate);
    const size_t settled = static_cast<size_t>(1.4 * kRate);
    return toDb(rmsOf(out, start, start + five)) - toDb(rmsOf(out, settled, settled + 4800));
  };
  const double squashed = overshootDb(0.0);
  const double open = overshootDb(100.0);
  EXPECT_LT(squashed, 5.0);
  EXPECT_GT(open, 8.0);
  EXPECT_GT(open, squashed + 6.0);
}

TEST(SustainerTest, ToneTiltsTheHighsAndLevelIsAPlainGain) {
  // Sustain at 0 keeps a quiet tone clear of the compression and the makeup
  // at nothing, so what is left is the tone filter and Level.
  const auto gain = [](double tone, double freq, double level = 0.0) {
    return sineGainDb("dynamics.sustainer",
                      {{"sustain", 0.0}, {"tone", tone}, {"levelDb", level}}, -50.0, 1.0, 0.5, freq);
  };
  EXPECT_NEAR(gain(50.0, 8000.0), 0.0, 0.3);
  EXPECT_NEAR(gain(50.0, 100.0), 0.0, 0.3);
  EXPECT_GT(gain(100.0, 8000.0) - gain(0.0, 8000.0), 14.0);
  EXPECT_NEAR(gain(100.0, 100.0), 0.0, 1.0);
  EXPECT_NEAR(gain(0.0, 100.0), 0.0, 1.0);
  EXPECT_NEAR(gain(50.0, 1000.0, 6.0), 6.0, 0.2);
}

TEST(SustainerTest, LevelStaysNearUnityAtTheDefaults) {
  expectNearUnityAtDefaults("dynamics.sustainer");
  // The makeup is worked out so a -18 dBFS tone keeps its level at any
  // setting of Sustain.
  for (const double knob : {10.0, 40.0, 70.0, 100.0})
    EXPECT_NEAR(sineGainDb("dynamics.sustainer", {{"sustain", knob}}, -18.0, 4.0, 3.0), 0.0, 3.0) << knob;
}

TEST(SustainerTest, ReadingMatchesTheMeasuredReduction) {
  // Makeup is on top of the reduction: measure with the level it adds taken
  // out (a tone far above the threshold and the makeup known from a quiet
  // one that is not reduced).
  const Params params = {{"sustain", 70.0}, {"tone", 50.0}};
  const double makeupDb = sineGainDb("dynamics.sustainer", params, -80.0, 1.0, 0.5);
  const ReadingCheck c = readingAgainstAudio("dynamics.sustainer", params, -10.0, makeupDb, 4.0);
  EXPECT_GT(c.measured, 10.0);
  EXPECT_NEAR(c.reported, c.measured, 1.0);
  expectGainReductionReading("dynamics.sustainer");
}

TEST(SustainerTest, StereoPairIsLinkedAndDualMonoIsNot) {
  const Params params = {{"sustain", 50.0}};
  const LinkResult r = linkMeasurement("dynamics.sustainer", params, -10.0, -50.0);
  EXPECT_NEAR(r.linkedRight, r.linkedLeft, 0.3);
  EXPECT_GT(r.dualRight, r.dualLeft + 6.0) << "the quiet guitar keeps its own gain";
}

TEST(SustainerTest, ReportsNoLatency) { expectNoLatencyNoTail("dynamics.sustainer"); }

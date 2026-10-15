// dynamics.adaptivegate (plugin/src/effects/dynamics/AdaptiveGateEffect.cpp):
// the conformance test, then what the kind is for: the threshold settles on
// the noise floor plus the margin at quiet and noisy levels (and follows a
// floor that rises), a plucked note opens, holds and closes without clicks,
// hum under the threshold is filtered out of the detector, switching track
// off freezes the floor, the range limits the attenuation, and silence stays
// silent. And what it must never do: learn a held note as noise. A
// sustained note after a high-gain amp or a compressor (a clipped tone, a
// saw, a held chord, feedback) is as steady as a noise floor, and the floor
// must not climb up under it and cut the player off mid-note, at 44.1, 48
// and 96 kHz.
#include "effects/effect_test_helpers.h"

#include "effects/EffectDsp.h"

#include <gtest/gtest.h>

using namespace fxtest;

namespace {

constexpr const char* kKind = "dynamics.adaptivegate";
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

// Reading indices (see readingSpecs).
constexpr int kFloorReading = 0;
constexpr int kThresholdReading = 1;
constexpr int kOpenReading = 2;

float amp(double levelDb) { return static_cast<float>(std::pow(10.0, levelDb / 20.0)); }

// White noise of the given broadband RMS level.
std::vector<float> noiseAt(double seconds, unsigned seed, double rmsDb, double rate = kRate) {
  return noise(seconds, seed, amp(rmsDb) * std::sqrt(3.0f), rate);
}

// Prepares the kind at another sample rate (DirectKind prepares at 48 kHz).
void prepareAt(DirectKind& kind, double rate) {
  BlockPrepareSpec spec;
  spec.chainSampleRate = rate;
  spec.oversampleFactor = 1;
  spec.baseBlockSize = kBlock;
  spec.maxBlockSize = kBlock;
  spec.numChannels = 2;
  kind.engine().prepareForChain(spec);
}

// RMS in dB of `x` through the kind's sidechain (high pass 100 Hz, low pass
// 5 kHz): what the floor tracker should find in steady noise.
double sidechainRmsDb(const std::vector<float>& x) {
  fx::Biquad hp, lp;
  hp.setCoeffs(fx::BiquadCoeffs::highpass(100.0, 0.7071, kRate));
  lp.setCoeffs(fx::BiquadCoeffs::lowpass(5000.0, 0.7071, kRate));
  std::vector<float> y(x.size());
  for (size_t i = 0; i < x.size(); ++i)
    y[i] = lp.process(hp.process(x[i]));
  return toDb(rmsOf(y, static_cast<size_t>(kRate), y.size()));
}

// Plays `in` through the kind block by block. `each` is called after every
// block with the time (seconds) at its end.
template <typename Fn>
std::vector<float> play(DirectKind& kind, const std::vector<float>& in, Fn each, double rate = kRate) {
  std::vector<float> out;
  out.reserve(in.size());
  for (size_t offset = 0; offset < in.size(); offset += kBlock) {
    const size_t n = std::min<size_t>(kBlock, in.size() - offset);
    const std::vector<float> block(in.begin() + static_cast<std::ptrdiff_t>(offset),
                                   in.begin() + static_cast<std::ptrdiff_t>(offset + n));
    const std::vector<float> o = kind.render(block);
    out.insert(out.end(), o.begin(), o.end());
    each(static_cast<double>(offset + n) / rate);
  }
  return out;
}

std::vector<float> play(DirectKind& kind, const std::vector<float>& in) {
  return play(kind, in, [](double) {});
}

void setParam(DirectKind& kind, const char* id, double value) {
  const int index = kind.engine().findParameter(id);
  ASSERT_GE(index, 0) << id;
  kind.engine().setParameter(index, static_cast<float>(value));
}

double reading(DirectKind& kind, int index) { return kind.engine().getReading(index); }

std::vector<float> concat(std::vector<float> a, const std::vector<float>& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

// Seconds after `from` at which `value()` is within `tolerance` of `target`
// and stays so to the end of the run; -1 if it never settles.
struct SettleClock {
  double target;
  double tolerance;
  double from;
  double settledAt{-1.0};
  void sample(double now, double value) {
    if (now < from)
      return;
    if (std::abs(value - target) <= tolerance) {
      if (settledAt < 0.0)
        settledAt = now - from;
    } else {
      settledAt = -1.0;
    }
  }
};

}  // namespace

TEST(AdaptiveGateEffectTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"thresholdDb", -24.0}, {"rangeDb", -30.0}, {"holdMs", 20.0}};
  options.stateParams = {{"thresholdDb", -33.0}, {"margin", 15.0},    {"hysteresis", 6.0},
                         {"attackMs", 3.0},      {"holdMs", 80.0},    {"releaseMs", 300.0},
                         {"rangeDb", -45.0},     {"track", 0.0}};
  // The played-or-noise detector (a 1024 point FFT and an autocorrelation
  // every 50 ms) costs about a microsecond per block.
  options.cpuAverageBudgetNs = 6000.0;
  options.cpuWorstBudgetNs = 200000.0;
  expectKindConforms("dynamics.adaptivegate", options);
}

TEST(AdaptiveGateEffectTest, ThresholdSettlesOnTheFloorPlusMarginAtQuietAndNoisyLevels) {
  for (const double level : {-70.0, -50.0}) {
    SCOPED_TRACE(level);
    DirectKind kind(kKind);
    ASSERT_TRUE(kind.valid());
    const std::vector<float> in = noiseAt(8.0, 5u, level);
    const double side = sidechainRmsDb(in);
    SettleClock clock{side + 10.0, 1.5, 0.0};
    play(kind, in, [&](double now) { clock.sample(now, reading(kind, kThresholdReading)); });
    EXPECT_NEAR(reading(kind, kFloorReading), side, 1.5);
    EXPECT_NEAR(reading(kind, kThresholdReading), side + 10.0, 1.5);
    EXPECT_GE(clock.settledAt, 0.0);
    EXPECT_LT(clock.settledAt, 2.0) << "settling time " << clock.settledAt << " s";
    std::cout << "[ settle ] steady noise at " << level << " dBFS: threshold within 1.5 dB of floor + "
              << "margin after " << clock.settledAt << " s\n";
  }
}

TEST(AdaptiveGateEffectTest, FollowsAFloorThatRisesAndThatFalls) {
  DirectKind kind(kKind);
  ASSERT_TRUE(kind.valid());
  const std::vector<float> quiet = noiseAt(5.0, 1u, -70.0);
  const std::vector<float> loud = noiseAt(10.0, 2u, -50.0);
  const double sideQuiet = sidechainRmsDb(quiet);
  const double sideLoud = sidechainRmsDb(loud);

  play(kind, quiet);
  EXPECT_NEAR(reading(kind, kThresholdReading), sideQuiet + 10.0, 1.5);

  SettleClock up{sideLoud + 10.0, 1.5, 0.0};
  play(kind, loud, [&](double now) { up.sample(now, reading(kind, kThresholdReading)); });
  EXPECT_GE(up.settledAt, 0.0);
  EXPECT_LT(up.settledAt, 6.0) << "settling time " << up.settledAt << " s";
  std::cout << "[ settle ] floor rising -70 -> -50 dBFS: settled after " << up.settledAt << " s\n";

  SettleClock down{sideQuiet + 10.0, 1.5, 0.0};
  play(kind, quiet, [&](double now) { down.sample(now, reading(kind, kThresholdReading)); });
  EXPECT_GE(down.settledAt, 0.0);
  EXPECT_LT(down.settledAt, 2.0) << "settling time " << down.settledAt << " s";
  std::cout << "[ settle ] floor falling -50 -> -70 dBFS: settled after " << down.settledAt << " s\n";
}

TEST(AdaptiveGateEffectTest, FollowsAColouredFloorThatRises) {
  // The noise after a cab is far from white: low passed twice at 1.5 kHz.
  const auto coloured = [](double seconds, unsigned seed, double rmsDb) {
    std::vector<float> x = noiseAt(seconds, seed, rmsDb);
    fx::Biquad a, b;
    a.setCoeffs(fx::BiquadCoeffs::lowpass(1500.0, 0.7071, kRate));
    b.setCoeffs(fx::BiquadCoeffs::lowpass(1500.0, 0.7071, kRate));
    for (auto& v : x)
      v = b.process(a.process(v));
    return x;
  };
  DirectKind kind(kKind);
  const std::vector<float> quiet = coloured(5.0, 21u, -64.0);
  const std::vector<float> loud = coloured(10.0, 22u, -44.0);
  play(kind, quiet);
  EXPECT_NEAR(reading(kind, kThresholdReading), sidechainRmsDb(quiet) + 10.0, 1.5);
  SettleClock up{sidechainRmsDb(loud) + 10.0, 1.5, 0.0};
  play(kind, loud, [&](double now) { up.sample(now, reading(kind, kThresholdReading)); });
  EXPECT_GE(up.settledAt, 0.0);
  EXPECT_LT(up.settledAt, 6.0) << "settling time " << up.settledAt << " s";
  std::cout << "[ settle ] coloured floor rising 20 dB: settled after " << up.settledAt << " s\n";
}

TEST(AdaptiveGateEffectTest, MarginSetsTheDistanceAboveTheFloor) {
  for (const double margin : {3.0, 24.0}) {
    SCOPED_TRACE(margin);
    DirectKind kind(kKind, {{"margin", margin}});
    const std::vector<float> in = noiseAt(5.0, 3u, -65.0);
    play(kind, in);
    EXPECT_NEAR(reading(kind, kThresholdReading) - reading(kind, kFloorReading), margin, 0.01);
  }
}

namespace {

// A plucked note: a decaying 220 Hz tone with two harmonics, starting at
// `startDb` and falling `dbPerSecond`.
std::vector<float> pluck(double seconds, double startDb, double dbPerSecond, double rate = kRate) {
  std::vector<float> x(static_cast<size_t>(std::lround(seconds * rate)));
  for (size_t i = 0; i < x.size(); ++i) {
    const double t = static_cast<double>(i) / rate;
    const double env = std::pow(10.0, (startDb - dbPerSecond * t) / 20.0);
    const double phase = 2.0 * kPi * 220.0 * t;
    x[i] = static_cast<float>(env * (0.7 * std::sin(phase) + 0.2 * std::sin(2.0 * phase) +
                                     0.1 * std::sin(3.0 * phase)));
  }
  return x;
}

// Seconds the gate reading says open, over a run of `in`.
struct PluckRun {
  std::vector<float> in, out;
  double openSeconds{0.0};
  double firstOpenAt{-1.0};
  bool openAtEnd{false};
};

PluckRun runPluck(const Params& params, unsigned seed, double rate = kRate) {
  DirectKind kind(kKind, params);
  prepareAt(kind, rate);
  PluckRun run;
  const std::vector<float> floor = noiseAt(4.0, seed, -70.0, rate);
  const std::vector<float> note = pluck(2.0, -20.0, 40.0, rate);
  const std::vector<float> noiseTail = noiseAt(2.0, seed + 1u, -70.0, rate);
  std::vector<float> noteInNoise(note.size());
  for (size_t i = 0; i < note.size(); ++i)
    noteInNoise[i] = note[i] + noiseTail[i];
  run.in = concat(concat(floor, noteInNoise), noiseAt(2.0, seed + 2u, -70.0, rate));
  const double noteStart = 4.0;
  run.out = play(
      kind, run.in,
      [&](double now) {
        if (reading(kind, kOpenReading) > 0.5) {
          if (now >= noteStart) {
            run.openSeconds += kBlock / rate;
            if (run.firstOpenAt < 0.0)
              run.firstOpenAt = now - noteStart;
          }
        }
      },
      rate);
  run.openAtEnd = reading(kind, kOpenReading) > 0.5;
  return run;
}

}  // namespace

TEST(AdaptiveGateEffectTest, APluckedNoteOpensHoldsAndClosesWithoutClicks) {
  const PluckRun run = runPluck({{"holdMs", 300.0}}, 40u);
  const PluckRun quick = runPluck({{"holdMs", 0.0}}, 40u);

  // It opens on the pluck, within a few milliseconds of the note.
  EXPECT_GE(run.firstOpenAt, 0.0);
  EXPECT_LT(run.firstOpenAt, 0.02);
  // The note falls through the threshold; the hold keeps the gate open for
  // about 300 ms longer than without one.
  EXPECT_NEAR(run.openSeconds - quick.openSeconds, 0.3, 0.1);
  // ... and it has closed again by the end, in the noise.
  EXPECT_FALSE(run.openAtEnd);

  // Gain steps: out / in wherever the note is well above the noise.
  double worstStep = 0.0;
  float last = -1.0f;
  const size_t begin = static_cast<size_t>(3.5 * kRate);
  for (size_t i = begin; i < run.in.size(); ++i) {
    if (std::abs(run.in[i]) < 2.0e-3f) {
      last = -1.0f;
      continue;
    }
    const float g = run.out[i] / run.in[i];
    if (last >= 0.0f)
      worstStep = std::max(worstStep, static_cast<double>(std::abs(g - last)));
    last = g;
  }
  EXPECT_LT(worstStep, 0.03) << "the gain moved by " << worstStep << " in one sample";
  expectCleanSignal(run.out, 1.0f, "adaptive gate");
  std::cout << "[ pluck ] opened after " << run.firstOpenAt * 1000.0 << " ms, worst gain step " << worstStep
            << " per sample\n";

  // The note passes at unity at the start, and the noise is pulled down at
  // the end by the range.
  const size_t note0 = static_cast<size_t>(4.1 * kRate);
  EXPECT_NEAR(toDb(rmsOf(run.out, note0, note0 + 2400)) - toDb(rmsOf(run.in, note0, note0 + 2400)), 0.0,
              0.5);
  const size_t tail0 = static_cast<size_t>(7.5 * kRate);
  EXPECT_LT(toDb(rmsOf(run.out, tail0, run.out.size())) - toDb(rmsOf(run.in, tail0, run.in.size())),
            -40.0);
}

TEST(AdaptiveGateEffectTest, HoldAndReleaseAreTheSameAt44And96kHz) {
  for (const double rate : {44100.0, 96000.0}) {
    SCOPED_TRACE(rate);
    const PluckRun run = runPluck({{"holdMs", 300.0}}, 41u, rate);
    const PluckRun quick = runPluck({{"holdMs", 0.0}}, 41u, rate);
    EXPECT_GE(run.firstOpenAt, 0.0);
    EXPECT_LT(run.firstOpenAt, 0.02);
    EXPECT_NEAR(run.openSeconds - quick.openSeconds, 0.3, 0.1);
    EXPECT_FALSE(run.openAtEnd);
    // The release: 120 ms after the gate shuts, the noise is well down.
    const size_t tail0 = static_cast<size_t>(7.5 * rate);
    EXPECT_LT(toDb(rmsOf(run.out, tail0, run.out.size())) - toDb(rmsOf(run.in, tail0, run.in.size())),
              -40.0);
  }
}

namespace {

std::vector<float> atRmsDb(std::vector<float> x, double rmsDb) {
  const double now = rmsOf(x, 0, x.size());
  const float gain = static_cast<float>(std::pow(10.0, rmsDb / 20.0) / now);
  for (auto& v : x)
    v *= gain;
  return x;
}

// Phase of a note at `hz` with vibrato of `cents` at `rateHz`, sample `i`.
struct Vibrato {
  double hz, cents, rateHz, sampleRate;
  double phase{0.0};
  double next(size_t i) {
    const double t = static_cast<double>(i) / sampleRate;
    const double f = hz * std::pow(2.0, cents / 1200.0 * std::sin(2.0 * kPi * rateHz * t));
    phase += 2.0 * kPi * f / sampleRate;
    return phase;
  }
};

// A high-gain held note: a 110 Hz sine with slow vibrato, clipped hard.
std::vector<float> clippedNote(double seconds, double rate) {
  std::vector<float> x(static_cast<size_t>(std::lround(seconds * rate)));
  Vibrato v{110.0, 30.0, 4.5, rate};
  for (size_t i = 0; i < x.size(); ++i)
    x[i] = static_cast<float>(std::tanh(12.0 * std::sin(v.next(i))));
  return x;
}

// A saw at 196 Hz with slow vibrato.
std::vector<float> sawNote(double seconds, double rate) {
  std::vector<float> x(static_cast<size_t>(std::lround(seconds * rate)));
  Vibrato v{196.0, 25.0, 3.0, rate};
  for (size_t i = 0; i < x.size(); ++i) {
    const double cycles = v.next(i) / (2.0 * kPi);
    x[i] = static_cast<float>(2.0 * (cycles - std::floor(cycles)) - 1.0);
  }
  return x;
}

// An open G chord (six strings, equal tempered, each with harmonics falling
// as 1/k), held and compressed: each string wobbles a little in level, the
// sum stays steady. `drive` > 0 puts it through a hard clipper as well.
// `cents` > 0 adds a 5 Hz vibrato to every string.
std::vector<float> heldChord(double seconds, double rate, double drive, double cents = 0.0) {
  const double strings[] = {98.0, 123.47, 146.83, 196.0, 246.94, 392.0};
  double phases[6] = {};
  std::vector<float> x(static_cast<size_t>(std::lround(seconds * rate)));
  for (size_t i = 0; i < x.size(); ++i) {
    const double t = static_cast<double>(i) / rate;
    const double bend = std::pow(2.0, cents / 1200.0 * std::sin(2.0 * kPi * 5.0 * t));
    double sum = 0.0;
    for (int s = 0; s < 6; ++s) {
      phases[s] += 2.0 * kPi * strings[s] * bend / rate;
      const double level = 1.0 + 0.1 * std::sin(2.0 * kPi * (0.3 + 0.17 * s) * t + s);
      for (int k = 1; k <= 12 && strings[s] * k < 5000.0; ++k)
        sum += level / k * std::sin(k * phases[s] + 0.7 * s * k);
    }
    x[i] = static_cast<float>(drive > 0.0 ? std::tanh(drive * sum) : sum);
  }
  return x;
}

struct HeldRun {
  double closedSeconds{0.0};  // gate closed while the note sounds
  double floorRiseDb{0.0};    // the highest floor during the note, over the floor before
  double worstGainDb{0.0};    // the lowest out / in over 100 ms windows of the note
};

// Four seconds of -70 dBFS noise to learn the floor, then `note` (scaled to
// `levelDb` RMS) over the same noise.
HeldRun runHeld(const std::vector<float>& note, double levelDb, double rate) {
  DirectKind kind(kKind);
  prepareAt(kind, rate);
  play(kind, noiseAt(4.0, 70u, -70.0, rate), [](double) {}, rate);
  const double floorBefore = reading(kind, kFloorReading);
  std::vector<float> in = atRmsDb(note, levelDb);
  const std::vector<float> under = noiseAt(static_cast<double>(in.size()) / rate, 71u, -70.0, rate);
  for (size_t i = 0; i < in.size(); ++i)
    in[i] += under[i];
  HeldRun run;
  double highestFloor = floorBefore;
  const std::vector<float> out = play(
      kind, in,
      [&](double now) {
        if (now > 0.05 && reading(kind, kOpenReading) < 0.5)
          run.closedSeconds += kBlock / rate;
        highestFloor = std::max(highestFloor, reading(kind, kFloorReading));
      },
      rate);
  run.floorRiseDb = highestFloor - floorBefore;
  const size_t window = static_cast<size_t>(0.1 * rate);
  for (size_t from = window; from + window <= in.size(); from += window)
    run.worstGainDb =
        std::min(run.worstGainDb, toDb(rmsOf(out, from, from + window)) - toDb(rmsOf(in, from, from + window)));
  return run;
}

void expectHeldOpen(const HeldRun& run, const char* what, double rate) {
  EXPECT_EQ(run.closedSeconds, 0.0) << what << ": the gate closed for " << run.closedSeconds << " s";
  EXPECT_LT(run.floorRiseDb, 3.0) << what << ": the floor rose under the note";
  EXPECT_GT(run.worstGainDb, -0.5) << what << ": the note was turned down";
  std::cout << "[ held ] " << what << " at " << rate / 1000.0 << " kHz: closed " << run.closedSeconds
            << " s, floor rise " << run.floorRiseDb << " dB, worst gain " << run.worstGainDb << " dB\n";
}

}  // namespace

TEST(AdaptiveGateEffectTest, ASustainedHighGainNoteIsNeverLearnedAsNoise) {
  for (const double rate : {44100.0, 48000.0, 96000.0}) {
    SCOPED_TRACE(rate);
    expectHeldOpen(runHeld(clippedNote(10.0, rate), -18.0, rate), "clipped sine with vibrato", rate);
    expectHeldOpen(runHeld(sawNote(10.0, rate), -18.0, rate), "saw with vibrato", rate);
  }
}

TEST(AdaptiveGateEffectTest, ACompressedHeldChordIsNeverLearnedAsNoise) {
  for (const double rate : {44100.0, 48000.0, 96000.0}) {
    SCOPED_TRACE(rate);
    expectHeldOpen(runHeld(heldChord(10.0, rate, 0.0), -18.0, rate), "clean compressed chord", rate);
    expectHeldOpen(runHeld(heldChord(10.0, rate, 4.0), -18.0, rate), "distorted chord", rate);
    expectHeldOpen(runHeld(heldChord(10.0, rate, 4.0, 30.0), -18.0, rate), "distorted chord, vibrato", rate);
  }
  // Quieter, and a fuzz.
  expectHeldOpen(runHeld(heldChord(10.0, kRate, 30.0), -40.0, kRate), "fuzz chord at -40 dBFS", kRate);
}

TEST(AdaptiveGateEffectTest, SteadyFeedbackKeepsTheGateOpen) {
  for (const double hz : {196.0, 659.3, 1318.5}) {
    SCOPED_TRACE(hz);
    expectHeldOpen(runHeld(sine(8.0, hz, 1.0f, kRate), -20.0, kRate), "feedback sine", kRate);
  }
  // Quiet enough that a floor learned from it would shut the gate.
  expectHeldOpen(runHeld(sine(8.0, 659.3, 1.0f, kRate), -40.0, kRate), "feedback sine at -40 dBFS", kRate);
  // A single note with a wide vibrato (a semitone at 6 Hz): its spectrum
  // moves, its period does not go away.
  std::vector<float> wide(static_cast<size_t>(8.0 * kRate));
  Vibrato v{220.0, 100.0, 6.0, kRate};
  for (size_t i = 0; i < wide.size(); ++i)
    wide[i] = static_cast<float>(std::tanh(5.0 * std::sin(v.next(i))));
  expectHeldOpen(runHeld(wide, -40.0, kRate), "wide vibrato note at -40 dBFS", kRate);
}

TEST(AdaptiveGateEffectTest, HumUnderTheThresholdDoesNotOpenTheGateButAToneAtTheSameLevelDoes) {
  // Tracking off and nothing learned: the manual threshold is -40 dB. A
  // sine at -34 dB peak is -37 dB RMS, over the threshold; at 60 Hz the
  // sidechain's high pass takes about 9 dB off it, at 10 kHz the low pass
  // takes 12 dB off, and both fall under.
  const Params params = {{"track", 0.0}, {"thresholdDb", -40.0}, {"holdMs", 0.0}, {"rangeDb", -60.0}};
  const auto openAfter = [&](double freq) {
    DirectKind kind(kKind, params);
    play(kind, sine(1.0, freq, amp(-34.0), kRate));
    return reading(kind, kOpenReading) > 0.5;
  };
  EXPECT_FALSE(openAfter(60.0)) << "60 Hz hum held the gate open";
  EXPECT_FALSE(openAfter(10000.0)) << "10 kHz fizz held the gate open";
  EXPECT_TRUE(openAfter(1000.0)) << "a tone at the same level should open it";
}

TEST(AdaptiveGateEffectTest, TrackOffFreezesTheLearnedFloor) {
  DirectKind kind(kKind);
  const std::vector<float> quiet = noiseAt(5.0, 8u, -70.0);
  play(kind, quiet);
  const double floorBefore = reading(kind, kFloorReading);
  EXPECT_NEAR(floorBefore, sidechainRmsDb(quiet), 1.5);

  setParam(kind, "track", 0.0);
  play(kind, noiseAt(8.0, 9u, -50.0));
  EXPECT_NEAR(reading(kind, kFloorReading), floorBefore, 0.001);
  // The threshold is still floor + margin from the frozen value.
  EXPECT_NEAR(reading(kind, kThresholdReading), floorBefore + 10.0, 0.001);
  setParam(kind, "margin", 16.0);
  play(kind, noiseAt(0.5, 10u, -50.0));
  EXPECT_NEAR(reading(kind, kThresholdReading), floorBefore + 16.0, 0.001);

  // Tracking on again follows the noise.
  setParam(kind, "track", 1.0);
  play(kind, noiseAt(10.0, 11u, -50.0));
  EXPECT_GT(reading(kind, kFloorReading), floorBefore + 10.0);
}

TEST(AdaptiveGateEffectTest, TheManualThresholdIsUsedUntilAFloorIsLearned) {
  {
    DirectKind kind(kKind, {{"track", 0.0}, {"thresholdDb", -42.0}});
    play(kind, noiseAt(3.0, 12u, -60.0));
    EXPECT_NEAR(reading(kind, kThresholdReading), -42.0, 0.001);
  }
  {
    DirectKind kind(kKind, {{"thresholdDb", -42.0}});
    play(kind, noiseAt(0.2, 12u, -60.0));
    EXPECT_NEAR(reading(kind, kThresholdReading), -42.0, 0.001) << "no floor after 0.2 s";
    play(kind, noiseAt(3.0, 13u, -60.0));
    EXPECT_LT(reading(kind, kThresholdReading), -42.0 - 5.0);
  }
}

TEST(AdaptiveGateEffectTest, RangeLimitsTheAttenuation) {
  for (const double range : {-12.0, -30.0, -60.0}) {
    SCOPED_TRACE(range);
    DirectKind kind(kKind, {{"track", 0.0}, {"thresholdDb", -30.0}, {"rangeDb", range}});
    const std::vector<float> in = sine(2.0, 1000.0, amp(-50.0), kRate);
    const std::vector<float> out = play(kind, in);
    const size_t from = static_cast<size_t>(1.5 * kRate);
    EXPECT_NEAR(toDb(rmsOf(out, from, out.size())) - toDb(rmsOf(in, from, in.size())), range, 0.5);
  }
}

TEST(AdaptiveGateEffectTest, SilenceInGivesSilenceOut) {
  DirectKind kind(kKind);
  const std::vector<float> out = play(kind, std::vector<float>(static_cast<size_t>(2 * kRate), 0.0f));
  for (const float v : out)
    ASSERT_EQ(v, 0.0f);
  EXPECT_LE(reading(kind, kFloorReading), -99.0);
  EXPECT_EQ(reading(kind, kOpenReading), 0.0);
}

TEST(AdaptiveGateEffectTest, ReportsNoTailAndNoLatencyAndThreeReadings) {
  EffectRig rig(kKind);
  EXPECT_EQ(rig.tailSeconds(), 0.0);
  EXPECT_EQ(rig.latencySamples(), 0);
  DirectKind kind(kKind);
  ASSERT_EQ(kind.engine().readingTable().size(), 3u);
}

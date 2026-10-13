// amp.highgain: our high gain tube amp model, a composition of the amp
// components (plugin/include/amp). The conformance test of the framework, then
// what this amp is for: distortion that rises with gain and a compressed
// level, the tone stack's mid dip and the reach of Bass, Middle and Treble,
// Presence and Depth through the feedback, the bright cap, the voicings, the
// supply sag, a low alias level, reset, and stability at extreme settings.
#include "effects/effect_test_helpers.h"

#include <gtest/gtest.h>

using namespace fxtest;

namespace {

constexpr double kRate = 48000.0;
constexpr const char* kId = "amp.highgain";

float level(double dbfs) { return static_cast<float>(std::pow(10.0, dbfs / 20.0)); }

// A kind run on its own on one sine, `seconds` long.
std::vector<float> run(const Params& params, double hz, double dbfs, double seconds = 1.0) {
  DirectKind kind(kId, params);
  return kind.render(sine(seconds, hz, level(dbfs), kRate));
}

// Level in dB of the component at `hz`, in the second half of the render.
double toneDb(const std::vector<float>& out, double hz) {
  const size_t from = out.size() / 2;
  return db(goertzelPower(out.data() + from, out.size() - from, hz, kRate));
}

// Gain in dB at `hz` for a small sine (a linear measurement of the stack).
double smallSignalDb(const Params& params, double hz, double dbfs = -60.0) {
  return toneDb(run(params, hz, dbfs, 1.0), hz) - dbfs;
}

double thdAt(const Params& params, double dbfs) {
  const std::vector<float> out = run(params, 1000.0, dbfs);
  return thdPercent(out, 1000.0, kRate, static_cast<size_t>(0.3 * kRate), 10);
}

Params with(Params base, const char* id, double value) {
  base.emplace_back(id, value);
  return base;
}

}  // namespace

// ---------------------------------------------------------------------------
// Conformance
// ---------------------------------------------------------------------------

TEST(HighGainAmpTest, ConformsInMonoStereoAndDualMono) {
  ConformanceOptions options;
  options.params = {{"gain", 70.0}, {"bass", 60.0}, {"middle", 40.0}, {"treble", 65.0},
                    {"presence", 55.0}, {"depth", 45.0}, {"master", 60.0}, {"bright", 1.0}};
  options.stateParams = {{"gain", 20.0}, {"bass", 30.0}, {"middle", 80.0}, {"treble", 25.0},
                         {"presence", 10.0}, {"depth", 90.0}, {"master", 35.0}, {"levelDb", -6.0},
                         {"voicing", 2.0}};
  options.sweepBound = 12.0f;
  options.cpuAverageBudgetNs = 80000.0;
  options.cpuWorstBudgetNs = 600000.0;
  expectKindConforms("amp.highgain", options);
}

TEST(HighGainAmpTest, IsAnAmpWithTheDocumentedParameters) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kId);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->category, BlockCategory::Amp);
  struct Row {
    const char* id;
    double lo, hi, def;
  };
  const Row rows[] = {{"gain", 0, 100, 50},    {"bass", 0, 100, 50},   {"middle", 0, 100, 50},
                      {"treble", 0, 100, 50},  {"presence", 0, 100, 50}, {"depth", 0, 100, 50},
                      {"master", 0, 100, 50},  {"levelDb", -48, 18, 0},  {"bright", 0, 1, 0},
                      {"voicing", 0, 2, 1}};
  ASSERT_EQ(info->params.size(), std::size(rows));
  for (size_t i = 0; i < std::size(rows); ++i) {
    EXPECT_EQ(info->params[i].id, juce::String(rows[i].id));
    EXPECT_DOUBLE_EQ(info->params[i].min, rows[i].lo) << rows[i].id;
    EXPECT_DOUBLE_EQ(info->params[i].max, rows[i].hi) << rows[i].id;
    EXPECT_DOUBLE_EQ(info->params[i].defaultValue, rows[i].def) << rows[i].id;
  }
  EXPECT_EQ(info->params[9].choices.joinIntoString(","), juce::String("Tight,Classic,Loose"));
  ASSERT_EQ(info->readings.size(), 1u);
  EXPECT_EQ(info->readings[0].id, juce::String("sag"));
}

TEST(HighGainAmpTest, ReportsTheLatencyOfTheOversampler) {
  DirectKind kind(kId);
  EXPECT_EQ(kind.latencySamples(), 3);
}

// ---------------------------------------------------------------------------
// Gain
// ---------------------------------------------------------------------------

TEST(HighGainAmpTest, DistortionRisesWithGain) {
  for (const double dbfs : {-30.0, -18.0}) {
    double last = -1.0;
    for (const double gain : {0.0, 25.0, 50.0, 75.0, 100.0}) {
      const double thd = thdAt({{"gain", gain}}, dbfs);
      std::cout << "[   THD    ] 1 kHz at " << dbfs << " dBFS, gain " << gain << ": "
                << juce::String(thd, 2) << " %" << std::endl;
      if (gain > 0.0)
        EXPECT_GT(thd, last) << "gain " << gain << " at " << dbfs << " dBFS";
      last = thd;
    }
  }
  // Heavy rhythm saturation at the defaults, a crunchy edge at gain 0.
  EXPECT_GT(thdAt({}, -18.0), 20.0);
  EXPECT_GT(thdAt({{"gain", 0.0}}, -18.0), 3.0);
  EXPECT_LT(thdAt({{"gain", 0.0}}, -18.0), thdAt({}, -18.0));
}

TEST(HighGainAmpTest, TheOutputLevelIsCompressed) {
  const double quiet = toneDb(run({}, 220.0, -30.0), 220.0);
  const double loud = toneDb(run({}, 220.0, -10.0), 220.0);
  std::cout << "[  LEVEL   ] +20 dB in gives " << juce::String(loud - quiet, 2) << " dB more out"
            << std::endl;
  EXPECT_LT(loud - quiet, 8.0);
  EXPECT_GT(loud - quiet, 0.0);
}

TEST(HighGainAmpTest, LevelAtTheDefaultsSitsWithTheOtherBlocks) {
  // A guitar-like 220 Hz sine at -18 dBFS peak: the output is near -18 dBFS RMS.
  const std::vector<float> out = run({}, 220.0, -18.0);
  const double rms = toDb(rmsOf(out, static_cast<size_t>(0.3 * kRate)));
  std::cout << "[  LEVEL   ] defaults: " << juce::String(rms, 2) << " dBFS RMS" << std::endl;
  EXPECT_NEAR(rms, -18.0, 3.0);
}

// ---------------------------------------------------------------------------
// Tone stack
// ---------------------------------------------------------------------------

namespace {

// Small signal at gain 0, so the stack dominates the response.
const Params kStackBase = {{"gain", 0.0}, {"master", 50.0}};

}  // namespace

TEST(HighGainAmpTest, ToneStackShowsTheMidDipAtTheMiddleSettings) {
  std::cout << "[  STACK   ]";
  double response[8];
  const double freqs[] = {80.0, 150.0, 300.0, 500.0, 800.0, 1500.0, 3000.0, 5000.0};
  for (int i = 0; i < 8; ++i) {
    response[i] = smallSignalDb(kStackBase, freqs[i]);
    std::cout << " " << freqs[i] << ":" << juce::String(response[i], 1);
  }
  std::cout << std::endl;
  double dip = response[2];
  for (int i = 2; i <= 4; ++i)
    dip = std::min(dip, response[i]);
  EXPECT_LT(dip, std::max(response[1], response[0]) - 3.0);
  EXPECT_LT(dip, response[6] - 3.0);
}

TEST(HighGainAmpTest, BassMiddleAndTrebleMoveTheirBands) {
  const auto move = [&](const char* knob, double hz) {
    const double more = smallSignalDb(with(kStackBase, knob, 100.0), hz);
    const double less = smallSignalDb(with(kStackBase, knob, 0.0), hz);
    std::cout << "[  STACK   ] " << knob << " 0 -> 100 at " << hz << " Hz: " << juce::String(more - less, 1)
              << " dB" << std::endl;
    return more - less;
  };
  EXPECT_GT(move("bass", 80.0), 6.0);
  EXPECT_GT(move("middle", 600.0), 6.0);
  EXPECT_GT(move("treble", 4000.0), 6.0);
  // Each knob does its own band far more than the opposite end.
  EXPECT_GT(move("bass", 80.0), move("bass", 4000.0) + 6.0);
  EXPECT_GT(move("treble", 4000.0), move("treble", 80.0) + 6.0);
}

// ---------------------------------------------------------------------------
// Power amp: Presence and Depth
// ---------------------------------------------------------------------------

TEST(HighGainAmpTest, PresenceLiftsTheUpperMidsAndHighs) {
  const Params base = {{"gain", 0.0}, {"depth", 0.0}};
  for (const double hz : {3500.0, 5000.0}) {
    const double lift = smallSignalDb(with(base, "presence", 100.0), hz) -
                        smallSignalDb(with(base, "presence", 0.0), hz);
    std::cout << "[ POWER    ] presence 0 -> 100 at " << hz << " Hz: " << juce::String(lift, 1) << " dB"
              << std::endl;
    EXPECT_GT(lift, 3.0) << hz;
  }
}

TEST(HighGainAmpTest, DepthLiftsTheLowResonance) {
  const Params base = {{"gain", 0.0}, {"presence", 0.0}};
  const double lift = smallSignalDb(with(base, "depth", 100.0), 100.0) -
                      smallSignalDb(with(base, "depth", 0.0), 100.0);
  std::cout << "[ POWER    ] depth 0 -> 100 at 100 Hz: " << juce::String(lift, 1) << " dB" << std::endl;
  EXPECT_GT(lift, 3.0);
  const double highs = smallSignalDb(with(base, "depth", 100.0), 3000.0) -
                       smallSignalDb(with(base, "depth", 0.0), 3000.0);
  EXPECT_LT(std::abs(highs), 1.5);
}

// ---------------------------------------------------------------------------
// Bright
// ---------------------------------------------------------------------------

TEST(HighGainAmpTest, BrightAddsHighsAtLowGainAndLessAtHighGain) {
  const auto added = [&](double gain) {
    const Params off = {{"gain", gain}}, on = {{"gain", gain}, {"bright", 1.0}};
    // A small 5 kHz tone against a small 500 Hz one, so the preamp is
    // linear enough to read the cap's lift.
    const double highs = smallSignalDb(on, 5000.0, -60.0) - smallSignalDb(off, 5000.0, -60.0);
    const double mids = smallSignalDb(on, 500.0, -60.0) - smallSignalDb(off, 500.0, -60.0);
    std::cout << "[  BRIGHT  ] gain " << gain << ": +" << juce::String(highs - mids, 1) << " dB highs" << std::endl;
    return highs - mids;
  };
  const double atLow = added(10.0), atMid = added(50.0), atHigh = added(95.0);
  EXPECT_GT(atLow, 4.0);
  EXPECT_GT(atLow, atMid);
  EXPECT_GT(atMid, atHigh);
  EXPECT_LT(atHigh, atLow - 3.0);
}

// ---------------------------------------------------------------------------
// Voicings
// ---------------------------------------------------------------------------

TEST(HighGainAmpTest, VoicingsDifferInLowEndTightness) {
  // Two tones, 60 Hz and 500 Hz, driven hard: the share of 60 Hz in the
  // output falls as the voicing gets tighter.
  const int frames = static_cast<int>(kRate);
  std::vector<float> in(static_cast<size_t>(frames));
  for (int i = 0; i < frames; ++i)
    in[static_cast<size_t>(i)] = 0.03f * static_cast<float>(std::sin(2.0 * kPi * 60.0 * i / kRate) +
                                                           std::sin(2.0 * kPi * 500.0 * i / kRate));
  double ratio[3];
  for (int v = 0; v < 3; ++v) {
    DirectKind kind(kId, {{"gain", 70.0}, {"voicing", static_cast<double>(v)}});
    const std::vector<float> out = kind.render(in);
    ratio[v] = toneDb(out, 60.0) - toneDb(out, 500.0);
    std::cout << "[ VOICING  ] " << v << ": 60 Hz relative to 500 Hz " << juce::String(ratio[v], 1) << " dB"
              << std::endl;
  }
  EXPECT_LT(ratio[0], ratio[1] - 2.0);
  EXPECT_LT(ratio[1], ratio[2] - 2.0);
}

// ---------------------------------------------------------------------------
// Sag
// ---------------------------------------------------------------------------

TEST(HighGainAmpTest, ALoudBurstCompressesMoreAfterFiftyMilliseconds) {
  const Params loud = {{"gain", 60.0}, {"master", 100.0}, {"voicing", 2.0}};
  DirectKind kind(kId, loud);
  const std::vector<float> burst = sine(0.4, 196.0, level(-12.0), kRate);
  const std::vector<float> out = kind.render(burst);
  const auto rmsMs = [&](double from, double to) {
    return rmsOf(out, static_cast<size_t>(from * 1e-3 * kRate), static_cast<size_t>(to * 1e-3 * kRate));
  };
  // The first 10 ms (after the oversampler's few samples) against 60 to 70 ms.
  const double early = toDb(rmsMs(2.0, 12.0)), later = toDb(rmsMs(60.0, 70.0));
  std::cout << "[   SAG    ] first 10 ms " << juce::String(early, 2) << " dBFS, at 60 ms "
            << juce::String(later, 2) << " dBFS, reading " << kind.engine().getReading(0) << std::endl;
  EXPECT_LT(later, early - 0.5);
  EXPECT_GT(kind.engine().getReading(0), 0.1f);
  // And the supply recovers afterwards.
  kind.render(std::vector<float>(static_cast<size_t>(0.6 * kRate), 0.0f));
  EXPECT_LT(kind.engine().getReading(0), 0.02f);
}

TEST(HighGainAmpTest, LooseSagsMoreThanTight) {
  const auto droop = [&](double voicing) {
    DirectKind kind(kId, {{"gain", 60.0}, {"master", 100.0}, {"voicing", voicing}});
    kind.render(sine(0.3, 196.0, level(-12.0), kRate));
    return kind.engine().getReading(0);
  };
  EXPECT_GT(droop(2.0), droop(0.0));
}

// ---------------------------------------------------------------------------
// Aliasing
// ---------------------------------------------------------------------------

namespace {

// The strongest component of a rendered sine that is not at a multiple of its
// frequency, in dB relative to the fundamental: aliasing (and nothing else,
// the circuit being periodic once settled).
double strongestAliasDb(const Params& params, double fundamentalHz, double dbfs, double* atHz = nullptr) {
  const int n = 48000;
  DirectKind kind(kId, params);
  const std::vector<float> out = kind.render(sine(2.0, fundamentalHz, level(dbfs), kRate));
  const std::vector<float> tail(out.end() - n, out.end());

  const int order = 16;
  const int size = 1 << order;
  std::vector<float> data(static_cast<size_t>(2 * size), 0.0f);
  for (int i = 0; i < n; ++i) {
    const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (n - 1));
    data[static_cast<size_t>(i)] = tail[static_cast<size_t>(i)] * static_cast<float>(w);
  }
  juce::dsp::FFT fft(order);
  fft.performFrequencyOnlyForwardTransform(data.data());
  const double binHz = kRate / size;
  double fundamental = 0.0, worst = 0.0, worstHz = 0.0;
  for (int bin = 1; bin < size / 2; ++bin) {
    const double hz = bin * binHz;
    const double magnitude = data[static_cast<size_t>(bin)];
    const double harmonic = hz / fundamentalHz;
    const double distance = std::abs(harmonic - std::round(harmonic)) * fundamentalHz;
    if (std::abs(hz - fundamentalHz) < 100.0)
      fundamental = std::max(fundamental, magnitude);
    else if (distance > 100.0 && hz > 150.0 && std::round(harmonic) >= 1.0 && magnitude > worst) {
      worst = magnitude;
      worstHz = hz;
    }
  }
  if (atHz != nullptr)
    *atHz = worstHz;
  return toDb(worst) - toDb(fundamental);
}

}  // namespace

TEST(HighGainAmpTest, AHighToneAtFullGainDoesNotAlias) {
  // A 5 kHz sine has its harmonics at multiples of 5 kHz. Anything else in
  // the spectrum below Nyquist is an alias; the strongest must be at least
  // 40 dB under the fundamental, at full gain, full master and bright on.
  double hz = 0.0;
  const double aliasDb = strongestAliasDb({{"gain", 100.0}, {"master", 100.0}, {"bright", 1.0}}, 5000.0, -6.0, &hz);
  std::cout << "[  ALIAS   ] strongest non-harmonic component " << juce::String(aliasDb, 1) << " dB at "
            << juce::String(hz, 0) << " Hz" << std::endl;
  EXPECT_LT(aliasDb, -40.0);
  // And at lower drive, where the power stage is the one clipping.
  EXPECT_LT(strongestAliasDb({{"gain", 100.0}, {"master", 5.0}}, 5000.0, -6.0), -40.0);
  EXPECT_LT(strongestAliasDb({{"gain", 60.0}, {"master", 100.0}}, 3700.0, -12.0), -40.0);
}

// ---------------------------------------------------------------------------
// Reset, silence, extremes
// ---------------------------------------------------------------------------

TEST(HighGainAmpTest, ResetRestoresTheFirstRenderExactly) {
  const std::vector<float> in = musicalSignal(0.6, kRate, 0.3f);
  for (const Params& params : {Params{}, Params{{"gain", 100.0}, {"master", 100.0}, {"voicing", 2.0}, {"bright", 1.0}}}) {
    DirectKind kind(kId, params, Layout::Stereo);
    const Stereo first = kind.render(Stereo{in, in});
    kind.engine().reset();
    const Stereo second = kind.render(Stereo{in, in});
    EXPECT_EQ(std::memcmp(first.left.data(), second.left.data(), in.size() * sizeof(float)), 0);
    EXPECT_EQ(std::memcmp(first.right.data(), second.right.data(), in.size() * sizeof(float)), 0);
    // After a reset, silence in is silence out.
    kind.engine().reset();
    const std::vector<float> silent = kind.render(std::vector<float>(static_cast<size_t>(0.3 * kRate), 0.0f));
    EXPECT_EQ(peakOf(silent), 0.0f);
    EXPECT_EQ(kind.engine().getReading(0), 0.0f);
  }
}

TEST(HighGainAmpTest, NoNaNOrDenormalsAtExtremeSettings) {
  const std::vector<float> silence(static_cast<size_t>(0.25 * kRate), 0.0f);
  const std::vector<float> loudNoise = makeNoise(static_cast<int>(0.5 * kRate), 5u, 1.0f);
  const std::vector<float> fullSine = sine(0.5, 1000.0, 1.0f, kRate);
  const std::vector<float> highSine = sine(0.5, 7000.0, 1.0f, kRate);
  const std::vector<float> decay = [] {
    std::vector<float> x = sine(0.3, 150.0, 1.0f, kRate);
    x.resize(x.size() + static_cast<size_t>(2.0 * kRate), 0.0f);
    return x;
  }();
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kId);
  ASSERT_NE(info, nullptr);
  for (int pattern = 0; pattern < 4; ++pattern) {
    Params params;
    int index = 0;
    for (const BlockParamSpec& spec : info->params) {
      const bool high = pattern == 0 ? false : pattern == 1 ? true : pattern == 2 ? (index % 2 == 0) : (index % 2 == 1);
      params.emplace_back(spec.id, static_cast<double>(high ? spec.max : spec.min));
      ++index;
    }
    for (const Layout layout : {Layout::Mono, Layout::Stereo}) {
      SCOPED_TRACE("pattern " + std::to_string(pattern));
      DirectKind kind(kId, params, layout);
      for (const std::vector<float>* signal : {&silence, &loudNoise, &fullSine, &highSine, &decay}) {
        kind.engine().reset();
        const Stereo out = kind.render(Stereo{*signal, *signal});
        expectCleanSignal(out.left, 40.0f, "left");
        expectCleanSignal(out.right, 40.0f, "right");
      }
    }
  }
}

TEST(HighGainAmpTest, StereoChannelsKeepTheirOwnCircuitState) {
  // Different signals on the two channels: each output is what a mono render
  // of that channel gives.
  const std::vector<float> a = musicalSignal(0.8, kRate, 0.3f), b = makeNoise(static_cast<int>(0.8 * kRate), 3u, 0.2f);
  DirectKind stereo(kId, {{"gain", 80.0}}, Layout::Stereo);
  const Stereo out = stereo.render(Stereo{a, b});
  DirectKind monoA(kId, {{"gain", 80.0}}), monoB(kId, {{"gain", 80.0}});
  EXPECT_EQ(out.left, monoA.render(a));
  EXPECT_EQ(out.right, monoB.render(b));
}

TEST(HighGainAmpTest, AStereoPairThatStartsIdenticalAndThenDiffersStaysExact) {
  // The two channels carry the same audio at first (one circuit serves both),
  // then diverge in the middle of a block: each output must still be what a
  // lane of its own would have made of its channel from the start.
  const size_t total = static_cast<size_t>(0.8 * kRate);
  const size_t diverge = 7000;  // not a multiple of the 256 sample block
  const std::vector<float> left = musicalSignal(0.8, kRate, 0.3f);
  std::vector<float> right = left;
  for (size_t i = diverge; i < total; ++i)
    right[i] = 0.5f * left[i] + 0.1f * static_cast<float>(std::sin(0.3 * static_cast<double>(i)));
  DirectKind pair(kId, {{"gain", 80.0}, {"bright", 1.0}}, Layout::Stereo);
  const Stereo out = pair.render(Stereo{left, right});
  DirectKind monoLeft(kId, {{"gain", 80.0}, {"bright", 1.0}}), monoRight(kId, {{"gain", 80.0}, {"bright", 1.0}});
  EXPECT_EQ(out.left, monoLeft.render(left));
  EXPECT_EQ(out.right, monoRight.render(right));
}

TEST(HighGainAmpTest, CostPerBlock) {
  const KindCost cost = measureKindCost(kId, {{"gain", 70.0}, {"bright", 1.0}});
  std::cout << "[   CPU    ] amp.highgain, same audio on both channels (one circuit runs): "
            << static_cast<long long>(cost.averageNs) << " ns per 128-sample block at 48 kHz ("
            << juce::String(100.0 * cost.averageNs / 2666667.0, 2) << "% of a core)" << std::endl;

  // Two different channels (dual mono): two circuits run.
  DirectKind kind(kId, {{"gain", 70.0}, {"bright", 1.0}}, Layout::DualMono, 128);
  const std::vector<float> a = makeNoise(128 * 64, 7u, 0.25f), b = makeNoise(128 * 64, 8u, 0.25f);
  kind.render(Stereo{a, b});  // warm up
  double best = 1.0e18;
  for (int pass = 0; pass < 3; ++pass) {
    const auto begin = std::chrono::steady_clock::now();
    for (int rep = 0; rep < 20; ++rep)
      kind.render(Stereo{a, b});
    const auto end = std::chrono::steady_clock::now();
    best = std::min(best, std::chrono::duration<double, std::nano>(end - begin).count() / (20.0 * 64.0));
  }
  std::cout << "[   CPU    ] amp.highgain, two different channels (two circuits run): "
            << static_cast<long long>(best) << " ns per 128-sample block at 48 kHz ("
            << juce::String(100.0 * best / 2666667.0, 2) << "% of a core)" << std::endl;
}

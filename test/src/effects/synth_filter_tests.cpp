// filter.synth (plugin/src/effects/filter/SynthFilterEffect.cpp): the
// conformance test, then what a ladder filter is for: a 24 dB per octave low
// pass whose resonance peak grows until it sings, the other three modes, a
// signal that stays bounded at full resonance and full drive, and a cutoff
// that follows how hard you play. Then the quality of the oversampled ladder:
// no aliasing when driven, a resonance and a self-oscillation that sit on
// the cutoff up to the top of the range, a level that holds as Drive turns,
// the reported latency and the cost.
#include "effects/effect_test_helpers.h"

#include <gtest/gtest.h>

using namespace fxtest;

namespace {

constexpr const char* kKind = "filter.synth";
constexpr double kRate = 48000.0;
// Per 128-sample stereo block at 48 kHz (see ConformanceOptions). The 4x
// ladder is one long chain of dependent steps per sample (three tanh in the
// loop), so it costs about twice an oversampled drive: some 55 us on a
// 2.1 GHz Xeon where the drive kinds take 20 to 30 us, against their 50 us
// budget. Priced with the same headroom as the heavier kinds.
constexpr double kCpuAverageBudgetNs = 75000.0;
constexpr double kCpuWorstBudgetNs = 500000.0;

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

// Everything in a rendered sine's spectrum that is not a harmonic of it, in
// dB against the fundamental (energy, not a single peak). The sine sits on an
// FFT bin and a Blackman-Harris window keeps its leakage under -90 dB, so
// once the filter has settled whatever is left between the harmonics is
// aliasing: tanh stages run on a periodic signal make only harmonics.
constexpr int kFftOrder = 14;
constexpr int kFftSize = 1 << kFftOrder;

double nonHarmonicDb(const std::vector<float>& out, int fundamentalBin) {
  std::vector<float> data(static_cast<size_t>(2 * kFftSize), 0.0f);
  const size_t from = out.size() - static_cast<size_t>(kFftSize);
  for (int i = 0; i < kFftSize; ++i) {
    const double phase = 2.0 * kPi * i / kFftSize;
    const double w = 0.35875 - 0.48829 * std::cos(phase) + 0.14128 * std::cos(2.0 * phase) -
                     0.01168 * std::cos(3.0 * phase);
    data[static_cast<size_t>(i)] = out[from + static_cast<size_t>(i)] * static_cast<float>(w);
  }
  juce::dsp::FFT fft(kFftOrder);
  fft.performFrequencyOnlyForwardTransform(data.data());
  constexpr int kLobe = 4;  // the window's main lobe, in bins each side
  double fundamental = 0.0, other = 0.0;
  for (int bin = 8; bin < kFftSize / 2; ++bin) {
    const double power = static_cast<double>(data[static_cast<size_t>(bin)]) *
                         static_cast<double>(data[static_cast<size_t>(bin)]);
    const int harmonic = (bin + fundamentalBin / 2) / fundamentalBin;
    if (std::abs(bin - fundamentalBin) <= kLobe)
      fundamental += power;
    else if (std::abs(bin - harmonic * fundamentalBin) > kLobe)
      other += power;
  }
  return 10.0 * std::log10(std::max(other, 1.0e-30) / std::max(fundamental, 1.0e-30));
}

// Frequency of a steady oscillation from its upward zero crossings
// (interpolated between samples), from `from` to the end.
double zeroCrossingHz(const std::vector<float>& x, size_t from, double fs) {
  double first = -1.0, last = -1.0;
  int crossings = 0;
  for (size_t i = std::max<size_t>(from, 1); i < x.size(); ++i) {
    if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
      const double at = static_cast<double>(i - 1) +
                        static_cast<double>(x[i - 1]) / static_cast<double>(x[i - 1] - x[i]);
      if (crossings++ == 0)
        first = at;
      last = at;
    }
  }
  return crossings > 1 ? (crossings - 1) * fs / (last - first) : 0.0;
}

}  // namespace

TEST(SynthFilterEffectTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"cutoffHz", 800.0}, {"resonancePct", 70.0}, {"driveDb", 9.0},
                    {"envAmountPct", 60.0}};
  options.stateParams = {{"cutoffHz", 2500.0}, {"resonancePct", 45.0}, {"driveDb", 12.0},
                         {"envAmountPct", -50.0}, {"envAttackMs", 30.0}, {"envReleaseMs", 500.0},
                         {"mode", 2.0}};
  options.cpuAverageBudgetNs = kCpuAverageBudgetNs;
  options.cpuWorstBudgetNs = kCpuWorstBudgetNs;
  options.sweepBound = 20.0f;
  // A four-pole low pass rises slowly from the impulse, so its 1 % onset
  // comes well after the oversampler's delay: that is the filter, not
  // latency. ReportsTheOversamplerLatency checks the reported value exactly
  // in the mode whose output contains the input.
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
  EXPECT_EQ(high.latencySamples(), 3) << "the 4x oversampler's half-band filters";
}

TEST(SynthFilterEffectTest, ReportsTheOversamplerLatency) {
  // High pass passes the input straight through the ladder, so its onset is
  // the delay of the oversampler alone, which the kind must report.
  expectLatencyMatches(kKind, {{"cutoffHz", 200.0}, {"resonancePct", 0.0}, {"mode", 3.0}},
                       LatencyCheck::MatchesOnset);
}

TEST(SynthFilterEffectTest, DrivenHighToneDoesNotAlias) {
  // A hot guitar note (0.25 peak, the level the conformance noise uses) at
  // full drive and moderate resonance, the cutoff high so the harmonics are
  // not filtered away: tanh at 16x gain is close to a square wave, whose
  // harmonics reach far past the oversampled Nyquist. Between 3 and 7 kHz
  // the harmonics that fold back into the audio band are the 25th and up;
  // everything that is not a harmonic must stay 60 dB under the
  // fundamental. At the chain rate (the first version) it was about -11 dB.
  double worst = -300.0, worstHz = 0.0;
  for (const double mode : {0.0, 1.0}) {
    for (const double cutoffHz : {20000.0, 8000.0}) {
      for (const int bin : {1024, 1365, 1707, 2048, 2389}) {  // 3, 4, 5, 6, 7 kHz
        const double hz = bin * kRate / kFftSize;
        SCOPED_TRACE(juce::String(hz, 0) + " Hz, cutoff " + juce::String(cutoffHz, 0) + ", mode " +
                     juce::String(mode, 0));
        DirectKind kind(kKind, {{"cutoffHz", cutoffHz}, {"resonancePct", 50.0}, {"driveDb", 24.0},
                                {"mode", mode}});
        const std::vector<float> out = kind.render(sine(0.5 + kFftSize / kRate, hz, 0.25f, kRate));
        const double db = nonHarmonicDb(out, bin);
        EXPECT_LT(db, -60.0);
        if (db > worst) {
          worst = db;
          worstHz = hz;
        }
      }
    }
  }
  std::cout << "[  ALIAS   ] filter.synth: non-harmonic energy at most " << juce::String(worst, 1)
            << " dB against the fundamental (at " << juce::String(worstHz, 0) << " Hz)" << std::endl;
}

TEST(SynthFilterEffectTest, SelfOscillatesAtTheCutoffAtEveryHostRate) {
  // At 100 % the ladder sings a sine at the cutoff on its own: within 0.5 % of
  // it from the bass to the top of the range, at the same bounded level
  // everywhere, and clean (THD under 1 %).
  double worstError = 0.0, worstThd = 0.0, spread = 0.0;
  for (const double hostRate : {44100.0, 48000.0, 96000.0}) {
    RigOptions options;
    options.sampleRate = hostRate;
    double quietest = 1.0e9, loudest = 0.0;
    for (const double cutoffHz : {100.0, 1000.0, 5000.0, 15000.0}) {
      SCOPED_TRACE(juce::String(cutoffHz, 0) + " Hz at " + juce::String(hostRate, 0));
      EffectRig rig(kKind, {{"cutoffHz", cutoffHz}, {"resonancePct", 100.0}, {"mix", 1.0}}, options);
      // A 10 ms burst of noise starts it; the low cutoffs take longest to
      // grow to their full level.
      const std::vector<float> out =
          rig.render(withSilence(noise(0.01, 5u, 0.1f, hostRate), static_cast<int>(3.0 * hostRate)));
      const size_t settled = static_cast<size_t>(2.0 * hostRate);
      const double hz = zeroCrossingHz(out, settled, hostRate);
      EXPECT_NEAR(hz / cutoffHz, 1.0, 0.005) << "oscillates at " << hz << " Hz";
      worstError = std::max(worstError, std::abs(hz / cutoffHz - 1.0));
      const double peak = peakOf(out, settled);
      quietest = std::min(quietest, peak);
      loudest = std::max(loudest, peak);
      const double thd = thdPercent(out, hz, hostRate, settled, 5);
      worstThd = std::max(worstThd, thd);
      EXPECT_LT(thd, 1.0) << "a clean sine";
      expectCleanSignal(out, 1.0f, "self-oscillation");
    }
    EXPECT_GT(quietest, 0.03) << "audible";
    EXPECT_LT(toDb(loudest) - toDb(quietest), 1.5) << "the same level across the range";
    spread = std::max(spread, toDb(loudest) - toDb(quietest));
  }
  std::cout << "[   OSC    ] filter.synth: pitch within " << juce::String(100.0 * worstError, 3)
            << " % of the cutoff, THD at most " << juce::String(worstThd, 2) << " %, level spread "
            << juce::String(spread, 2) << " dB" << std::endl;
}

TEST(SynthFilterEffectTest, CutoffAndResonanceHoldAtTheTopOfTheRange) {
  // The loop is solved without a unit delay and the ladder runs at four
  // times the rate, so the response at the cutoff does not drift as the
  // cutoff rises: four poles each 3 dB down with no resonance, and at 90 %
  // the linear ladder's 1 / (4 - k) = +11.5 dB, both at the cutoff itself,
  // from 500 Hz to 15 kHz alike.
  for (const double cutoffHz : {500.0, 5000.0, 15000.0}) {
    SCOPED_TRACE(cutoffHz);
    EXPECT_NEAR(gainAt({{"cutoffHz", cutoffHz}, {"resonancePct", 0.0}, {"mix", 1.0}}, cutoffHz), -12.0,
                1.0);
    const Params resonant = {{"cutoffHz", cutoffHz}, {"resonancePct", 90.0}, {"mix", 1.0}};
    const double atCutoff = gainAt(resonant, cutoffHz);
    EXPECT_NEAR(atCutoff, 11.5, 1.0);
    EXPECT_GT(atCutoff, gainAt(resonant, 0.94 * cutoffHz) + 1.0) << "the peak is at the cutoff";
    EXPECT_GT(atCutoff, gainAt(resonant, 1.06 * cutoffHz) + 1.0) << "the peak is at the cutoff";
  }
}

TEST(SynthFilterEffectTest, DriveChangesTheToneNotTheLevel) {
  // At guitar level (0.2 peak) and the default cutoff, each step of Drive
  // adds harmonics to a note, while the level of a chord stays within 2 dB
  // of the undriven one across the whole range.
  const std::vector<float> chord = musicalSignal(2.0, kRate, 0.2f);
  const std::vector<float> note = sine(1.0, 220.0, 0.2f, kRate);
  const size_t settled = static_cast<size_t>(0.5 * kRate);
  double cleanLevel = 0.0, previousThd = -1.0;
  for (const double driveDb : {0.0, 6.0, 12.0, 18.0, 24.0}) {
    SCOPED_TRACE(driveDb);
    EffectRig rig(kKind, {{"cutoffHz", 1000.0}, {"resonancePct", 0.0}, {"driveDb", driveDb}, {"mix", 1.0}});
    const double level = toDb(rmsOf(rig.render(chord), settled));
    rig.restart();
    const double thd = thdPercent(rig.render(note), 220.0, kRate, settled);
    std::cout << "[  DRIVE   ] " << driveDb << " dB: chord " << juce::String(level, 1) << " dB, THD "
              << juce::String(thd, 2) << " %" << std::endl;
    if (driveDb == 0.0)
      cleanLevel = level;
    EXPECT_NEAR(level, cleanLevel, 2.0);
    EXPECT_GT(thd, previousThd + 0.5) << "more drive, more harmonics";
    previousThd = thd;
  }
}

TEST(SynthFilterEffectTest, CostAtTheHeaviestSettingsIsInBudget) {
  // Every tanh busy and the cutoff moving with the envelope: the same budget
  // as the conformance run.
  expectCpuUnderBudget(kKind, {{"cutoffHz", 3000.0}, {"resonancePct", 100.0}, {"driveDb", 24.0},
                               {"envAmountPct", 100.0}, {"envAttackMs", 1.0}, {"mode", 3.0}},
                       kCpuAverageBudgetNs, kCpuWorstBudgetNs);
}

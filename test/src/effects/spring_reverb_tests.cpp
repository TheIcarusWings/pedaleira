// reverb.spring (plugin/src/effects/reverb/SpringReverb.cpp): the
// conformance test, then what a spring tank is for, each as a measurement
// of the wet signal (see reverb_test_helpers.h).
#include "effects/reverb_test_helpers.h"

#include <gtest/gtest.h>

using namespace fxtest;
using namespace reverbtest;

namespace {

constexpr const char* kKind = "reverb.spring";

// When the energy of one band of an impulse response arrives, in ms: the
// centroid of the band's energy over the first `windowSeconds`. Every band
// is 300 Hz wide, so the analysis filter delays them all alike (2 ms) and
// the differences are the reverb's.
double arrivalMs(const std::vector<float>& ir, double centreHz, double windowSeconds) {
  fx::Biquad first, second;
  first.setCoeffs(fx::BiquadCoeffs::bandpass(centreHz, centreHz / 300.0, kRate));
  second.setCoeffs(fx::BiquadCoeffs::bandpass(centreHz, centreHz / 300.0, kRate));
  double weighted = 0.0, total = 0.0;
  for (int i = 0; i < samples(windowSeconds); ++i) {
    const double v = second.process(first.process(ir[static_cast<size_t>(i)]));
    weighted += static_cast<double>(i) * v * v;
    total += v * v;
  }
  return 1000.0 * weighted / total / kRate;
}

}  // namespace

TEST(SpringReverbTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"decayS", 1.2}, {"dwell", 60.0}};
  options.stateParams = {{"decayS", 3.5}, {"tone", 25.0}, {"dwell", 80.0}, {"springs", 0.0},
                         {"spillover", 0.0}};
  // The wet path is the springs only: the first sound has crossed half a
  // spring, which is the effect and not latency.
  options.latency = LatencyCheck::AtMostOnset;
  // Measured 5,600 ns on average and 14,000 to 23,000 ns for the worst block
  // (Apple silicon, with other builds running).
  options.cpuAverageBudgetNs = 18000.0;
  options.cpuWorstBudgetNs = 150000.0;
  expectKindConforms("reverb.spring", options);
}

TEST(SpringReverbTest, DecayParameterIsTheRt60) {
  // Broadband: the loop's shelf shortens the decay above 2 kHz, but most of
  // the energy is below it.
  expectRt60MatchesDecay(kKind, {}, {0.8, 2.2, 5.0});
  expectRt60MatchesDecay(kKind, {{"springs", 0.0}}, {2.2});
  // The low band alone, and the top of the band clearly shorter.
  const std::vector<float> ir = wetImpulseResponse(kKind, {{"decayS", 3.0}, {"tone", 100.0}}, 4.5).left;
  const double low = bandRt60(ir, 200.0, 1200.0);
  const double high = bandRt60(ir, 3200.0, 4200.0);
  std::cout << "[ DAMPING  ] " << kKind << ": RT60 " << low << " s at 200 Hz to 1.2 kHz, " << high
            << " s at 3.2 to 4.2 kHz" << std::endl;
  EXPECT_NEAR(low, 3.0, 0.6);
  EXPECT_LT(high, 0.9 * low);
}

TEST(SpringReverbTest, ReportedTailHolds) {
  expectTailHolds(kKind, {{"decayS", 5.0}, {"tone", 100.0}});
  expectTailHolds(kKind, {{"decayS", 0.5}, {"tone", 100.0}, {"dwell", 100.0}});
}

TEST(SpringReverbTest, ASpringChirps) {
  // The drip: in each echo the high frequencies arrive after the low ones,
  // later the closer they are to the top of the springs' band. Measured as
  // the arrival time of five bands in the first pass through the springs
  // (two springs, before either has echoed).
  const std::vector<float> ir =
      wetImpulseResponse(kKind, {{"springs", 0.0}, {"tone", 100.0}, {"dwell", 0.0}}, 1.0).left;
  const double bands[] = {600.0, 1200.0, 2000.0, 2800.0, 3500.0, 4000.0};
  double previous = 0.0;
  double firstArrival = 0.0, lastArrival = 0.0;
  for (const double hz : bands) {
    const double ms = arrivalMs(ir, hz, 0.075);
    std::cout << "[  CHIRP   ] " << kKind << ": " << hz << " Hz arrives at " << ms << " ms" << std::endl;
    EXPECT_GT(ms, previous) << "group delay must rise with frequency (" << hz << " Hz)";
    if (previous == 0.0)
      firstArrival = ms;
    lastArrival = ms;
    previous = ms;
  }
  // From the bottom of the band to the top the first pass alone spreads by
  // several milliseconds, and every later pass adds as much again.
  EXPECT_GT(lastArrival - firstArrival, 6.0);

  // The band stops there: nothing above the springs' transition frequency.
  const double inBand = energyOf(bandOf(ir, 500.0, 3500.0));
  // (Filtered twice: once leaves the skirt of the band itself in the figure.)
  const double above = energyOf(bandOf(bandOf(ir, 9000.0, 16000.0), 9000.0, 16000.0));
  std::cout << "[  CHIRP   ] " << kKind << ": energy above 9 kHz "
            << 10.0 * std::log10(above / inBand) << " dB against the band" << std::endl;
  EXPECT_LT(10.0 * std::log10(above / inBand), -40.0);
}

TEST(SpringReverbTest, EchoesRepeatAtTheSpringsRoundTrips) {
  // A spring tank flutters: its tail repeats at each spring's round trip
  // (57 and 68 ms at low frequencies, and a third at 80 ms with three
  // springs). That is its sound, where a room, hall or plate must not.
  const std::vector<float> two =
      bandOf(wetImpulseResponse(kKind, {{"springs", 0.0}, {"decayS", 3.0}}, 3.0).left, 150.0, 1000.0);
  const Periodicity p = tailPeriodicity(two, 0.3, 2.5, 3.0, 40.0, 100.0);
  std::cout << "[ PERIODIC ] " << kKind << ": autocorrelation peak " << p.peak << " at " << p.lagMs
            << " ms" << std::endl;
  EXPECT_GT(p.peak, 0.3);
  EXPECT_GT(p.lagMs, 50.0);
  EXPECT_LT(p.lagMs, 90.0);
}

TEST(SpringReverbTest, ThirdSpringChangesThePatternNotTheLevel) {
  const std::vector<float> two = wetImpulseResponse(kKind, {{"springs", 0.0}}, 3.0).left;
  const std::vector<float> three = wetImpulseResponse(kKind, {{"springs", 1.0}}, 3.0).left;
  EXPECT_LT(correlationOf(two, three, 0, two.size()), 0.95);
  EXPECT_NEAR(10.0 * std::log10(energyOf(three) / energyOf(two)), 0.0, 1.0);

  // Switching under audio fades the third spring; it does not step.
  EffectRig rig(kKind, {{"springs", 0.0}, {"mix", 1.0}});
  const std::vector<float> tone = sine(4.0, 220.0, 0.25f, kRate);
  std::vector<float> out = rig.render(std::vector<float>(tone.begin(), tone.begin() + 96000));
  const float steady = maxStep(out, 48000, out.size());
  rig.set("springs", 1.0);
  const std::vector<float> on(tone.begin() + 96000, tone.begin() + 144000);
  const std::vector<float> switched = rig.render(on);
  rig.set("springs", 0.0);
  const std::vector<float> back = rig.render(std::vector<float>(tone.begin() + 144000, tone.end()));
  EXPECT_LT(maxStep(switched, 0, switched.size()), steady * 2.0f + 0.004f);
  EXPECT_LT(maxStep(back, 0, back.size()), steady * 2.0f + 0.004f);
}

TEST(SpringReverbTest, DwellDrivesTheSpringsHarder) {
  // Small signals: the wet level rises 3 dB for each doubling of the drive
  // (0.5 to 8 times across the knob, 12 dB in all).
  const auto wetDb = [](double dwell, float amplitude) {
    DirectKind direct(kKind, {{"dwell", dwell}});
    const std::vector<float> in = sine(4.0, 440.0, amplitude, kRate);
    const std::vector<float> out = direct.render(in);
    return 20.0 * std::log10(rmsOf(out, 96000) / rmsOf(in, 96000));
  };
  const double quietLow = wetDb(0.0, 0.01f), quietHigh = wetDb(100.0, 0.01f);
  const double loudHigh = wetDb(100.0, 1.0f);
  std::cout << "[  DWELL   ] " << kKind << ": wet gain at -40 dBFS " << quietLow << " dB (dwell 0), "
            << quietHigh << " dB (dwell 100); at full scale " << loudHigh << " dB (dwell 100)"
            << std::endl;
  EXPECT_NEAR(quietHigh - quietLow, 12.0, 1.5);
  // Loud signals: the driver saturates, so the same dwell gives less gain.
  EXPECT_LT(loudHigh, quietHigh - 6.0);
}

TEST(SpringReverbTest, ToneSetsTheTop) {
  const auto topDb = [](double tone) {
    const std::vector<float> ir = wetImpulseResponse(kKind, {{"tone", tone}}, 2.0).left;
    return 10.0 * std::log10(energyOf(bandOf(ir, 2500.0, 4000.0)) / energyOf(bandOf(ir, 300.0, 900.0)));
  };
  const double dark = topDb(0.0), bright = topDb(100.0);
  std::cout << "[   TONE   ] " << kKind << ": 2.5 to 4 kHz against 300 to 900 Hz: " << dark
            << " dB at tone 0, " << bright << " dB at tone 100" << std::endl;
  EXPECT_GT(bright, dark + 10.0);
}

TEST(SpringReverbTest, WideInTrueStereoMonoElsewhere) {
  expectWideInStereoOnly(kKind, {{"decayS", 3.0}}, 0.3, 2.5);
  expectWideInStereoOnly(kKind, {{"decayS", 3.0}, {"springs", 0.0}}, 0.3, 2.5);
}

TEST(SpringReverbTest, BoundedAtTheLongestDecayWithHotInput) {
  expectBoundedWithHotInput(kKind, {{"decayS", 5.0}, {"dwell", 100.0}, {"tone", 100.0}}, 8.0f);
}

TEST(SpringReverbTest, MovingDecayToneAndDwellDoesNotZipper) {
  // All three are slewed and ramped between control ticks.
  const ZipperResult r = sweepUnderAudio(
      kKind, {}, {{"decayS", 0.5, 5.0}, {"tone", 0.0, 100.0}, {"dwell", 0.0, 100.0}});
  expectNoZipper(r);
}

TEST(SpringReverbTest, WetLevelFollowsTheCommonLaw) {
  // At the default dwell and a level the driver does not saturate at.
  for (const double decay : {1.0, 4.0}) {
    const double level = wetLevelDb(kKind, {{"decayS", decay}, {"tone", 100.0}}, decay * 1.5 + 1.0);
    const double law = 10.0 * std::log10(0.5 * std::sqrt(decay / 2.0));
    std::cout << "[  LEVEL   ] " << kKind << ": wet " << level << " dB at a decay of " << decay
              << " s (law " << law << " dB)" << std::endl;
    EXPECT_NEAR(level, law, 2.5);
  }
}

TEST(SpringReverbTest, TailRingsOutOnBypassOnlyWithSpilloverOn) {
  expectSpilloverSwitchWorks(kKind, {});
}

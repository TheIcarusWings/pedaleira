// reverb.hall (plugin/src/effects/reverb/HallReverb.cpp): the conformance
// test, then what a hall reverb is for, each as a measurement of the wet
// signal (see reverb_test_helpers.h).
#include "effects/reverb/ReverbParts.h"
#include "effects/reverb_test_helpers.h"

#include <gtest/gtest.h>

using namespace fxtest;
using namespace reverbtest;

namespace {

constexpr const char* kKind = "reverb.hall";

// Nothing colouring the tail: every band decays alike and the wet filters
// are out of the way, so a broadband measurement is the one the decay
// parameter sets.
const Params kFlat = {{"damping", 0.0}, {"lowCutHz", 20.0}, {"highCutHz", 20000.0},
                      {"preDelayMs", 0.0}};

// Share of an impulse response's energy that has arrived `seconds` after
// its onset.
double energyShareBy(const std::vector<float>& ir, double seconds) {
  const size_t onset = static_cast<size_t>(std::max(0, onsetIndex(ir, 0.001f)));
  return energyOf(ir, 0, onset + static_cast<size_t>(samples(seconds))) / energyOf(ir);
}

}  // namespace

TEST(HallReverbTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"decayS", 1.5}, {"size", 40.0}, {"preDelayMs", 10.0}, {"modulation", 60.0}};
  options.stateParams = {{"decayS", 6.5},     {"size", 20.0},      {"preDelayMs", 80.0},
                         {"damping", 70.0},   {"lowCutHz", 200.0}, {"highCutHz", 5000.0},
                         {"modulation", 75.0}, {"spillover", 0.0}};
  // The wet path is the reverberation only: it starts after the pre-delay
  // and the shortest line, which is the effect and not latency.
  options.latency = LatencyCheck::AtMostOnset;
  // Measured 10,600 to 11,800 ns on average and 25,000 to 31,000 ns for the
  // worst block (Apple silicon, with other builds running).
  options.cpuAverageBudgetNs = 36000.0;
  options.cpuWorstBudgetNs = 200000.0;
  expectKindConforms("reverb.hall", options);
}

TEST(HallReverbTest, DecayParameterIsTheRt60) {
  expectRt60MatchesDecay(kKind, kFlat, {1.0, 3.0, 12.0});
  // At both ends of the size range, and with the modulation off and full.
  expectRt60MatchesDecay(kKind, with(kFlat, {{"size", 0.0}}), {3.0});
  expectRt60MatchesDecay(kKind, with(kFlat, {{"size", 100.0}, {"modulation", 0.0}}), {3.0});
  expectRt60MatchesDecay(kKind, with(kFlat, {{"modulation", 100.0}}), {3.0});
  // And at the default damping, in the band under the damping corner.
  const std::vector<float> ir = wetImpulseResponse(kKind, {{"decayS", 4.0}}, 5.5).left;
  EXPECT_NEAR(bandRt60(ir, 200.0, 1000.0), 4.0, 0.8);
}

TEST(HallReverbTest, ReportedTailHoldsWithNothingDamped) {
  expectTailHolds(kKind, with(kFlat, {{"decayS", 4.0}, {"preDelayMs", 200.0}, {"size", 100.0}}));
  expectTailHolds(kKind, with(kFlat, {{"decayS", 1.0}, {"size", 100.0}}));
}

TEST(HallReverbTest, TheModulatedReadNeverAmplifies) {
  // The moving lines of every reverb are read with fx::reverb::readLagrange
  // from inside a loop that is close to lossless at long decays. If that
  // read had gain anywhere, at any position between two samples, the loop
  // would grow there. Its response over the whole band at 101 positions:
  double worst = 0.0, worstAtSixKhz = 1.0;
  for (int p = 0; p <= 100; ++p) {
    float w[4];
    fx::reverb::lagrangeWeights(static_cast<float>(p) * 0.01f, w);
    for (int k = 0; k <= 480; ++k) {
      const double omega = kPi * k / 480.0;
      std::complex<double> h{0.0, 0.0};
      for (int n = 0; n < 4; ++n)
        h += static_cast<double>(w[n]) * std::exp(std::complex<double>(0.0, -omega * n));
      worst = std::max(worst, std::abs(h));
      if (k == 120)
        worstAtSixKhz = std::min(worstAtSixKhz, std::abs(h));
    }
  }
  std::cout << "[ LAGRANGE ] largest gain " << worst << ", lowest gain at 6 kHz "
            << 20.0 * std::log10(worstAtSixKhz) << " dB" << std::endl;
  EXPECT_LE(worst, 1.0 + 1.0e-6);
  EXPECT_GT(20.0 * std::log10(worstAtSixKhz), -0.1);
}

TEST(HallReverbTest, DampingShortensTheHighs) {
  const std::vector<float> damped =
      wetImpulseResponse(kKind, with(kFlat, {{"decayS", 4.0}, {"damping", 80.0}}), 5.5).left;
  const double low = bandRt60(damped, 200.0, 1000.0);
  const double high = bandRt60(damped, 6000.0, 10000.0);
  std::cout << "[ DAMPING  ] " << kKind << ": RT60 " << low << " s at 200 Hz to 1 kHz, " << high
            << " s at 6 to 10 kHz with the damping at 80%" << std::endl;
  EXPECT_NEAR(low, 4.0, 0.8);
  EXPECT_LT(high, 0.5 * low);

  const std::vector<float> open = wetImpulseResponse(
      kKind, with(kFlat, {{"decayS", 4.0}, {"modulation", 0.0}}), 5.5).left;
  EXPECT_NEAR(bandRt60(open, 6000.0, 10000.0), bandRt60(open, 200.0, 1000.0), 0.8);
}

TEST(HallReverbTest, PreDelayMatchesTheParameter) {
  expectPreDelayMatches(kKind, kFlat, {10.0, 62.5, 200.0});
}

TEST(HallReverbTest, LowAndHighCutShapeTheWetSignal) {
  const auto bandDb = [](const Params& params, double lowHz, double highHz) {
    return 10.0 * std::log10(energyOf(bandOf(wetImpulseResponse(kKind, params, 2.0).left, lowHz, highHz)));
  };
  const Params open = with(kFlat, {{"decayS", 1.5}});
  EXPECT_LT(bandDb(with(open, {{"lowCutHz", 500.0}}), 100.0, 250.0), bandDb(open, 100.0, 250.0) - 10.0);
  EXPECT_LT(bandDb(with(open, {{"highCutHz", 2000.0}}), 5000.0, 9000.0),
            bandDb(open, 5000.0, 9000.0) - 15.0);
  EXPECT_NEAR(bandDb(with(open, {{"lowCutHz", 100.0}, {"highCutHz", 8000.0}}), 500.0, 2000.0),
              bandDb(open, 500.0, 2000.0), 1.0);
}

TEST(HallReverbTest, EchoDensityBuildsAndTheTailHasNoPitch) {
  const Params params = with(kFlat, {{"decayS", 4.0}});
  const Density d = echoDensity(kKind, params, 0.04, 0.15, 0.25, 0.6, 0.8);
  EXPECT_GT(d.early, 6.0);
  EXPECT_LT(d.middle, d.early);
  EXPECT_LT(d.late, 4.0);
  EXPECT_LE(d.late, d.middle + 0.5);

  // No lag between 1 and 400 ms at which the late tail repeats itself, with
  // the modulation off (which is the hard case) at every size.
  for (const double size : {0.0, 60.0, 100.0}) {
    const std::vector<float> ir = wetImpulseResponse(
        kKind, with(params, {{"size", size}, {"modulation", 0.0}}), 3.2).left;
    const Periodicity p = tailPeriodicity(ir, 0.6, 3.0, 4.0, 1.0, 400.0);
    std::cout << "[ PERIODIC ] " << kKind << " at size " << size << ": autocorrelation peak "
              << p.peak << " at " << p.lagMs << " ms" << std::endl;
    EXPECT_LT(p.peak, 0.3);
  }
}

TEST(HallReverbTest, BuildsUpSlowlyAndSizeSlowsItFurther) {
  // A room has delivered a good part of its energy 30 ms after it starts; a
  // hall has barely begun.
  const Params params = with(kFlat, {{"decayS", 2.0}});
  const double hall = energyShareBy(wetImpulseResponse(kKind, params, 3.0).left, 0.03);
  const double room = energyShareBy(wetImpulseResponse("reverb.room", params, 3.0).left, 0.03);
  const double smallRoom = energyShareBy(wetImpulseResponse(kKind, with(params, {{"size", 0.0}}), 3.0).left, 0.1);
  const double large = energyShareBy(wetImpulseResponse(kKind, with(params, {{"size", 100.0}}), 3.0).left, 0.1);
  std::cout << "[ BUILD-UP ] energy arrived after 30 ms: hall " << 100.0 * hall << "%, room "
            << 100.0 * room << "%; hall after 100 ms: " << 100.0 * smallRoom << "% at size 0, "
            << 100.0 * large << "% at size 100" << std::endl;
  EXPECT_LT(hall, 0.05);
  EXPECT_GT(room, 2.0 * hall);
  EXPECT_LT(large, smallRoom);
}

TEST(HallReverbTest, ModulationMovesTheTailWithoutChangingItsLevel) {
  // A steady 1 kHz sine: with the modulation off the wet signal is that
  // sine and nothing else; with it on, the moving lines spread it into
  // sidebands. Measured as what is left after removing the carrier.
  const auto sidebandsDb = [](double modulation) {
    DirectKind direct(kKind, with(kFlat, {{"decayS", 3.0}, {"modulation", modulation}}));
    const std::vector<float> out = direct.render(sine(8.0, 1000.0, 0.25f, kRate));
    const size_t from = static_cast<size_t>(samples(6.0));
    const double carrier = goertzelPower(out.data() + from, out.size() - from, 1000.0, kRate);
    const double total = 2.0 * rmsOf(out, from) * rmsOf(out, from);
    return 10.0 * std::log10(std::max(1.0e-12, total - carrier) / carrier);
  };
  const double still = sidebandsDb(0.0), moving = sidebandsDb(100.0);
  std::cout << "[   MOD    ] " << kKind << ": energy off the carrier " << still
            << " dB with the modulation at 0, " << moving << " dB at 100%" << std::endl;
  EXPECT_GT(moving, still + 10.0);

  const double levelStill = wetLevelDb(kKind, with(kFlat, {{"modulation", 0.0}}), 6.0);
  const double levelMoving = wetLevelDb(kKind, with(kFlat, {{"modulation", 100.0}}), 6.0);
  EXPECT_NEAR(levelMoving, levelStill, 1.0);
}

TEST(HallReverbTest, WideInTrueStereoMonoElsewhere) {
  expectWideInStereoOnly(kKind, with(kFlat, {{"decayS", 4.0}}), 0.5, 3.0);
}

TEST(HallReverbTest, BoundedAtTheLongestDecayWithHotInput) {
  expectBoundedWithHotInput(kKind, with(kFlat, {{"decayS", 12.0}, {"size", 0.0}, {"modulation", 100.0}}),
                            8.0f);
}

TEST(HallReverbTest, MovingDecayAndSizeDoesNotZipper) {
  // Decay, damping and modulation depth are slewed and ramped per sample; a
  // size change is a crossfade between two read taps.
  const ZipperResult decay = sweepUnderAudio(
      kKind, {}, {{"decayS", 1.0, 12.0}, {"damping", 0.0, 100.0}, {"modulation", 0.0, 100.0}});
  expectNoZipper(decay);
  const ZipperResult size = sweepUnderAudio(kKind, {}, {{"size", 0.0, 100.0}});
  expectNoZipper(size);
  const ZipperResult rest = sweepUnderAudio(
      kKind, {}, {{"preDelayMs", 0.0, 200.0}, {"lowCutHz", 20.0, 1000.0}, {"highCutHz", 20000.0, 1000.0}});
  expectNoZipper(rest);
}

TEST(HallReverbTest, WetLevelFollowsTheCommonLaw) {
  for (const double decay : {1.0, 4.0}) {
    const double level = wetLevelDb(kKind, with(kFlat, {{"decayS", decay}}), decay * 1.5 + 1.0);
    const double law = 10.0 * std::log10(0.5 * std::sqrt(decay / 2.0));
    std::cout << "[  LEVEL   ] " << kKind << ": wet " << level << " dB at a decay of " << decay
              << " s (law " << law << " dB)" << std::endl;
    EXPECT_NEAR(level, law, 2.0);
  }
}

TEST(HallReverbTest, TailRingsOutOnBypassOnlyWithSpilloverOn) {
  expectSpilloverSwitchWorks(kKind, {});
}

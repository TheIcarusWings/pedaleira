// reverb.plate (plugin/src/effects/reverb/PlateReverb.cpp): the conformance
// test, then what a plate reverb is for, each as a measurement of the wet
// signal (see reverb_test_helpers.h).
#include "effects/reverb_test_helpers.h"

#include <gtest/gtest.h>

using namespace fxtest;
using namespace reverbtest;

namespace {

constexpr const char* kKind = "reverb.plate";

// Nothing colouring the tail: every band decays alike and the wet filters
// are out of the way.
const Params kFlat = {{"damping", 0.0}, {"lowCutHz", 20.0}, {"highCutHz", 20000.0},
                      {"preDelayMs", 0.0}};

}  // namespace

TEST(PlateReverbTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"decayS", 1.2}, {"preDelayMs", 10.0}, {"modulation", 60.0}};
  options.stateParams = {{"decayS", 4.5},     {"preDelayMs", 45.0}, {"damping", 60.0},
                         {"lowCutHz", 300.0}, {"highCutHz", 6000.0}, {"modulation", 80.0},
                         {"spillover", 0.0}};
  // The wet path is the plate only: its first tap is a few milliseconds
  // into the tank, which is the effect and not latency.
  options.latency = LatencyCheck::AtMostOnset;
  // Measured 2,900 to 3,100 ns on average and 5,500 to 10,500 ns for the
  // worst block (Apple silicon, with other builds running).
  options.cpuAverageBudgetNs = 12000.0;
  options.cpuWorstBudgetNs = 120000.0;
  expectKindConforms("reverb.plate", options);
}

TEST(PlateReverbTest, DecayParameterIsTheRt60) {
  expectRt60MatchesDecay(kKind, kFlat, {0.5, 2.0, 10.0});
  expectRt60MatchesDecay(kKind, with(kFlat, {{"modulation", 0.0}}), {2.0});
  expectRt60MatchesDecay(kKind, with(kFlat, {{"modulation", 100.0}}), {2.0});
  // And at the default damping, in the band under the damping corner.
  const std::vector<float> ir = wetImpulseResponse(kKind, {{"decayS", 3.0}}, 4.5).left;
  EXPECT_NEAR(bandRt60(ir, 200.0, 1000.0), 3.0, 0.6);
}

TEST(PlateReverbTest, ReportedTailHoldsWithNothingDamped) {
  expectTailHolds(kKind, with(kFlat, {{"decayS", 4.0}, {"preDelayMs", 200.0}}));
  expectTailHolds(kKind, with(kFlat, {{"decayS", 0.5}}));
}

TEST(PlateReverbTest, DampingShortensTheHighs) {
  const std::vector<float> damped =
      wetImpulseResponse(kKind, with(kFlat, {{"decayS", 3.0}, {"damping", 80.0}}), 4.5).left;
  const double low = bandRt60(damped, 200.0, 1000.0);
  const double high = bandRt60(damped, 8000.0, 12000.0);
  std::cout << "[ DAMPING  ] " << kKind << ": RT60 " << low << " s at 200 Hz to 1 kHz, " << high
            << " s at 8 to 12 kHz with the damping at 80%" << std::endl;
  EXPECT_NEAR(low, 3.0, 0.6);
  EXPECT_LT(high, 0.5 * low);

  // A plate is bright: with the damping at 0 the top decays with the rest.
  const std::vector<float> open = wetImpulseResponse(
      kKind, with(kFlat, {{"decayS", 3.0}, {"modulation", 0.0}}), 4.5).left;
  EXPECT_NEAR(bandRt60(open, 8000.0, 12000.0), bandRt60(open, 200.0, 1000.0), 0.6);
}

TEST(PlateReverbTest, PreDelayMatchesTheParameter) {
  expectPreDelayMatches(kKind, kFlat, {10.0, 62.5, 200.0});
}

TEST(PlateReverbTest, LowAndHighCutShapeTheWetSignal) {
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

TEST(PlateReverbTest, ImmediateDenseAndWithoutPitch) {
  const Params params = with(kFlat, {{"decayS", 3.0}});
  // Immediate: the plate speaks within 10 ms of the input and has delivered
  // a real share of its energy 50 ms later (a hall has not, see its test).
  const std::vector<float> ir = wetImpulseResponse(kKind, params, 4.0).left;
  const int onset = onsetIndex(ir, 0.01f);
  EXPECT_LT(onset, samples(0.01));
  EXPECT_GT(energyOf(ir, 0, static_cast<size_t>(onset + samples(0.05))) / energyOf(ir), 0.02);

  // Dense: the four input diffusers make a burst of the impulse before it
  // reaches the tank, so the response is close to noise from the start and
  // is noise within a third of a second.
  const Density d = echoDensity(kKind, params, 0.03, 0.1, 0.2, 0.5, 0.8);
  EXPECT_LT(d.middle, d.early);
  EXPECT_LT(d.late, 4.0);
  EXPECT_LE(d.late, d.middle + 0.5);

  // The tank is one loop of 0.73 s: the late tail must not repeat at that
  // period, or at any lag from 1 ms up to it, modulation off.
  const std::vector<float> still =
      wetImpulseResponse(kKind, with(params, {{"modulation", 0.0}}), 4.0).left;
  const Periodicity p = tailPeriodicity(still, 0.8, 3.8, 3.0, 1.0, 1000.0);
  std::cout << "[ PERIODIC ] " << kKind << ": autocorrelation peak " << p.peak << " at " << p.lagMs
            << " ms" << std::endl;
  EXPECT_LT(p.peak, 0.3);
}

TEST(PlateReverbTest, ModulationMovesTheTailWithoutChangingItsLevel) {
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

  const double levelStill = wetLevelDb(kKind, with(kFlat, {{"modulation", 0.0}}), 5.0);
  const double levelMoving = wetLevelDb(kKind, with(kFlat, {{"modulation", 100.0}}), 5.0);
  EXPECT_NEAR(levelMoving, levelStill, 1.0);
}

TEST(PlateReverbTest, WideInTrueStereoMonoElsewhere) {
  expectWideInStereoOnly(kKind, with(kFlat, {{"decayS", 3.0}}), 0.5, 2.5);
}

TEST(PlateReverbTest, BoundedAtTheLongestDecayWithHotInput) {
  expectBoundedWithHotInput(kKind, with(kFlat, {{"decayS", 10.0}, {"modulation", 100.0}}), 8.0f);
}

TEST(PlateReverbTest, MovingDecayDoesNotZipper) {
  // Decay, damping and modulation depth are slewed and ramped per sample;
  // the pre-delay glides.
  const ZipperResult decay = sweepUnderAudio(
      kKind, {}, {{"decayS", 0.5, 10.0}, {"damping", 0.0, 100.0}, {"modulation", 0.0, 100.0}});
  expectNoZipper(decay);
  const ZipperResult rest = sweepUnderAudio(
      kKind, {}, {{"preDelayMs", 0.0, 200.0}, {"lowCutHz", 20.0, 1000.0}, {"highCutHz", 20000.0, 1000.0}});
  expectNoZipper(rest);
}

TEST(PlateReverbTest, WetLevelFollowsTheCommonLaw) {
  for (const double decay : {1.0, 4.0}) {
    const double level = wetLevelDb(kKind, with(kFlat, {{"decayS", decay}}), decay * 1.5 + 1.0);
    const double law = 10.0 * std::log10(0.5 * std::sqrt(decay / 2.0));
    std::cout << "[  LEVEL   ] " << kKind << ": wet " << level << " dB at a decay of " << decay
              << " s (law " << law << " dB)" << std::endl;
    EXPECT_NEAR(level, law, 2.0);
  }
}

TEST(PlateReverbTest, TailRingsOutOnBypassOnlyWithSpilloverOn) {
  expectSpilloverSwitchWorks(kKind, {});
}

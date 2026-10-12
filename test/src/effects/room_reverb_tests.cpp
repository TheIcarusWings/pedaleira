// reverb.room (plugin/src/effects/reverb/RoomReverb.cpp): the conformance
// test, then what a room reverb is for, each as a measurement of the wet
// signal (see reverb_test_helpers.h).
#include "effects/reverb_test_helpers.h"

#include <gtest/gtest.h>

using namespace fxtest;
using namespace reverbtest;

namespace {

constexpr const char* kKind = "reverb.room";

// Nothing colouring the tail: every band decays alike and the wet filters
// are out of the way, so a broadband measurement is the one the decay
// parameter sets.
const Params kFlat = {{"damping", 0.0}, {"lowCutHz", 20.0}, {"highCutHz", 20000.0},
                      {"preDelayMs", 0.0}};

}  // namespace

TEST(RoomReverbTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"decayS", 0.6}, {"size", 70.0}, {"preDelayMs", 15.0}};
  options.stateParams = {{"decayS", 1.7},     {"size", 20.0},       {"preDelayMs", 33.0},
                         {"damping", 70.0},   {"lowCutHz", 200.0},  {"highCutHz", 5000.0},
                         {"spillover", 0.0}};
  // The wet path is reflections only: it starts after the pre-delay and the
  // first reflection, which is the effect and not latency. The kind reports
  // none.
  options.latency = LatencyCheck::AtMostOnset;
  // Measured 10,600 to 11,600 ns on average and 20,000 to 31,000 ns for the
  // worst block (Apple silicon, with other builds running).
  options.cpuAverageBudgetNs = 36000.0;
  options.cpuWorstBudgetNs = 200000.0;
  expectKindConforms("reverb.room", options);
}

TEST(RoomReverbTest, DecayParameterIsTheRt60) {
  expectRt60MatchesDecay(kKind, kFlat, {0.3, 1.0, 2.5});
  // The same at both ends of the size range: the decay does not follow the
  // room size.
  expectRt60MatchesDecay(kKind, with(kFlat, {{"size", 0.0}}), {1.0});
  expectRt60MatchesDecay(kKind, with(kFlat, {{"size", 100.0}}), {1.0});
  // And at the default damping, in the band under the damping corner.
  const std::vector<float> ir = wetImpulseResponse(kKind, {{"decayS", 1.5}}, 3.0).left;
  EXPECT_NEAR(bandRt60(ir, 200.0, 1000.0), 1.5, 0.3);
}

TEST(RoomReverbTest, ReportedTailHoldsWithNothingDamped) {
  expectTailHolds(kKind, with(kFlat, {{"decayS", 2.5}, {"preDelayMs", 200.0}}));
  expectTailHolds(kKind, with(kFlat, {{"decayS", 0.2}, {"size", 100.0}}));
}

TEST(RoomReverbTest, DampingShortensTheHighs) {
  const std::vector<float> damped =
      wetImpulseResponse(kKind, with(kFlat, {{"decayS", 2.0}, {"damping", 80.0}}), 3.5).left;
  const double low = bandRt60(damped, 200.0, 1000.0);
  const double high = bandRt60(damped, 7000.0, 11000.0);
  std::cout << "[ DAMPING  ] " << kKind << ": RT60 " << low << " s at 200 Hz to 1 kHz, " << high
            << " s at 7 to 11 kHz with the damping at 80%" << std::endl;
  EXPECT_NEAR(low, 2.0, 0.4);
  EXPECT_LT(high, 0.5 * low);

  // With the damping at 0 the two bands decay together.
  const std::vector<float> open = wetImpulseResponse(kKind, with(kFlat, {{"decayS", 2.0}}), 3.5).left;
  EXPECT_NEAR(bandRt60(open, 7000.0, 11000.0), bandRt60(open, 200.0, 1000.0), 0.4);
}

TEST(RoomReverbTest, PreDelayMatchesTheParameter) {
  expectPreDelayMatches(kKind, kFlat, {10.0, 62.5, 200.0});
}

TEST(RoomReverbTest, LowAndHighCutShapeTheWetSignal) {
  const auto bandDb = [](const Params& params, double lowHz, double highHz) {
    return 10.0 * std::log10(energyOf(bandOf(wetImpulseResponse(kKind, params, 1.5).left, lowHz, highHz)));
  };
  const Params open = with(kFlat, {{"decayS", 1.0}});
  // An octave under a 500 Hz low cut and over a 2 kHz high cut: 12 dB per
  // octave each.
  EXPECT_LT(bandDb(with(open, {{"lowCutHz", 500.0}}), 100.0, 250.0), bandDb(open, 100.0, 250.0) - 10.0);
  EXPECT_LT(bandDb(with(open, {{"highCutHz", 2000.0}}), 5000.0, 9000.0),
            bandDb(open, 5000.0, 9000.0) - 15.0);
  // And the middle is left alone.
  EXPECT_NEAR(bandDb(with(open, {{"lowCutHz", 100.0}, {"highCutHz", 8000.0}}), 500.0, 2000.0),
              bandDb(open, 500.0, 2000.0), 1.0);
}

TEST(RoomReverbTest, EchoDensityBuildsAndTheTailHasNoPitch) {
  const Params params = with(kFlat, {{"decayS", 2.0}});
  // Separate reflections at first, noise within a tenth of a second.
  const Density d = echoDensity(kKind, params, 0.02, 0.06, 0.1, 0.3, 0.4);
  EXPECT_GT(d.early, 8.0);
  EXPECT_LT(d.middle, d.early);
  EXPECT_LT(d.late, 4.0);
  EXPECT_LE(d.late, d.middle + 0.5);

  // No lag between 1 and 200 ms at which the late tail repeats itself: at
  // the smallest size too, where the lines are shortest.
  for (const double size : {0.0, 50.0, 100.0}) {
    const std::vector<float> ir = wetImpulseResponse(kKind, with(params, {{"size", size}}), 2.0).left;
    const Periodicity p = tailPeriodicity(ir, 0.3, 1.5, 2.0, 1.0, 200.0);
    std::cout << "[ PERIODIC ] " << kKind << " at size " << size << ": autocorrelation peak "
              << p.peak << " at " << p.lagMs << " ms" << std::endl;
    EXPECT_LT(p.peak, 0.3);
  }
}

TEST(RoomReverbTest, SizeMovesTheReflections) {
  // The energy of the first 100 ms arrives later in a larger room.
  const auto centroidMs = [](double size) {
    const std::vector<float> ir = wetImpulseResponse(kKind, with(kFlat, {{"decayS", 1.0}, {"size", size}}), 0.1).left;
    double weighted = 0.0, total = 0.0;
    for (size_t i = 0; i < ir.size(); ++i) {
      weighted += static_cast<double>(i) * ir[i] * ir[i];
      total += static_cast<double>(ir[i]) * ir[i];
    }
    return 1000.0 * weighted / total / kRate;
  };
  const double smallRoom = centroidMs(0.0), large = centroidMs(100.0);
  std::cout << "[   SIZE   ] " << kKind << ": centroid of the first 100 ms at " << smallRoom
            << " ms (size 0) and " << large << " ms (size 100)" << std::endl;
  EXPECT_GT(large, smallRoom + 4.0);
}

TEST(RoomReverbTest, WideInTrueStereoMonoElsewhere) {
  expectWideInStereoOnly(kKind, with(kFlat, {{"decayS", 2.0}}), 0.2, 1.5);
}

TEST(RoomReverbTest, BoundedAtTheLongestDecayWithHotInput) {
  // The level law keeps the wet signal under its input (0.58 RMS for noise
  // at full scale) at every decay of this kind; the bound leaves room for
  // peaks.
  expectBoundedWithHotInput(kKind, with(kFlat, {{"decayS", 2.5}, {"size", 0.0}}), 8.0f);
}

TEST(RoomReverbTest, MovingDecayAndSizeDoesNotZipper) {
  // Decay and damping are slewed and ramped per sample; a size change is a
  // crossfade between two read taps. Neither may step the waveform.
  const ZipperResult decay = sweepUnderAudio(kKind, {}, {{"decayS", 0.2, 2.5}, {"damping", 0.0, 100.0}});
  expectNoZipper(decay);
  const ZipperResult size = sweepUnderAudio(kKind, {{"decayS", 1.5}}, {{"size", 0.0, 100.0}});
  expectNoZipper(size);
  const ZipperResult rest = sweepUnderAudio(
      kKind, {{"decayS", 1.5}},
      {{"preDelayMs", 0.0, 200.0}, {"lowCutHz", 20.0, 1000.0}, {"highCutHz", 20000.0, 1000.0}});
  expectNoZipper(rest);
}

TEST(RoomReverbTest, WetLevelFollowsTheCommonLaw) {
  // -3 dB at a 2 s decay and 1.5 dB per doubling (wetEnergyTarget), with the
  // early reflections on top.
  for (const double decay : {0.5, 2.0}) {
    const double level = wetLevelDb(kKind, with(kFlat, {{"decayS", decay}}), 3.0);
    const double law = 10.0 * std::log10(0.5 * std::sqrt(decay / 2.0) + 0.08);
    std::cout << "[  LEVEL   ] " << kKind << ": wet " << level << " dB at a decay of " << decay
              << " s (law " << law << " dB)" << std::endl;
    EXPECT_NEAR(level, law, 2.0);
  }
}

TEST(RoomReverbTest, TailRingsOutOnBypassOnlyWithSpilloverOn) {
  expectSpilloverSwitchWorks(kKind, {{"decayS", 2.0}});
}

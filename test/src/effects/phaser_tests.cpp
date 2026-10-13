// modulation.phaser (plugin/src/effects/modulation/Phaser.cpp): the
// conformance test, then what a phaser is for: one notch for every two
// allpass stages, at the frequencies the stage break point predicts, swept at
// the stated rate (free and tempo-synced), feedback that sharpens the
// resonances, and a loop that cannot run away.
#include "effects/effect_test_helpers.h"
#include "effects/modulation_test_helpers.h"

#include <gtest/gtest.h>

using namespace fxtest;
using namespace modtest;

namespace {

constexpr const char* kKind = "modulation.phaser";

// Stage-count choice index for 4, 6 and 8 stages.
double stageChoice(int stages) { return (stages - 4) / 2; }

// The phaser alone, fully wet, held still (no depth) with its stages broken
// at `centerHz`: its impulse response.
std::vector<float> staticResponse(int stages, double centerHz, double feedback = 0.0,
                                  double seconds = 0.4) {
  EffectRig rig(kKind, {{"depth", 0.0}, {"centerHz", centerHz}, {"stages", stageChoice(stages)},
                        {"feedback", feedback}, {"mix", 1.0}});
  return captureImpulseResponse(rig, seconds);
}

// Notch k (1 to N / 2) of N first-order allpass stages broken at `centerHz`,
// added to their input: each stage has -2 atan(tan(w / 2) / t0) of phase
// (t0 = tan(w0 / 2) at the break), and the sum cancels where the chain has
// -(2k - 1) pi in total.
double notchHz(int stages, double centerHz, int k) {
  const double t0 = std::tan(kPi * centerHz / kRate);
  return kRate / kPi *
         std::atan(t0 * std::tan((2.0 * k - 1.0) * kPi / (2.0 * stages)));
}

// Rate of the notch sweep, read off the level of a probe tone that sits
// between the lowest notch and the peak above it: at 10% depth the notch
// moves 15% either way and never crosses the probe, so its level rises and
// falls once per cycle.
double measuredRate(Params params, double bpm = 120.0) {
  params.emplace_back("mix", 1.0);
  params.emplace_back("depth", 10.0);
  params.emplace_back("centerHz", 1000.0);
  params.emplace_back("stages", 0.0);
  params.emplace_back("feedback", 0.0);
  RigOptions options;
  options.bpm = bpm;
  EffectRig rig(kKind, params, options);
  const std::vector<float> out =
      rig.render(sine(8.0, 1.25 * notchHz(4, 1000.0, 1), 0.25f, kRate));
  return amplitudeModulationRateHz(out, kRate, static_cast<size_t>(samples(0.5)));
}

}  // namespace

TEST(PhaserTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"depth", 80.0}, {"rateHz", 1.0}, {"feedback", 50.0}, {"stages", 1.0}};
  options.stateParams = {{"sync", 1.0},        {"division", fx::kDivEighth},
                         {"rateHz", 2.0},      {"depth", 30.0},
                         {"centerHz", 1500.0}, {"stages", 2.0},
                         {"feedback", -40.0},  {"width", 20.0}};
  options.cpuAverageBudgetNs = 12000.0;
  // About 50x the average: one block in thousands can be descheduled on a
  // busy machine, and that says nothing about the effect.
  options.cpuWorstBudgetNs = 150000.0;
  expectKindConforms("modulation.phaser", options);
}

TEST(PhaserTest, RegisteredAsACrossfadeWithTheAllpassedCopyInTheWet) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kKind);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->category, BlockCategory::Modulation);
  EXPECT_EQ(info->mixLaw, BlockMixLaw::Crossfade);
  EffectRig rig(kKind, {{"mix", 1.0}});
  EXPECT_EQ(onsetIndex(captureImpulseResponse(rig, 0.1)), 0);
}

TEST(PhaserTest, HasOneNotchForEveryTwoStagesAtAFrozenPhase) {
  for (const int stages : {4, 6, 8}) {
    SCOPED_TRACE(stages);
    const std::vector<float> ir = staticResponse(stages, 1000.0);
    const std::vector<double> notches = responseNotches(ir, 30.0, 20000.0, 5.0, 20.0);
    ASSERT_EQ(notches.size(), static_cast<size_t>(stages / 2));
    for (int k = 1; k <= stages / 2; ++k) {
      const double expected = notchHz(stages, 1000.0, k);
      EXPECT_NEAR(notches[static_cast<size_t>(k - 1)], expected, 0.03 * expected + 5.0) << k;
    }
  }
}

TEST(PhaserTest, CenterMovesTheNotches) {
  const auto firstNotch = [](double centerHz) {
    const std::vector<double> notches =
        responseNotches(staticResponse(4, centerHz), 30.0, 20000.0, 5.0, 20.0);
    return notches.empty() ? 0.0 : notches.front();
  };
  const double low = firstNotch(500.0);
  const double high = firstNotch(2000.0);
  ASSERT_GT(low, 0.0);
  EXPECT_NEAR(high / low, 4.0, 0.4);
  EXPECT_NEAR(low, notchHz(4, 500.0, 1), 0.03 * low + 5.0);
}

TEST(PhaserTest, StaticResponseIsTheSumOfTheInputAndTheAllpassChain) {
  // |1 + chain| / sqrt(2) at a few frequencies away from the notches.
  const std::vector<float> ir = staticResponse(6, 800.0);
  const double t0 = std::tan(kPi * 800.0 / kRate);
  const std::vector<double> freqs = {120.0, 350.0, 1400.0, 2600.0, 5500.0, 11000.0};
  const std::vector<double> measured = magnitudeResponseDb(ir, freqs, kRate);
  for (size_t i = 0; i < freqs.size(); ++i) {
    const double x = std::tan(kPi * freqs[i] / kRate) / t0;
    const double expected = 0.70710678 * 2.0 * std::abs(std::cos(6.0 * std::atan(x)));
    EXPECT_NEAR(measured[i], toDb(expected), 0.3) << freqs[i];
  }
}

TEST(PhaserTest, SweepRunsAtTheRateParameter) {
  for (const double rateHz : {1.0, 3.0}) {
    SCOPED_TRACE(rateHz);
    EXPECT_NEAR(measuredRate({{"rateHz", rateHz}}), rateHz, 0.03 * rateHz);
  }
}

TEST(PhaserTest, SyncedRateFollowsTheTempoAtTwoTempos) {
  struct Case {
    double bpm;
    int division;
    double expectedHz;
  };
  for (const Case& c : {Case{120.0, fx::kDivQuarter, 2.0}, Case{90.0, fx::kDivQuarter, 1.5},
                        Case{90.0, fx::kDivEighth, 3.0}}) {
    SCOPED_TRACE(c.bpm);
    EXPECT_NEAR(measuredRate({{"sync", 1.0}, {"division", static_cast<double>(c.division)},
                              {"rateHz", 7.0}},
                             c.bpm),
                c.expectedHz, 0.03 * c.expectedHz);
  }
}

TEST(PhaserTest, DepthZeroIsAStaticFilterWhateverTheRate) {
  const std::vector<float> in = noise(2.0, 5u, 0.25f, kRate);
  const auto render = [&](double rateHz) {
    DirectKind kind(kKind, {{"depth", 0.0}, {"feedback", 40.0}, {"rateHz", rateHz}});
    return kind.render(in);
  };
  EXPECT_LT(maxDifference(render(0.2), render(9.0)), 1.0e-6f);
}

TEST(PhaserTest, FeedbackSharpensTheResonanceAndNegativeFeedbackReversesIt) {
  // Four stages broken at 1 kHz: the chain is a full turn behind there, in
  // phase with the input, which is where the wet peaks. Positive feedback
  // narrows that peak (it rises 8 dB over its own shoulder at 1.25 kHz,
  // against 1 dB without feedback); negative feedback takes the peak away.
  const auto level = [](double feedback, double hz) {
    return magnitudeResponseDb(staticResponse(4, 1000.0, feedback), {hz}, kRate).front();
  };
  const auto contrast = [&](double feedback) { return level(feedback, 1000.0) - level(feedback, 1250.0); };
  EXPECT_GT(contrast(70.0), contrast(0.0) + 5.0);
  EXPECT_LT(level(-70.0, 1000.0), level(0.0, 1000.0) - 3.0);
  EXPECT_LT(contrast(-70.0), 1.0);
}

TEST(PhaserTest, ChangingTheStageCountDoesNotClick) {
  EffectRig rig(kKind, {{"depth", 0.0}, {"feedback", 30.0}, {"centerHz", 1000.0}, {"mix", 1.0}});
  const std::vector<float> in = sine(2.0, 220.0, 0.25f, kRate);
  std::vector<float> out = rig.render(std::vector<float>(in.begin(), in.begin() + samples(1.0)));
  rig.set("stages", 2.0);
  const std::vector<float> after = rig.render(std::vector<float>(in.begin() + samples(1.0), in.end()));
  out.insert(out.end(), after.begin(), after.end());
  const float steady = maxStep(out, static_cast<size_t>(samples(0.5)), static_cast<size_t>(samples(1.0)));
  EXPECT_GT(steady, 0.001f);
  EXPECT_LT(maxStep(out, static_cast<size_t>(samples(1.0)), out.size()), steady * 1.5f + 0.001f);
  // And the 8-stage sound is what comes out in the end.
  EffectRig eight(kKind, {{"depth", 0.0}, {"feedback", 30.0}, {"centerHz", 1000.0},
                          {"stages", 2.0}, {"mix", 1.0}});
  const std::vector<float> reference = eight.render(in);
  EXPECT_NEAR(rmsOf(out, static_cast<size_t>(samples(1.6))),
              rmsOf(reference, static_cast<size_t>(samples(1.6))), 1.0e-3);
}

TEST(PhaserTest, TheSweepIsSmooth) {
  // A 300 Hz sine through the fastest, widest sweep: the output moves no
  // faster than the sine and its swinging level explain. A coefficient that
  // jumped would show as a step many times larger.
  DirectKind kind(kKind, {{"depth", 100.0}, {"rateHz", 10.0}, {"feedback", 0.0}});
  const std::vector<float> out = kind.render(sine(3.0, 300.0, 0.25f, kRate));
  EXPECT_LT(maxStep(out, static_cast<size_t>(samples(0.2)), out.size()), 0.04f);
  EXPECT_GT(peakOf(out), 0.2f);
}

TEST(PhaserTest, TailCoversTheRingOfTheChainAndTheLoop) {
  for (const double feedback : {0.0, 85.0, -85.0}) {
    SCOPED_TRACE(feedback);
    const TailMeasurement tail = expectSilenceAfterReportedTail(
        kKind, {{"depth", 0.0}, {"centerHz", 200.0}, {"stages", 2.0}, {"feedback", feedback}});
    EXPECT_FALSE(tail.capped);
    EXPECT_GT(tail.measuredSeconds, 0.01);
  }
}

TEST(PhaserTest, SidesDifferOnlyInATrueStereoImage) {
  const std::vector<float> in = noise(2.0, 3u, 0.25f, kRate);
  {
    EffectRig rig(kKind, {{"width", 100.0}, {"depth", 100.0}, {"rateHz", 2.0}, {"mix", 1.0}});
    const Stereo out = rig.renderBoth(in);
    EXPECT_GT(relativeDifferenceDb(out.left, out.right, static_cast<size_t>(samples(0.2))), -12.0)
        << "no stereo spread at full width";
  }
  {
    EffectRig rig(kKind, {{"width", 0.0}, {"mix", 1.0}});
    const Stereo out = rig.renderBoth(in);
    EXPECT_LT(maxDifference(out.left, out.right), 1.0e-6f) << "width 0 must be a mono phaser";
  }
  {
    EffectRig rig(kKind, {{"width", 100.0}, {"mix", 1.0}}, rigFor(Layout::DualMono));
    const Stereo out = rig.renderBoth(in);
    EXPECT_LT(maxDifference(out.left, out.right), 1.0e-6f);
  }
  {
    DirectKind kind(kKind, {{"width", 100.0}}, Layout::Stereo);
    kind.ctx.nam.lastNamIndex = 3;
    const Stereo out = kind.render(Stereo{in, in});
    EXPECT_LT(maxDifference(out.left, out.right), 1.0e-6f) << "stereo before an amp is lost";
  }
}

TEST(PhaserTest, StaysFiniteAndBoundedAtMaximumFeedbackWithAHotInput) {
  for (const double feedback : {90.0, -90.0})
    for (const double centerHz : {100.0, 4000.0}) {
      SCOPED_TRACE(feedback);
      SCOPED_TRACE(centerHz);
      DirectKind kind(kKind,
                      {{"feedback", feedback}, {"centerHz", centerHz}, {"depth", 100.0},
                       {"stages", 2.0}, {"rateHz", 10.0}, {"width", 100.0}},
                      Layout::Stereo);
      const std::vector<float> hot = noise(20.0, 31u, 1.0f, kRate);
      const std::vector<float> other = noise(20.0, 32u, 1.0f, kRate);
      const Stereo out = kind.render(Stereo{hot, other});
      expectCleanSignal(out.left, 4.0f, "hot phaser, left");
      expectCleanSignal(out.right, 4.0f, "hot phaser, right");
      EXPECT_GT(rmsOf(out.left), 0.1);
    }
}

// ---------------------------------------------------------------------------
// The Lifted voicing: the response measured on a reference unit. At the 50%
// mix it is made for, the output is the dry signal plus 0.9 of the allpass
// chain, which lifts by up to 5.6 dB rather than dipping.
// ---------------------------------------------------------------------------

namespace {

std::vector<float> liftedResponse(int stages, double centerHz, double feedback = 0.0) {
  EffectRig rig(kKind, {{"depth", 0.0}, {"centerHz", centerHz}, {"stages", stageChoice(stages)},
                        {"feedback", feedback}, {"voicing", 1.0}, {"mix", 0.5}});
  return captureImpulseResponse(rig, 0.4);
}

// Level of `out` against `in` in a sixth-octave either side of each centre,
// summed over many bins, in dB.
std::vector<double> bandLevelsDb(const std::vector<float>& out, const std::vector<float>& in,
                                 const std::vector<double>& centres) {
  const size_t skip = static_cast<size_t>(samples(0.5));
  std::vector<double> levels;
  for (const double c : centres) {
    double powerOut = 0.0, powerIn = 0.0;
    for (double f = c * std::pow(2.0, -1.0 / 6.0); f < c * std::pow(2.0, 1.0 / 6.0); f *= 1.02) {
      powerOut += goertzelPower(out.data() + skip, out.size() - skip, f, kRate);
      powerIn += goertzelPower(in.data() + skip, in.size() - skip, f, kRate);
    }
    levels.push_back(10.0 * std::log10(powerOut / powerIn));
  }
  return levels;
}

}  // namespace

TEST(PhaserTest, ClassicIsTheDefaultVoicing) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kKind);
  ASSERT_NE(info, nullptr);
  const int voicing = findBlockParam(info->params, "voicing");
  ASSERT_GE(voicing, 0);
  EXPECT_EQ(info->params[static_cast<size_t>(voicing)].defaultValue, 0.0f);
  EXPECT_EQ(info->mixLaw, BlockMixLaw::Crossfade);
  EXPECT_FLOAT_EQ(info->defaultMix, 1.0f);
}

TEST(PhaserTest, LiftedStaticResponseIsTheDryPlusNineTenthsOfTheChain) {
  const std::vector<float> ir = liftedResponse(6, 800.0);
  const double t0 = std::tan(kPi * 800.0 / kRate);
  const std::vector<double> freqs = {120.0, 350.0, 1400.0, 2600.0, 5500.0, 11000.0};
  const std::vector<double> measured = magnitudeResponseDb(ir, freqs, kRate);
  for (size_t i = 0; i < freqs.size(); ++i) {
    const double x = std::tan(kPi * freqs[i] / kRate) / t0;
    const double expected = std::sqrt(1.81 + 1.8 * std::cos(12.0 * std::atan(x)));
    EXPECT_NEAR(measured[i], toDb(expected), 0.4) << freqs[i];
  }
}

TEST(PhaserTest, LiftedStillHasOneNotchForEveryTwoStages) {
  for (const int stages : {4, 6, 8}) {
    SCOPED_TRACE(stages);
    const std::vector<double> notches =
        responseNotches(liftedResponse(stages, 1000.0), 30.0, 20000.0, 5.0, 12.0);
    ASSERT_EQ(notches.size(), static_cast<size_t>(stages / 2));
    for (int k = 1; k <= stages / 2; ++k) {
      const double expected = notchHz(stages, 1000.0, k);
      EXPECT_NEAR(notches[static_cast<size_t>(k - 1)], expected, 0.03 * expected + 5.0) << k;
    }
  }
}

TEST(PhaserTest, LiftedNeverLiftsByMoreThanSixDecibelsWhateverTheFeedback) {
  // The loop is scaled so the wet never exceeds the dry: dry plus wet is at
  // most +5.6 dB, positive or negative feedback.
  for (const double feedback : {-90.0, -40.0, 0.0, 40.0, 90.0}) {
    SCOPED_TRACE(feedback);
    const std::vector<float> ir = liftedResponse(4, 1000.0, feedback);
    std::vector<double> freqs;
    for (double f = 40.0; f < 20000.0; f *= 1.05)
      freqs.push_back(f);
    for (const double level : magnitudeResponseDb(ir, freqs, kRate))
      EXPECT_LT(level, 5.8);
  }
}

TEST(PhaserTest, LiftedDefaultResponseLiftsAcrossTheBandInsteadOfDipping) {
  // Long-term level of noise at depth 50% and mix 50%. (A reference unit
  // measures +1.5 to +5.7 dB from 50 Hz to 16 kHz; the classic voicing dips
  // to -5 dB between 500 Hz and 2 kHz.) Measured here: +4.6 dB at 31.5 Hz,
  // -1.3 at 250 Hz, +4.1 at 16 kHz.
  const std::vector<double> centres = {31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0};
  const std::vector<float> in = noise(30.0, 5u, 0.25f, kRate);
  EffectRig rig(kKind, {{"depth", 50.0}, {"mix", 0.5}, {"width", 0.0}, {"voicing", 1.0}});
  const Stereo out = rig.renderBoth(in);
  const std::vector<double> left = bandLevelsDb(out.left, in, centres);
  std::cout << "[  PHASER  ] lifted band levels dB at 31.5 63 125 250 500 1k 2k 4k 8k 16k:";
  for (const double v : left)
    std::cout << " " << juce::String(v, 1);
  std::cout << std::endl;
  for (size_t i = 0; i < centres.size(); ++i) {
    EXPECT_GT(left[i], -2.0) << centres[i];
    EXPECT_LT(left[i], 6.2) << centres[i];
  }
  for (const size_t i : {size_t{0}, size_t{1}, size_t{8}, size_t{9}})
    EXPECT_GT(left[i], 3.0) << centres[i];
}

TEST(PhaserTest, LiftedSidesAgreeAtTheExtremesOfTheBand) {
  // The width moves the right side's sweep behind the left's, which changes
  // where the notches are at a moment but not the long-term level where the
  // chain is in phase with the dry all the time.
  const std::vector<double> centres = {31.5, 63.0, 16000.0};
  const std::vector<float> in = noise(30.0, 5u, 0.25f, kRate);
  EffectRig rig(kKind, {{"depth", 50.0}, {"mix", 0.5}, {"width", 50.0}, {"voicing", 1.0}});
  const Stereo out = rig.renderBoth(in);
  const std::vector<double> left = bandLevelsDb(out.left, in, centres);
  const std::vector<double> right = bandLevelsDb(out.right, in, centres);
  for (size_t i = 0; i < centres.size(); ++i)
    EXPECT_NEAR(left[i], right[i], 1.5) << centres[i];
}

TEST(PhaserTest, LiftedConforms) {
  ConformanceOptions options;
  options.params = {{"depth", 80.0}, {"rateHz", 1.0}, {"feedback", 50.0}, {"stages", 1.0},
                    {"voicing", 1.0}};
  options.cpuAverageBudgetNs = 12000.0;
  options.cpuWorstBudgetNs = 150000.0;
  expectKindConforms("modulation.phaser", options);
}

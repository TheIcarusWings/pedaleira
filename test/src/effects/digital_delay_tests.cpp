// delay.digital (plugin/src/effects/delay/DigitalDelay.cpp): the conformance
// test, then what the effect is for: an exact delay time (free and synced),
// clean repeats at the feedback level, filters that wear the repeats down,
// modulation that is off at depth 0 and moves the pitch at the set rate when
// it is on, a time change that never steps, a loop that cannot run away, and
// a tail that rings out after a bypass.
#include "effects/delay_test_support.h"
#include "effects/dsp/TempoSync.h"

#include <gtest/gtest.h>

using namespace delaytest;

namespace {

constexpr const char* kKind = "delay.digital";

// A setting where nothing but the delay is in play.
const Params kClean = {{"timeMs", 250.0}, {"feedback", 60.0}, {"lowCutHz", 20.0},
                       {"highCutHz", 20000.0}, {"modDepth", 0.0}, {"mix", 1.0}};

}  // namespace

TEST(DigitalDelayTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"timeMs", 120.0}, {"feedback", 40.0}, {"modDepth", 40.0}};
  options.stateParams = {{"sync", 1.0},          {"division", fx::kDivEighthDotted},
                         {"timeMs", 123.0},      {"feedback", 55.0},
                         {"lowCutHz", 150.0},    {"highCutHz", 5000.0},
                         {"modDepth", 30.0},     {"modRateHz", 2.0},
                         {"spillover", 0.0}};
  // The wet path is the echo alone: it starts a delay time after the input.
  options.latency = LatencyCheck::AtMostOnset;
  options.cpuAverageBudgetNs = 6000.0;
  options.cpuWorstBudgetNs = 50000.0;
  expectKindConforms("delay.digital", options);
}

TEST(DigitalDelayTest, DelayTimeMatchesTheParameter) {
  for (const double timeMs : {25.0, 250.0, 1234.0}) {
    SCOPED_TRACE(timeMs);
    // The impulse comes back whole: one sample, at the set time.
    EffectRig rig(kKind, withParams(kClean, {{"timeMs", timeMs}}));
    EffectRig reference{juce::String()};
    const int origin = peakIndexOf(captureImpulseResponse(reference, 0.05));
    const std::vector<float> ir = captureImpulseResponse(rig, 3.0);
    EXPECT_NEAR(peakIndexOf(ir) - origin, ms(timeMs), 1);

    // The same answer from noise, by cross-correlation.
    EXPECT_NEAR(measuredDelaySamples(kKind, withParams(kClean, {{"timeMs", timeMs}}), ms(timeMs)),
                ms(timeMs), 1);
  }
}

TEST(DigitalDelayTest, SyncedTimeFollowsTheTempoAtTwoTempos) {
  struct Case {
    double bpm;
    int division;
    double expectedMs;
  };
  // An eighth at 100 bpm and a dotted eighth at 150 are both 300 ms; a quarter
  // at 150 is 400.
  for (const Case& c : {Case{100.0, fx::kDivEighth, 300.0}, Case{150.0, fx::kDivEighthDotted, 300.0},
                        Case{150.0, fx::kDivQuarter, 400.0}}) {
    SCOPED_TRACE(c.bpm);
    RigOptions options;
    options.bpm = c.bpm;
    const Params params = withParams(kClean, {{"sync", 1.0},
                                              {"division", static_cast<double>(c.division)},
                                              {"timeMs", 77.0}});
    EXPECT_NEAR(measuredDelaySamples(kKind, params, ms(c.expectedMs), options), ms(c.expectedMs), 1);
    EffectRig rig(kKind, withParams(params, {{"feedback", 0.0}}), options);
    EXPECT_NEAR(peakIndexOf(captureImpulseResponse(rig, 1.0)), ms(c.expectedMs), 1);
  }

  // A tempo change on a running rig moves the time, and the reported tail
  // with it once the rig has seen the new tempo.
  EffectRig rig(kKind, {{"sync", 1.0}, {"division", static_cast<double>(fx::kDivQuarter)},
                        {"feedback", 0.0}, {"mix", 1.0}});
  rig.renderSilence(4800);
  const double tailAt120 = rig.tailSeconds();
  rig.setTempo(60.0);
  rig.renderSilence(48000);  // the crossfade to the new time
  EXPECT_NEAR(peakIndexOf(rig.render(impulse(96000))), ms(1000.0), 1);
  EXPECT_NEAR(rig.tailSeconds() - tailAt120, 0.5, 1e-6);
}

TEST(DigitalDelayTest, RepeatsFallByTheFeedbackSetting) {
  for (const double feedback : {30.0, 60.0, 85.0}) {
    SCOPED_TRACE(feedback);
    EffectRig rig(kKind, withParams(kClean, {{"feedback", feedback}}));
    // 40 ms tone bursts 250 ms apart: window k holds repeat k.
    const std::vector<float> out = rig.render(toneBurst(0.04, 440.0, 0.25f, 3.0));
    const double first = rmsWindow(out, 250.0, 60.0);
    EXPECT_GT(first, 0.01);
    double previous = first;
    for (int k = 2; k <= 4; ++k) {
      const double level = rmsWindow(out, 250.0 * k, 60.0);
      EXPECT_NEAR(level / previous, feedback * 0.01, 0.02) << "repeat " << k;
      previous = level;
    }
    // The first repeat is the input at unity.
    EXPECT_NEAR(first, rmsWindow(toneBurst(0.04, 440.0, 0.25f, 3.0), 0.0, 60.0), 0.002);
  }
}

TEST(DigitalDelayTest, TheFeedbackFiltersWearDownTheRepeats) {
  // Low cut: the bass of a noise burst drops out of the repeats pass by pass,
  // while the first echo (outside the loop) is clean.
  {
    EffectRig rig(kKind, withParams(kClean, {{"timeMs", 200.0}, {"feedback", 80.0},
                                             {"lowCutHz", 1000.0}}));
    const std::vector<float> out = rig.render(noiseBurst(0.08, 8u, 0.4f, 1.5));
    double previous = bandPower(out, static_cast<size_t>(ms(200.0)), 100.0, 400.0);
    EXPECT_GT(previous, 0.0);
    for (int k = 2; k <= 4; ++k) {
      const double bass = bandPower(out, static_cast<size_t>(ms(200.0 * k)), 100.0, 400.0);
      // A one-pole high-pass at 1 kHz takes 10 dB or more off a 100-400 Hz
      // band per pass, on top of the 80% feedback.
      EXPECT_LT(bass, previous * 0.2) << "repeat " << k;
      previous = bass;
    }
    // The treble is hardly touched by it: the same run with the low cut at
    // its minimum loses almost the same share of 3-5 kHz between repeats.
    EffectRig wide(kKind, withParams(kClean, {{"timeMs", 200.0}, {"feedback", 80.0}}));
    const std::vector<float> reference = wide.render(noiseBurst(0.08, 8u, 0.4f, 1.5));
    const auto trebleShare = [](const std::vector<float>& x) {
      return bandPower(x, static_cast<size_t>(ms(600.0)), 3000.0, 5000.0) /
             bandPower(x, static_cast<size_t>(ms(200.0)), 3000.0, 5000.0);
    };
    EXPECT_GT(trebleShare(out) / trebleShare(reference), 0.6);
    EXPECT_LT(trebleShare(out) / trebleShare(reference), 1.05);
  }
  // High cut: the same for the top end, and the centroid falls.
  {
    EffectRig rig(kKind, withParams(kClean, {{"timeMs", 200.0}, {"feedback", 80.0},
                                             {"highCutHz", 3000.0}}));
    const std::vector<float> out = rig.render(noiseBurst(0.08, 8u, 0.4f, 1.5));
    double previous = spectralCentroidHz(out, static_cast<size_t>(ms(200.0)));
    for (int k = 2; k <= 4; ++k) {
      const double centroid = spectralCentroidHz(out, static_cast<size_t>(ms(200.0 * k)));
      EXPECT_LT(centroid, previous * 0.85) << "repeat " << k;
      previous = centroid;
    }
  }
}

TEST(DigitalDelayTest, ModulationIsOffAtDepthZeroAndMovesThePitchAtTheSetRate) {
  // A steady 800 Hz sine, one echo (no feedback): the output is the echo.
  const std::vector<float> in = sine(8.0, 800.0, 0.25f, kRate);
  const size_t skip = static_cast<size_t>(ms(600.0));

  EffectRig still(kKind, withParams(kClean, {{"timeMs", 200.0}, {"feedback", 0.0},
                                             {"modDepth", 0.0}, {"modRateHz", 3.0}}));
  const PitchModulation none = analysePitchModulation(still.render(in), skip, 0.5, 12.0);
  EXPECT_NEAR(none.carrierHz, 800.0, 0.5);
  EXPECT_LT(none.worstDeviation, 1.0e-4) << "the pitch moves with the depth at zero";

  for (const double rate : {1.0, 3.0, 6.0}) {
    SCOPED_TRACE(rate);
    EffectRig moving(kKind, withParams(kClean, {{"timeMs", 200.0}, {"feedback", 0.0},
                                                {"modDepth", 100.0}, {"modRateHz", rate}}));
    const PitchModulation m = analysePitchModulation(moving.render(in), skip, 0.5, 12.0);
    EXPECT_NEAR(m.rateHz, rate, 0.1 + 0.05 * rate);
    // +-2 ms of delay at `rate` Hz is a pitch swing of 2 pi rate 0.002.
    EXPECT_NEAR(m.deviation, 2.0 * kPi * rate * 0.002, 0.15 * 2.0 * kPi * rate * 0.002);
  }

  // Half the depth, half the swing.
  EffectRig half(kKind, withParams(kClean, {{"timeMs", 200.0}, {"feedback", 0.0},
                                            {"modDepth", 50.0}, {"modRateHz", 3.0}}));
  EXPECT_NEAR(analysePitchModulation(half.render(in), skip, 0.5, 12.0).deviation,
              kPi * 3.0 * 0.002, 0.15 * kPi * 3.0 * 0.002);
}

TEST(DigitalDelayTest, ModulationDiffersBetweenTheSidesOnlyInTrueStereo) {
  const std::vector<float> in = sine(4.0, 800.0, 0.25f, kRate);
  const Params params = withParams(kClean, {{"timeMs", 200.0}, {"feedback", 0.0},
                                            {"modDepth", 100.0}, {"modRateHz", 2.0}});
  EffectRig stereo(kKind, params);
  const Stereo s = stereo.renderBoth(in);
  EXPECT_GT(rmsOf([&] {
              std::vector<float> d(s.size());
              for (size_t i = 0; i < d.size(); ++i)
                d[i] = s.left[i] - s.right[i];
              return d;
            }(), static_cast<size_t>(ms(500.0))),
            0.05)
      << "the two sides move together";

  EffectRig dual(kKind, params, rigFor(Layout::DualMono));
  const Stereo d = dual.renderBoth(in);
  EXPECT_EQ(d.left, d.right) << "dual mono must treat both channels alike";
}

TEST(DigitalDelayTest, ATimeChangeWhileAudioRunsNeverSteps) {
  // A 220 Hz sine at 0.25 moves about 0.007 per sample; with the repeats on
  // top it moves a little more. The crossfade adds a weight change of
  // 1 / 1920 per sample times the level, so the bound is the steady slope
  // plus a hair, not a fraction of the level.
  for (const double to : {400.0, 90.0}) {
    SCOPED_TRACE(to);
    EffectRig rig(kKind, withParams(kClean, {{"timeMs", 250.0}, {"feedback", 50.0}}));
    const std::vector<float> in = sine(5.0, 220.0, 0.25f, kRate);
    std::vector<float> out = rig.render(std::vector<float>(in.begin(), in.begin() + ms(2000.0)));
    const float steady = maxStep(out, static_cast<size_t>(ms(1000.0)), out.size());
    rig.set("timeMs", to);
    const std::vector<float> after =
        rig.render(std::vector<float>(in.begin() + ms(2000.0), in.end()));
    out.insert(out.end(), after.begin(), after.end());
    expectCleanSignal(out, 2.0f, "time change");
    EXPECT_LT(maxStep(out, static_cast<size_t>(ms(1900.0)), out.size()), steady * 1.5f + 0.004f)
        << "the change of time stepped the waveform";
    // And it arrives: an impulse, long after, comes back at the new time.
    rig.set("feedback", 0.0);
    rig.renderSilence(ms(4000.0));
    EXPECT_NEAR(peakIndexOf(rig.render(impulse(ms(1500.0)))), ms(to), 1);
  }
}

TEST(DigitalDelayTest, OutputStaysBoundedAtMaximumFeedbackWithAHotInput) {
  expectBoundedAtMaximumFeedback(kKind, {{"timeMs", 90.0}, {"modDepth", 60.0}}, 1.5f);
  expectBoundedAtMaximumFeedback(kKind, {{"timeMs", 1500.0}, {"lowCutHz", 20.0},
                                         {"highCutHz", 20000.0}}, 1.5f);
}

TEST(DigitalDelayTest, TailFollowsTimeFeedbackAndModulation) {
  EffectRig rig(kKind, {{"timeMs", 200.0}, {"feedback", 0.0}, {"highCutHz", 20000.0}});
  // No feedback: one echo, a delay time after the input (plus the fade).
  const double single = rig.tailSeconds();
  EXPECT_NEAR(single, 0.2 + 0.05, 0.003);
  // 50%: the tenth repeat after the first is 60 dB down (0.5^10 < 0.001).
  rig.set("feedback", 50.0);
  EXPECT_NEAR(rig.tailSeconds(), 0.2 * 11.0 + 0.05, 0.01);
  // Modulation lengthens each pass by up to its swing.
  const double plain = rig.tailSeconds();
  rig.set("modDepth", 100.0);
  EXPECT_GT(rig.tailSeconds(), plain);
  // A tail beyond the cap is reported at the cap.
  rig.set("timeMs", 2000.0);
  rig.set("feedback", 95.0);
  EXPECT_DOUBLE_EQ(rig.tailSeconds(), kMaxBlockTailSeconds);
}

TEST(DigitalDelayTest, SpilloverRingsOutAfterBypassAndStopsAtTheReportedTail) {
  expectSpilloverBehaviour(kKind, {{"timeMs", 200.0}, {"feedback", 70.0}, {"lowCutHz", 20.0},
                                   {"highCutHz", 20000.0}, {"modDepth", 0.0}},
                           350.0, 550.0);
}

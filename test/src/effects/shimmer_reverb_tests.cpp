// reverb.shimmer (plugin/src/effects/reverb/ShimmerReverb.cpp): the
// conformance test, then what a shimmer reverb is for, each as a
// measurement of the wet signal (see reverb_test_helpers.h).
#include "effects/reverb_test_helpers.h"

#include <gtest/gtest.h>

using namespace fxtest;
using namespace reverbtest;

namespace {

constexpr const char* kKind = "reverb.shimmer";

const Params kPlain = {{"shimmer", 0.0}, {"damping", 0.0}, {"preDelayMs", 0.0}};

// A 330 Hz note of 0.4 s into the reverb, then silence; returns the wet
// signal for `seconds`.
std::vector<float> noteThrough(const Params& params, double seconds) {
  DirectKind direct(kKind, params);
  std::vector<float> in = sine(0.4, 330.0, 0.5f, kRate);
  // A short fade at each end, so the note has no click of its own.
  for (int i = 0; i < 480; ++i) {
    in[static_cast<size_t>(i)] *= static_cast<float>(i) / 480.0f;
    in[in.size() - 1 - static_cast<size_t>(i)] *= static_cast<float>(i) / 480.0f;
  }
  return direct.render(withSilence(in, samples(seconds - 0.4)));
}

// Level in dB of the band around `hz` in a stretch of the wet signal.
double bandDb(const std::vector<float>& out, double hz, double from, double to) {
  return 20.0 * std::log10(std::max(1.0e-9, bandRms(out, hz, 6.0, static_cast<size_t>(samples(from)),
                                                    static_cast<size_t>(samples(to)))));
}

}  // namespace

TEST(ShimmerReverbTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"decayS", 1.5}, {"shimmer", 70.0}, {"preDelayMs", 10.0}};
  options.stateParams = {{"decayS", 8.5},     {"shimmer", 65.0},    {"interval", 1.0},
                         {"damping", 60.0},   {"preDelayMs", 45.0}, {"modulation", 80.0},
                         {"spillover", 0.0}};
  // The wet path is the reverberation only: it starts after the pre-delay
  // and the shortest line, which is the effect and not latency.
  options.latency = LatencyCheck::AtMostOnset;
  // Measured 11,000 to 12,500 ns on average and 28,000 to 37,000 ns for the
  // worst block (Apple silicon, with other builds running).
  options.cpuAverageBudgetNs = 36000.0;
  options.cpuWorstBudgetNs = 200000.0;
  expectKindConforms("reverb.shimmer", options);
}

TEST(ShimmerReverbTest, DecayParameterIsTheRt60WithTheShimmerOff) {
  expectRt60MatchesDecay(kKind, kPlain, {1.0, 5.0, 20.0});
  // With the shimmer up the tail is shorter, never longer: what climbs is
  // taken from below and ends in the damping.
  const std::vector<float> ir =
      wetImpulseResponse(kKind, with(kPlain, {{"decayS", 5.0}, {"shimmer", 100.0}}), 6.5).left;
  const double measured = estimateRt60Seconds(ir, kRate);
  std::cout << "[   RT60   ] " << kKind << ": decay 5 s at full shimmer, measured " << measured << " s"
            << std::endl;
  EXPECT_GT(measured, 0.0);
  EXPECT_LT(measured, 5.0);
}

TEST(ShimmerReverbTest, DampingShortensTheHighs) {
  const std::vector<float> damped =
      wetImpulseResponse(kKind, with(kPlain, {{"decayS", 4.0}, {"damping", 80.0}}), 5.5).left;
  const double low = bandRt60(damped, 200.0, 1000.0);
  const double high = bandRt60(damped, 6000.0, 9000.0);
  std::cout << "[ DAMPING  ] " << kKind << ": RT60 " << low << " s at 200 Hz to 1 kHz, " << high
            << " s at 6 to 9 kHz with the damping at 80%" << std::endl;
  EXPECT_NEAR(low, 4.0, 0.8);
  EXPECT_LT(high, 0.5 * low);
}

TEST(ShimmerReverbTest, PreDelayMatchesTheParameter) {
  expectPreDelayMatches(kKind, kPlain, {10.0, 62.5, 200.0});
}

TEST(ShimmerReverbTest, TheOctaveGrowsOutOfTheTail) {
  // A 330 Hz note of 0.4 s. Levels of the bands at the note, an octave up
  // (660 Hz) and two octaves up (1320 Hz): while the note sounds (0.1 to
  // 0.4 s), just after it (0.5 to 1 s) and well into the tail (2 to 3 s).
  const Params base = {{"decayS", 8.0}, {"modulation", 0.0}, {"preDelayMs", 0.0}};
  struct Levels {
    double note[3], octave[3], second[3];
  };
  const auto measure = [&](double shimmer) {
    const std::vector<float> out = noteThrough(with(base, {{"shimmer", shimmer}}), 3.2);
    const double windows[3][2] = {{0.1, 0.4}, {0.5, 1.0}, {2.0, 3.0}};
    Levels l{};
    for (int w = 0; w < 3; ++w) {
      l.note[w] = bandDb(out, 330.0, windows[w][0], windows[w][1]);
      l.octave[w] = bandDb(out, 660.0, windows[w][0], windows[w][1]);
      l.second[w] = bandDb(out, 1320.0, windows[w][0], windows[w][1]);
    }
    std::cout << "[ SHIMMER  ] " << kKind << " at " << shimmer << "%: 330 Hz "
              << juce::String(l.note[0], 1) << " / " << juce::String(l.note[1], 1) << " / "
              << juce::String(l.note[2], 1) << " dB, 660 Hz " << juce::String(l.octave[0], 1) << " / "
              << juce::String(l.octave[1], 1) << " / " << juce::String(l.octave[2], 1)
              << " dB, 1320 Hz " << juce::String(l.second[0], 1) << " / " << juce::String(l.second[1], 1)
              << " / " << juce::String(l.second[2], 1) << " dB (during / after / late)" << std::endl;
    return l;
  };
  const Levels off = measure(0.0), half = measure(40.0), full = measure(100.0);

  // Shimmer off: nothing an octave up. What the measurement shows there is
  // the skirt of its own filter, more than 30 dB under the note.
  for (int w = 0; w < 3; ++w)
    EXPECT_LT(off.octave[w], off.note[w] - 30.0);

  // Shimmer up: the octave is far above that floor from the start, and it
  // is louder after the note has stopped than while it sounded. Everything
  // else in a reverb only falls once the input is gone: this band rose.
  for (const Levels* l : {&half, &full}) {
    EXPECT_GT(l->octave[0], off.octave[0] + 15.0);
    EXPECT_GT(l->octave[1], l->octave[0] + 1.0);
    // The second octave comes out of the first, later still.
    EXPECT_GT(l->second[1], off.second[1] + 20.0);
    EXPECT_GT(l->second[1], l->second[0] + 3.0);
    // And the note itself gives way: it falls faster than without shimmer.
    EXPECT_LT(l->note[2] - l->note[1], off.note[2] - off.note[1] - 1.0);
  }
  // More shimmer, more octave, sooner; and at full shimmer the tail has
  // climbed on past the first octave three seconds in.
  EXPECT_GT(full.octave[0], half.octave[0] + 2.0);
  EXPECT_GT(full.second[2], full.octave[2] + 6.0);
}

TEST(ShimmerReverbTest, WhatClimbsIsTakenFromBelow) {
  // The shifters sit inside a passive loop: energy that goes up an octave
  // comes out of the band it left. A burst of low mid-band noise (a single
  // note would only show where it falls among the modes): the tail's total
  // energy is never higher for the shimmer, and the band the burst was in
  // empties faster.
  const Params base = {{"decayS", 8.0}, {"modulation", 0.0}, {"damping", 0.0}, {"preDelayMs", 0.0}};
  const std::vector<float> burst =
      withSilence(bandOf(noise(0.4, 71u, 0.5f, kRate), 200.0, 500.0), samples(4.0));
  const auto render = [&](double shimmer) {
    DirectKind direct(kKind, with(base, {{"shimmer", shimmer}}));
    return direct.render(burst);
  };
  const std::vector<float> off = render(0.0), half = render(40.0), full = render(100.0);
  const size_t from = static_cast<size_t>(samples(1.0));
  const auto db = [](double a, double b) { return 10.0 * std::log10(a / b); };
  std::cout << "[ SHIMMER  ] " << kKind << ": tail energy after 1 s against the shimmer off: "
            << db(energyOf(half, from), energyOf(off, from)) << " dB at 40%, "
            << db(energyOf(full, from), energyOf(off, from)) << " dB at 100%; in the burst's own band "
            << db(energyOf(bandOf(full, 200.0, 500.0), from), energyOf(bandOf(off, 200.0, 500.0), from))
            << " dB at 100%" << std::endl;
  EXPECT_LE(energyOf(half, from), energyOf(off, from) * 1.02);
  EXPECT_LE(energyOf(full, from), energyOf(off, from) * 1.02);
  EXPECT_LT(db(energyOf(bandOf(full, 200.0, 500.0), from), energyOf(bandOf(off, 200.0, 500.0), from)), -6.0);
  // And an octave up there is more than without it.
  EXPECT_GT(db(energyOf(bandOf(full, 800.0, 2000.0), from), energyOf(bandOf(off, 800.0, 2000.0), from)), 6.0);
}

TEST(ShimmerReverbTest, ReportedTailHoldsWithNothingDamped) {
  expectTailHolds(kKind, with(kPlain, {{"decayS", 4.0}, {"preDelayMs", 200.0}}));
  expectTailHolds(kKind, {{"decayS", 2.0}, {"shimmer", 100.0}, {"interval", 1.0}, {"damping", 0.0}});
}

TEST(ShimmerReverbTest, TheFifthAppearsOnlyInItsMode) {
  const Params base = {{"decayS", 8.0}, {"modulation", 0.0}, {"shimmer", 100.0}, {"preDelayMs", 0.0}};
  const std::vector<float> octave = noteThrough(with(base, {{"interval", 0.0}}), 2.0);
  const std::vector<float> fifth = noteThrough(with(base, {{"interval", 1.0}}), 2.0);
  const std::vector<float> off = noteThrough(with(base, {{"shimmer", 0.0}}), 2.0);
  // 495 Hz is a fifth over the 330 Hz note.
  const double inOctaveMode = bandDb(octave, 495.0, 0.5, 1.5);
  const double inFifthMode = bandDb(fifth, 495.0, 0.5, 1.5);
  std::cout << "[ SHIMMER  ] " << kKind << ": 495 Hz at " << inOctaveMode << " dB in octave mode, "
            << inFifthMode << " dB in fifth + octave mode; 660 Hz at " << bandDb(fifth, 660.0, 0.5, 1.5)
            << " dB in fifth + octave mode, " << bandDb(off, 660.0, 0.5, 1.5) << " dB with the shimmer off"
            << std::endl;
  EXPECT_GT(inFifthMode, inOctaveMode + 10.0);
  // The octave is still there beside it.
  EXPECT_GT(bandDb(fifth, 660.0, 0.5, 1.5), bandDb(off, 660.0, 0.5, 1.5) + 15.0);
}

TEST(ShimmerReverbTest, EchoDensityBuildsAndTheTailHasNoPitch) {
  const Params params = with(kPlain, {{"decayS", 4.0}});
  const Density d = echoDensity(kKind, params, 0.04, 0.15, 0.25, 0.6, 0.8);
  EXPECT_LT(d.middle, d.early);
  EXPECT_LT(d.late, 4.0);
  const std::vector<float> ir = wetImpulseResponse(kKind, with(params, {{"modulation", 0.0}}), 3.2).left;
  const Periodicity p = tailPeriodicity(ir, 0.6, 3.0, 4.0, 1.0, 400.0);
  std::cout << "[ PERIODIC ] " << kKind << ": autocorrelation peak " << p.peak << " at " << p.lagMs
            << " ms" << std::endl;
  EXPECT_LT(p.peak, 0.3);
}

TEST(ShimmerReverbTest, WideInTrueStereoMonoElsewhere) {
  expectWideInStereoOnly(kKind, with(kPlain, {{"decayS", 4.0}}), 0.5, 3.0);
  expectWideInStereoOnly(kKind, {{"decayS", 4.0}, {"shimmer", 100.0}}, 0.5, 3.0);
}

TEST(ShimmerReverbTest, StableAtFullShimmerAndTheLongestDecayWithHotInput) {
  // The case a shimmer built as a send from the output back to the input
  // runs away in. Both intervals, damping off (nothing to absorb what
  // climbs but the shifters' own low-pass), full-scale noise for 30 s.
  for (const double interval : {0.0, 1.0})
    expectBoundedWithHotInput(kKind,
                              {{"decayS", 20.0}, {"shimmer", 100.0}, {"interval", interval},
                               {"damping", 0.0}, {"modulation", 100.0}},
                              8.0f);
  // And a single low note held for 30 s, which is all correlation and no
  // noise.
  DirectKind direct(kKind, {{"decayS", 20.0}, {"shimmer", 100.0}, {"damping", 0.0}}, Layout::Stereo);
  const std::vector<float> note = sine(30.0, 82.41, 1.0f, kRate);
  const Stereo out = direct.render(Stereo{note, note});
  std::cout << "[   HOT    ] " << kKind << ": a full-scale low E for 30 s, wet peak "
            << std::max(peakOf(out.left), peakOf(out.right)) << std::endl;
  expectCleanSignal(out.left, 8.0f, "held note, left");
  expectCleanSignal(out.right, 8.0f, "held note, right");
}

TEST(ShimmerReverbTest, MovingTheControlsDoesNotZipper) {
  // Decay, damping, shimmer amount and modulation depth are slewed and
  // ramped per sample; the interval is taken up by each shifter tap at its
  // own seam.
  // Starting from full shimmer, so the octaves that make the waveform
  // steeper are in the steady measurement too.
  const ZipperResult r = sweepUnderAudio(
      kKind, {},
      {{"decayS", 1.0, 20.0}, {"damping", 0.0, 100.0}, {"shimmer", 100.0, 0.0}, {"modulation", 0.0, 100.0}});
  // The shifters themselves put a little above 6 kHz when fed a pure tone
  // (the top of the octave stack); moving the controls must not add to it.
  expectNoZipper(r, r.steadyHighDb + 6.0);
  const ZipperResult interval =
      sweepUnderAudio(kKind, {{"shimmer", 100.0}}, {{"interval", 0.0, 1.0}, {"preDelayMs", 0.0, 200.0}});
  expectNoZipper(interval, interval.steadyHighDb + 6.0);
}

TEST(ShimmerReverbTest, WetLevelFollowsTheCommonLaw) {
  for (const double decay : {2.0, 8.0}) {
    const double level = wetLevelDb(kKind, with(kPlain, {{"decayS", decay}}), decay * 1.5 + 1.0);
    const double law = 10.0 * std::log10(0.5 * std::sqrt(decay / 2.0));
    std::cout << "[  LEVEL   ] " << kKind << ": wet " << level << " dB at a decay of " << decay
              << " s (law " << law << " dB)" << std::endl;
    EXPECT_NEAR(level, law, 2.0);
  }
}

TEST(ShimmerReverbTest, TailRingsOutOnBypassOnlyWithSpilloverOn) {
  expectSpilloverSwitchWorks(kKind, {});
}

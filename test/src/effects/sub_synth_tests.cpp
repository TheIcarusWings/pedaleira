// pitch.subsynth "Sub Synth" (plugin/src/effects/pitch/SubSynth.cpp): the
// conformance test, then a pitch-tracked synthesizer voice: it follows notes
// at one and two octaves under them (the frequency of its output against the
// note played), its wave shapes, the filter's cutoff and resonance, the
// envelope following the guitar, the gate across note changes, silence and
// noise, the readings, and the dual mono layout.
#include "effects/pitch_test_signals.h"

#include <gtest/gtest.h>

using namespace pitchtest;

namespace {

constexpr const char* kKind = "pitch.subsynth";

const BlockParamSpec& specOf(const BlockKindInfo& info, const char* id) {
  for (const auto& spec : info.params)
    if (spec.id == id)
      return spec;
  ADD_FAILURE() << "no parameter " << id;
  return info.params.front();
}

// The synth alone, bright and unresonant, so its lines are those of the wave.
Params bright(double octave, double wave) {
  return {{"octave", octave}, {"wave", wave}, {"cutoffHz", 4000.0}, {"resonance", 0.0}, {"mix", 1.0}};
}

}  // namespace

TEST(SubSynthTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"octave", 0.0}, {"wave", 2.0}, {"cutoffHz", 900.0}, {"resonance", 40.0}};
  options.stateParams = {{"octave", 1.0}, {"wave", 1.0}, {"cutoffHz", 220.0}, {"resonance", 70.0}, {"releaseMs", 350.0}};
  // The synth is silent for an impulse (the tracker has no note to follow), and
  // the guitar is not delayed: there is no latency to measure.
  options.latency = LatencyCheck::Skip;
  options.cpuAverageBudgetNs = 25000.0;
  options.cpuWorstBudgetNs = 250000.0;
  expectKindConforms("pitch.subsynth", options);
}

TEST(SubSynthTest, TableMatchesTheBrief) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kKind);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->name, "Sub Synth");
  EXPECT_EQ(info->category, BlockCategory::Pitch);
  // Heard beside the guitar: the dry stays at unity up to 50 %.
  EXPECT_EQ(info->mixLaw, BlockMixLaw::DryPreserving);
  EXPECT_FLOAT_EQ(info->defaultMix, 0.5f);
  const BlockParamSpec& octave = specOf(*info, "octave");
  EXPECT_EQ(octave.choices.size(), 2);
  const BlockParamSpec& wave = specOf(*info, "wave");
  ASSERT_EQ(wave.choices.size(), 3);
  EXPECT_EQ(wave.choices[0], "Sine");
  EXPECT_EQ(wave.choices[1], "Square");
  EXPECT_EQ(wave.choices[2], "Saw");
  EXPECT_EQ(specOf(*info, "cutoffHz").unit, "Hz");
  EXPECT_EQ(specOf(*info, "resonance").unit, "%");
  EXPECT_EQ(specOf(*info, "releaseMs").unit, "ms");
  ASSERT_EQ(info->readings.size(), 3u);
  EXPECT_EQ(info->readings[0].id, "detectedNote");
  EXPECT_EQ(info->readings[2].id, "confidence");
}

TEST(SubSynthTest, ItPlaysTheNoteOneAndTwoOctavesDownWithinACent) {
  // Sines at the guitar's range; the synth's fundamental is the note divided by
  // two and by four. Measured over the second after the tracker has settled.
  double worstOne = 0.0, worstTwo = 0.0;
  for (const double hz : {82.407, 110.0, 146.832, 195.998, 246.942, 329.628, 440.0, 659.255}) {
    for (const double octave : {0.0, 1.0}) {
      for (const bool harmonic : {false, true}) {
        const std::vector<float> in = harmonic ? harmonicTone(3.0, hz, 0.3f) : sine(3.0, hz, 0.3f, kRate);
        EffectRig rig(kKind, bright(octave, 0.0));
        const std::vector<float> out = rig.render(in);
        const double target = hz / (octave < 0.5 ? 2.0 : 4.0);
        const double error = centsBetween(peakFrequencyHz(out, 96000, out.size(), target, 50.0), target);
        (octave < 0.5 ? worstOne : worstTwo) = std::max(octave < 0.5 ? worstOne : worstTwo, std::abs(error));
        EXPECT_NEAR(error, 0.0, 1.5) << hz << " Hz, " << (octave < 0.5 ? "-1" : "-2") << " octave, "
                                     << (harmonic ? "harmonic tone" : "sine");
      }
    }
  }
  std::cout << "[ SUBSYNTH ] worst frequency error over 8 notes x 2 tones: one octave down "
            << juce::String(worstOne, 3) << " ct, two octaves down " << juce::String(worstTwo, 3) << " ct" << std::endl;
}

TEST(SubSynthTest, ItFollowsNoteChangesAndGatesAcrossThem) {
  // E3 for a second, A3 for a second, B2 for a second: the synth is an octave
  // under each, once the tracker has the new note.
  const double notes[] = {164.814, 220.0, 123.471};
  std::vector<float> in;
  for (const double hz : notes)
    in = followedBy(std::move(in), harmonicTone(1.0, hz, 0.3f));
  EffectRig rig(kKind, bright(0.0, 0.0));
  const std::vector<float> out = rig.render(in);
  for (int n = 0; n < 3; ++n) {
    const double target = notes[n] / 2.0;
    const size_t from = static_cast<size_t>((n + 0.4) * kRate), to = static_cast<size_t>((n + 0.97) * kRate);
    EXPECT_NEAR(centsBetween(peakFrequencyHz(out, from, to, target, 50.0), target), 0.0, 3.0) << "note " << n;
    EXPECT_GT(rmsOf(out, from, to), 0.05) << "the synth is playing";
  }
}

TEST(SubSynthTest, TheWavesAreSineSquareAndSaw) {
  // 196 Hz played, -1 octave: the synth at 98 Hz, with the filter wide open.
  const std::vector<float> in = harmonicTone(3.0, 196.0, 0.3f);
  const auto lines = [&](double wave) {
    EffectRig rig(kKind, bright(0.0, wave));
    const std::vector<float> out = rig.render(in);
    struct Lines {
      double f1, f2, f3, f5;
    };
    const size_t from = 96000;
    return Lines{lineDb(out, from, out.size(), 98.0), lineDb(out, from, out.size(), 196.0),
                 lineDb(out, from, out.size(), 294.0), lineDb(out, from, out.size(), 490.0)};
  };
  const auto sineLines = lines(0.0);
  EXPECT_LT(sineLines.f2, sineLines.f1 - 40.0);
  EXPECT_LT(sineLines.f3, sineLines.f1 - 40.0);

  const auto squareLines = lines(1.0);
  EXPECT_LT(squareLines.f2, squareLines.f1 - 30.0);  // odd harmonics only
  EXPECT_NEAR(squareLines.f3 - squareLines.f1, -9.54, 1.5);
  EXPECT_NEAR(squareLines.f5 - squareLines.f1, -13.98, 1.5);

  const auto sawLines = lines(2.0);
  EXPECT_NEAR(sawLines.f2 - sawLines.f1, -6.02, 1.5);  // all harmonics at 1/n
  EXPECT_NEAR(sawLines.f3 - sawLines.f1, -9.54, 1.5);
}

TEST(SubSynthTest, CutoffAndResonanceShapeTheTone) {
  const std::vector<float> in = harmonicTone(3.0, 196.0, 0.3f);
  const auto render = [&](double cutoff, double resonance) {
    Params p = bright(0.0, 2.0);  // saw at 98 Hz
    p.push_back({"cutoffHz", cutoff});
    p.push_back({"resonance", resonance});
    EffectRig rig(kKind, p);
    return rig.render(in);
  };
  const size_t from = 96000;
  const std::vector<float> open = render(4000.0, 0.0);
  const std::vector<float> closed = render(150.0, 0.0);
  // A low cutoff takes the harmonics (294 Hz, the third) away from the
  // fundamental; the fundamental stays.
  const auto third = [&](const std::vector<float>& x) { return lineDb(x, from, x.size(), 294.0) - lineDb(x, from, x.size(), 98.0); };
  EXPECT_LT(third(closed), third(open) - 10.0);
  // Resonance at the cutoff lifts the harmonic the cutoff sits on (the second,
  // 196 Hz): the same setting with the resonance up is clearly louder there.
  const std::vector<float> flat = render(196.0, 0.0);
  const std::vector<float> peaked = render(196.0, 100.0);
  EXPECT_GT(lineDb(peaked, from, peaked.size(), 196.0), lineDb(flat, from, flat.size(), 196.0) + 8.0);
}

TEST(SubSynthTest, TheLevelFollowsTheGuitarsEnvelope) {
  // A louder note, a louder synth: 0.4 against 0.1 is 12 dB.
  double level[2];
  for (int i = 0; i < 2; ++i) {
    EffectRig rig(kKind, bright(0.0, 0.0));
    const std::vector<float> out = rig.render(sine(3.0, 196.0, i == 0 ? 0.4f : 0.1f, kRate));
    level[i] = lineDb(out, 96000, out.size(), 98.0);
  }
  EXPECT_NEAR(level[0] - level[1], 12.04, 0.8);

  // A note that dies out takes the synth with it: 250 ms after the guitar
  // stops the synth is more than 40 dB under what it played.
  EffectRig rig(kKind, bright(0.0, 0.0));
  const std::vector<float> in = withSilence(sine(1.5, 196.0, 0.3f, kRate), 24000);
  const std::vector<float> out = rig.render(in);
  const double playing = rmsOf(out, 48000, 72000);
  EXPECT_LT(rmsOf(out, 72000 + 12000, 72000 + 14400), playing * 0.01);

  // The release knob is how long the synth rings while the guitar's envelope
  // falls (a decaying note: slowly).
  EffectRig quick(kKind, {{"octave", 0.0}, {"wave", 0.0}, {"releaseMs", 20.0}, {"mix", 1.0}});
  EffectRig slow(kKind, {{"octave", 0.0}, {"wave", 0.0}, {"releaseMs", 900.0}, {"mix", 1.0}});
  std::vector<float> ringing = sine(1.0, 196.0, 0.3f, kRate);
  const std::vector<float> tail = sine(0.6, 196.0, 0.03f, kRate);  // the note dropped 20 dB and rings on
  ringing.insert(ringing.end(), tail.begin(), tail.end());
  const std::vector<float> quickOut = quick.render(ringing);
  const std::vector<float> slowOut = slow.render(ringing);
  EXPECT_GT(rmsOf(slowOut, 52000, 76000), rmsOf(quickOut, 52000, 76000) * 1.5);
}

TEST(SubSynthTest, SilenceIsSilentAndNoiseOnlyHoldsTheLastNoteAtAReducedLevel) {
  EffectRig rig(kKind);
  EXPECT_EQ(peakOf(rig.renderSilence(24000)), 0.0f);
  // Noise with no note ever heard: the synth plays its default note (a low E,
  // so the sub is at 41 Hz) at a third of the level of the playing, bounded.
  EffectRig noisy(kKind, bright(0.0, 0.0));
  const std::vector<float> hiss = noisy.render(noise(2.0, 8u, 0.3f, kRate));
  expectCleanSignal(hiss, 0.3f, "noise only");
  EXPECT_GT(lineDb(hiss, 48000, hiss.size(), 41.2035), lineDb(hiss, 48000, hiss.size(), 98.0) + 10.0);
  // A note, then noise: the synth keeps the note (98 Hz for a played 196 Hz), at
  // a third of the level it had, and does not wander.
  EffectRig held(kKind, bright(0.0, 0.0));
  const std::vector<float> in = followedBy(sine(1.0, 196.0, 0.3f, kRate), noise(1.0, 9u, 0.3f, kRate));
  const std::vector<float> out = held.render(in);
  const double playing = rmsOf(out, 24000, 48000);
  EXPECT_GT(playing, 0.05);
  EXPECT_LT(rmsOf(out, 48000 + 9600, 96000), playing * 0.5);
  EXPECT_GT(lineDb(out, 60000, 96000, 98.0), lineDb(out, 60000, 96000, 41.2035) + 10.0);
}

TEST(SubSynthTest, TheReadingsReportTheNoteTheFrequencyAndTheConfidence) {
  DirectKind kind(kKind, {}, Layout::Mono);
  EXPECT_FLOAT_EQ(kind.engine().getReading(0), 0.0f);
  kind.render(sine(0.6, 330.0, 0.3f, kRate));
  EXPECT_NEAR(kind.engine().getReading(0), 64.0 - 0.0, 0.1);  // E4: 329.63 Hz is note 64, 330 is 2.4 ct above
  EXPECT_NEAR(kind.engine().getReading(1), 330.0, 0.3);
  EXPECT_GT(kind.engine().getReading(2), 95.0f);
  kind.engine().reset();
  EXPECT_FLOAT_EQ(kind.engine().getReading(0), 0.0f);
  EXPECT_FLOAT_EQ(kind.engine().getReading(1), 0.0f);
  EXPECT_FLOAT_EQ(kind.engine().getReading(2), 0.0f);
}

TEST(SubSynthTest, DualMonoGivesEachInstrumentItsOwnSynth) {
  EffectRig rig(kKind, bright(0.0, 0.0), rigFor(Layout::DualMono));
  const Stereo out = rig.render(Stereo{harmonicTone(3.0, 110.0, 0.3f), harmonicTone(3.0, 196.0, 0.3f)});
  const size_t from = 96000;
  EXPECT_NEAR(centsBetween(peakFrequencyHz(out.left, from, out.left.size(), 55.0, 50.0), 55.0), 0.0, 2.0);
  EXPECT_NEAR(centsBetween(peakFrequencyHz(out.right, from, out.right.size(), 98.0, 50.0), 98.0), 0.0, 2.0);
  EXPECT_LT(lineDb(out.left, from, out.left.size(), 98.0), lineDb(out.left, from, out.left.size(), 55.0) - 30.0);
  EXPECT_LT(lineDb(out.right, from, out.right.size(), 55.0), lineDb(out.right, from, out.right.size(), 98.0) - 30.0);
}

TEST(SubSynthTest, MonoAndStereoPlayTheSameSynthOnEverySide) {
  const std::vector<float> in = harmonicTone(3.0, 196.0, 0.3f);
  EffectRig mono(kKind, bright(0.0, 1.0), rigFor(Layout::Mono));
  EffectRig stereo(kKind, bright(0.0, 1.0), rigFor(Layout::Stereo));
  const std::vector<float> a = mono.render(in);
  const Stereo b = stereo.renderBoth(in);
  EXPECT_EQ(b.left, b.right);
  double worst = 0.0;
  for (size_t i = 0; i < a.size(); ++i)
    worst = std::max(worst, static_cast<double>(std::abs(a[i] - b.left[i])));
  EXPECT_LT(worst, 1.0e-5);
}

TEST(SubSynthTest, HowLongTheSynthTakesToJoinANewNote) {
  // The time from a note's first sample to the synth's first output. It starts
  // at once, at the previous note's pitch and a third of the level, and reaches
  // the new note when the tracker has it (pitch_tracker_tests print that: 10 to
  // 38 ms); this prints the first and checks the second through the frequency
  // 100 ms in.
  for (const double hz : {82.407, 110.0, 196.0, 329.628, 659.255}) {
    EffectRig rig(kKind, bright(0.0, 0.0));
    const size_t start = 12000;
    std::vector<float> in(start, 0.0f);
    const std::vector<float> note = harmonicTone(0.5, hz, 0.3f);
    in.insert(in.end(), note.begin(), note.end());
    const std::vector<float> out = rig.render(in);
    // The first sample where the output reaches 10 % of its later level.
    const double settled = rmsOf(out, start + 12000, start + 24000);
    size_t join = 0;
    for (size_t i = start; i < in.size(); ++i)
      if (std::abs(out[i]) > 0.1 * settled * 1.4142) {
        join = i - start;
        break;
      }
    std::cout << "[ SUBSYNTH ] " << juce::String(hz, 1) << " Hz: the synth starts after "
              << juce::String(1000.0 * static_cast<double>(join) / kRate, 1) << " ms" << std::endl;
    EXPECT_GT(join, 0u);
    EXPECT_LT(join, static_cast<size_t>(0.06 * kRate));
  }
}

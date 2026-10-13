// pitch.harmonizer (plugin/src/effects/pitch/Harmonizer.cpp): the conformance
// test every effect runs, then what is specific to a harmonizer: fixed
// intervals in chromatic mode; in scale mode the interval following the note
// it detects (a third above C in C major is E, above D it is F); the fall back
// to the chromatic interval when no pitch is heard; level and pan per voice;
// and the mix law that keeps the guitar at unity beside the voices.
#include "PitchShift.h"
#include "effects/pitch_test_signals.h"

#include <gtest/gtest.h>

using namespace pitchtest;

namespace {

constexpr const char* kKind = "pitch.harmonizer";
constexpr double kOff = -60.0;

double midiHz(int midi) { return 440.0 * std::pow(2.0, (midi - 69) / 12.0); }

Params join(Params a, const Params& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

// Scale mode, voice 1 only: `steps` scale degrees from the played note
// (+2 a third above, +4 a fifth, -2 a third below), `fallbackSemitones` when
// no note is heard.
Params scaleVoice(double key, double minor, int steps, double fallbackSemitones) {
  return {{"mode", 1.0},
          {"key", key},
          {"scale", minor},
          {"voice1Degree", static_cast<double>(14 + steps)},
          {"voice1Semitones", fallbackSemitones},
          {"voice1Db", 0.0},
          {"voice2Db", kOff},
          {"mix", 1.0}};
}

struct Note {
  int midi;
  int expectedSemitones;
};

// Plays each note for a second (a hard cut from one to the next), and expects
// the voice at the expected interval above it: the strongest line near the
// shifted frequency within 8 cents, and, where the fallback interval would
// have put it somewhere else, nothing there.
void expectHarmony(const Params& params, double fallbackSemitones, const std::vector<Note>& notes) {
  EffectRig rig(kKind, params);
  std::vector<float> in;
  for (const Note& note : notes)
    in = followedBy(std::move(in), harmonicTone(1.0, midiHz(note.midi), 0.3f, 6));
  const std::vector<float> out = rig.render(in);
  for (size_t i = 0; i < notes.size(); ++i) {
    const Note& note = notes[i];
    SCOPED_TRACE("note " + std::to_string(note.midi) + " expecting " +
                 std::to_string(note.expectedSemitones));
    const size_t begin = i * 48000 + 24000, end = (i + 1) * 48000 - 2400;
    const double target = shifted(midiHz(note.midi), note.expectedSemitones);
    EXPECT_NEAR(centsFrom(out, begin, end, target, 100), 0.0, 8.0);
    const double chromatic = shifted(midiHz(note.midi), fallbackSemitones);
    if (std::abs(fallbackSemitones - note.expectedSemitones) >= 2.0)
      EXPECT_LT(lineDb(out, begin, end, chromatic), lineDb(out, begin, end, target) - 20.0)
          << "the voice is where the chromatic interval would put it";
  }
}

}  // namespace

TEST(HarmonizerTest, Conforms) {
  ConformanceOptions options;
  // Scale mode with both voices on, so the detector runs in the CPU test.
  options.params = {{"mode", 1.0}, {"voice1Db", -3.0}, {"voice2Db", -6.0}};
  options.stateParams = {{"mode", 1.0},           {"key", 7.0},          {"scale", 1.0},
                         {"voice1Semitones", 3.0}, {"voice1Degree", 10.0}, {"voice1Db", -9.0},
                         {"voice1Pan", -20.0},    {"voice2Semitones", -5.0}, {"voice2Degree", 20.0},
                         {"voice2Db", -2.0},      {"voice2Pan", 35.0},   {"window", 0.0}};
  // As for the shifter (see pitch_shifter_tests.cpp): an impulse reaches the
  // voices exactly at the reported latency.
  options.latencyToleranceSamples = 4;  // the 1 % onset of an interpolated impulse
  expectKindConforms("pitch.harmonizer", options);
}

TEST(HarmonizerTest, TableMatchesTheBrief) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kKind);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->name, "Harmonizer");
  EXPECT_EQ(info->category, BlockCategory::Pitch);
  // The voices are heard beside the guitar: the dry signal stays at unity up
  // to 50 %.
  EXPECT_FLOAT_EQ(info->defaultMix, 0.5f);
  EXPECT_EQ(info->mixLaw, BlockMixLaw::DryPreserving);

  const auto find = [&](const char* id) -> const BlockParamSpec& {
    for (const auto& spec : info->params)
      if (spec.id == id)
        return spec;
    ADD_FAILURE() << id;
    return info->params.front();
  };
  EXPECT_EQ(find("mode").choices, juce::StringArray({"Chromatic", "Scale"}));
  EXPECT_EQ(find("key").choices.size(), 12);
  EXPECT_EQ(find("key").choices[0], "C");
  EXPECT_EQ(find("key").choices[11], "B");
  EXPECT_EQ(find("scale").choices, juce::StringArray({"Major", "Minor"}));
  for (const char* voice : {"voice1", "voice2"}) {
    const juce::String v(voice);
    const BlockParamSpec& semitones = find((v + "Semitones").toRawUTF8());
    EXPECT_EQ(semitones.unit, "st");
    EXPECT_FLOAT_EQ(semitones.min, -24.0f);
    EXPECT_FLOAT_EQ(semitones.max, 24.0f);
    EXPECT_FLOAT_EQ(semitones.step, 1.0f);
    const BlockParamSpec& degree = find((v + "Degree").toRawUTF8());
    EXPECT_EQ(degree.type, BlockParamType::Choice);
    EXPECT_EQ(degree.choices.size(), 29);
    EXPECT_EQ(degree.choices[14], "Unison");
    EXPECT_EQ(degree.choices[16], "3rd above");
    EXPECT_EQ(degree.choices[18], "5th above");
    EXPECT_EQ(degree.choices[12], "3rd below");
    EXPECT_EQ(degree.choices[21], "Octave above");
    EXPECT_EQ(find((v + "Db").toRawUTF8()).unit, "dB");
    EXPECT_EQ(find((v + "Pan").toRawUTF8()).unit, "%");
  }
  // A triad by default: a third and a fifth up, in both modes.
  EXPECT_FLOAT_EQ(find("voice1Semitones").defaultValue, 4.0f);
  EXPECT_FLOAT_EQ(find("voice2Semitones").defaultValue, 7.0f);
  EXPECT_FLOAT_EQ(find("voice1Degree").defaultValue, 16.0f);
  EXPECT_FLOAT_EQ(find("voice2Degree").defaultValue, 18.0f);
  const BlockParamSpec& window = find("window");
  EXPECT_TRUE(window.affectsLatency);
  EXPECT_FALSE(window.automatable);
}

// ---------------------------------------------------------------------------
// Chromatic mode
// ---------------------------------------------------------------------------

TEST(HarmonizerTest, ChromaticVoicesAreFixedIntervalsAtTheirOwnLevels) {
  EffectRig rig(kKind, {{"mode", 0.0}, {"voice1Semitones", 4.0}, {"voice1Db", -3.0},
                        {"voice2Semitones", -5.0}, {"voice2Db", -9.0}, {"voice1Pan", 0.0},
                        {"voice2Pan", 0.0}, {"mix", 1.0}});
  const std::vector<float> in = sine(2.0, 220.0, 0.25f, kRate);
  const std::vector<float> out = rig.render(in);
  const size_t from = 48000, to = out.size();
  const double input = lineDb(in, from, to, 220.0);
  EXPECT_NEAR(centsFrom(out, from, to, shifted(220.0, 4.0), 60), 0.0, 4.0);
  EXPECT_NEAR(lineDb(out, from, to, shifted(220.0, 4.0)), input - 3.0, 0.8);
  EXPECT_NEAR(centsFrom(out, from, to, shifted(220.0, -5.0), 60), 0.0, 4.0);
  EXPECT_NEAR(lineDb(out, from, to, shifted(220.0, -5.0)), input - 9.0, 0.8);
  // Wet only: the played pitch is not in it.
  EXPECT_LT(lineDb(out, from, to, 220.0), input - 40.0);

  // Whatever the note, the intervals stay: chromatic mode does not listen.
  const std::vector<float> other = harmonicTone(2.0, 330.0, 0.3f);
  rig.restart();
  const std::vector<float> out2 = rig.render(other);
  EXPECT_NEAR(centsFrom(out2, from, to, shifted(330.0, 4.0), 60), 0.0, 5.0);
  EXPECT_NEAR(centsFrom(out2, from, to, shifted(330.0, -5.0), 60), 0.0, 5.0);
}

TEST(HarmonizerTest, AVoiceAtItsFloorIsOff) {
  EffectRig rig(kKind, {{"mode", 0.0}, {"voice1Semitones", 7.0}, {"voice1Db", kOff},
                        {"voice2Semitones", 12.0}, {"voice2Db", 0.0}, {"voice1Pan", 0.0},
                        {"voice2Pan", 0.0}, {"mix", 1.0}});
  const std::vector<float> in = sine(2.0, 220.0, 0.25f, kRate);
  const std::vector<float> out = rig.render(in);
  const size_t from = 48000, to = out.size();
  const double input = lineDb(in, from, to, 220.0);
  EXPECT_NEAR(lineDb(out, from, to, 440.0), input, 0.8);
  EXPECT_LT(lineDb(out, from, to, shifted(220.0, 7.0)), input - 50.0);
}

// ---------------------------------------------------------------------------
// Scale mode
// ---------------------------------------------------------------------------

TEST(HarmonizerTest, AThirdAboveIsMajorOnCAndMinorOnDInCMajor) {
  // The brief's case: C4 gets E4 (+4), D4 gets F4 (+3), from the same knob.
  expectHarmony(scaleVoice(0, 0, 2, 9.0), 9.0, {{60, 4}, {62, 3}});
}

TEST(HarmonizerTest, ThirdsAboveFollowEveryNoteOfCMajor) {
  // C D E F G A B: E F G A B C D, i.e. +4 +3 +3 +4 +4 +3 +3.
  expectHarmony(scaleVoice(0, 0, 2, 9.0), 9.0,
                {{60, 4}, {62, 3}, {64, 3}, {65, 4}, {67, 4}, {69, 3}, {71, 3}});
}

TEST(HarmonizerTest, FifthsAboveAreFirmExceptOnTheLeadingTone) {
  // A perfect fifth on every degree but the seventh, whose fifth above is
  // diminished (B to F).
  expectHarmony(scaleVoice(0, 0, 4, 2.0), 2.0,
                {{60, 7}, {62, 7}, {64, 7}, {65, 7}, {67, 7}, {69, 7}, {71, 6}});
}

TEST(HarmonizerTest, ThirdsBelowGoDownTheScale) {
  // C D E F G A B: A B C D E F G (an octave down where needed): -3 -3 -4 -3
  // -3 -4 -4.
  expectHarmony(scaleVoice(0, 0, -2, -9.0), -9.0,
                {{60, -3}, {62, -3}, {64, -4}, {65, -3}, {67, -3}, {69, -4}, {71, -4}});
}

TEST(HarmonizerTest, AMinorUsesTheNaturalMinorScale) {
  // A B C D E F G: thirds above are C D E F G A B: +3 +3 +4 +3 +3 +4 +4.
  expectHarmony(scaleVoice(9, 1, 2, 9.0), 9.0,
                {{57, 3}, {59, 3}, {60, 4}, {62, 3}, {64, 3}, {65, 4}, {67, 4}});
}

TEST(HarmonizerTest, TheKeyMovesTheScale) {
  // D major (D E F# G A B C#): the third above D is F# (+4), above E is G
  // (+3), above F# is A (+3), above G is B (+4).
  expectHarmony(scaleVoice(2, 0, 2, 9.0), 9.0, {{62, 4}, {64, 3}, {66, 3}, {67, 4}});
}

TEST(HarmonizerTest, AnOctaveAboveIsTwelveInAnyKey) {
  expectHarmony(scaleVoice(5, 1, 7, 5.0), 5.0, {{60, 12}, {63, 12}});
}

TEST(HarmonizerTest, ANoteOutsideTheScaleKeepsItsDistanceFromTheDegreeBelow) {
  // C# in C major sits above C: a third above C is +4, and the voice moves
  // with the note rather than snapping to a scale note.
  expectHarmony(scaleVoice(0, 0, 2, 9.0), 9.0, {{61, 4}});
}

TEST(HarmonizerTest, EachVoiceHasItsOwnDegree) {
  // Voice 1 a third up, voice 2 a fifth up, on C and on D in C major.
  EffectRig rig(kKind, {{"mode", 1.0}, {"voice1Degree", 16.0}, {"voice1Db", 0.0},
                        {"voice2Degree", 18.0}, {"voice2Db", 0.0},
                        {"voice1Semitones", 10.0}, {"voice2Semitones", 11.0},
                        {"voice1Pan", 0.0}, {"voice2Pan", 0.0}, {"mix", 1.0}});
  const std::vector<float> in =
      followedBy(harmonicTone(1.0, midiHz(60), 0.3f, 6), harmonicTone(1.0, midiHz(62), 0.3f, 6));
  const std::vector<float> out = rig.render(in);
  const struct {
    size_t begin, end;
    double hz;
    int third, fifth;
  } segments[] = {{24000, 45600, midiHz(60), 4, 7}, {72000, 93600, midiHz(62), 3, 7}};
  for (const auto& seg : segments) {
    EXPECT_NEAR(centsFrom(out, seg.begin, seg.end, shifted(seg.hz, seg.third), 60), 0.0, 8.0);
    EXPECT_NEAR(centsFrom(out, seg.begin, seg.end, shifted(seg.hz, seg.fifth), 60), 0.0, 8.0);
    // Both are there, at about the same level.
    EXPECT_NEAR(lineDb(out, seg.begin, seg.end, shifted(seg.hz, seg.third)),
                lineDb(out, seg.begin, seg.end, shifted(seg.hz, seg.fifth)), 3.0);
  }
}

// ---------------------------------------------------------------------------
// No stable pitch: the chromatic interval
// ---------------------------------------------------------------------------

TEST(HarmonizerTest, NoiseFallsBackToTheChromaticInterval) {
  // Scale mode and chromatic mode with the same chromatic intervals render
  // noise bit for bit alike: the detector hears no pitch, so the scale mode's
  // voices are the chromatic ones. (Vice versa, a tone does make them differ,
  // see the tests above.)
  const Params voices = {{"voice1Semitones", 5.0}, {"voice2Semitones", -3.0}, {"voice1Db", -3.0},
                         {"voice2Db", -3.0},      {"mix", 1.0}};
  for (const Layout layout : {Layout::Mono, Layout::Stereo, Layout::DualMono}) {
    SCOPED_TRACE(layoutName(layout));
    EffectRig chromatic(kKind, join(voices, {{"mode", 0.0}}), rigFor(layout));
    EffectRig scale(kKind, join(voices, {{"mode", 1.0}, {"key", 4.0}, {"scale", 1.0}}), rigFor(layout));
    const std::vector<float> in = noise(3.0, 71u, 0.3f, kRate);
    const Stereo a = chromatic.renderBoth(in);
    const Stereo b = scale.renderBoth(in);
    EXPECT_EQ(a.left, b.left);
    EXPECT_EQ(a.right, b.right);
    EXPECT_GT(peakOf(a.left), 0.05f);
  }
}

TEST(HarmonizerTest, ATooQuietNoteFallsBackToo) {
  // Below the detector's floor (about -56 dBFS) there is no note to follow;
  // the voices keep their chromatic interval rather than guess.
  const Params voices = {{"voice1Semitones", 7.0}, {"voice2Db", kOff}, {"mix", 1.0}};
  EffectRig chromatic(kKind, join(voices, {{"mode", 0.0}}));
  EffectRig scale(kKind, join(voices, {{"mode", 1.0}}));
  const std::vector<float> quiet = harmonicTone(2.0, 220.0, 0.002f);
  EXPECT_EQ(chromatic.render(quiet), scale.render(quiet));
}

TEST(HarmonizerTest, ANoteThatStopsIsHeldBrieflyThenTheVoicesReturnToTheChromaticInterval) {
  // C4 then silence then a signal that is not a note: after about 80 ms the
  // note is dropped, and a quiet pitched tail would be followed no more.
  // Seen from the outside: C4, a gap, then a quiet E4 that is below the
  // detector's floor sounds at the fallback interval (+9), not at +4 above E.
  EffectRig rig(kKind, join(scaleVoice(0, 0, 2, 9.0), {}));
  std::vector<float> in = harmonicTone(1.0, midiHz(60), 0.3f, 6);
  in = followedBy(std::move(in), std::vector<float>(24000, 0.0f));
  in = followedBy(std::move(in), harmonicTone(1.5, midiHz(64), 0.002f, 6));
  const std::vector<float> out = rig.render(in);
  const size_t begin = 72000 + 30000, end = in.size() - 2400;
  const double fallback = shifted(midiHz(64), 9.0);
  const double followed = shifted(midiHz(64), 3.0);  // a third above E in C major is G
  EXPECT_GT(lineDb(out, begin, end, fallback), lineDb(out, begin, end, followed) + 20.0);
}

TEST(HarmonizerTest, ADetectedNoteTakesAFewTensOfMillisecondsToArrive) {
  // After silence, a C4 starts. Until the detector has heard it twice the
  // voice plays the chromatic interval (+9 here); it then moves to the
  // diatonic one (+4). Reported, and bounded.
  EffectRig rig(kKind, scaleVoice(0, 0, 2, 9.0));
  std::vector<float> in(24000, 0.0f);
  in = followedBy(std::move(in), harmonicTone(1.0, midiHz(60), 0.3f, 6));
  const std::vector<float> out = rig.render(in);
  const double want = shifted(midiHz(60), 4.0);
  const int window = 2400;  // 50 ms
  int lockedAt = -1;
  for (int start = 24000; start + window < static_cast<int>(out.size()); start += 480) {
    const size_t b = static_cast<size_t>(start), e = static_cast<size_t>(start + window);
    if (lineDb(out, b, e, want) > lineDb(out, b, e, shifted(midiHz(60), 9.0)) + 10.0) {
      lockedAt = start - 24000;
      break;
    }
  }
  ASSERT_GE(lockedAt, 0);
  std::cout << "[   LOCK   ] the diatonic interval is established within " << lockedAt / 48
            << " ms of the note starting (the analysis window is 50 ms)" << std::endl;
  EXPECT_LT(lockedAt, 7200);  // 150 ms
}

// ---------------------------------------------------------------------------
// Dual mono, pan, mix
// ---------------------------------------------------------------------------

TEST(HarmonizerTest, TwoInstrumentsInDualMonoAreTrackedSeparately) {
  // A3 on the left and B3 on the right, in C major, a third above: C4 (+3)
  // and D4 (+3). A detector on the mean would hear neither, and the chromatic
  // interval (+9) would show.
  EffectRig rig(kKind, scaleVoice(0, 0, 2, 9.0), rigFor(Layout::DualMono));
  const Stereo out = rig.render(Stereo{harmonicTone(2.0, midiHz(57), 0.3f, 6),
                                       harmonicTone(2.0, midiHz(59), 0.3f, 6)});
  const size_t from = 48000, to = out.left.size();
  EXPECT_NEAR(centsFrom(out.left, from, to, shifted(midiHz(57), 3.0), 60), 0.0, 8.0);
  EXPECT_NEAR(centsFrom(out.right, from, to, shifted(midiHz(59), 3.0), 60), 0.0, 8.0);
  EXPECT_LT(lineDb(out.left, from, to, shifted(midiHz(57), 9.0)),
            lineDb(out.left, from, to, shifted(midiHz(57), 3.0)) - 20.0);
  EXPECT_LT(lineDb(out.right, from, to, shifted(midiHz(59), 9.0)),
            lineDb(out.right, from, to, shifted(midiHz(59), 3.0)) - 20.0);
  // And nothing of one instrument's voice in the other's output.
  EXPECT_LT(lineDb(out.left, from, to, shifted(midiHz(59), 3.0)),
            lineDb(out.left, from, to, shifted(midiHz(57), 3.0)) - 30.0);
  EXPECT_LT(lineDb(out.right, from, to, shifted(midiHz(57), 3.0)),
            lineDb(out.right, from, to, shifted(midiHz(59), 3.0)) - 30.0);
}

TEST(HarmonizerTest, OnATrueStereoImageEachVoiceHasItsPan) {
  const Params base = {{"mode", 0.0}, {"voice1Semitones", 7.0}, {"voice1Db", 0.0},
                       {"voice2Semitones", -12.0}, {"voice2Db", 0.0}, {"mix", 1.0}};
  const std::vector<float> in = sine(2.0, 220.0, 0.25f, kRate);
  const size_t from = 48000, to = in.size();
  const double input = lineDb(in, from, to, 220.0);
  const double v1 = shifted(220.0, 7.0), v2 = shifted(220.0, -12.0);

  {  // Hard left and hard right: each voice on its side, 3 dB up (a balance that is unity at the centre).
    EffectRig rig(kKind, join(base, {{"voice1Pan", -100.0}, {"voice2Pan", 100.0}}));
    const Stereo out = rig.renderBoth(in);
    EXPECT_NEAR(lineDb(out.left, from, to, v1), input + 3.01, 0.8);
    EXPECT_NEAR(lineDb(out.right, from, to, v2), input + 3.01, 0.8);
    EXPECT_LT(lineDb(out.right, from, to, v1), input - 40.0);
    EXPECT_LT(lineDb(out.left, from, to, v2), input - 40.0);
  }
  {  // Centred: both voices at unity on both sides, and the sides alike.
    EffectRig rig(kKind, join(base, {{"voice1Pan", 0.0}, {"voice2Pan", 0.0}}));
    const Stereo out = rig.renderBoth(in);
    for (const std::vector<float>* side : {&out.left, &out.right}) {
      EXPECT_NEAR(lineDb(*side, from, to, v1), input, 0.8);
      EXPECT_NEAR(lineDb(*side, from, to, v2), input, 0.8);
    }
    EXPECT_EQ(out.left, out.right);
  }
  {  // Half way right: the balance law, cos and sin of 3/8 pi times the square root of two.
    EffectRig rig(kKind, join(base, {{"voice1Pan", 50.0}, {"voice2Pan", 50.0}}));
    const Stereo out = rig.renderBoth(in);
    const double left = std::sqrt(2.0) * std::cos(0.75 * kPi * 0.5);
    const double right = std::sqrt(2.0) * std::sin(0.75 * kPi * 0.5);
    EXPECT_NEAR(lineDb(out.left, from, to, v1), input + toDb(left), 0.8);
    EXPECT_NEAR(lineDb(out.right, from, to, v1), input + toDb(right), 0.8);
  }
}

TEST(HarmonizerTest, WithoutAStereoImageThePansAreIgnored) {
  const Params panned = {{"mode", 0.0}, {"voice1Semitones", 7.0}, {"voice1Db", 0.0},
                         {"voice1Pan", -100.0}, {"voice2Semitones", -12.0}, {"voice2Db", 0.0},
                         {"voice2Pan", 100.0}, {"mix", 1.0}};
  const std::vector<float> in = sine(2.0, 220.0, 0.25f, kRate);
  const size_t from = 48000, to = in.size();
  const double input = lineDb(in, from, to, 220.0);
  const double v1 = shifted(220.0, 7.0), v2 = shifted(220.0, -12.0);

  // A mono bus: the one channel carries both voices at their own level.
  EffectRig mono(kKind, panned, rigFor(Layout::Mono));
  const std::vector<float> m = mono.render(in);
  EXPECT_NEAR(lineDb(m, from, to, v1), input, 0.8);
  EXPECT_NEAR(lineDb(m, from, to, v2), input, 0.8);

  // Two instruments: neither is panned, and neither side hears the other.
  EffectRig dual(kKind, panned, rigFor(Layout::DualMono));
  const Stereo d = dual.render(Stereo{in, std::vector<float>(in.size(), 0.0f)});
  EXPECT_NEAR(lineDb(d.left, from, to, v1), input, 0.8);
  EXPECT_NEAR(lineDb(d.left, from, to, v2), input, 0.8);
  EXPECT_EQ(peakOf(d.right), 0.0f);
}

TEST(HarmonizerTest, TheDryGuitarStaysAtUnityBesideTheVoicesUpToFiftyPercent) {
  // DryPreserving: the dry signal at unity up to 50 %, where the voices reach
  // their own level; above it the dry fades to the voices alone.
  const Params voices = {{"mode", 0.0}, {"voice1Semitones", 7.0}, {"voice1Db", 0.0},
                         {"voice1Pan", 0.0}, {"voice2Db", kOff}};
  const std::vector<float> in = sine(2.0, 220.0, 0.25f, kRate);
  const size_t from = 48000, to = in.size();
  const double input = lineDb(in, from, to, 220.0);
  const double voice = shifted(220.0, 7.0);
  struct Case {
    double mix, dryDb, voiceDb;
  };
  for (const Case& c : {Case{0.25, 0.0, -6.02}, Case{0.5, 0.0, 0.0}, Case{0.75, -6.02, 0.0}}) {
    SCOPED_TRACE(c.mix);
    EffectRig rig(kKind, join(voices, {{"mix", c.mix}}));
    const std::vector<float> out = rig.render(in);
    EXPECT_NEAR(lineDb(out, from, to, 220.0), input + c.dryDb, 0.6);
    EXPECT_NEAR(lineDb(out, from, to, voice), input + c.voiceDb, 0.9);
  }
  // A new block of the kind starts at 50 %.
  EffectRig fresh(kKind, voices);
  const std::vector<float> out = fresh.render(in);
  EXPECT_NEAR(lineDb(out, from, to, 220.0), input, 0.6);
  EXPECT_NEAR(lineDb(out, from, to, voice), input, 0.9);
}

// ---------------------------------------------------------------------------
// Latency, bounds, tail
// ---------------------------------------------------------------------------

TEST(HarmonizerTest, ReportsTheEnginesLatencyAndDelaysTheDryShareByIt) {
  EffectRig rig(kKind, {{"mix", 0.0}});
  EXPECT_EQ(rig.latencySamples(), PitchShift::minOnsetDelaySamples(PitchShift::Window::ms30, kRate, 24.0));
  const std::vector<float> in = noise(0.5, 81u, 0.25f, kRate);
  EXPECT_EQ(estimateDelaySamples(rig.render(in), in, 0, 4000), rig.latencySamples());
  rig.set("window", 3.0);
  EXPECT_EQ(rig.latencySamples(), 446) << "latched until the framework commits it";
  rig.settle();
  EXPECT_EQ(rig.latencySamples(), PitchShift::minOnsetDelaySamples(PitchShift::Window::ms60, kRate, 24.0));
  rig.restart();
  EXPECT_EQ(estimateDelaySamples(rig.render(in), in, 0, 4000), rig.latencySamples());
}

TEST(HarmonizerTest, SilenceNoiseAndSweepsStayFiniteAndBounded) {
  for (const double mode : {0.0, 1.0}) {
    SCOPED_TRACE(mode);
    EffectRig rig(kKind, {{"mode", mode}, {"voice1Db", 0.0}, {"voice2Db", 0.0}, {"mix", 1.0}});
    EXPECT_EQ(peakOf(rig.renderSilence(24000)), 0.0f);
    // Two voices at unity, each an input-sized signal with room for the
    // engine's normalised fades.
    expectCleanSignal(rig.render(noise(2.0, 91u, 0.5f, kRate)), 0.5f * 2.0f * 1.3f, "noise");
    rig.restart();
    expectCleanSignal(rig.render(logSweep(3.0, 40.0, 9000.0, 0.4f)), 0.4f * 2.0f * 1.5f, "sweep");
    rig.restart();
    // A chord (no single note to follow), and a note switched on and off.
    std::vector<float> chord = harmonicTone(1.0, midiHz(52), 0.2f, 4);
    const std::vector<float> top = harmonicTone(1.0, midiHz(59), 0.2f, 4);
    for (size_t i = 0; i < chord.size(); ++i)
      chord[i] += top[i];
    expectCleanSignal(rig.render(chord), 0.4f * 2.0f * 1.5f, "chord");
  }
}

TEST(HarmonizerTest, TheTailFollowsTheWindowAndCoversTheMeasuredDecay) {
  for (int w = 0; w < 4; ++w) {
    SCOPED_TRACE("window " + std::to_string(w));
    const TailMeasurement tail = expectSilenceAfterReportedTail(
        kKind, {{"mode", 1.0}, {"voice1Db", -3.0}, {"voice2Db", -3.0},
                {"window", static_cast<double>(w)}});
    EXPECT_FALSE(tail.capped);
    EXPECT_LE(tail.measuredSeconds, tail.reportedSeconds + tail.latencySamples / kRate + 0.05);
  }
}

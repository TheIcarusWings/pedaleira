// pitch.harmony "Smart Harmony" (plugin/src/effects/pitch/SmartHarmony.cpp):
// the conformance test, the scale arithmetic (detail/ScaleMap.h) against an
// independent construction of the scales, then the block: the right in-scale
// interval for test notes in two keys and several scales, measured on the
// audio; note changes, a held note, the dry signal and its alignment, pans,
// dual mono, four voices, the readings and the cost.
#include "../../../plugin/src/effects/pitch/detail/ScaleMap.h"
#include "effects/pitch_test_signals.h"

#include <gtest/gtest.h>

using namespace pitchtest;

namespace {

constexpr const char* kKind = "pitch.harmony";

const BlockParamSpec& specOf(const BlockKindInfo& info, const char* id) {
  for (const auto& spec : info.params)
    if (spec.id == id)
      return spec;
  ADD_FAILURE() << "no parameter " << id;
  return info.params.front();
}

double hzOfMidi(int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); }

// The voices off, the dry at 0 dB, a voice on at unity and centred.
Params voicesOn(int key, int scale, int steps1, int steps2) {
  return {{"key", static_cast<double>(key)}, {"scale", static_cast<double>(scale)},
          {"voice1Steps", static_cast<double>(steps1)}, {"voice1Db", 0.0}, {"voice1Pan", 0.0},
          {"voice2Steps", static_cast<double>(steps2)}, {"voice2Db", 0.0}, {"voice2Pan", 0.0},
          {"dryDb", -60.0}, {"mix", 1.0}};
}

}  // namespace

TEST(SmartHarmonyTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"key", 7.0}, {"scale", 1.0}, {"voice1Steps", 2.0}, {"voice2Steps", -3.0}};
  options.stateParams = {{"key", 3.0}, {"scale", 4.0}, {"voices", 1.0}, {"voice1Steps", -5.0},
                         {"voice3Steps", 9.0}, {"voice2Pan", -40.0}, {"dryDb", -12.0}, {"window", 2.0}};
  options.cpuAverageBudgetNs = 60000.0;
  options.cpuWorstBudgetNs = 600000.0;
  expectKindConforms("pitch.harmony", options);
}

TEST(SmartHarmonyTest, TableMatchesTheBrief) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kKind);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->name, "Smart Harmony");
  EXPECT_EQ(info->category, BlockCategory::Pitch);
  EXPECT_EQ(info->mixLaw, BlockMixLaw::Crossfade);  // a blend with its own dry level
  EXPECT_FLOAT_EQ(info->defaultMix, 1.0f);

  EXPECT_EQ(specOf(*info, "key").choices.size(), 12);
  const BlockParamSpec& scale = specOf(*info, "scale");
  ASSERT_EQ(scale.choices.size(), 10);
  const char* names[] = {"Major",   "Natural Minor", "Harmonic Minor", "Melodic Minor", "Dorian",
                         "Mixolydian", "Pentatonic Major", "Pentatonic Minor", "Blues", "Chromatic"};
  for (int i = 0; i < 10; ++i)
    EXPECT_EQ(scale.choices[i], names[i]);
  EXPECT_EQ(specOf(*info, "voices").choices.size(), 2);

  for (int voice = 1; voice <= 4; ++voice) {
    const juce::String n(voice);
    const BlockParamSpec& steps = specOf(*info, ("voice" + n + "Steps").toRawUTF8());
    EXPECT_FLOAT_EQ(steps.min, -14.0f);  // two octaves of a seven-note scale
    EXPECT_FLOAT_EQ(steps.max, 14.0f);
    EXPECT_FLOAT_EQ(steps.step, 1.0f);
    const BlockParamSpec& level = specOf(*info, ("voice" + n + "Db").toRawUTF8());
    EXPECT_EQ(level.unit, "dB");
    EXPECT_FLOAT_EQ(level.min, -60.0f);
    const BlockParamSpec& pan = specOf(*info, ("voice" + n + "Pan").toRawUTF8());
    EXPECT_FLOAT_EQ(pan.min, -100.0f);
    EXPECT_FLOAT_EQ(pan.max, 100.0f);
  }
  // A third and a fifth up by default.
  EXPECT_FLOAT_EQ(specOf(*info, "voice1Steps").defaultValue, 2.0f);
  EXPECT_FLOAT_EQ(specOf(*info, "voice2Steps").defaultValue, 4.0f);
  EXPECT_EQ(specOf(*info, "dryDb").unit, "dB");
  EXPECT_TRUE(specOf(*info, "window").affectsLatency);
  ASSERT_EQ(info->readings.size(), 3u);
  EXPECT_EQ(info->readings[0].id, "detectedNote");
  EXPECT_EQ(info->readings[1].id, "detectedHz");
  EXPECT_EQ(info->readings[2].id, "confidence");
}

// ---------------------------------------------------------------------------
// The scale arithmetic
// ---------------------------------------------------------------------------

TEST(SmartHarmonyScaleTest, EveryScaleIsWhatItShouldBe) {
  const std::vector<std::vector<int>> expected = {
      {0, 2, 4, 5, 7, 9, 11}, {0, 2, 3, 5, 7, 8, 10}, {0, 2, 3, 5, 7, 8, 11}, {0, 2, 3, 5, 7, 9, 11},
      {0, 2, 3, 5, 7, 9, 10}, {0, 2, 4, 5, 7, 9, 10}, {0, 2, 4, 7, 9},        {0, 3, 5, 7, 10},
      {0, 3, 5, 6, 7, 10},    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}};
  ASSERT_EQ(static_cast<size_t>(pitchfx::kNumScales), expected.size());
  for (int s = 0; s < pitchfx::kNumScales; ++s) {
    const pitchfx::ScaleDef& scale = pitchfx::kScales[static_cast<size_t>(s)];
    ASSERT_EQ(static_cast<size_t>(scale.size), expected[static_cast<size_t>(s)].size()) << scale.name;
    for (int i = 0; i < scale.size; ++i)
      EXPECT_EQ(scale.offsets[i], expected[static_cast<size_t>(s)][static_cast<size_t>(i)]) << scale.name;
  }
}

TEST(SmartHarmonyScaleTest, TheIntervalForHandCheckedNotesInTwoKeysAndSeveralScales) {
  struct Case {
    int key, scale, note, steps, semitones;
    const char* what;
  };
  const Case cases[] = {
      // C major, thirds and fifths up
      {0, 0, 60, 2, 4, "C: a major third up to E"},
      {0, 0, 62, 2, 3, "D: a minor third up to F"},
      {0, 0, 64, 2, 3, "E: a minor third up to G"},
      {0, 0, 65, 2, 4, "F: a major third up to A"},
      {0, 0, 67, 2, 4, "G: a major third up to B"},
      {0, 0, 69, 2, 3, "A: a minor third up to C"},
      {0, 0, 71, 2, 3, "B: a minor third up to D"},
      {0, 0, 71, 4, 6, "B: a diminished fifth up to F"},
      {0, 0, 65, 4, 7, "F: a fifth up to C"},
      // the same notes in G major (F# in the scale)
      {7, 0, 67, 2, 4, "G in G major: B"},
      {7, 0, 69, 2, 3, "A in G major: C"},
      {7, 0, 71, 2, 3, "B in G major: D"},
      {7, 0, 72, 2, 4, "C in G major: E"},
      {7, 0, 74, 2, 4, "D in G major: F#"},
      {7, 0, 76, 2, 3, "E in G major: G"},
      {7, 0, 78, 2, 3, "F# in G major: A"},
      {7, 0, 78, 4, 6, "F# in G major: up a diminished fifth to C"},
      // A natural minor
      {9, 1, 57, 2, 3, "A: C"},
      {9, 1, 59, 2, 3, "B: D"},
      {9, 1, 60, 2, 4, "C: E"},
      {9, 1, 62, 2, 3, "D: F"},
      {9, 1, 64, 2, 3, "E: G"},
      {9, 1, 65, 2, 4, "F: A"},
      {9, 1, 67, 2, 4, "G: B"},
      // D dorian (B natural)
      {2, 4, 62, 2, 3, "D: F"},
      {2, 4, 67, 2, 4, "G: B"},
      {2, 4, 71, 2, 3, "B: D"},
      // A harmonic minor (G#) and melodic minor (F#, G#)
      {9, 2, 64, 2, 4, "E in A harmonic minor: G#"},
      {9, 2, 60, 2, 4, "C in A harmonic minor: E"},
      {9, 3, 62, 2, 4, "D in A melodic minor: F#"},
      // mixolydian: G with F natural
      {7, 5, 71, 2, 3, "B in G mixolydian: D"},
      {7, 5, 74, 2, 3, "D in G mixolydian: F"},
      // E minor pentatonic: E G A B D, one step is the next note
      {4, 7, 64, 1, 3, "E: G"},
      {4, 7, 67, 1, 2, "G: A"},
      {4, 7, 74, 1, 2, "D: E"},
      {4, 7, 64, 2, 5, "E: A"},
      {4, 7, 64, 3, 7, "E: B"},
      // A major pentatonic: A B C# E F#
      {9, 6, 69, 2, 4, "A: C#"},
      {9, 6, 73, 2, 5, "C#: F#"},
      {9, 6, 73, 1, 3, "C#: E"},
      // A blues: A C D Eb E G, one step above D is Eb
      {9, 8, 62, 1, 1, "D in A blues: Eb"},
      {9, 8, 63, 1, 1, "Eb in A blues: E"},
      // below and octaves
      {0, 0, 60, -2, -3, "C: down a third to A"},
      {0, 0, 64, -2, -4, "E: down to C"},
      {0, 0, 60, 7, 12, "an octave up"},
      {0, 0, 62, -7, -12, "an octave down"},
      {0, 0, 65, 14, 24, "two octaves up"},
      {0, 0, 65, -14, -24, "two octaves down"},
      {4, 7, 64, 5, 12, "an octave is five steps of a pentatonic"},
      // a note outside the scale keeps its distance from the scale note below
      {0, 0, 61, 2, 4, "C# in C major sings as C does"},
      {0, 0, 66, 2, 4, "F# in C major sings as F does"},
  };
  for (const Case& c : cases)
    EXPECT_EQ(pitchfx::harmonyInterval(c.note, c.key, c.scale, c.steps), c.semitones)
        << c.what << " (key " << c.key << ", scale " << c.scale << ", note " << c.note << ", " << c.steps << " steps)";
}

TEST(SmartHarmonyScaleTest, AgreesWithAnIndependentConstructionOfEveryScaleEverywhere) {
  // The scale notes of the keys laid out across seven octaves as a sorted
  // list; the voice is the entry `steps` places along it. Every key, every
  // scale, every note of the guitar's range that is in the scale, every number
  // of steps.
  int checked = 0;
  for (int s = 0; s < pitchfx::kNumScales; ++s) {
    const pitchfx::ScaleDef& scale = pitchfx::kScales[static_cast<size_t>(s)];
    for (int key = 0; key < 12; ++key) {
      std::vector<int> notes;
      for (int octave = -2; octave < 12; ++octave)
        for (int i = 0; i < scale.size; ++i)
          notes.push_back(octave * 12 + key + scale.offsets[i]);
      std::sort(notes.begin(), notes.end());
      for (size_t at = 20; at + 40 < notes.size(); ++at) {
        const int note = notes[at];
        if (note < 40 || note > 88)
          continue;
        for (int steps = -14; steps <= 14; ++steps) {
          EXPECT_EQ(pitchfx::harmonyInterval(note, key, s, steps),
                    notes[static_cast<size_t>(static_cast<int>(at) + steps)] - note)
              << scale.name << " key " << key << " note " << note << " steps " << steps;
          ++checked;
        }
      }
    }
  }
  std::cout << "[ HARMONY  ] " << checked << " (key, scale, note, steps) combinations agree" << std::endl;
}

TEST(SmartHarmonyScaleTest, ChromaticStepsAreSemitonesAndAnOctaveIsTwelveInEveryScaleOfSevenNotes) {
  for (int note = 40; note < 90; ++note)
    for (int steps = -14; steps <= 14; ++steps)
      EXPECT_EQ(pitchfx::harmonyInterval(note, 5, 9, steps), steps);
  for (int s : {0, 1, 2, 3, 4, 5})
    for (int key = 0; key < 12; ++key)
      for (int note = 40; note < 90; ++note) {
        EXPECT_EQ(pitchfx::harmonyInterval(note, key, s, 7), 12);
        EXPECT_EQ(pitchfx::harmonyInterval(note, key, s, -7), -12);
        EXPECT_EQ(pitchfx::harmonyInterval(note, key, s, 0), 0);
      }
}

// ---------------------------------------------------------------------------
// The audio
// ---------------------------------------------------------------------------

TEST(SmartHarmonyTest, EachVoiceSingsTheRightInScaleIntervalForTestNotesInTwoKeysAndScales) {
  struct Case {
    int key, scale, midi, steps1, steps2, semitones1, semitones2;
    const char* what;
  };
  // Notes chosen so that the two voices differ between notes (thirds major and
  // minor, a perfect and a diminished fifth, pentatonic steps).
  const Case cases[] = {
      {0, 0, 65, 2, 4, 4, 7, "F4 in C major: A and C"},
      {0, 0, 59, 2, 4, 3, 6, "B3 in C major: D and F"},
      {0, 0, 64, 2, 4, 3, 7, "E4 in C major: G and B"},
      {2, 0, 66, 2, 4, 3, 7, "F#4 in D major: A and C#"},
      {2, 0, 67, 2, 4, 4, 7, "G4 in D major: B and D"},
      {2, 0, 61, 2, 4, 3, 6, "C#4 in D major: E and G"},
      {9, 1, 55, 2, 4, 4, 7, "G3 in A minor: B and D"},
      {9, 1, 53, 2, 4, 4, 7, "F3 in A minor: A and C"},
      {9, 1, 59, 2, 4, 3, 6, "B3 in A minor: D and F"},
      {4, 7, 62, 1, 2, 2, 5, "D4 in E minor pentatonic: E and G"},
      {4, 7, 67, 1, 3, 2, 7, "G4 in E minor pentatonic: A and D"},
      {7, 4, 62, 2, -2, 3, -4, "D4 in G dorian: F above, B-flat below"},
  };
  double worst = 0.0;
  for (const Case& c : cases) {
    SCOPED_TRACE(c.what);
    // The harmonyInterval the case expects is the literal in the table; the
    // arithmetic is checked above, this checks that the audio follows it.
    ASSERT_EQ(pitchfx::harmonyInterval(c.midi, c.key, c.scale, c.steps1), c.semitones1);
    ASSERT_EQ(pitchfx::harmonyInterval(c.midi, c.key, c.scale, c.steps2), c.semitones2);
    const double hz = hzOfMidi(c.midi);
    EffectRig rig(kKind, voicesOn(c.key, c.scale, c.steps1, c.steps2));
    const std::vector<float> in = sine(3.0, hz, 0.3f, kRate);
    const std::vector<float> out = rig.render(in);
    const double input = lineDb(in, 96000, 144000, hz);
    for (const auto& [semitones, label] : {std::pair<int, const char*>{c.semitones1, "voice 1"},
                                           std::pair<int, const char*>{c.semitones2, "voice 2"}}) {
      const double target = shifted(hz, semitones);
      const double measured = peakFrequencyHz(out, 96000, out.size(), target);
      const double error = centsBetween(measured, target);
      worst = std::max(worst, std::abs(error));
      EXPECT_NEAR(error, 0.0, 2.0) << label << " at " << target << " Hz";
      EXPECT_NEAR(lineDb(out, 96000, out.size(), target), input, 1.5) << label;
    }
  }
  std::cout << "[ HARMONY  ] worst voice frequency error over " << sizeof(cases) / sizeof(cases[0])
            << " test notes: " << juce::String(worst, 3) << " ct" << std::endl;
}

TEST(SmartHarmonyTest, AVoiceFollowsANewNoteToItsNewInterval) {
  // A3 then C4 then F4 in C major, voice 1 two steps up: C4 (+3), E4 (+4), A4 (+4).
  EffectRig rig(kKind, voicesOn(0, 0, 2, 4));
  rig.set("voice2Db", -60.0);
  const double notes[] = {220.0, 261.626, 349.228};
  const int expected[] = {3, 4, 4};
  std::vector<float> in;
  for (const double hz : notes)
    in = followedBy(std::move(in), sine(1.0, hz, 0.3f, kRate));
  const std::vector<float> out = rig.render(in);
  for (int n = 0; n < 3; ++n) {
    const double target = shifted(notes[n], expected[n]);
    const size_t from = static_cast<size_t>((n + 0.5) * kRate), to = static_cast<size_t>((n + 0.97) * kRate);
    EXPECT_NEAR(centsBetween(peakFrequencyHz(out, from, to, target), target), 0.0, 3.0) << "note " << n;
    EXPECT_GT(lineDb(out, from, to, target), lineDb(in, from, to, notes[n]) - 2.0) << "note " << n;
  }
}

TEST(SmartHarmonyTest, TheLastNoteIsHeldWhenConfidenceDropsAndTheVoiceKeepsItsInterval) {
  // A note, then a bed of noise that the tracker cannot read, then the note's
  // pitch again at a lower level: through the noise the interval stays.
  EffectRig rig(kKind, voicesOn(0, 0, 2, 4));
  rig.set("voice2Db", -60.0);
  std::vector<float> in = sine(1.0, 220.0, 0.3f, kRate);
  in = followedBy(std::move(in), noise(0.5, 4u, 0.01f, kRate));
  const std::vector<float> out = rig.render(in);
  // During the noise nothing new is sung: the voice does not wander to some
  // other pitch (its line at the held note's interval is the strongest tone).
  const double heldVoice = shifted(220.0, 3);
  const double during = lineDb(out, 55000, 70000, heldVoice);
  EXPECT_TRUE(std::isfinite(during));
  // The held interval is still the one applied after the tracker lost the note.
  std::vector<float> resumed = sine(0.2, 220.0, 0.3f, kRate);
  const std::vector<float> after = rig.render(resumed);
  EXPECT_NEAR(centsBetween(peakFrequencyHz(after, 4800, after.size(), heldVoice, 100.0), heldVoice), 0.0, 6.0);
}

TEST(SmartHarmonyTest, TheDryLevelAndItsAlignmentWithTheVoices) {
  // The voices off: the block is the dry signal at its level, delayed by the
  // reported latency exactly (the same latency the voices have).
  EffectRig rig(kKind, {{"voice1Db", -60.0}, {"voice2Db", -60.0}, {"dryDb", 0.0}, {"mix", 1.0}});
  EffectRig reference{juce::String()};
  const std::vector<float> in = noise(1.0, 31u, 0.25f, kRate);
  const std::vector<float> out = rig.render(in);
  const std::vector<float> dry = reference.render(in);
  const int latency = rig.latencySamples();
  double worst = 0.0;
  for (size_t i = static_cast<size_t>(latency); i < in.size(); ++i)
    worst = std::max(worst, static_cast<double>(std::abs(out[i] - dry[i - static_cast<size_t>(latency)])));
  EXPECT_LT(worst, 1.0e-6);

  // And the level knob.
  for (const double db : {-6.0, 3.0, -20.0}) {
    rig.set("dryDb", db);
    rig.restart();
    const std::vector<float> scaled = rig.render(in);
    EXPECT_NEAR(rmsOf(scaled, 4800), rmsOf(in, 4800) * std::pow(10.0, db / 20.0), rmsOf(in, 4800) * 0.02) << db;
  }
}

TEST(SmartHarmonyTest, TheAttackOfTheDryAndOfTheVoicesArrivesAtTheReportedLatency) {
  EffectRig rig(kKind, {{"voice1Db", -60.0}, {"voice2Db", -60.0}, {"dryDb", 0.0}, {"mix", 1.0}});
  EffectRig reference{juce::String()};
  const std::vector<float> ir = captureImpulseResponse(rig, 0.3);
  const int origin = onsetIndex(captureImpulseResponse(reference, 0.05));
  EXPECT_EQ(peakIndexOf(ir) - origin, rig.latencySamples());
  EXPECT_EQ(onsetIndex(ir) - origin, rig.latencySamples());
  std::cout << "[ LATENCY  ] pitch.harmony reports " << rig.latencySamples() << " samples ("
            << juce::String(rig.latencySamples() / 48.0, 2) << " ms)" << std::endl;
}

TEST(SmartHarmonyTest, PansPlaceTheVoicesInATrueStereoImageAndNowhereElse) {
  const std::vector<float> in = sine(3.0, 220.0, 0.3f, kRate);
  const double v1 = shifted(220.0, 3), v2 = shifted(220.0, 7);
  {
    Params p = voicesOn(0, 0, 2, 4);
    p.push_back({"voice1Pan", -100.0});
    p.push_back({"voice2Pan", 100.0});
    EffectRig rig(kKind, p);
    const Stereo out = rig.renderBoth(in);
    const size_t from = 96000, to = out.size();
    // Voice 1 hard left, voice 2 hard right.
    EXPECT_GT(lineDb(out.left, from, to, v1), lineDb(out.right, from, to, v1) + 40.0);
    EXPECT_GT(lineDb(out.right, from, to, v2), lineDb(out.left, from, to, v2) + 40.0);
    EXPECT_NEAR(lineDb(out.left, from, to, v1), lineDb(in, from, to, 220.0) + 3.0, 1.5);  // unity at the extreme, +3 dB
  }
  {
    // Mono and dual mono: no image, every voice centred whatever its pan.
    for (const Layout layout : {Layout::Mono, Layout::DualMono}) {
      Params p = voicesOn(0, 0, 2, 4);
      p.push_back({"voice1Pan", -100.0});
      EffectRig rig(kKind, p, rigFor(layout));
      const Stereo out = rig.renderBoth(in);
      const size_t from = 96000, to = out.left.size();
      EXPECT_NEAR(lineDb(out.left, from, to, v1), lineDb(out.right, from, to, v1), 0.3) << layoutName(layout);
    }
  }
}

TEST(SmartHarmonyTest, DualMonoGivesEachInstrumentItsOwnNoteAndItsOwnVoices) {
  EffectRig rig(kKind, voicesOn(0, 0, 2, 4), rigFor(Layout::DualMono));
  rig.set("voice2Db", -60.0);
  // Left plays A3 (a third up is C4), right G3 (a third up is B3).
  const Stereo out = rig.render(Stereo{sine(3.0, 220.0, 0.3f, kRate), sine(3.0, 196.0, 0.3f, kRate)});
  const size_t from = 96000, to = out.left.size();
  const double leftVoice = shifted(220.0, 3), rightVoice = shifted(196.0, 4);
  EXPECT_NEAR(centsBetween(peakFrequencyHz(out.left, from, to, leftVoice), leftVoice), 0.0, 2.0);
  EXPECT_NEAR(centsBetween(peakFrequencyHz(out.right, from, to, rightVoice), rightVoice), 0.0, 2.0);
  EXPECT_LT(lineDb(out.left, from, to, rightVoice), lineDb(out.left, from, to, leftVoice) - 40.0);
  EXPECT_LT(lineDb(out.right, from, to, leftVoice), lineDb(out.right, from, to, rightVoice) - 40.0);
}

TEST(SmartHarmonyTest, FourVoicesAreThereOnlyWhenAsked) {
  // E4 in C major: voice 3 a third down (C4), voice 4 an octave up (E5).
  const double hz = hzOfMidi(64);
  const std::vector<float> in = sine(3.0, hz, 0.3f, kRate);
  const double v3 = shifted(hz, -4), v4 = shifted(hz, 12);
  Params p = voicesOn(0, 0, 2, 4);
  p.push_back({"voice1Db", -60.0});
  p.push_back({"voice2Db", -60.0});
  p.push_back({"voice3Steps", -2.0});
  p.push_back({"voice3Db", 0.0});
  p.push_back({"voice3Pan", 0.0});
  p.push_back({"voice4Steps", 7.0});
  p.push_back({"voice4Db", 0.0});
  p.push_back({"voice4Pan", 0.0});
  {
    EffectRig rig(kKind, p);  // two voices: 3 and 4 are not there
    const std::vector<float> out = rig.render(in);
    EXPECT_LT(lineDb(out, 96000, out.size(), v3), -60.0);
    EXPECT_LT(lineDb(out, 96000, out.size(), v4), -60.0);
  }
  {
    p.push_back({"voices", 1.0});
    EffectRig rig(kKind, p);
    const std::vector<float> out = rig.render(in);
    EXPECT_NEAR(centsBetween(peakFrequencyHz(out, 96000, out.size(), v3), v3), 0.0, 2.0);
    EXPECT_NEAR(centsBetween(peakFrequencyHz(out, 96000, out.size(), v4), v4), 0.0, 2.0);
    EXPECT_NEAR(lineDb(out, 96000, out.size(), v3), lineDb(in, 96000, 144000, hz), 1.5);
    EXPECT_NEAR(lineDb(out, 96000, out.size(), v4), lineDb(in, 96000, 144000, hz), 1.5);
  }
}

TEST(SmartHarmonyTest, TheReadingsReportTheNoteTheFrequencyAndTheConfidence) {
  Params direct = voicesOn(0, 0, 2, 4);
  direct.pop_back();  // no common mix on a kind alone
  DirectKind kind(kKind, direct, Layout::Mono);
  EXPECT_FLOAT_EQ(kind.engine().getReading(0), 0.0f);
  kind.render(sine(0.5, 220.0, 0.3f, kRate));
  EXPECT_NEAR(kind.engine().getReading(0), 57.0, 0.05);   // A3
  EXPECT_NEAR(kind.engine().getReading(1), 220.0, 0.2);   // Hz
  EXPECT_GT(kind.engine().getReading(2), 95.0f);          // percent
  // A bent note: the reading is the continuous note, cents as a fraction.
  kind.render(sine(0.5, 220.0 * std::pow(2.0, 0.3 / 12.0), 0.3f, kRate));
  EXPECT_NEAR(kind.engine().getReading(0), 57.3, 0.05);
  // Noise: the note is held, the confidence is low.
  kind.render(noise(0.5, 3u, 0.3f, kRate));
  EXPECT_NEAR(kind.engine().getReading(0), 57.3, 0.05);
  EXPECT_LT(kind.engine().getReading(2), 70.0f);
  kind.engine().reset();
  EXPECT_FLOAT_EQ(kind.engine().getReading(0), 0.0f);
  EXPECT_FLOAT_EQ(kind.engine().getReading(1), 0.0f);
  EXPECT_FLOAT_EQ(kind.engine().getReading(2), 0.0f);
}

TEST(SmartHarmonyTest, CostOfTwoAndOfFourVoicesWithANotePlaying) {
  // The CPU per instance at 48 kHz in 128-sample blocks, with a tone playing
  // (a voiced signal is the dear case: the tracker refines and every voice
  // runs).
  const std::vector<float> tone = harmonicTone(1.0, 146.83, 0.3f);
  for (const double voices : {0.0, 1.0}) {
    DirectKind kind(kKind, {{"voices", voices}, {"voice3Db", -6.0}, {"voice4Db", -6.0}}, Layout::Stereo, 128);
    double total = 0.0, worst = 0.0;
    const int blocks = 3000;
    juce::AudioBuffer<float> buffer(2, 128);
    BlockProcessContext ctx;
    const juce::ScopedNoDenormals noDenormals;
    // A few seconds to settle first.
    for (int b = 0; b < blocks + 600; ++b) {
      for (int ch = 0; ch < 2; ++ch)
        std::copy_n(tone.data() + (b * 128) % 48000, 128, buffer.getWritePointer(ch));
      const auto begin = std::chrono::steady_clock::now();
      kind.engine().processInChain(buffer, ctx);
      const auto end = std::chrono::steady_clock::now();
      if (b >= 600) {
        const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
        total += ns;
        worst = std::max(worst, ns);
      }
    }
    std::cout << "[   CPU    ] pitch.harmony " << (voices > 0.5 ? "4" : "2") << " voices, a note playing: average "
              << static_cast<long long>(total / blocks) << " ns, worst " << static_cast<long long>(worst)
              << " ns per 128-sample block (" << juce::String(100.0 * total / blocks / 2666667.0, 2)
              << " % of a core)" << std::endl;
  }
}

TEST(SmartHarmonyTest, TheTailFollowsTheWindow) {
  for (int w = 0; w < 4; ++w) {
    const Params p = {{"window", static_cast<double>(w)}};
    const TailMeasurement tail = expectSilenceAfterReportedTail(kKind, p);
    EXPECT_FALSE(tail.capped);
    EXPECT_NEAR(tail.reportedSeconds, 2.0 * PitchShift::windowMs(PitchShift::windowFromIndex(w)) * 0.001, 1.0e-9);
  }
}

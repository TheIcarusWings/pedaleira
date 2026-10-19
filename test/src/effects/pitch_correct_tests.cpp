// pitch.correct "Pitch Correct" (plugin/src/effects/pitch/PitchCorrect.cpp):
// the conformance test and the table, then the correction measured on the
// audio: a flat note pulled to the nearest note within a cent, a note in tune
// left alone, Amount, the retune time (10 % to 90 %) for several speeds, the
// choice of target in several scales and a custom set, detune, humanize, the
// bass low E, silence, noise, note changes without clicks, dual mono, the mix,
// the readings, the latency, the cost and the allocation guard.
#include "effects/allocation_guard.h"
#include "effects/pitch_test_signals.h"

#include <gtest/gtest.h>

using namespace pitchtest;

namespace {

constexpr const char* kKind = "pitch.correct";

// The scale choices (Chromatic, the scales of ScaleMap.h, Custom).
constexpr double kChromatic = 0.0;
constexpr double kMajor = 1.0;
constexpr double kMinor = 2.0;
constexpr double kMinorPentatonic = 8.0;
constexpr double kBlues = 9.0;
constexpr double kCustom = 10.0;
constexpr double kKeyA = 9.0;

const BlockParamSpec& specOf(const BlockKindInfo& info, const char* id) {
  for (const auto& spec : info.params)
    if (spec.id == id)
      return spec;
  ADD_FAILURE() << "no parameter " << id;
  return info.params.front();
}

double hzOfMidi(double note) { return 440.0 * std::pow(2.0, (note - 69.0) / 12.0); }

Params correction(double key, double scale, double speedMs = 0.0) {
  return {{"key", key}, {"scale", scale}, {"speedMs", speedMs}, {"amount", 100.0}, {"mix", 1.0}};
}

// The frequency of a clean tone cycle by cycle, from its upward zero
// crossings (linearly interpolated), with the time of each cycle's middle.
struct CycleTrack {
  std::vector<double> seconds, hz;
};

CycleTrack cyclesOf(const std::vector<float>& x, size_t from, size_t to) {
  CycleTrack track;
  double previous = -1.0;
  for (size_t i = std::max<size_t>(from, 1); i < std::min(to, x.size()); ++i) {
    if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
      const double crossing = static_cast<double>(i - 1) + x[i - 1] / (x[i - 1] - x[i]);
      if (previous >= 0.0) {
        track.seconds.push_back(0.5 * (crossing + previous) / kRate);
        track.hz.push_back(kRate / (crossing - previous));
      }
      previous = crossing;
    }
  }
  return track;
}

// The cycle track in cents against `reference`, averaged over `span` cycles.
std::vector<double> smoothedCents(const CycleTrack& track, double reference, int span) {
  std::vector<double> cents(track.hz.size(), 0.0);
  for (size_t i = 0; i < track.hz.size(); ++i) {
    double sum = 0.0;
    int count = 0;
    for (int k = -span / 2; k <= span / 2; ++k) {
      const long j = static_cast<long>(i) + k;
      if (j >= 0 && j < static_cast<long>(track.hz.size())) {
        sum += centsBetween(track.hz[static_cast<size_t>(j)], reference);
        ++count;
      }
    }
    cents[i] = sum / count;
  }
  return cents;
}

// The output's frequency near `expected` over [from, to) seconds.
double measuredHz(const std::vector<float>& out, double from, double to, double expected, double range = 80.0) {
  return peakFrequencyHz(out, static_cast<size_t>(from * kRate), static_cast<size_t>(to * kRate), expected, range);
}

}  // namespace

TEST(PitchCorrectTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"key", kKeyA}, {"scale", kMinor}, {"speedMs", 10.0}};
  options.stateParams = {{"key", 3.0},       {"scale", kCustom},   {"speedMs", 120.0}, {"amount", 70.0},
                         {"humanize", 40.0}, {"detuneCents", -12.0}, {"range", 1.0},    {"noteCs", 1.0},
                         {"noteE", 0.0},     {"window", 2.0}};
  options.cpuAverageBudgetNs = 40000.0;
  options.cpuWorstBudgetNs = 400000.0;
  expectKindConforms("pitch.correct", options);
}

TEST(PitchCorrectTest, TableMatchesTheBrief) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kKind);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->name, "Pitch Correct");
  EXPECT_EQ(info->category, BlockCategory::Pitch);
  EXPECT_EQ(info->mixLaw, BlockMixLaw::Crossfade);
  EXPECT_FLOAT_EQ(info->defaultMix, 1.0f);

  const BlockParamSpec& key = specOf(*info, "key");
  ASSERT_EQ(key.choices.size(), 12);
  EXPECT_EQ(key.choices[0], "C");
  EXPECT_EQ(key.choices[11], "B");
  const BlockParamSpec& scale = specOf(*info, "scale");
  const char* names[] = {"Chromatic",  "Major",      "Natural Minor",    "Harmonic Minor",
                         "Melodic Minor", "Dorian", "Mixolydian",       "Pentatonic Major",
                         "Pentatonic Minor", "Blues", "Custom"};
  ASSERT_EQ(scale.choices.size(), 11);
  for (int i = 0; i < 11; ++i)
    EXPECT_EQ(scale.choices[i], names[i]);
  EXPECT_FLOAT_EQ(scale.defaultValue, 0.0f);  // chromatic

  const BlockParamSpec& speed = specOf(*info, "speedMs");
  EXPECT_EQ(speed.unit, "ms");
  EXPECT_FLOAT_EQ(speed.min, 0.0f);
  EXPECT_FLOAT_EQ(speed.max, 400.0f);
  const BlockParamSpec& amount = specOf(*info, "amount");
  EXPECT_EQ(amount.unit, "%");
  EXPECT_FLOAT_EQ(amount.max, 100.0f);
  EXPECT_FLOAT_EQ(amount.defaultValue, 100.0f);
  const BlockParamSpec& humanize = specOf(*info, "humanize");
  EXPECT_EQ(humanize.unit, "%");
  EXPECT_FLOAT_EQ(humanize.defaultValue, 0.0f);
  const BlockParamSpec& detune = specOf(*info, "detuneCents");
  EXPECT_EQ(detune.unit, "ct");
  EXPECT_FLOAT_EQ(detune.min, -50.0f);
  EXPECT_FLOAT_EQ(detune.max, 50.0f);
  const BlockParamSpec& range = specOf(*info, "range");
  ASSERT_EQ(range.choices.size(), 3);
  EXPECT_EQ(range.choices[0], "Guitar");
  EXPECT_EQ(range.choices[1], "Bass");
  EXPECT_EQ(range.choices[2], "Voice");
  // The custom set: a switch per note name, the white keys on by default.
  const char* notes[] = {"noteC",  "noteCs", "noteD",  "noteDs", "noteE",  "noteF",
                         "noteFs", "noteG",  "noteGs", "noteA",  "noteAs", "noteB"};
  const bool white[] = {true, false, true, false, true, true, false, true, false, true, false, true};
  for (int n = 0; n < 12; ++n) {
    const BlockParamSpec& note = specOf(*info, notes[n]);
    EXPECT_EQ(note.type, BlockParamType::Bool) << notes[n];
    EXPECT_FLOAT_EQ(note.defaultValue, white[n] ? 1.0f : 0.0f) << notes[n];
  }
  EXPECT_TRUE(specOf(*info, "window").affectsLatency);
  EXPECT_EQ(info->params.back().id, "window");
  ASSERT_EQ(info->readings.size(), 4u);
  EXPECT_EQ(info->readings[0].id, "detectedNote");
  EXPECT_EQ(info->readings[1].id, "detectedHz");
  EXPECT_EQ(info->readings[2].id, "confidence");
  EXPECT_EQ(info->readings[3].id, "targetNote");
}

// ---------------------------------------------------------------------------
// Accuracy
// ---------------------------------------------------------------------------

TEST(PitchCorrectTest, AFlatNoteComesOutOnTheNoteWithinACent) {
  // 430 Hz in A minor, Amount 100 %, Speed 0: the nearest scale note is A4.
  // A sine, a guitar-like tone, a sharp note, and a low string.
  struct Case {
    double inHz, targetHz;
    bool harmonic;
    const char* what;
  };
  const Case cases[] = {
      {430.0, 440.0, false, "430 Hz sine to A4"},
      {430.0, 440.0, true, "430 Hz harmonic tone to A4"},
      {451.0, 440.0, false, "451 Hz sine down to A4"},
      {107.5, 110.0, true, "107.5 Hz harmonic tone to A2"},
      {335.0, hzOfMidi(64), true, "335 Hz harmonic tone down to E4"},
  };
  double worst = 0.0;
  for (const Case& c : cases) {
    SCOPED_TRACE(c.what);
    EffectRig rig(kKind, correction(kKeyA, kMinor));
    const std::vector<float> in = c.harmonic ? harmonicTone(3.0, c.inHz, 0.3f) : sine(3.0, c.inHz, 0.3f, kRate);
    const std::vector<float> out = rig.render(in);
    const double error = centsBetween(measuredHz(out, 1.0, 3.0, c.targetHz), c.targetHz);
    worst = std::max(worst, std::abs(error));
    EXPECT_NEAR(error, 0.0, 1.0);
    // The level is kept and nothing is left at the played pitch.
    EXPECT_NEAR(lineDb(out, 48000, 144000, c.targetHz), lineDb(in, 48000, 144000, c.inHz), 1.0);
    EXPECT_LT(lineDb(out, 48000, 144000, c.inHz), lineDb(out, 48000, 144000, c.targetHz) - 30.0);
  }
  std::cout << "[ CORRECT  ] worst error of a corrected note: " << juce::String(worst, 3) << " ct" << std::endl;
}

TEST(PitchCorrectTest, ANoteInTuneIsUntouched) {
  // A4 and A3 in A minor (and a note a third of a cent off): the block plays
  // the dry signal, delayed by its latency, sample for sample.
  for (const double hz : {440.0, 220.0, 440.0 * std::pow(2.0, 0.3 / 1200.0)}) {
    SCOPED_TRACE(hz);
    EffectRig rig(kKind, correction(kKeyA, kMinor));
    EffectRig reference{juce::String()};
    const std::vector<float> in = harmonicTone(2.0, hz, 0.3f);
    const std::vector<float> out = rig.render(in);
    const std::vector<float> dry = reference.render(in);
    const size_t latency = static_cast<size_t>(rig.latencySamples());
    double worst = 0.0;
    for (size_t i = latency; i < in.size(); ++i)
      worst = std::max(worst, static_cast<double>(std::abs(out[i] - dry[i - latency])));
    EXPECT_LT(worst, 1.0e-6);
    EXPECT_NEAR(centsBetween(measuredHz(out, 0.5, 2.0, hz), hz), 0.0, 0.2);
  }
}

TEST(PitchCorrectTest, AmountMovesTheNoteThatShareOfTheWayInCents) {
  const double full = centsBetween(440.0, 430.0);  // 39.8 ct
  for (const double amount : {0.0, 25.0, 50.0, 75.0}) {
    SCOPED_TRACE(amount);
    Params p = correction(kKeyA, kMinor);
    p.push_back({"amount", amount});
    EffectRig rig(kKind, p);
    const std::vector<float> out = rig.render(sine(3.0, 430.0, 0.3f, kRate));
    const double moved = centsBetween(measuredHz(out, 1.0, 3.0, 435.0), 430.0);
    std::cout << "[ CORRECT  ] amount " << amount << " %: moved " << juce::String(moved, 2) << " ct of "
              << juce::String(full, 2) << std::endl;
    EXPECT_NEAR(moved, full * amount / 100.0, 1.0);
  }
}

// ---------------------------------------------------------------------------
// Retune time
// ---------------------------------------------------------------------------

TEST(PitchCorrectTest, SpeedIsTheTenToNinetyPercentRetuneTime) {
  // 430 Hz from silence, A minor: the output starts at the played pitch and
  // glides to 440 Hz. The time from 10 % to 90 % of the 39.8 ct is the Speed.
  const double full = centsBetween(440.0, 430.0);
  for (const double speed : {0.0, 50.0, 100.0, 200.0, 400.0}) {
    SCOPED_TRACE(speed);
    EffectRig rig(kKind, correction(kKeyA, kMinor, speed));
    std::vector<float> in(4800, 0.0f);
    const std::vector<float> note = sine(2.5, 430.0, 0.3f, kRate);
    in.insert(in.end(), note.begin(), note.end());
    const std::vector<float> out = rig.render(in);
    const CycleTrack track = cyclesOf(out, 4800, out.size());
    const std::vector<double> cents = smoothedCents(track, 430.0, 3);
    double t10 = -1.0, t90 = -1.0;
    for (size_t i = 0; i < cents.size(); ++i) {
      if (t10 < 0.0 && cents[i] >= 0.1 * full)
        t10 = track.seconds[i];
      if (t90 < 0.0 && cents[i] >= 0.9 * full)
        t90 = track.seconds[i];
    }
    ASSERT_GE(t10, 0.0);
    ASSERT_GE(t90, 0.0);
    const double ms = 1000.0 * (t90 - t10);
    std::cout << "[ CORRECT  ] speed " << speed << " ms: 10 % to 90 % in " << juce::String(ms, 1)
              << " ms, 10 % reached " << juce::String(1000.0 * t10 - 100.0, 1) << " ms after the note began"
              << std::endl;
    if (speed < 1.0)
      EXPECT_LT(ms, 25.0);
    else
      EXPECT_NEAR(ms, speed, 0.1 * speed + 6.0);
    // It gets there, and stays.
    EXPECT_NEAR(centsBetween(measuredHz(out, 2.0, 2.6, 440.0), 440.0), 0.0, 1.0);
  }
}

// ---------------------------------------------------------------------------
// The target
// ---------------------------------------------------------------------------

TEST(PitchCorrectTest, TheTargetIsTheNearestNoteOfTheScale) {
  struct Case {
    double key, scale, played, target;
    const char* what;
  };
  const Case cases[] = {
      {kKeyA, kMinor, 62.4, 62.0, "D4 + 40 ct in A minor, between D and E: D"},
      {kKeyA, kMinor, 63.6, 64.0, "E4 - 40 ct in A minor, between D and E: E"},
      {0.0, kMajor, 63.3, 64.0, "D#4 + 30 ct in C major: E"},
      {0.0, kMajor, 61.4, 62.0, "C#4 + 40 ct in C major: D"},
      {kKeyA, kMinorPentatonic, 71.0, 72.0, "B4 in A minor pentatonic (not in it): C5"},
      {kKeyA, kMinorPentatonic, 65.0, 64.0, "F4 in A minor pentatonic (not in it): E4"},
      {kKeyA, kBlues, 63.3, 63.0, "Eb4 + 30 ct in A blues: the blue note"},
      {kKeyA, kChromatic, 69.2, 69.0, "A4 + 20 ct chromatic: A4"},
      {kKeyA, kChromatic, 57.7, 58.0, "A3 + 70 ct chromatic: A#3"},
  };
  double worst = 0.0;
  for (const Case& c : cases) {
    SCOPED_TRACE(c.what);
    EffectRig rig(kKind, correction(c.key, c.scale));
    const std::vector<float> out = rig.render(sine(2.0, hzOfMidi(c.played), 0.3f, kRate));
    const double target = hzOfMidi(c.target);
    const double error = centsBetween(measuredHz(out, 0.8, 2.0, target, 120.0), target);
    worst = std::max(worst, std::abs(error));
    EXPECT_NEAR(error, 0.0, 1.0);
  }
  std::cout << "[ CORRECT  ] worst error over the scale cases: " << juce::String(worst, 3) << " ct" << std::endl;
}

TEST(PitchCorrectTest, ACustomSetOfNotesIsTheTargetAndTheKeyIsNotUsedForIt) {
  // Only C and G on: E4 is three semitones from G4 and four from C4.
  Params p = correction(5.0, kCustom);
  for (const char* id : {"noteD", "noteE", "noteF", "noteA", "noteB"})
    p.push_back({id, 0.0});
  {
    EffectRig rig(kKind, p);
    const std::vector<float> out = rig.render(sine(2.0, hzOfMidi(64), 0.3f, kRate));
    EXPECT_NEAR(centsBetween(measuredHz(out, 0.8, 2.0, hzOfMidi(67)), hzOfMidi(67)), 0.0, 1.0);
  }
  {
    // F#4 + 10 ct: G4. And with every note off nothing is corrected.
    EffectRig rig(kKind, p);
    const double played = hzOfMidi(66.1);
    std::vector<float> out = rig.render(sine(2.0, played, 0.3f, kRate));
    EXPECT_NEAR(centsBetween(measuredHz(out, 0.8, 2.0, hzOfMidi(67)), hzOfMidi(67)), 0.0, 1.0);
    rig.set("noteC", 0.0);
    rig.set("noteG", 0.0);
    out = rig.render(sine(2.0, played, 0.3f, kRate));
    EXPECT_NEAR(centsBetween(measuredHz(out, 0.8, 2.0, played), played), 0.0, 0.5);
  }
}

TEST(PitchCorrectTest, DetuneMovesTheGridOfTargets) {
  // +20 ct: A4 in tune is moved up 20 ct; 445 Hz (+19.6 ct) with a +10 ct grid
  // lands on A4 + 10 ct.
  {
    Params p = correction(kKeyA, kChromatic);
    p.push_back({"detuneCents", 20.0});
    EffectRig rig(kKind, p);
    const std::vector<float> out = rig.render(sine(2.0, 440.0, 0.3f, kRate));
    EXPECT_NEAR(centsBetween(measuredHz(out, 0.8, 2.0, 445.0), 440.0), 20.0, 1.0);
  }
  {
    Params p = correction(kKeyA, kChromatic);
    p.push_back({"detuneCents", 10.0});
    EffectRig rig(kKind, p);
    const std::vector<float> out = rig.render(sine(2.0, 445.0, 0.3f, kRate));
    EXPECT_NEAR(centsBetween(measuredHz(out, 0.8, 2.0, 443.0), 440.0), 10.0, 1.0);
  }
  {
    // -50 ct: the grid sits on the quarter tones; 427.5 Hz (-50 ct) is on it.
    Params p = correction(kKeyA, kChromatic);
    p.push_back({"detuneCents", -50.0});
    EffectRig rig(kKind, p);
    const std::vector<float> out = rig.render(sine(2.0, 432.0, 0.3f, kRate));
    EXPECT_NEAR(centsBetween(measuredHz(out, 0.8, 2.0, 428.0), 440.0), -50.0, 1.0);
  }
}

// A sine at `centreHz` with a vibrato of `depthCents` at `rateHz`.
std::vector<float> vibratoTone(double seconds, double centreHz, double depthCents, double rateHz) {
  std::vector<float> x(static_cast<size_t>(seconds * kRate));
  double phase = 0.0;
  for (size_t i = 0; i < x.size(); ++i) {
    const double t = static_cast<double>(i) / kRate;
    phase += 2.0 * kPi * centreHz * std::pow(2.0, depthCents * std::sin(2.0 * kPi * rateHz * t) / 1200.0) / kRate;
    x[i] = 0.3f * static_cast<float>(std::sin(phase));
  }
  return x;
}

TEST(PitchCorrectTest, HumanizeLetsASustainedNoteMoveAroundItsCorrectedCentre) {
  // 430 Hz with a 5 Hz vibrato of 15 ct. Hard tune without humanize flattens
  // the vibrato onto 440 Hz; with humanize the centre is still 440 Hz and the
  // vibrato is kept.
  const std::vector<float> in = vibratoTone(3.0, 430.0, 15.0, 5.0);
  double swing[2] = {0.0, 0.0};
  for (const double humanize : {0.0, 100.0}) {
    SCOPED_TRACE(humanize);
    Params p = correction(kKeyA, kMinor);
    p.push_back({"humanize", humanize});
    EffectRig rig(kKind, p);
    const std::vector<float> out = rig.render(in);
    const CycleTrack track = cyclesOf(out, static_cast<size_t>(1.0 * kRate), static_cast<size_t>(3.0 * kRate));
    const std::vector<double> cents = smoothedCents(track, 440.0, 5);
    double mean = 0.0, lowest = 1.0e9, highest = -1.0e9;
    for (const double c : cents) {
      mean += c;
      lowest = std::min(lowest, c);
      highest = std::max(highest, c);
    }
    mean /= static_cast<double>(cents.size());
    swing[humanize > 0.0 ? 1 : 0] = highest - lowest;
    std::cout << "[ CORRECT  ] humanize " << humanize << " %: centre " << juce::String(mean, 2)
              << " ct from A4, swing " << juce::String(highest - lowest, 1) << " ct (the input swings 30 ct)"
              << std::endl;
    EXPECT_NEAR(mean, 0.0, 2.0);
  }
  // A hard tune follows the input, but a shifted note's delay wanders over the
  // engine's window while the tracker reads the newest input, so at 5 Hz about
  // half the vibrato is left; humanize keeps nearly all of it.
  EXPECT_LT(swing[0], 18.0);
  EXPECT_GT(swing[1], swing[0] + 8.0);
  EXPECT_LT(swing[1], 32.0);

  // A slow drift (0.5 Hz, 15 ct) is flattened by a hard tune.
  EffectRig rig(kKind, correction(kKeyA, kMinor));
  const std::vector<float> out = rig.render(vibratoTone(4.0, 430.0, 15.0, 0.5));
  const CycleTrack track = cyclesOf(out, static_cast<size_t>(1.0 * kRate), static_cast<size_t>(4.0 * kRate));
  const std::vector<double> cents = smoothedCents(track, 440.0, 5);
  double worst = 0.0;
  for (const double c : cents)
    worst = std::max(worst, std::abs(c));
  std::cout << "[ CORRECT  ] a 0.5 Hz drift of 15 ct under a hard tune: worst " << juce::String(worst, 2)
            << " ct from A4" << std::endl;
  EXPECT_LT(worst, 3.0);
}

// ---------------------------------------------------------------------------
// Ranges, silence, noise
// ---------------------------------------------------------------------------

TEST(PitchCorrectTest, TheBassRangeCorrectsTheLowE) {
  // E1 is 41.2 Hz; played at 40.8 Hz (17 ct flat), a bass-like tone.
  const double e1 = hzOfMidi(28);
  const std::vector<float> in = harmonicTone(4.0, 40.8, 0.3f, 10);
  {
    Params p = correction(4.0, kChromatic);
    p.push_back({"range", 1.0});
    EffectRig rig(kKind, p);
    const std::vector<float> out = rig.render(in);
    const double error = centsBetween(measuredHz(out, 1.5, 4.0, e1), e1);
    std::cout << "[ CORRECT  ] bass E1 played 17 ct flat comes out " << juce::String(error, 3) << " ct from 41.20 Hz"
              << std::endl;
    EXPECT_NEAR(error, 0.0, 1.0);
    // The second harmonic moves with it.
    EXPECT_NEAR(centsBetween(measuredHz(out, 1.5, 4.0, 2.0 * e1), 2.0 * e1), 0.0, 1.0);
  }
  {
    // The guitar range does not read a note that low: the bass passes.
    EffectRig rig(kKind, correction(4.0, kChromatic));
    const std::vector<float> out = rig.render(in);
    EXPECT_NEAR(centsBetween(measuredHz(out, 1.5, 4.0, 40.8), 40.8), 0.0, 1.0);
  }
}

TEST(PitchCorrectTest, TheVoiceRangeCorrectsASungLikeNote) {
  // A vowel-like tone (harmonics under a formant bump) with some pitch jitter,
  // G3 played 30 ct flat, voice range.
  std::vector<float> in(static_cast<size_t>(3.0 * kRate), 0.0f);
  const double hz = hzOfMidi(54.7);
  std::mt19937 random(7u);
  std::normal_distribution<double> jitter(0.0, 1.0);
  double phase = 0.0, wobble = 0.0;
  for (size_t i = 0; i < in.size(); ++i) {
    wobble = 0.999 * wobble + 0.02 * jitter(random);  // a slow random drift of a few cents
    phase += 2.0 * kPi * hz * std::pow(2.0, wobble / 1200.0) / kRate;
    double v = 0.0;
    for (int h = 1; h <= 12; ++h) {
      const double f = h * hz;
      const double formant = std::exp(-std::pow((f - 700.0) / 400.0, 2.0)) + 0.5 * std::exp(-std::pow((f - 1200.0) / 300.0, 2.0));
      v += (0.2 / h + formant) * std::sin(h * phase);
    }
    in[i] = static_cast<float>(0.1 * v);
  }
  Params p = correction(7.0, kMajor);
  p.push_back({"range", 2.0});
  EffectRig rig(kKind, p);
  const std::vector<float> out = rig.render(in);
  const double g3 = hzOfMidi(55);
  const double error = centsBetween(measuredHz(out, 1.0, 3.0, g3), g3);
  std::cout << "[ CORRECT  ] voice-like G3 played 30 ct flat comes out " << juce::String(error, 2) << " ct from G3"
            << std::endl;
  EXPECT_NEAR(error, 0.0, 2.0);
}

TEST(PitchCorrectTest, SilenceInIsSilenceOut) {
  DirectKind kind(kKind, {{"key", kKeyA}, {"scale", kMinor}, {"speedMs", 0.0}}, Layout::Stereo);
  const Stereo out = kind.render(Stereo{std::vector<float>(96000, 0.0f), std::vector<float>(96000, 0.0f)});
  EXPECT_EQ(peakOf(out.left), 0.0f);
  EXPECT_EQ(peakOf(out.right), 0.0f);
  // And after a corrected note, silence again once the tail is over.
  kind.render(sine(1.0, 430.0, 0.3f, kRate));
  const std::vector<float> after = kind.render(std::vector<float>(48000, 0.0f));
  const size_t tail = static_cast<size_t>(std::ceil(kind.tailSeconds() * kRate)) + 1;
  EXPECT_EQ(peakOf(after, tail), 0.0f);
}

TEST(PitchCorrectTest, NoiseIsPassedThroughAndDoesNotWarbleACorrectedNote) {
  // White noise alone: no note, the dry signal (delayed) comes out.
  {
    EffectRig rig(kKind, correction(kKeyA, kMinor));
    EffectRig reference{juce::String()};
    for (const unsigned seed : {1u, 2u, 3u}) {
      const std::vector<float> in = noise(2.0, seed, 0.3f, kRate);
      const std::vector<float> out = rig.render(in);
      const std::vector<float> dry = reference.render(in);
      const size_t latency = static_cast<size_t>(rig.latencySamples());
      double error = 0.0, energy = 0.0;
      for (size_t i = 4800; i < in.size(); ++i) {
        const double d = static_cast<double>(out[i]) - dry[i - latency];
        error += d * d;
        energy += static_cast<double>(dry[i - latency]) * dry[i - latency];
      }
      const double residualDb = 10.0 * std::log10(std::max(error, 1.0e-30) / energy);
      std::cout << "[ CORRECT  ] white noise seed " << seed << ": output against the delayed dry "
                << juce::String(residualDb, 1) << " dB" << std::endl;
      EXPECT_LT(residualDb, -40.0) << seed;
    }
  }
  // A flat note under noise 20 dB down: the corrected pitch stays put.
  {
    EffectRig rig(kKind, correction(kKeyA, kMinor));
    std::vector<float> in = sine(3.0, 430.0, 0.3f, kRate);
    const std::vector<float> hiss = noise(3.0, 11u, 0.052f, kRate);  // 20 dB under the tone (rms)
    for (size_t i = 0; i < in.size(); ++i)
      in[i] += hiss[i];
    const std::vector<float> out = rig.render(in);
    double worst = 0.0;
    for (double t = 0.5; t + 0.05 <= 3.0; t += 0.05)
      worst = std::max(worst, std::abs(centsBetween(measuredHz(out, t, t + 0.05, 440.0), 440.0)));
    std::cout << "[ CORRECT  ] a corrected note under noise 20 dB down: worst 50 ms pitch error "
              << juce::String(worst, 2) << " ct" << std::endl;
    EXPECT_LT(worst, 3.0);
  }
}

TEST(PitchCorrectTest, NoteChangesAndStopsAreFreeOfClicks) {
  // A legato line (continuous phase, smooth level) over notes that each need a
  // different correction, then a stop and a new note: the output's largest
  // sample step stays near what its own slope explains.
  const double notes[] = {430.0, 340.0, 500.0, 210.0, 391.0, 455.0};
  std::vector<float> in;
  double phase = 0.0;
  for (const double hz : notes) {
    const size_t length = static_cast<size_t>(0.35 * kRate);
    for (size_t i = 0; i < length; ++i) {
      phase += 2.0 * kPi * hz / kRate;
      in.push_back(0.3f * static_cast<float>(std::sin(phase)));
    }
  }
  // A stop with a 5 ms release, silence, and a note with a 5 ms attack.
  for (size_t i = 0; i < 240; ++i)
    in.push_back(0.3f * static_cast<float>(std::sin(phase += 2.0 * kPi * 455.0 / kRate)) * (1.0f - i / 240.0f));
  in.resize(in.size() + 9600, 0.0f);
  for (size_t i = 0; i < static_cast<size_t>(0.5 * kRate); ++i)
    in.push_back(0.3f * static_cast<float>(std::sin(2.0 * kPi * 285.0 * i / kRate)) * std::min(1.0f, i / 240.0f));
  for (const double speed : {0.0, 30.0}) {
    EffectRig rig(kKind, correction(0.0, kMajor, speed));
    const std::vector<float> out = rig.render(in);
    const float inStep = maxStep(in, 0, in.size());
    const float outStep = maxStep(out, 0, out.size());
    std::cout << "[ CORRECT  ] speed " << speed << " ms, note changes: largest step " << outStep << " (input "
              << inStep << ")" << std::endl;
    EXPECT_LT(outStep, 1.6f * inStep) << speed;
    expectCleanSignal(out, 0.6f, "note changes");
  }
}

TEST(PitchCorrectTest, NoNaNsOrDenormalsWhateverComesIn) {
  std::vector<std::pair<const char*, std::vector<float>>> inputs;
  inputs.push_back({"tiny noise", noise(1.0, 5u, 1.0e-20f, kRate)});
  std::vector<float> decay = harmonicTone(3.0, 220.0, 0.9f);
  for (size_t i = 0; i < decay.size(); ++i)
    decay[i] *= static_cast<float>(std::exp(-static_cast<double>(i) / kRate * 12.0));
  inputs.push_back({"a note decaying to nothing", decay});
  std::vector<float> square(48000);
  for (size_t i = 0; i < square.size(); ++i)
    square[i] = (i / 100) % 2 == 0 ? 1.0f : -1.0f;
  inputs.push_back({"a full-scale square", square});
  inputs.push_back({"DC", std::vector<float>(48000, 0.5f)});
  inputs.push_back({"loud noise", noise(1.0, 9u, 1.0f, kRate)});
  for (const double range : {0.0, 1.0, 2.0})
    for (const auto& [what, in] : inputs) {
      DirectKind kind(kKind, {{"key", kKeyA}, {"scale", kMinor}, {"speedMs", 0.0}, {"humanize", 50.0},
                              {"range", range}},
                      Layout::Mono);
      std::vector<float> withTail = in;
      withTail.resize(in.size() + 48000, 0.0f);
      expectCleanSignal(kind.render(withTail), 2.0f, what);
    }
}

// ---------------------------------------------------------------------------
// Layouts, mix, readings, latency
// ---------------------------------------------------------------------------

TEST(PitchCorrectTest, DualMonoCorrectsEachInstrumentOnItsOwn) {
  EffectRig rig(kKind, correction(kKeyA, kChromatic), rigFor(Layout::DualMono));
  // Left 430 Hz (to A4), right 202 Hz (to G3, 196 Hz, or G#3, 207.65 Hz: the
  // nearer is G#3, 47 ct up).
  const Stereo out = rig.render(Stereo{sine(3.0, 430.0, 0.3f, kRate), sine(3.0, 202.0, 0.3f, kRate)});
  const double gSharp = hzOfMidi(56);
  EXPECT_NEAR(centsBetween(measuredHz(out.left, 1.0, 3.0, 440.0), 440.0), 0.0, 1.0);
  EXPECT_NEAR(centsBetween(measuredHz(out.right, 1.0, 3.0, gSharp), gSharp), 0.0, 1.0);
  EXPECT_LT(lineDb(out.left, 48000, 144000, gSharp), -60.0);
  EXPECT_LT(lineDb(out.right, 48000, 144000, 440.0), -60.0);
}

TEST(PitchCorrectTest, TheMixBlendsTheCorrectedNoteWithTheDryOne) {
  Params p = correction(kKeyA, kMinor);
  p.back() = {"mix", 0.5};
  EffectRig rig(kKind, p);
  const std::vector<float> in = sine(3.0, 430.0, 0.3f, kRate);
  const std::vector<float> out = rig.render(in);
  const double input = lineDb(in, 48000, 144000, 430.0);
  EXPECT_NEAR(lineDb(out, 48000, 144000, 440.0), input - 6.0, 1.0);
  EXPECT_NEAR(lineDb(out, 48000, 144000, 430.0), input - 6.0, 1.0);
}

TEST(PitchCorrectTest, TheReadingsReportTheNoteAndTheTarget) {
  DirectKind kind(kKind, {{"key", kKeyA}, {"scale", kMinor}, {"speedMs", 0.0}}, Layout::Mono);
  for (int r = 0; r < 4; ++r)
    EXPECT_FLOAT_EQ(kind.engine().getReading(r), 0.0f) << r;
  kind.render(sine(0.5, 430.0, 0.3f, kRate));
  const double played = 69.0 + 12.0 * std::log2(430.0 / 440.0);
  EXPECT_NEAR(kind.engine().getReading(0), played, 0.01);
  EXPECT_NEAR(kind.engine().getReading(1), 430.0, 0.2);
  EXPECT_GT(kind.engine().getReading(2), 95.0f);
  EXPECT_NEAR(kind.engine().getReading(3), 69.0, 1.0e-4);
  // With a detune the target carries it as a fraction.
  kind.engine().setParameter(kind.engine().findParameter("detuneCents"), 25.0f);
  kind.render(sine(0.3, 430.0, 0.3f, kRate));
  EXPECT_NEAR(kind.engine().getReading(3), 69.25, 1.0e-4);
  kind.engine().reset();
  EXPECT_FLOAT_EQ(kind.engine().getReading(3), 0.0f);
}

TEST(PitchCorrectTest, AnAttackArrivesAtTheReportedLatency) {
  EffectRig rig(kKind, correction(kKeyA, kMinor));
  EffectRig reference{juce::String()};
  const std::vector<float> ir = captureImpulseResponse(rig, 0.3);
  const int origin = onsetIndex(captureImpulseResponse(reference, 0.05));
  EXPECT_EQ(onsetIndex(ir) - origin, rig.latencySamples());
  // And a corrected note's attack: its start lands at the same delay.
  std::vector<float> in(2400, 0.0f);
  const std::vector<float> note = sine(0.5, 430.0, 0.5f, kRate);
  in.insert(in.end(), note.begin(), note.end());
  const std::vector<float> out = rig.render(in);
  const std::vector<float> dry = reference.render(in);
  EXPECT_EQ(onsetIndex(out, 0.05f) - onsetIndex(dry, 0.05f), rig.latencySamples());
  for (int w = 0; w < 4; ++w) {
    rig.set("window", w);
    rig.restart();
    std::cout << "[ LATENCY  ] pitch.correct reports " << rig.latencySamples() << " samples ("
              << juce::String(rig.latencySamples() / 48.0, 2) << " ms) with the "
              << PitchShift::windowMs(PitchShift::windowFromIndex(w)) << " ms window" << std::endl;
  }
}

// ---------------------------------------------------------------------------
// Cost and allocation
// ---------------------------------------------------------------------------

TEST(PitchCorrectTest, CostWithANotePlaying) {
  struct Case {
    double range, hz;
    const char* what;
  };
  for (const Case& c : {Case{0.0, 107.5, "guitar, A2 flat"}, Case{1.0, 40.8, "bass, E1 flat"},
                        Case{2.0, 215.0, "voice, A3 flat"}}) {
    const std::vector<float> tone = harmonicTone(1.0, c.hz, 0.3f);
    DirectKind kind(kKind, {{"key", kKeyA}, {"scale", kMinor}, {"speedMs", 20.0}, {"range", c.range}},
                    Layout::Stereo, 128);
    double total = 0.0, worst = 0.0;
    const int blocks = 3000;
    juce::AudioBuffer<float> buffer(2, 128);
    BlockProcessContext ctx;
    const juce::ScopedNoDenormals noDenormals;
    for (int b = 0; b < blocks + 600; ++b) {
      for (int ch = 0; ch < 2; ++ch)
        std::copy_n(tone.data() + (b * 128) % 47872, 128, buffer.getWritePointer(ch));
      const auto begin = std::chrono::steady_clock::now();
      kind.engine().processInChain(buffer, ctx);
      const auto end = std::chrono::steady_clock::now();
      if (b >= 600) {
        const double ns =
            static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
        total += ns;
        worst = std::max(worst, ns);
      }
    }
    std::cout << "[   CPU    ] pitch.correct " << c.what << ": average " << static_cast<long long>(total / blocks)
              << " ns, worst " << static_cast<long long>(worst) << " ns per 128-sample block ("
              << juce::String(100.0 * total / blocks / 2666667.0, 2) << " % of a core)" << std::endl;
    const bool onCi = juce::SystemStats::getEnvironmentVariable("CI", {}).isNotEmpty();
    if (!onCi)
      EXPECT_LT(total / blocks, 40000.0) << c.what;
  }
}

TEST(PitchCorrectTest, NothingAllocatesWhileProcessingOrWhenTheControlsMove) {
  for (const Layout layout : {Layout::Mono, Layout::Stereo, Layout::DualMono}) {
    SCOPED_TRACE(layoutName(layout));
    DirectKind kind(kKind, {{"key", kKeyA}, {"scale", kMinor}, {"speedMs", 30.0}, {"humanize", 50.0}}, layout);
    const int channels = layout == Layout::Mono ? 1 : 2;
    const std::vector<float> source = followedBy(harmonicTone(1.0, 107.5, 0.3f), noise(0.5, 3u, 0.2f, kRate));
    juce::AudioBuffer<float> buffer(channels, 256);
    buffer.clear();
    kind.processor->processInChain(buffer, kind.ctx);
    long total = 0, heap = 0;
    const char* moved[] = {"scale", "range", "key", "speedMs", "amount", "detuneCents", "noteE", "humanize"};
    const float values[] = {10.0f, 1.0f, 4.0f, 0.0f, 60.0f, -20.0f, 0.0f, 0.0f};
    for (int b = 0; b < 600; ++b) {
      for (int ch = 0; ch < channels; ++ch)
        buffer.copyFrom(ch, 0, source.data() + (static_cast<size_t>(b) * 256 % (source.size() - 256)), 256);
      if (b % 60 == 30) {
        const int which = (b / 60) % 8;
        kind.engine().setParameter(kind.engine().findParameter(moved[which]), values[which]);
      }
      if (b == 400)
        kind.engine().setParameter(kind.engine().findParameter("range"), 2.0f);
      AllocationGuard guard;
      kind.processor->processInChain(buffer, kind.ctx);
      total += guard.count();
      heap += guard.heapCount();
    }
    EXPECT_EQ(total, 0);
    EXPECT_EQ(heap, 0);
  }
}

TEST(PitchCorrectTest, TheTailFollowsTheWindow) {
  for (int w = 0; w < 4; ++w) {
    const Params p = {{"window", static_cast<double>(w)}};
    const TailMeasurement tail = expectSilenceAfterReportedTail(kKind, p);
    EXPECT_FALSE(tail.capped);
    EXPECT_NEAR(tail.reportedSeconds, 2.0 * PitchShift::windowMs(PitchShift::windowFromIndex(w)) * 0.001, 1.0e-9);
  }
}

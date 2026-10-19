// The pitch family's monophonic tracker (plugin/src/effects/pitch/detail/
// PitchTracker.h, McLeod's method): the accuracy of the frequency it reports
// on guitar-range notes (82 Hz to 1.3 kHz), how long it takes to know a new
// note, and how it behaves when there is no note: silence, noise, a note that
// ends in noise, a chord. It is tested alone, fed sample by sample and stepped
// on the 16-sample grid the kinds step it on. The bass and voice ranges
// (pitch.correct) are tested at the end.
#include "../../../plugin/src/effects/pitch/detail/PitchTracker.h"
#include "effects/pitch_test_signals.h"

#include <gtest/gtest.h>

using namespace pitchtest;

namespace {

// What a run of the tracker over a signal leaves behind, per sample.
struct Trace {
  std::vector<float> frequency;
  std::vector<float> confidence;
  std::vector<int> note;
  std::vector<char> voiced;
  std::vector<char> hasNote;
};

Trace runTracker(const std::vector<float>& in, double sampleRate = kRate,
                 const pitchfx::PitchTracker::Range& range = pitchfx::PitchTracker::Range::guitar()) {
  pitchfx::PitchTracker tracker;
  tracker.prepare(sampleRate, range);
  Trace trace;
  const size_t n = in.size();
  trace.frequency.resize(n);
  trace.confidence.resize(n);
  trace.note.resize(n);
  trace.voiced.resize(n);
  trace.hasNote.resize(n);
  for (size_t i = 0; i < n; ++i) {
    tracker.push(in[i]);
    if ((i & 15u) == 15u)
      tracker.step();
    trace.frequency[i] = tracker.frequencyHz();
    trace.confidence[i] = tracker.confidence();
    trace.note[i] = tracker.note();
    trace.voiced[i] = tracker.isVoiced() ? 1 : 0;
    trace.hasNote[i] = tracker.hasNote() ? 1 : 0;
  }
  return trace;
}

int midiOf(double hz) { return static_cast<int>(std::lround(69.0 + 12.0 * std::log2(hz / 440.0))); }

// Open strings and a spread of the neck, 82 Hz to 1.32 kHz.
const double kNotes[] = {82.407, 110.0, 146.832, 195.998, 246.942, 329.628, 440.0, 659.255, 880.0, 1318.51};

std::vector<float> guitarLike(double seconds, double hz, float amplitude) {
  // Partials 1..8 at 1/n with a slow decay, as a plucked string rings.
  std::vector<float> x = harmonicTone(seconds, hz, amplitude);
  for (size_t i = 0; i < x.size(); ++i)
    x[i] *= static_cast<float>(std::exp(-static_cast<double>(i) / kRate * 0.8));
  return x;
}

}  // namespace

TEST(PitchTrackerTest, ReportsTheFrequencyOfEveryGuitarNoteWithinACent) {
  double worstSine = 0.0, worstGuitar = 0.0;
  for (const double hz : kNotes) {
    for (const bool sineWave : {true, false}) {
      const std::vector<float> in = sineWave ? sine(0.5, hz, 0.3f, kRate) : harmonicTone(0.5, hz, 0.3f);
      const Trace trace = runTracker(in);
      // The reading at the end of the half second.
      const size_t last = in.size() - 1;
      const double cents = centsBetween(trace.frequency[last], hz);
      EXPECT_TRUE(trace.voiced[last]) << hz << " Hz";
      EXPECT_GT(trace.confidence[last], 0.95f) << hz << " Hz";
      EXPECT_EQ(trace.note[last], midiOf(hz)) << hz << " Hz";
      EXPECT_NEAR(cents, 0.0, 1.0) << (sineWave ? "sine " : "harmonic tone ") << hz << " Hz";
      (sineWave ? worstSine : worstGuitar) = std::max(sineWave ? worstSine : worstGuitar, std::abs(cents));
    }
  }
  std::cout << "[  TRACKER ] worst frequency error over 82 Hz to 1.32 kHz: sines " << juce::String(worstSine, 3)
            << " ct, harmonic tones " << juce::String(worstGuitar, 3) << " ct" << std::endl;
}

TEST(PitchTrackerTest, FollowsNotesBetweenTheSemitonesToo) {
  // 100 Hz, 150 Hz, 1 kHz are not on the equal-tempered grid: the frequency is
  // reported as it is and the stable note is the nearest.
  double worst = 0.0;
  for (const double hz : {100.0, 150.0, 233.0, 777.7, 1000.0}) {
    const Trace trace = runTracker(harmonicTone(0.5, hz, 0.3f));
    const double cents = centsBetween(trace.frequency.back(), hz);
    EXPECT_NEAR(cents, 0.0, 1.0) << hz;
    EXPECT_EQ(trace.note.back(), midiOf(hz)) << hz;
    worst = std::max(worst, std::abs(cents));
  }
  std::cout << "[  TRACKER ] worst error off the semitone grid: " << juce::String(worst, 3) << " ct" << std::endl;
}

TEST(PitchTrackerTest, ASoundWithAWeakFundamentalIsNotOctaveDoubled) {
  // The fundamental 12 dB under its second and third harmonics: a plain
  // autocorrelation or a threshold tracker reads the octave.
  for (const double hz : {110.0, 196.0, 330.0}) {
    std::vector<float> x(static_cast<size_t>(0.6 * kRate), 0.0f);
    for (size_t i = 0; i < x.size(); ++i) {
      const double t = static_cast<double>(i) / kRate;
      x[i] = static_cast<float>(0.07 * std::sin(2.0 * kPi * hz * t) + 0.28 * std::sin(2.0 * kPi * 2.0 * hz * t + 1.0) +
                                0.25 * std::sin(2.0 * kPi * 3.0 * hz * t + 2.0) + 0.1 * std::sin(2.0 * kPi * 4.0 * hz * t));
    }
    const Trace trace = runTracker(x);
    EXPECT_NEAR(centsBetween(trace.frequency.back(), hz), 0.0, 2.0) << hz << " Hz read as " << trace.frequency.back();
  }
}

TEST(PitchTrackerTest, KnowsANewNoteQuicklyAndTheLowStringsALittleLater) {
  // A note after 100 ms of silence: the time from its first sample to the
  // first voiced frame, and to the stable (two frames running) correct note.
  double worstHigh = 0.0, worstLow = 0.0;
  for (const double hz : kNotes) {
    const size_t start = 4800;
    std::vector<float> in(start, 0.0f);
    const std::vector<float> note = guitarLike(0.4, hz, 0.4f);
    in.insert(in.end(), note.begin(), note.end());
    const Trace trace = runTracker(in);
    long firstVoiced = -1, stable = -1;
    for (size_t i = start; i < in.size(); ++i) {
      if (firstVoiced < 0 && trace.voiced[i])
        firstVoiced = static_cast<long>(i - start);
      if (stable < 0 && trace.note[i] == midiOf(hz))
        stable = static_cast<long>(i - start);
    }
    ASSERT_GE(stable, 0) << hz << " Hz was never recognised";
    const double ms = 1000.0 * static_cast<double>(stable) / kRate;
    std::cout << "[  TRACKER ] " << juce::String(hz, 1) << " Hz: first confident frame after "
              << juce::String(1000.0 * static_cast<double>(firstVoiced) / kRate, 1) << " ms, stable note after "
              << juce::String(ms, 1) << " ms" << std::endl;
    // The high notes (above 250 Hz) are known from the short window.
    if (hz > 250.0) {
      EXPECT_LT(ms, 30.0) << hz;
      worstHigh = std::max(worstHigh, ms);
    } else {
      EXPECT_LT(ms, 50.0) << hz;
      worstLow = std::max(worstLow, ms);
    }
    // And once known, it stays known to the end of the note.
    for (size_t i = start + static_cast<size_t>(stable) + 480; i < in.size(); ++i)
      ASSERT_EQ(trace.note[i], midiOf(hz)) << hz << " Hz lost at " << (i - start);
  }
  std::cout << "[  TRACKER ] slowest stable note: " << juce::String(worstHigh, 1) << " ms above 250 Hz, "
            << juce::String(worstLow, 1) << " ms below" << std::endl;
}

TEST(PitchTrackerTest, FollowsANoteChange) {
  // E3 to A3 with a hard cut, as a new note is played on a ringing string.
  const std::vector<float> in = followedBy(harmonicTone(0.3, 164.814, 0.3f), harmonicTone(0.3, 220.0, 0.3f));
  const Trace trace = runTracker(in);
  const size_t change = 14400;
  EXPECT_EQ(trace.note[change - 1], midiOf(164.814));
  long found = -1;
  for (size_t i = change; i < in.size(); ++i)
    if (trace.note[i] == midiOf(220.0)) {
      found = static_cast<long>(i - change);
      break;
    }
  ASSERT_GE(found, 0);
  std::cout << "[  TRACKER ] a hard note change (E3 to A3) is known after " << juce::String(1000.0 * found / kRate, 1)
            << " ms" << std::endl;
  EXPECT_LT(found, 0.040 * kRate);
}

TEST(PitchTrackerTest, FollowsABendWithoutChangingTheNoteUntilItIsOne) {
  // 220 Hz bent up 40 cents in a second: the frequency follows it, and the
  // stable note stays on A3 (a bend under 65 cents is not a new note).
  const int frames = static_cast<int>(1.0 * kRate);
  std::vector<float> in(static_cast<size_t>(frames));
  double phase = 0.0;
  for (int i = 0; i < frames; ++i) {
    const double cents = 40.0 * i / frames;
    phase += 2.0 * kPi * 220.0 * std::pow(2.0, cents / 1200.0) / kRate;
    in[static_cast<size_t>(i)] = 0.3f * static_cast<float>(std::sin(phase) + 0.4 * std::sin(2.0 * phase));
  }
  const Trace trace = runTracker(in);
  for (const double at : {0.25, 0.5, 0.75, 0.98}) {
    const size_t i = static_cast<size_t>(at * kRate);
    const double expected = 220.0 * std::pow(2.0, 40.0 * at / 1200.0);
    EXPECT_NEAR(centsBetween(trace.frequency[i], expected), 0.0, 4.0) << "at " << at << " s";
    EXPECT_EQ(trace.note[i], 57);
  }
}

TEST(PitchTrackerTest, SilenceIsNotANote) {
  const Trace trace = runTracker(std::vector<float>(48000, 0.0f));
  for (size_t i = 0; i < trace.note.size(); ++i) {
    ASSERT_FALSE(trace.voiced[i]);
    ASSERT_FALSE(trace.hasNote[i]);
    ASSERT_EQ(trace.note[i], -1);
    ASSERT_EQ(trace.confidence[i], 0.0f);
    ASSERT_EQ(trace.frequency[i], 0.0f);
  }
  // Below the floor (-60 dBFS) a tone is silence too: a noise gate's leakage
  // and the hiss of a high-gain chain must not be sung along with.
  const Trace quiet = runTracker(sine(0.5, 220.0, 0.0006f, kRate));
  EXPECT_FALSE(quiet.hasNote.back());
  EXPECT_EQ(quiet.confidence.back(), 0.0f);
}

TEST(PitchTrackerTest, NoiseIsNotANoteAndReadsAsLowConfidence) {
  for (const unsigned seed : {1u, 2u, 3u}) {
    const Trace trace = runTracker(noise(2.0, seed, 0.3f, kRate));
    size_t voicedFrames = 0;
    double confidenceSum = 0.0;
    for (size_t i = 0; i < trace.voiced.size(); ++i) {
      voicedFrames += trace.voiced[i] ? 1u : 0u;
      confidenceSum += static_cast<double>(trace.confidence[i]);
    }
    const double voicedShare = static_cast<double>(voicedFrames) / static_cast<double>(trace.voiced.size());
    const double meanConfidence = confidenceSum / static_cast<double>(trace.voiced.size());
    std::cout << "[  TRACKER ] white noise seed " << seed << ": voiced " << juce::String(100.0 * voicedShare, 2)
              << " % of the time, mean confidence " << juce::String(meanConfidence, 3) << std::endl;
    EXPECT_LT(voicedShare, 0.02);
    EXPECT_LT(meanConfidence, 0.5);
  }
}

TEST(PitchTrackerTest, HoldsTheLastGoodNoteWhenConfidenceDrops) {
  // A note, then noise: the frequency and the note stay where they were, the
  // confidence says the signal is no longer a note.
  const std::vector<float> in = followedBy(harmonicTone(0.4, 196.0, 0.3f), noise(0.5, 9u, 0.3f, kRate));
  const Trace trace = runTracker(in);
  const size_t before = 19199;
  ASSERT_TRUE(trace.voiced[before]);
  // The frames that straddle the cut can still read as a note, a little off
  // (the window holds the end of the string and the start of the noise); from
  // 60 ms on, the note held is constant and within 30 cents of the note played.
  const float held = trace.frequency[22400];
  EXPECT_NEAR(centsBetween(held, 196.0), 0.0, 30.0);
  for (size_t i = 22400; i < in.size(); i += 97) {
    EXPECT_EQ(trace.frequency[i], held) << "at " << i;
    EXPECT_EQ(trace.note[i], midiOf(196.0));
    EXPECT_TRUE(trace.hasNote[i]);
  }
  size_t unvoiced = 0, total = 0;
  for (size_t i = 24000; i < in.size(); ++i, ++total)
    unvoiced += trace.voiced[i] ? 0u : 1u;
  EXPECT_GT(static_cast<double>(unvoiced) / static_cast<double>(total), 0.97);
  EXPECT_LT(trace.confidence.back(), 0.7f);

  // Silence after a note does not forget it either (a voice that is waiting
  // for the next note keeps its interval); only a reset does.
  const Trace silent = runTracker(followedBy(harmonicTone(0.3, 196.0, 0.3f), std::vector<float>(48000, 0.0f)));
  EXPECT_TRUE(silent.hasNote.back());
  EXPECT_EQ(silent.note.back(), midiOf(196.0));
  EXPECT_EQ(silent.confidence.back(), 0.0f);
  EXPECT_FALSE(silent.voiced.back());

  pitchfx::PitchTracker tracker;
  tracker.prepare(kRate);
  for (size_t i = 0; i < 9600; ++i) {
    tracker.push(0.3f * static_cast<float>(std::sin(2.0 * kPi * 196.0 * static_cast<double>(i) / kRate)));
    if ((i & 15u) == 15u)
      tracker.step();
  }
  ASSERT_TRUE(tracker.hasNote());
  tracker.reset();
  EXPECT_FALSE(tracker.hasNote());
  EXPECT_EQ(tracker.note(), -1);
  EXPECT_EQ(tracker.frequencyHz(), 0.0f);
}

TEST(PitchTrackerTest, AChordReadsAsLowerConfidenceThanANote) {
  // Three notes of an open C chord against one: the tracker may pick one of
  // them or none, but its confidence says it is less sure, and it never
  // reports something that is not in the chord.
  std::vector<float> chord(static_cast<size_t>(0.6 * kRate), 0.0f);
  for (const double hz : {130.81, 164.81, 196.0}) {
    const std::vector<float> one = harmonicTone(0.6, hz, 0.15f, 5);
    for (size_t i = 0; i < chord.size(); ++i)
      chord[i] += one[i];
  }
  const Trace trace = runTracker(chord);
  const Trace single = runTracker(harmonicTone(0.6, 164.81, 0.3f, 5));
  double chordConfidence = 0.0, singleConfidence = 0.0;
  for (size_t i = 24000; i < chord.size(); ++i) {
    chordConfidence += static_cast<double>(trace.confidence[i]);
    singleConfidence += static_cast<double>(single.confidence[i]);
    if (trace.voiced[i]) {
      const double hz = trace.frequency[i];
      const bool inChord = std::abs(centsBetween(hz, 130.81)) < 30 || std::abs(centsBetween(hz, 164.81)) < 30 ||
                           std::abs(centsBetween(hz, 196.0)) < 30 || std::abs(centsBetween(hz, 65.4)) < 30;
      EXPECT_TRUE(inChord) << hz << " Hz at " << i;
    }
  }
  const double count = static_cast<double>(chord.size() - 24000);
  std::cout << "[  TRACKER ] mean confidence: one note " << juce::String(singleConfidence / count, 3)
            << ", a three-note chord " << juce::String(chordConfidence / count, 3) << std::endl;
  EXPECT_LT(chordConfidence, singleConfidence);
}

TEST(PitchTrackerTest, WorksAtOtherSampleRates) {
  for (const double rate : {44100.0, 96000.0}) {
    pitchfx::PitchTracker tracker;
    tracker.prepare(rate);
    for (int i = 0; i < static_cast<int>(0.5 * rate); ++i) {
      tracker.push(0.3f * static_cast<float>(std::sin(2.0 * kPi * 330.0 * i / rate)));
      if ((i & 15) == 15)
        tracker.step();
    }
    EXPECT_NEAR(centsBetween(tracker.frequencyHz(), 330.0), 0.0, 1.0) << rate;
    EXPECT_EQ(tracker.note(), 64) << rate;
  }
}

TEST(PitchTrackerTest, CostIsASmallShareOfACore) {
  // The tracker alone, per 128-sample block at 48 kHz, on a guitar-like tone
  // (a voiced signal is the dear case: it also refines).
  pitchfx::PitchTracker tracker;
  tracker.prepare(kRate);
  const std::vector<float> in = harmonicTone(1.0, 110.0, 0.3f);
  const int blocks = 6000;
  double worst = 0.0, total = 0.0;
  for (int b = 0; b < blocks; ++b) {
    const auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < 128; ++i) {
      tracker.push(in[static_cast<size_t>((b * 128 + i) % 48000)]);
      if ((i & 15) == 15)
        tracker.step();
    }
    const auto end = std::chrono::steady_clock::now();
    const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
    total += ns;
    worst = std::max(worst, ns);
  }
  std::cout << "[  TRACKER ] cost: " << static_cast<long long>(total / blocks) << " ns per 128-sample block on average ("
            << juce::String(100.0 * total / blocks / 2666667.0, 2) << " % of a core), worst block "
            << static_cast<long long>(worst) << " ns" << std::endl;
  const bool onCi = juce::SystemStats::getEnvironmentVariable("CI", {}).isNotEmpty();
  if (!onCi)
    EXPECT_LT(total / blocks, 40000.0);
}

// ---------------------------------------------------------------------------
// The other ranges
// ---------------------------------------------------------------------------

TEST(PitchTrackerTest, TheGuitarRangeIsWhatPrepareGivesWithoutOne) {
  // The kinds that do not name a range get the guitar's, to the bit.
  const std::vector<float> in = followedBy(harmonicTone(0.4, 110.0, 0.3f), noise(0.2, 4u, 0.2f, kRate));
  const Trace implicit = runTracker(in);
  pitchfx::PitchTracker tracker;
  tracker.prepare(kRate, pitchfx::PitchTracker::Range::guitar());
  for (size_t i = 0; i < in.size(); ++i) {
    tracker.push(in[i]);
    if ((i & 15u) == 15u)
      tracker.step();
    ASSERT_EQ(tracker.frequencyHz(), implicit.frequency[i]) << i;
    ASSERT_EQ(tracker.confidence(), implicit.confidence[i]) << i;
  }
}

TEST(PitchTrackerTest, TheBassRangeReadsABassDownToTheLowB) {
  // B0 (five-string), E1, A1, D2, G2 and two higher notes, sines and
  // bass-like tones, within a cent; and how long a new note takes.
  const auto bass = pitchfx::PitchTracker::Range::bass();
  const double notes[] = {30.868, 41.203, 55.0, 73.416, 97.999, 196.0, 392.0};
  double worst = 0.0, slowest = 0.0;
  for (const double hz : notes) {
    for (const bool sineWave : {true, false}) {
      const std::vector<float> in = sineWave ? sine(0.6, hz, 0.3f, kRate) : harmonicTone(0.6, hz, 0.3f, 10);
      const Trace trace = runTracker(in, kRate, bass);
      const double cents = centsBetween(trace.frequency.back(), hz);
      EXPECT_TRUE(trace.voiced.back()) << hz;
      EXPECT_EQ(trace.note.back(), midiOf(hz)) << hz;
      EXPECT_NEAR(cents, 0.0, 1.0) << (sineWave ? "sine " : "bass tone ") << hz << " Hz";
      worst = std::max(worst, std::abs(cents));
    }
    std::vector<float> in(4800, 0.0f);
    const std::vector<float> note = guitarLike(0.5, hz, 0.4f);
    in.insert(in.end(), note.begin(), note.end());
    const Trace trace = runTracker(in, kRate, bass);
    long stable = -1;
    for (size_t i = 4800; i < in.size() && stable < 0; ++i)
      if (trace.note[i] == midiOf(hz))
        stable = static_cast<long>(i - 4800);
    ASSERT_GE(stable, 0) << hz;
    const double ms = 1000.0 * static_cast<double>(stable) / kRate;
    slowest = std::max(slowest, ms);
    std::cout << "[  TRACKER ] bass range, " << juce::String(hz, 2) << " Hz: stable note after "
              << juce::String(ms, 1) << " ms" << std::endl;
    EXPECT_LT(ms, 110.0) << hz;
  }
  std::cout << "[  TRACKER ] bass range: worst frequency error " << juce::String(worst, 3) << " ct, slowest note "
            << juce::String(slowest, 1) << " ms" << std::endl;
  // The guitar range does not reach the low E of a bass.
  const Trace guitar = runTracker(sine(0.6, 41.203, 0.3f, kRate));
  EXPECT_NE(guitar.note.back(), midiOf(41.203));
}

TEST(PitchTrackerTest, TheBassRangeWorksAtOtherSampleRates) {
  for (const double rate : {44100.0, 96000.0}) {
    pitchfx::PitchTracker tracker;
    tracker.prepare(rate, pitchfx::PitchTracker::Range::bass());
    for (int i = 0; i < static_cast<int>(0.6 * rate); ++i) {
      tracker.push(0.3f * static_cast<float>(std::sin(2.0 * kPi * 41.203 * i / rate)));
      if ((i & 15) == 15)
        tracker.step();
    }
    EXPECT_NEAR(centsBetween(tracker.frequencyHz(), 41.203), 0.0, 1.0) << rate;
    EXPECT_EQ(tracker.note(), 28) << rate;
  }
}

TEST(PitchTrackerTest, TheBassAndVoiceRangesStillTakeNoiseForNoNote) {
  for (const auto& [name, range] : {std::pair<const char*, pitchfx::PitchTracker::Range>{"bass", pitchfx::PitchTracker::Range::bass()},
                                    std::pair<const char*, pitchfx::PitchTracker::Range>{"voice", pitchfx::PitchTracker::Range::voice()}}) {
    for (const unsigned seed : {1u, 2u}) {
      const Trace trace = runTracker(noise(2.0, seed, 0.3f, kRate), kRate, range);
      size_t voicedFrames = 0;
      for (const char v : trace.voiced)
        voicedFrames += v ? 1u : 0u;
      const double share = static_cast<double>(voicedFrames) / static_cast<double>(trace.voiced.size());
      std::cout << "[  TRACKER ] " << name << " range, white noise seed " << seed << ": voiced "
                << juce::String(100.0 * share, 2) << " % of the time" << std::endl;
      EXPECT_LT(share, 0.03) << name;
    }
  }
}

TEST(PitchTrackerTest, CostOfTheBassRange) {
  pitchfx::PitchTracker tracker;
  tracker.prepare(kRate, pitchfx::PitchTracker::Range::bass());
  const std::vector<float> in = harmonicTone(1.0, 41.203, 0.3f, 10);
  const int blocks = 6000;
  double worst = 0.0, total = 0.0;
  for (int b = 0; b < blocks; ++b) {
    const auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < 128; ++i) {
      tracker.push(in[static_cast<size_t>((b * 128 + i) % 48000)]);
      if ((i & 15) == 15)
        tracker.step();
    }
    const auto end = std::chrono::steady_clock::now();
    const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
    total += ns;
    worst = std::max(worst, ns);
  }
  std::cout << "[  TRACKER ] bass range cost: " << static_cast<long long>(total / blocks)
            << " ns per 128-sample block on average (" << juce::String(100.0 * total / blocks / 2666667.0, 2)
            << " % of a core), worst block " << static_cast<long long>(worst) << " ns" << std::endl;
  const bool onCi = juce::SystemStats::getEnvironmentVariable("CI", {}).isNotEmpty();
  if (!onCi)
    EXPECT_LT(total / blocks, 40000.0);
}

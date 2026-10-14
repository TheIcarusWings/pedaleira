// The looper (plugin/include/Looper.h, docs/looper.md): the engine on its own
// (Looper, driven block by block the way the processor drives it), then in the
// processor through the command layer and the MIDI map.
//
// Engine tests use a fixed 128-frame block and a loop length that is a whole
// number of blocks, so an action requested between two calls lands on a block
// boundary, as the processor's would, and the length is exactly what was
// recorded. The numbers the tests measure are printed as "[   LOOPER ]" lines.
#include "Looper.h"

#include "effects/allocation_guard.h"
#include "effects/effect_test_helpers.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <functional>
#include <thread>
#include <vector>

using namespace fxtest;
using Action = Looper::Action;
using State = Looper::State;

namespace {

constexpr double kRate = 48000.0;
constexpr int kBlock = 128;
// 100 blocks: 12800 frames, 0.2667 s.
constexpr int kLoop = 100 * kBlock;
// The fade at each end of the loop: 2 ms, 96 frames at 48 kHz.
constexpr int kFade = 96;

void printLooper(const std::string& text) { std::cout << "[   LOOPER ] " << text << std::endl; }

std::string num(double value, int digits = 3) { return juce::String(value, digits).toStdString(); }

float fadeShape(int k) {
  constexpr float kPi = 3.14159265358979f;
  return 0.5f - 0.5f * std::cos(kPi * (static_cast<float>(k) + 0.5f) / static_cast<float>(kFade));
}

// What the loop does to a frame of the loop at `index` of `length`.
float fadeAt(int index, int length) {
  if (index < kFade)
    return fadeShape(index);
  if (length - 1 - index < kFade)
    return fadeShape(length - 1 - index);
  return 1.0f;
}

std::vector<float> zeros(int frames) { return std::vector<float>(static_cast<size_t>(frames), 0.0f); }

// A looper at 48 kHz with the helpers to drive it.
struct Harness {
  Looper looper;
  double bpm{120.0};

  Harness() { looper.prepare(kRate); }

  // A record action reserves the loop's memory on the looper's worker; the
  // engine tests that go on to record wait for it, so what they measure is
  // the loop (the lazy reservation has tests of its own).
  void act(Action action) {
    ASSERT_TRUE(looper.request(action));
    if (action == Action::record || action == Action::cycle)
      ASSERT_TRUE(looper.waitForMemory(5000));
  }

  /** Runs `in` through the looper in blocks of kBlock (a shorter last block
      if need be), on `channels` channels (the second is `right`, or a copy of
      the left), and returns what comes out. */
  Stereo run(const std::vector<float>& left, const std::vector<float>* right = nullptr,
             int channels = 2) {
    Stereo out;
    out.left = left;
    out.right = right != nullptr ? *right : left;
    for (size_t offset = 0; offset < left.size(); offset += kBlock) {
      const int n = static_cast<int>(std::min<size_t>(kBlock, left.size() - offset));
      float* pointers[2] = {out.left.data() + offset, out.right.data() + offset};
      looper.process(pointers, channels, n, bpm);
    }
    return out;
  }

  std::vector<float> runMono(const std::vector<float>& in) { return run(in, nullptr, 1).left; }

  /** Records `in` (a whole number of blocks) as the loop and starts playing. */
  void recordAndPlay(const std::vector<float>& in) {
    act(Action::record);
    run(in);
    act(Action::play);
    run(zeros(kBlock));  // the play lands; position moves on by one block
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// Record and play
// ---------------------------------------------------------------------------

TEST(LooperTest, RecordThenPlayReproducesTheInputToTheSample) {
  Harness h;
  const std::vector<float> in = makeNoise(kLoop, 5u, 0.3f);

  h.act(Action::record);
  const Stereo whileRecording = h.run(in);
  EXPECT_EQ(whileRecording.left, in) << "recording leaves the rig's sound untouched";
  EXPECT_EQ(h.looper.snapshot().state, State::recording);

  h.act(Action::play);
  const Stereo played = h.run(zeros(3 * kLoop));
  const Looper::Snapshot snapshot = h.looper.snapshot();
  EXPECT_EQ(snapshot.state, State::playing);
  EXPECT_EQ(snapshot.lengthFrames, kLoop);

  // The loop begins on the very frame after the recording ended: pass one is
  // the recording, frame for frame. Apart from the fades at the two ends the
  // samples are the input's own, bit for bit.
  int different = 0;
  for (int k = kFade; k < kLoop - kFade; ++k)
    if (played.left[static_cast<size_t>(k)] != in[static_cast<size_t>(k)])
      ++different;
  EXPECT_EQ(different, 0);
  float worstFade = 0.0f;
  for (int k = 0; k < kLoop; ++k) {
    const float expected = in[static_cast<size_t>(k)] * fadeAt(k, kLoop);
    worstFade = std::max(worstFade, std::abs(played.left[static_cast<size_t>(k)] - expected));
  }
  EXPECT_LT(worstFade, 1.0e-6f);
  // Both sides, and every pass the same.
  EXPECT_EQ(played.left, played.right);
  int passesDiffer = 0;
  for (int k = 0; k < kLoop; ++k)
    if (played.left[static_cast<size_t>(k)] != played.left[static_cast<size_t>(k + kLoop)] ||
        played.left[static_cast<size_t>(k)] != played.left[static_cast<size_t>(k + 2 * kLoop)])
      ++passesDiffer;
  EXPECT_EQ(passesDiffer, 0);
  printLooper("record then play, " + std::to_string(kLoop) + " frames: " +
              std::to_string(different) + " samples differ from the input outside the " +
              std::to_string(kFade) + "-frame fades, largest difference from input x fade " +
              num(worstFade, 9) + ", three passes identical");
}

TEST(LooperTest, TheSeamIsFaded) {
  Harness h;
  const std::vector<float> in = makeNoise(kLoop, 6u, 0.5f);
  h.recordAndPlay(in);
  const Stereo played = h.run(zeros(3 * kLoop));
  // recordAndPlay ran one block of the first pass, so the seam between the
  // first pass and the second is a block short of a loop in.
  const size_t seam = static_cast<size_t>(kLoop - kBlock);
  const float rawJump = std::abs(in.front() - in.back());
  const float tail = std::abs(played.left[seam - 1]), head = std::abs(played.left[seam]);
  const float step = std::abs(played.left[seam] - played.left[seam - 1]);
  printLooper("loop seam: last frame " + num(tail, 6) + ", first frame " + num(head, 6) +
              ", step across it " + num(step, 6) + " (the input's own end-to-start jump " +
              num(rawJump, 3) + ")");
  EXPECT_LT(tail, 1.0e-3f);
  EXPECT_LT(head, 1.0e-3f);
  // No step anywhere in the pass is as big as a click would be.
  float largest = 0.0f;
  for (size_t i = seam - 2 * kFade; i < seam + 2 * kFade; ++i)
    largest = std::max(largest, std::abs(played.left[i] - played.left[i - 1]));
  EXPECT_LT(largest, 2.0f * 0.5f) << "no step bigger than the noise's own";
}

TEST(LooperTest, TheLoopIsAddedToTheLiveSound) {
  Harness h;
  const std::vector<float> a = makeNoise(kLoop, 7u, 0.2f);
  h.recordAndPlay(a);
  // Playing the loop never touches what comes in: live + loop.
  const std::vector<float> live = makeNoise(kLoop, 8u, 0.2f);
  const Stereo out = h.run(live);
  // The recordAndPlay call advanced the position by one block.
  float worst = 0.0f;
  for (int k = 0; k < kLoop; ++k) {
    const int loopIndex = (k + kBlock) % kLoop;
    const float expected =
        live[static_cast<size_t>(k)] + a[static_cast<size_t>(loopIndex)] * fadeAt(loopIndex, kLoop);
    worst = std::max(worst, std::abs(out.left[static_cast<size_t>(k)] - expected));
  }
  EXPECT_LT(worst, 1.0e-6f);
}

// ---------------------------------------------------------------------------
// Overdub, undo, redo
// ---------------------------------------------------------------------------

TEST(LooperTest, OverdubSumsAndUndoRestores) {
  Harness h;
  const std::vector<float> a = makeNoise(kLoop, 11u, 0.25f);
  const std::vector<float> b = makeNoise(kLoop, 12u, 0.25f);

  h.act(Action::record);
  h.run(a);
  // Overdub from the end of the first recording: it begins at the top of the
  // loop and hears the first layer while it records the second.
  h.act(Action::overdub);
  const Stereo whileOverdubbing = h.run(b);
  EXPECT_EQ(h.looper.snapshot().state, State::overdubbing);
  float worstHeard = 0.0f;
  for (int k = 0; k < kLoop; ++k)
    worstHeard = std::max(worstHeard, std::abs(whileOverdubbing.left[static_cast<size_t>(k)] -
                                               (b[static_cast<size_t>(k)] +
                                                a[static_cast<size_t>(k)] * fadeAt(k, kLoop))));
  EXPECT_LT(worstHeard, 1.0e-6f) << "overdubbing hears the loop beside the live sound";

  h.act(Action::play);
  const Stereo summed = h.run(zeros(2 * kLoop));
  EXPECT_EQ(h.looper.snapshot().layers, 2);
  int different = 0;
  float worst = 0.0f;
  for (int k = 0; k < kLoop; ++k) {
    const float expected = (a[static_cast<size_t>(k)] + b[static_cast<size_t>(k)]) * fadeAt(k, kLoop);
    worst = std::max(worst, std::abs(summed.left[static_cast<size_t>(k)] - expected));
    if (k >= kFade && k < kLoop - kFade &&
        summed.left[static_cast<size_t>(k)] != a[static_cast<size_t>(k)] + b[static_cast<size_t>(k)])
      ++different;
  }
  EXPECT_LT(worst, 1.0e-6f);
  EXPECT_EQ(different, 0);
  printLooper("overdub: loop is the sum of the layers, largest difference " + num(worst, 9) +
              ", " + std::to_string(different) + " interior samples not exactly a + b");

  // Undo takes the second layer out: the first recording alone, bit for bit.
  h.act(Action::undo);
  const Stereo undone = h.run(zeros(kLoop));
  EXPECT_EQ(h.looper.snapshot().layers, 1);
  EXPECT_EQ(h.looper.snapshot().redoLayers, 1);
  int undoneDifferent = 0;
  for (int k = kFade; k < kLoop - kFade; ++k)
    if (undone.left[static_cast<size_t>(k)] != a[static_cast<size_t>(k)])
      ++undoneDifferent;
  EXPECT_EQ(undoneDifferent, 0);
  printLooper("undo: the first recording again, " + std::to_string(undoneDifferent) +
              " interior samples differ from it");

  // Redo puts it back; undo with only the recording left does nothing.
  h.act(Action::redo);
  const Stereo redone = h.run(zeros(kLoop));
  EXPECT_EQ(h.looper.snapshot().layers, 2);
  float redoneWorst = 0.0f;
  for (int k = 0; k < kLoop; ++k)
    redoneWorst = std::max(redoneWorst, std::abs(redone.left[static_cast<size_t>(k)] -
                                                 summed.left[static_cast<size_t>(k)]));
  EXPECT_EQ(redoneWorst, 0.0f) << "redo restores exactly what undo took out";
  h.act(Action::undo);
  h.act(Action::undo);
  h.run(zeros(kBlock));
  EXPECT_EQ(h.looper.snapshot().layers, 1) << "the first recording is not undone";
}

TEST(LooperTest, AnOverdubKeepsOnlyWhatItCoveredAndStartsWhereItWasPressed) {
  Harness h;
  const std::vector<float> a = makeNoise(kLoop, 21u, 0.25f);
  h.recordAndPlay(a);  // the position is now one block in
  // Press overdub, play over the next half loop, then play on.
  const std::vector<float> c = makeNoise(kLoop / 2, 22u, 0.25f);
  h.act(Action::overdub);
  h.run(c);
  h.act(Action::play);
  // Position now: the overdub began at frame kBlock and ran kLoop / 2 frames.
  const size_t start = kBlock, count = kLoop / 2;
  const Stereo out = h.run(zeros(kLoop));
  // Play landed where the layer ended, so out[0] is loop index start + count.
  int wrong = 0;
  const size_t first = (start + count) % kLoop;
  for (int k = kFade + 5; k < kLoop - kFade - 5; ++k) {
    const size_t index = (first + static_cast<size_t>(k)) % kLoop;
    float expected = a[index];
    const size_t delta = (index + kLoop - start) % kLoop;
    if (delta < count)
      expected += c[delta];
    expected *= fadeAt(static_cast<int>(index), kLoop);
    if (std::abs(out.left[static_cast<size_t>(k)] - expected) > 1.0e-6f)
      ++wrong;
  }
  EXPECT_EQ(wrong, 0) << "the layer sounds only over the half it recorded, from where it began";
  EXPECT_EQ(h.looper.snapshot().layers, 2);
}

TEST(LooperTest, UndoWhileOverdubbingThrowsTheLayerAwayAndKeepsPlaying) {
  Harness h;
  const std::vector<float> a = makeNoise(kLoop, 31u, 0.25f);
  h.recordAndPlay(a);
  h.act(Action::overdub);
  h.run(makeNoise(kLoop / 2, 32u, 0.25f));
  EXPECT_EQ(h.looper.snapshot().state, State::overdubbing);
  EXPECT_EQ(h.looper.snapshot().layers, 2) << "the layer being recorded counts";
  h.act(Action::undo);
  h.run(zeros(kBlock));
  EXPECT_EQ(h.looper.snapshot().state, State::playing);
  EXPECT_EQ(h.looper.snapshot().layers, 1);
  EXPECT_EQ(h.looper.snapshot().redoLayers, 0);
}

TEST(LooperTest, ANewOverdubEndsTheRedo) {
  Harness h;
  h.recordAndPlay(makeNoise(kLoop, 33u, 0.25f));
  h.act(Action::overdub);
  h.run(makeNoise(kLoop, 34u, 0.25f));
  h.act(Action::overdub);  // ends the layer
  h.run(zeros(kBlock));
  h.act(Action::undo);
  h.run(zeros(kBlock));
  EXPECT_EQ(h.looper.snapshot().redoLayers, 1);
  h.act(Action::overdub);
  h.run(makeNoise(kLoop / 4, 35u, 0.25f));
  h.act(Action::play);
  h.run(zeros(kBlock));
  EXPECT_EQ(h.looper.snapshot().redoLayers, 0);
  EXPECT_EQ(h.looper.snapshot().layers, 2);
}

TEST(LooperTest, FourOverdubsAreTheLimit) {
  Harness h;
  h.recordAndPlay(makeNoise(kLoop, 41u, 0.1f));
  for (int layer = 1; layer < Looper::kMaxLayers; ++layer) {
    h.act(Action::overdub);
    h.run(makeNoise(kLoop / 2, 41u + static_cast<unsigned>(layer), 0.1f));
    h.act(Action::play);
    h.run(zeros(kBlock));
    EXPECT_EQ(h.looper.snapshot().layers, layer + 1);
  }
  const juce::var full = h.looper.describe();
  EXPECT_TRUE(static_cast<bool>(full["layersFull"]));
  h.act(Action::overdub);
  h.run(zeros(kBlock));
  EXPECT_EQ(h.looper.snapshot().state, State::playing) << "a fifth layer is refused";
  EXPECT_EQ(h.looper.snapshot().layers, Looper::kMaxLayers);
}

// ---------------------------------------------------------------------------
// Playback options
// ---------------------------------------------------------------------------

TEST(LooperTest, HalfSpeedDoublesTheLength) {
  Harness h;
  std::vector<float> in = zeros(kLoop);
  in[1000] = 1.0f;
  h.act(Action::record);
  h.run(in);
  h.looper.setHalfSpeed(true);
  h.act(Action::play);
  const Stereo out = h.run(zeros(4 * kLoop));

  // The click at loop frame 1000 is heard at 2000 and then every 2 x 12800.
  const auto peakIn = [&](size_t from, size_t to) {
    size_t best = from;
    for (size_t i = from; i < to; ++i)
      if (std::abs(out.left[i]) > std::abs(out.left[best]))
        best = i;
    return std::pair<size_t, float>{best, std::abs(out.left[best])};
  };
  const auto first = peakIn(0, 2 * kLoop);
  const auto second = peakIn(2 * kLoop, 4 * kLoop);
  const double period = static_cast<double>(second.first - first.first);
  const double seconds = static_cast<double>(h.looper.snapshot().lengthFrames) / kRate;
  const juce::var state = h.looper.describe();
  printLooper("half speed: the click at loop frame 1000 is heard at " + std::to_string(first.first) +
              " and again at " + std::to_string(second.first) + ", a period of " +
              num(period, 0) + " frames against the loop's " + std::to_string(kLoop) +
              "; looper.get: lengthSeconds " + num(static_cast<double>(state["lengthSeconds"]), 4) +
              ", playbackLengthSeconds " +
              num(static_cast<double>(state["playbackLengthSeconds"]), 4));
  EXPECT_EQ(first.first, 2000u);
  EXPECT_NEAR(first.second, 1.0f, 1.0e-6f);
  EXPECT_NEAR(period, 2.0 * kLoop, 1.0);
  EXPECT_NEAR(static_cast<double>(state["playbackLengthSeconds"]), 2.0 * seconds, 1.0e-9);
  EXPECT_EQ(h.looper.snapshot().lengthFrames, kLoop) << "the loop itself is as recorded";
}

TEST(LooperTest, ReversePlaysBackwards) {
  Harness h;
  const std::vector<float> in = makeNoise(kLoop, 51u, 0.3f);
  h.act(Action::record);
  h.run(in);
  h.looper.setReverse(true);
  h.act(Action::play);
  const Stereo out = h.run(zeros(2 * kLoop));
  int different = 0;
  for (int k = 0; k < kLoop; ++k) {
    const int source = kLoop - 1 - k;
    if (source < kFade || source >= kLoop - kFade)
      continue;
    if (out.left[static_cast<size_t>(k)] != in[static_cast<size_t>(source)])
      ++different;
  }
  EXPECT_EQ(different, 0);
  // A second pass: the same, still backwards.
  for (int k = 0; k < kLoop; ++k)
    EXPECT_EQ(out.left[static_cast<size_t>(k)], out.left[static_cast<size_t>(k + kLoop)]) << k;
  printLooper("reverse: output frame k is loop frame " + std::to_string(kLoop) +
              " - 1 - k, " + std::to_string(different) + " interior samples differ");
}

TEST(LooperTest, OneShotPlaysOnceAndStops) {
  Harness h;
  const std::vector<float> in = makeNoise(kLoop, 61u, 0.3f);
  h.act(Action::record);
  h.run(in);
  h.looper.setOneShot(true);
  h.act(Action::play);
  const Stereo out = h.run(zeros(3 * kLoop));
  EXPECT_EQ(h.looper.snapshot().state, State::stopped);
  EXPECT_EQ(h.looper.snapshot().positionFrames, 0);
  int different = 0;
  for (int k = kFade; k < kLoop - kFade; ++k)
    if (out.left[static_cast<size_t>(k)] != in[static_cast<size_t>(k)])
      ++different;
  EXPECT_EQ(different, 0);
  EXPECT_EQ(peakOf(out.left, static_cast<size_t>(kLoop)), 0.0f) << "silence after the one pass";
  // Play again: once more.
  h.act(Action::play);
  const Stereo again = h.run(zeros(2 * kLoop));
  EXPECT_GT(peakOf(again.left, 0, static_cast<size_t>(kLoop)), 0.1f);
  EXPECT_EQ(peakOf(again.left, static_cast<size_t>(kLoop)), 0.0f);
}

TEST(LooperTest, QuantiseRoundsTheLengthToWholeBeats) {
  // 120 BPM at 48 kHz: a beat is 24000 frames.
  struct Case {
    double bpm;
    int recorded;
    int expected;
  };
  const Case cases[] = {{120.0, 31 * kBlock * 8 / 8 * 8, 24000},      // 31744: 1.32 beats -> 1
                        {120.0, 37 * 1000, 48000},                    // 37000: 1.54 -> 2, runs on
                        {120.0, 11 * 1000, 24000},                    // 0.46 beat -> 1 (at least one)
                        {90.0, 100 * 1000, 96000}};                   // beat 32000: 3.1 -> 3
  for (const Case& c : cases) {
    SCOPED_TRACE(c.recorded);
    Harness h;
    h.bpm = c.bpm;
    h.looper.setQuantise(true);
    h.act(Action::record);
    h.run(makeNoise(c.recorded, 71u, 0.2f));
    h.act(Action::play);
    h.run(zeros(kBlock));
    Looper::Snapshot snapshot = h.looper.snapshot();
    // Running on to the beat: feed silence until the loop closes.
    int guard = 0;
    while (snapshot.state == State::recording && guard++ < 2000) {
      h.run(zeros(kBlock));
      snapshot = h.looper.snapshot();
    }
    printLooper("quantise at " + num(c.bpm, 0) + " BPM: stopped after " +
                std::to_string(c.recorded) + " frames (" +
                num(c.recorded / (kRate * 60.0 / c.bpm), 2) + " beats), loop is " +
                std::to_string(snapshot.lengthFrames) + " frames (" +
                num(static_cast<double>(snapshot.lengthFrames) / (kRate * 60.0 / c.bpm), 2) +
                " beats)");
    EXPECT_EQ(snapshot.state, State::playing);
    EXPECT_EQ(snapshot.lengthFrames, c.expected);
  }
  // Off, the length is exactly what was recorded.
  Harness h;
  h.act(Action::record);
  h.run(makeNoise(31 * kBlock, 72u, 0.2f));
  h.act(Action::play);
  h.run(zeros(kBlock));
  EXPECT_EQ(h.looper.snapshot().lengthFrames, 31 * kBlock);
}

TEST(LooperTest, LevelScalesThePlaybackAlone) {
  Harness h;
  const std::vector<float> in = makeNoise(kLoop, 81u, 0.4f);
  h.recordAndPlay(in);
  const auto playbackRms = [&](float db) {
    h.looper.setLevelDb(db);
    h.run(zeros(5 * kLoop));  // let the 20 ms ramp finish
    const Stereo out = h.run(zeros(kLoop));
    return rmsOf(out.left);
  };
  const double unity = playbackRms(0.0f);
  const double minusSix = playbackRms(-6.0f);
  const double plusSix = playbackRms(6.0f);
  const double off = playbackRms(-60.0f);
  printLooper("level: 0 dB " + num(toDb(unity), 2) + " dBFS, -6 dB " + num(toDb(minusSix), 2) +
              ", +6 dB " + num(toDb(plusSix), 2) + ", -60 dB (off) " + num(off, 9) + " rms");
  EXPECT_NEAR(toDb(minusSix) - toDb(unity), -6.0, 0.05);
  EXPECT_NEAR(toDb(plusSix) - toDb(unity), 6.0, 0.05);
  EXPECT_EQ(off, 0.0);
}

TEST(LooperTest, MonoAndStereoLoopsFollowTheBuffer) {
  const std::vector<float> left = makeNoise(kLoop, 91u, 0.3f);
  const std::vector<float> right = makeNoise(kLoop, 92u, 0.3f);
  {
    // A stereo loop: each side its own.
    Harness h;
    h.act(Action::record);
    h.run(left, &right);
    h.act(Action::play);
    const Stereo out = h.run(zeros(kLoop), nullptr, 2);
    (void)out;
    h.act(Action::stop);
    h.run(zeros(kBlock));
    h.act(Action::play);
    const Stereo played = h.run(zeros(kLoop), nullptr, 2);
    EXPECT_EQ(h.looper.snapshot().channels, 2);
    int wrong = 0;
    for (int k = kFade; k < kLoop - kFade; ++k)
      if (played.left[static_cast<size_t>(k)] != left[static_cast<size_t>(k)] ||
          played.right[static_cast<size_t>(k)] != right[static_cast<size_t>(k)])
        ++wrong;
    EXPECT_EQ(wrong, 0);
  }
  {
    // A mono loop: one channel recorded, heard on both.
    Harness h;
    h.act(Action::record);
    h.runMono(left);
    h.act(Action::play);
    h.runMono(zeros(kBlock));
    h.act(Action::stop);
    h.run(zeros(kBlock), nullptr, 1);
    h.act(Action::play);
    const Stereo played = h.run(zeros(kLoop), nullptr, 2);
    EXPECT_EQ(h.looper.snapshot().channels, 1);
    EXPECT_EQ(played.left, played.right);
    int wrong = 0;
    for (int k = kFade; k < kLoop - kFade; ++k)
      if (played.left[static_cast<size_t>(k)] != left[static_cast<size_t>(k)])
        ++wrong;
    EXPECT_EQ(wrong, 0);
  }
}

TEST(LooperTest, StopKeepsTheLoopAndPlayStartsFromTheTop) {
  Harness h;
  const std::vector<float> in = makeNoise(kLoop, 101u, 0.3f);
  h.recordAndPlay(in);
  h.act(Action::stop);
  const Stereo silent = h.run(zeros(kLoop));
  EXPECT_EQ(h.looper.snapshot().state, State::stopped);
  EXPECT_EQ(peakOf(silent.left), 0.0f);
  EXPECT_EQ(h.looper.snapshot().lengthFrames, kLoop);
  h.act(Action::play);
  const Stereo again = h.run(zeros(kLoop));
  int wrong = 0;
  for (int k = kFade; k < kLoop - kFade; ++k)
    if (again.left[static_cast<size_t>(k)] != in[static_cast<size_t>(k)])
      ++wrong;
  EXPECT_EQ(wrong, 0) << "play after stop begins at the top of the loop";
  h.act(Action::clear);
  const Stereo cleared = h.run(zeros(kLoop));
  EXPECT_EQ(h.looper.snapshot().state, State::empty);
  EXPECT_EQ(h.looper.snapshot().lengthFrames, 0);
  EXPECT_EQ(peakOf(cleared.left), 0.0f);
}

TEST(LooperTest, ASecondRecordWhilePlayingReplacesTheLoop) {
  Harness h;
  h.recordAndPlay(makeNoise(kLoop, 111u, 0.3f));
  const std::vector<float> second = makeNoise(kLoop / 2, 112u, 0.3f);
  h.act(Action::record);
  h.run(second);
  h.act(Action::play);
  h.run(zeros(kBlock));
  EXPECT_EQ(h.looper.snapshot().lengthFrames, kLoop / 2);
  EXPECT_EQ(h.looper.snapshot().layers, 1);
}

TEST(LooperTest, TheCycleActionWalksRecordOverdubPlay) {
  Harness h;
  const State expected[] = {State::recording, State::overdubbing, State::playing, State::overdubbing,
                            State::playing};
  for (const State want : expected) {
    h.act(Action::cycle);
    h.run(makeNoise(kLoop, 121u, 0.1f));
    EXPECT_EQ(h.looper.snapshot().state, want) << Looper::stateName(want);
  }
}

TEST(LooperTest, ARecordingStopsAtTheLimit) {
  Harness h;
  h.act(Action::record);
  // 61 seconds of silence in 4800-frame steps; the loop closes itself at 60.
  const std::vector<float> chunk = zeros(4800);
  for (int i = 0; i < 61 * 10; ++i) {
    std::vector<float> data = chunk;
    float* pointers[2] = {data.data(), data.data()};
    h.looper.process(pointers, 2, 4800, 120.0);
  }
  const Looper::Snapshot snapshot = h.looper.snapshot();
  EXPECT_EQ(snapshot.state, State::playing);
  EXPECT_EQ(snapshot.lengthFrames, snapshot.capacityFrames);
  EXPECT_NEAR(static_cast<double>(snapshot.lengthFrames) / kRate, 60.0, 1.0e-6);
  printLooper("limit: a recording with no stop closes itself at " +
              num(static_cast<double>(snapshot.lengthFrames) / kRate, 3) + " s (" +
              std::to_string(snapshot.lengthFrames) + " frames)");
}

// ---------------------------------------------------------------------------
// Real time
// ---------------------------------------------------------------------------

TEST(LooperTest, ProcessingNeverAllocates) {
  Harness h;
  std::vector<float> left = makeNoise(kBlock, 131u, 0.2f);
  std::vector<float> right = makeNoise(kBlock, 132u, 0.2f);
  h.looper.setQuantise(true);
  ASSERT_TRUE(h.looper.waitForMemory(5000));  // reserved off the audio thread
  AllocationGuard guard;
  const auto block = [&](int count) {
    for (int i = 0; i < count; ++i) {
      float* pointers[2] = {left.data(), right.data()};
      h.looper.process(pointers, 2, kBlock, 120.0);
    }
  };
  // Every state and every action, a few times over, options on and off.
  for (int round = 0; round < 3; ++round) {
    h.looper.request(Action::record);
    block(300);
    h.looper.request(Action::overdub);
    block(150);
    h.looper.setHalfSpeed(round == 1);
    h.looper.setReverse(round == 2);
    h.looper.request(Action::play);
    block(100);
    h.looper.request(Action::undo);
    block(5);
    h.looper.request(Action::redo);
    block(5);
    h.looper.requestFromAudioThread(Action::overdub);
    block(200);
    h.looper.requestFromAudioThread(Action::cycle);
    block(50);
    h.looper.setOneShot(round == 1);
    h.looper.requestFromAudioThread(Action::playStop);
    block(20);
    h.looper.request(Action::stop);
    block(5);
    h.looper.setLevelDb(-12.0f);
    h.looper.request(Action::play);
    block(300);
    h.looper.request(Action::clear);
    block(5);
  }
  const long allocations = guard.count();
  printLooper("process, recording, overdubbing, playing, undo, redo, stop, clear, all options: " +
              std::to_string(allocations) + " allocations through operator new");
  EXPECT_EQ(allocations, 0);
}

TEST(LooperTest, ActionsFromAnotherThreadAreAppliedInOrderWithoutLocks) {
  Harness h;
  std::atomic<bool> done{false};
  std::atomic<long> posted{0};
  std::thread producer([&] {
    const Action sequence[] = {Action::record, Action::overdub, Action::play, Action::undo,
                               Action::redo,   Action::stop,    Action::clear, Action::cycle};
    for (int i = 0; i < 20000; ++i) {
      while (!h.looper.request(sequence[i % 8]))
        std::this_thread::yield();
      posted.fetch_add(1);
    }
    done.store(true);
  });
  std::vector<float> left = makeNoise(kBlock, 141u, 0.2f), right = left;
  while (!done.load()) {
    float* pointers[2] = {left.data(), right.data()};
    h.looper.process(pointers, 2, kBlock, 120.0);
  }
  producer.join();
  for (int i = 0; i < 10; ++i) {
    float* pointers[2] = {left.data(), right.data()};
    h.looper.process(pointers, 2, kBlock, 120.0);
  }
  const Looper::Snapshot snapshot = h.looper.snapshot();
  EXPECT_EQ(snapshot.queued, 0);
  EXPECT_LE(snapshot.layers, Looper::kMaxLayers);
  EXPECT_LE(snapshot.lengthFrames, snapshot.capacityFrames);
  printLooper("20000 actions from a second thread against the audio thread: " +
              std::to_string(posted.load()) + " accepted, nothing left queued, state " +
              Looper::stateName(snapshot.state));
}

// Posting in order: the last of two conflicting actions wins.
TEST(LooperTest, QueuedActionsKeepTheirOrder) {
  Harness h;
  h.act(Action::record);
  h.act(Action::clear);
  h.act(Action::record);
  h.run(makeNoise(kBlock, 151u, 0.2f));
  EXPECT_EQ(h.looper.snapshot().state, State::recording);
  EXPECT_EQ(h.looper.snapshot().recordedFrames, kBlock);
  h.act(Action::clear);
  h.act(Action::play);  // nothing to play
  h.run(zeros(kBlock));
  EXPECT_EQ(h.looper.snapshot().state, State::empty);
}

// ---------------------------------------------------------------------------
// The memory: reserved on the first record, off the audio thread
// ---------------------------------------------------------------------------

namespace {

bool waitUntil(const std::function<bool()>& condition, int timeoutMs) {
  const auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(timeoutMs);
  while (!condition()) {
    if (juce::Time::getMillisecondCounter() > deadline)
      return false;
    juce::Thread::sleep(2);
  }
  return true;
}

}  // namespace

TEST(LooperMemoryTest, PrepareReservesNothing) {
  Harness h;
  const Looper::Snapshot snapshot = h.looper.snapshot();
  EXPECT_TRUE(snapshot.ready);
  EXPECT_FALSE(snapshot.memoryReady);
  EXPECT_EQ(snapshot.memoryBytes, 0u);
  EXPECT_FALSE(snapshot.arming);
  EXPECT_EQ(snapshot.state, State::empty);
  EXPECT_GT(snapshot.capacityFrames, 0) << "the nominal capacity is known without the memory";
  // Preparing again at the same rate still reserves nothing.
  h.looper.prepare(kRate);
  EXPECT_FALSE(h.looper.isMemoryReady());
  printLooper("prepare: ready " + std::to_string(snapshot.ready) + ", memory reserved 0 bytes");
}

TEST(LooperMemoryTest, TheFirstRecordArmsUntilTheMemoryIsThenRecordsFromTheNextBlock) {
  Harness h;
  const std::vector<float> in = makeNoise(10 * kBlock, 301u, 0.3f);
  // The audio-thread door (MIDI) only flags the need; nothing has asked the
  // worker yet when the first block runs, so that block must arm.
  h.looper.requestFromAudioThread(Action::record);
  const Stereo first = h.run(std::vector<float>(in.begin(), in.begin() + kBlock));
  Looper::Snapshot snapshot = h.looper.snapshot();
  EXPECT_TRUE(snapshot.arming);
  EXPECT_EQ(snapshot.state, State::empty);
  EXPECT_EQ(snapshot.recordedFrames, 0);
  EXPECT_EQ(first.left, std::vector<float>(in.begin(), in.begin() + kBlock))
      << "the sound passes untouched while the looper arms";

  ASSERT_TRUE(waitUntil([&] { return h.looper.isMemoryReady(); }, 5000));
  EXPECT_TRUE(h.looper.snapshot().arming) << "nothing changes until the audio thread's next block";
  const std::vector<float> rest(in.begin() + kBlock, in.end());
  h.run(rest);
  snapshot = h.looper.snapshot();
  EXPECT_FALSE(snapshot.arming);
  EXPECT_EQ(snapshot.state, State::recording);
  EXPECT_EQ(snapshot.recordedFrames, static_cast<std::int64_t>(rest.size()))
      << "recording starts in the first block after the handover";

  // And what was recorded is the audio from that block on, to the sample.
  h.act(Action::play);
  const Stereo played = h.run(zeros(2 * static_cast<int>(rest.size())));
  int different = 0;
  for (int k = kFade; k < static_cast<int>(rest.size()) - kFade; ++k)
    if (played.left[static_cast<size_t>(k)] != rest[static_cast<size_t>(k)])
      ++different;
  EXPECT_EQ(different, 0);
  const juce::var described = h.looper.describe();
  EXPECT_TRUE(static_cast<bool>(described["memoryReady"]));
  printLooper("arming: first block passed through untouched, recording began in the block after the "
              "memory arrived, " + std::to_string(rest.size()) + " frames recorded");
}

TEST(LooperMemoryTest, TheStateReadsArmingAndASecondRecordChangesNothing) {
  Harness h;
  h.looper.requestFromAudioThread(Action::record);
  h.looper.requestFromAudioThread(Action::record);
  h.run(zeros(kBlock));
  EXPECT_TRUE(h.looper.snapshot().arming);
  EXPECT_EQ(h.looper.describe().getProperty("state", {}).toString(), "arming");
  ASSERT_TRUE(waitUntil([&] { return h.looper.isMemoryReady(); }, 5000));
  h.run(zeros(kBlock));
  EXPECT_EQ(h.looper.snapshot().state, State::recording)
      << "two record actions while arming are one recording, not a record and a stop";
}

TEST(LooperMemoryTest, StopAndClearTakeAnArmingRecordBack) {
  for (const Action cancel : {Action::stop, Action::clear}) {
    Harness h;
    h.looper.requestFromAudioThread(Action::record);
    h.run(zeros(kBlock));
    ASSERT_TRUE(h.looper.snapshot().arming);
    h.looper.requestFromAudioThread(cancel);
    h.run(zeros(kBlock));
    EXPECT_FALSE(h.looper.snapshot().arming);
    ASSERT_TRUE(waitUntil([&] { return h.looper.isMemoryReady(); }, 5000));
    h.run(zeros(4 * kBlock));
    EXPECT_EQ(h.looper.snapshot().state, State::empty)
        << "the memory came, but the record had been taken back";
  }
}

TEST(LooperMemoryTest, ArmingNeverAllocatesOnTheAudioThread) {
  Harness h;
  std::vector<float> left = makeNoise(kBlock, 311u, 0.2f), right = left;
  long allocations = 0;
  const auto block = [&] {
    AllocationGuard guard;
    float* pointers[2] = {left.data(), right.data()};
    h.looper.process(pointers, 2, kBlock, 120.0);
    allocations += guard.count();
  };
  h.looper.requestFromAudioThread(Action::record);
  block();
  EXPECT_TRUE(h.looper.snapshot().arming);
  for (int i = 0; i < 20; ++i)
    block();  // still arming, or already recording once the worker is done
  ASSERT_TRUE(waitUntil([&] { return h.looper.isMemoryReady(); }, 5000));
  for (int i = 0; i < 20; ++i)
    block();
  EXPECT_EQ(h.looper.snapshot().state, State::recording);
  EXPECT_EQ(allocations, 0) << "reserving the memory is the worker's, never the audio thread's";
}

TEST(LooperMemoryTest, TheMemoryIsFreedAfterTheGracePeriodOnceTheLoopIsCleared) {
  Harness h;
  h.looper.setMemoryGraceMs(150);
  h.recordAndPlay(makeNoise(kLoop, 321u, 0.3f));
  ASSERT_TRUE(h.looper.isMemoryReady());

  // Not while there is a loop, however long the grace period has run.
  juce::Thread::sleep(400);
  h.run(zeros(kBlock));
  EXPECT_TRUE(h.looper.isMemoryReady()) << "a playing loop keeps its memory";
  EXPECT_EQ(h.looper.snapshot().state, State::playing);

  h.act(Action::clear);
  h.run(zeros(kBlock));
  EXPECT_EQ(h.looper.snapshot().state, State::empty);
  const auto cleared = juce::Time::getMillisecondCounter();
  EXPECT_TRUE(h.looper.isMemoryReady()) << "the memory outlives the clear by the grace period";
  ASSERT_TRUE(waitUntil([&] { return !h.looper.isMemoryReady(); }, 5000));
  const auto freedAfter = juce::Time::getMillisecondCounter() - cleared;
  EXPECT_GE(freedAfter, 100u);
  EXPECT_EQ(h.looper.snapshot().memoryBytes, 0u);
  printLooper("clear: memory freed " + std::to_string(freedAfter) + " ms later (grace 150 ms)");

  // Blocks go on passing the sound through, and a new record arms again.
  const std::vector<float> in = makeNoise(4 * kBlock, 322u, 0.2f);
  EXPECT_EQ(h.run(in).left, in);
  h.act(Action::record);  // waits for the memory
  EXPECT_TRUE(h.looper.isMemoryReady());
  h.run(makeNoise(kLoop, 323u, 0.3f));
  EXPECT_EQ(h.looper.snapshot().state, State::recording);
  EXPECT_EQ(h.looper.snapshot().recordedFrames, kLoop);
}

TEST(LooperMemoryTest, TheMemoryIsNotTakenBackWhileARecordNeedsIt) {
  // The memory is not taken back while an action is waiting in the ring.
  Harness h;
  h.looper.setMemoryGraceMs(0);
  ASSERT_TRUE(h.looper.waitForMemory(5000));
  juce::Thread::sleep(100);
  // With a zero grace period the worker frees an idle looper at its next
  // pass; here the looper is idle, so the memory goes.
  EXPECT_TRUE(waitUntil([&] { return !h.looper.isMemoryReady(); }, 5000));
  // A record posted with the worker polling goes the other way round: the
  // request is ahead of any release.
  h.act(Action::record);
  h.run(makeNoise(2 * kBlock, 331u, 0.2f));
  EXPECT_EQ(h.looper.snapshot().state, State::recording);
  juce::Thread::sleep(100);
  EXPECT_TRUE(h.looper.isMemoryReady()) << "a recording keeps its memory";
}

TEST(LooperMemoryTest, ANewSampleRateDropsTheMemoryAndTheLoop) {
  Harness h;
  h.recordAndPlay(makeNoise(kLoop, 341u, 0.3f));
  ASSERT_TRUE(h.looper.isMemoryReady());
  h.looper.prepare(44100.0);
  EXPECT_FALSE(h.looper.isMemoryReady());
  EXPECT_EQ(h.looper.snapshot().state, State::empty);
  EXPECT_EQ(h.looper.snapshot().capacityFrames, static_cast<std::int64_t>(60.0 * 44100.0));
  ASSERT_TRUE(h.looper.waitForMemory(5000));
  EXPECT_NEAR(static_cast<double>(h.looper.snapshot().memoryBytes), 5.0 * 2.0 * 60.0 * 44100.0 * 4.0, 1.0);
}

// ---------------------------------------------------------------------------
// In the processor: commands, the master, MIDI
// ---------------------------------------------------------------------------

namespace {

constexpr int kRigBlock = 256;

juce::var runCommand(EffectRig& rig, const char* name, const juce::String& args = "{}") {
  return rig.must(name, args);
}

std::string stateOf(EffectRig& rig) { return runCommand(rig, "looper.get")["state"].toString().toStdString(); }

// One block of silence (or `in`) through the processor itself, with `midi`.
std::vector<float> processBlock(EffectRig& rig, const std::vector<float>& in, juce::MidiBuffer& midi) {
  juce::AudioBuffer<float> buffer(2, static_cast<int>(in.size()));
  buffer.copyFrom(0, 0, in.data(), static_cast<int>(in.size()));
  buffer.copyFrom(1, 0, in.data(), static_cast<int>(in.size()));
  rig.proc.processBlock(buffer, midi);
  return std::vector<float>(buffer.getReadPointer(0), buffer.getReadPointer(0) + in.size());
}

void sendCc(EffectRig& rig, int number, int value) {
  juce::MidiBuffer midi;
  midi.addEvent(juce::MidiMessage::controllerEvent(1, number, value), 0);
  processBlock(rig, zeros(kRigBlock), midi);
}

void pump() { juce::MessageManager::getInstance()->runDispatchLoopUntil(20); }

// The first record action reserves the loop's memory on the looper's worker
// thread; the tests that go on to record wait for it.
void settle(EffectRig& rig) { ASSERT_TRUE(rig.proc.getLooper().waitForMemory(5000)); }

}  // namespace

TEST(LooperCommandsTest, EveryCommandIsRegisteredAndDocumented) {
  EffectRig rig{juce::String()};
  const CommandRegistry& registry = rig.commands.registry();
  for (const char* name : {"looper.get", "looper.record", "looper.overdub", "looper.play",
                           "looper.stop", "looper.undo", "looper.redo", "looper.clear",
                           "looper.setOptions"}) {
    const Command* command = registry.find(name);
    ASSERT_NE(command, nullptr) << name;
    EXPECT_GT(command->summary.length(), 40) << name << " is documented";
    EXPECT_NE(command->permission, CommandPermission::unclassified) << name;
    EXPECT_EQ(command->mutates, juce::String(name) != "looper.get") << name;
  }
  const juce::String markdown = rig.commands.markdownReference();
  EXPECT_TRUE(markdown.contains("looper.setOptions"));
}

TEST(LooperCommandsTest, GetReportsAnEmptyLooper) {
  EffectRig rig{juce::String()};
  const juce::var state = runCommand(rig, "looper.get");
  EXPECT_EQ(state["state"].toString(), "empty");
  EXPECT_TRUE(static_cast<bool>(state["ready"]));
  EXPECT_EQ(static_cast<int>(state["layers"]), 0);
  EXPECT_EQ(static_cast<int>(state["lengthFrames"]), 0);
  EXPECT_DOUBLE_EQ(static_cast<double>(state["maxSeconds"]), 60.0);
  EXPECT_NEAR(static_cast<double>(state["capacitySeconds"]), 60.0, 1.0e-3);
  EXPECT_FALSE(static_cast<bool>(state["canUndo"]));
  EXPECT_FALSE(static_cast<bool>(state["canRedo"]));
  const juce::var options = state["options"];
  EXPECT_FALSE(static_cast<bool>(options["halfSpeed"]));
  EXPECT_FALSE(static_cast<bool>(options["reverse"]));
  EXPECT_FALSE(static_cast<bool>(options["oneShot"]));
  EXPECT_FALSE(static_cast<bool>(options["quantise"]));
  EXPECT_DOUBLE_EQ(static_cast<double>(options["levelDb"]), 0.0);
}

TEST(LooperCommandsTest, RecordAndPlayRoundTripThroughTheProcessor) {
  EffectRig rig{juce::String()};
  const std::vector<float> in = makeNoise(40 * kRigBlock, 161u, 0.25f);

  // The command posts; the audio thread picks it up in its next block.
  EXPECT_FALSE(static_cast<bool>(runCommand(rig, "looper.get")["memoryReady"]))
      << "nothing is reserved before the first record";
  const juce::var posted = runCommand(rig, "looper.record");
  EXPECT_EQ(static_cast<int>(posted["queued"]), 1);
  EXPECT_EQ(posted["state"].toString(), "empty");
  settle(rig);  // the audio thread would otherwise report "arming" for a block or two
  EXPECT_TRUE(static_cast<bool>(runCommand(rig, "looper.get")["memoryReady"]));
  EXPECT_NEAR(static_cast<double>(runCommand(rig, "looper.get")["memoryBytes"]),
              5.0 * 2.0 * 60.0 * 48000.0 * 4.0, 1.0e6);
  const std::vector<float> recorded = rig.render(in);
  EXPECT_EQ(stateOf(rig), "recording");
  EXPECT_GT(static_cast<double>(runCommand(rig, "looper.get")["recordedSeconds"]), 0.2);

  runCommand(rig, "looper.play");
  const std::vector<float> played = rig.render(zeros(3 * 40 * kRigBlock));
  const juce::var state = runCommand(rig, "looper.get");
  EXPECT_EQ(state["state"].toString(), "playing");
  EXPECT_EQ(static_cast<int>(state["lengthFrames"]), 40 * kRigBlock);
  EXPECT_EQ(static_cast<int>(state["layers"]), 1);

  // The loop is the rig's own output, recorded after the chain: the
  // recording's frames again, apart from the end fades and the tail of the
  // rig's DC blocker, which is still ringing in the silent passes.
  double worst = 0.0;
  for (int k = kFade; k < 40 * kRigBlock - kFade; ++k)
    worst = std::max(worst, std::abs(static_cast<double>(played[static_cast<size_t>(k)]) -
                                     recorded[static_cast<size_t>(k)]));
  printLooper("processor: the first pass of the loop against the recording, largest difference " +
              num(worst, 6) + " (peak " + num(peakOf(recorded), 3) + ")");
  EXPECT_LT(worst, 1.0e-2);
  double worstPass = 0.0;
  for (int k = 0; k < 40 * kRigBlock; ++k)
    worstPass = std::max(worstPass, std::abs(static_cast<double>(played[static_cast<size_t>(k)]) -
                                             played[static_cast<size_t>(k + 40 * kRigBlock)]));
  EXPECT_LT(worstPass, 1.0e-2);

  // The rest of the actions answer with the state.
  for (const char* name : {"looper.overdub", "looper.undo", "looper.redo", "looper.stop",
                           "looper.clear"}) {
    const juce::var answer = runCommand(rig, name);
    EXPECT_TRUE(answer.hasProperty("state")) << name;
    rig.render(zeros(kRigBlock));
  }
  EXPECT_EQ(stateOf(rig), "empty");
}

TEST(LooperCommandsTest, SetOptionsRoundTripsAndValidates) {
  EffectRig rig{juce::String()};
  const juce::var set = runCommand(
      rig, "looper.setOptions",
      R"({"halfSpeed":true,"reverse":true,"oneShot":true,"quantise":true,"levelDb":-6.5})");
  const juce::var options = set["options"];
  EXPECT_TRUE(static_cast<bool>(options["halfSpeed"]));
  EXPECT_TRUE(static_cast<bool>(options["reverse"]));
  EXPECT_TRUE(static_cast<bool>(options["oneShot"]));
  EXPECT_TRUE(static_cast<bool>(options["quantise"]));
  EXPECT_NEAR(static_cast<double>(options["levelDb"]), -6.5, 1.0e-6);

  // One at a time leaves the others alone.
  const juce::var one = runCommand(rig, "looper.setOptions", R"({"reverse":false})");
  EXPECT_FALSE(static_cast<bool>(one["options"]["reverse"]));
  EXPECT_TRUE(static_cast<bool>(one["options"]["halfSpeed"]));
  EXPECT_NEAR(static_cast<double>(one["options"]["levelDb"]), -6.5, 1.0e-6);
  EXPECT_TRUE(static_cast<bool>(runCommand(rig, "looper.get")["options"]["oneShot"]));

  const auto fails = [&](const juce::String& args, const char* code) {
    const CommandResult result = rig.commands.registry().run("looper.setOptions", juce::JSON::parse(args));
    EXPECT_FALSE(result.ok) << args.toStdString();
    EXPECT_EQ(result.errorCode, juce::String(code)) << args.toStdString();
  };
  fails("{}", CommandError::invalidArgs);
  fails(R"({"levelDb":99})", CommandError::invalidArgs);
  fails(R"({"levelDb":-61})", CommandError::invalidArgs);
  fails(R"({"halfSpeed":"yes"})", CommandError::invalidArgs);
  fails(R"({"speed":2})", CommandError::invalidArgs);
}

TEST(LooperCommandsTest, HalfSpeedAndReverseThroughTheCommandsChangeTheSound) {
  EffectRig rig{juce::String()};
  const std::vector<float> in = makeNoise(20 * kRigBlock, 171u, 0.25f);
  runCommand(rig, "looper.record");
  settle(rig);
  const std::vector<float> recorded = rig.render(in);
  runCommand(rig, "looper.setOptions", R"({"reverse":true})");
  runCommand(rig, "looper.play");
  const std::vector<float> played = rig.render(zeros(20 * kRigBlock));
  double worst = 0.0;
  for (int k = kFade; k < 20 * kRigBlock - kFade; ++k) {
    const int source = 20 * kRigBlock - 1 - k;
    if (source < kFade || source >= 20 * kRigBlock - kFade)
      continue;
    worst = std::max(worst, std::abs(static_cast<double>(played[static_cast<size_t>(k)]) -
                                     recorded[static_cast<size_t>(source)]));
  }
  EXPECT_LT(worst, 1.0e-2);
  const juce::var state = runCommand(rig, "looper.setOptions", R"({"halfSpeed":true})");
  EXPECT_NEAR(static_cast<double>(state["playbackLengthSeconds"]),
              2.0 * static_cast<double>(state["lengthSeconds"]), 1.0e-9);
}

TEST(LooperCommandsTest, TheLoopSitsBeforeTheMasterLevel) {
  // The master (outputLevel) scales the loop exactly as it scales live sound.
  const auto liveRmsAt = [](float level) {
    EffectRig rig{juce::String()};
    rig.proc.parameters.getParameter("outputLevel")->setValueNotifyingHost(level);
    rig.render(zeros(6 * kRigBlock));  // settle the smoothing
    return rmsOf(rig.render(makeNoise(40 * kRigBlock, 181u, 0.25f)), 10 * kRigBlock);
  };
  const auto loopRmsAt = [](float level) {
    EffectRig rig{juce::String()};
    runCommand(rig, "looper.record");
    settle(rig);
    rig.render(makeNoise(40 * kRigBlock, 181u, 0.25f));
    runCommand(rig, "looper.play");
    rig.render(zeros(kRigBlock));
    rig.proc.parameters.getParameter("outputLevel")->setValueNotifyingHost(level);
    rig.render(zeros(6 * kRigBlock));
    return rmsOf(rig.render(zeros(40 * kRigBlock)), 10 * kRigBlock);
  };
  const double liveRatio = toDb(liveRmsAt(0.25f)) - toDb(liveRmsAt(0.5f));
  const double loopRatio = toDb(loopRmsAt(0.25f)) - toDb(loopRmsAt(0.5f));
  printLooper("output level 0.5 to 0.25 (-12 dB): live sound " + num(liveRatio, 2) +
              " dB, the loop " + num(loopRatio, 2) + " dB");
  EXPECT_NEAR(liveRatio, -12.0, 0.3);
  EXPECT_NEAR(loopRatio, liveRatio, 0.3);
}

TEST(LooperMidiTest, TheMapListsCc48To61AndTheyDriveTheLooperInTheBlockTheyArriveIn) {
  EffectRig rig{juce::String()};
  const juce::var defaults = runCommand(rig, "midi.getMap")["defaults"];
  ASSERT_EQ(defaults.size(), 18);
  const char* names[] = {"looperRecord",   "looperOverdub",  "looperPlay",      "looperStop",
                         "looperUndo",     "looperRedo",     "looperClear",     "looperHalfSpeed",
                         "looperReverse",  "looperOneShot",  "looperQuantise",  "looperLevel",
                         "looperCycle",    "looperPlayStop"};
  for (int i = 0; i < 14; ++i) {
    EXPECT_EQ(defaults[4 + i]["action"].toString(), juce::String(names[i]));
    EXPECT_EQ(static_cast<int>(defaults[4 + i]["cc"]), 48 + i);
    EXPECT_FALSE(static_cast<bool>(defaults[4 + i]["overridden"]));
  }

  const Looper& looper = rig.proc.getLooper();
  // A press: 127 then 0 (a momentary switch) counts once.
  sendCc(rig, 48, 127);
  EXPECT_TRUE(looper.snapshot().arming)
      << "the action landed in the block the CC arrived in, and waits for the memory";
  EXPECT_EQ(looper.snapshot().state, State::empty);
  settle(rig);
  sendCc(rig, 48, 0);
  EXPECT_FALSE(looper.snapshot().arming);
  EXPECT_EQ(looper.snapshot().state, State::recording) << "recording starts in the first block after the memory";
  for (int i = 0; i < 20; ++i)
    rig.render(makeNoise(kRigBlock, 191u + static_cast<unsigned>(i), 0.2f));
  sendCc(rig, 50, 127);  // play
  sendCc(rig, 50, 0);
  EXPECT_EQ(looper.snapshot().state, State::playing);
  EXPECT_GT(looper.snapshot().lengthFrames, 20 * kRigBlock);
  sendCc(rig, 49, 127);  // overdub
  EXPECT_EQ(looper.snapshot().state, State::overdubbing);
  sendCc(rig, 49, 0);
  sendCc(rig, 49, 127);  // again: back to play
  sendCc(rig, 49, 0);
  EXPECT_EQ(looper.snapshot().state, State::playing);
  EXPECT_EQ(looper.snapshot().layers, 2);
  sendCc(rig, 52, 127);  // undo
  EXPECT_EQ(looper.snapshot().layers, 1);
  sendCc(rig, 52, 0);
  sendCc(rig, 53, 127);  // redo
  EXPECT_EQ(looper.snapshot().layers, 2);
  sendCc(rig, 53, 0);
  sendCc(rig, 51, 127);  // stop
  EXPECT_EQ(looper.snapshot().state, State::stopped);
  sendCc(rig, 51, 0);
  sendCc(rig, 61, 127);  // play / stop toggle
  EXPECT_EQ(looper.snapshot().state, State::playing);
  sendCc(rig, 61, 0);
  sendCc(rig, 61, 127);
  EXPECT_EQ(looper.snapshot().state, State::stopped);
  sendCc(rig, 61, 0);

  // Switches: 64 and above on, below off.
  sendCc(rig, 55, 127);
  sendCc(rig, 56, 100);
  sendCc(rig, 57, 64);
  sendCc(rig, 58, 127);
  Looper::Snapshot s = looper.snapshot();
  EXPECT_TRUE(s.halfSpeed && s.reverse && s.oneShot && s.quantise);
  sendCc(rig, 55, 0);
  sendCc(rig, 56, 63);
  s = looper.snapshot();
  EXPECT_FALSE(s.halfSpeed);
  EXPECT_FALSE(s.reverse);
  EXPECT_TRUE(s.oneShot);

  // Level: 0 to 127 over -60 to +12 dB.
  sendCc(rig, 59, 127);
  EXPECT_NEAR(looper.snapshot().levelDb, 12.0f, 1.0e-4f);
  sendCc(rig, 59, 0);
  EXPECT_NEAR(looper.snapshot().levelDb, -60.0f, 1.0e-4f);
  sendCc(rig, 59, 64);
  EXPECT_NEAR(looper.snapshot().levelDb, -60.0f + 72.0f * 64.0f / 127.0f, 1.0e-3f);

  // Clear, then the single pedal walks the states (with the switches off
  // again: quantise would hold the first recording to the beat and one shot
  // would stop the loop at its end).
  sendCc(rig, 57, 0);
  sendCc(rig, 58, 0);
  sendCc(rig, 54, 127);
  sendCc(rig, 54, 0);
  EXPECT_EQ(looper.snapshot().state, State::empty);
  sendCc(rig, 60, 127);
  sendCc(rig, 60, 0);
  EXPECT_EQ(looper.snapshot().state, State::recording);
  rig.render(makeNoise(4 * kRigBlock, 199u, 0.2f));
  sendCc(rig, 60, 127);
  sendCc(rig, 60, 0);
  EXPECT_EQ(looper.snapshot().state, State::overdubbing);
  sendCc(rig, 60, 127);
  sendCc(rig, 60, 0);
  EXPECT_EQ(looper.snapshot().state, State::playing);

  // Reserved gig view stays silent; the first four fixed controls are as they were.
  sendCc(rig, 46, 127);
  EXPECT_EQ(looper.snapshot().state, State::playing);
  pump();
}

TEST(LooperMidiTest, TheDefaultsCanBeMovedSwitchedOffAndTakenOverByAMapping) {
  EffectRig rig{juce::String()};
  const Looper& looper = rig.proc.getLooper();

  rig.proc.midiMapper.setFixedControlCc(MidiMapper::FixedControl::looperRecord, 90);
  sendCc(rig, 48, 127);
  EXPECT_EQ(looper.snapshot().state, State::empty) << "the old number does nothing";
  sendCc(rig, 90, 127);
  EXPECT_TRUE(looper.snapshot().arming);
  runCommand(rig, "looper.clear");
  rig.render(zeros(kRigBlock));

  rig.proc.midiMapper.setFixedControlCc(MidiMapper::FixedControl::looperRecord, -1);
  sendCc(rig, 90, 127);
  EXPECT_EQ(looper.snapshot().state, State::empty);

  // A mapping of the user's own on CC 50 takes the number from the looper's play.
  rig.must("midi.setCc", R"({"targetId":"gateEnabled","cc":50})");
  EXPECT_TRUE(static_cast<bool>(runCommand(rig, "midi.getMap")["defaults"][6]["overridden"]));
  sendCc(rig, 49, 127);  // overdub on an empty looper: nothing
  rig.proc.midiMapper.setFixedControlCc(MidiMapper::FixedControl::looperRecord, 48);
  settle(rig);
  sendCc(rig, 48, 127);
  EXPECT_EQ(looper.snapshot().state, State::recording);
  rig.render(makeNoise(4 * kRigBlock, 211u, 0.2f));
  sendCc(rig, 50, 127);
  EXPECT_EQ(looper.snapshot().state, State::recording) << "CC 50 now belongs to the mapping";
  rig.must("midi.remove", R"({"targetId":"gateEnabled"})");
  sendCc(rig, 50, 127);
  EXPECT_EQ(looper.snapshot().state, State::playing);

  // The channel filter gates them like everything else.
  rig.must("midi.setChannelFilter", R"({"channel":5})");
  sendCc(rig, 54, 127);  // channel 1: ignored
  EXPECT_EQ(looper.snapshot().state, State::playing);
  rig.must("midi.setChannelFilter", R"({"channel":0})");
}

TEST(LooperMidiTest, ProcessBlockWithTheLooperBusyDoesNotAllocateBeyondTheBaseline) {
  EffectRig rig{juce::String()};
  const auto countAllocations = [&](bool looperBusy) {
    juce::AudioBuffer<float> buffer(2, kRigBlock);
    juce::MidiBuffer midi;
    if (looperBusy) {
      runCommand(rig, "looper.record");
      settle(rig);
      rig.render(zeros(kRigBlock));
    }
    // Whatever the processor sets up lazily happens before the count.
    for (int i = 0; i < 30; ++i) {
      buffer.clear();
      rig.proc.processBlock(buffer, midi);
    }
    long total = 0;
    for (int i = 0; i < 200; ++i) {
      buffer.clear();
      midi.clear();
      if (looperBusy && i == 100)
        midi.addEvent(juce::MidiMessage::controllerEvent(1, 50, 127), 0);
      AllocationGuard guard;
      rig.proc.processBlock(buffer, midi);
      total += guard.count();
    }
    return total;
  };
  const long idle = countAllocations(false);
  const long busy = countAllocations(true);
  printLooper("processBlock, 200 blocks: " + std::to_string(idle) + " allocations with the looper idle, " +
              std::to_string(busy) + " recording and then playing (a CC mid-run)");
  EXPECT_EQ(busy, idle);
}

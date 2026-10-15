#pragma once
// Private to the pitch family (plugin/src/effects/pitch), and borrowed by
// delay.crystal for the shifters in its feedback loop: the plumbing the
// three kinds share around the polyphonic engine in PitchShift.h. There is no
// kind in here, only the engine's lifecycle made safe for a block:
//
//  - a "voice" is one shift ratio. It owns the engines that render it, one
//    per independent signal: a mono or true-stereo buffer needs one (the
//    engine shares its control path between the two channels, which is what
//    keeps a stereo image from smearing), dual mono needs two, because there
//    the channels are two instruments and a splice decided on their mean
//    would be a cross-feed of control;
//  - the engine starts "powered off" and blends the shifted signal against
//    the DRY signal it is handed, for 25 ms after it is switched on. A block
//    is wet only, so a start has to be run through on silence first (a
//    run-in) and only then meets audio; PitchVoice does that lazily on the
//    first block that needs the voice;
//  - the engine runs in its constant-latency mode (PitchShift.h, Params::
//    onsetDelaySamples): an attack, or an impulse, arrives exactly the block's
//    reported latency after it went in, at every interval. That latency
//    depends on the window (the engine's buffer) and on the widest interval the
//    kind offers, and the framework wants it latched
//    (BlockProcessor::commitLatency); WindowLatch keeps that bookkeeping in one
//    place;
//  - every control reaches the engine on a fixed 16-sample grid counted from
//    the start of the stream, never from the start of a host block, so the
//    output does not depend on the block size (docs/effects.md, Realtime
//    rules).
#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>

#include "PitchShift.h"

namespace pitchfx {

constexpr int kControlGrid = 16;
constexpr int kMaxLanes = 2;

// The window choices of the engine (PitchShift::kWindowMs), as the labels of
// the `window` choice parameter. Index == PitchShift::Window.
inline juce::StringArray windowLabels() {
  juce::StringArray labels;
  for (const int ms : PitchShift::kWindowMs)
    labels.add(juce::String(ms) + " ms");
  return labels;
}

/** A pitch glide counted in control-grid steps: a new target is reached by a
    straight ramp in semitones over a fixed time, so a stepped interval
    change bends instead of jumping and the engine's pending lag search sees
    a drift that moves a little at a time (PitchShift::setParams). */
class Glide {
public:
  void prepare(double sampleRate, double seconds) noexcept {
    gridsPerGlide = std::max(1, static_cast<int>(std::lround(seconds * sampleRate / kControlGrid)));
    snap();
  }
  /** The next target is taken at once: a block switched on or reset starts
      at its settings, not gliding in from where it was left. */
  void snap() noexcept {
    snapNext = true;
    stepsLeft = 0;
  }
  void setTarget(float t) noexcept {
    if (snapNext) {
      current = target = t;
      stepsLeft = 0;
      snapNext = false;
    } else if (!juce::exactlyEqual(t, target)) {
      target = t;
      stepsLeft = gridsPerGlide;
      delta = (t - current) / static_cast<float>(gridsPerGlide);
    }
  }
  /** Once per control-grid boundary. */
  float tick() noexcept {
    if (stepsLeft > 0) {
      current += delta;
      if (--stepsLeft == 0)
        current = target;
    }
    return current;
  }
  float value() const noexcept { return current; }

private:
  int gridsPerGlide{1};
  int stepsLeft{0};
  float current{0.0f}, target{0.0f}, delta{0.0f};
  bool snapNext{true};
};

/** Walks a render in segments that never cross a control-grid line, so work
    done at the start of a segment with `boundary` set happens at the same
    stream positions whatever the host block size. */
class ControlGrid {
public:
  void reset() noexcept { pos = 0; }

  template <typename Fn>
  void run(int numSamples, Fn&& fn) {
    int done = 0;
    while (done < numSamples) {
      const int length = std::min(numSamples - done, kControlGrid - pos);
      fn(done, length, pos == 0);
      done += length;
      pos = (pos + length) % kControlGrid;
    }
  }

private:
  int pos{0};
};

/** The window (and with it the latency) a block runs with, latched as
    BlockProcessor asks: the parameter may move at any time, the wet path and
    the reported latency change together at commitLatency(). Atomics, because
    the latency is read from the message thread while the audio thread plays.

    The latency is the engine's constant attack latency for the window: the
    smallest delay at which an attack can land exactly at every interval up to
    `maxAbsSemitones` either way (PitchShift::minOnsetDelaySamples). It does not
    depend on the interval set, so moving the interval never moves it. */
class WindowLatch {
public:
  void prepare(double sampleRate, float windowParam, double maxAbsSemitones) noexcept {
    rate = sampleRate;
    range = maxAbsSemitones;
    commit(windowParam);
  }
  void commit(float windowParam) noexcept {
    const int index = indexOf(windowParam);
    windowIndex.store(index, std::memory_order_relaxed);
    latency.store(PitchShift::minOnsetDelaySamples(PitchShift::windowFromIndex(index), rate, range),
                  std::memory_order_relaxed);
  }
  bool pending(float windowParam) const noexcept {
    return indexOf(windowParam) != windowIndex.load(std::memory_order_relaxed);
  }
  PitchShift::Window current() const noexcept {
    return PitchShift::windowFromIndex(windowIndex.load(std::memory_order_relaxed));
  }
  int latencySamples() const noexcept { return latency.load(std::memory_order_relaxed); }

  /** Seconds the wet signal can still be heard after the input stops, for
      the latched window: the read tap never trails the write head by more
      than the window (plus the margins the engine keeps for a late search),
      and a fade in progress at the end can run the old tap on for up to
      another window; measured against the engine in the tests. */
  double tailSeconds() const noexcept {
    const double windowSeconds =
        PitchShift::windowMs(PitchShift::windowFromIndex(windowIndex.load(std::memory_order_relaxed))) *
        0.001;
    return kTailWindows * windowSeconds + kTailPadSeconds;
  }

private:
  static constexpr double kTailWindows = 2.0;
  static constexpr double kTailPadSeconds = 0.0;
  static int indexOf(float windowParam) noexcept {
    return juce::jlimit(0, static_cast<int>(PitchShift::kWindowMs.size()) - 1,
                        static_cast<int>(std::lround(windowParam)));
  }
  double rate{48000.0};
  double range{24.0};
  std::atomic<int> windowIndex{static_cast<int>(PitchShift::kDefaultWindow)};
  std::atomic<int> latency{0};
};

/** One shift ratio, rendered by one engine per independent signal. */
class PitchVoice {
public:
  /** Not on the audio thread. Allocates the engines and the run-in buffer, and
      runs the engines in, so that the first block that plays costs nothing
      extra (later cold starts, after a bypass, happen on the audio thread). */
  void prepare(double sampleRate, int maxBlockSize, PitchShift::Window window, int onsetDelay) {
    rate = sampleRate;
    onsetDelaySamples = onsetDelay;
    // Fresh engines, not the old ones prepared again: PitchShift counts the
    // samples it has seen without ever resetting the count, and its read
    // position is a double that starts from it, so an engine that has played
    // renders the same input with different rounding than a new one. A block
    // prepared again must sound exactly like a new block (the determinism
    // the conformance test checks), so it gets new engines.
    for (auto& engine : engines) {
      engine = std::make_unique<PitchShift>();
      engine->prepare(sampleRate, juce::jmax(1, maxBlockSize));
    }
    silence.setSize(2, kRunInChunk, false, true, false);
    // Until the tap has audio under it the engine plays the dry signal (the
    // prime), then blends the shifted one in over 25 ms. Run both out.
    runInSamples = PitchShift::minDelaySamples(sampleRate) + 8 +
                   static_cast<int>(std::ceil(sampleRate * kBlendSeconds)) + 16;
    for (int lane = 0; lane < kMaxLanes; ++lane)
      coldStart(lane, window);
  }

  /** The engines hold stale audio: the next block that uses the voice starts
      them from rest first. Cheap, any time. */
  void markCold() noexcept {
    for (auto& flag : cold)
      flag = true;
  }

  /** Renders `length` samples starting at `start` of the channels in place
      through the `window`, at `semitones[lane]` (the caller's glide). In dual
      mono each channel is its own lane with its own engine and its own
      interval; otherwise the channels share lane 0. */
  void process(float* const* channels, int numChannels, bool dualMono, int start, int length,
               const float (&semitones)[kMaxLanes], PitchShift::Window window, int onsetDelay) {
    onsetDelaySamples = onsetDelay;
    const int lanes = (dualMono && numChannels > 1) ? 2 : 1;
    for (int lane = 0; lane < lanes; ++lane) {
      PitchShift& engine = *engines[lane];
      if (cold[lane])
        coldStart(lane, window);
      engine.setParams(PitchShift::Params{semitones[lane], 0.0f, window, onsetDelaySamples});
      float* pointers[kMaxLanes] = {channels[lane] + start,
                                    (lanes == 1 && numChannels > 1) ? channels[1] + start : nullptr};
      juce::AudioBuffer<float> view(pointers, lanes == 1 ? juce::jmin(numChannels, 2) : 1, 0, length);
      engine.process(view);
    }
  }

private:
  static constexpr int kRunInChunk = 256;
  static constexpr double kBlendSeconds = 0.025;  // PitchShift's power blend

  // Power the engine down, let the fade land, power it up (which clears its
  // rings and puts the tap at the floor), then play silence through it until
  // the prime and the blend-in are over, so that what follows is the shifted
  // signal alone. The run-in uses a ratio of exactly 1: the tap then rests at
  // the floor, as in a fresh start, rather than drifting off it, which would
  // delay the first note by the distance it drifted.
  void coldStart(int lane, PitchShift::Window window) {
    PitchShift& engine = *engines[lane];
    const PitchShift::Params unity{0.0f, 0.0f, window, onsetDelaySamples};
    engine.setParams(unity);
    if (engine.isRunning()) {
      engine.setEnabled(false);
      for (int guard = 0; engine.isRunning() && guard < 64; ++guard)
        feedSilence(engine, kRunInChunk);
    }
    engine.setEnabled(true);
    for (int left = runInSamples; left > 0; left -= kRunInChunk)
      feedSilence(engine, std::min(left, kRunInChunk));
    cold[lane] = false;
  }

  void feedSilence(PitchShift& engine, int length) {
    silence.clear();
    juce::AudioBuffer<float> view(silence.getArrayOfWritePointers(), 2, 0, length);
    engine.process(view);
  }

  double rate{48000.0};
  int onsetDelaySamples{0};
  int runInSamples{1};
  std::unique_ptr<PitchShift> engines[kMaxLanes];
  juce::AudioBuffer<float> silence;
  bool cold[kMaxLanes] = {true, true};
};

}  // namespace pitchfx

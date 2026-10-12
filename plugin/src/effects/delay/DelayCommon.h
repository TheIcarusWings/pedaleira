#pragma once
// Pieces the delay family shares: the tail arithmetic of a feedback loop, the
// two ways a delay time can move (a crossfade between two taps, a slewed read
// pointer) and the loop's safety nonlinearities. Header only, all inline, so
// every effect file stays one self-contained translation unit with no new
// shared source. Nothing here registers a kind (the build only looks for
// hooks in .cpp files).
#include <algorithm>
#include <cmath>
#include "effects/EffectDsp.h"

namespace delayfx {

constexpr int kMaxChannels = 2;

// Stands in for "rings forever" (a loop whose gain reaches 1). The chain caps
// whatever a kind reports at kMaxBlockTailSeconds, so a large finite number
// is an honest answer, and unlike infinity it survives arithmetic.
constexpr double kTailForever = 1.0e6;

// Control-rate work (filter coefficients, LFO-driven values) runs on this
// fixed grid of samples, counted across blocks, so the output never depends
// on where the host cut the audio into blocks.
constexpr unsigned kControlGrid = 16;

class ControlTick {
public:
  void reset() noexcept { count = 0; }
  /** True on every kControlGrid-th sample since the last reset. */
  bool due() noexcept { return (count++ % kControlGrid) == 0; }

private:
  unsigned count{0};
};

/** Passes through the loop, after the first echo, until a repeat is 60 dB
    down: ceil(log(0.001) / log(gain)). -1 when the loop never gets there. */
inline double repeatsAfterFirst(double loopGain) noexcept {
  if (loopGain <= 0.0005)
    return 0.0;
  if (loopGain >= 0.9995)
    return -1.0;
  return std::ceil(std::log(0.001) / std::log(loopGain));
}

/** How long a feedback delay stays above -60 dB once its input stops: the
    first echo arrives `passSeconds` after the input and every further one
    `passSeconds` later, `loopGain` quieter each time. `perPassExtra` is what
    one trip round the loop can add on top of the delay (modulation
    excursion, filter group delay); `fixedExtra` is added once (a glide in
    progress). The loop filters and saturators only ever lower the gain, so
    this is an upper bound. */
inline double feedbackTailSeconds(double passSeconds, double loopGain, double perPassExtra,
                                  double fixedExtra) noexcept {
  const double after = repeatsAfterFirst(loopGain);
  if (after < 0.0)
    return kTailForever;
  return (passSeconds + perPassExtra) * (1.0 + after) + fixedExtra;
}

/** Group delay in seconds of `poles` one-pole low-passes at `cutoffHz` at low
    frequencies, for the tail bound. */
inline double lowpassDelaySeconds(double cutoffHz, int poles) noexcept {
  return static_cast<double>(poles) / (juce::MathConstants<double>::twoPi * std::max(cutoffHz, 1.0));
}

/** Transparent below 0.8, then bends smoothly (slope 1 at the knee) towards
    exactly +-1: a ceiling inside a feedback loop that leaves ordinary
    levels alone and can never let a hot input or a loop gain of 1 run away.
    |softLimit(x)| <= |x| everywhere, so it only ever lowers the loop gain. */
inline float softLimit(float x) noexcept {
  const float a = std::abs(x);
  if (a <= 0.8f)
    return x;
  const float limited = 0.8f + 0.2f * fx::fastTanh((a - 0.8f) * 5.0f);
  return x < 0.0f ? -limited : limited;
}

/** A saturator with unity gain for small signals: tanh(drive x) / drive.
    Odd, so it adds no DC; its output is never larger than its input and never
    beyond +-1/drive. `drive` >= 1, `inverseDrive` = 1 / drive. */
inline float saturate(float x, float drive, float inverseDrive) noexcept {
  return fx::fastTanh(drive * x) * inverseDrive;
}

/**
 * A delay time that moves by crossfading between two taps of the same line:
 * the old position fades out as the new one fades in, so a change is never a
 * pitch bend and never a jump, whatever its size. While a fade runs the new
 * target waits; when it ends the tap sits at the position the fade went to
 * and, if the parameter has moved on, the next fade starts from there. That
 * keeps the incoming tap still while it is audible (retargeting it would step
 * the waveform in proportion to its weight), at the price of a turned knob
 * arriving in fade-length hops, which sound like a smooth sweep.
 *
 *   setTarget(delaySamples);          // once per block
 *   const auto taps = tap.next();     // once per sample
 *   y = read(taps.from) + taps.toWeight * (read(taps.to) - read(taps.from))
 *
 * Audio thread only after prepare.
 */
class CrossfadeTap {
public:
  struct Taps {
    float from;
    float to;
    float toWeight;  // 0 while settled: `to` need not be read
  };

  void prepare(double sampleRate, double fadeSeconds) noexcept {
    step = static_cast<float>(1.0 / std::max(1.0, fadeSeconds * sampleRate));
    snapNext = true;
    fading = false;
  }

  /** The next setTarget takes effect at once (a block that was just reset or
      switched on starts where its settings are). */
  void snapOnNextTarget() noexcept {
    snapNext = true;
    fading = false;
  }

  void setTarget(float delaySamples) noexcept {
    target = delaySamples;
    if (snapNext) {
      current = delaySamples;
      snapNext = false;
    }
  }

  Taps next() noexcept {
    if (!fading && target != current) {
      fading = true;
      incoming = target;
      progress = 0.0f;
    }
    if (!fading)
      return {current, current, 0.0f};
    progress += step;
    if (progress >= 1.0f) {
      const Taps last{current, incoming, 1.0f};
      current = incoming;
      fading = false;
      return last;
    }
    return {current, incoming, progress};
  }

  /** The longer of the settled and the incoming position, for tail bounds. */
  float longest() const noexcept { return fading ? std::max(current, incoming) : current; }

private:
  float step{0.001f};
  float target{0.0f};
  float current{0.0f};
  float incoming{0.0f};
  float progress{0.0f};
  bool fading{false};
  bool snapNext{true};
};

/**
 * A delay time that moves by slewing the read pointer, the way a tape
 * transport or a bucket-brigade clock does: the pointer chases the target
 * exponentially but never faster than `maxSlope` samples per sample, so a
 * change is heard as a pitch bend (the echo plays slower while the delay
 * grows, faster while it shrinks) whose speed error is capped at maxSlope
 * (0.3 is about 6 semitones flat or 4.5 sharp) instead of growing with the size
 * of the jump. The path from here to the target is continuous, so there is
 * no click.
 *
 * One instance serves every channel (they share a target). Audio thread only
 * after prepare.
 */
class DelaySlew {
public:
  void prepare(double sampleRate, double tauSeconds, double newMaxSlope) noexcept {
    coeff = static_cast<float>(1.0 - std::exp(-1.0 / std::max(1.0, tauSeconds * sampleRate)));
    maxSlope = static_cast<float>(newMaxSlope);
    snapNext = true;
  }

  void snapOnNextTarget() noexcept { snapNext = true; }

  void setTarget(float delaySamples) noexcept {
    target = delaySamples;
    if (snapNext) {
      value = delaySamples;
      snapNext = false;
    }
  }

  float next() noexcept {
    const float difference = target - value;
    if (std::abs(difference) < 0.02f) {
      value = target;
    } else {
      value += juce::jlimit(-maxSlope, maxSlope, difference * coeff);
    }
    return value;
  }

  float current() const noexcept { return value; }

private:
  float coeff{0.001f};
  float maxSlope{0.3f};
  float target{0.0f};
  float value{0.0f};
  bool snapNext{true};
};

}  // namespace delayfx

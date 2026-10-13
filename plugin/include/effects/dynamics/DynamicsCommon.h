#pragma once
// Shared pieces of the compressor styles (plugin/src/effects/dynamics): the
// stereo layout rule, the dB helpers, a peak detector, the soft-knee curve and
// the gain reduction state with its program-dependent release. Header only,
// audio thread, no allocation.
#include <cmath>
#include "effects/EffectDsp.h"

namespace dynfx {

constexpr int kMaxChannels = 2;
constexpr float kDbPerNeper = 8.68588964f;  // 20 / ln(10)
constexpr float kNeperPerDb = 0.115129255f;  // ln(10) / 20

/** Level (linear, above zero) in dB. */
inline float levelToDb(float level) noexcept { return kDbPerNeper * std::log(level); }

/** Gain for a number of dB; exactly 1 for 0 so an idle block is bit-exact. */
inline float dbToGainFast(float db) noexcept { return db == 0.0f ? 1.0f : std::exp(kNeperPerDb * db); }

/** Step of a one-pole whose time constant is `seconds` (63% of a step). */
inline float stepFor(double seconds, double sampleRate) noexcept {
  return static_cast<float>(-std::expm1(-1.0 / (seconds * sampleRate)));
}

/** The same step without the exponential, for a time constant of at least a
    few hundred samples (the error is below 0.1%). Used where the time
    changes with the programme, every sample. */
inline float stepForSlow(float seconds, double sampleRate) noexcept {
  return 1.0f / (seconds * static_cast<float>(sampleRate));
}

/** How a block is laid out: one stereo signal shares one gain reduction (the
    image holds), dual mono is two guitars each compressed on its own. */
struct Lanes {
  int channels;
  int count;
  bool linked;
};

inline Lanes lanesFor(int numChannels, bool dualMono) noexcept {
  Lanes lanes;
  lanes.channels = numChannels < kMaxChannels ? numChannels : kMaxChannels;
  lanes.linked = lanes.channels == 2 && !dualMono;
  lanes.count = lanes.linked ? 1 : lanes.channels;
  return lanes;
}

/** Peak detector: rises at once, falls with a one-pole. */
struct PeakDetector {
  float level{0.0f};
  void reset() noexcept { level = 0.0f; }
  float process(float input, float fallKeep) noexcept {
    level = input > level ? input : input + fallKeep * (level - input);
    return level;
  }
};

/**
 * The static curve of a feed-forward compressor: how many dB (>= 0) a level
 * `over` dB above the threshold is pulled down. `slope` is 1 - 1/ratio.
 * `knee` is the width in dB of the quadratic knee centred on the threshold;
 * 0 is a hard knee.
 */
inline float kneeReduction(float over, float knee, float slope) noexcept {
  if (knee > 0.0f && 2.0f * over > -knee && 2.0f * over < knee) {
    const float t = over + 0.5f * knee;
    return slope * t * t / (2.0f * knee);
  }
  return over > 0.0f ? slope * over : 0.0f;
}

/**
 * The gain reduction in dB and a slow average of the reduction the curve asks
 * for. The reduction moves toward its target with an attack step when it has
 * to grow and a release step when it relaxes (both one-poles on the dB value,
 * so the static curve is exact whatever the attack). The average is what
 * programme-dependent releases read: it is high after sustained heavy
 * reduction and low after a short transient, and it follows the target, not
 * the reduction, so a release that is under way does not feed itself.
 */
struct Reduction {
  float value{0.0f};
  float average{0.0f};

  void reset() noexcept { value = average = 0.0f; }

  float next(float target, float attackStep, float releaseStep, float averageStep) noexcept {
    value += (target > value ? attackStep : releaseStep) * (target - value);
    if (value < 1.0e-6f && target == 0.0f)
      value = 0.0f;
    average += averageStep * (target - average);
    if (average < 1.0e-6f)
      average = 0.0f;
    return value;
  }
};

}  // namespace dynfx

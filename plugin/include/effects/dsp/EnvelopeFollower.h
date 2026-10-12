#pragma once
#include <cmath>

namespace fx {

/**
 * Level detector with separate attack and release, for compressors, gates,
 * envelope filters and auto-wahs. One channel per instance; feed it the
 * louder channel (or the sum) for linked stereo detection.
 *
 * Peak follows |x|. Rms follows x squared and returns its square root, so
 * its times act on power: the same settings read a little slower than Peak.
 * A time is how long the envelope takes to cover about 63% of a step. The
 * result is linear (use gainToDb for a level). Audio thread; no allocation.
 */
class EnvelopeFollower {
public:
  enum class Mode { Peak, Rms };

  void prepare(double newSampleRate) noexcept {
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    setTimes(attackMs, releaseMs);
    reset();
  }
  void reset() noexcept { state = 0.0f; }

  void setMode(Mode newMode) noexcept { mode = newMode; }

  /** Attack and release in milliseconds; 0 follows instantly. Cheap, but it
      takes two exp(): once per block, not per sample. */
  void setTimes(float newAttackMs, float newReleaseMs) noexcept {
    attackMs = newAttackMs;
    releaseMs = newReleaseMs;
    attackCoeff = coeffFor(attackMs);
    releaseCoeff = coeffFor(releaseMs);
  }

  /** The envelope after this sample. */
  float process(float x) noexcept {
    const float in = mode == Mode::Peak ? std::abs(x) : x * x;
    const float coeff = in > state ? attackCoeff : releaseCoeff;
    state = in + coeff * (state - in);
    return mode == Mode::Peak ? state : std::sqrt(state);
  }

  /** The envelope as last computed, without feeding a sample. */
  float current() const noexcept { return mode == Mode::Peak ? state : std::sqrt(state); }

private:
  float coeffFor(float ms) const noexcept {
    return ms > 0.0f ? static_cast<float>(std::exp(-1.0 / (0.001 * static_cast<double>(ms) * sampleRate)))
                     : 0.0f;
  }

  double sampleRate{48000.0};
  float attackMs{10.0f}, releaseMs{100.0f};
  float attackCoeff{0.0f}, releaseCoeff{0.0f};
  float state{0.0f};
  Mode mode{Mode::Peak};
};

}  // namespace fx

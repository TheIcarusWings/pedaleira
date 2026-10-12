#pragma once
#include <cstdint>

namespace fx {

/** A well-mixed 32-bit hash (the "lowbias32" finaliser). Stateless, so a
    value can be derived from a counter without keeping a generator. */
inline std::uint32_t hash32(std::uint32_t x) noexcept {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

/** A hash as a float in [-1, 1). */
inline float hashToBipolar(std::uint32_t x) noexcept {
  return static_cast<float>(static_cast<std::int32_t>(hash32(x))) * (1.0f / 2147483648.0f);
}

/**
 * The seed an effect's random source starts from. Always the same for the
 * same `salt`, so two renders of one rig are identical; the salt keeps two
 * sources in one effect (left and right, noise and wobble) from running the
 * same stream. When an offline render has pinned the random streams
 * (TONE3000Processor::setDeterministicRandomForOfflineRender, the hook the
 * Spread wobble uses) that seed is mixed in, so a caller can still ask for a
 * different, equally repeatable render. Reads two atomics: safe on the audio
 * thread.
 */
std::uint32_t effectNoiseSeed(std::uint32_t salt) noexcept;

/**
 * A small deterministic white noise source (xorshift32) for dither-level
 * noise, random modulation and diffusion. Never seeded from the clock: seed
 * it in prepareEffect and again in resetEffect, so every render and every
 * power-on starts the same stream.
 */
class NoiseSource {
public:
  explicit NoiseSource(std::uint32_t salt = 0) noexcept { seed(salt); }

  /** Restart the stream for `salt` (see effectNoiseSeed). */
  void seed(std::uint32_t salt) noexcept {
    state = effectNoiseSeed(salt);
    if (state == 0)
      state = 0x9e3779b9u;  // xorshift has no way out of zero
  }

  std::uint32_t nextUInt() noexcept {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  }

  /** Uniform in [-1, 1). */
  float nextBipolar() noexcept {
    return static_cast<float>(static_cast<std::int32_t>(nextUInt())) * (1.0f / 2147483648.0f);
  }

  /** Uniform in [0, 1). */
  float nextUnipolar() noexcept {
    return static_cast<float>(nextUInt() >> 8) * (1.0f / 16777216.0f);
  }

private:
  std::uint32_t state{1};
};

}  // namespace fx

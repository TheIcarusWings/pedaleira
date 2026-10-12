#pragma once
#include <juce_core/juce_core.h>

namespace fx {

/**
 * A tanh-shaped saturator without the transcendental: x(27 + x^2)/(27 + 9x^2),
 * clamped so it reaches exactly +-1 at |x| = 3 and stays there. Unity slope
 * at zero, odd, monotonic, within about 2.5% of tanh. For feedback paths,
 * drive stages and anything called per sample.
 */
inline float fastTanh(float x) noexcept {
  const float c = juce::jlimit(-3.0f, 3.0f, x);
  const float c2 = c * c;
  return c * (27.0f + c2) / (27.0f + 9.0f * c2);
}

/**
 * Cubic soft clip: unity slope at zero, flat at +-1 from |x| = 1.5 on. Softer
 * knee and fewer high harmonics than fastTanh; a safety limiter for a
 * feedback loop that must never run away.
 */
inline float softClip(float x) noexcept {
  const float c = juce::jlimit(-1.5f, 1.5f, x);
  return c - (4.0f / 27.0f) * c * c * c;
}

/** Plain clamp to +-limit. */
inline float hardClip(float x, float limit = 1.0f) noexcept {
  return juce::jlimit(-limit, limit, x);
}

}  // namespace fx

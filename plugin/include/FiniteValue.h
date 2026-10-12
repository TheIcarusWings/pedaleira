// Numbers out of a saved tree (a host's state, a preset file), read so that
// what is not a finite number is the default. A NaN passes straight through
// juce::jlimit (every comparison with it is false) and would reach a gain or
// a filter, and a NaN or an infinity cast to int is undefined.
#pragma once

#include <juce_core/juce_core.h>

#include <cmath>

namespace t3k {

inline float finiteFloat(const juce::var& value, float fallback) {
  if (value.isVoid())
    return fallback;
  const double number = static_cast<double>(value);
  if (!std::isfinite(number) || std::abs(number) > 1.0e30)
    return fallback;
  return static_cast<float>(number);
}

inline int finiteInt(const juce::var& value, int fallback) {
  if (value.isVoid())
    return fallback;
  const double number = static_cast<double>(value);
  if (!std::isfinite(number) || std::abs(number) > 1.0e9)
    return fallback;
  return static_cast<int>(number);
}

}  // namespace t3k

#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>

namespace fx {

/** dB to linear gain and back: juce::Decibels under shorter names, with the
    floor every effect uses (-100 dB and below is silence, gain 0). */
inline float dbToGain(float db) noexcept { return juce::Decibels::decibelsToGain(db, -100.0f); }
inline float gainToDb(float gain) noexcept { return juce::Decibels::gainToDecibels(gain, -100.0f); }

/**
 * Denormals. The audio callback and the lane workers already run under
 * juce::ScopedNoDenormals (flush-to-zero), so an effect's process code needs
 * no guard of its own. DenormalGuard is that same scope, for code that runs
 * a filter or a feedback loop on another thread (an offline analysis, a
 * test).
 */
using DenormalGuard = juce::ScopedNoDenormals;

/** Snap a value that has decayed below any audible level to exactly zero.
    For state kept across blocks where flush-to-zero cannot be relied on, and
    for letting a feedback path reach true silence so a tail test ends. */
inline float flushDenormal(float x) noexcept { return std::abs(x) < 1.0e-15f ? 0.0f : x; }

}  // namespace fx

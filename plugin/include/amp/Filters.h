#pragma once
// amp/Filters.h: the two small linear filters the amp components share.
//
// What they are: a one-pole low-pass / high-pass (6 dB per octave) and a
// state variable filter in the trapezoidal (TPT) form, 12 dB per octave, with
// low, band and high outputs (Zavalishin, "The Art of VA Filter Design").
//
// Plain C++ and std only, so the amp components can be lifted into another
// project. One instance is ONE channel: keep one per channel.
//
// Sample rate: every setter takes the rate the filter runs at (for an
// oversampled amp that is the oversampled rate) and clamps the frequency
// below 0.49 of it, so any setting is stable. Coefficients are plain floats;
// setting them on the audio thread allocates nothing and is safe under
// per-block or per-control-tick modulation.
#include <algorithm>
#include <cmath>

namespace amp {

constexpr double kTwoPi = 6.283185307179586476925286766559;

/** Snaps a value that has decayed to nothing audible to exactly zero. */
inline float flushTiny(float x) noexcept { return std::abs(x) < 1.0e-15f ? 0.0f : x; }

class OnePole {
public:
  void setCutoff(double hz, double sampleRate) noexcept {
    const double fc = std::clamp(hz, 1.0, 0.49 * sampleRate);
    coeff = static_cast<float>(1.0 - std::exp(-kTwoPi * fc / sampleRate));
  }
  void reset(float value = 0.0f) noexcept { state = value; }

  float lowpass(float x) noexcept {
    state += coeff * (x - state);
    return state;
  }
  /** The complement of the low-pass; one state, so use either lowpass or
      highpass on an instance, not both. */
  float highpass(float x) noexcept { return x - lowpass(x); }
  /** The smoothing coefficient and the state: the next low-pass output is
      state + coefficient * (x - state). */
  float coefficient() const noexcept { return coeff; }
  float value() const noexcept { return state; }

private:
  float coeff{1.0f};
  float state{0.0f};
};

class Svf {
public:
  struct Outputs {
    float low, band, high;
  };

  /** `q` 0.3 to 40; the band output peaks at q, not at unity. */
  void set(double hz, double q, double sampleRate) noexcept {
    const double fc = std::clamp(hz, 5.0, 0.49 * sampleRate);
    const double g = std::tan(0.5 * kTwoPi * fc / sampleRate);
    const double kd = 1.0 / std::clamp(q, 0.3, 40.0);
    k = static_cast<float>(kd);
    a1 = static_cast<float>(1.0 / (1.0 + g * (g + kd)));
    a2 = static_cast<float>(g) * a1;
    a3 = static_cast<float>(g) * a2;
  }
  void reset() noexcept { ic1 = ic2 = 0.0f; }
  /** The next band output is bandOffset() + bandSlope() * x. */
  float bandOffset() const noexcept { return a1 * ic1 - a2 * ic2; }
  float bandSlope() const noexcept { return a2; }

  Outputs process(float x) noexcept {
    const float v3 = x - ic2;
    const float v1 = a1 * ic1 + a2 * v3;
    const float v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2.0f * v1 - ic1;
    ic2 = 2.0f * v2 - ic2;
    return {v2, v1, x - k * v1 - v2};
  }

private:
  float k{1.4142f}, a1{0.0f}, a2{0.0f}, a3{0.0f};
  float ic1{0.0f}, ic2{0.0f};
};

}  // namespace amp

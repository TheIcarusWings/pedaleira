#pragma once
#include <juce_core/juce_core.h>
#include <cmath>

namespace fx {

// Each filter below is ONE channel: keep one instance per channel. They hold
// plain floats, so setting coefficients on the audio thread is free of
// allocation, which is the reason not to use juce::dsp::IIR here (its
// coefficient objects are reference-counted and allocate when replaced).

/**
 * One-pole low-pass / high-pass (6 dB per octave): tone controls, damping in
 * a feedback path, smoothing a control signal. setCutoff is cheap enough to
 * call per block; the filter is stable at any cutoff and under any
 * modulation.
 */
class OnePole {
public:
  void setCutoff(double cutoffHz, double sampleRate) noexcept {
    const double fc = juce::jlimit(1.0, 0.49 * sampleRate, cutoffHz);
    coeff = static_cast<float>(1.0 - std::exp(-juce::MathConstants<double>::twoPi * fc / sampleRate));
  }
  void reset(float value = 0.0f) noexcept { state = value; }

  float lowpass(float x) noexcept {
    state += coeff * (x - state);
    return state;
  }
  /** The complement of the low-pass (one state, so use one or the other per
      instance). */
  float highpass(float x) noexcept { return x - lowpass(x); }

private:
  float coeff{1.0f};
  float state{0.0f};
};

/**
 * State-variable filter in the trapezoidal (TPT) form: low, band and high
 * outputs from one stage, 12 dB per octave. Unlike a biquad it stays stable
 * and well behaved when cutoff and resonance move every block or every
 * sample, so it is the filter for wahs, envelope filters, phaser-like sweeps
 * and synth-style resonance. set() costs one tan(); updating it once per
 * block or every 16 samples is plenty for audible sweeps.
 */
class StateVariableFilter {
public:
  struct Outputs {
    float low;
    float band;
    float high;
  };

  /** `q` 0.5 (soft) to about 20 (ringing); 0.7071 is Butterworth. The band
      output peaks at q, not at unity. */
  void set(double cutoffHz, double q, double sampleRate) noexcept {
    const double fc = juce::jlimit(5.0, 0.49 * sampleRate, cutoffHz);
    const double g = std::tan(juce::MathConstants<double>::pi * fc / sampleRate);
    const double kd = 1.0 / juce::jlimit(0.3, 40.0, q);
    k = static_cast<float>(kd);
    a1 = static_cast<float>(1.0 / (1.0 + g * (g + kd)));
    a2 = static_cast<float>(g) * a1;
    a3 = static_cast<float>(g) * a2;
  }
  void reset() noexcept { ic1 = ic2 = 0.0f; }

  Outputs process(float x) noexcept {
    const float v3 = x - ic2;
    const float v1 = a1 * ic1 + a2 * v3;
    const float v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2.0f * v1 - ic1;
    ic2 = 2.0f * v2 - ic2;
    return {v2, v1, x - k * v1 - v2};
  }
  float lowpass(float x) noexcept { return process(x).low; }
  float bandpass(float x) noexcept { return process(x).band; }
  float highpass(float x) noexcept { return process(x).high; }

private:
  float k{1.4142f}, a1{0.0f}, a2{0.0f}, a3{0.0f};
  float ic1{0.0f}, ic2{0.0f};
};

/**
 * Biquad coefficients as plain values, with the usual designs (the "Audio EQ
 * Cookbook" forms). Build them anywhere, including on the audio thread.
 * Gains are in dB, `q` is the usual quality factor (shelves: 0.7071 for the
 * steepest slope without overshoot).
 */
struct BiquadCoeffs {
  float b0{1.0f}, b1{0.0f}, b2{0.0f}, a1{0.0f}, a2{0.0f};

  static BiquadCoeffs lowpass(double hz, double q, double sampleRate) noexcept {
    const Design d(hz, q, sampleRate);
    return d.make((1.0 - d.cosW) * 0.5, 1.0 - d.cosW, (1.0 - d.cosW) * 0.5, 1.0 + d.alpha,
                  -2.0 * d.cosW, 1.0 - d.alpha);
  }
  static BiquadCoeffs highpass(double hz, double q, double sampleRate) noexcept {
    const Design d(hz, q, sampleRate);
    return d.make((1.0 + d.cosW) * 0.5, -(1.0 + d.cosW), (1.0 + d.cosW) * 0.5, 1.0 + d.alpha,
                  -2.0 * d.cosW, 1.0 - d.alpha);
  }
  /** Unity gain at the centre. */
  static BiquadCoeffs bandpass(double hz, double q, double sampleRate) noexcept {
    const Design d(hz, q, sampleRate);
    return d.make(d.alpha, 0.0, -d.alpha, 1.0 + d.alpha, -2.0 * d.cosW, 1.0 - d.alpha);
  }
  static BiquadCoeffs notch(double hz, double q, double sampleRate) noexcept {
    const Design d(hz, q, sampleRate);
    return d.make(1.0, -2.0 * d.cosW, 1.0, 1.0 + d.alpha, -2.0 * d.cosW, 1.0 - d.alpha);
  }
  static BiquadCoeffs allpass(double hz, double q, double sampleRate) noexcept {
    const Design d(hz, q, sampleRate);
    return d.make(1.0 - d.alpha, -2.0 * d.cosW, 1.0 + d.alpha, 1.0 + d.alpha, -2.0 * d.cosW,
                  1.0 - d.alpha);
  }
  static BiquadCoeffs peak(double hz, double q, double gainDb, double sampleRate) noexcept {
    const Design d(hz, q, sampleRate);
    const double a = std::pow(10.0, gainDb / 40.0);
    return d.make(1.0 + d.alpha * a, -2.0 * d.cosW, 1.0 - d.alpha * a, 1.0 + d.alpha / a,
                  -2.0 * d.cosW, 1.0 - d.alpha / a);
  }
  static BiquadCoeffs lowShelf(double hz, double q, double gainDb, double sampleRate) noexcept {
    const Design d(hz, q, sampleRate);
    const double a = std::pow(10.0, gainDb / 40.0);
    const double s = 2.0 * std::sqrt(a) * d.alpha;
    return d.make(a * ((a + 1.0) - (a - 1.0) * d.cosW + s),
                  2.0 * a * ((a - 1.0) - (a + 1.0) * d.cosW),
                  a * ((a + 1.0) - (a - 1.0) * d.cosW - s), (a + 1.0) + (a - 1.0) * d.cosW + s,
                  -2.0 * ((a - 1.0) + (a + 1.0) * d.cosW), (a + 1.0) + (a - 1.0) * d.cosW - s);
  }
  static BiquadCoeffs highShelf(double hz, double q, double gainDb, double sampleRate) noexcept {
    const Design d(hz, q, sampleRate);
    const double a = std::pow(10.0, gainDb / 40.0);
    const double s = 2.0 * std::sqrt(a) * d.alpha;
    return d.make(a * ((a + 1.0) + (a - 1.0) * d.cosW + s),
                  -2.0 * a * ((a - 1.0) + (a + 1.0) * d.cosW),
                  a * ((a + 1.0) + (a - 1.0) * d.cosW - s), (a + 1.0) - (a - 1.0) * d.cosW + s,
                  2.0 * ((a - 1.0) - (a + 1.0) * d.cosW), (a + 1.0) - (a - 1.0) * d.cosW - s);
  }

  /** Linear magnitude of the response at `hz`, for tests and curve drawing. */
  double magnitudeAt(double hz, double sampleRate) const noexcept {
    const double w = juce::MathConstants<double>::twoPi * hz / sampleRate;
    const double c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2.0 * w), s2 = std::sin(2.0 * w);
    const double numRe = b0 + b1 * c1 + b2 * c2, numIm = -(b1 * s1 + b2 * s2);
    const double denRe = 1.0 + a1 * c1 + a2 * c2, denIm = -(a1 * s1 + a2 * s2);
    return std::sqrt((numRe * numRe + numIm * numIm) / (denRe * denRe + denIm * denIm));
  }

private:
  // The terms every cookbook design shares, with the inputs kept in range.
  struct Design {
    Design(double hz, double q, double sampleRate) noexcept {
      const double w =
          juce::MathConstants<double>::twoPi * juce::jlimit(5.0, 0.49 * sampleRate, hz) / sampleRate;
      cosW = std::cos(w);
      alpha = std::sin(w) / (2.0 * juce::jlimit(0.05, 50.0, q));
    }
    BiquadCoeffs make(double nb0, double nb1, double nb2, double na0, double na1,
                      double na2) const noexcept {
      BiquadCoeffs c;
      c.b0 = static_cast<float>(nb0 / na0);
      c.b1 = static_cast<float>(nb1 / na0);
      c.b2 = static_cast<float>(nb2 / na0);
      c.a1 = static_cast<float>(na1 / na0);
      c.a2 = static_cast<float>(na2 / na0);
      return c;
    }
    double cosW{1.0};
    double alpha{0.0};
  };
};

/**
 * One biquad section (transposed direct form II) run from BiquadCoeffs: the
 * filter for fixed or slowly moving EQ shapes, shelves and crossovers.
 * Changing coefficients while it runs is safe but can click on large jumps;
 * ramp the frequency or gain with a SmoothedParam and rebuild per block, or
 * use StateVariableFilter for fast sweeps.
 */
class Biquad {
public:
  void setCoeffs(const BiquadCoeffs& newCoeffs) noexcept { c = newCoeffs; }
  const BiquadCoeffs& coeffs() const noexcept { return c; }
  void reset() noexcept { z1 = z2 = 0.0f; }

  float process(float x) noexcept {
    const float y = c.b0 * x + z1;
    z1 = c.b1 * x - c.a1 * y + z2;
    z2 = c.b2 * x - c.a2 * y;
    return y;
  }

private:
  BiquadCoeffs c;
  float z1{0.0f}, z2{0.0f};
};

}  // namespace fx

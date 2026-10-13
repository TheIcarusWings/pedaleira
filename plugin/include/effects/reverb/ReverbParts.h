#pragma once
// Building blocks shared by the reverb kinds (plugin/src/effects/reverb).
// Private to the family: nothing here registers a kind.
//
// Three conventions every reverb follows, so the five kinds behave alike:
//
//   decay     The decay parameter IS the RT60. Every reverb starts from a
//             lossless prototype (unitary feedback, allpass diffusion, plain
//             delays) and then scales each delay of n samples by
//             gamma^n, gamma = 10^(-3 / (RT60 * rate)). That multiplies the
//             whole impulse response by gamma^t, so every mode loses 60 dB in
//             RT60 seconds whatever its frequency. Damping is one first-order
//             shelf per delay that takes the same law to a shorter RT60 above
//             its corner.
//   level     wetEnergyTarget(): the energy of the wet impulse response as a
//             function of the decay time alone, the same for all kinds, so
//             swapping one reverb for another keeps the balance.
//   control   Coefficients move on a fixed grid of kControlInterval samples
//             counted from the last reset, never per host block, and ramp
//             linearly in between: the output does not depend on the block
//             size and a moving knob does not zipper.
#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "effects/dsp/DelayLine.h"
#include "effects/dsp/Filters.h"

namespace fx::reverb {

inline constexpr int kControlInterval = 32;

// ln(1000): 60 dB as a natural-log amplitude ratio.
inline constexpr double kLn1000 = 6.907755278982137;

/** Gain of a delay of `delaySamples` in a loop that decays 60 dB in
    `rt60Seconds`. */
inline double decayGain(double delaySamples, double rt60Seconds, double sampleRate) noexcept {
  return std::exp(-kLn1000 * delaySamples / (std::max(1.0e-3, rt60Seconds) * sampleRate));
}

/**
 * Energy of the wet impulse response (the power gain for steady noise) every
 * reverb is normalised to: -3 dB at a 2 s decay, rising with the square root
 * of the decay time. A lossless loop left alone gets louder in proportion to
 * its decay time (18 dB from 0.2 s to 12 s); a full correction would make
 * long tails start too quiet. Half of it in dB keeps the knob a length
 * control that still fills out as it opens.
 */
inline double wetEnergyTarget(double rt60Seconds) noexcept {
  return 0.5 * std::sqrt(std::max(1.0e-3, rt60Seconds) / 2.0);
}

/** A value that lands exactly on its target at each control tick and moves
    linearly across the interval in between. */
struct Ramped {
  float value{0.0f};
  float target{0.0f};
  float delta{0.0f};

  void snap(float v) noexcept {
    value = target = v;
    delta = 0.0f;
  }
  /** At a control tick: start from the last target, head for `next`. */
  void aim(float next) noexcept {
    value = target;
    target = next;
    delta = (next - value) * (1.0f / static_cast<float>(kControlInterval));
  }
  float step() noexcept {
    const float v = value;
    value += delta;
    return v;
  }
};

/** One-pole smoothing of a control value at the control rate, exact on its
    target once it is close: the settled state costs nothing downstream. */
class ControlSmoother {
public:
  void prepare(double sampleRate, double seconds) noexcept {
    const double ticksPerSecond = sampleRate / static_cast<double>(kControlInterval);
    coeff = static_cast<float>(1.0 - std::exp(-1.0 / (ticksPerSecond * std::max(1.0e-4, seconds))));
    snapNext = true;
  }
  void snapOnNextTarget() noexcept { snapNext = true; }

  /** One control tick toward `target`; returns the value to use. */
  float advance(float target) noexcept {
    if (snapNext) {
      value = target;
      snapNext = false;
    } else if (value != target) {
      value += coeff * (target - value);
      if (std::abs(target - value) <= 1.0e-4f * std::max(1.0e-3f, std::abs(target)))
        value = target;
    }
    return value;
  }
  float current() const noexcept { return value; }

private:
  float value{0.0f};
  float coeff{1.0f};
  bool snapNext{true};
};

/** Primes up to a limit, for delay lengths that share no common factor (two
    lines with a common divisor pile their echoes onto the same instants). */
class PrimeTable {
public:
  void prepare(int maxValue) {
    limit = std::max(8, maxValue);
    composite.assign(static_cast<size_t>(limit) + 1, 0);
    composite[0] = composite[1] = 1;
    for (int p = 2; p * p <= limit; ++p)
      if (composite[static_cast<size_t>(p)] == 0)
        for (int q = p * p; q <= limit; q += p)
          composite[static_cast<size_t>(q)] = 1;
  }

  /** The smallest prime >= n (the largest one in the table when n is past
      it). A bounded scan of a table: fine on the audio thread. */
  int atLeast(int n) const noexcept {
    int p = juce::jlimit(2, limit, n);
    while (p < limit && composite[static_cast<size_t>(p)] != 0)
      ++p;
    while (p > 2 && composite[static_cast<size_t>(p)] != 0)
      --p;
    return p;
  }

private:
  std::vector<std::uint8_t> composite;
  int limit{8};
};

/** Up to four Schroeder allpasses in series: the input diffusion that turns
    one impulse into a dense burst before it reaches a tank. Outside any
    loop, so it has no decay scaling. */
class AllpassChain {
public:
  static constexpr int kMaxStages = 4;

  void prepare(const double* seconds, const float* coefficients, int count, double sampleRate) {
    stages = juce::jlimit(0, kMaxStages, count);
    int longest = 2;
    for (int s = 0; s < stages; ++s) {
      length[static_cast<size_t>(s)] =
          std::max(1, static_cast<int>(std::lround(seconds[s] * sampleRate)));
      coeff[static_cast<size_t>(s)] = coefficients[s];
      longest = std::max(longest, length[static_cast<size_t>(s)]);
    }
    line.prepare(std::max(1, stages), longest + 1);
  }
  void clear() noexcept { line.clear(); }

  /** Scales every coefficient (0 to 1): a diffusion knob. 1 is the design,
      and multiplying by 1 changes nothing. */
  void setScale(float newScale) noexcept { scale = newScale; }

  float process(float x) noexcept {
    for (int s = 0; s < stages; ++s) {
      const float k = coeff[static_cast<size_t>(s)] * scale;
      const float delayed = line.read(s, length[static_cast<size_t>(s)]);
      const float v = x - k * delayed;
      line.write(s, v);
      x = delayed + k * v;
    }
    return x;
  }

private:
  fx::DelayLine line;
  std::array<int, kMaxStages> length{};
  std::array<float, kMaxStages> coeff{};
  float scale{1.0f};
  int stages{0};
};

/** Weights of the four samples around a read position `fraction` (0 to 1)
    past the second of them: third-order Lagrange interpolation. */
inline void lagrangeWeights(float fraction, float weights[4]) noexcept {
  const float f = fraction;
  const float below = f + 1.0f, above = f - 1.0f, beyond = f - 2.0f;
  weights[0] = -f * above * beyond * (1.0f / 6.0f);
  weights[1] = below * above * beyond * 0.5f;
  weights[2] = -below * f * beyond * 0.5f;
  weights[3] = below * f * above * (1.0f / 6.0f);
}

/**
 * A read at a fractional delay (2 to the line's longest, less 2) for a delay
 * that moves INSIDE a decaying loop. Third-order Lagrange, and neither of
 * the reads fx::DelayLine offers, because of what a loop close to lossless
 * asks of it: its gain must never exceed 1 at any frequency or position
 * (true of an odd-order Lagrange read between its two middle samples, not
 * of the Hermite read, which overshoots), and it should lose as little as
 * possible below that, since every loss is decay time the parameter did not
 * ask for (the linear read is 0.7 dB down at 6 kHz half way between two
 * samples, this one 0.07 dB).
 */
inline float readLagrange(const fx::DelayLine& line, int channel, float delay) noexcept {
  const int whole = static_cast<int>(delay);
  float w[4];
  lagrangeWeights(delay - static_cast<float>(whole), w);
  return w[0] * line.read(channel, whole - 1) + w[1] * line.read(channel, whole) +
         w[2] * line.read(channel, whole + 1) + w[3] * line.read(channel, whole + 2);
}

/** A sine and a cosine from one rotating vector: two modulation phases a
    quarter turn apart for four multiplies a sample, with no table and no
    call into the maths library. */
struct QuadratureLfo {
  float cosine{1.0f};
  float sine{0.0f};
  float stepCos{1.0f};
  float stepSin{0.0f};

  void setRate(double hz, double sampleRate) noexcept {
    const double angle = juce::MathConstants<double>::twoPi * hz / sampleRate;
    stepCos = static_cast<float>(std::cos(angle));
    stepSin = static_cast<float>(std::sin(angle));
  }
  void reset(double phaseTurns = 0.0) noexcept {
    const double angle = juce::MathConstants<double>::twoPi * phaseTurns;
    cosine = static_cast<float>(std::cos(angle));
    sine = static_cast<float>(std::sin(angle));
  }
  void advance() noexcept {
    const float c = cosine * stepCos - sine * stepSin;
    sine = sine * stepCos + cosine * stepSin;
    cosine = c;
  }
  /** Rounding lets the vector's length drift over hours; one Newton step
      toward length 1 at each control tick holds it. */
  void renormalise() noexcept {
    const float gain = 1.5f - 0.5f * (cosine * cosine + sine * sine);
    cosine *= gain;
    sine *= gain;
  }
};

/** The low cut and high cut on one channel of the wet signal (12 dB per
    octave each). State-variable, so the cutoffs can move while it runs. */
class WetFilter {
public:
  void set(double lowCutHz, double highCutHz, double sampleRate) noexcept {
    lowCut.set(lowCutHz, 0.7071, sampleRate);
    highCut.set(highCutHz, 0.7071, sampleRate);
  }
  void reset() noexcept {
    lowCut.reset();
    highCut.reset();
  }
  float process(float x) noexcept { return highCut.lowpass(lowCut.highpass(x)); }

private:
  fx::StateVariableFilter lowCut;
  fx::StateVariableFilter highCut;
};

/** The excursion, in milliseconds, of a modulated line for a depth knob
    (`depth`, 0 to 1) at an oscillator speed of `rateHz`: at most `maxMs`, and
    never so much that the line's pitch drifts by more than `maxDrift` (a
    fraction: 0.008 is 14 cents). A line moved x(t) = a sin(2 pi f t) drifts by
    2 pi f a at the steepest, so a fast oscillator gets a smaller excursion. */
inline float modExcursionMs(float depth, double rateHz, double maxMs, double maxDrift) noexcept {
  const double byDrift =
      1000.0 * maxDrift / (juce::MathConstants<double>::twoPi * std::max(rateHz, 0.01));
  return depth * static_cast<float>(std::min(maxMs, byDrift));
}

/**
 * The swell of a reverb that builds instead of starting at once. The input
 * of the late field goes through a sparse filter of 24 taps of mixed sign
 * whose weights rise with their delay (so the energy it passes climbs with
 * time), spread over a span that grows with the amount, up to
 * `maxSeconds`. Behind a bank of allpasses and a feedback network each tap
 * becomes a dense cloud, and the sum of the clouds is a field whose level
 * climbs over the span. The share of the direct signal and of the filtered
 * one follow a quarter circle, cos and sin of the amount, so the energy that
 * enters the network is the same at every setting. Amount 0 passes the input
 * as it is, bit for bit, and costs one write.
 */
class AttackSmear {
public:
  static constexpr int kTaps = 24;

  void prepare(double maxSeconds, double newSampleRate) {
    sampleRate = newSampleRate;
    maxSpan = maxSeconds * sampleRate;
    line.prepare(1, static_cast<int>(std::ceil(maxSpan)) + 4);
    // Fixed taps, stratified over the span with a little jitter so no two are
    // evenly spaced, and a sign each: a deterministic sequence, so every run
    // and every channel sounds the same.
    std::uint32_t state = 0x9E3779B9u;
    for (int k = 0; k < kTaps; ++k) {
      state = state * 1664525u + 1013904223u;
      const double jitter = static_cast<double>(state >> 8) / 16777216.0;
      const size_t n = static_cast<size_t>(k);
      fraction[n] = (static_cast<double>(k) + 0.15 + 0.7 * jitter) / kTaps;
      sign[n] = ((state >> 20) & 1u) != 0u ? 1.0f : -1.0f;
    }
    // Weights in proportion to the delay, so the energy of the taps rises with
    // the square of it (the swell outclimbs the decay of a long tail). Unit
    // total energy.
    double energy = 0.0;
    for (int k = 0; k < kTaps; ++k)
      energy += fraction[static_cast<size_t>(k)] * fraction[static_cast<size_t>(k)];
    for (int k = 0; k < kTaps; ++k) {
      const size_t n = static_cast<size_t>(k);
      weight[n] = sign[n] * static_cast<float>(fraction[n] / std::sqrt(energy));
    }
    reset();
  }

  void reset() noexcept {
    line.clear();
    direct.snap(1.0f);
    smeared.snap(0.0f);
    idleTicks = 2;
  }

  /** At a control tick. `amount` is 0 to 1. */
  void control(float amount) noexcept {
    const float a = juce::jlimit(0.0f, 1.0f, amount);
    const bool off = a <= 0.001f;
    const float angle = off ? 0.0f : a * 1.5707963f;
    direct.aim(std::cos(angle));
    smeared.aim(std::sin(angle));
    idleTicks = off ? std::min(idleTicks + 1, 2) : 0;
    // From 20 ms at the start of the knob to the full span.
    const double span = 0.02 * sampleRate + a * (maxSpan - 0.02 * sampleRate);
    for (int k = 0; k < kTaps; ++k)
      position[static_cast<size_t>(k)] =
          std::max(1, static_cast<int>(std::lround(fraction[static_cast<size_t>(k)] * span)));
  }

  float process(float x) noexcept {
    line.write(0, x);
    if (idleTicks >= 2)
      return x;
    float sum = 0.0f;
    for (int k = 0; k < kTaps; ++k)
      sum += weight[static_cast<size_t>(k)] * line.read(0, position[static_cast<size_t>(k)]);
    return direct.step() * x + smeared.step() * sum;
  }

private:
  fx::DelayLine line;
  double sampleRate{48000.0};
  double maxSpan{1.0};
  std::array<double, kTaps> fraction{};
  std::array<float, kTaps> sign{};
  std::array<float, kTaps> weight{};
  std::array<int, kTaps> position{};
  Ramped direct, smeared;
  int idleTicks{2};
};

/** The first-order shelf that follows a delay in a decaying loop: unity at
    low frequencies, `highGain` (at most 1) far above the corner. Its gain
    never exceeds 1 anywhere, so it cannot destabilise the loop it sits in. */
struct DampingShelf {
  float state{0.0f};

  void reset() noexcept { state = 0.0f; }
  float process(float x, float highGain, float corner) noexcept {
    state += corner * (x - state);
    return highGain * x + (1.0f - highGain) * state;
  }
};

/** Coefficient of the one-pole inside DampingShelf for a corner in Hz. */
inline float shelfCorner(double cornerHz, double sampleRate) noexcept {
  const double fc = juce::jlimit(20.0, 0.45 * sampleRate, cornerHz);
  return static_cast<float>(1.0 - std::exp(-juce::MathConstants<double>::twoPi * fc / sampleRate));
}

/** The damping knob (0 to 100 %) as the ratio of the high-frequency RT60 to
    the low-frequency one: 1 at 0 % (every band decays together) down to
    1/12 at 100 %, exponentially, so each step of the knob is the same
    darkening. */
inline double dampingToHighRatio(double percent) noexcept {
  return std::exp(-std::log(12.0) * juce::jlimit(0.0, 100.0, percent) * 0.01);
}

}  // namespace fx::reverb

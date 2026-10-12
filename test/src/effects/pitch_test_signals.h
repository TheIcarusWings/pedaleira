#pragma once
// Signals and measurements shared by the pitch family's tests
// (pitch_shifter_tests.cpp, octaver_tests.cpp, harmonizer_tests.cpp).
// Header-only and inline, because the test sources are separate translation
// units; nothing here is specific to one kind.
#include "effects/effect_test_helpers.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace pitchtest {

using namespace fxtest;

constexpr double kRate = 48000.0;

/** A plucked-guitar-like tone: partials 1..`partials` at 1/n, the
    fundamental the strongest, `seconds` long, peak `amplitude`. */
inline std::vector<float> harmonicTone(double seconds, double fundamentalHz, float amplitude,
                                       int partials = 8) {
  const int frames = static_cast<int>(std::lround(seconds * kRate));
  std::vector<float> x(static_cast<size_t>(frames), 0.0f);
  for (int h = 1; h <= partials; ++h) {
    const double gain = 1.0 / h;
    for (int i = 0; i < frames; ++i)
      x[static_cast<size_t>(i)] +=
          static_cast<float>(gain * std::sin(2.0 * kPi * fundamentalHz * h * i / kRate + h));
  }
  const float peak = peakOf(x);
  for (float& v : x)
    v *= amplitude / peak;
  return x;
}

/** `a` followed by `b` with a hard cut at the join, as a new note is. */
inline std::vector<float> followedBy(std::vector<float> a, const std::vector<float>& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

/** Level in dB of the spectral line at `hz` in x[begin, end), where a
    full-scale sine reads 0 dB (Hann window; see goertzelPower). */
inline double lineDb(const std::vector<float>& x, size_t begin, size_t end, double hz) {
  end = std::min(end, x.size());
  return db(goertzelPower(x.data() + begin, end - begin, hz, kRate));
}

/** Where the strongest line near `expectedHz` sits, in cents from it: a
    1-cent scan over +/-`rangeCents` of a Hann-windowed Goertzel. The
    answer for a signal with nothing near `expectedHz` is meaningless;
    check lineDb first. */
inline double centsFrom(const std::vector<float>& x, size_t begin, size_t end, double expectedHz,
                        int rangeCents = 150) {
  end = std::min(end, x.size());
  double best = -1.0;
  int bestCents = 0;
  for (int cents = -rangeCents; cents <= rangeCents; ++cents) {
    const double power = goertzelPower(x.data() + begin, end - begin,
                                       expectedHz * std::pow(2.0, cents / 1200.0), kRate);
    if (power > best) {
      best = power;
      bestCents = cents;
    }
  }
  return bestCents;
}

inline double shifted(double hz, double semitones) { return hz * std::pow(2.0, semitones / 12.0); }

/** A log sweep from `fromHz` to `toHz` over `seconds`. */
inline std::vector<float> logSweep(double seconds, double fromHz, double toHz, float amplitude) {
  const int frames = static_cast<int>(std::lround(seconds * kRate));
  std::vector<float> x(static_cast<size_t>(frames));
  const double k = std::log(toHz / fromHz);
  for (int i = 0; i < frames; ++i) {
    const double t = i / kRate;
    const double phase = 2.0 * kPi * fromHz * seconds / k * (std::exp(k * t / seconds) - 1.0);
    x[static_cast<size_t>(i)] = amplitude * static_cast<float>(std::sin(phase));
  }
  return x;
}

/** A slowly rising and falling amplitude (a triangle between 0.1 and 1, one
    second up and one down) on `x`: for reading how late a signal arrives from
    how much of the envelope it has reached. */
inline void applyTriangleEnvelope(std::vector<float>& x) {
  for (size_t i = 0; i < x.size(); ++i) {
    const double phase = std::fmod(static_cast<double>(i) / kRate, 2.0);
    x[i] *= static_cast<float>(0.1 + 0.9 * (phase < 1.0 ? phase : 2.0 - phase));
  }
}

/** The average delay, in samples, of `out` behind `in`, for signals shaped by
    applyTriangleEnvelope (period 2 s). A tone whose pitch has been moved
    cannot be lined up with its source by correlation, but its LOUDNESS can:
    on a ramp, how much quieter (or louder) the output is than the input at
    the same instant is the time it lags, divided by the ramp's slope. The
    average over many blocks of the ramps is the average delay of whatever
    path the signal took, however it was shifted. Blocks are 100 ms, kept
    off the corners of the triangle. */
inline double meanDelayFromEnvelopeSamples(const std::vector<float>& out,
                                           const std::vector<float>& in, double fromSeconds) {
  const int block = 4800;
  double sum = 0.0;
  int count = 0;
  const int firstRamp = static_cast<int>(std::ceil(fromSeconds));
  const int lastRamp = static_cast<int>(in.size() / kRate) - 1;
  for (int ramp = firstRamp; ramp < lastRamp; ++ramp) {
    std::vector<double> t, rmsIn, rmsOut;
    for (double begin = ramp + 0.15; begin + block / kRate < ramp + 0.85; begin += block / kRate) {
      const size_t from = static_cast<size_t>(begin * kRate);
      t.push_back(begin + 0.5 * block / kRate);
      rmsIn.push_back(rmsOf(in, from, from + block));
      rmsOut.push_back(rmsOf(out, from, from + block));
    }
    // The ramp's slope in rms per second, by regression over its blocks.
    double meanT = 0.0, meanR = 0.0;
    for (size_t i = 0; i < t.size(); ++i) {
      meanT += t[i];
      meanR += rmsIn[i];
    }
    meanT /= static_cast<double>(t.size());
    meanR /= static_cast<double>(t.size());
    double num = 0.0, den = 0.0;
    for (size_t i = 0; i < t.size(); ++i) {
      num += (t[i] - meanT) * (rmsIn[i] - meanR);
      den += (t[i] - meanT) * (t[i] - meanT);
    }
    const double slope = num / den;
    for (size_t i = 0; i < t.size(); ++i) {
      sum += (rmsIn[i] - rmsOut[i]) / slope * kRate;
      ++count;
    }
  }
  return count > 0 ? sum / count : 0.0;
}

}  // namespace pitchtest

#pragma once
// A bank of equaliser bands for the eq.* kinds: peaking, shelving and
// high/low-pass sections in series, double precision, with every control
// gliding so a move never steps the response.
//
// A band is described each block by a BandTarget (type, frequency, gain, Q,
// active). The bank smooths frequency (on a log scale), gain, Q and the
// band's in/out amount over 20 ms and rebuilds the coefficients on a fixed
// 16-sample grid while any of them is moving, so the sound does not depend on
// the block size. A band that is bypassed, or a peak or shelf at 0 dB, is
// skipped outright once it has settled, so a flat bank is bit-exact
// pass-through. A band that is only partly in is a cross-fade between its
// input and its output, which keeps a bypass click-free for every type.
//
// Double precision matters at the low end: a 20 Hz resonance has its poles a
// thousandth from the unit circle, and a float biquad turns its rounding
// error into noise a flat EQ must not add.
//
//   prepareEffect:  bank.prepare(spec.sampleRate);
//   resetEffect:    bank.reset();
//   processEffect:  bank.setTargets(targets, levelDb);   // once per block
//                   bank.process(channels, numChannels, numSamples);
//
// Audio thread only after prepare. No allocation.
#include <algorithm>
#include <cmath>
#include "effects/dsp/Levels.h"
#include "effects/dsp/Smoothing.h"

namespace fx::eq {

enum class BandType { Peak, LowShelf, HighShelf, HighPass2, LowPass2, HighPass1, LowPass1 };

struct BandTarget {
  BandType type{BandType::Peak};
  float hz{1000.0f};
  float gainDb{0.0f};
  float q{0.7071f};
  bool active{false};
};

/** The Q of the k-th (0 based) second-order section of an n-th order
    Butterworth filter, k < n / 2. */
inline double butterworthQ(int order, int k) noexcept {
  return 1.0 / (2.0 * std::sin((2.0 * k + 1.0) * 3.14159265358979323846 / (2.0 * order)));
}

/** A log-skewed taper hint (juce's skew) putting the half-way point of a
    control at `mid`. */
inline float skewForMidpoint(double lo, double hi, double mid) noexcept {
  return static_cast<float>(std::log(0.5) / std::log((mid - lo) / (hi - lo)));
}

/** Whether the band shapes the level (a gain of 0 dB is then no band). */
inline bool hasGain(BandType type) noexcept {
  return type == BandType::Peak || type == BandType::LowShelf || type == BandType::HighShelf;
}

/** Slowest ringing of the band, as seconds to fall 60 dB (0 when it is
    not in). Used for the reported tail. */
inline double ringSeconds(const BandTarget& band) noexcept {
  if (!band.active)
    return 0.0;
  double poleQ = band.q;
  if (band.type == BandType::HighPass1 || band.type == BandType::LowPass1)
    poleQ = 0.5;
  else if (hasGain(band.type)) {
    if (std::abs(band.gainDb) < 0.05f)
      return 0.0;
    // A boost is a resonance whose poles sit at Q * boost; a cut rings less.
    poleQ *= std::pow(10.0, std::max(band.gainDb, 0.0f) / 40.0);
  }
  return 6.9078 * poleQ / (3.14159265358979 * std::max(band.hz, 1.0f));
}

/** The coefficients of one section (Audio EQ Cookbook forms, first-order
    sections by the bilinear transform). */
struct Coeffs {
  double b0{1.0}, b1{0.0}, b2{0.0}, a1{0.0}, a2{0.0};

  void design(BandType type, double hz, double gainDb, double q, double sampleRate) noexcept {
    constexpr double pi = 3.14159265358979323846;
    hz = std::clamp(hz, 1.0, 0.49 * sampleRate);
    q = std::max(q, 0.05);
    const double w = 2.0 * pi * hz / sampleRate;
    const double sinW = std::sin(w);
    const double cosW = std::cos(w);
    const double alpha = sinW / (2.0 * q);
    double a0 = 1.0;
    switch (type) {
      case BandType::Peak: {
        const double a = std::pow(10.0, gainDb / 40.0);
        a0 = 1.0 + alpha / a;
        b0 = 1.0 + alpha * a;
        b1 = -2.0 * cosW;
        b2 = 1.0 - alpha * a;
        a1 = -2.0 * cosW;
        a2 = 1.0 - alpha / a;
        break;
      }
      case BandType::LowShelf: {
        const double a = std::pow(10.0, gainDb / 40.0);
        const double sq = 2.0 * std::sqrt(a) * alpha;
        b0 = a * ((a + 1.0) - (a - 1.0) * cosW + sq);
        b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cosW);
        b2 = a * ((a + 1.0) - (a - 1.0) * cosW - sq);
        a0 = (a + 1.0) + (a - 1.0) * cosW + sq;
        a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cosW);
        a2 = (a + 1.0) + (a - 1.0) * cosW - sq;
        break;
      }
      case BandType::HighShelf: {
        const double a = std::pow(10.0, gainDb / 40.0);
        const double sq = 2.0 * std::sqrt(a) * alpha;
        b0 = a * ((a + 1.0) + (a - 1.0) * cosW + sq);
        b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cosW);
        b2 = a * ((a + 1.0) + (a - 1.0) * cosW - sq);
        a0 = (a + 1.0) - (a - 1.0) * cosW + sq;
        a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cosW);
        a2 = (a + 1.0) - (a - 1.0) * cosW - sq;
        break;
      }
      case BandType::HighPass2:
        b0 = (1.0 + cosW) / 2.0;
        b1 = -(1.0 + cosW);
        b2 = b0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cosW;
        a2 = 1.0 - alpha;
        break;
      case BandType::LowPass2:
        b0 = (1.0 - cosW) / 2.0;
        b1 = 1.0 - cosW;
        b2 = b0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cosW;
        a2 = 1.0 - alpha;
        break;
      case BandType::HighPass1: {
        const double k = std::tan(w / 2.0);
        b0 = 1.0;
        b1 = -1.0;
        b2 = 0.0;
        a0 = 1.0 + k;
        a1 = k - 1.0;
        a2 = 0.0;
        break;
      }
      case BandType::LowPass1: {
        const double k = std::tan(w / 2.0);
        b0 = k;
        b1 = k;
        b2 = 0.0;
        a0 = 1.0 + k;
        a1 = k - 1.0;
        a2 = 0.0;
        break;
      }
    }
    b0 /= a0;
    b1 /= a0;
    b2 /= a0;
    a1 /= a0;
    a2 /= a0;
  }
};

/** Transposed direct form II memory of one section on one channel. */
struct SectionState {
  double z1{0.0}, z2{0.0};
  void reset() noexcept { z1 = z2 = 0.0; }
  double process(const Coeffs& c, double in) noexcept {
    const double out = c.b0 * in + z1;
    z1 = c.b1 * in - c.a1 * out + z2;
    z2 = c.b2 * in - c.a2 * out;
    return out;
  }
};

template <int N>
class Bank {
public:
  static constexpr int kMaxChannels = 2;
  static constexpr int kControlGrid = 16;

  void prepare(double newSampleRate, double smoothSeconds = 0.02) noexcept {
    sampleRate = newSampleRate;
    for (auto& band : bands) {
      band.logHz.prepare(sampleRate, smoothSeconds);
      band.gain.prepare(sampleRate, smoothSeconds);
      band.q.prepare(sampleRate, smoothSeconds);
      band.amount.prepare(sampleRate, smoothSeconds);
    }
    level.prepare(sampleRate, smoothSeconds);
    reset();
  }

  /** Clear the filter memory and take the next targets at once. */
  void reset() noexcept {
    for (auto& band : bands) {
      for (auto& state : band.state)
        state.reset();
      band.logHz.snapOnNextTarget();
      band.gain.snapOnNextTarget();
      band.q.snapOnNextTarget();
      band.amount.snapOnNextTarget();
      band.designed = false;
    }
    level.snapOnNextTarget();
    gridLeft = 0;
  }

  void setTargets(const BandTarget* targets, float levelDb) noexcept {
    for (int b = 0; b < N; ++b) {
      Band& band = bands[b];
      if (band.type != targets[b].type) {
        band.type = targets[b].type;
        band.designed = false;
      }
      band.logHz.setTarget(std::log2(std::max(targets[b].hz, 1.0f)));
      band.gain.setTarget(targets[b].gainDb);
      band.q.setTarget(targets[b].q);
      band.amount.setTarget(targets[b].active ? 1.0f : 0.0f);
    }
    level.setTarget(fx::dbToGain(levelDb));
  }

  void process(float* const* channels, int numChannels, int numSamples) noexcept {
    numChannels = std::min(numChannels, kMaxChannels);
    float levels[kControlGrid];
    bool running[N];
    bool whole[N];
    double amounts[N];

    int done = 0;
    while (done < numSamples) {
      if (gridLeft == 0) {
        refresh(running, whole, amounts);
        gridLeft = kControlGrid;
      }
      const int chunk = std::min(numSamples - done, gridLeft);

      for (int i = 0; i < chunk; ++i)
        levels[i] = level.next();
      for (int ch = 0; ch < numChannels; ++ch) {
        float* x = channels[ch] + done;
        for (int i = 0; i < chunk; ++i) {
          double v = static_cast<double>(x[i]);
          for (int b = 0; b < N; ++b) {
            if (!running[b])
              continue;
            const double y = bands[b].state[ch].process(bands[b].coeffs, v);
            v = whole[b] ? y : v + amounts[b] * (y - v);
          }
          x[i] = static_cast<float>(v * static_cast<double>(levels[i]));
        }
      }
      // A channel that is not there still lets the controls glide on.
      for (auto& band : bands) {
        band.logHz.skip(chunk);
        band.gain.skip(chunk);
        band.q.skip(chunk);
        band.amount.skip(chunk);
      }

      gridLeft -= chunk;
      done += chunk;
    }
  }

  /** The longest ring of the bands in `targets`, plus the glide. */
  static double tailSeconds(const BandTarget* targets, double smoothSeconds = 0.02) noexcept {
    double longest = 0.0;
    for (int b = 0; b < N; ++b)
      longest = std::max(longest, ringSeconds(targets[b]));
    return longest > 0.0 ? longest + smoothSeconds : 0.0;
  }

private:
  struct Band {
    fx::SmoothedParam logHz, gain, q, amount;
    BandType type{BandType::Peak};
    Coeffs coeffs;
    SectionState state[kMaxChannels];
    float usedLogHz{0.0f}, usedGain{0.0f}, usedQ{0.0f};
    bool designed{false};
  };

  // Decide which bands run this chunk and bring their coefficients up to
  // date. Both channels share the coefficients, so a channel that appears
  // later (mono to stereo) is already right.
  void refresh(bool* running, bool* whole, double* amounts) noexcept {
    for (int b = 0; b < N; ++b) {
      Band& band = bands[b];
      const float amount = band.amount.current();
      const bool out = amount == 0.0f && band.amount.target() == 0.0f;
      const bool flat = hasGain(band.type) && band.gain.current() == 0.0f &&
                        band.gain.target() == 0.0f;
      running[b] = !(out || flat);
      whole[b] = amount == 1.0f;
      amounts[b] = static_cast<double>(amount);
      if (!running[b]) {
        // An identity section has no memory worth keeping.
        for (auto& state : band.state)
          state.reset();
        continue;
      }
      const float logHz = band.logHz.current();
      const float gain = band.gain.current();
      const float q = band.q.current();
      if (band.designed && logHz == band.usedLogHz && gain == band.usedGain && q == band.usedQ)
        continue;
      band.usedLogHz = logHz;
      band.usedGain = gain;
      band.usedQ = q;
      band.designed = true;
      band.coeffs.design(band.type, std::exp2(static_cast<double>(logHz)),
                         static_cast<double>(gain), static_cast<double>(q), sampleRate);
    }
  }

  Band bands[N];
  fx::SmoothedParam level;
  double sampleRate{48000.0};
  int gridLeft{0};
};

}  // namespace fx::eq

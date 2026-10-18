#pragma once

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// Band-limited sample-rate converter: a Kaiser-windowed sinc evaluated as a
// polyphase filter bank. It lets a NAM capture run at the rate it was
// trained at inside a chain that runs at another (see NamEngine), for the
// ratios the chain's power-of-two half-band stages cannot do (44.1 kHz
// models at 48 kHz, for one).
//
// The filter is designed against the LOWER of the two rates, whichever
// side that is, so the one design serves both directions:
//
//   raising the rate   the cutoff sits at the input Nyquist: the images of
//                      the input spectrum above it are removed;
//   lowering the rate  the cutoff sits at the output Nyquist: what the
//                      input holds above it is removed before it can fold.
//
// The figures, in terms of the lower rate F (pinned by
// test/src/nam_architecture_tests.cpp):
//
//   passband   0 to 0.4535 F (20 kHz at 44.1 kHz), flat within 0.0002 dB
//   stopband   from 0.5465 F, at least 99 dB down
//   delay      36 / F seconds each way (0.82 ms at 44.1 kHz), linear phase
//
// The transition band straddles F/2 symmetrically, so whatever folds when the
// rate is lowered lands above 0.4535 F, outside the band that is kept.
//
// The interface is the one AudioDSPTools' LanczosResampler has (push, pop,
// how many inputs a number of outputs needs, phase renormalisation), so the
// multi-port boundary's push/pop schedule (BasicChainBoundaryMulti) drives
// either. Like that class it tracks the two positions as exact rationals of
// the integer rates, so nothing drifts however long it runs.
//
// The rates are rounded to whole hertz. When their reduced ratio has a small
// denominator (every pair of standard rates) each output sample lands on one
// of the bank's phases exactly; otherwise the bank holds kMaxPhases phases and
// the coefficients are interpolated between the two nearest.
//
// Construction allocates (the history and the bank); everything after it is
// realtime-safe: no allocation, no locks.
template <int NumChannels>
class SincResampler {
public:
  // Half the kernel, in periods of the lower rate.
  static constexpr int kZeroCrossings = 36;
  static constexpr double kKaiserBeta = 10.0;
  // The history ring. One push can hold at most this minus the kernel.
  static constexpr int kBufferSize = 1 << 17;
  static constexpr int kMaxPhases = 4096;

  SincResampler(float inputRate, float outputRate) {
    const std::int64_t inRate = std::max<std::int64_t>(1, std::llround(inputRate));
    const std::int64_t outRate = std::max<std::int64_t>(1, std::llround(outputRate));
    const std::int64_t divisor = gcd(inRate, outRate);
    // Units of 1 / phaseDenominator input samples: one input advances the
    // write side by outStep, one output advances the read side by inStep.
    phaseDenominator = outRate / divisor;
    inStepPerInput = phaseDenominator;
    readStepPerOutput = inRate / divisor;

    // The kernel's bandwidth relative to the input rate: 1 when raising the
    // rate, out/in when lowering it.
    const double bandwidth =
        inRate <= outRate ? 1.0 : static_cast<double>(outRate) / static_cast<double>(inRate);
    const double halfLength = kZeroCrossings / bandwidth;  // in input samples
    halfTaps = static_cast<int>(std::ceil(halfLength));
    taps = 2 * halfTaps;
    exactPhases = phaseDenominator <= kMaxPhases;
    phases = exactPhases ? static_cast<int>(phaseDenominator) : kMaxPhases;

    // Row k of the bank is the output that sits k / phases of an input
    // sample after the newest of the taps' first half. Tap j reads input
    // base - halfTaps + 1 + j, at a distance of frac + halfTaps - 1 - j.
    bank.assign(static_cast<size_t>(phases + 1) * static_cast<size_t>(taps), 0.0f);
    const double i0Beta = besselI0(kKaiserBeta);
    for (int row = 0; row <= phases; ++row) {
      const double frac = static_cast<double>(row) / phases;
      float* coefficients = &bank[static_cast<size_t>(row) * static_cast<size_t>(taps)];
      double sum = 0.0;
      std::vector<double> weights(static_cast<size_t>(taps));
      for (int j = 0; j < taps; ++j) {
        const double x = frac + halfTaps - 1 - j;
        const double t = x / halfLength;
        double w = 0.0;
        if (std::abs(t) < 1.0) {
          const double arg = bandwidth * x;
          const double sinc =
              std::abs(arg) < 1e-12 ? 1.0 : std::sin(kPi * arg) / (kPi * arg);
          w = bandwidth * sinc * besselI0(kKaiserBeta * std::sqrt(1.0 - t * t)) / i0Beta;
        }
        weights[static_cast<size_t>(j)] = w;
        sum += w;
      }
      // Unity gain at DC on every phase, so a phase change never moves the
      // level.
      for (int j = 0; j < taps; ++j)
        coefficients[j] = static_cast<float>(weights[static_cast<size_t>(j)] / sum);
    }

    history.assign(static_cast<size_t>(NumChannels) * 2 * kBufferSize, 0.0f);
  }

  SincResampler(const SincResampler&) = delete;
  SincResampler& operator=(const SincResampler&) = delete;

  /** Half the kernel length, in input samples. */
  int halfLengthSamples() const { return halfTaps; }

  /** How many more input samples make `nOutputSamples` outputs available. */
  size_t GetNumSamplesRequiredFor(size_t nOutputSamples) const {
    if (nOutputSamples == 0)
      return 0;
    // Output n - 1 needs (in - out - (n - 1) * readStep) + r * inStep to be
    // more than halfTaps * denominator.
    const std::int64_t lead =
        phaseIn - phaseOut - static_cast<std::int64_t>(nOutputSamples - 1) * readStepPerOutput;
    const std::int64_t needed = static_cast<std::int64_t>(halfTaps) * phaseDenominator - lead;
    if (needed < 0)
      return 0;
    return static_cast<size_t>(needed / inStepPerInput + 1);
  }

  void PushBlock(float** inputs, size_t numFrames) {
    for (size_t s = 0; s < numFrames; ++s) {
      for (int c = 0; c < NumChannels; ++c) {
        float* channel = channelHistory(c);
        channel[writePos] = inputs[c][s];
        channel[writePos + kBufferSize] = inputs[c][s];
      }
      writePos = (writePos + 1) & (kBufferSize - 1);
      phaseIn += inStepPerInput;
    }
  }

  size_t PopBlock(float** outputs, size_t maxFrames) {
    size_t populated = 0;
    const std::int64_t threshold = static_cast<std::int64_t>(halfTaps) * phaseDenominator;
    while (populated < maxFrames && phaseIn - phaseOut > threshold) {
      readSample(phaseIn - phaseOut, outputs, populated);
      phaseOut += readStepPerOutput;
      ++populated;
    }
    return populated;
  }

  void RenormalizePhases() {
    phaseIn -= phaseOut;
    phaseOut = 0;
  }

  void ClearBuffer() { std::fill(history.begin(), history.end(), 0.0f); }

private:
  static constexpr double kPi = 3.14159265358979323846;

  static std::int64_t gcd(std::int64_t a, std::int64_t b) {
    while (b != 0) {
      const std::int64_t t = b;
      b = a % b;
      a = t;
    }
    return a;
  }

  // The modified Bessel function of the first kind, order 0 (power series).
  static double besselI0(double x) {
    double sum = 1.0;
    double term = 1.0;
    const double quarterSquare = 0.25 * x * x;
    for (int k = 1; k < 200; ++k) {
      term *= quarterSquare / (static_cast<double>(k) * static_cast<double>(k));
      sum += term;
      if (term < sum * 1e-17)
        break;
    }
    return sum;
  }

  float* channelHistory(int c) { return &history[static_cast<size_t>(c) * 2 * kBufferSize]; }
  const float* channelHistory(int c) const {
    return &history[static_cast<size_t>(c) * 2 * kBufferSize];
  }

  // One output at `lead` (in 1 / denominator input samples) behind the write
  // position.
  void readSample(std::int64_t lead, float** outputs, size_t s) const {
    const std::int64_t whole = lead / phaseDenominator;
    const std::int64_t rest = lead % phaseDenominator;
    // The output sits at writePos - whole - rest / denominator: the newest
    // input at or before it, and how far past that input it is.
    const std::int64_t fractionNumerator = rest == 0 ? 0 : phaseDenominator - rest;
    const std::int64_t base = static_cast<std::int64_t>(writePos) - whole - (rest == 0 ? 0 : 1);
    const int start = static_cast<int>((base - halfTaps + 1) & (kBufferSize - 1));

    const float* row0 = nullptr;
    const float* row1 = nullptr;
    float blend = 0.0f;
    if (exactPhases) {
      row0 = &bank[static_cast<size_t>(fractionNumerator) * static_cast<size_t>(taps)];
    } else {
      const double position =
          static_cast<double>(fractionNumerator) * phases / static_cast<double>(phaseDenominator);
      const int row = std::min(phases - 1, static_cast<int>(position));
      blend = static_cast<float>(position - row);
      row0 = &bank[static_cast<size_t>(row) * static_cast<size_t>(taps)];
      row1 = row0 + taps;
    }

    for (int c = 0; c < NumChannels; ++c) {
      const float* x = channelHistory(c) + start;
      // Four running sums: independent chains the compiler can overlap.
      float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
      if (row1 == nullptr) {
        int j = 0;
        for (; j + 3 < taps; j += 4) {
          a0 += row0[j] * x[j];
          a1 += row0[j + 1] * x[j + 1];
          a2 += row0[j + 2] * x[j + 2];
          a3 += row0[j + 3] * x[j + 3];
        }
        for (; j < taps; ++j)
          a0 += row0[j] * x[j];
      } else {
        for (int j = 0; j < taps; ++j)
          a0 += (row0[j] + blend * (row1[j] - row0[j])) * x[j];
      }
      outputs[c][s] = (a0 + a1) + (a2 + a3);
    }
  }

  std::vector<float> bank;
  std::vector<float> history;
  int writePos = 0;
  int halfTaps = 1;
  int taps = 2;
  int phases = 1;
  bool exactPhases = true;
  std::int64_t phaseIn = 0;
  std::int64_t phaseOut = 0;
  std::int64_t inStepPerInput = 1;
  std::int64_t readStepPerOutput = 1;
  std::int64_t phaseDenominator = 1;
};

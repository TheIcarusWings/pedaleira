#pragma once
// Shared by the delay family's tests (digital, analog, tape, ping-pong): the
// measurements and the whole-behaviour checks they all make, on top of
// effects/effect_test_helpers.h. Not a *_tests.cpp file, so the test build
// does not compile it on its own.
#include "effects/effect_test_helpers.h"

#include <gtest/gtest.h>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <vector>

namespace delaytest {

using namespace fxtest;

constexpr double kRate = 48000.0;

inline int ms(double milliseconds) { return static_cast<int>(std::lround(milliseconds * 0.001 * kRate)); }

/** `base` followed by `extra`: a later value for the same parameter wins,
    because a rig sets them in order. */
inline Params withParams(Params base, const Params& extra) {
  base.insert(base.end(), extra.begin(), extra.end());
  return base;
}

/** A sine burst under a Hann window, `seconds` long, then silence to `totalSeconds`. */
inline std::vector<float> toneBurst(double seconds, double hz, float amplitude, double totalSeconds) {
  std::vector<float> x(static_cast<size_t>(std::lround(totalSeconds * kRate)), 0.0f);
  const size_t n = static_cast<size_t>(std::lround(seconds * kRate));
  for (size_t i = 0; i < n && i < x.size(); ++i) {
    const double window = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n));
    x[i] = amplitude * static_cast<float>(window * std::sin(2.0 * kPi * hz * static_cast<double>(i) / kRate));
  }
  return x;
}

/** White noise under a Hann window, then silence to `totalSeconds`. */
inline std::vector<float> noiseBurst(double seconds, unsigned seed, float amplitude, double totalSeconds) {
  std::vector<float> x(static_cast<size_t>(std::lround(totalSeconds * kRate)), 0.0f);
  const std::vector<float> source = noise(seconds, seed, amplitude, kRate);
  for (size_t i = 0; i < source.size() && i < x.size(); ++i) {
    const double window =
        0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(source.size()));
    x[i] = source[i] * static_cast<float>(window);
  }
  return x;
}

/** RMS over [fromMs, fromMs + lengthMs). */
inline double rmsWindow(const std::vector<float>& x, double fromMs, double lengthMs) {
  const size_t begin = static_cast<size_t>(std::max(0, ms(fromMs)));
  return rmsOf(x, begin, begin + static_cast<size_t>(ms(lengthMs)));
}

/** Magnitude spectrum of 4096 samples from `begin` under a Hann window:
    bins 0 to 2048, each 11.7 Hz at 48 kHz. */
inline std::vector<float> magnitudeSpectrum(const std::vector<float>& x, size_t begin) {
  constexpr int kOrder = 12;
  constexpr size_t kSize = size_t{1} << kOrder;
  juce::dsp::FFT fft(kOrder);
  std::vector<float> data(2 * kSize, 0.0f);
  for (size_t i = 0; i < kSize && begin + i < x.size(); ++i) {
    const double window = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(kSize));
    data[i] = x[begin + i] * static_cast<float>(window);
  }
  fft.performFrequencyOnlyForwardTransform(data.data());
  data.resize(kSize / 2 + 1);
  return data;
}

/** Power between two frequencies in the 4096 samples from `begin`. */
inline double bandPower(const std::vector<float>& x, size_t begin, double loHz, double hiHz) {
  const std::vector<float> spectrum = magnitudeSpectrum(x, begin);
  const double binHz = kRate / 4096.0;
  double power = 0.0;
  for (size_t k = 0; k < spectrum.size(); ++k) {
    const double f = static_cast<double>(k) * binHz;
    if (f >= loHz && f < hiHz)
      power += static_cast<double>(spectrum[k]) * static_cast<double>(spectrum[k]);
  }
  return power;
}

/** Power-weighted mean frequency of the 4096 samples from `begin`. */
inline double spectralCentroidHz(const std::vector<float>& x, size_t begin) {
  const std::vector<float> spectrum = magnitudeSpectrum(x, begin);
  const double binHz = kRate / 4096.0;
  double weighted = 0.0, total = 0.0;
  for (size_t k = 1; k < spectrum.size(); ++k) {
    const double power = static_cast<double>(spectrum[k]) * static_cast<double>(spectrum[k]);
    weighted += power * static_cast<double>(k) * binHz;
    total += power;
  }
  return total > 0.0 ? weighted / total : 0.0;
}

struct PitchModulation {
  double carrierHz{0.0};
  // The strongest modulation rate in the searched range, and the peak
  // deviation there as a fraction of the carrier (0.01 = 1%).
  double rateHz{0.0};
  double deviation{0.0};
  // The widest the local frequency strays from the carrier, as a fraction.
  double worstDeviation{0.0};
  // Lowest and highest local frequency over the analysed part, in Hz.
  double lowestHz{0.0};
  double highestHz{0.0};
};

/** The pitch modulation of a rendered sine: the local frequency from the
    spacing of upward zero crossings, then a scan of that series for its
    strongest component between `minRateHz` and `maxRateHz` (an exact
    nonuniform DFT, so a drifting or random component does not fool a
    crossing counter). `skip` leaves out the start. */
inline PitchModulation analysePitchModulation(const std::vector<float>& out, size_t skip,
                                              double minRateHz, double maxRateHz,
                                              size_t end = std::numeric_limits<size_t>::max()) {
  std::vector<double> crossing;
  for (size_t i = std::max<size_t>(skip, 1); i < std::min(end, out.size()); ++i)
    if (out[i - 1] < 0.0f && out[i] >= 0.0f) {
      const double frac = static_cast<double>(-out[i - 1]) /
                          (static_cast<double>(out[i]) - static_cast<double>(out[i - 1]));
      crossing.push_back((static_cast<double>(i - 1) + frac) / kRate);
    }
  PitchModulation result;
  if (crossing.size() < 16)
    return result;
  std::vector<double> time, frequency;
  for (size_t i = 1; i < crossing.size(); ++i) {
    time.push_back(0.5 * (crossing[i] + crossing[i - 1]));
    frequency.push_back(1.0 / (crossing[i] - crossing[i - 1]));
  }
  result.carrierHz = static_cast<double>(crossing.size() - 1) / (crossing.back() - crossing.front());
  result.lowestHz = *std::min_element(frequency.begin(), frequency.end());
  result.highestHz = *std::max_element(frequency.begin(), frequency.end());
  std::vector<double> deviation(frequency.size());
  double mean = 0.0;
  for (size_t i = 0; i < frequency.size(); ++i) {
    deviation[i] = frequency[i] / result.carrierHz - 1.0;
    mean += deviation[i];
  }
  mean /= static_cast<double>(deviation.size());
  for (double& d : deviation) {
    d -= mean;
    result.worstDeviation = std::max(result.worstDeviation, std::abs(d));
  }
  double best = 0.0;
  for (double rate = minRateHz; rate <= maxRateHz + 1e-9; rate += 0.05) {
    std::complex<double> sum{0.0, 0.0};
    for (size_t i = 0; i < deviation.size(); ++i) {
      const double phase = -2.0 * kPi * rate * time[i];
      sum += deviation[i] * std::complex<double>(std::cos(phase), std::sin(phase));
    }
    const double magnitude = 2.0 * std::abs(sum) / static_cast<double>(deviation.size());
    if (magnitude > best) {
      best = magnitude;
      result.rateHz = rate;
    }
  }
  result.deviation = best;
  return result;
}

// ---------------------------------------------------------------------------
// Whole-behaviour checks
// ---------------------------------------------------------------------------

/** Where `kind` places an echo of noise: the cross-correlation lag of the wet
    path (mix 1) against the input, in samples, searched around `expected`. */
inline int measuredDelaySamples(const char* kind, const Params& params, int expected,
                                const RigOptions& options = {}) {
  EffectRig rig(kind, withParams(params, {{"mix", 1.0}}), options);
  const std::vector<float> in = withSilence(noise(0.25, 3u, 0.25f, kRate), expected + ms(100.0));
  return estimateDelaySamples(rig.render(in), in, std::max(1, expected - ms(60.0)),
                              expected + ms(60.0));
}

/** The loop's output stays inside `bound` and clean for 20 s of hot input
    (full-scale noise, then a near full-scale sine) at 95% feedback. */
inline void expectBoundedAtMaximumFeedback(const char* kind, const Params& params, float bound) {
  EffectRig rig(kind, withParams(params, {{"feedback", 95.0}, {"mix", 1.0}}));
  const std::vector<float> loud = noise(10.0, 41u, 1.0f, kRate);
  const std::vector<float> tone = sine(10.0, 110.0, 0.95f, kRate);
  std::vector<float> in = loud;
  in.insert(in.end(), tone.begin(), tone.end());
  const std::vector<float> out = rig.render(in);
  expectCleanSignal(out, bound, "20 s of hot input at 95% feedback");
  EXPECT_GT(peakOf(out), 0.1f);
}

/** After a change of time on a running rig the output never steps by more
    than the steady signal does (`stepFactor` times its largest step, plus a
    little). A 220 Hz sine through the delay. */
inline float steadyStepOf(const std::vector<float>& out) {
  return maxStep(out, out.size() / 2, out.size());
}

// A burst, the block switched off right after it, then silence.
struct BypassRender {
  std::vector<float> added;  // what the block adds to the dry signal
  size_t bypassAt{0};
  double tailSeconds{0.0};
  float idleResidue{0.0f};  // what is still added once the block is idle
};

inline BypassRender renderBypass(const char* kind, const Params& params, bool spillover,
                                 bool lateBurst) {
  EffectRig rig(kind, withParams(params, {{"spillover", spillover ? 1.0 : 0.0}, {"mix", 0.5}}));
  EffectRig reference{juce::String()};
  BypassRender result;
  result.tailSeconds = rig.tailSeconds();

  const std::vector<float> burst = noise(0.3, 5u, 0.25f, kRate);
  // Long enough for the tail, the fades and the DC blocker's memory to pass,
  // and for a second burst a second after the switch.
  std::vector<float> rest(static_cast<size_t>((result.tailSeconds + 2.0) * kRate), 0.0f);
  if (lateBurst) {
    const std::vector<float> late = noise(0.1, 6u, 0.25f, kRate);
    std::copy(late.begin(), late.end(), rest.begin() + ms(1000.0));
  }

  // Whole blocks, so the switch lands exactly between two of them.
  const size_t first = (burst.size() / 256) * 256;
  std::vector<float> in(burst.begin(), burst.begin() + static_cast<std::ptrdiff_t>(first));
  result.bypassAt = in.size();

  std::vector<float> out = rig.render(in);
  std::vector<float> dry = reference.render(in);
  rig.setEnabled(false);
  const std::vector<float> outRest = rig.render(rest);
  const std::vector<float> dryRest = reference.render(rest);
  out.insert(out.end(), outRest.begin(), outRest.end());
  dry.insert(dry.end(), dryRest.begin(), dryRest.end());

  result.added.resize(out.size());
  for (size_t i = 0; i < out.size(); ++i)
    result.added[i] = out[i] - dry[i];
  // Idle once the tail and the fades have passed, and the rig's DC blocker
  // has forgotten the tail as well (half a second later).
  const size_t idleFrom =
      result.bypassAt + static_cast<size_t>((result.tailSeconds + 0.1 + 0.5) * kRate);
  EXPECT_LT(idleFrom, out.size());
  result.idleResidue = peakOf(result.added, idleFrom);
  return result;
}

/** The spillover contract of a delay: with the switch on, the repeats keep
    coming after a bypass (here between `ringFromMs` and `ringToMs` after it),
    fall below -60 dB by the reported tail and then add nothing; audio played
    after the bypass never reaches the loop; with the switch off the tail is
    cut after the 25 ms bypass fade. */
inline void expectSpilloverBehaviour(const char* kind, const Params& params, double ringFromMs,
                                     double ringToMs) {
  const BypassRender on = renderBypass(kind, params, true, false);
  const BypassRender onLate = renderBypass(kind, params, true, true);
  const BypassRender off = renderBypass(kind, params, false, false);
  const float beforeBypass = peakOf(on.added, 0, on.bypassAt);
  EXPECT_GT(beforeBypass, 0.03f);

  // Rings after the switch...
  const float ringing = peakOf(on.added, on.bypassAt + static_cast<size_t>(ms(ringFromMs)),
                               on.bypassAt + static_cast<size_t>(ms(ringToMs)));
  EXPECT_GT(ringing, 0.01f) << "the tail was cut";
  EXPECT_LT(ringing, beforeBypass * 1.2f) << "the tail is louder than the playing block was";
  // ...is down to -60 dB by the reported tail (against the loudest it was),
  const double decay = decayTimeSeconds(on.added, on.bypassAt, beforeBypass, kRate);
  EXPECT_LE(decay, on.tailSeconds + 0.1) << "the tail outlived the reported tail";
  // ...and then adds nothing.
  EXPECT_LT(on.idleResidue, 1.0e-6f);
  expectCleanSignal(on.added, 1.5f, "spillover render");

  // The input is closed after the switch: a burst a second later changes
  // nothing the block adds.
  float worst = 0.0f;
  for (size_t i = 0; i < on.added.size(); ++i)
    worst = std::max(worst, std::abs(on.added[i] - onLate.added[i]));
  EXPECT_LT(worst, 1.0e-5f) << "audio played after the bypass reached the loop";

  // Switch off: the 25 ms fade, then nothing but the DC blocker settling.
  EXPECT_GT(peakOf(off.added, 0, off.bypassAt), 0.03f);
  EXPECT_LT(peakOf(off.added, off.bypassAt + static_cast<size_t>(ms(30.0))), 5.0e-4f);
  EXPECT_LT(peakOf(off.added, off.bypassAt + static_cast<size_t>(ms(400.0))), 1.0e-6f);
  EXPECT_LT(off.idleResidue, 1.0e-6f);
}

}  // namespace delaytest

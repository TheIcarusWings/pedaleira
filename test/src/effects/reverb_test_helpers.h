#pragma once
// Measurements the five reverb test files share, on top of the effect
// harness (effect_test_helpers.h). Nobody can listen to a reverb in a test
// run, so each of these turns a property a listener would judge (does it
// decay as long as the knob says, do the highs die first, is the tail dense,
// does it ring at one pitch, is it wide, is it stable) into a number taken
// from the wet signal of the kind alone (DirectKind: no mix, no DC blocker).
#include "effects/dsp/Filters.h"
#include "effects/effect_test_helpers.h"

#include <gtest/gtest.h>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <iostream>
#include <vector>

namespace reverbtest {

using fxtest::DirectKind;
using fxtest::EffectRig;
using fxtest::Layout;
using fxtest::Params;
using fxtest::Stereo;

constexpr double kRate = 48000.0;

inline int samples(double seconds) { return static_cast<int>(std::lround(seconds * kRate)); }

/** `base` with the values of `extra` added or replacing the same ids. */
inline Params with(Params base, const Params& extra) {
  for (const auto& [id, value] : extra) {
    bool found = false;
    for (auto& entry : base)
      if (entry.first == id) {
        entry.second = value;
        found = true;
      }
    if (!found)
      base.emplace_back(id, value);
  }
  return base;
}

/** The wet impulse response of the kind alone. In a stereo layout both
    inputs get the impulse (what a one-block rig feeds a reverb). */
inline Stereo wetImpulseResponse(const char* kind, const Params& params, double seconds,
                                 Layout layout = Layout::Mono) {
  DirectKind direct(kind, params, layout);
  const std::vector<float> in = fxtest::impulse(samples(seconds));
  return direct.render(Stereo{in, in});
}

/** `x` through a fourth-order Butterworth high-pass at `lowHz` and low-pass
    at `highHz`: one band of it. */
inline std::vector<float> bandOf(const std::vector<float>& x, double lowHz, double highHz) {
  fx::Biquad sections[4];
  sections[0].setCoeffs(fx::BiquadCoeffs::highpass(lowHz, 0.5412, kRate));
  sections[1].setCoeffs(fx::BiquadCoeffs::highpass(lowHz, 1.3066, kRate));
  sections[2].setCoeffs(fx::BiquadCoeffs::lowpass(highHz, 0.5412, kRate));
  sections[3].setCoeffs(fx::BiquadCoeffs::lowpass(highHz, 1.3066, kRate));
  std::vector<float> y(x.size());
  for (size_t i = 0; i < x.size(); ++i) {
    float v = x[i];
    for (auto& section : sections)
      v = section.process(v);
    y[i] = v;
  }
  return y;
}

/** RT60 of one band of an impulse response (Schroeder integration). */
inline double bandRt60(const std::vector<float>& ir, double lowHz, double highHz) {
  return fxtest::estimateRt60Seconds(bandOf(ir, lowHz, highHz), kRate);
}

inline double energyOf(const std::vector<float>& x, size_t begin = 0,
                       size_t end = std::numeric_limits<size_t>::max()) {
  double sum = 0.0;
  for (size_t i = begin; i < std::min(end, x.size()); ++i)
    sum += static_cast<double>(x[i]) * x[i];
  return sum;
}

/** Normalised kurtosis (fourth moment over the squared second) of a stretch
    of an impulse response. 3 for Gaussian noise, which is what a fully dense
    reverb tail is; far higher while the response is still a few separate
    echoes with silence between them. Abel and Huang's echo density measure
    rests on the same idea. */
inline double kurtosisOf(const std::vector<float>& x, size_t begin, size_t end) {
  end = std::min(end, x.size());
  double second = 0.0, fourth = 0.0;
  for (size_t i = begin; i < end; ++i) {
    const double v = static_cast<double>(x[i]) * x[i];
    second += v;
    fourth += v * v;
  }
  const double n = static_cast<double>(end - begin);
  return second > 0.0 ? (fourth / n) / ((second / n) * (second / n)) : 0.0;
}

/** Correlation coefficient of two signals over a stretch: 1 for the same
    signal on both sides (mono), 0 for unrelated ones (wide). */
inline double correlationOf(const std::vector<float>& a, const std::vector<float>& b, size_t begin,
                            size_t end) {
  end = std::min({end, a.size(), b.size()});
  double ab = 0.0, aa = 0.0, bb = 0.0;
  for (size_t i = begin; i < end; ++i) {
    ab += static_cast<double>(a[i]) * b[i];
    aa += static_cast<double>(a[i]) * a[i];
    bb += static_cast<double>(b[i]) * b[i];
  }
  return aa > 0.0 && bb > 0.0 ? ab / std::sqrt(aa * bb) : 0.0;
}

struct Periodicity {
  double peak{0.0};  // largest |autocorrelation| in the lag range, 0 to 1
  double lagMs{0.0};
};

/** The largest normalised autocorrelation of a stretch of a reverb tail at
    lags from `minLagMs` to `maxLagMs`. A tail that repeats (a flutter echo,
    a comb, one delay line feeding itself) has a peak near 1 at its period;
    noise has none. The stretch is first flattened with the inverse of its
    own exponential decay (`rt60`), so a late repeat is not hidden by having
    faded. */
inline Periodicity tailPeriodicity(const std::vector<float>& ir, double fromSeconds,
                                   double toSeconds, double rt60, double minLagMs,
                                   double maxLagMs) {
  const size_t begin = static_cast<size_t>(samples(fromSeconds));
  const size_t end = std::min(ir.size(), static_cast<size_t>(samples(toSeconds)));
  const size_t n = end - begin;
  const int order = static_cast<int>(std::ceil(std::log2(static_cast<double>(2 * n))));
  const size_t size = size_t{1} << order;
  juce::dsp::FFT fft(order);
  using Complex = juce::dsp::Complex<float>;
  std::vector<Complex> time(size), spectrum(size), correlation(size);
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / kRate;
    time[i] = Complex(static_cast<float>(ir[begin + i] * std::exp(6.907755278982137 * t / rt60)), 0.0f);
  }
  fft.perform(time.data(), spectrum.data(), false);
  for (auto& bin : spectrum)
    bin = Complex(std::norm(bin), 0.0f);
  fft.perform(spectrum.data(), correlation.data(), true);

  Periodicity result;
  const double zero = correlation[0].real();
  if (zero <= 0.0)
    return result;
  const size_t minLag = static_cast<size_t>(samples(minLagMs * 0.001));
  const size_t maxLag = std::min(n / 2, static_cast<size_t>(samples(maxLagMs * 0.001)));
  for (size_t lag = minLag; lag <= maxLag; ++lag) {
    // Unbiased: a lag of k has n - k products in it.
    const double value = std::abs(correlation[lag].real()) / zero * static_cast<double>(n) /
                         static_cast<double>(n - lag);
    if (value > result.peak) {
      result.peak = value;
      result.lagMs = 1000.0 * static_cast<double>(lag) / kRate;
    }
  }
  return result;
}

/** RMS of `x` in a band around `centreHz` (two cascaded band-passes of
    quality `q`), over [begin, end). */
inline double bandRms(const std::vector<float>& x, double centreHz, double q, size_t begin,
                      size_t end) {
  fx::Biquad first, second;
  first.setCoeffs(fx::BiquadCoeffs::bandpass(centreHz, q, kRate));
  second.setCoeffs(fx::BiquadCoeffs::bandpass(centreHz, q, kRate));
  std::vector<float> y(x.size());
  for (size_t i = 0; i < x.size(); ++i)
    y[i] = second.process(first.process(x[i]));
  return fxtest::rmsOf(y, begin, end);
}

// ---------------------------------------------------------------------------
// Checks every reverb runs
// ---------------------------------------------------------------------------

/** The decay parameter is the RT60: measured on the broadband impulse
    response by Schroeder integration, within `tolerance` (a fraction) of the
    setting, at each of `decays`. `params` should switch the damping off (or
    the kind should have none), so the broadband figure is the one set. */
inline void expectRt60MatchesDecay(const char* kind, const Params& params,
                                   const std::vector<double>& decays, double tolerance = 0.2) {
  for (const double decay : decays) {
    const std::vector<float> ir =
        wetImpulseResponse(kind, with(params, {{"decayS", decay}}), decay * 1.1 + 1.0).left;
    const double measured = fxtest::estimateRt60Seconds(ir, kRate);
    std::cout << "[   RT60   ] " << kind << ": decay " << decay << " s, measured "
              << juce::String(measured, 3) << " s (" << juce::String(100.0 * (measured / decay - 1.0), 1)
              << "%)" << std::endl;
    EXPECT_NEAR(measured, decay, tolerance * decay) << kind << " at decay " << decay;
  }
}

/** The response starts `preDelayMs` later than with the pre-delay at 0, to
    the sample. */
inline void expectPreDelayMatches(const char* kind, const Params& params,
                                  const std::vector<double>& preDelaysMs) {
  const int origin =
      fxtest::onsetIndex(wetImpulseResponse(kind, with(params, {{"preDelayMs", 0.0}}), 0.5).left, 0.001f);
  ASSERT_GE(origin, 0);
  for (const double ms : preDelaysMs) {
    const int onset = fxtest::onsetIndex(
        wetImpulseResponse(kind, with(params, {{"preDelayMs", ms}}), 0.8).left, 0.001f);
    EXPECT_NEAR(onset - origin, samples(ms * 0.001), 1) << kind << " at a pre-delay of " << ms << " ms";
  }
}

struct Density {
  double early{0.0}, middle{0.0}, late{0.0};
};

/** Kurtosis of the impulse response in three windows counted from its
    onset (seconds). Prints them. */
inline Density echoDensity(const char* kind, const Params& params, double earlyTo, double middleFrom,
                           double middleTo, double lateFrom, double lateTo) {
  const std::vector<float> ir = wetImpulseResponse(kind, params, lateTo + 0.2).left;
  const size_t onset = static_cast<size_t>(std::max(0, fxtest::onsetIndex(ir, 0.001f)));
  const auto at = [onset](double seconds) { return onset + static_cast<size_t>(samples(seconds)); };
  Density d;
  d.early = kurtosisOf(ir, onset, at(earlyTo));
  d.middle = kurtosisOf(ir, at(middleFrom), at(middleTo));
  d.late = kurtosisOf(ir, at(lateFrom), at(lateTo));
  std::cout << "[ DENSITY  ] " << kind << ": kurtosis " << juce::String(d.early, 1) << " in the first "
            << juce::String(earlyTo * 1000.0, 0) << " ms, " << juce::String(d.middle, 2) << " at "
            << juce::String(middleFrom * 1000.0, 0) << " ms, " << juce::String(d.late, 2) << " at "
            << juce::String(lateFrom * 1000.0, 0) << " ms (Gaussian: 3)" << std::endl;
  return d;
}

struct Width {
  double correlation{0.0};    // of left and right over the late tail
  double monoSumDb{0.0};      // energy of (L + R) against energy of L plus energy of R
  double leftAgainstMonoDb{0.0};  // level of the stereo left against the mono reverb
};

/** Left/right correlation of the late tail on a true stereo buffer, what
    summing the two sides to mono costs, and the level of one side against
    the mono version. Prints them. */
inline Width stereoWidth(const char* kind, const Params& params, double fromSeconds,
                         double toSeconds) {
  const Stereo ir = wetImpulseResponse(kind, params, toSeconds + 0.1, Layout::Stereo);
  const std::vector<float> mono = wetImpulseResponse(kind, params, toSeconds + 0.1, Layout::Mono).left;
  const size_t begin = static_cast<size_t>(samples(fromSeconds));
  const size_t end = static_cast<size_t>(samples(toSeconds));
  std::vector<float> sum(ir.left.size());
  for (size_t i = 0; i < sum.size(); ++i)
    sum[i] = ir.left[i] + ir.right[i];
  Width w;
  w.correlation = correlationOf(ir.left, ir.right, begin, end);
  w.monoSumDb = 10.0 * std::log10(energyOf(sum, begin, end) /
                                  (energyOf(ir.left, begin, end) + energyOf(ir.right, begin, end)));
  w.leftAgainstMonoDb = 10.0 * std::log10(energyOf(ir.left) / energyOf(mono));
  std::cout << "[  WIDTH   ] " << kind << ": left/right correlation " << juce::String(w.correlation, 3)
            << ", mono sum " << juce::String(w.monoSumDb, 2) << " dB, stereo left against mono "
            << juce::String(w.leftAgainstMonoDb, 2) << " dB" << std::endl;
  return w;
}

/** What every reverb must do with its channels: wide on a true stereo
    buffer (late-tail correlation under 0.3) without the mono sum losing
    more than 1.5 dB, the same level on one side as the mono version (1.5
    dB), and in dual mono two identical channels for identical inputs (no
    stereo treatment there). */
inline void expectWideInStereoOnly(const char* kind, const Params& params, double fromSeconds,
                                   double toSeconds) {
  const Width w = stereoWidth(kind, params, fromSeconds, toSeconds);
  EXPECT_LT(std::abs(w.correlation), 0.3) << kind << ": left and right of the late tail are alike";
  EXPECT_GT(w.monoSumDb, -1.5) << kind << ": the sides cancel when summed to mono";
  EXPECT_NEAR(w.leftAgainstMonoDb, 0.0, 1.5) << kind << ": stereo and mono differ in level";

  const Stereo dual = wetImpulseResponse(kind, params, 0.5, Layout::DualMono);
  EXPECT_EQ(dual.left, dual.right) << kind << ": dual mono treats its two channels differently";
  const std::vector<float> mono = wetImpulseResponse(kind, params, 0.5, Layout::Mono).left;
  EXPECT_EQ(dual.left, mono) << kind << ": a dual mono channel is not the mono reverb";
}

/** Thirty seconds of full-scale noise into the kind at its most reckless
    setting, on a true stereo buffer: finite, free of denormals and under
    `bound` on both sides. Prints the peak. */
inline void expectBoundedWithHotInput(const char* kind, const Params& params, float bound) {
  DirectKind direct(kind, params, Layout::Stereo);
  const Stereo in{fxtest::noise(30.0, 31u, 1.0f, kRate), fxtest::noise(30.0, 32u, 1.0f, kRate)};
  const Stereo out = direct.render(in);
  std::cout << "[   HOT    ] " << kind << ": full-scale noise for 30 s, wet peak "
            << juce::String(std::max(fxtest::peakOf(out.left), fxtest::peakOf(out.right)), 2)
            << ", RMS over the last 5 s "
            << juce::String(fxtest::rmsOf(out.left, static_cast<size_t>(samples(25.0))), 3) << std::endl;
  fxtest::expectCleanSignal(out.left, bound, "hot input, left");
  fxtest::expectCleanSignal(out.right, bound, "hot input, right");
  // And it still decays afterwards: nothing is stuck ringing.
  const Stereo after = direct.render(Stereo{std::vector<float>(static_cast<size_t>(samples(1.0)), 0.0f),
                                            std::vector<float>(static_cast<size_t>(samples(1.0)), 0.0f)});
  fxtest::expectCleanSignal(after.left, bound, "after the hot input");
  EXPECT_LT(fxtest::rmsOf(after.left, static_cast<size_t>(samples(0.9))),
            fxtest::rmsOf(out.left, static_cast<size_t>(samples(29.0))));
}

struct Sweep {
  juce::String param;
  double from;
  double to;
};

struct ZipperResult {
  // Largest sample-to-sample step and peak level, before and during the
  // sweep.
  float steadyStep{0.0f}, steadyPeak{0.0f};
  float sweepStep{0.0f}, sweepPeak{0.0f};
  // The step as a share of the peak: how steep the waveform gets for its
  // size. A 220 Hz sine has 0.029; a click has a step with no level to
  // explain it.
  // Energy above 6 kHz against the whole, in dB. The input is a 220 Hz sine:
  // a control applied in steps (per block, or per control tick without a
  // ramp) is a train of small clicks, and this is where they show.
  double steadyHighDb{0.0}, sweepHighDb{0.0};
  float steadySteepness() const { return steadyStep / std::max(1.0e-9f, steadyPeak); }
  float sweepSteepness() const { return sweepStep / std::max(1.0e-9f, sweepPeak); }
};

/** A steady 220 Hz sine through the rig (wet only) while the given
    parameters are moved a little further on every block for two seconds,
    there and back. A parameter applied in steps shows as jumps in the
    waveform; a smoothed one never makes the output steeper than its level
    and its content explain. Returns the largest sample-to-sample step and
    the peak before and during the sweep, and prints them. */
inline ZipperResult sweepUnderAudio(const char* kind, const Params& params,
                                    const std::vector<Sweep>& sweeps) {
  Params settings = with(params, {{"mix", 1.0}});
  for (const Sweep& sweep : sweeps)
    settings = with(settings, {{sweep.param, sweep.from}});
  EffectRig rig(kind, settings);
  const int block = rig.blockSize();
  const std::vector<float> tone = fxtest::sine(6.0, 220.0, 0.25f, kRate);
  const auto slice = [&tone](int from, int count) {
    return std::vector<float>(tone.begin() + from, tone.begin() + from + count);
  };

  const int settle = samples(3.0) / block * block;
  std::vector<float> out = rig.render(slice(0, settle));
  ZipperResult result;
  result.steadyStep = fxtest::maxStep(out, static_cast<size_t>(settle - samples(1.0)), out.size());
  result.steadyPeak = fxtest::peakOf(out, static_cast<size_t>(settle - samples(1.0)));

  const int blocks = samples(2.0) / block;
  for (int b = 0; b < blocks; ++b) {
    // A triangle: to the far end and back.
    const double phase = static_cast<double>(b) / (blocks - 1);
    const double position = phase < 0.5 ? 2.0 * phase : 2.0 - 2.0 * phase;
    for (const Sweep& sweep : sweeps)
      rig.set(sweep.param, sweep.from + position * (sweep.to - sweep.from));
    const std::vector<float> rendered = rig.render(slice(settle + b * block, block));
    out.insert(out.end(), rendered.begin(), rendered.end());
  }
  result.sweepStep = fxtest::maxStep(out, static_cast<size_t>(settle), out.size());
  result.sweepPeak = fxtest::peakOf(out, static_cast<size_t>(settle));
  const auto highDb = [&out](size_t from, size_t to) {
    std::vector<float> part(out.begin() + static_cast<std::ptrdiff_t>(from),
                            out.begin() + static_cast<std::ptrdiff_t>(to));
    // Faded in and out over 50 ms: the cut ends of the stretch would be the
    // loudest thing above 6 kHz otherwise.
    const size_t taper = static_cast<size_t>(samples(0.05));
    for (size_t i = 0; i < taper; ++i) {
      const float w = 0.5f - 0.5f * std::cos(3.14159265f * static_cast<float>(i) / static_cast<float>(taper));
      part[i] *= w;
      part[part.size() - 1 - i] *= w;
    }
    return 10.0 * std::log10(std::max(1.0e-30, energyOf(bandOf(part, 6000.0, 20000.0))) /
                             std::max(1.0e-30, energyOf(part)));
  };
  result.steadyHighDb = highDb(static_cast<size_t>(settle - samples(1.0)), static_cast<size_t>(settle));
  result.sweepHighDb = highDb(static_cast<size_t>(settle), out.size());
  fxtest::expectCleanSignal(out, 4.0f, "parameter sweep under audio");
  std::cout << "[  ZIPPER  ] " << kind << ": largest step " << juce::String(result.steadyStep, 5)
            << " (peak " << juce::String(result.steadyPeak, 3) << ", steepness "
            << juce::String(result.steadySteepness(), 4) << ") steady, "
            << juce::String(result.sweepStep, 5) << " (peak " << juce::String(result.sweepPeak, 3)
            << ", steepness " << juce::String(result.sweepSteepness(), 4) << "); above 6 kHz "
            << juce::String(result.steadyHighDb, 1) << " dB steady, " << juce::String(result.sweepHighDb, 1)
            << " dB while";
  for (const Sweep& sweep : sweeps)
    std::cout << " " << sweep.param;
  std::cout << " moved" << std::endl;
  return result;
}

/** The bounds every reverb is held to while its controls move. The waveform
    gets no steeper for its level than twice what it is at rest (movement
    adds sidebands, and a decay sweep changes the level, neither of which is
    a click), with a small absolute allowance. And nothing appears above
    6 kHz: under `highBoundDb` of the wet signal (-70 dB; at rest a 220 Hz
    sine through a reverb measures -116 dB there). */
inline void expectNoZipper(const ZipperResult& r, double highBoundDb = -70.0) {
  EXPECT_LT(r.sweepStep, 2.0f * r.steadySteepness() * r.sweepPeak + 0.002f)
      << "a control moved the output in steps";
  EXPECT_LT(r.sweepHighDb, highBoundDb) << "a moving control put clicks into the output";
}

/** The reported tail holds (silent after it, not shorter than the measured
    decay) at a setting of the caller's choice, mono and true stereo: the
    conformance test checks it only at its own setting. Prints both. */
inline void expectTailHolds(const char* kind, const Params& params) {
  for (const Layout layout : {Layout::Mono, Layout::Stereo}) {
    const fxtest::TailMeasurement tail = fxtest::expectSilenceAfterReportedTail(kind, params, 0.05, layout);
    std::cout << "[   TAIL   ] " << kind << " (" << fxtest::layoutName(layout) << "): reported "
              << juce::String(tail.reportedSeconds, 3) << " s, measured "
              << juce::String(tail.measuredSeconds, 3) << " s to -60 dB" << std::endl;
  }
}

/** Power gain of the wet path for steady mid-band noise (300 Hz to 3 kHz,
    where a guitar is), in dB, on the left of a true stereo buffer. */
inline double wetLevelDb(const char* kind, const Params& params, double settleSeconds) {
  DirectKind direct(kind, params, Layout::Stereo);
  const std::vector<float> in =
      bandOf(fxtest::noise(settleSeconds + 2.0, 41u, 0.1f, kRate), 300.0, 3000.0);
  const Stereo out = direct.render(Stereo{in, in});
  const size_t from = static_cast<size_t>(samples(settleSeconds));
  return 20.0 * std::log10(fxtest::rmsOf(out.left, from) / fxtest::rmsOf(in, from));
}

/** Tail spillover: a burst, the block switched off, then silence. With the
    switch on the tail is still there 150 ms later; with it off the bypass
    fade has cut it. */
inline void expectSpilloverSwitchWorks(const char* kind, const Params& params) {
  const auto tailAfterBypass = [&](bool spillover) {
    EffectRig rig(kind, with(params, {{"spillover", spillover ? 1.0 : 0.0}, {"mix", 0.5}}));
    EffectRig reference{juce::String()};
    const std::vector<float> burst = fxtest::noise(0.5, 5u, 0.25f, kRate);
    const size_t whole = (burst.size() / 256) * 256;
    const std::vector<float> in(burst.begin(), burst.begin() + static_cast<std::ptrdiff_t>(whole));
    rig.render(in);
    reference.render(in);
    rig.setEnabled(false);
    const std::vector<float> silence(static_cast<size_t>(samples(0.4)), 0.0f);
    const std::vector<float> out = rig.render(silence);
    const std::vector<float> dry = reference.render(silence);
    std::vector<float> added(out.size());
    for (size_t i = 0; i < out.size(); ++i)
      added[i] = out[i] - dry[i];
    return fxtest::rmsOf(added, static_cast<size_t>(samples(0.15)), static_cast<size_t>(samples(0.35)));
  };
  const double ringing = tailAfterBypass(true);
  const double cut = tailAfterBypass(false);
  std::cout << "[ SPILLOVER] " << kind << ": tail 150 ms after the bypass " << juce::String(ringing, 5)
            << " RMS with the switch on, " << juce::String(cut, 7) << " with it off" << std::endl;
  EXPECT_GT(ringing, 0.003) << kind << ": the tail was cut with spillover on";
  EXPECT_LT(cut, ringing * 0.01) << kind << ": the tail rings on with spillover off";
}

}  // namespace reverbtest

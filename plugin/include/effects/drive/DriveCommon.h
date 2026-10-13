#pragma once
// Helpers private to the drive family (plugin/src/effects/drive). Shared DSP
// stays in effects/dsp; what is here only makes sense for a clipping stage:
// the waveshapers, a 4x oversampling wrapper for the clipping core, a DC
// blocker and the little scheduling helpers the kinds share.
//
// How every drive kind is laid out:
//
//   base rate   input gate / input shaping that does not clip
//   4x rate     everything from the first filter that feeds a nonlinearity to
//               the last filter after it, so the paths that are summed (a
//               clean path and a clipped path, a low band and a driven band)
//               all see the same delay through the oversampler
//   base rate   tone controls and the level
//
// The oversampler is the chain's half-band IIR pair (ChainOversampler): two
// stages for 4x, more than 90 dB of rejection, no reported latency. The
// shapers are smooth algebraic curves rather than a hard clamp, which keeps
// their harmonics falling off quickly.
#include <algorithm>
#include <cmath>
#include <vector>
#include <juce_audio_basics/juce_audio_basics.h>
#include "ChainOversampler.h"
#include "effects/EffectDsp.h"

namespace drivefx {

constexpr int kMaxLanes = 2;
constexpr int kOversampleFactor = 4;
// Control-rate values (filter coefficients) are refreshed on a fixed grid of
// this many base-rate samples, counted across blocks, so the output does not
// depend on the block size.
constexpr int kControlGrid = 16;
constexpr int kControlGridOs = kControlGrid * kOversampleFactor;
// What a signal through the oversampler is late by, in base-rate samples,
// measured as where an impulse's response begins.
constexpr int kLatencySamples = 3;

/** x / sqrt(1 + x^2): smooth, odd, unity slope at 0, bounded by 1. */
inline float softSat(float x) noexcept {
  const float c = juce::jlimit(-1.0e4f, 1.0e4f, x);
  return c / std::sqrt(1.0f + c * c);
}

/** x / (1 + x^4)^(1/4): the same, with a sharper knee (a diode pair clipping
    to ground rather than a gentle soft stage). Bounded by 1. */
inline float hardSat(float x) noexcept {
  const float c = juce::jlimit(-1.0e3f, 1.0e3f, x);
  const float c2 = c * c;
  return c / std::sqrt(std::sqrt(1.0f + c2 * c2));
}

/** softSat of a shifted input with the offset taken out and the slope at
    the origin restored to 1: positive and negative halves clip at different
    heights, which adds even harmonics. */
class AsymmetricSoft {
public:
  void setBias(float bias) noexcept {
    shift = bias;
    offset = softSat(bias);
    norm = std::pow(1.0f + bias * bias, 1.5f);
  }
  float operator()(float x) const noexcept { return (softSat(x + shift) - offset) * norm; }

private:
  float shift{0.0f}, offset{0.0f}, norm{1.0f};
};

/** Removes DC (and the slow drift an asymmetric clipper makes). */
class DcBlocker {
public:
  void setCutoff(double hz, double sampleRate) noexcept {
    r = static_cast<float>(std::exp(-juce::MathConstants<double>::twoPi * hz / sampleRate));
  }
  void reset() noexcept { x1 = y1 = 0.0f; }
  float process(float x) noexcept {
    const float y = x - x1 + r * y1;
    x1 = x;
    y1 = fx::flushDenormal(y);
    return y;
  }

private:
  float r{0.995f}, x1{0.0f}, y1{0.0f};
};

/** True once every `period` calls, starting with the first after reset(). */
class ControlTick {
public:
  explicit ControlTick(int periodSamples) : period(periodSamples) {}
  void reset() noexcept { left = 0; }
  bool step() noexcept {
    if (left > 0) {
      --left;
      return false;
    }
    left = period - 1;
    return true;
  }

private:
  int period;
  int left{0};
};

/** lo * (hi / lo)^t for t in 0..1: a sweep that moves evenly by ear. */
inline float expSweep(float lo, float hi, float t) noexcept {
  return lo * std::pow(hi / lo, juce::jlimit(0.0f, 1.0f, t));
}

/** First-order shelves built as x + (gain - 1) * band, so a gain of 1 is
    exactly the input and the gain can move every sample without a
    coefficient to rebuild. */
class OnePoleShelf {
public:
  void setCorner(double hz, double sampleRate) noexcept { pole.setCutoff(hz, sampleRate); }
  void reset() noexcept { pole.reset(); }
  float low(float x, float gain) noexcept { return x + (gain - 1.0f) * pole.lowpass(x); }
  float high(float x, float gain) noexcept { return x + (gain - 1.0f) * pole.highpass(x); }

private:
  fx::OnePole pole;
};

/**
 * Runs `fn(float* const* channels, int frames)` on the block at four times the
 * base rate, in place: interpolate up, call, filter and decimate back. The
 * oversampler always carries two channels; a one-channel block is paired with
 * a silent spare so nothing is mixed, and `fn` is told how many are real.
 */
class Oversampler4x {
public:
  void prepare(int maxBlockSize) {
    oversampler.prepare(kOversampleFactor, juce::jmax(1, maxBlockSize));
    spare.assign(static_cast<size_t>(juce::jmax(1, maxBlockSize)), 0.0f);
  }
  void reset() noexcept { oversampler.reset(); }

  template <typename Fn>
  void run(juce::AudioBuffer<float>& buffer, Fn&& fn) {
    const int frames = buffer.getNumSamples();
    const int lanes = juce::jmin(buffer.getNumChannels(), kMaxLanes);
    float* io[kMaxLanes] = {buffer.getWritePointer(0), nullptr};
    if (lanes > 1) {
      io[1] = buffer.getWritePointer(1);
    } else {
      std::fill_n(spare.data(), frames, 0.0f);
      io[1] = spare.data();
    }
    oversampler.process(io, io, frames,
                        [&](float** channels, float**, int osFrames) { fn(channels, osFrames, lanes); });
  }

private:
  ChainOversampler oversampler;
  std::vector<float> spare;
};

}  // namespace drivefx

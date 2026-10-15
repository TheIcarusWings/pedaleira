#pragma once
// Helpers private to the drive family (plugin/src/effects/drive). Shared DSP
// stays in effects/dsp; what is here only makes sense for a clipping stage:
// the waveshapers, a 4x oversampling wrapper for the clipping core, a DC
// blocker and the little scheduling helpers the kinds share. The synth filter
// (filter/SynthFilterEffect.cpp) borrows the oversampler for its tanh ladder.
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

/** Unchanged up to 0.8, then eased into 1.0 with a matching slope: a
    ceiling for a stage whose gain comes after its clipping. */
inline float softCeiling(float x) noexcept {
  constexpr float kKnee = 0.8f;
  const float a = std::abs(x);
  if (a <= kKnee)
    return x;
  const float limited = kKnee + (1.0f - kKnee) * softSat((a - kKnee) / (1.0f - kKnee));
  return x < 0.0f ? -limited : limited;
}

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
 * The fixed part of a voicing fitted to a family of pedals measured on a unit:
 * where the clipping sits, how hard it is, and the response around it. The
 * kinds' measured voicings are rows of these, run by VoicedStage. Frequencies
 * are in Hz and 0 switches that filter off; a peak or shelf of 0 dB is
 * skipped.
 */
struct VoiceShape {
  // Before the clipper, at the 4x rate.
  float inputHz;       // one-pole high-pass on the whole input
  float splitHz;       // one-pole high-pass on the clipped path only
  float preLowpassHz;  // one-pole low-pass on the clipped path only
  float minGain;       // gain into the clipper at drive 0
  float maxGain;       // and at drive 1, exponential in between
  float clipLevel;     // where the clipper tops out, full-scale units
  int knee;            // 2 soft (softSat), 4 hard (hardSat), 8 close to a clamp
  float bias;          // input offset of the clipper, in units of clipLevel
  float clean;         // share of the unclipped input added back
  // An optional second clipping stage: a coupling high-pass, a gain and a
  // hard knee (secondHz 0 = none).
  float secondHz;
  float secondGain;
  float secondLevel;
  // After the clipper, at the base rate.
  float toneHz;       // low-pass at the middle of the tone control (0 = none)
  float toneQ;        // its resonance; 0 makes it a one-pole
  float toneOctaves;  // how far it moves over the whole tone control
  float lowPeakHz, lowPeakQ, lowPeakDb;
  float midPeakHz, midPeakQ, midPeakDb;
  float highShelfHz, highShelfDb;
  float postHighpassHz;
  float makeup;  // output scale before the kind's level control
};

/**
 * Runs a VoiceShape. The kind owns the oversampler, the smoothing and its
 * knobs; this holds the filters and the clipping of every lane:
 *
 *   4x:    v = highpass(x, inputHz)
 *          u = lowpass(highpass(v, splitHz), preLowpassHz)
 *          y = clean * v + wet * L * (shape(g * u / L + bias) - shape(bias))
 *          (y = L2 * hardSat(gain2 * highpass(y, secondHz) / L2), when set)
 *   base:  y = highpass(shelf(peaks(lowpass(y, tone))), postHighpassHz) * makeup
 *
 * The tone low-pass sits at toneHz with the control at its middle and moves
 * toneOctaves over the whole travel. Everything after the clipping is
 * linear, so the response to a sweep follows the post filters wherever the
 * clipper is saturated.
 */
class VoicedStage {
public:
  void prepare(double baseRate) noexcept {
    rate = baseRate;
    osRate = baseRate * kOversampleFactor;
  }

  /** Fixes the filters for `newShape` and clears every lane. */
  void setShape(const VoiceShape& newShape) noexcept {
    shape = &newShape;
    for (Lane& lane : lanes) {
      lane.input.setCutoff(juce::jmax(1.0f, shape->inputHz), osRate);
      lane.split.setCutoff(juce::jmax(1.0f, shape->splitHz), osRate);
      lane.pre.setCutoff(juce::jmax(1.0f, shape->preLowpassHz), osRate);
      lane.second.setCutoff(juce::jmax(1.0f, shape->secondHz), osRate);
      lane.tonePole.setCutoff(juce::jmax(1.0f, shape->toneHz), rate);
      lane.toneSvf.set(juce::jmax(5.0f, shape->toneHz), juce::jmax(0.3f, shape->toneQ), rate);
      lane.lowPeak.setCoeffs(
          fx::BiquadCoeffs::peak(shape->lowPeakHz, shape->lowPeakQ, shape->lowPeakDb, rate));
      lane.midPeak.setCoeffs(
          fx::BiquadCoeffs::peak(shape->midPeakHz, shape->midPeakQ, shape->midPeakDb, rate));
      lane.highShelf.setCoeffs(
          fx::BiquadCoeffs::highShelf(shape->highShelfHz, 0.7071, shape->highShelfDb, rate));
      lane.post.setCutoff(juce::jmax(1.0f, shape->postHighpassHz), rate);
    }
    biasOffset = clip(shape->bias);
    reset();
  }

  void reset() noexcept {
    for (Lane& lane : lanes) {
      lane.input.reset();
      lane.split.reset();
      lane.pre.reset();
      lane.second.reset();
      lane.tonePole.reset();
      lane.toneSvf.reset();
      lane.lowPeak.reset();
      lane.midPeak.reset();
      lane.highShelf.reset();
      lane.post.reset();
    }
  }

  /** The clipper gain for a drive of 0..1. */
  float gainFor(float drive) const noexcept {
    return shape->minGain *
           std::pow(shape->maxGain / shape->minGain, juce::jlimit(0.0f, 1.0f, drive));
  }

  /** The tone low-pass for a control of 0..1 (control rate). */
  void setTone(float tone) noexcept {
    if (shape->toneHz <= 0.0f)
      return;
    const float hz =
        shape->toneHz * std::exp2(shape->toneOctaves * (juce::jlimit(0.0f, 1.0f, tone) - 0.5f));
    for (Lane& lane : lanes) {
      if (shape->toneQ > 0.0f)
        lane.toneSvf.set(hz, shape->toneQ, rate);
      else
        lane.tonePole.setCutoff(hz, rate);
    }
  }

  /** Moves the split high-pass away from the shape's own corner (control
      rate, for a kind whose knob shifts it). */
  void setSplitHz(float hz) noexcept {
    for (Lane& lane : lanes)
      lane.split.setCutoff(juce::jmax(1.0f, hz), osRate);
  }

  /** Moves the post high-pass away from the shape's own corner (control
      rate). */
  void setPostHighpassHz(float hz) noexcept {
    for (Lane& lane : lanes)
      lane.post.setCutoff(juce::jmax(1.0f, hz), rate);
  }

  /** One sample of one lane at the 4x rate; `wet` scales the clipped path
      against the clean one (1 is the fitted mix). */
  float clipSample(int ch, float x, float gain, float wet = 1.0f) noexcept {
    Lane& lane = lanes[ch];
    const VoiceShape& s = *shape;
    const float v = s.inputHz > 0.0f ? lane.input.highpass(x) : x;
    float u = s.splitHz > 0.0f ? lane.split.highpass(v) : v;
    if (s.preLowpassHz > 0.0f)
      u = lane.pre.lowpass(u);
    float y = s.clipLevel * (clip(gain * u * (1.0f / s.clipLevel) + s.bias) - biasOffset);
    if (s.secondHz > 0.0f)
      y = s.secondLevel * hardSat(s.secondGain * lane.second.highpass(y) * (1.0f / s.secondLevel));
    return s.clean * v + wet * y;
  }

  /** One sample of one lane at the base rate, after the clipping. */
  float postSample(int ch, float x) noexcept {
    Lane& lane = lanes[ch];
    const VoiceShape& s = *shape;
    float y = x;
    if (s.toneHz > 0.0f)
      y = s.toneQ > 0.0f ? lane.toneSvf.lowpass(y) : lane.tonePole.lowpass(y);
    if (s.lowPeakDb != 0.0f)
      y = lane.lowPeak.process(y);
    if (s.midPeakDb != 0.0f)
      y = lane.midPeak.process(y);
    if (s.highShelfDb != 0.0f)
      y = lane.highShelf.process(y);
    if (s.postHighpassHz > 0.0f)
      y = lane.post.highpass(y);
    return y * s.makeup;
  }

private:
  float clip(float x) const noexcept {
    switch (shape->knee) {
      case 2:
        return softSat(x);
      case 8: {
        const float c = juce::jlimit(-100.0f, 100.0f, x);
        const float c4 = c * c * c * c;
        return c / std::sqrt(std::sqrt(std::sqrt(1.0f + c4 * c4)));
      }
      default:
        return hardSat(x);
    }
  }

  struct Lane {
    fx::OnePole input, split, pre, second;
    fx::OnePole tonePole;
    fx::StateVariableFilter toneSvf;
    fx::Biquad lowPeak, midPeak, highShelf;
    fx::OnePole post;
  };

  Lane lanes[kMaxLanes];
  const VoiceShape* shape{nullptr};
  float biasOffset{0.0f};
  double rate{48000.0};
  double osRate{192000.0};
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

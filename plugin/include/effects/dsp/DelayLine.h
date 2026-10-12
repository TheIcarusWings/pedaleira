#pragma once
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cstddef>
#include <vector>

namespace fx {

/**
 * A multi-channel delay line with integer, linear and third-order (Hermite)
 * reads, for delays, choruses, flangers, comb and allpass stages.
 *
 * prepare() allocates for the longest delay the effect will ever ask for;
 * nothing allocates after that. Each channel has its own write position, so
 * channels are written and read independently, one sample at a time:
 *
 *   const float echo = line.readLinear(ch, delaySamples);   // x[n - delay]
 *   line.write(ch, input + feedback * echo);                // x[n]
 *
 * A read of `delay` returns the sample written `delay` writes before the
 * NEXT write to that channel: delay 1 is the latest sample written. Reading
 * before writing, as above, therefore gives a feedback loop of exactly
 * `delay` samples. Delays are clamped to what the read can serve:
 *   read         1 .. maxDelaySamples
 *   readLinear   1 .. maxDelaySamples
 *   readHermite  2 .. maxDelaySamples   (it needs one newer sample)
 *
 * Linear is cheap and darkens slightly at fractional positions; use it for
 * fixed or slowly gliding times. Hermite (Catmull-Rom) stays flat far higher
 * and is the one to use when the delay is modulated (chorus, flanger, tape
 * wobble).
 */
class DelayLine {
public:
  /** Allocate `numChannels` lines that can delay by up to `maxDelaySamples`
      and clear them. Not on the audio thread. */
  void prepare(int numChannels, int maxDelaySamples) {
    channels = std::max(1, numChannels);
    maxDelay = std::max(2, maxDelaySamples);
    // Power of two, so wrapping is a mask; the guard covers the extra points
    // the Hermite read takes on the old side.
    size = static_cast<size_t>(juce::nextPowerOfTwo(maxDelay + kGuard));
    mask = size - 1;
    data.assign(static_cast<size_t>(channels) * size, 0.0f);
    writePos.assign(static_cast<size_t>(channels), 0);
  }

  /** Silence every line. No allocation: safe from resetEffect. */
  void clear() noexcept {
    std::fill(data.begin(), data.end(), 0.0f);
    std::fill(writePos.begin(), writePos.end(), size_t{0});
  }

  int numChannels() const noexcept { return channels; }
  int maxDelaySamples() const noexcept { return maxDelay; }

  /** Push one sample into `channel`. */
  void write(int channel, float sample) noexcept {
    size_t& pos = writePos[static_cast<size_t>(channel)];
    line(channel)[pos] = sample;
    pos = (pos + 1) & mask;
  }

  /** The sample `delay` writes back (see the class comment). */
  float read(int channel, int delay) const noexcept {
    return at(channel, static_cast<size_t>(juce::jlimit(1, maxDelay, delay)));
  }

  /** Linear interpolation at a fractional delay. */
  float readLinear(int channel, float delay) const noexcept {
    const float d = juce::jlimit(1.0f, static_cast<float>(maxDelay), delay);
    const size_t whole = static_cast<size_t>(d);
    const float frac = d - static_cast<float>(whole);
    const float newer = at(channel, whole);
    const float older = at(channel, whole + 1);
    return newer + frac * (older - newer);
  }

  /** Third-order Hermite (Catmull-Rom) interpolation at a fractional delay. */
  float readHermite(int channel, float delay) const noexcept {
    const float d = juce::jlimit(2.0f, static_cast<float>(maxDelay), delay);
    const size_t whole = static_cast<size_t>(d);
    const float frac = d - static_cast<float>(whole);
    // Four points around the read position, newest first; the result lies
    // between y0 (frac 0) and y1 (frac 1).
    const float ym1 = at(channel, whole - 1);
    const float y0 = at(channel, whole);
    const float y1 = at(channel, whole + 1);
    const float y2 = at(channel, whole + 2);
    const float c1 = 0.5f * (y1 - ym1);
    const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
    const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
    return ((c3 * frac + c2) * frac + c1) * frac + y0;
  }

private:
  static constexpr int kGuard = 4;

  float* line(int channel) noexcept { return data.data() + static_cast<size_t>(channel) * size; }
  const float* line(int channel) const noexcept {
    return data.data() + static_cast<size_t>(channel) * size;
  }
  // `back` is at most maxDelay + 2, below size, so one added size keeps the
  // unsigned subtraction from wrapping.
  float at(int channel, size_t back) const noexcept {
    return line(channel)[(writePos[static_cast<size_t>(channel)] + size - back) & mask];
  }

  std::vector<float> data;
  std::vector<size_t> writePos;
  size_t size{0};
  size_t mask{0};
  int channels{0};
  int maxDelay{0};
};

}  // namespace fx

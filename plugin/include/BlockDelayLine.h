#pragma once
#include <juce_audio_basics/juce_audio_basics.h>

// A fixed integer delay over up to two channels, in place. The chain uses it
// to keep paths in step with a block that reports latency: the dry share of
// that block's mix, and the lane that would otherwise arrive early (see
// TONE3000Processor::refreshLatencyCompensation).
//
// configure() allocates and belongs to the message or loader thread, under
// chainMutex; process() only reads and writes the ring it was given.
class BlockDelayLine {
public:
  // Size the line for `delaySamples` (0 = pass-through, frees nothing the
  // audio thread could be reading: callers hold chainMutex). The ring is
  // cleared, so a change of delay starts from silence; an unchanged delay
  // keeps its contents.
  void configure(int delaySamples) {
    delaySamples = juce::jmax(0, delaySamples);
    if (delaySamples == delay)
      return;
    delay = delaySamples;
    writePos = 0;
    ring.setSize(kChannels, juce::jmax(1, delay), false, true, false);
    ring.clear();
  }

  void clear() noexcept {
    ring.clear();
    writePos = 0;
  }

  int delaySamples() const noexcept { return delay; }

  // Delay `numChannels` (at most two) channels of `numSamples` frames in
  // place. A pass-through at zero delay.
  void process(float* const* channels, int numChannels, int numSamples) noexcept {
    if (delay <= 0 || numSamples <= 0)
      return;
    numChannels = juce::jmin(numChannels, kChannels);
    for (int ch = 0; ch < numChannels; ++ch) {
      float* line = ring.getWritePointer(ch);
      float* x = channels[ch];
      int pos = writePos;
      for (int i = 0; i < numSamples; ++i) {
        const float delayed = line[pos];
        line[pos] = x[i];
        x[i] = delayed;
        if (++pos == delay)
          pos = 0;
      }
    }
    writePos = (writePos + numSamples) % delay;
  }

private:
  static constexpr int kChannels = 2;
  juce::AudioBuffer<float> ring;
  int delay{0};
  int writePos{0};
};

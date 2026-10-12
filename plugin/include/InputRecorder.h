#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

// Copies one input channel into a buffer that was allocated beforehand, for
// recording a guitar profile take from the input the user is playing into.
// This is the one place the agent and library layer touches the audio
// thread, so it is kept to the minimum: capture() takes no lock, allocates
// nothing and, while no recording is armed, does one atomic read.
//
// Everything but capture() belongs to the message thread. The buffer is only
// replaced while the audio thread is known to be out of it: start() and
// cancel() first leave the recording state and then wait for `writing` to
// clear, and capture() raises `writing` before it looks at the state again
// (both sequentially consistent), so the two can never overlap.
class InputRecorder {
public:
  enum class State { idle, recording, full };

  // Arms a recording of `numSamples` samples of input channel `channel`
  // (a channel the device does not have falls back to channel 0). Replaces a
  // recording in progress.
  void start(int numSamples, int channel) {
    leaveRecording();
    samples.assign(static_cast<size_t>(std::max(1, numSamples)), 0.0f);
    position.store(0);
    peak.store(0.0f);
    sourceChannel.store(std::max(0, channel));
    state.store(static_cast<int>(State::recording));
  }

  // Stops and forgets what was recorded.
  void cancel() {
    leaveRecording();
    position.store(0);
  }

  State getState() const { return static_cast<State>(state.load()); }
  int recordedSamples() const { return position.load(); }
  int totalSamples() const { return static_cast<int>(samples.size()); }
  // The highest sample magnitude so far, for a level display while recording.
  float peakLevel() const { return peak.load(); }

  // Takes the finished recording out and returns to idle. Empty unless the
  // state is `full`.
  std::vector<float> take() {
    if (getState() != State::full)
      return {};
    leaveRecording();
    position.store(0);
    return std::move(samples);
  }

  // Audio thread.
  void capture(const juce::AudioBuffer<float>& buffer) noexcept {
    if (state.load(std::memory_order_relaxed) != static_cast<int>(State::recording))
      return;
    writing.store(true);
    if (state.load() == static_cast<int>(State::recording)) {
      const int channel = std::min(sourceChannel.load(std::memory_order_relaxed),
                                   buffer.getNumChannels() - 1);
      const int at = position.load(std::memory_order_relaxed);
      const int count = std::min(buffer.getNumSamples(), static_cast<int>(samples.size()) - at);
      if (channel >= 0 && count > 0) {
        const float* source = buffer.getReadPointer(channel);
        std::copy(source, source + count, samples.begin() + at);
        float blockPeak = peak.load(std::memory_order_relaxed);
        for (int i = 0; i < count; ++i)
          blockPeak = std::max(blockPeak, std::abs(source[i]));
        peak.store(blockPeak, std::memory_order_relaxed);
        position.store(at + count);
      }
      if (position.load(std::memory_order_relaxed) >= static_cast<int>(samples.size()))
        state.store(static_cast<int>(State::full));
    }
    writing.store(false);
  }

private:
  void leaveRecording() {
    state.store(static_cast<int>(State::idle));
    // At most one callback's copy; the audio thread never blocks on us.
    while (writing.load())
      std::this_thread::yield();
  }

  std::vector<float> samples;
  std::atomic<int> state{static_cast<int>(State::idle)};
  std::atomic<bool> writing{false};
  std::atomic<int> position{0};
  std::atomic<int> sourceChannel{0};
  std::atomic<float> peak{0.0f};
};

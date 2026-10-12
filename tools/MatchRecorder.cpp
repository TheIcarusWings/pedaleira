#include "MatchRecorder.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

void MatchRecorder::prepare(std::vector<float> signal, std::vector<int> outputs,
                            std::vector<int> inputs, int tailSamples) {
  running_.store(false, std::memory_order_relaxed);
  finished_.store(false, std::memory_order_relaxed);
  position_.store(0, std::memory_order_relaxed);
  signal_ = std::move(signal);
  outputs_ = std::move(outputs);
  inputs_ = std::move(inputs);
  total_ = static_cast<int>(signal_.size()) + std::max(tailSamples, 0);
  recording_.setSize(std::max(static_cast<int>(inputs_.size()), 1), std::max(total_, 1), false, true,
                     false);
  recording_.clear();
}

void MatchRecorder::start() {
  position_.store(0, std::memory_order_relaxed);
  finished_.store(false, std::memory_order_relaxed);
  recording_.clear();
  running_.store(true, std::memory_order_release);
}

MatchRecorder::Wait MatchRecorder::waitUntilFinished(int timeoutMs) const {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  for (;;) {
    if (finished_.load(std::memory_order_acquire))
      return Wait::finished;
    if (stopped_.load(std::memory_order_acquire))
      return Wait::deviceStopped;
    if (std::chrono::steady_clock::now() >= deadline)
      return Wait::timeout;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

double MatchRecorder::progress() const noexcept {
  if (total_ <= 0)
    return 0.0;
  return std::min(1.0, static_cast<double>(position_.load(std::memory_order_relaxed)) / total_);
}

int MatchRecorder::activeChannelIndex(const juce::BigInteger& activeMask, int channel) {
  if (channel < 0 || !activeMask[channel])
    return -1;
  int index = 0;
  for (int c = 0; c < channel; ++c)
    if (activeMask[c])
      ++index;
  return index;
}

void MatchRecorder::audioDeviceAboutToStart(juce::AudioIODevice*) {
  stopped_.store(false, std::memory_order_release);
}

void MatchRecorder::audioDeviceStopped() { stopped_.store(true, std::memory_order_release); }

void MatchRecorder::audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                                     int numInputChannels,
                                                     float* const* outputChannelData,
                                                     int numOutputChannels, int numSamples,
                                                     const juce::AudioIODeviceCallbackContext&) {
  // Silence everywhere first: the unused outputs stay silent, and so do the
  // used ones outside the signal.
  for (int c = 0; c < numOutputChannels; ++c)
    if (outputChannelData[c] != nullptr)
      std::memset(outputChannelData[c], 0, sizeof(float) * static_cast<size_t>(numSamples));

  if (!running_.load(std::memory_order_acquire))
    return;

  const int start = position_.load(std::memory_order_relaxed);
  const int count = std::min(numSamples, total_ - start);
  if (count > 0) {
    // The part of this block that lies inside the signal.
    const int playing = std::clamp(static_cast<int>(signal_.size()) - start, 0, count);
    if (playing > 0) {
      for (const int c : outputs_)
        if (c >= 0 && c < numOutputChannels && outputChannelData[c] != nullptr)
          std::memcpy(outputChannelData[c], signal_.data() + start,
                      sizeof(float) * static_cast<size_t>(playing));
    }
    // The same positions, recorded from the inputs. A listed input the device
    // did not deliver stays silent in the take.
    for (size_t k = 0; k < inputs_.size(); ++k) {
      const int c = inputs_[k];
      if (c >= 0 && c < numInputChannels && inputChannelData[c] != nullptr)
        std::memcpy(recording_.getWritePointer(static_cast<int>(k)) + start, inputChannelData[c],
                    sizeof(float) * static_cast<size_t>(count));
    }
  }

  const int next = start + std::max(count, 0);
  position_.store(next, std::memory_order_relaxed);
  if (next >= total_) {
    running_.store(false, std::memory_order_relaxed);
    finished_.store(true, std::memory_order_release);
  }
}

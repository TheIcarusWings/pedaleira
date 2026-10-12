#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <vector>

// Plays the effect-matching test signal and records the unit's answer in the
// same callback (docs/matching.md, "Recording without a DAW"). It knows
// nothing about devices: it is an AudioIODeviceCallback that is handed
// buffers, so the tests drive it with a fake loopback.
//
// Alignment: one play position serves both directions. In every callback the
// signal is read at the position and the input is written at the same position,
// so recorded sample n was captured while signal sample n was being handed to
// the device. What is left between them is the path's own latency, which the
// analysis finds from the sync markers.
//
// Real-time safety: the signal and the record buffer are allocated in
// prepare(), before the device runs. The callback only reads and writes those
// buffers and a few atomics: no allocation, no locks, no logging, no file I/O.
//
// Threads: prepare(), start() and recording() belong to the main thread, the
// callbacks to the audio thread. start() hands the buffers to the audio thread
// and the audio thread hands them back when finished() turns true (release and
// acquire on the flags), so prepare() and recording() must not be used in
// between.
class MatchRecorder : public juce::AudioIODeviceCallback {
 public:
  enum class Wait { finished, timeout, deviceStopped };

  // `outputs` and `inputs` are channel indices as the callback sees them, that
  // is counted over the active channels only (see activeChannelIndex). The
  // signal is played on every listed output; the listed inputs are recorded in
  // the order given. `tailSamples` is how long to keep recording after the
  // signal has ended.
  void prepare(std::vector<float> signal, std::vector<int> outputs, std::vector<int> inputs,
               int tailSamples);

  // Arms the recorder: from the next callback on the signal plays from its
  // first sample. Call it once the device is running.
  void start();

  // Blocks the calling thread (main thread only) until the take is complete,
  // the device stopped under it, or the timeout passed.
  Wait waitUntilFinished(int timeoutMs) const;

  bool finished() const noexcept { return finished_.load(std::memory_order_acquire); }
  // 0 to 1 over the whole take.
  double progress() const noexcept;
  int totalSamples() const noexcept { return total_; }
  int signalSamples() const noexcept { return static_cast<int>(signal_.size()); }

  // The inputs, one channel per entry of `inputs`, totalSamples() long. Only
  // meaningful once finished().
  const juce::AudioBuffer<float>& recording() const noexcept { return recording_; }

  // The index of device channel `channel` (0-based) in a callback that was
  // opened with `activeMask`: JUCE hands the callback only the channels set in
  // the mask, in ascending order. -1 when the channel is not active.
  static int activeChannelIndex(const juce::BigInteger& activeMask, int channel);

  void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
  void audioDeviceStopped() override;
  void audioDeviceIOCallbackWithContext(const float* const* inputChannelData, int numInputChannels,
                                        float* const* outputChannelData, int numOutputChannels,
                                        int numSamples,
                                        const juce::AudioIODeviceCallbackContext& context) override;

 private:
  std::vector<float> signal_;
  std::vector<int> outputs_;
  std::vector<int> inputs_;
  juce::AudioBuffer<float> recording_;
  int total_ = 0;

  std::atomic<int> position_{0};
  std::atomic<bool> running_{false};
  std::atomic<bool> finished_{false};
  std::atomic<bool> stopped_{false};
};

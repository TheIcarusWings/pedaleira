#pragma once
// Ducking for the reverbs: the wet output is lowered while the player is
// playing (the Ducker of plugin/include/effects/dsp/Ducker.h, the one the
// delays use) and comes back when the playing stops. A reverb runs in place,
// so the dry input is copied aside, in chunks held on the stack, before the
// reverb overwrites it; the gain follows that copy and multiplies the reverb's
// output. The reverb's own state is never touched, so the tail keeps ringing
// at its full level underneath.
//
// With the depth at 0 and the gain at unity (active() false) the reverb is
// called on the whole block and nothing else happens: a reverb with ducking
// off is bit-identical to one without the feature, and costs nothing.
//
// A linked stereo pair gets one gain, from the louder of the two inputs, so the
// image does not lean; in mono and dual mono each channel has its own.
#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <array>

#include "effects/dsp/Ducker.h"

namespace fx::reverb {

class WetDucking {
public:
  static constexpr int kMaxChannels = 2;
  static constexpr int kChunk = 128;

  void prepare(double sampleRate) noexcept {
    for (auto& ducker : duckers)
      ducker.prepare(sampleRate);
  }

  void reset() noexcept {
    for (auto& ducker : duckers)
      ducker.reset();
  }

  /**
   * Runs `process(buffer)`, the reverb on the block in place, and then lowers
   * its output by the ducking gain. `stereoImage`: the block is one linked
   * stereo pair (what the reverb itself is told).
   */
  template <typename Process>
  void run(juce::AudioBuffer<float>& buffer, bool stereoImage, float depthDb, float releaseMs,
           Process&& process) noexcept {
    const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxChannels);
    const int numSamples = buffer.getNumSamples();
    for (auto& ducker : duckers)
      ducker.setTarget(depthDb, releaseMs);
    const bool linked = stereoImage && numChannels > 1;
    const bool needed = duckers[0].active() || (!linked && numChannels > 1 && duckers[1].active());
    if (!needed || numChannels <= 0) {
      process(buffer);
      return;
    }

    float dry[kMaxChannels][kChunk];
    for (int start = 0; start < numSamples; start += kChunk) {
      const int length = std::min(kChunk, numSamples - start);
      float* channels[kMaxChannels] = {nullptr, nullptr};
      for (int ch = 0; ch < numChannels; ++ch) {
        channels[ch] = buffer.getWritePointer(ch) + start;
        std::copy_n(channels[ch], length, dry[ch]);
      }
      juce::AudioBuffer<float> view(channels, numChannels, length);
      process(view);

      if (linked) {
        for (int i = 0; i < length; ++i) {
          const float gain = duckers[0].next(dry[0][i], dry[1][i]);
          channels[0][i] *= gain;
          channels[1][i] *= gain;
        }
      } else {
        for (int ch = 0; ch < numChannels; ++ch)
          for (int i = 0; i < length; ++i)
            channels[ch][i] *= duckers[static_cast<size_t>(ch)].next(dry[ch][i], dry[ch][i]);
      }
    }
  }

private:
  std::array<fx::Ducker, kMaxChannels> duckers;
};

}  // namespace fx::reverb

#pragma once
// The reverse-window engine shared by delay.reverse and delay.dualreverse: the
// two playback voices that take turns reading a line backwards, with the
// equal-power crossfade at the joins. One ReverseWindow serves one line (all
// the channels of that line read at the same delays, so the caller reads the
// delay line itself). Header only, like the rest of the delay helpers.
//
//   window.advance(chunkSamples, fadeSamples);     // once per sample
//   echo = sum over v of window.weight[v] * line.read(ch, window.delay[v])
#include <algorithm>
#include <cmath>

namespace delayfx {

// The free time stops at 2 s; a synced chunk that would be longer is held
// at this length, and one shorter than the minimum is held there.
constexpr double kReverseMaxChunkSeconds = 2.0;
constexpr double kReverseMinChunkSeconds = 0.03;
// The crossfade at the chunk joins: a quarter of the chunk, at most this.
constexpr double kReverseMaxFadeSeconds = 0.06;

inline double reverseFadeSeconds(double chunk) noexcept {
  return std::min(kReverseMaxFadeSeconds, 0.25 * chunk);
}
inline double reverseVoiceSeconds(double chunk) noexcept {
  return chunk + reverseFadeSeconds(chunk);
}

class ReverseWindow {
public:
  static constexpr int kVoices = 2;

  // Set by advance(): the weight of each voice and the delay it reads at.
  float weight[kVoices] = {0.0f, 0.0f};
  int delay[kVoices] = {1, 1};

  void reset() noexcept {
    for (auto& voice : voices)
      voice.active = false;
    nextVoice = 0;
    untilNext = 0;
  }

  /** One sample: starts a new voice every chunk, at the chunk and fade of the
      moment, then sets the weight of each playing voice and the delay it reads
      at: the voice that started at time s reads, at s + n, the sample from
      s - 1 - n, which is 2n + 1 samples back. */
  void advance(int chunkSamples, int fadeSamples) noexcept {
    if (untilNext == 0) {
      Voice& voice = voices[nextVoice];
      voice.active = true;
      voice.position = 0;
      voice.fade = fadeSamples;
      voice.length = chunkSamples + fadeSamples;
      nextVoice ^= 1;
      untilNext = chunkSamples;
    }
    --untilNext;

    for (int v = 0; v < kVoices; ++v) {
      weight[v] = 0.0f;
      delay[v] = 1;
      Voice& voice = voices[v];
      if (!voice.active)
        continue;
      weight[v] = voice.weightAt();
      delay[v] = 2 * voice.position + 1;
      if (++voice.position >= voice.length)
        voice.active = false;
    }
  }

  /** The longest voice that is playing or about to start, in samples. */
  int longestSamples(int chunkSamples, int fadeSamples) const noexcept {
    int longest = chunkSamples + fadeSamples;
    for (const Voice& voice : voices)
      if (voice.active)
        longest = std::max(longest, voice.length);
    return longest;
  }

private:
  // One chunk being read backwards. The weight rises over `fade` samples,
  // stays at 1, and falls over the last `fade` samples. The rise and the fall
  // are an equal-power pair (their squares sum to 1) on a raised-cosine
  // path, so the join of two chunks, which are not correlated, keeps its
  // level and has no corner at either end.
  struct Voice {
    bool active{false};
    int position{0};
    int length{0};
    int fade{1};

    float weightAt() const noexcept {
      constexpr float kPi = 3.14159265358979f;
      if (position < fade) {
        const float path = 0.5f - 0.5f * std::cos(kPi * static_cast<float>(position) /
                                                   static_cast<float>(fade));
        return std::sin(0.5f * kPi * path);
      }
      const int intoFall = position - (length - fade);
      if (intoFall >= 0) {
        const float path = 0.5f - 0.5f * std::cos(kPi * static_cast<float>(intoFall) /
                                                   static_cast<float>(fade));
        return std::cos(0.5f * kPi * path);
      }
      return 1.0f;
    }
  };

  Voice voices[kVoices];
  int nextVoice{0};
  int untilNext{0};
};

}  // namespace delayfx

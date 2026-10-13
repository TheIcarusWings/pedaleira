// delay.reverse: echoes that play backwards. The input is recorded into a
// line, and every `time` a chunk of what was just played is read back from its
// newest sample to its oldest, so a phrase comes back reversed, one chunk
// after the other (the swell of a reversed guitar, the sucked-in start of a
// note). Reading backwards while the line is written forwards means the read
// delay grows by two samples per sample: a chunk of T seconds is read at
// delays from 1 sample to 2T seconds.
//
// Two playback voices take turns, a new one starting every T and each lasting
// T plus a fade; the fade is an equal-power crossfade on a raised cosine, the
// voice that ends fading out exactly as the next fades in (the squares of the
// two weights sum to 1), so the joins between chunks, where the audio itself
// jumps, are inaudible and keep their level. The repeats
// feed back into the line, so a repeat of a reversed chunk is reversed again,
// that is, forward.
#include <atomic>
#include <cmath>
#include "DelayCommon.h"
#include "effects/EffectDsp.h"

namespace {

using namespace delayfx;

class ReverseDelayProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "delay.reverse";

  enum Param {
    kSync = 0,
    kDivision,
    kTimeMs,
    kFeedback,
    kToneHz,
    kDuckDb,
    kDuckReleaseMs,
    kSpillover
  };

  // The free time stops at 2 s; a synced chunk that would be longer is held
  // at this length, and one shorter than the minimum is held there.
  static constexpr double kMaxChunkSeconds = 2.0;
  static constexpr double kMinChunkSeconds = 0.03;
  // The crossfade at the chunk joins: a quarter of the chunk, at most this.
  static constexpr double kMaxFadeSeconds = 0.06;

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      fx::TimeParamOptions time;
      time.minMs = 100.0f;
      time.maxMs = 2000.0f;
      time.defaultMs = 500.0f;
      time.affectsTail = true;
      fx::appendTimeParams(t, time);
      t.push_back(BlockParamSpec::floatParam("feedback", "Feedback", "%", 0.0f, 90.0f, 30.0f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("toneHz", "Tone", "Hz", 500.0f, 20000.0f, 7000.0f)
                      .withSkew(0.4f)
                      .withAffectsTail());
      appendDuckParams(t);
      t.push_back(spilloverParam());
      return t;
    }();
    return table;
  }

  ReverseDelayProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // An input sample is read back at a delay of at most twice the length of
  // the voice that reads it (the chunk plus its fade), and the read of the
  // newest sample can be that far away again for the next trip round the
  // loop. The tone filter and the limiter only lower the gain. A voice that
  // is already playing keeps its own length when the time is turned down,
  // hence the longer of the two.
  double tailSeconds() const noexcept override {
    const double feedback = static_cast<double>(param(kFeedback)) * 0.01;
    const double current = voiceSeconds(chunkSeconds(lastBpm.load(std::memory_order_relaxed)));
    const double playing = static_cast<double>(longestVoice.load(std::memory_order_relaxed));
    return feedbackTailSeconds(2.0 * std::max(current, playing), feedback,
                               lowpassDelaySeconds(static_cast<double>(param(kToneHz)), 2), 0.05);
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    line.prepare(kMaxChannels,
                 static_cast<int>(std::ceil(2.0 * (kMaxChunkSeconds + kMaxFadeSeconds) * sampleRate)) +
                     16);
    feedbackGain.prepare(sampleRate, 0.02);
    tone.prepare(sampleRate, 0.02);
    for (auto& duck : ducker)
      duck.prepare(sampleRate);
    resetEffect();
  }

  void resetEffect() noexcept override {
    line.clear();
    for (int ch = 0; ch < kMaxChannels; ++ch) {
      toneFilter[ch][0].reset();
      toneFilter[ch][1].reset();
      ducker[ch].reset();
    }
    feedbackGain.snapOnNextTarget();
    tone.snapOnNextTarget();
    for (auto& voice : voices)
      voice.active = false;
    nextVoice = 0;
    untilNext = 0;
    grid.reset();
    longestVoice.store(0.0f, std::memory_order_relaxed);
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxChannels);
    lastBpm.store(ctx.transport.bpm, std::memory_order_relaxed);

    const double chunk = chunkSeconds(ctx.transport.bpm);
    const int chunkSamples = std::max(1, static_cast<int>(std::lround(chunk * sampleRate)));
    const int fadeSamples = std::max(1, static_cast<int>(std::lround(fadeSeconds(chunk) * sampleRate)));
    feedbackGain.setTarget(param(kFeedback) * 0.01f);
    tone.setTarget(param(kToneHz));
    for (auto& duck : ducker)
      duck.setTarget(param(kDuckDb), param(kDuckReleaseMs));

    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    for (int i = 0; i < numSamples; ++i) {
      const float feedback = feedbackGain.next();
      const float toneHz = tone.next();
      if (grid.due()) {
        for (int ch = 0; ch < numChannels; ++ch) {
          toneFilter[ch][0].setCutoff(toneHz, sampleRate);
          toneFilter[ch][1].setCutoff(toneHz, sampleRate);
        }
      }

      // A new voice every chunk, at the chunk and fade of the moment.
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

      // The weight of each playing voice, and the delay it reads at: the
      // voice that started at time s reads, at s + n, the sample from
      // s - 1 - n, which is 2n + 1 samples back.
      float weight[kVoices] = {0.0f, 0.0f};
      int delay[kVoices] = {1, 1};
      for (int v = 0; v < kVoices; ++v) {
        Voice& voice = voices[v];
        if (!voice.active)
          continue;
        weight[v] = voice.weightAt();
        delay[v] = 2 * voice.position + 1;
        if (++voice.position >= voice.length)
          voice.active = false;
      }

      for (int ch = 0; ch < numChannels; ++ch) {
        float raw = 0.0f;
        for (int v = 0; v < kVoices; ++v)
          if (weight[v] != 0.0f)
            raw += weight[v] * line.read(ch, delay[v]);
        const float echo = toneFilter[ch][1].lowpass(toneFilter[ch][0].lowpass(raw));
        const float input = channels[ch][i];
        line.write(ch, softLimit(input + feedback * echo));
        // Wet only: the dry signal and the balance are the block's mix. The
        // ducking lowers it while the input is loud.
        channels[ch][i] = echo * ducker[ch].next(input, input);
      }
    }

    // The longest voice that is playing or about to start, for the tail.
    int longest = chunkSamples + fadeSamples;
    for (const Voice& voice : voices)
      if (voice.active)
        longest = std::max(longest, voice.length);
    longestVoice.store(static_cast<float>(static_cast<double>(longest) / sampleRate),
                       std::memory_order_relaxed);
  }

private:
  static constexpr int kVoices = 2;

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

  static double fadeSeconds(double chunk) noexcept {
    return std::min(kMaxFadeSeconds, 0.25 * chunk);
  }
  static double voiceSeconds(double chunk) noexcept { return chunk + fadeSeconds(chunk); }

  double chunkSeconds(double bpm) const noexcept {
    BlockTransport transport;
    transport.bpm = bpm;
    return juce::jlimit(kMinChunkSeconds, kMaxChunkSeconds,
                        fx::resolveTimeSeconds(param(kSync), param(kDivision), param(kTimeMs),
                                               transport));
  }

  fx::DelayLine line;
  fx::OnePole toneFilter[kMaxChannels][2];
  fx::SmoothedParam feedbackGain;
  fx::SmoothedParam tone;
  Ducker ducker[kMaxChannels];
  Voice voices[kVoices];
  int nextVoice{0};
  int untilNext{0};
  ControlTick grid;
  double sampleRate{48000.0};
  std::atomic<double> lastBpm{120.0};
  std::atomic<float> longestVoice{0.0f};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(ReverseDelay) {
  // Heard beside the dry signal (see DigitalDelay): 35% to start, since the
  // reversed chunks are softer at their edges.
  addEffectKind<ReverseDelayProcessor>(registry, "Reverse Delay", BlockCategory::Delay, 0.35f,
                                       BlockMixLaw::DryPreserving);
}

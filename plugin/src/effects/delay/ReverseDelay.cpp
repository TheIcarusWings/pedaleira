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
#include "ReverseWindow.h"
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

  static constexpr double kMaxChunkSeconds = kReverseMaxChunkSeconds;
  static constexpr double kMinChunkSeconds = kReverseMinChunkSeconds;
  static constexpr double kMaxFadeSeconds = kReverseMaxFadeSeconds;

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
    const double current = reverseVoiceSeconds(chunkSeconds(lastBpm.load(std::memory_order_relaxed)));
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
    window.reset();
    grid.reset();
    longestVoice.store(0.0f, std::memory_order_relaxed);
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxChannels);
    lastBpm.store(ctx.transport.bpm, std::memory_order_relaxed);

    const double chunk = chunkSeconds(ctx.transport.bpm);
    const int chunkSamples = std::max(1, static_cast<int>(std::lround(chunk * sampleRate)));
    const int fadeSamples = std::max(1, static_cast<int>(std::lround(reverseFadeSeconds(chunk) * sampleRate)));
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

      window.advance(chunkSamples, fadeSamples);

      for (int ch = 0; ch < numChannels; ++ch) {
        float raw = 0.0f;
        for (int v = 0; v < ReverseWindow::kVoices; ++v)
          if (window.weight[v] != 0.0f)
            raw += window.weight[v] * line.read(ch, window.delay[v]);
        const float echo = toneFilter[ch][1].lowpass(toneFilter[ch][0].lowpass(raw));
        const float input = channels[ch][i];
        line.write(ch, softLimit(input + feedback * echo));
        // Wet only: the dry signal and the balance are the block's mix. The
        // ducking lowers it while the input is loud.
        channels[ch][i] = echo * ducker[ch].next(input, input);
      }
    }

    const int longest = window.longestSamples(chunkSamples, fadeSamples);
    longestVoice.store(static_cast<float>(static_cast<double>(longest) / sampleRate),
                       std::memory_order_relaxed);
  }

private:
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
  ReverseWindow window;
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

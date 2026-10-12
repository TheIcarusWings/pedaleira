// dynamics.limiter: a look-ahead brick-wall limiter. The audio is delayed by
// the look-ahead (2 ms, reported as the block's latency) while the gain is
// worked out from the audio that has not been played yet, so the gain is
// already down, along a smooth ramp, when a peak reaches the output.
//
// The gain for each sample is built in three steps:
//   1. the gain that would just fit each incoming sample under the ceiling,
//   2. the minimum of that over the look-ahead window (sliding minimum), so a
//      peak pulls the gain down for the whole window around it,
//   3. a release curve on top (it may fall at once, it recovers slowly), then
//      a moving average over the window, which turns the falls into ramps.
// With a window of delay + 1 samples the average of values that are all at or
// below the needed gain is itself at or below it exactly when the peak comes
// out of the delay, so the output never exceeds the ceiling; a final clamp
// only takes care of float rounding and of the ceiling being moved.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "effects/EffectDsp.h"

namespace {

class LimiterProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "dynamics.limiter";

  enum Param { kCeilingDb = 0, kReleaseMs, kInputGainDb };
  enum Reading { kGainReduction = 0 };

  static constexpr double kLookaheadSeconds = 0.002;

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::floatParam("ceilingDb", "Ceiling", "dB", -24.0f, 0.0f, -1.0f));
      t.push_back(BlockParamSpec::floatParam("releaseMs", "Release", "ms", 5.0f, 1000.0f, 100.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("inputGainDb", "Input Gain", "dB", -12.0f, 24.0f, 0.0f));
      return t;
    }();
    return table;
  }

  // How far the gain is pulled down: the most within each processed block.
  static const BlockReadingTable& readingSpecs() {
    static const BlockReadingTable table = {
        {"gainReductionDb", "Gain reduction", "dB", 0.0f, 60.0f}};
    return table;
  }

  LimiterProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs(), readingSpecs()) {}

  // The look-ahead is fixed (it does not follow a parameter), so it is not
  // latched: it is set when the block is prepared.
  int latencySamples() const noexcept override { return lookahead; }

  // Nothing rings after the input stops except the audio still in the
  // look-ahead delay.
  double tailSeconds() const noexcept override { return static_cast<double>(lookahead) / sampleRate; }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    lookahead = juce::jmax(1, static_cast<int>(std::lround(kLookaheadSeconds * sampleRate)));
    for (auto& lane : lanes)
      lane.allocate(lookahead);
    inputGain.prepare(sampleRate, 0.02);
    resetEffect();
  }

  void resetEffect() noexcept override {
    for (auto& lane : lanes)
      lane.clear();
    inputGain.snapOnNextTarget();
    wasLinked = false;
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxChannels);
    // One stereo signal gets one gain (the image holds); dual mono is two
    // guitars, each limited on its own.
    const bool linked = numChannels == 2 && !ctx.dualMono;
    if (linked != wasLinked) {
      // The second gain stage sat idle (or the first drove both): start it
      // fresh rather than from a stale window.
      lanes[1].clearGain();
      wasLinked = linked;
    }

    const float ceiling = fx::dbToGain(param(kCeilingDb));
    const float releaseStep = static_cast<float>(
        -std::expm1(-1.0 / (0.001 * static_cast<double>(param(kReleaseMs)) * sampleRate)));
    inputGain.setTarget(fx::dbToGain(param(kInputGainDb)));

    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    float leastGain = 1.0f;
    for (int i = 0; i < numSamples; ++i) {
      const float drive = inputGain.next();
      float in[kMaxChannels] = {0.0f, 0.0f};
      for (int ch = 0; ch < numChannels; ++ch)
        in[ch] = channels[ch][i] * drive;

      float gain[kMaxChannels] = {1.0f, 1.0f};
      if (linked) {
        gain[0] = gain[1] = lanes[0].nextGain(
            neededGain(juce::jmax(std::abs(in[0]), std::abs(in[1])), ceiling), releaseStep);
      } else {
        for (int ch = 0; ch < numChannels; ++ch)
          gain[ch] = lanes[ch].nextGain(neededGain(std::abs(in[ch]), ceiling), releaseStep);
      }

      for (int ch = 0; ch < numChannels; ++ch) {
        const float delayed = lanes[ch].delay(in[ch]);
        channels[ch][i] = juce::jlimit(-ceiling, ceiling, delayed * gain[ch]);
        leastGain = juce::jmin(leastGain, gain[ch]);
      }
    }
    // The gain the limiter applies, not counting its input gain.
    setReading(kGainReduction, leastGain < 1.0f ? -fx::gainToDb(leastGain) : 0.0f);
  }

private:
  static constexpr int kMaxChannels = 2;

  static float neededGain(float magnitude, float ceiling) noexcept {
    return magnitude > ceiling ? ceiling / magnitude : 1.0f;
  }

  // One channel's audio delay and gain computer.
  struct Lane {
    void allocate(int lookaheadSamples) {
      window = lookaheadSamples + 1;
      audio.assign(static_cast<size_t>(lookaheadSamples), 0.0f);
      average.assign(static_cast<size_t>(window), 1.0f);
      minValue.assign(static_cast<size_t>(window) + 1, 1.0f);
      minTime.assign(static_cast<size_t>(window) + 1, 0);
      clear();
    }

    void clear() noexcept {
      std::fill(audio.begin(), audio.end(), 0.0f);
      audioPos = 0;
      clearGain();
    }

    void clearGain() noexcept {
      std::fill(average.begin(), average.end(), 1.0f);
      sum = static_cast<double>(window);
      averagePos = 0;
      front = back = 0;
      count = 0;
      time = 0;
      held = 1.0f;
    }

    // The sample from `lookahead` samples ago; stores this one.
    float delay(float x) noexcept {
      const float out = audio[static_cast<size_t>(audioPos)];
      audio[static_cast<size_t>(audioPos)] = x;
      if (++audioPos == static_cast<int>(audio.size()))
        audioPos = 0;
      return out;
    }

    float nextGain(float needed, float releaseStep) noexcept {
      // Sliding minimum over the last `window` samples: the monotonic
      // deque keeps candidates in increasing order, so the front is the
      // minimum, and each sample is pushed and popped once.
      const int capacity = window + 1;
      while (count > 0 && minValue[static_cast<size_t>((back + capacity - 1) % capacity)] >= needed) {
        back = (back + capacity - 1) % capacity;
        --count;
      }
      minValue[static_cast<size_t>(back)] = needed;
      minTime[static_cast<size_t>(back)] = time;
      back = (back + 1) % capacity;
      ++count;
      while (minTime[static_cast<size_t>(front)] <= time - window) {
        front = (front + 1) % capacity;
        --count;
      }
      const float lowest = minValue[static_cast<size_t>(front)];
      ++time;

      // Falls at once, recovers by the release curve (never above `lowest`).
      held = lowest < held ? lowest : lowest + (1.0f - releaseStep) * (held - lowest);

      // Moving average over the window: the fall becomes a ramp that spans
      // the look-ahead.
      sum += static_cast<double>(held) - static_cast<double>(average[static_cast<size_t>(averagePos)]);
      average[static_cast<size_t>(averagePos)] = held;
      if (++averagePos == window)
        averagePos = 0;
      return juce::jmin(1.0f, static_cast<float>(sum / static_cast<double>(window)));
    }

    std::vector<float> audio;
    std::vector<float> average;
    std::vector<float> minValue;
    std::vector<std::int64_t> minTime;
    double sum{0.0};
    std::int64_t time{0};
    float held{1.0f};
    int window{1};
    int audioPos{0};
    int averagePos{0};
    int front{0}, back{0}, count{0};
  };

  Lane lanes[kMaxChannels];
  fx::SmoothedParam inputGain;
  double sampleRate{48000.0};
  int lookahead{96};
  bool wasLinked{false};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(LimiterEffect) {
  addEffectKind<LimiterProcessor>(registry, "Limiter", BlockCategory::Dynamics);
}

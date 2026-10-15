// delay.dualreverse: two reverse delays side by side, a left line and a right
// line, each with its own time (free or tempo-synced to a note value) and its
// own feedback. Each line is the reverse delay of delay.reverse (the same
// two-voice window engine, ReverseWindow.h: chunks of what was just played read
// back from newest to oldest, joined by an equal-power crossfade), so the
// time of a line changes the way it does there, at the next chunk.
// `crossFeedback` decides where each line's repeats go: at 0% every line feeds
// only itself, at 100% each feeds the other, and between a share of each
// repeat crosses over. A low-pass (tone) and a high-pass shape the repeats on
// every trip round the loop.
//
// Layouts. In a true stereo image the left input feeds the left line and the
// right input the right line, and each is heard on its own side. Everywhere
// else (a mono buffer, dual mono, a stereo image an amp model later in the
// lane would throw away) every channel runs both lines on its own input and
// is heard as their sum, so a mono row gets both lines in one signal and the
// two halves of dual mono stay apart.
#include <atomic>
#include <cmath>
#include "DelayCommon.h"
#include "ReverseWindow.h"
#include "effects/EffectDsp.h"

namespace {

using namespace delayfx;

class DualReverseDelayProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "delay.dualreverse";

  // Table order. Each side's time is the standard three parameters under a
  // prefix (leftSync, leftDivision, leftTimeMs; rightSync, ...), as in
  // delay.dual.
  enum Param {
    kLeftSync = 0,
    kLeftDivision,
    kLeftTimeMs,
    kRightSync,
    kRightDivision,
    kRightTimeMs,
    kLeftFeedback,
    kRightFeedback,
    kCrossFeedback,
    kToneHz,
    kLowCutHz,
    kDuckDb,
    kDuckReleaseMs,
    kSpillover
  };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      fx::TimeParamOptions left;
      left.minMs = 100.0f;
      left.maxMs = 2000.0f;
      left.defaultMs = 500.0f;
      left.affectsTail = true;
      left.name = "Left Time";
      left.idPrefix = "left";
      fx::appendTimeParams(t, left);
      fx::TimeParamOptions right = left;
      right.defaultMs = 750.0f;
      right.name = "Right Time";
      right.idPrefix = "right";
      fx::appendTimeParams(t, right);
      t.push_back(BlockParamSpec::floatParam("leftFeedback", "Left Feedback", "%", 0.0f, 90.0f, 30.0f)
                      .withAffectsTail());
      t.push_back(
          BlockParamSpec::floatParam("rightFeedback", "Right Feedback", "%", 0.0f, 90.0f, 30.0f)
              .withAffectsTail());
      // The share of each line's repeats that goes to the other line.
      t.push_back(BlockParamSpec::floatParam("crossFeedback", "Cross Feedback", "%", 0.0f, 100.0f,
                                             0.0f));
      t.push_back(BlockParamSpec::floatParam("toneHz", "Tone", "Hz", 500.0f, 20000.0f, 7000.0f)
                      .withSkew(0.4f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("lowCutHz", "Low Cut", "Hz", 20.0f, 800.0f, 20.0f)
                      .withSkew(0.4f));
      appendDuckParams(t);
      t.push_back(spilloverParam());
      return t;
    }();
    return table;
  }

  DualReverseDelayProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // As delay.reverse, with the longer of the two lines as the pass and the
  // larger feedback as the loop gain: the matrix that splits the repeats has
  // column sums equal to the two feedbacks, so every hop round the loop,
  // across or not, applies at most the larger one and takes at most the
  // longer pass. The filters and the limiter only lower the gain.
  double tailSeconds() const noexcept override {
    const double bpm = lastBpm.load(std::memory_order_relaxed);
    double pass = 0.0;
    for (int n = 0; n < 2; ++n) {
      const double current = reverseVoiceSeconds(chunkSeconds(n, bpm));
      const double playing = static_cast<double>(longestVoice[n].load(std::memory_order_relaxed));
      pass = std::max(pass, 2.0 * std::max(current, playing));
    }
    const double feedback =
        static_cast<double>(std::max(param(kLeftFeedback), param(kRightFeedback))) * 0.01;
    return feedbackTailSeconds(pass, feedback,
                               lowpassDelaySeconds(static_cast<double>(param(kToneHz)), 2), 0.05);
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    line.prepare(kLines, static_cast<int>(std::ceil(2.0 * (kReverseMaxChunkSeconds +
                                                           kReverseMaxFadeSeconds) *
                                                    sampleRate)) +
                             16);
    leftGain.prepare(sampleRate, 0.02);
    rightGain.prepare(sampleRate, 0.02);
    crossShare.prepare(sampleRate, 0.03);
    tone.prepare(sampleRate, 0.02);
    lowCut.prepare(sampleRate, 0.02);
    for (auto& duck : ducker)
      duck.prepare(sampleRate);
    resetEffect();
  }

  void resetEffect() noexcept override {
    line.clear();
    for (int n = 0; n < kLines; ++n) {
      toneFilter[n][0].reset();
      toneFilter[n][1].reset();
      lowCutFilter[n].reset();
      window[n].reset();
    }
    for (auto& duck : ducker)
      duck.reset();
    leftGain.snapOnNextTarget();
    rightGain.snapOnNextTarget();
    crossShare.snapOnNextTarget();
    tone.snapOnNextTarget();
    lowCut.snapOnNextTarget();
    grid.reset();
    for (auto& longest : longestVoice)
      longest.store(0.0f, std::memory_order_relaxed);
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxChannels);
    lastBpm.store(ctx.transport.bpm, std::memory_order_relaxed);

    int chunkSamples[2];
    int fadeSamples[2];
    for (int side = 0; side < 2; ++side) {
      const double chunk = chunkSeconds(side, ctx.transport.bpm);
      chunkSamples[side] = std::max(1, static_cast<int>(std::lround(chunk * sampleRate)));
      fadeSamples[side] =
          std::max(1, static_cast<int>(std::lround(reverseFadeSeconds(chunk) * sampleRate)));
    }
    leftGain.setTarget(param(kLeftFeedback) * 0.01f);
    rightGain.setTarget(param(kRightFeedback) * 0.01f);
    crossShare.setTarget(param(kCrossFeedback) * 0.01f);
    tone.setTarget(param(kToneHz));
    lowCut.setTarget(param(kLowCutHz));
    for (auto& duck : ducker)
      duck.setTarget(param(kDuckDb), param(kDuckReleaseMs));

    const bool image = blockHasStereoImage(buffer.getNumChannels(), ctx);
    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    for (int i = 0; i < numSamples; ++i) {
      const float leftFeedback = leftGain.next();
      const float rightFeedback = rightGain.next();
      const float cross = crossShare.next();
      const float toneHz = tone.next();
      const float lowHz = lowCut.next();
      if (grid.due()) {
        const int used = image ? 2 : numChannels * 2;
        for (int n = 0; n < used; ++n) {
          toneFilter[n][0].setCutoff(toneHz, sampleRate);
          toneFilter[n][1].setCutoff(toneHz, sampleRate);
          lowCutFilter[n].setCutoff(lowHz, sampleRate);
        }
      }

      if (image) {
        // Lines 0 (left) and 1 (right); the left input feeds the left line,
        // the right input the right one.
        const float inLeft = channels[0][i];
        const float inRight = channels[1][i];
        float echoLeft, echoRight;
        step(0, 1, inLeft, inRight, leftFeedback, rightFeedback, cross, chunkSamples,
             fadeSamples, echoLeft, echoRight);
        // One gain from the louder side, so the image does not lean.
        const float duck = ducker[0].next(inLeft, inRight);
        channels[0][i] = echoLeft * duck;
        channels[1][i] = echoRight * duck;
        continue;
      }

      // Each channel is a pair of lines on its own input, heard together.
      for (int ch = 0; ch < numChannels; ++ch) {
        const float input = channels[ch][i];
        float echoLeft, echoRight;
        step(2 * ch, 2 * ch + 1, input, input, leftFeedback, rightFeedback, cross, chunkSamples,
             fadeSamples, echoLeft, echoRight);
        channels[ch][i] = (echoLeft + echoRight) * ducker[ch].next(input, input);
      }
    }

    // The longest voice that is playing or about to start, for the tail.
    for (int side = 0; side < 2; ++side) {
      int longest = 0;
      for (int n = side; n < kLines; n += 2)
        longest = std::max(longest, window[n].longestSamples(chunkSamples[side], fadeSamples[side]));
      longestVoice[side].store(static_cast<float>(static_cast<double>(longest) / sampleRate),
                               std::memory_order_relaxed);
    }
  }

private:
  static constexpr int kLines = 2 * kMaxChannels;

  float readEcho(int n) noexcept {
    float raw = 0.0f;
    for (int v = 0; v < ReverseWindow::kVoices; ++v)
      if (window[n].weight[v] != 0.0f)
        raw += window[n].weight[v] * line.read(n, window[n].delay[v]);
    return toneFilter[n][1].lowpass(toneFilter[n][0].lowpass(lowCutFilter[n].highpass(raw)));
  }

  /** One sample of a pair of lines (`a` the left one, `b` the right one).
      Each echo goes back with its own line's feedback, split between its own
      line and the other by `cross`; the column sums of that split are the two
      feedbacks, so the loop never gains. */
  void step(int a, int b, float inA, float inB, float feedbackA, float feedbackB, float cross,
            const int* chunkSamples, const int* fadeSamples, float& echoA,
            float& echoB) noexcept {
    window[a].advance(chunkSamples[0], fadeSamples[0]);
    window[b].advance(chunkSamples[1], fadeSamples[1]);
    echoA = readEcho(a);
    echoB = readEcho(b);
    const float stay = 1.0f - cross;
    line.write(a, softLimit(inA + feedbackA * stay * echoA + feedbackB * cross * echoB));
    line.write(b, softLimit(inB + feedbackB * stay * echoB + feedbackA * cross * echoA));
  }

  double chunkSeconds(int side, double bpm) const noexcept {
    BlockTransport transport;
    transport.bpm = bpm;
    const int first = side == 0 ? kLeftSync : kRightSync;
    return juce::jlimit(kReverseMinChunkSeconds, kReverseMaxChunkSeconds,
                        fx::resolveTimeSeconds(param(first), param(first + 1), param(first + 2),
                                               transport));
  }

  fx::DelayLine line;
  ReverseWindow window[kLines];
  fx::OnePole toneFilter[kLines][2];
  fx::OnePole lowCutFilter[kLines];
  fx::SmoothedParam leftGain;
  fx::SmoothedParam rightGain;
  fx::SmoothedParam crossShare;
  fx::SmoothedParam tone;
  fx::SmoothedParam lowCut;
  Ducker ducker[kMaxChannels];
  ControlTick grid;
  double sampleRate{48000.0};
  std::atomic<double> lastBpm{120.0};
  std::atomic<float> longestVoice[2] = {{0.0f}, {0.0f}};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(DualReverseDelay) {
  // Heard beside the dry signal (see ReverseDelay): 35% to start.
  addEffectKind<DualReverseDelayProcessor>(registry, "Dual Reverse", BlockCategory::Delay, 0.35f,
                                           BlockMixLaw::DryPreserving);
}

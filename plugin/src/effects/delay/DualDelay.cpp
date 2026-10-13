// delay.dual: two delays side by side, a left one and a right one, each with
// its own time (free or tempo-synced to a note value) and its own feedback.
// `crossFeedback` decides where each side's repeats go: at 0% every side
// feeds only itself (two independent delays), at 100% each side feeds the
// other (a ping pong whose two legs have different lengths), and anywhere
// between a share of each repeat crosses over. A low cut and a high cut shape
// the repeats on every trip round the loop.
//
// Layouts. In a true stereo image the left input feeds the left delay and the
// right input the right delay, and each is heard on its own side. Everywhere
// else (a mono buffer, dual mono, a stereo image an amp model later in the
// lane would throw away) every channel runs both delays on its own input and
// is heard as their average, so a mono row gets both rhythms in one signal
// (the sum of the two echoes, each at half level when the times differ) and
// the two halves of dual mono stay apart.
#include <atomic>
#include <cmath>
#include "DelayCommon.h"
#include "effects/EffectDsp.h"

namespace {

using namespace delayfx;

class DualDelayProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "delay.dual";

  // Table order. Each side's time is the standard three parameters under a
  // prefix (leftSync, leftDivision, leftTimeMs; rightSync, ...), so a client
  // pairs them by id the way it does for a single time.
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
    kLowCutHz,
    kHighCutHz,
    kDuckDb,
    kDuckReleaseMs,
    kSpillover
  };

  // The free times stop at 6 s; a synced time that would be longer is held
  // at this length.
  static constexpr double kMaxDelaySeconds = 6.0;

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      fx::TimeParamOptions left;
      left.minMs = 7.0f;
      left.maxMs = 6000.0f;
      left.defaultMs = 250.0f;
      left.affectsTail = true;
      left.name = "Left Time";
      left.idPrefix = "left";
      fx::appendTimeParams(t, left);
      fx::TimeParamOptions right = left;
      right.defaultMs = 375.0f;
      right.name = "Right Time";
      right.idPrefix = "right";
      fx::appendTimeParams(t, right);
      t.push_back(BlockParamSpec::floatParam("leftFeedback", "Left Feedback", "%", 0.0f, 95.0f, 35.0f)
                      .withAffectsTail());
      t.push_back(
          BlockParamSpec::floatParam("rightFeedback", "Right Feedback", "%", 0.0f, 95.0f, 35.0f)
              .withAffectsTail());
      // The share of each side's repeats that goes to the other side.
      t.push_back(BlockParamSpec::floatParam("crossFeedback", "Cross Feedback", "%", 0.0f, 100.0f,
                                             0.0f));
      t.push_back(BlockParamSpec::floatParam("lowCutHz", "Low Cut", "Hz", 20.0f, 800.0f, 80.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("highCutHz", "High Cut", "Hz", 1000.0f, 16000.0f,
                                             8000.0f)
                      .withSkew(0.4f)
                      .withAffectsTail());
      appendDuckParams(t);
      t.push_back(spilloverParam());
      return t;
    }();
    return table;
  }

  DualDelayProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // Every hop round the loop, own side or across, applies one side's feedback
  // and takes at most the longer time; the cut filters and the limiter only
  // lower the gain. The matrix that splits the repeats has column sums equal
  // to the two feedbacks, so the larger one bounds the loop. A crossfade in
  // progress adds its length once.
  double tailSeconds() const noexcept override {
    const double bpm = lastBpm.load(std::memory_order_relaxed);
    const double feedback =
        static_cast<double>(std::max(param(kLeftFeedback), param(kRightFeedback))) * 0.01;
    return feedbackTailSeconds(std::max(leftSeconds(bpm), rightSeconds(bpm)), feedback,
                               lowpassDelaySeconds(static_cast<double>(param(kHighCutHz)), 2),
                               kFadeSeconds + 0.01);
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    // Four lines: the left and the right delay of each of two channels.
    line.prepare(kLines, static_cast<int>(std::ceil(kMaxDelaySeconds * sampleRate)) + 16);
    for (auto& tap : taps)
      tap.prepare(sampleRate, kFadeSeconds);
    leftGain.prepare(sampleRate, 0.02);
    rightGain.prepare(sampleRate, 0.02);
    crossShare.prepare(sampleRate, 0.03);
    lowCut.prepare(sampleRate, 0.02);
    highCut.prepare(sampleRate, 0.02);
    for (auto& duck : ducker)
      duck.prepare(sampleRate);
    resetEffect();
  }

  void resetEffect() noexcept override {
    line.clear();
    for (int n = 0; n < kLines; ++n) {
      lowCutFilter[n].reset();
      highCutFilter[n][0].reset();
      highCutFilter[n][1].reset();
      taps[n].snapOnNextTarget();
    }
    for (auto& duck : ducker)
      duck.reset();
    leftGain.snapOnNextTarget();
    rightGain.snapOnNextTarget();
    crossShare.snapOnNextTarget();
    lowCut.snapOnNextTarget();
    highCut.snapOnNextTarget();
    grid.reset();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxChannels);
    lastBpm.store(ctx.transport.bpm, std::memory_order_relaxed);

    // Whole samples, as in the digital delay: a Hermite read at a whole delay
    // returns the stored sample, so repeats pass through untouched.
    const float leftTarget =
        std::round(static_cast<float>(leftSeconds(ctx.transport.bpm) * sampleRate));
    const float rightTarget =
        std::round(static_cast<float>(rightSeconds(ctx.transport.bpm) * sampleRate));
    for (int ch = 0; ch < kMaxChannels; ++ch) {
      taps[left(ch)].setTarget(leftTarget);
      taps[right(ch)].setTarget(rightTarget);
    }
    leftGain.setTarget(param(kLeftFeedback) * 0.01f);
    rightGain.setTarget(param(kRightFeedback) * 0.01f);
    crossShare.setTarget(param(kCrossFeedback) * 0.01f);
    lowCut.setTarget(param(kLowCutHz));
    highCut.setTarget(param(kHighCutHz));
    for (auto& duck : ducker)
      duck.setTarget(param(kDuckDb), param(kDuckReleaseMs));

    const bool image = blockHasStereoImage(buffer.getNumChannels(), ctx);
    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    for (int i = 0; i < numSamples; ++i) {
      const float leftFeedback = leftGain.next();
      const float rightFeedback = rightGain.next();
      const float cross = crossShare.next();
      const float low = lowCut.next();
      const float high = highCut.next();
      if (grid.due()) {
        const int used = image ? 2 : numChannels * 2;
        for (int n = 0; n < used; ++n) {
          lowCutFilter[n].setCutoff(low, sampleRate);
          highCutFilter[n][0].setCutoff(high, sampleRate);
          highCutFilter[n][1].setCutoff(high, sampleRate);
        }
      }

      if (image) {
        // Lines 0 (left) and 1 (right) are the two delays; the left input
        // feeds the left one, the right input the right one.
        const float inLeft = channels[0][i];
        const float inRight = channels[1][i];
        float echoLeft, echoRight;
        step(0, 1, inLeft, inRight, leftFeedback, rightFeedback, cross, echoLeft, echoRight);
        // One gain from the louder side, so the image does not lean.
        const float duck = ducker[0].next(inLeft, inRight);
        channels[0][i] = echoLeft * duck;
        channels[1][i] = echoRight * duck;
        continue;
      }

      // Each channel is a pair of delays on its own input, heard together.
      for (int ch = 0; ch < numChannels; ++ch) {
        const float input = channels[ch][i];
        float echoLeft, echoRight;
        step(left(ch), right(ch), input, input, leftFeedback, rightFeedback, cross, echoLeft,
             echoRight);
        channels[ch][i] = 0.5f * (echoLeft + echoRight) * ducker[ch].next(input, input);
      }
    }
  }

private:
  static constexpr int kLines = 2 * kMaxChannels;
  static constexpr double kFadeSeconds = 0.04;

  // Line numbers: channel `ch`'s left and right delay.
  static int left(int ch) noexcept { return 2 * ch; }
  static int right(int ch) noexcept { return 2 * ch + 1; }

  float readEcho(int n) noexcept {
    const CrossfadeTap::Taps t = taps[n].next();
    float echo = line.readHermite(n, t.from);
    if (t.toWeight > 0.0f)
      echo += t.toWeight * (line.readHermite(n, t.to) - echo);
    return echo;
  }

  /** One sample of a pair of delays (lines `a` and `b`). Each side's filtered
      repeat goes back with its own feedback, split between its own line and
      the other by `cross`; the column sums of that split are the two
      feedbacks, so the loop never gains. Only the repeats are filtered: the
      echoes returned are the clean copies. */
  void step(int a, int b, float inA, float inB, float feedbackA, float feedbackB, float cross,
            float& echoA, float& echoB) noexcept {
    echoA = readEcho(a);
    echoB = readEcho(b);
    const float fedA = highCutFilter[a][1].lowpass(highCutFilter[a][0].lowpass(
        lowCutFilter[a].highpass(echoA)));
    const float fedB = highCutFilter[b][1].lowpass(highCutFilter[b][0].lowpass(
        lowCutFilter[b].highpass(echoB)));
    const float stay = 1.0f - cross;
    line.write(a, softLimit(inA + feedbackA * stay * fedA + feedbackB * cross * fedB));
    line.write(b, softLimit(inB + feedbackB * stay * fedB + feedbackA * cross * fedA));
  }

  double leftSeconds(double bpm) const noexcept { return timeSeconds(kLeftSync, bpm); }
  double rightSeconds(double bpm) const noexcept { return timeSeconds(kRightSync, bpm); }

  // `first` is the index of a side's sync parameter; division and the free
  // time follow it.
  double timeSeconds(int first, double bpm) const noexcept {
    BlockTransport transport;
    transport.bpm = bpm;
    return juce::jmin(kMaxDelaySeconds,
                      fx::resolveTimeSeconds(param(first), param(first + 1), param(first + 2),
                                             transport));
  }

  fx::DelayLine line;
  CrossfadeTap taps[kLines];
  fx::OnePole lowCutFilter[kLines];
  fx::OnePole highCutFilter[kLines][2];
  fx::SmoothedParam leftGain;
  fx::SmoothedParam rightGain;
  fx::SmoothedParam crossShare;
  fx::SmoothedParam lowCut;
  fx::SmoothedParam highCut;
  Ducker ducker[kMaxChannels];
  ControlTick grid;
  double sampleRate{48000.0};
  std::atomic<double> lastBpm{120.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(DualDelay) {
  // Heard beside the dry signal (see DigitalDelay): 30% to start.
  addEffectKind<DualDelayProcessor>(registry, "Dual Delay", BlockCategory::Delay, 0.30f,
                                    BlockMixLaw::DryPreserving);
}

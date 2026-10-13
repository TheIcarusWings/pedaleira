// delay.pingPong: repeats that bounce between the left and the right side.
// In a true stereo image the left line is fed by the input and by the right
// line, and the right line by the left one, so successive repeats alternate
// sides; `spread` blends from independent left and right delays (0) to the
// full bounce (100%), and `ratio` makes the right line shorter than the left
// (a dotted or golden-ratio pattern). Everywhere else (a mono buffer, dual
// mono, a stereo image that an amp model later in the lane would throw away)
// it is an ordinary delay per channel with the left time, and nothing crosses
// between channels.
#include <atomic>
#include <cmath>
#include "DelayCommon.h"
#include "effects/EffectDsp.h"

namespace {

using namespace delayfx;

class PingPongDelayProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "delay.pingpong";

  enum Param { kSync = 0, kDivision, kTimeMs, kFeedback, kSpread, kRatio, kToneHz, kDuckDb, kDuckReleaseMs, kSpillover };

  // The free time stops at 2 s; a synced time that would be longer is held
  // at this length.
  static constexpr double kMaxDelaySeconds = 4.0;

  // The right line as a share of the left, per `ratio` choice. None is above
  // 1, so the left time bounds every trip round the loop.
  static float rightShare(int choice) noexcept {
    switch (choice) {
      case 1: return 0.75f;        // dotted: the right is three quarters of the left
      case 2: return 0.618034f;    // golden: the right is 1 / phi of the left
      default: return 1.0f;
    }
  }

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      fx::TimeParamOptions time;
      time.minMs = 1.0f;
      time.maxMs = 2000.0f;
      time.defaultMs = 375.0f;
      time.affectsTail = true;
      fx::appendTimeParams(t, time);
      t.push_back(BlockParamSpec::floatParam("feedback", "Feedback", "%", 0.0f, 95.0f, 40.0f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("spread", "Spread", "%", 0.0f, 100.0f, 100.0f));
      t.push_back(BlockParamSpec::choiceParam("ratio", "Ratio", {"1:1", "Dotted", "Golden"}, 0));
      t.push_back(BlockParamSpec::floatParam("toneHz", "Tone", "Hz", 500.0f, 20000.0f, 8000.0f)
                      .withSkew(0.4f));
      appendDuckParams(t);
      t.push_back(spilloverParam());
      return t;
    }();
    return table;
  }

  PingPongDelayProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // Every hop, left to right or back, applies the feedback once, whatever the
  // spread, and takes at most the left time; the tone low-pass and the
  // limiter only lower the gain. A crossfade in progress adds its length.
  double tailSeconds() const noexcept override {
    const double feedback = static_cast<double>(param(kFeedback)) * 0.01;
    return feedbackTailSeconds(delaySeconds(lastBpm.load(std::memory_order_relaxed)), feedback,
                               lowpassDelaySeconds(static_cast<double>(param(kToneHz)), 1),
                               kFadeSeconds + 0.01);
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    line.prepare(kMaxChannels, static_cast<int>(std::ceil(kMaxDelaySeconds * sampleRate)) + 16);
    for (auto& tap : taps)
      tap.prepare(sampleRate, kFadeSeconds);
    feedbackGain.prepare(sampleRate, 0.02);
    spreadAmount.prepare(sampleRate, 0.03);
    toneCutoff.prepare(sampleRate, 0.02);
    for (auto& duck : ducker)
      duck.prepare(sampleRate);
    resetEffect();
  }

  void resetEffect() noexcept override {
    line.clear();
    for (int ch = 0; ch < kMaxChannels; ++ch) {
      tone[ch].reset();
      taps[ch].snapOnNextTarget();
    }
    feedbackGain.snapOnNextTarget();
    spreadAmount.snapOnNextTarget();
    toneCutoff.snapOnNextTarget();
    grid.reset();
    for (auto& duck : ducker)
      duck.reset();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxChannels);
    lastBpm.store(ctx.transport.bpm, std::memory_order_relaxed);

    // Only a true stereo image bounces. The left line keeps the set time in
    // every layout; the right line takes the ratio only when it is a line of
    // its own in the bounce.
    const bool bounce = blockHasStereoImage(buffer.getNumChannels(), ctx);
    const float leftSamples =
        std::round(static_cast<float>(delaySeconds(ctx.transport.bpm) * sampleRate));
    taps[0].setTarget(leftSamples);
    taps[1].setTarget(bounce ? std::round(leftSamples * rightShare(static_cast<int>(
                                              std::lround(param(kRatio)))))
                             : leftSamples);
    feedbackGain.setTarget(param(kFeedback) * 0.01f);
    // Smoothed, so a layout change or a turn of the knob moves the bounce
    // gradually instead of switching it.
    spreadAmount.setTarget(bounce ? param(kSpread) * 0.01f : 0.0f);
    toneCutoff.setTarget(param(kToneHz));
    for (auto& duck : ducker)
      duck.setTarget(param(kDuckDb), param(kDuckReleaseMs));

    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    for (int i = 0; i < numSamples; ++i) {
      const float feedback = feedbackGain.next();
      const float spread = spreadAmount.next();
      const float cutoff = toneCutoff.next();
      if (grid.due()) {
        for (int ch = 0; ch < numChannels; ++ch)
          tone[ch].setCutoff(cutoff, sampleRate);
      }

      if (!bounce) {
        // One line per channel, left time, no crossing: what a mono delay
        // does, and what a mono rig renders for each side of dual mono.
        for (int ch = 0; ch < numChannels; ++ch) {
          const float echo = readEcho(ch);
          const float input = channels[ch][i];
          line.write(ch, softLimit(input + feedback * tone[ch].lowpass(echo)));
          channels[ch][i] = echo * ducker[ch].next(input, input);
        }
        continue;
      }

      // Line 0 is the left, line 1 the right. At spread 1 the input feeds only
      // the left, each line feeds the other, and the feedback applies at
      // every hop, so repeat k is feedback^(k-1) of the first, on alternate
      // sides. At spread 0 each side is its own delay. In between the
      // feedback matrix mixes "own" and "other" and every row sums to the
      // feedback, so the loop gain never exceeds it.
      const float echoLeft = readEcho(0);
      const float echoRight = readEcho(1);
      const float fedLeft = tone[0].lowpass(echoLeft);
      const float fedRight = tone[1].lowpass(echoRight);
      const float inLeft = channels[0][i];
      const float inRight = channels[1][i];
      // The input is heard on the left as the bounce takes over, from the
      // mono sum, so it does not depend on which side it arrived on.
      const float inject = inLeft + spread * (0.5f * (inLeft + inRight) - inLeft);
      line.write(0, softLimit(inject + feedback * ((1.0f - spread) * fedLeft + spread * fedRight)));
      line.write(1, softLimit((1.0f - spread) * (inRight + feedback * fedRight) +
                              spread * feedback * fedLeft));
      // The ducking of a bounce is linked: one gain from the louder side, so
      // the image does not lean.
      const float duck = ducker[0].next(inLeft, inRight);
      channels[0][i] = echoLeft * duck;
      channels[1][i] = echoRight * duck;
    }
  }

private:
  static constexpr double kFadeSeconds = 0.04;

  // The tap's echo from its own line, crossfading when the time has moved.
  float readEcho(int ch) noexcept {
    const CrossfadeTap::Taps t = taps[ch].next();
    float echo = line.readHermite(ch, t.from);
    if (t.toWeight > 0.0f)
      echo += t.toWeight * (line.readHermite(ch, t.to) - echo);
    return echo;
  }

  double delaySeconds(double bpm) const noexcept {
    BlockTransport transport;
    transport.bpm = bpm;
    return juce::jmin(kMaxDelaySeconds, fx::resolveTimeSeconds(param(kSync), param(kDivision),
                                                               param(kTimeMs), transport));
  }

  fx::DelayLine line;
  CrossfadeTap taps[kMaxChannels];
  fx::OnePole tone[kMaxChannels];
  fx::SmoothedParam feedbackGain;
  fx::SmoothedParam spreadAmount;
  fx::SmoothedParam toneCutoff;
  ControlTick grid;
  Ducker ducker[kMaxChannels];
  double sampleRate{48000.0};
  std::atomic<double> lastBpm{120.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(PingPongDelay) {
  // Heard beside the dry signal (see DigitalDelay): 30% to start.
  addEffectKind<PingPongDelayProcessor>(registry, "Ping Pong Delay", BlockCategory::Delay, 0.30f,
                                        BlockMixLaw::DryPreserving);
}

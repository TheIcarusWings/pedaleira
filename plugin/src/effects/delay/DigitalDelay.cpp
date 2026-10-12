// delay.digital: clean repeats. One line per channel, a low cut and a high cut
// in the feedback path, and an optional slow modulation of the delay time (a
// subtle chorus that builds up on the repeats, because the modulated signal is
// what gets fed back).
#include <atomic>
#include <cmath>
#include "DelayCommon.h"
#include "effects/EffectDsp.h"

namespace {

using namespace delayfx;

class DigitalDelayProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "delay.digital";

  // Table order. The first three are the standard time parameters.
  enum Param {
    kSync = 0,
    kDivision,
    kTimeMs,
    kFeedback,
    kLowCutHz,
    kHighCutHz,
    kModDepth,
    kModRateHz,
    kSpillover
  };

  // The free time stops at 2 s; a synced time that would be longer (a dotted
  // whole note at a slow tempo) is held at this length.
  static constexpr double kMaxDelaySeconds = 4.0;
  // The delay swings by +-this at a modulation depth of 100%.
  static constexpr double kMaxModSeconds = 0.002;

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      fx::TimeParamOptions time;
      time.minMs = 1.0f;
      time.maxMs = 2000.0f;
      time.defaultMs = 350.0f;
      time.affectsTail = true;
      fx::appendTimeParams(t, time);
      t.push_back(BlockParamSpec::floatParam("feedback", "Feedback", "%", 0.0f, 95.0f, 35.0f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("lowCutHz", "Low Cut", "Hz", 20.0f, 2000.0f, 80.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("highCutHz", "High Cut", "Hz", 1000.0f, 20000.0f,
                                             8000.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("modDepth", "Mod Depth", "%", 0.0f, 100.0f, 0.0f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("modRateHz", "Mod Rate", "Hz", 0.05f, 10.0f, 0.8f)
                      .withSkew(0.4f));
      t.push_back(spilloverParam());
      return t;
    }();
    return table;
  }

  DigitalDelayProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // Time until the repeats are 60 dB down. The filters and the limiter only
  // lower the loop gain, so feedback alone sets the count; each trip round
  // the loop can take the modulation excursion and the high cut's group delay
  // on top of the delay, and a crossfade in progress adds its length once.
  // Read on the audio thread during a spillover release: atomics and
  // arithmetic only.
  double tailSeconds() const noexcept override {
    const double feedback = static_cast<double>(param(kFeedback)) * 0.01;
    const double excursion = static_cast<double>(param(kModDepth)) * 0.01 * kMaxModSeconds;
    const double filterDelay = lowpassDelaySeconds(static_cast<double>(param(kHighCutHz)), 2);
    return feedbackTailSeconds(delaySeconds(lastBpm.load(std::memory_order_relaxed)), feedback,
                               excursion + filterDelay, kFadeSeconds + 0.01);
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    // Room for the longest time plus the modulation swing.
    line.prepare(kMaxChannels, static_cast<int>(std::ceil((kMaxDelaySeconds + 0.01) * sampleRate)));
    for (auto& tap : taps)
      tap.prepare(sampleRate, kFadeSeconds);
    feedbackGain.prepare(sampleRate, 0.02);
    lowCut.prepare(sampleRate, 0.02);
    highCut.prepare(sampleRate, 0.02);
    modSamples.prepare(sampleRate, 0.05);
    lfo.prepare(sampleRate);
    resetEffect();
  }

  void resetEffect() noexcept override {
    line.clear();
    for (int ch = 0; ch < kMaxChannels; ++ch) {
      lowCutFilter[ch].reset();
      highCutFilter[ch][0].reset();
      highCutFilter[ch][1].reset();
      taps[ch].snapOnNextTarget();
    }
    feedbackGain.snapOnNextTarget();
    lowCut.snapOnNextTarget();
    highCut.snapOnNextTarget();
    modSamples.snapOnNextTarget();
    lfo.reset();
    grid.reset();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxChannels);
    lastBpm.store(ctx.transport.bpm, std::memory_order_relaxed);

    // Whole samples when nothing modulates: a Hermite read at a whole delay
    // returns the stored sample exactly, so unmodulated repeats pass through
    // the line untouched (clean) and the time is within half a sample of the
    // parameter.
    const float delayTarget =
        std::round(static_cast<float>(delaySeconds(ctx.transport.bpm) * sampleRate));
    for (int ch = 0; ch < numChannels; ++ch)
      taps[ch].setTarget(delayTarget);
    feedbackGain.setTarget(param(kFeedback) * 0.01f);
    lowCut.setTarget(param(kLowCutHz));
    highCut.setTarget(param(kHighCutHz));
    modSamples.setTarget(param(kModDepth) * 0.01f * static_cast<float>(kMaxModSeconds * sampleRate));
    lfo.setRateHz(static_cast<double>(param(kModRateHz)));

    // The right side runs a quarter turn behind the left, but only for a true
    // stereo image. Dual mono and mono modulate every channel alike, so each
    // is what a mono rig makes of it.
    const float rightPhase = blockHasStereoImage(buffer.getNumChannels(), ctx) ? 0.25f : 0.0f;

    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    for (int i = 0; i < numSamples; ++i) {
      const float feedback = feedbackGain.next();
      const float low = lowCut.next();
      const float high = highCut.next();
      const float depth = modSamples.next();
      if (grid.due()) {
        for (int ch = 0; ch < numChannels; ++ch) {
          lowCutFilter[ch].setCutoff(low, sampleRate);
          highCutFilter[ch][0].setCutoff(high, sampleRate);
          highCutFilter[ch][1].setCutoff(high, sampleRate);
        }
      }
      // The oscillator always advances, so its phase does not depend on
      // whether it was audible; the sines are only evaluated when it is.
      float swing[kMaxChannels] = {0.0f, 0.0f};
      if (depth != 0.0f) {
        swing[0] = depth * lfo.valueAt(0.0f);
        swing[1] = depth * lfo.valueAt(rightPhase);
      }
      lfo.advance();

      for (int ch = 0; ch < numChannels; ++ch) {
        const CrossfadeTap::Taps t = taps[ch].next();
        float echo = line.readHermite(ch, t.from + swing[ch]);
        if (t.toWeight > 0.0f)
          echo += t.toWeight * (line.readHermite(ch, t.to + swing[ch]) - echo);

        // Only the repeats are filtered: the first echo is the clean copy,
        // and each pass round the loop loses a little more at both ends.
        const float filtered =
            highCutFilter[ch][1].lowpass(highCutFilter[ch][0].lowpass(lowCutFilter[ch].highpass(echo)));
        // The ceiling leaves ordinary levels alone and keeps a hot input at
        // full feedback from running away.
        line.write(ch, softLimit(channels[ch][i] + feedback * filtered));
        // Wet only: the dry signal and the balance are the block's mix.
        channels[ch][i] = echo;
      }
    }
  }

private:
  // A change of time crossfades over this long: slow enough to be inaudible
  // as a step, short enough to follow a knob.
  static constexpr double kFadeSeconds = 0.04;

  double delaySeconds(double bpm) const noexcept {
    BlockTransport transport;
    transport.bpm = bpm;
    return juce::jmin(kMaxDelaySeconds, fx::resolveTimeSeconds(param(kSync), param(kDivision),
                                                               param(kTimeMs), transport));
  }

  fx::DelayLine line;
  CrossfadeTap taps[kMaxChannels];
  fx::OnePole lowCutFilter[kMaxChannels];
  fx::OnePole highCutFilter[kMaxChannels][2];
  fx::SmoothedParam feedbackGain;
  fx::SmoothedParam lowCut;
  fx::SmoothedParam highCut;
  fx::SmoothedParam modSamples;
  fx::Lfo lfo;
  ControlTick grid;
  double sampleRate{48000.0};
  std::atomic<double> lastBpm{120.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(DigitalDelay) {
  // Heard beside the dry signal: the dry stays at unity up to 50% mix, and a
  // new block brings the repeats in at 30% under it.
  addEffectKind<DigitalDelayProcessor>(registry, "Digital Delay", BlockCategory::Delay, 0.30f,
                                       BlockMixLaw::DryPreserving);
}

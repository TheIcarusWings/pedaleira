// delay.simple: one echo line per channel with feedback and a low-pass in the
// feedback path. The reference built-in effect: it is here to show the whole
// shape (table, tempo-synced time, smoothing, tail, spillover, registration)
// in the fewest lines, and docs/effects.md walks through it.
#include <atomic>
#include <cmath>
#include "effects/EffectDsp.h"

namespace {

class SimpleDelayProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "delay.simple";

  // Table order; the indices the audio path reads. The first three are the
  // standard time parameters (fx::appendTimeParams adds them in this order).
  enum Param { kSync = 0, kDivision, kTimeMs, kFeedback, kToneHz, kSpillover };

  // The longest delay the line can hold. The free time stops at 2 s; a
  // synced time that would be longer (a dotted whole note at a slow tempo)
  // is held at this length.
  static constexpr double kMaxDelaySeconds = 4.0;

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
      t.push_back(BlockParamSpec::floatParam("toneHz", "Tone", "Hz", 500.0f, 20000.0f, 6000.0f)
                      .withSkew(0.4f));
      // Echoes ring out when the block is bypassed.
      t.push_back(spilloverParam());
      return t;
    }();
    return table;
  }

  SimpleDelayProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // Time until the echoes are 60 dB down: the first one arrives a delay time
  // after the input stops, and each later one is `feedback` quieter. The
  // low-pass only shortens it, so this is the upper bound. Read on the audio
  // thread during a spillover release: atomics and arithmetic only.
  double tailSeconds() const noexcept override {
    const double feedback = static_cast<double>(param(kFeedback)) * 0.01;
    double echoes = 1.0;
    if (feedback > 0.0005)
      echoes += std::ceil(std::log(0.001) / std::log(feedback));
    // The glide of a time change in progress rides on top. Uncapped: the
    // chain stops what it reports at kMaxBlockTailSeconds.
    return delaySeconds(lastBpm.load(std::memory_order_relaxed)) * echoes + kTimeGlideSeconds;
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    line.prepare(juce::jmax(1, spec.numChannels),
                 static_cast<int>(std::ceil(kMaxDelaySeconds * sampleRate)) + 1);
    delaySamples.prepare(sampleRate, kTimeGlideSeconds);
    feedbackGain.prepare(sampleRate, 0.02);
    resetEffect();
  }

  void resetEffect() noexcept override {
    line.clear();
    for (auto& filter : damping)
      filter.reset();
    // A block switched back on starts at its current settings, not gliding
    // from where it was when it went quiet.
    delaySamples.snapOnNextTarget();
    feedbackGain.snapOnNextTarget();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), line.numChannels(), kMaxChannels);

    // The tempo the time follows when synced, kept for tailSeconds, which has
    // no context to read it from.
    lastBpm.store(ctx.transport.bpm, std::memory_order_relaxed);

    // A time change glides the read position (a short pitch bend, like
    // turning the knob on a tape echo) rather than jumping it.
    delaySamples.setTarget(static_cast<float>(delaySeconds(ctx.transport.bpm) * sampleRate));
    feedbackGain.setTarget(param(kFeedback) * 0.01f);
    for (int ch = 0; ch < numChannels; ++ch)
      damping[ch].setCutoff(param(kToneHz), sampleRate);

    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    for (int i = 0; i < numSamples; ++i) {
      const float delay = delaySamples.next();
      const float feedback = feedbackGain.next();
      // Each channel has its own line and filter: nothing crosses over, so
      // the two halves of a dual-mono signal stay apart.
      for (int ch = 0; ch < numChannels; ++ch) {
        // Hermite: exact at whole-sample delays, clean while the time glides.
        const float echo = line.readHermite(ch, delay);
        line.write(ch, channels[ch][i] + feedback * damping[ch].lowpass(echo));
        // Wet only: the dry signal and the balance are the block's mix.
        channels[ch][i] = echo;
      }
    }
  }

private:
  static constexpr int kMaxChannels = 2;
  static constexpr double kTimeGlideSeconds = 0.05;

  double delaySeconds(double bpm) const noexcept {
    BlockTransport transport;
    transport.bpm = bpm;
    return juce::jmin(kMaxDelaySeconds, fx::resolveTimeSeconds(param(kSync), param(kDivision),
                                                               param(kTimeMs), transport));
  }

  fx::DelayLine line;
  fx::OnePole damping[kMaxChannels];
  fx::SmoothedParam delaySamples;
  fx::SmoothedParam feedbackGain;
  double sampleRate{48000.0};
  std::atomic<double> lastBpm{120.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(SimpleDelay) {
  // The echoes are heard beside the dry signal, so the mix keeps the dry at
  // unity (up to 50%) and a new block brings them in at 35% under it.
  addEffectKind<SimpleDelayProcessor>(registry, "Simple Delay", BlockCategory::Delay, 0.35f,
                                      BlockMixLaw::DryPreserving);
}

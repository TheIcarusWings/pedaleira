// delay.multitap: one delay line read at four to eight points, so a note comes
// back as a cluster of echoes spread over the set time, the last of them at
// the full time. The pattern decides how the taps are spaced (even,
// quickening, slowing or swung), each tap a little quieter than the one
// before. The feedback is taken from the last tap, so the whole cluster
// repeats every `time`, and the repeats are diffused: on every trip round the
// loop they pass through a chain of allpass filters (blended in by the
// diffusion control) that smears each tap into a short burst, so the
// clusters melt together the more they repeat. A low cut and a high cut shape
// the repeats as they go round.
#include <array>
#include <atomic>
#include <cmath>
#include "DelayCommon.h"
#include "effects/EffectDsp.h"
#include "effects/reverb/ReverbParts.h"

namespace {

using namespace delayfx;

class MultiTapDelayProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "delay.multitap";

  enum Param {
    kSync = 0,
    kDivision,
    kTimeMs,
    kTaps,
    kPattern,
    kDiffusion,
    kFeedback,
    kLowCutHz,
    kHighCutHz,
    kDuckDb,
    kDuckReleaseMs,
    kSpillover
  };

  static constexpr int kMaxTaps = 8;
  // The free time stops at 2 s; a synced time that would be longer is held
  // at this length.
  static constexpr double kMaxDelaySeconds = 4.0;

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      fx::TimeParamOptions time;
      time.minMs = 20.0f;
      time.maxMs = 2000.0f;
      time.defaultMs = 500.0f;
      time.affectsTail = true;
      fx::appendTimeParams(t, time);
      t.push_back(BlockParamSpec::floatParam("taps", "Taps", "taps", 4.0f, 8.0f, 6.0f)
                      .withStep(1.0f));
      t.push_back(BlockParamSpec::choiceParam("pattern", "Pattern",
                                              {"Even", "Accelerating", "Decelerating", "Swing"}, 0));
      t.push_back(BlockParamSpec::floatParam("diffusion", "Diffusion", "%", 0.0f, 100.0f, 30.0f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("feedback", "Feedback", "%", 0.0f, 95.0f, 30.0f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("lowCutHz", "Low Cut", "Hz", 20.0f, 800.0f, 80.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("highCutHz", "High Cut", "Hz", 1000.0f, 16000.0f,
                                             7000.0f)
                      .withSkew(0.4f)
                      .withAffectsTail());
      appendDuckParams(t);
      t.push_back(spilloverParam());
      return t;
    }();
    return table;
  }

  MultiTapDelayProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The loop is the last tap, at the full time, with the diffusers' delay and
  // the cut filters' group delay on top; the filters and the limiter only
  // lower the gain. A glide of the time in progress rides on top once.
  double tailSeconds() const noexcept override {
    const double feedback = static_cast<double>(param(kFeedback)) * 0.01;
    const double pass = std::max(delaySeconds(lastBpm.load(std::memory_order_relaxed)),
                                 static_cast<double>(slewedDelay.load(std::memory_order_relaxed)) /
                                     sampleRate);
    return feedbackTailSeconds(pass, feedback,
                               kDiffuserSeconds +
                                   lowpassDelaySeconds(static_cast<double>(param(kHighCutHz)), 2),
                               0.1);
  }

  /** Where the taps sit, as fractions of the time (the last is 1), for
      `count` taps in pattern `pattern`: the cumulative sum of the gaps, each
      gap weighted by the pattern. */
  static void tapPositions(int pattern, int count, float* positions) noexcept {
    float gaps[kMaxTaps];
    float total = 0.0f;
    for (int k = 0; k < count; ++k) {
      switch (pattern) {
        case 1: gaps[k] = std::pow(0.72f, static_cast<float>(k)); break;                  // quickening
        case 2: gaps[k] = std::pow(0.72f, static_cast<float>(count - 1 - k)); break;      // slowing
        case 3: gaps[k] = (k % 2 == 0) ? 2.0f : 1.0f; break;                              // swing
        default: gaps[k] = 1.0f; break;
      }
      total += gaps[k];
    }
    float run = 0.0f;
    for (int k = 0; k < count; ++k) {
      run += gaps[k];
      positions[k] = run / total;
    }
    positions[count - 1] = 1.0f;
  }

  /** The level of tap `k` (0 based) of `count`: each 0.8 of the one before,
      the set scaled so its energy is 1, which keeps the loudness about the
      same whatever the count. */
  static float tapGain(int k, int count) noexcept {
    float energy = 0.0f;
    for (int j = 0; j < count; ++j)
      energy += std::pow(0.64f, static_cast<float>(j));
    return std::pow(0.8f, static_cast<float>(k)) / std::sqrt(energy);
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    line.prepare(kMaxChannels, static_cast<int>(std::ceil((kMaxDelaySeconds + 0.01) * sampleRate)));
    // The time moves by slewing the pointers, as on a tape echo: every tap
    // bends in pitch together, with no click.
    slew.prepare(sampleRate, 0.08, 0.25);
    for (int k = 0; k < kMaxTaps; ++k) {
      position[k].prepare(sampleRate, 0.1);
      level[k].prepare(sampleRate, 0.03);
    }
    feedbackGain.prepare(sampleRate, 0.02);
    diffusionAmount.prepare(sampleRate, 0.03);
    lowCut.prepare(sampleRate, 0.02);
    highCut.prepare(sampleRate, 0.02);
    static constexpr std::array<double, 4> kSeconds = {0.0043, 0.0067, 0.0091, 0.0129};
    static constexpr std::array<float, 4> kCoefficients = {0.6f, 0.6f, 0.6f, 0.6f};
    for (auto& chain : diffuser)
      chain.prepare(kSeconds.data(), kCoefficients.data(), 4, sampleRate);
    for (auto& duck : ducker)
      duck.prepare(sampleRate);
    resetEffect();
  }

  void resetEffect() noexcept override {
    line.clear();
    for (int ch = 0; ch < kMaxChannels; ++ch) {
      lowCutFilter[ch].reset();
      highCutFilter[ch][0].reset();
      highCutFilter[ch][1].reset();
      diffuser[ch].clear();
      ducker[ch].reset();
    }
    slew.snapOnNextTarget();
    for (int k = 0; k < kMaxTaps; ++k) {
      position[k].snapOnNextTarget();
      level[k].snapOnNextTarget();
    }
    feedbackGain.snapOnNextTarget();
    diffusionAmount.snapOnNextTarget();
    lowCut.snapOnNextTarget();
    highCut.snapOnNextTarget();
    grid.reset();
    slewedDelay.store(0.0f, std::memory_order_relaxed);
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxChannels);
    lastBpm.store(ctx.transport.bpm, std::memory_order_relaxed);

    slew.setTarget(static_cast<float>(delaySeconds(ctx.transport.bpm) * sampleRate));
    const int count = juce::jlimit(4, kMaxTaps, static_cast<int>(std::lround(param(kTaps))));
    float where[kMaxTaps];
    tapPositions(static_cast<int>(std::lround(param(kPattern))), count, where);
    for (int k = 0; k < kMaxTaps; ++k) {
      // Taps beyond the count are silent and rest on the last position.
      position[k].setTarget(k < count ? where[k] : 1.0f);
      level[k].setTarget(k < count ? tapGain(k, count) : 0.0f);
    }
    feedbackGain.setTarget(param(kFeedback) * 0.01f);
    diffusionAmount.setTarget(param(kDiffusion) * 0.01f);
    lowCut.setTarget(param(kLowCutHz));
    highCut.setTarget(param(kHighCutHz));
    for (auto& duck : ducker)
      duck.setTarget(param(kDuckDb), param(kDuckReleaseMs));

    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    float delay = slew.current();
    for (int i = 0; i < numSamples; ++i) {
      delay = slew.next();
      const float feedback = feedbackGain.next();
      const float diffusion = diffusionAmount.next();
      const float low = lowCut.next();
      const float high = highCut.next();
      float at[kMaxTaps];
      float gain[kMaxTaps];
      for (int k = 0; k < kMaxTaps; ++k) {
        at[k] = position[k].next() * delay;
        gain[k] = level[k].next();
      }
      if (grid.due()) {
        for (int ch = 0; ch < numChannels; ++ch) {
          lowCutFilter[ch].setCutoff(low, sampleRate);
          highCutFilter[ch][0].setCutoff(high, sampleRate);
          highCutFilter[ch][1].setCutoff(high, sampleRate);
        }
      }

      for (int ch = 0; ch < numChannels; ++ch) {
        // The cluster: every tap that is audible.
        float echo = 0.0f;
        for (int k = 0; k < kMaxTaps; ++k)
          if (gain[k] != 0.0f)
            echo += gain[k] * line.readHermite(ch, at[k]);
        // The loop reads the full time, whatever the taps do.
        const float last = line.readHermite(ch, delay);
        // The repeats only: diffused (blended in, so 0 leaves them exact),
        // then cut at both ends. Run always, so the diffuser's memory is
        // never stale when the control is turned up.
        const float smeared = diffuser[ch].process(last);
        const float diffused = last + diffusion * (smeared - last);
        const float fed = highCutFilter[ch][1].lowpass(
            highCutFilter[ch][0].lowpass(lowCutFilter[ch].highpass(diffused)));
        const float input = channels[ch][i];
        line.write(ch, softLimit(input + feedback * fed));
        // Wet only: the dry signal and the balance are the block's mix. The
        // ducking lowers it while the input is loud.
        channels[ch][i] = echo * ducker[ch].next(input, input);
      }
    }
    slewedDelay.store(delay, std::memory_order_relaxed);
  }

private:
  // The four allpasses' delays add up to this.
  static constexpr double kDiffuserSeconds = 0.0330;

  double delaySeconds(double bpm) const noexcept {
    BlockTransport transport;
    transport.bpm = bpm;
    return juce::jmin(kMaxDelaySeconds, fx::resolveTimeSeconds(param(kSync), param(kDivision),
                                                               param(kTimeMs), transport));
  }

  fx::DelayLine line;
  DelaySlew slew;
  fx::SmoothedParam position[kMaxTaps];
  fx::SmoothedParam level[kMaxTaps];
  fx::OnePole lowCutFilter[kMaxChannels];
  fx::OnePole highCutFilter[kMaxChannels][2];
  fx::reverb::AllpassChain diffuser[kMaxChannels];
  fx::SmoothedParam feedbackGain;
  fx::SmoothedParam diffusionAmount;
  fx::SmoothedParam lowCut;
  fx::SmoothedParam highCut;
  Ducker ducker[kMaxChannels];
  ControlTick grid;
  double sampleRate{48000.0};
  std::atomic<double> lastBpm{120.0};
  std::atomic<float> slewedDelay{0.0f};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(MultiTapDelay) {
  // Heard beside the dry signal (see DigitalDelay): 30% to start.
  addEffectKind<MultiTapDelayProcessor>(registry, "Multi-Tap", BlockCategory::Delay, 0.30f,
                                        BlockMixLaw::DryPreserving);
}

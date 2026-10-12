// delay.analog: a bucket-brigade echo. Every pass through the line goes through
// the same low-pass pair a BBD needs to hide its clock, and through a gentle
// saturator that stands in for the compander's squash, so the repeats get
// darker and a little dirtier each time round. The low-pass follows the delay
// time the way a real clock does (a longer time means a slower clock, so a
// lower cutoff), and a change of time bends the pitch because the read pointer
// slews to its new place instead of jumping.
#include <atomic>
#include <cmath>
#include "DelayCommon.h"
#include "effects/EffectDsp.h"

namespace {

using namespace delayfx;

class AnalogDelayProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "delay.analog";

  enum Param { kSync = 0, kDivision, kTimeMs, kFeedback, kToneHz, kGrit, kSpillover };

  // The free time stops at 1.2 s, as a bucket-brigade chip does at a usable
  // clock rate. A synced time that would be longer is held at twice that.
  static constexpr double kMaxDelaySeconds = 2.4;

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      fx::TimeParamOptions time;
      time.minMs = 10.0f;
      time.maxMs = 1200.0f;
      time.defaultMs = 300.0f;
      time.affectsTail = true;
      fx::appendTimeParams(t, time);
      t.push_back(BlockParamSpec::floatParam("feedback", "Feedback", "%", 0.0f, 95.0f, 40.0f)
                      .withAffectsTail());
      // The low-pass cutoff at a 300 ms delay; it scales with the delay time.
      t.push_back(BlockParamSpec::floatParam("toneHz", "Tone", "Hz", 800.0f, 12000.0f, 4000.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("grit", "Grit", "%", 0.0f, 100.0f, 30.0f));
      t.push_back(spilloverParam());
      return t;
    }();
    return table;
  }

  AnalogDelayProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The loop filters and the saturator only lower the gain, so feedback sets
  // the count. The pass is the longer of the target and where the slewing
  // pointer is now, with the low-pass pair's group delay on top.
  double tailSeconds() const noexcept override {
    const double feedback = static_cast<double>(param(kFeedback)) * 0.01;
    const double pass = std::max(delaySeconds(lastBpm.load(std::memory_order_relaxed)),
                                 static_cast<double>(slewedDelay.load(std::memory_order_relaxed)) /
                                     sampleRate);
    return feedbackTailSeconds(pass, feedback,
                               lowpassDelaySeconds(cutoffFor(param(kToneHz), pass), 2), 0.05);
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    line.prepare(kMaxChannels, static_cast<int>(std::ceil(kMaxDelaySeconds * sampleRate)) + 16);
    // Chases the target with a 100 ms time constant, at most 0.3 samples of
    // delay per sample: a pitch bend of about 6 semitones down while it grows
    // and 4.5 up while it shrinks, at its most.
    slew.prepare(sampleRate, 0.1, 0.3);
    feedbackGain.prepare(sampleRate, 0.02);
    tone.prepare(sampleRate, 0.02);
    grit.prepare(sampleRate, 0.02);
    resetEffect();
  }

  void resetEffect() noexcept override {
    line.clear();
    for (int ch = 0; ch < kMaxChannels; ++ch) {
      coupling[ch].reset();
      reconstruction[ch][0].reset();
      reconstruction[ch][1].reset();
      coupling[ch].setCutoff(kCouplingHz, sampleRate);
    }
    slew.snapOnNextTarget();
    feedbackGain.snapOnNextTarget();
    tone.snapOnNextTarget();
    grit.snapOnNextTarget();
    grid.reset();
    slewedDelay.store(0.0f, std::memory_order_relaxed);
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxChannels);
    lastBpm.store(ctx.transport.bpm, std::memory_order_relaxed);

    slew.setTarget(static_cast<float>(delaySeconds(ctx.transport.bpm) * sampleRate));
    feedbackGain.setTarget(param(kFeedback) * 0.01f);
    tone.setTarget(param(kToneHz));
    grit.setTarget(param(kGrit) * 0.01f);

    // Both channels are processed alike (there is no stereo image to make),
    // each with its own line and filters, so dual mono stays apart.
    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    float delay = slew.current();
    for (int i = 0; i < numSamples; ++i) {
      delay = slew.next();
      const float feedback = feedbackGain.next();
      const float toneHz = tone.next();
      const float drive = 1.0f + 3.0f * grit.next();
      const float inverseDrive = 1.0f / drive;
      if (grid.due()) {
        const float cutoff =
            static_cast<float>(cutoffFor(toneHz, static_cast<double>(delay) / sampleRate));
        for (int ch = 0; ch < numChannels; ++ch) {
          reconstruction[ch][0].setCutoff(cutoff, sampleRate);
          reconstruction[ch][1].setCutoff(cutoff, sampleRate);
        }
      }

      for (int ch = 0; ch < numChannels; ++ch) {
        // The read pointer moves while the time glides: Hermite stays clean.
        const float raw = line.readHermite(ch, delay);
        // Every echo, the first included, leaves through the low-pass pair,
        // so a repeat has been through it as many times as it has passes.
        const float echo = reconstruction[ch][1].lowpass(reconstruction[ch][0].lowpass(raw));
        // The coupling high-pass keeps a stray offset from piling up in the
        // loop (1 / (1 - feedback) at DC).
        const float fed = coupling[ch].highpass(echo);
        line.write(ch, saturate(channels[ch][i] + feedback * fed, drive, inverseDrive));
        // Wet only: the dry signal and the balance are the block's mix.
        channels[ch][i] = echo;
      }
    }
    slewedDelay.store(delay, std::memory_order_relaxed);
  }

private:
  // Reference delay for the tone control, and the window the cutoff may move in.
  static constexpr double kReferenceSeconds = 0.3;
  static constexpr double kCouplingHz = 25.0;

  /** The low-pass cutoff for a tone setting at a given delay. A chip's corner
      follows its clock, so it falls in proportion to the delay time; the
      corner here goes with the square root of it, a gentler law that keeps a
      1 s delay usable (the proportional one would take 4 kHz at 300 ms down
      to 1.2 kHz at 1 s). */
  static double cutoffFor(double toneHz, double delaySeconds) noexcept {
    return juce::jlimit(300.0, 16000.0,
                        toneHz * std::sqrt(kReferenceSeconds / std::max(0.02, delaySeconds)));
  }

  double delaySeconds(double bpm) const noexcept {
    BlockTransport transport;
    transport.bpm = bpm;
    return juce::jmin(kMaxDelaySeconds, fx::resolveTimeSeconds(param(kSync), param(kDivision),
                                                               param(kTimeMs), transport));
  }

  fx::DelayLine line;
  DelaySlew slew;
  fx::OnePole reconstruction[kMaxChannels][2];
  fx::OnePole coupling[kMaxChannels];
  fx::SmoothedParam feedbackGain;
  fx::SmoothedParam tone;
  fx::SmoothedParam grit;
  ControlTick grid;
  double sampleRate{48000.0};
  std::atomic<double> lastBpm{120.0};
  std::atomic<float> slewedDelay{0.0f};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(AnalogDelay) {
  // Heard beside the dry signal (see DigitalDelay): 30% to start.
  addEffectKind<AnalogDelayProcessor>(registry, "Analog Delay", BlockCategory::Delay, 0.30f,
                                      BlockMixLaw::DryPreserving);
}

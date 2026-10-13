// delay.slapback: the short single echo of an old rockabilly record. One line
// per channel, a time of 40 to 200 ms, a few repeats at most, a low-pass sweep
// for the tone, and a touch of tape in it: the signal is saturated on its way
// into the line and the delay wobbles a little (a slow wow and a quicker
// flutter, common to both channels as one transport would make them). A change
// of time slews the read pointer, so the echo bends in pitch like a tape
// machine being re-timed rather than jumping.
#include <atomic>
#include <cmath>
#include "DelayCommon.h"
#include "effects/EffectDsp.h"

namespace {

using namespace delayfx;

class SlapbackDelayProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "delay.slapback";

  enum Param {
    kSync = 0,
    kDivision,
    kTimeMs,
    kFeedback,
    kToneHz,
    kSaturation,
    kWobble,
    kDuckDb,
    kDuckReleaseMs,
    kSpillover
  };

  // The free time stops at 200 ms; a synced time that would be longer (a
  // quarter note at a slow tempo) is held at this length.
  static constexpr double kMaxDelaySeconds = 1.0;
  // The pitch swing of the wobble at 100%, as a fraction of the pitch (peak).
  static constexpr double kWowDeviation = 0.006;
  static constexpr double kFlutterDeviation = 0.0025;
  static constexpr double kWowHz = 0.9;
  static constexpr double kFlutterHz = 6.1;

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      fx::TimeParamOptions time;
      time.minMs = 40.0f;
      time.maxMs = 200.0f;
      time.defaultMs = 110.0f;
      time.affectsTail = true;
      fx::appendTimeParams(t, time);
      t.push_back(BlockParamSpec::floatParam("feedback", "Feedback", "%", 0.0f, 60.0f, 15.0f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("toneHz", "Tone", "Hz", 500.0f, 16000.0f, 5500.0f)
                      .withSkew(0.4f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("saturation", "Saturation", "%", 0.0f, 100.0f, 25.0f));
      t.push_back(BlockParamSpec::floatParam("wobble", "Wobble", "%", 0.0f, 100.0f, 20.0f)
                      .withAffectsTail());
      appendDuckParams(t);
      t.push_back(spilloverParam());
      return t;
    }();
    return table;
  }

  SlapbackDelayProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The low-pass pair and the saturator only lower the loop gain, so the
  // feedback sets the count of repeats. Each trip round can take the wobble's
  // swing and the tone filter's group delay on top of the delay; a glide in
  // progress rides on top once.
  double tailSeconds() const noexcept override {
    const double feedback = static_cast<double>(param(kFeedback)) * 0.01;
    const double pass = std::max(delaySeconds(lastBpm.load(std::memory_order_relaxed)),
                                 static_cast<double>(slewedDelay.load(std::memory_order_relaxed)) /
                                     sampleRate);
    return feedbackTailSeconds(pass, feedback,
                               wobbleSwingSeconds() +
                                   lowpassDelaySeconds(static_cast<double>(param(kToneHz)), 2),
                               0.05);
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    // Room for the longest time plus the wobble's widest swing.
    line.prepare(kMaxChannels,
                 static_cast<int>(std::ceil((kMaxDelaySeconds + 0.03) * sampleRate)) + 16);
    slew.prepare(sampleRate, 0.1, 0.3);
    feedbackGain.prepare(sampleRate, 0.02);
    tone.prepare(sampleRate, 0.02);
    saturationAmount.prepare(sampleRate, 0.02);
    wowSamples.prepare(sampleRate, 0.05);
    flutterSamples.prepare(sampleRate, 0.05);
    wowLfo.prepare(sampleRate);
    flutterLfo.prepare(sampleRate);
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
    slew.snapOnNextTarget();
    feedbackGain.snapOnNextTarget();
    tone.snapOnNextTarget();
    saturationAmount.snapOnNextTarget();
    wowSamples.snapOnNextTarget();
    flutterSamples.snapOnNextTarget();
    // Out of step, so the wobble does not begin as a pure sine of either.
    wowLfo.reset(0.0);
    flutterLfo.reset(0.3);
    grid.reset();
    slewedDelay.store(0.0f, std::memory_order_relaxed);
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxChannels);
    lastBpm.store(ctx.transport.bpm, std::memory_order_relaxed);

    const double twoPi = juce::MathConstants<double>::twoPi;
    const double wobble = static_cast<double>(param(kWobble)) * 0.01;
    slew.setTarget(static_cast<float>(delaySeconds(ctx.transport.bpm) * sampleRate));
    feedbackGain.setTarget(param(kFeedback) * 0.01f);
    tone.setTarget(param(kToneHz));
    saturationAmount.setTarget(param(kSaturation) * 0.01f);
    // The delay excursions that give the set pitch deviations at these rates.
    wowSamples.setTarget(
        static_cast<float>(kWowDeviation * wobble * sampleRate / (twoPi * kWowHz)));
    flutterSamples.setTarget(
        static_cast<float>(kFlutterDeviation * wobble * sampleRate / (twoPi * kFlutterHz)));
    wowLfo.setRateHz(kWowHz);
    flutterLfo.setRateHz(kFlutterHz);
    for (auto& duck : ducker)
      duck.setTarget(param(kDuckDb), param(kDuckReleaseMs));

    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    float delay = slew.current();
    for (int i = 0; i < numSamples; ++i) {
      delay = slew.next();
      const float feedback = feedbackGain.next();
      const float toneHz = tone.next();
      const float drive = 1.0f + 2.0f * saturationAmount.next();
      const float inverseDrive = 1.0f / drive;
      const float wow = wowSamples.next();
      const float flutter = flutterSamples.next();
      if (grid.due()) {
        for (int ch = 0; ch < numChannels; ++ch) {
          toneFilter[ch][0].setCutoff(toneHz, sampleRate);
          toneFilter[ch][1].setCutoff(toneHz, sampleRate);
        }
      }
      // The oscillators advance every sample, audible or not.
      const float wowValue = wowLfo.next();
      const float flutterValue = flutterLfo.next();
      float position = delay;
      if (wow != 0.0f || flutter != 0.0f)
        position += wow * wowValue + flutter * flutterValue;

      for (int ch = 0; ch < numChannels; ++ch) {
        // The pointer moves while the time glides or wobbles: Hermite.
        const float raw = line.readHermite(ch, position);
        // Every echo leaves through the tone filter, so a repeat has been
        // through it once per pass.
        const float echo = toneFilter[ch][1].lowpass(toneFilter[ch][0].lowpass(raw));
        const float input = channels[ch][i];
        // The record stage saturates the input and the repeats alike, and is
        // the loop's ceiling.
        line.write(ch, saturate(input + feedback * echo, drive, inverseDrive));
        // Wet only: the dry signal and the balance are the block's mix. The
        // ducking lowers it while the input is loud.
        channels[ch][i] = echo * ducker[ch].next(input, input);
      }
    }
    slewedDelay.store(delay, std::memory_order_relaxed);
  }

private:
  /** The widest the wobble moves the delay, in seconds, at the current
      setting (the sum of its two parts' peak excursions). */
  double wobbleSwingSeconds() const noexcept {
    const double twoPi = juce::MathConstants<double>::twoPi;
    const double wobble = static_cast<double>(param(kWobble)) * 0.01;
    return wobble * (kWowDeviation / (twoPi * kWowHz) + kFlutterDeviation / (twoPi * kFlutterHz));
  }

  double delaySeconds(double bpm) const noexcept {
    BlockTransport transport;
    transport.bpm = bpm;
    return juce::jmin(kMaxDelaySeconds, fx::resolveTimeSeconds(param(kSync), param(kDivision),
                                                               param(kTimeMs), transport));
  }

  fx::DelayLine line;
  DelaySlew slew;
  fx::OnePole toneFilter[kMaxChannels][2];
  fx::SmoothedParam feedbackGain;
  fx::SmoothedParam tone;
  fx::SmoothedParam saturationAmount;
  fx::SmoothedParam wowSamples;
  fx::SmoothedParam flutterSamples;
  fx::Lfo wowLfo;
  fx::Lfo flutterLfo;
  Ducker ducker[kMaxChannels];
  ControlTick grid;
  double sampleRate{48000.0};
  std::atomic<double> lastBpm{120.0};
  std::atomic<float> slewedDelay{0.0f};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(SlapbackDelay) {
  // Heard beside the dry signal (see DigitalDelay): 30% to start.
  addEffectKind<SlapbackDelayProcessor>(registry, "Slapback", BlockCategory::Delay, 0.30f,
                                        BlockMixLaw::DryPreserving);
}

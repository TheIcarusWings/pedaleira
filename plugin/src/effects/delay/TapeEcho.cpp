// delay.tape: a tape echo. The delay is a tape loop, so the time wobbles
// (wow, flutter, and a little slow random drift), the signal is saturated on
// its way onto the tape, and every trip round the loop is band-limited by the
// heads: a low cut, a high cut and a head bump at the bottom, all of which
// worsen as the tape `age`s. Turning the time slews the read pointer, so the
// echoes bend in pitch like a transport changing speed. The head bump can
// push the loop gain above 1 near its frequency at high feedback; the
// saturator in the loop then holds it as a self-oscillation instead of
// letting it run away.
#include <atomic>
#include <cmath>
#include <cstdint>
#include "DelayCommon.h"
#include "effects/EffectDsp.h"

namespace {

using namespace delayfx;

class TapeEchoProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "delay.tape";

  enum Param {
    kSync = 0,
    kDivision,
    kTimeMs,
    kFeedback,
    kWow,
    kWowRateHz,
    kFlutter,
    kFlutterRateHz,
    kSaturation,
    kAge,
    kDuckDb,
    kDuckReleaseMs,
    kSpillover
  };

  // The free time stops at 2 s; a synced time that would be longer is held
  // at this length.
  static constexpr double kMaxDelaySeconds = 4.0;
  // The pitch swing at 100%, as a fraction of the pitch (peak). The delay
  // excursion that gives it is dev / (2 pi rate) seconds, so it shrinks as the
  // rate goes up.
  static constexpr double kWowDeviation = 0.012;
  static constexpr double kFlutterDeviation = 0.006;
  // The random drift as a share of the wow deviation, reckoned at 1 Hz.
  static constexpr double kRandomShare = 0.35;

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      fx::TimeParamOptions time;
      time.minMs = 10.0f;
      time.maxMs = 2000.0f;
      time.defaultMs = 400.0f;
      time.affectsTail = true;
      fx::appendTimeParams(t, time);
      t.push_back(BlockParamSpec::floatParam("feedback", "Feedback", "%", 0.0f, 95.0f, 40.0f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("wow", "Wow", "%", 0.0f, 100.0f, 25.0f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("wowRateHz", "Wow Rate", "Hz", 0.1f, 4.0f, 0.7f)
                      .withSkew(0.5f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("flutter", "Flutter", "%", 0.0f, 100.0f, 15.0f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("flutterRateHz", "Flutter Rate", "Hz", 3.0f, 15.0f,
                                             7.0f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("saturation", "Saturation", "%", 0.0f, 100.0f, 35.0f));
      // Fresh tape to worn: narrower at both ends, the head bump fading.
      t.push_back(BlockParamSpec::floatParam("age", "Age", "%", 0.0f, 100.0f, 30.0f)
                      .withAffectsTail());
      appendDuckParams(t);
      t.push_back(spilloverParam());
      return t;
    }();
    return table;
  }

  TapeEchoProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The loop's worst-case gain is the feedback times the head bump's peak
  // (the filters and the saturator are at most 1 elsewhere). At 1 or above the
  // loop can ring for ever, and the answer is "for ever": the chain caps it.
  // Each trip can also take the wobble's swing and the high cut's group delay
  // on top of the delay.
  double tailSeconds() const noexcept override {
    const double feedback = static_cast<double>(param(kFeedback)) * 0.01;
    const double age = static_cast<double>(param(kAge)) * 0.01;
    const double pass = std::max(delaySeconds(lastBpm.load(std::memory_order_relaxed)),
                                 static_cast<double>(slewedDelay.load(std::memory_order_relaxed)) /
                                     sampleRate);
    const double twoPi = juce::MathConstants<double>::twoPi;
    const double wowSwing = kWowDeviation * static_cast<double>(param(kWow)) * 0.01 /
                            (twoPi * static_cast<double>(param(kWowRateHz)));
    const double flutterSwing = kFlutterDeviation * static_cast<double>(param(kFlutter)) * 0.01 /
                                (twoPi * static_cast<double>(param(kFlutterRateHz)));
    const double randomSwing =
        kRandomShare * kWowDeviation * static_cast<double>(param(kWow)) * 0.01 / twoPi;
    return feedbackTailSeconds(pass, feedback * bumpGain(age),
                               wowSwing + flutterSwing + randomSwing +
                                   lowpassDelaySeconds(highCutHz(age), 2),
                               0.05);
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    // Room for the longest time plus the biggest wobble (wow at its slowest
    // and deepest swings about 20 ms).
    line.prepare(kMaxChannels, static_cast<int>(std::ceil((kMaxDelaySeconds + 0.05) * sampleRate)));
    // A transport changing speed: a 150 ms time constant, at most 0.4 samples
    // of delay per sample (a bend of up to 8 semitones down and 6 up).
    slew.prepare(sampleRate, 0.15, 0.4);
    feedbackGain.prepare(sampleRate, 0.02);
    wowSamples.prepare(sampleRate, 0.05);
    flutterSamples.prepare(sampleRate, 0.05);
    randomSamples.prepare(sampleRate, 0.05);
    saturationAmount.prepare(sampleRate, 0.02);
    ageAmount.prepare(sampleRate, 0.05);
    wowLfo.prepare(sampleRate);
    flutterLfo.prepare(sampleRate);
    for (auto& duck : ducker)
      duck.prepare(sampleRate);
    // The drift: white noise through two 2 Hz low-pass poles, scaled back to
    // unit RMS. Two poles, because after one the derivative is still white
    // noise (the read pointer would jitter by whole samples); after two it is
    // smooth. The pair h[n] = c^2 (n + 1) a^n, a = 1 - c, has the energy
    // c^4 (1 + a^2) / (1 - a^2)^3, and uniform noise has variance 1/3.
    const double c = 1.0 - std::exp(-juce::MathConstants<double>::twoPi * kDriftHz / sampleRate);
    const double a2 = (1.0 - c) * (1.0 - c);
    const double energy = c * c * c * c * (1.0 + a2) / ((1.0 - a2) * (1.0 - a2) * (1.0 - a2));
    driftGain = static_cast<float>(std::sqrt(3.0 / energy));
    resetEffect();
  }

  void resetEffect() noexcept override {
    line.clear();
    for (int ch = 0; ch < kMaxChannels; ++ch) {
      lowCut[ch].reset();
      highCut[ch][0].reset();
      highCut[ch][1].reset();
      headBump[ch].reset();
    }
    for (auto& pole : drift) {
      pole.reset();
      pole.setCutoff(kDriftHz, sampleRate);
    }
    // Same stream at every power-on and every render.
    noise.seed(kNoiseSalt);
    slew.snapOnNextTarget();
    feedbackGain.snapOnNextTarget();
    wowSamples.snapOnNextTarget();
    flutterSamples.snapOnNextTarget();
    randomSamples.snapOnNextTarget();
    saturationAmount.snapOnNextTarget();
    ageAmount.snapOnNextTarget();
    // The two oscillators start out of step, so the wobble does not begin as
    // a pure sine of either.
    wowLfo.reset(0.0);
    flutterLfo.reset(0.3);
    grid.reset();
    for (auto& duck : ducker)
      duck.reset();
    slewedDelay.store(0.0f, std::memory_order_relaxed);
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxChannels);
    lastBpm.store(ctx.transport.bpm, std::memory_order_relaxed);

    const double twoPi = juce::MathConstants<double>::twoPi;
    const double wowRate = static_cast<double>(param(kWowRateHz));
    const double flutterRate = static_cast<double>(param(kFlutterRateHz));
    const double wowDepth = static_cast<double>(param(kWow)) * 0.01;
    const double flutterDepth = static_cast<double>(param(kFlutter)) * 0.01;
    // The delay excursions that give the set pitch deviations at these rates.
    slew.setTarget(static_cast<float>(delaySeconds(ctx.transport.bpm) * sampleRate));
    wowSamples.setTarget(
        static_cast<float>(kWowDeviation * wowDepth * sampleRate / (twoPi * wowRate)));
    flutterSamples.setTarget(
        static_cast<float>(kFlutterDeviation * flutterDepth * sampleRate / (twoPi * flutterRate)));
    randomSamples.setTarget(
        static_cast<float>(kRandomShare * kWowDeviation * wowDepth * sampleRate / twoPi));
    feedbackGain.setTarget(param(kFeedback) * 0.01f);
    saturationAmount.setTarget(param(kSaturation) * 0.01f);
    ageAmount.setTarget(param(kAge) * 0.01f);
    wowLfo.setRateHz(wowRate);
    flutterLfo.setRateHz(flutterRate);
    for (auto& duck : ducker)
      duck.setTarget(param(kDuckDb), param(kDuckReleaseMs));

    // One transport carries both channels, so the wobble is common to them
    // (a stereo difference would not be tape, and dual mono must stay alike).
    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    float delay = slew.current();
    for (int i = 0; i < numSamples; ++i) {
      delay = slew.next();
      const float feedback = feedbackGain.next();
      const float wow = wowSamples.next();
      const float flutter = flutterSamples.next();
      const float random = randomSamples.next();
      const float drive = 1.0f + 4.0f * saturationAmount.next();
      const float inverseDrive = 1.0f / drive;
      const float age = ageAmount.next();
      if (grid.due())
        updateHeads(age, numChannels);

      // The stream and the oscillators advance every sample, audible or not.
      const float drifting =
          juce::jlimit(-2.5f, 2.5f, drift[1].lowpass(drift[0].lowpass(noise.nextBipolar())) * driftGain);
      const float wowValue = wowLfo.next();
      const float flutterValue = flutterLfo.next();
      float position = delay;
      if (wow != 0.0f || flutter != 0.0f || random != 0.0f)
        position += wow * wowValue + flutter * flutterValue + random * drifting;

      for (int ch = 0; ch < numChannels; ++ch) {
        const float raw = line.readHermite(ch, position);
        // The heads: low cut, high cut (two poles) and the bump.
        const float echo = headBump[ch].process(
            highCut[ch][1].lowpass(highCut[ch][0].lowpass(lowCut[ch].highpass(raw))));
        // The record stage saturates everything that goes onto the tape: the
        // input, and the repeats. It is also the ceiling of the loop.
        const float input = channels[ch][i];
        line.write(ch, saturate(input + feedback * echo, drive, inverseDrive));
        // Wet only: the dry signal and the balance are the block's mix. The
        // ducking lowers it while the input is loud.
        channels[ch][i] = echo * ducker[ch].next(input, input);
      }
    }
    slewedDelay.store(delay, std::memory_order_relaxed);
  }

private:
  static constexpr std::uint32_t kNoiseSalt = 0x7a9e51c3u;
  static constexpr double kBumpHz = 90.0;
  static constexpr double kBumpDb = 2.0;
  static constexpr double kDriftHz = 2.0;

  // What age does to the heads: fresh tape reaches from 40 Hz to 9 kHz, worn
  // tape from 220 Hz to 2.2 kHz, and the head bump (+2 dB at 90 Hz) fades away.
  static double lowCutHz(double age) noexcept { return 40.0 * std::pow(5.5, age); }
  static double highCutHz(double age) noexcept { return 9000.0 * std::pow(2200.0 / 9000.0, age); }
  static double bumpGain(double age) noexcept {
    return std::pow(10.0, kBumpDb * (1.0 - age) / 20.0);
  }

  void updateHeads(float age, int numChannels) noexcept {
    const double a = static_cast<double>(age);
    const float low = static_cast<float>(lowCutHz(a));
    const float high = static_cast<float>(highCutHz(a));
    const fx::BiquadCoeffs bump =
        fx::BiquadCoeffs::peak(kBumpHz, 0.8, kBumpDb * (1.0 - a), sampleRate);
    for (int ch = 0; ch < numChannels; ++ch) {
      lowCut[ch].setCutoff(low, sampleRate);
      highCut[ch][0].setCutoff(high, sampleRate);
      highCut[ch][1].setCutoff(high, sampleRate);
      headBump[ch].setCoeffs(bump);
    }
  }

  double delaySeconds(double bpm) const noexcept {
    BlockTransport transport;
    transport.bpm = bpm;
    return juce::jmin(kMaxDelaySeconds, fx::resolveTimeSeconds(param(kSync), param(kDivision),
                                                               param(kTimeMs), transport));
  }

  fx::DelayLine line;
  DelaySlew slew;
  fx::OnePole lowCut[kMaxChannels];
  fx::OnePole highCut[kMaxChannels][2];
  fx::Biquad headBump[kMaxChannels];
  fx::OnePole drift[2];
  fx::NoiseSource noise;
  fx::Lfo wowLfo;
  fx::Lfo flutterLfo;
  fx::SmoothedParam feedbackGain;
  fx::SmoothedParam wowSamples;
  fx::SmoothedParam flutterSamples;
  fx::SmoothedParam randomSamples;
  fx::SmoothedParam saturationAmount;
  fx::SmoothedParam ageAmount;
  ControlTick grid;
  Ducker ducker[kMaxChannels];
  double sampleRate{48000.0};
  float driftGain{100.0f};
  std::atomic<double> lastBpm{120.0};
  std::atomic<float> slewedDelay{0.0f};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(TapeEcho) {
  // Heard beside the dry signal (see DigitalDelay): a little more wet than
  // the others at 35%, since the repeats are darker.
  addEffectKind<TapeEchoProcessor>(registry, "Tape Echo", BlockCategory::Delay, 0.35f,
                                   BlockMixLaw::DryPreserving);
}

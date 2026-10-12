// modulation.vibrato: pure pitch modulation. One delay line per channel read
// at a delay swept by a sine LFO; there is no dry in the wet (a vibrato mixed
// with its own dry is a chorus). The depth is the peak pitch deviation in
// cents, so the sweep of the delay is derived from it and the rate: a delay
// that moves by `a * sin(2 pi r t)` bends the pitch by up to 2 pi r a.
#include <cmath>
#include "effects/EffectDsp.h"

namespace {

class VibratoProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "modulation.vibrato";

  // The first three are the standard rate parameters (fx::appendRateParams).
  enum Param { kSync = 0, kDivision, kRateHz, kDepthCents, kRiseMs };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      fx::RateParamOptions rate;
      rate.minHz = 0.5f;
      rate.maxHz = 12.0f;
      rate.defaultHz = 5.0f;
      fx::appendRateParams(t, rate);
      // Peak deviation either side of the played pitch.
      t.push_back(BlockParamSpec::floatParam("depthCents", "Depth", "cents", 0.0f, 100.0f, 20.0f));
      // The modulation grows from nothing to full over this long after the
      // block is switched on.
      t.push_back(BlockParamSpec::floatParam("riseMs", "Rise", "ms", 0.0f, 5000.0f, 250.0f)
                      .withSkew(0.4f));
      return t;
    }();
    return table;
  }

  VibratoProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The line holds at most a centre delay plus the largest sweep, so the
  // wet is over that long after the input stops. Fixed: it does not depend
  // on a parameter.
  double tailSeconds() const noexcept override {
    return 2.0 * kMaxExcursionSeconds + kMinDelaySeconds + 0.005;
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    line.prepare(juce::jmax(1, spec.numChannels),
                 static_cast<int>(std::ceil((2.0 * kMaxExcursionSeconds + kMinDelaySeconds) *
                                            sampleRate)) +
                     4);
    lfo.prepare(sampleRate);
    // A change of depth or rate changes the centre delay; let it glide
    // slowly so that moves the pitch by a few cents at most.
    excursion.prepare(sampleRate, 0.08);
    resetEffect();
  }

  void resetEffect() noexcept override {
    line.clear();
    lfo.reset();
    excursion.snapOnNextTarget();
    // The rise starts over each time the block comes back to life.
    riseGain = 0.0f;
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), line.numChannels(), kMaxChannels);

    const double rateHz =
        fx::resolveRateHz(param(kSync), param(kDivision), param(kRateHz), ctx.transport);
    lfo.setRateHz(rateHz);
    if (param(kSync) >= 0.5f)
      lfo.syncPhaseToTransport(ctx.transport,
                               fx::noteDivisionBeats(static_cast<int>(std::lround(param(kDivision)))));

    // Peak pitch ratio change: sinh keeps the upward and the downward peak
    // equally far from the played pitch in cents (to a fraction of a cent).
    // Slow rates would need more delay than the line has; the depth gives
    // way there rather than the line overflowing.
    const double bend = std::sinh(static_cast<double>(param(kDepthCents)) *
                                  0.6931471805599453 / 1200.0);
    const double omega = juce::MathConstants<double>::twoPi * juce::jmax(rateHz, 0.01);
    excursion.setTarget(static_cast<float>(
        juce::jmin(kMaxExcursionSeconds, bend / omega) * sampleRate));

    const float riseSamples = param(kRiseMs) * 0.001f * static_cast<float>(sampleRate);
    const float riseStep = riseSamples > 1.0f ? 1.0f / riseSamples : 1.0f;
    const float minDelay = static_cast<float>(kMinDelaySeconds * sampleRate);

    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    for (int i = 0; i < numSamples; ++i) {
      const float sweep = excursion.next();
      riseGain = juce::jmin(1.0f, riseGain + riseStep);
      const float wave = lfo.valueAt(0.0f);
      // The centre sits a sweep and a little more clear of the write
      // position, so the delay never reaches 0 whatever the phase.
      const float delay = sweep + minDelay + sweep * riseGain * wave;
      for (int ch = 0; ch < numChannels; ++ch) {
        const float shifted = line.readHermite(ch, delay);
        line.write(ch, channels[ch][i]);
        channels[ch][i] = shifted;
      }
      lfo.advance();
    }
  }

private:
  static constexpr int kMaxChannels = 2;
  static constexpr double kMaxExcursionSeconds = 0.025;
  static constexpr double kMinDelaySeconds = 0.0005;

  fx::DelayLine line;
  fx::Lfo lfo;
  fx::SmoothedParam excursion;
  float riseGain{0.0f};
  double sampleRate{48000.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(Vibrato) {
  // Wet only, fully wet by default: the pitch wobble replaces the input.
  // A partial mix beside the dry is a chorus, which the crossfade still
  // gives if someone wants it.
  addEffectKind<VibratoProcessor>(registry, "Vibrato", BlockCategory::Modulation, 1.0f,
                                  BlockMixLaw::Crossfade);
}

// creative.bitcrush: lo-fi by arithmetic. The input is sampled at a reduced
// rate with a zero-order hold (the held value is simply repeated until the
// next tick of a phase accumulator running at `rateHz`) and each held value
// is rounded to a grid whose spacing follows the bit depth.
//
// Quantiser: mid-tread, q = round(x * L) / L with L = 2^(bits - 1), the grid
// a signed integer converter of that depth has, so 16 bits is the 16-bit
// grid and silence stays at zero (a mid-rise grid would put a DC offset on
// every pause). The depth is fractional: L is 2^(bits - 1) for any bits, so
// 7.5 bits is a grid between the 7 and the 8 bit ones. For a signal that
// spans many steps the rounding error is a white noise of Δ²/12 with
// Δ = 1/L, which is what the tests measure.
//
// Anti-alias: with the switch on, the input goes through a fourth-order
// Butterworth low-pass at 0.45 of the reduced rate before it is sampled, so
// the hold folds far less into the audible band. Off, the fold is the sound
// (the harsh, metallic aliasing of a real low-rate converter).
//
// Mono, stereo and dual mono are the same code on every channel; the hold
// ticks are shared, so two sides stay in step. The kind adds no latency (the
// filter is a causal one) and holds nothing past its input.
#include <cmath>
#include "effects/EffectDsp.h"

namespace {

class BitCrusherProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "creative.bitcrush";

  enum Param { kBits = 0, kRateHz, kAntiAlias };

  static constexpr int kMaxChannels = 2;

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::floatParam("bits", "Bit Depth", "bit", 1.0f, 16.0f, 8.0f));
      t.push_back(BlockParamSpec::floatParam("rateHz", "Sample Rate", "Hz", 500.0f, 48000.0f,
                                             12000.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::boolParam("antiAlias", "Anti-Alias", false));
      return t;
    }();
    return table;
  }

  BitCrusherProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    resetEffect();
  }

  void resetEffect() noexcept override {
    for (int ch = 0; ch < kMaxChannels; ++ch) {
      held[ch] = 0.0f;
      for (auto& section : antiAlias[ch])
        section.reset();
    }
    // The first sample is a tick.
    phase = 1.0;
    designedFor = -1.0f;
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext&) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxChannels);

    const float bits = param(kBits);
    const double rate = juce::jlimit(1.0, sampleRate, static_cast<double>(param(kRateHz)));
    const bool filtered = param(kAntiAlias) >= 0.5f && rate < sampleRate;
    const double increment = rate / sampleRate;
    const float levels = std::exp2(bits - 1.0f);
    const float inverse = 1.0f / levels;

    if (filtered && designedFor != static_cast<float>(rate)) {
      designedFor = static_cast<float>(rate);
      // Two Butterworth sections (Q 0.5412 and 1.3066): fourth order.
      const double corner = 0.45 * rate;
      for (int ch = 0; ch < kMaxChannels; ++ch) {
        antiAlias[ch][0].setCoeffs(fx::BiquadCoeffs::lowpass(corner, 0.5412, sampleRate));
        antiAlias[ch][1].setCoeffs(fx::BiquadCoeffs::lowpass(corner, 1.3066, sampleRate));
      }
    }

    float* data[kMaxChannels] = {buffer.getWritePointer(0),
                                 numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    for (int i = 0; i < numSamples; ++i) {
      // The filter runs on every sample, whether or not this one is taken.
      float sampled[kMaxChannels] = {0.0f, 0.0f};
      for (int ch = 0; ch < numChannels; ++ch) {
        float x = data[ch][i];
        if (filtered)
          x = antiAlias[ch][1].process(antiAlias[ch][0].process(x));
        sampled[ch] = x;
      }
      // A hair under 1 counts: a ratio like 3 (rate / 3) is not exact in a
      // double, and the hold should not stretch by a sample for it.
      if (phase >= 1.0 - 1.0e-9) {
        phase -= 1.0;
        for (int ch = 0; ch < numChannels; ++ch)
          held[ch] = std::round(sampled[ch] * levels) * inverse;
      }
      phase += increment;
      for (int ch = 0; ch < numChannels; ++ch)
        data[ch][i] = held[ch];
    }
  }

private:
  fx::Biquad antiAlias[kMaxChannels][2];
  float held[kMaxChannels] = {0.0f, 0.0f};
  double phase{1.0};
  double sampleRate{48000.0};
  float designedFor{-1.0f};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(BitCrusher) {
  // Replaces its input: the blend is a plain crossfade, fully wet to start.
  addEffectKind<BitCrusherProcessor>(registry, "Bit Crusher", BlockCategory::Creative, 1.0f,
                                     BlockMixLaw::Crossfade);
}

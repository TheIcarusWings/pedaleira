// creative.ringmod: the input multiplied by an oscillator. A sine carrier of
// frequency c turns every partial f of the input into two, at f + c and
// f - c (each at half the level), and removes the partial itself; a square or
// triangle carrier does the same for each of its odd harmonics, which is why
// they sound harsher. The slow LFO swings the carrier frequency by up to
// `lfoDepth` semitones either way (a sine LFO, so the swing is symmetric in
// pitch), for the warbling sweep of a ring modulator played with a knob.
//
// The oscillators are cosine-shaped: every shape is at +1 at phase 0 and the
// phase starts there, so the very first input sample is already carried.
// The sine is the exact cosine, the triangle a straight-line cosine and the
// square is band-limited with a two-sample polyBLEP at both edges, so it does
// not alias into the sidebands the way a naive one would.
//
// No latency, no tail. The block's mix is the dry/wet balance; the wet path
// is the pure product.
#include <cmath>
#include "effects/EffectDsp.h"

namespace {

class RingModulatorProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "creative.ringmod";

  enum Param { kFrequencyHz = 0, kShape, kLfoDepth, kLfoRateHz };
  enum Shape { kSine = 0, kSquare, kTriangle };

  static constexpr int kMaxChannels = 2;

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::floatParam("frequencyHz", "Frequency", "Hz", 1.0f, 4000.0f,
                                             440.0f)
                      .withSkew(0.3f));
      t.push_back(BlockParamSpec::choiceParam("shape", "Shape", {"Sine", "Square", "Triangle"},
                                              kSine));
      t.push_back(BlockParamSpec::floatParam("lfoDepth", "LFO Depth", "st", 0.0f, 24.0f, 0.0f));
      t.push_back(BlockParamSpec::floatParam("lfoRateHz", "LFO Rate", "Hz", 0.05f, 20.0f, 1.0f)
                      .withSkew(0.4f));
      return t;
    }();
    return table;
  }

  RingModulatorProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    lfo.prepare(sampleRate);
    lfo.setShape(fx::Lfo::Shape::Sine);
    frequency.prepare(sampleRate, 0.02);
    resetEffect();
  }

  void resetEffect() noexcept override {
    phase = 0.0;
    lfo.reset();
    frequency.snapOnNextTarget();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext&) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxChannels);
    const int shape = static_cast<int>(std::lround(param(kShape)));
    const float depthSemitones = param(kLfoDepth);
    frequency.setTarget(param(kFrequencyHz));
    lfo.setRateHz(static_cast<double>(param(kLfoRateHz)));

    float* data[kMaxChannels] = {buffer.getWritePointer(0),
                                 numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    const double nyquistLimit = 0.45 * sampleRate;
    for (int i = 0; i < numSamples; ++i) {
      double hz = static_cast<double>(frequency.next());
      if (depthSemitones > 0.0f)
        hz *= std::exp2(static_cast<double>(depthSemitones) * static_cast<double>(lfo.next()) /
                        12.0);
      hz = juce::jlimit(0.0, nyquistLimit, hz);
      const double step = hz / sampleRate;

      float carrier;
      switch (shape) {
        case kSquare: carrier = squareAt(phase, step); break;
        case kTriangle: carrier = 4.0f * static_cast<float>(std::abs(phase - 0.5)) - 1.0f; break;
        default: carrier = static_cast<float>(std::cos(kTwoPi * phase)); break;
      }
      for (int ch = 0; ch < numChannels; ++ch)
        data[ch][i] *= carrier;

      phase += step;
      if (phase >= 1.0)
        phase -= 1.0;
    }
  }

private:
  static constexpr double kTwoPi = 6.283185307179586;

  // +1 on [0, 0.25) and [0.75, 1), -1 between (the sign of the cosine), with
  // the two-sample polynomial correction at the falling edge (0.25) and the
  // rising edge (0.75).
  static float squareAt(double p, double step) noexcept {
    float value = (p < 0.25 || p >= 0.75) ? 1.0f : -1.0f;
    value += blep(p - 0.25, step) * -1.0f;
    value += blep(wrap(p - 0.75), step) * 1.0f;
    return value;
  }

  static double wrap(double p) noexcept { return p < 0.0 ? p + 1.0 : p; }

  // The correction for a +-1 square's jump of 2 at an edge, spread over one
  // sample either side of it (added at a rising edge, subtracted at a falling
  // one); `t` is the phase past the edge in turns.
  static float blep(double t, double step) noexcept {
    if (step <= 0.0)
      return 0.0f;
    if (t < 0.0)
      t += 1.0;
    if (t < step) {
      const double x = t / step;
      return static_cast<float>(x + x - x * x - 1.0);
    }
    if (t > 1.0 - step) {
      const double x = (t - 1.0) / step;
      return static_cast<float>(x * x + x + x + 1.0);
    }
    return 0.0f;
  }

  fx::Lfo lfo;
  fx::SmoothedParam frequency;
  double phase{0.0};
  double sampleRate{48000.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(RingModulator) {
  // Half and half to start: a ring modulator at 100% hides the playing.
  addEffectKind<RingModulatorProcessor>(registry, "Ring Modulator", BlockCategory::Creative, 0.5f,
                                        BlockMixLaw::Crossfade);
}

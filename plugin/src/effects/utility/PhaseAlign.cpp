// utility.phase: lines two paths up. A polarity switch and a delay of up to
// 5 ms, set to a fraction of a sample, for the case where two paths that
// are summed again (the two amps of a split, a direct and a miked path) reach
// the mix a little apart or against each other.
//
// The delay is a deliberate offset of this path, not processing latency, so
// the block reports none. With the switch off and the delay at 0 it is an
// exact pass-through. The fraction is Hermite interpolated (flat to well
// above the audio band). Below one sample the newest sample is the only
// neighbour on one side, so that case is linear.
#include <cmath>
#include "effects/EffectDsp.h"

namespace {

class PhaseAlignProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "utility.phase";

  enum Param { kPolarity = 0, kDelayMs };

  static constexpr float kMaxDelayMs = 5.0f;

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::boolParam("polarity", "Invert Polarity", false));
      t.push_back(BlockParamSpec::floatParam("delayMs", "Delay", "ms", 0.0f, kMaxDelayMs, 0.0f)
                      .withAffectsTail());
      return t;
    }();
    return table;
  }

  PhaseAlignProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  double tailSeconds() const noexcept override {
    return static_cast<double>(param(kDelayMs)) * 0.001;
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    line.prepare(juce::jmax(1, spec.numChannels),
                 static_cast<int>(std::ceil(kMaxDelayMs * 0.001 * sampleRate)) + 4);
    sign.prepare(sampleRate, 0.005);
    delay.prepare(sampleRate, 0.03);
    resetEffect();
  }

  void resetEffect() noexcept override {
    line.clear();
    sign.snapOnNextTarget();
    delay.snapOnNextTarget();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext&) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), line.numChannels());
    sign.setTarget(param(kPolarity) >= 0.5f ? -1.0f : 1.0f);
    delay.setTarget(param(kDelayMs) * 0.001f * static_cast<float>(sampleRate));

    for (int i = 0; i < numSamples; ++i) {
      const float s = sign.next();
      const float d = delay.next();
      for (int ch = 0; ch < numChannels; ++ch) {
        float* x = buffer.getWritePointer(ch);
        line.write(ch, x[i]);
        // After the write, a read at 1 is the sample just written.
        float y = x[i];
        if (d > 0.0f)
          y = d >= 1.0f ? line.readHermite(ch, d + 1.0f) : line.readLinear(ch, d + 1.0f);
        x[i] = y * s;
      }
    }
  }

private:
  fx::DelayLine line;
  fx::SmoothedParam sign, delay;
  double sampleRate{48000.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(PhaseAlign) {
  addEffectKind<PhaseAlignProcessor>(registry, "Phase Align", BlockCategory::Utility);
}

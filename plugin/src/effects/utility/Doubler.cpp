// utility.doubler: the sound of a part played twice. A copy of the input,
// delayed by up to `spreadMs` and slowly wobbled so it never lines up with
// the original the way a plain delay would, is added to the dry signal.
//
//   out = dry * dryDb + wobbled delay(in) * doubledDb
//
// The delay sits in the top fifth of the spread (0.8 to 1.0 times it), moved
// by a slow sine; the copy therefore never arrives later than the spread.
// Two sines of slightly different rate drive the two sides, so in a true
// stereo signal the two copies are decorrelated and the double sounds wide.
// In mono and in dual mono every channel is driven by the left sine, so each
// channel is exactly what a mono rig makes of it and nothing is mixed.
#include <cmath>
#include "effects/EffectDsp.h"

namespace {

class DoublerProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "utility.doubler";

  enum Param { kSpreadMs = 0, kDryDb, kDoubledDb };

  static constexpr float kMaxSpreadMs = 60.0f;
  static constexpr float kFloorDb = -40.0f;

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::floatParam("spreadMs", "Spread", "ms", 3.0f, kMaxSpreadMs, 25.0f));
      t.push_back(BlockParamSpec::floatParam("dryDb", "Dry", "dB", kFloorDb, 12.0f, 0.0f));
      t.push_back(BlockParamSpec::floatParam("doubledDb", "Doubled", "dB", kFloorDb, 12.0f, -3.0f));
      return t;
    }();
    return table;
  }

  DoublerProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The copy is gone one spread after the input stops. Fixed at the longest.
  double tailSeconds() const noexcept override { return kMaxSpreadMs * 0.001 + 0.005; }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    line.prepare(juce::jmax(1, spec.numChannels),
                 static_cast<int>(std::ceil(kMaxSpreadMs * 0.001 * sampleRate)) + 4);
    for (fx::Lfo& lfo : lfos)
      lfo.prepare(sampleRate);
    lfos[0].setRateHz(kRateLeftHz);
    lfos[1].setRateHz(kRateRightHz);
    // A moving delay bends the pitch, so the spread glides slowly.
    spread.prepare(sampleRate, 0.1);
    dryGain.prepare(sampleRate, 0.02);
    doubledGain.prepare(sampleRate, 0.02);
    resetEffect();
  }

  void resetEffect() noexcept override {
    line.clear();
    lfos[0].reset(0.0);
    lfos[1].reset(0.37);
    spread.snapOnNextTarget();
    dryGain.snapOnNextTarget();
    doubledGain.snapOnNextTarget();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), line.numChannels());
    const bool wide = blockHasStereoImage(numChannels, ctx);

    spread.setTarget(param(kSpreadMs) * 0.001f * static_cast<float>(sampleRate));
    dryGain.setTarget(fx::dbToGain(param(kDryDb)));
    doubledGain.setTarget(fx::dbToGain(param(kDoubledDb)));

    for (int i = 0; i < numSamples; ++i) {
      const float s = spread.next();
      const float dry = dryGain.next();
      const float wet = doubledGain.next();
      const float waves[2] = {lfos[0].valueAt(), wide ? lfos[1].valueAt() : lfos[0].valueAt()};
      for (int ch = 0; ch < numChannels; ++ch) {
        float* x = buffer.getWritePointer(ch);
        const float d = s * (0.9f + 0.1f * waves[ch]);
        // Read before the write: a delay of d samples, never fewer than 2.
        const float copy = line.readHermite(ch, juce::jmax(2.0f, d));
        line.write(ch, x[i]);
        x[i] = dry * x[i] + wet * copy;
      }
      lfos[0].advance();
      lfos[1].advance();
    }
  }

private:
  static constexpr double kRateLeftHz = 0.45;
  static constexpr double kRateRightHz = 0.61;

  fx::DelayLine line;
  fx::Lfo lfos[2];
  fx::SmoothedParam spread, dryGain, doubledGain;
  double sampleRate{48000.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(Doubler) {
  addEffectKind<DoublerProcessor>(registry, "Doubler", BlockCategory::Utility);
}

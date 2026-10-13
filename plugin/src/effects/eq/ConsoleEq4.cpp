// eq.console4: a four-band equaliser in a mixing-console layout, with a
// high-pass and a low-pass filter at the ends.
//
//   high-pass (12 dB/oct, 20 to 500 Hz, switch)
//   low shelf (20 to 200 Hz, +-12 dB)
//   low-mid bell (100 to 1000 Hz, +-12 dB)
//   high-mid bell (500 to 5000 Hz, +-12 dB)
//   high shelf (2 to 20 kHz, +-12 dB)
//   low-pass (12 dB/oct, 1 to 20 kHz, switch)
//   output level
//
// The shelves and filters have a fixed Butterworth Q and the bells a fixed
// moderate one, as a console has. The engine is effects/dsp/EqBank.h; the
// filters are off until their switch is on, a flat bell or shelf is skipped,
// so the neutral setting is bit-exact.
#include "effects/EffectDsp.h"
#include "effects/dsp/EqParams.h"

namespace {

using fx::eq::BandTarget;
using fx::eq::BandType;

class ConsoleEq4Processor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "eq.console4";
  static constexpr int kNumBands = 6;

  enum Param {
    kHpOn = 0,
    kHpFreq,
    kLowGain,
    kLowFreq,
    kLowMidGain,
    kLowMidFreq,
    kHighMidGain,
    kHighMidFreq,
    kHighGain,
    kHighFreq,
    kLpOn,
    kLpFreq,
    kLevelDb
  };

  static constexpr float kShelfQ = 0.7071f;
  static constexpr float kBellQ = 0.9f;
  static constexpr float kFilterQ = 0.7071f;

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      const auto gain = [](const char* id, const char* name) {
        return BlockParamSpec::floatParam(id, name, "dB", -12.0f, 12.0f, 0.0f).withAffectsTail();
      };
      t.push_back(BlockParamSpec::boolParam("hpOn", "High-pass", false).withAffectsTail());
      t.push_back(fx::eq::frequencyParam("hpFreq", "High-pass frequency", 20.0f, 500.0f, 80.0f));
      t.push_back(gain("lowGain", "Low gain"));
      t.push_back(fx::eq::frequencyParam("lowFreq", "Low frequency", 20.0f, 200.0f, 100.0f));
      t.push_back(gain("lowMidGain", "Low-mid gain"));
      t.push_back(fx::eq::frequencyParam("lowMidFreq", "Low-mid frequency", 100.0f, 1000.0f, 400.0f));
      t.push_back(gain("highMidGain", "High-mid gain"));
      t.push_back(
          fx::eq::frequencyParam("highMidFreq", "High-mid frequency", 500.0f, 5000.0f, 2500.0f));
      t.push_back(gain("highGain", "High gain"));
      t.push_back(fx::eq::frequencyParam("highFreq", "High frequency", 2000.0f, 20000.0f, 8000.0f));
      t.push_back(BlockParamSpec::boolParam("lpOn", "Low-pass", false).withAffectsTail());
      t.push_back(fx::eq::frequencyParam("lpFreq", "Low-pass frequency", 1000.0f, 20000.0f, 12000.0f));
      t.push_back(BlockParamSpec::floatParam("levelDb", "Output", "dB", -20.0f, 20.0f, 0.0f));
      return t;
    }();
    return table;
  }

  ConsoleEq4Processor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  double tailSeconds() const noexcept override {
    BandTarget targets[kNumBands];
    readTargets(targets);
    return fx::eq::Bank<kNumBands>::tailSeconds(targets);
  }

protected:
  void prepareEffect(const EffectSpec& spec) override { bank.prepare(spec.sampleRate); }
  void resetEffect() noexcept override { bank.reset(); }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext&) override {
    BandTarget targets[kNumBands];
    readTargets(targets);
    bank.setTargets(targets, param(kLevelDb));
    bank.process(buffer.getArrayOfWritePointers(), buffer.getNumChannels(),
                 buffer.getNumSamples());
  }

private:
  static void set(BandTarget& band, BandType type, float hz, float gainDb, float q,
                  bool active) noexcept {
    band.type = type;
    band.hz = hz;
    band.gainDb = gainDb;
    band.q = q;
    band.active = active;
  }

  void readTargets(BandTarget* t) const noexcept {
    set(t[0], BandType::HighPass2, param(kHpFreq), 0.0f, kFilterQ, param(kHpOn) >= 0.5f);
    set(t[1], BandType::LowShelf, param(kLowFreq), param(kLowGain), kShelfQ, true);
    set(t[2], BandType::Peak, param(kLowMidFreq), param(kLowMidGain), kBellQ, true);
    set(t[3], BandType::Peak, param(kHighMidFreq), param(kHighMidGain), kBellQ, true);
    set(t[4], BandType::HighShelf, param(kHighFreq), param(kHighGain), kShelfQ, true);
    set(t[5], BandType::LowPass2, param(kLpFreq), 0.0f, kFilterQ, param(kLpOn) >= 0.5f);
  }

  fx::eq::Bank<kNumBands> bank;
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(ConsoleEq4) {
  addEffectKind<ConsoleEq4Processor>(registry, "Console EQ", BlockCategory::Eq);
}

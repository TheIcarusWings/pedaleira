// utility.volume: a volume pedal. One control, the pedal position, sets the
// level between a floor (minimumDb, silence at the heel when it is at its
// lowest) and unity at the toe. Two tapers:
//
//   Linear       the amplitude moves in a straight line from the floor to 1
//   Audio taper  the level in dB moves in a straight line, which is how a
//                pedal feels under the foot: equal steps sound like equal
//                steps of loudness
//
// The gain is ramped (20 ms), so a pedal that is moved quickly, or an
// expression controller that steps, never zippers. At the toe (position 100)
// the block is an exact pass-through.
#include "effects/EffectDsp.h"

namespace {

class VolumePedalProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "utility.volume";

  enum Param { kPositionPercent = 0, kCurve, kMinimumDb };
  enum Curve { kLinear = 0, kAudioTaper };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(
          BlockParamSpec::floatParam("positionPercent", "Position", "%", 0.0f, 100.0f, 100.0f));
      t.push_back(BlockParamSpec::choiceParam("curve", "Curve", {"Linear", "Audio taper"},
                                              kAudioTaper));
      t.push_back(BlockParamSpec::floatParam("minimumDb", "Minimum", "dB", -60.0f, 0.0f, -60.0f));
      return t;
    }();
    return table;
  }

  VolumePedalProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

protected:
  void prepareEffect(const EffectSpec& spec) override {
    gain.prepare(spec.sampleRate, 0.02);
    resetEffect();
  }

  void resetEffect() noexcept override { gain.snapOnNextTarget(); }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext&) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    gain.setTarget(gainFor(param(kPositionPercent), static_cast<int>(std::lround(param(kCurve))),
                           param(kMinimumDb)));

    // Settled at unity: leave the samples alone.
    if (!gain.isSmoothing() && gain.current() == 1.0f)
      return;

    for (int i = 0; i < numSamples; ++i) {
      const float g = gain.next();
      for (int ch = 0; ch < numChannels; ++ch)
        buffer.getWritePointer(ch)[i] *= g;
    }
  }

private:
  /** The gain for a pedal position (0 to 100), a curve and a floor. */
  static float gainFor(float positionPercent, int curve, float minimumDb) noexcept {
    const float p = juce::jlimit(0.0f, 1.0f, positionPercent * 0.01f);
    // The lowest floor setting means "off at the heel", not -60 dB.
    const float floorGain = minimumDb <= -60.0f ? 0.0f : fx::dbToGain(minimumDb);
    if (p >= 1.0f)
      return 1.0f;
    if (p <= 0.0f)
      return floorGain;
    if (curve == kLinear)
      return floorGain + (1.0f - floorGain) * p;
    return fx::dbToGain(minimumDb * (1.0f - p));
  }

  fx::SmoothedParam gain;
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(VolumePedal) {
  addEffectKind<VolumePedalProcessor>(registry, "Volume Pedal", BlockCategory::Utility);
}

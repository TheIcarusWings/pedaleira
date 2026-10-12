// filter.wah: a resonant band-pass sweep driven by one control, the pedal
// position. The position parameter is the one an expression pedal drives, so
// it is smoothed and the cutoff is moved on a fixed control grid: a pedal
// that jumps from heel to toe glides over a few milliseconds instead of
// clicking, and the sound does not depend on the host block size.
#include <cmath>
#include "effects/EffectDsp.h"
#include "effects/filter/SweepFilter.h"

namespace {

class WahProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "filter.wah";

  enum Param { kPositionPct = 0, kResonancePct, kVoicing, kLevelDb };

  // The sweep of each voicing, heel down to toe down. The classic one is the
  // 350 Hz to 2.2 kHz of the pedal everyone knows.
  struct Voicing {
    const char* name;
    float heelHz;
    float toeHz;
  };
  static constexpr Voicing kVoicings[] = {
      {"Classic", 350.0f, 2200.0f},
      {"Wide", 250.0f, 3200.0f},
      {"Low", 200.0f, 1400.0f},
      {"Bright", 500.0f, 3000.0f},
  };
  static constexpr int kNumVoicings = 4;

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::floatParam("positionPct", "Position", "%", 0.0f, 100.0f, 50.0f));
      t.push_back(BlockParamSpec::floatParam("resonancePct", "Resonance", "%", 0.0f, 100.0f, 50.0f)
                      .withAffectsTail());
      juce::StringArray names;
      for (const auto& voicing : kVoicings)
        names.add(voicing.name);
      t.push_back(BlockParamSpec::choiceParam("voicing", "Voicing", names, 0).withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("levelDb", "Level", "dB", -12.0f, 12.0f, 0.0f));
      return t;
    }();
    return table;
  }

  WahProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The resonance rings for as long as its quality factor says, longest at
  // the heel (lowest cutoff) of the current voicing. Atomics and arithmetic.
  double tailSeconds() const noexcept override {
    const Voicing& voicing = currentVoicing();
    return fxfilter::resonanceDecaySeconds(voicing.heelHz,
                                           fxfilter::resonanceToQ(param(kResonancePct))) +
           kSmoothSeconds;
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    position.prepare(sampleRate, kPositionSmoothSeconds);
    heelLog2.prepare(sampleRate, kSmoothSeconds);
    toeLog2.prepare(sampleRate, kSmoothSeconds);
    quality.prepare(sampleRate, kSmoothSeconds);
    level.prepare(sampleRate, kSmoothSeconds);
    resetEffect();
  }

  void resetEffect() noexcept override {
    for (auto& filter : filters)
      filter.reset();
    position.snapOnNextTarget();
    heelLog2.snapOnNextTarget();
    toeLog2.snapOnNextTarget();
    quality.snapOnNextTarget();
    level.snapOnNextTarget();
    ticksLeft = 0;
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext&) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxChannels);

    const Voicing& voicing = currentVoicing();
    position.setTarget(param(kPositionPct) * 0.01f);
    heelLog2.setTarget(std::log2(voicing.heelHz));
    toeLog2.setTarget(std::log2(voicing.toeHz));
    quality.setTarget(fxfilter::resonanceToQ(param(kResonancePct)));
    level.setTarget(fx::dbToGain(param(kLevelDb)));

    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    for (int i = 0; i < numSamples; ++i) {
      const float pos = position.next();
      const float heel = heelLog2.next();
      const float toe = toeLog2.next();
      const float q = quality.next();
      const float gain = level.next();

      // The cutoff follows the pedal on a grid of kControlGrid samples that
      // runs across blocks, so it never depends on the block length.
      if (ticksLeft == 0) {
        // An exponential sweep: equal pedal travel is an equal musical
        // interval, as with the real pedal's taper.
        const double hz = std::exp2(static_cast<double>(heel + pos * (toe - heel)));
        for (int ch = 0; ch < numChannels; ++ch)
          filters[ch].set(hz, q, sampleRate, fxfilter::SweepFilter::Voice::Wah);
        ticksLeft = kControlGrid;
      }
      --ticksLeft;

      for (int ch = 0; ch < numChannels; ++ch)
        channels[ch][i] = gain * filters[ch].process(channels[ch][i]);
    }
  }

private:
  static constexpr int kMaxChannels = 2;
  static constexpr int kControlGrid = 16;
  // A heel-to-toe stomp lands in about this long; fast enough to feel
  // immediate, slow enough that no step reaches the filter.
  static constexpr double kPositionSmoothSeconds = 0.012;
  static constexpr double kSmoothSeconds = 0.02;

  const Voicing& currentVoicing() const noexcept {
    const int index = juce::jlimit(0, kNumVoicings - 1, static_cast<int>(std::lround(param(kVoicing))));
    return kVoicings[index];
  }

  fxfilter::SweepFilter filters[kMaxChannels];
  fx::SmoothedParam position, heelLog2, toeLog2, quality, level;
  double sampleRate{48000.0};
  int ticksLeft{0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(WahEffect) {
  addEffectKind<WahProcessor>(registry, "Wah", BlockCategory::Filter);
}

// dynamics.gate: a noise gate with a threshold, hysteresis, hold and a
// range (how far it closes), for taming amp hiss between notes.
//
// A fast peak detector decides open or closed; a smoothed gain follows the
// decision, so the gate opens and closes along a curve instead of switching
// (no clicks). It works on the signal as it arrives, with no look-ahead: the
// attack time is the price of that, and it is why the default is short.
#include <cmath>
#include "effects/EffectDsp.h"

namespace {

class NoiseGateProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "dynamics.gate";

  enum Param { kThresholdDb = 0, kAttackMs, kHoldMs, kReleaseMs, kRangeDb, kHysteresisDb };
  enum Reading { kGainReduction = 0 };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::floatParam("thresholdDb", "Threshold", "dB", -80.0f, 0.0f, -50.0f));
      t.push_back(BlockParamSpec::floatParam("attackMs", "Attack", "ms", 0.1f, 50.0f, 1.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("holdMs", "Hold", "ms", 0.0f, 500.0f, 40.0f)
                      .withSkew(0.5f));
      t.push_back(BlockParamSpec::floatParam("releaseMs", "Release", "ms", 5.0f, 2000.0f, 120.0f)
                      .withSkew(0.4f));
      // How much the gate attenuates when closed: -90 dB is a full mute.
      t.push_back(BlockParamSpec::floatParam("rangeDb", "Range", "dB", -90.0f, 0.0f, -60.0f));
      // The gate closes this far below the threshold it opened at, so a
      // note decaying through the threshold does not flutter.
      t.push_back(BlockParamSpec::floatParam("hysteresisDb", "Hysteresis", "dB", 0.0f, 12.0f, 3.0f));
      return t;
    }();
    return table;
  }

  // How far the gain is pulled down: the most within each processed block.
  static const BlockReadingTable& readingSpecs() {
    static const BlockReadingTable table = {
        {"gainReductionDb", "Gain reduction", "dB", 0.0f, 90.0f}};
    return table;
  }

  NoiseGateProcessor()
      : ParameterisedBlockProcessor(kKindId, parameterSpecs(), readingSpecs()) {}

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    detectorRelease = timeCoefficient(kDetectorReleaseMs);
    resetEffect();
  }

  void resetEffect() noexcept override {
    for (auto& lane : lanes)
      lane = Lane{};
    // The gate starts closed: the first block takes the range as its gain.
    fresh = true;
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxChannels);
    // One stereo signal is gated as one (both sides open and close together,
    // so the image does not wander); dual mono is two guitars, each its own.
    const bool linked = numChannels == 2 && !ctx.dualMono;
    const int numLanes = linked ? 1 : numChannels;

    const float threshold = param(kThresholdDb);
    const float openLevel = fx::dbToGain(threshold);
    const float closeLevel = fx::dbToGain(threshold - param(kHysteresisDb));
    const float rangeGain = fx::dbToGain(param(kRangeDb));
    const int holdSamples = static_cast<int>(std::lround(param(kHoldMs) * 0.001 * sampleRate));
    const float attack = stepCoefficient(param(kAttackMs));
    const float release = stepCoefficient(param(kReleaseMs));

    if (fresh) {
      for (auto& lane : lanes)
        lane.gain = rangeGain;
      fresh = false;
    }

    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    float leastGain = 1.0f;
    for (int lane = 0; lane < numLanes; ++lane) {
      Lane& s = lanes[lane];
      for (int i = 0; i < numSamples; ++i) {
        float level = std::abs(channels[lane][i]);
        if (linked)
          level = juce::jmax(level, std::abs(channels[1][i]));

        // Instant attack, short release: the level of the waveform's peaks,
        // not its ripple, so a low note does not open and close the gate
        // twice a cycle.
        s.level = level > s.level ? level : level + detectorRelease * (s.level - level);

        if (s.level > openLevel) {
          s.open = true;
          s.hold = holdSamples;
        } else if (s.open) {
          if (s.level > closeLevel)
            s.hold = holdSamples;
          else if (s.hold > 0)
            --s.hold;
          else
            s.open = false;
        }

        // The gain glides to the target (a one-pole, so it never steps).
        const float target = s.open ? 1.0f : rangeGain;
        s.gain += (target > s.gain ? attack : release) * (target - s.gain);

        channels[lane][i] *= s.gain;
        if (linked)
          channels[1][i] *= s.gain;
        leastGain = juce::jmin(leastGain, s.gain);
      }
    }
    setReading(kGainReduction, leastGain < 1.0f ? -fx::gainToDb(leastGain) : 0.0f);
  }

private:
  static constexpr int kMaxChannels = 2;
  // The level detector's own fall time, short enough to follow a decaying
  // note, long enough to bridge the half cycles of a low string.
  static constexpr double kDetectorReleaseMs = 8.0;

  struct Lane {
    float level{0.0f};
    float gain{0.0f};
    int hold{0};
    bool open{false};
  };

  // exp(-1/(time * rate)): the per-sample decay of a one-pole with that time
  // constant.
  float timeCoefficient(double ms) const noexcept {
    return ms > 0.0 ? static_cast<float>(std::exp(-1.0 / (0.001 * ms * sampleRate))) : 0.0f;
  }

  // 1 - exp(-1/(time * rate)), the share of the remaining distance covered
  // per sample; expm1 keeps it exact for the slow times, where the two
  // terms of the subtraction would agree to float precision.
  float stepCoefficient(double ms) const noexcept {
    return ms > 0.0 ? static_cast<float>(-std::expm1(-1.0 / (0.001 * ms * sampleRate))) : 1.0f;
  }

  Lane lanes[kMaxChannels];
  double sampleRate{48000.0};
  float detectorRelease{0.0f};
  bool fresh{true};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(NoiseGateEffect) {
  addEffectKind<NoiseGateProcessor>(registry, "Noise Gate", BlockCategory::Dynamics);
}

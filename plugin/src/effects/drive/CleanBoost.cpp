// drive.boost: a clean boost. A gain, a bass and a treble shelf, and a soft
// limit that only acts close to full scale, so the boost stays transparent
// until it is pushed against the ceiling. The limiter is a nonlinearity, so
// it runs at 4x (drive/DriveCommon.h).
//
//   in -> gain -> bass shelf (150 Hz) -> treble shelf (3 kHz) -> [4x soft limit]
#include "effects/drive/DriveCommon.h"

namespace {

using namespace drivefx;

class CleanBoostProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "drive.boost";

  enum Param { kGainDb = 0, kBassDb, kTrebleDb };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::floatParam("gainDb", "Gain", "dB", 0.0f, 20.0f, 2.0f));
      t.push_back(BlockParamSpec::floatParam("bassDb", "Bass", "dB", -12.0f, 12.0f, 0.0f));
      t.push_back(BlockParamSpec::floatParam("trebleDb", "Treble", "dB", -12.0f, 12.0f, 0.0f));
      return t;
    }();
    return table;
  }

  CleanBoostProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The half-band filters of the 4x oversampler delay every path by a few
  // samples (drive/DriveCommon.h).
  int latencySamples() const noexcept override { return kLatencySamples; }

  // The shelves are one-pole: the longest memory is the 150 Hz corner.
  double tailSeconds() const noexcept override { return 0.015; }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    oversampler.prepare(spec.maxBlockSize);
    gain.prepare(sampleRate, 0.02);
    bass.prepare(sampleRate, 0.02);
    treble.prepare(sampleRate, 0.02);
    for (Lane& lane : lanes) {
      lane.bass.setCorner(150.0, sampleRate);
      lane.treble.setCorner(3000.0, sampleRate);
    }
    resetEffect();
  }

  void resetEffect() noexcept override {
    oversampler.reset();
    for (Lane& lane : lanes) {
      lane.bass.reset();
      lane.treble.reset();
    }
    gain.snapOnNextTarget();
    bass.snapOnNextTarget();
    treble.snapOnNextTarget();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext&) override {
    const int numSamples = buffer.getNumSamples();
    const int numLanes = juce::jmin(buffer.getNumChannels(), kMaxLanes);
    gain.setTarget(fx::dbToGain(param(kGainDb)));
    bass.setTarget(fx::dbToGain(param(kBassDb)));
    treble.setTarget(fx::dbToGain(param(kTrebleDb)));

    for (int i = 0; i < numSamples; ++i) {
      const float g = gain.next();
      const float b = bass.next();
      const float t = treble.next();
      for (int ch = 0; ch < numLanes; ++ch) {
        Lane& lane = lanes[ch];
        float x = buffer.getReadPointer(ch)[i] * g;
        x = lane.bass.low(x, b);
        x = lane.treble.high(x, t);
        buffer.getWritePointer(ch)[i] = x;
      }
    }

    oversampler.run(buffer, [](float* const* io, int frames, int numReal) {
      for (int ch = 0; ch < numReal; ++ch)
        for (int i = 0; i < frames; ++i)
          io[ch][i] = softLimit(io[ch][i]);
    });
  }

private:
  // Unchanged up to the knee, then eases into 1.0: slope 1 at the knee, so
  // the join is smooth, and the output never passes full scale.
  static float softLimit(float x) noexcept {
    constexpr float kKnee = 0.8f;
    const float a = std::abs(x);
    if (a <= kKnee)
      return x;
    const float limited = kKnee + (1.0f - kKnee) * fx::fastTanh((a - kKnee) / (1.0f - kKnee));
    return x < 0.0f ? -limited : limited;
  }

  struct Lane {
    OnePoleShelf bass, treble;
  };

  Lane lanes[kMaxLanes];
  Oversampler4x oversampler;
  fx::SmoothedParam gain, bass, treble;
  double sampleRate{48000.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(CleanBoost) {
  addEffectKind<CleanBoostProcessor>(registry, "Clean Boost", BlockCategory::Drive);
}

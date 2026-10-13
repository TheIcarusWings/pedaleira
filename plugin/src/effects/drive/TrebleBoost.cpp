// drive.trebleboost: a boost focused on the top of the spectrum. A second
// order high-pass, whose corner is the Range control, is added back to the
// input scaled by the boost, so everything below the corner stays at unity
// and everything above it is lifted by Gain. The boosted signal then meets a
// mild asymmetric soft clipper, which only bites when the boost is pushed.
//
//   in -> [4x: x + (G-1) * highpass(x, range) -> asymmetric soft clip] -> DC block -> level
#include "effects/drive/DriveCommon.h"

namespace {

using namespace drivefx;

class TrebleBoostProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "drive.trebleboost";

  enum Param { kGainDb = 0, kRangeHz, kLevelDb };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::floatParam("gainDb", "Gain", "dB", 0.0f, 30.0f, 12.0f));
      t.push_back(BlockParamSpec::floatParam("rangeHz", "Range", "Hz", 400.0f, 4000.0f, 1000.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("levelDb", "Level", "dB", -18.0f, 18.0f, 0.0f));
      return t;
    }();
    return table;
  }

  TrebleBoostProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The half-band filters of the 4x oversampler delay every path by a few
  // samples (drive/DriveCommon.h).
  int latencySamples() const noexcept override { return kLatencySamples; }

  double tailSeconds() const noexcept override { return 0.05; }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    oversampler.prepare(spec.maxBlockSize);
    const double osRate = sampleRate * kOversampleFactor;
    gain.prepare(osRate, 0.02);
    range.prepare(osRate, 0.03);
    level.prepare(sampleRate, 0.02);
    shaper.setBias(kBias);
    for (Lane& lane : lanes)
      lane.dc.setCutoff(20.0, sampleRate);
    resetEffect();
  }

  void resetEffect() noexcept override {
    oversampler.reset();
    for (Lane& lane : lanes) {
      lane.highpass.reset();
      lane.dc.reset();
    }
    tick.reset();
    gain.snapOnNextTarget();
    range.snapOnNextTarget();
    level.snapOnNextTarget();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext&) override {
    const int numSamples = buffer.getNumSamples();
    const int numLanes = juce::jmin(buffer.getNumChannels(), kMaxLanes);
    gain.setTarget(fx::dbToGain(param(kGainDb)) - 1.0f);
    range.setTarget(param(kRangeHz));
    level.setTarget(fx::dbToGain(param(kLevelDb)) * kMakeup);

    const double osRate = sampleRate * kOversampleFactor;
    oversampler.run(buffer, [&](float* const* io, int frames, int numReal) {
      for (int i = 0; i < frames; ++i) {
        const float extra = gain.next();
        const float corner = range.next();
        if (tick.step())
          for (Lane& lane : lanes)
            lane.highpass.set(corner, 0.7071, osRate);
        for (int ch = 0; ch < numReal; ++ch) {
          Lane& lane = lanes[ch];
          const float x = io[ch][i];
          const float boosted = x + extra * lane.highpass.highpass(x);
          io[ch][i] = kThreshold * shaper(boosted * (1.0f / kThreshold));
        }
      }
    });

    for (int i = 0; i < numSamples; ++i) {
      const float g = level.next();
      for (int ch = 0; ch < numLanes; ++ch) {
        float* data = buffer.getWritePointer(ch);
        data[i] = lanes[ch].dc.process(data[i]) * g;
      }
    }
  }

private:
  // Where the clipper starts to act (in full-scale units) and how lopsided it
  // is: a small bias, so the two halves of a pushed wave clip a little apart.
  static constexpr float kThreshold = 0.5f;
  static constexpr float kBias = 0.3f;
  // Output scale that brings the default setting to about unity at guitar level.
  static constexpr float kMakeup = 1.17f;

  struct Lane {
    fx::StateVariableFilter highpass;
    DcBlocker dc;
  };

  Lane lanes[kMaxLanes];
  Oversampler4x oversampler;
  AsymmetricSoft shaper;
  ControlTick tick{kControlGridOs};
  fx::SmoothedParam gain, range, level;
  double sampleRate{48000.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(TrebleBoost) {
  addEffectKind<TrebleBoostProcessor>(registry, "Treble Boost", BlockCategory::Drive);
}

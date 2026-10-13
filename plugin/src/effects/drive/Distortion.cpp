// drive.distortion: an op-amp gain stage into a pair of hard clipping diodes
// to ground.
//
// The op-amp has a fixed gain-bandwidth product, so its bandwidth is that
// product divided by the gain: turning Distortion up makes the stage louder
// and also darker, the way the real circuit rolls off its own fizz. The
// diodes clip hard (a sharp knee, hardSat in DriveCommon.h) at a fixed level,
// so past the knee the loudness barely depends on the gain. The Filter
// control is a low-pass after the clipper that sweeps from dark to open.
//
//   in -> [4x: 90 Hz high-pass -> gain -> GBW roll-off -> hard clip] -> filter low-pass -> level
#include "effects/drive/DriveCommon.h"

namespace {

using namespace drivefx;

class DistortionProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "drive.distortion";

  enum Param { kDistortionPercent = 0, kFilterPercent, kLevelDb };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(
          BlockParamSpec::floatParam("distortionPercent", "Distortion", "%", 0.0f, 100.0f, 50.0f));
      t.push_back(BlockParamSpec::floatParam("filterPercent", "Filter", "%", 0.0f, 100.0f, 60.0f));
      t.push_back(BlockParamSpec::floatParam("levelDb", "Level", "dB", -48.0f, 18.0f, 0.0f));
      return t;
    }();
    return table;
  }

  DistortionProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The half-band filters of the 4x oversampler delay every path by a few
  // samples (drive/DriveCommon.h).
  int latencySamples() const noexcept override { return kLatencySamples; }

  double tailSeconds() const noexcept override { return 0.03; }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    oversampler.prepare(spec.maxBlockSize);
    const double osRate = sampleRate * kOversampleFactor;
    gain.prepare(osRate, 0.02);
    filter.prepare(sampleRate, 0.03);
    level.prepare(sampleRate, 0.02);
    for (Lane& lane : lanes)
      lane.highpass.setCutoff(90.0, osRate);
    resetEffect();
  }

  void resetEffect() noexcept override {
    oversampler.reset();
    for (Lane& lane : lanes) {
      lane.highpass.reset();
      lane.bandwidth.reset();
      lane.filter.reset();
    }
    osTick.reset();
    tick.reset();
    gain.snapOnNextTarget();
    filter.snapOnNextTarget();
    level.snapOnNextTarget();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext&) override {
    const int numSamples = buffer.getNumSamples();
    const int numLanes = juce::jmin(buffer.getNumChannels(), kMaxLanes);
    gain.setTarget(kMinGain * std::pow(kMaxGain / kMinGain, param(kDistortionPercent) * 0.01f));
    filter.setTarget(param(kFilterPercent) * 0.01f);
    level.setTarget(fx::dbToGain(param(kLevelDb)) * kMakeup);

    const double osRate = sampleRate * kOversampleFactor;
    oversampler.run(buffer, [&](float* const* io, int frames, int numReal) {
      for (int i = 0; i < frames; ++i) {
        const float amount = gain.next();
        if (osTick.step()) {
          const float bandwidthHz = juce::jlimit(kBandwidthFloorHz, kBandwidthCeilingHz,
                                                 kGainBandwidthHz / amount);
          for (Lane& lane : lanes)
            lane.bandwidth.setCutoff(bandwidthHz, osRate);
        }
        for (int ch = 0; ch < numReal; ++ch) {
          Lane& lane = lanes[ch];
          const float amplified = amount * lane.highpass.highpass(io[ch][i]);
          const float limited = lane.bandwidth.lowpass(amplified);
          io[ch][i] = kDiodeLevel * hardSat(limited * (1.0f / kDiodeLevel));
        }
      }
    });

    for (int i = 0; i < numSamples; ++i) {
      const float f = filter.next();
      const float g = level.next();
      if (tick.step())
        for (Lane& lane : lanes)
          lane.filter.set(expSweep(kFilterLowHz, kFilterHighHz, f), 0.7071, sampleRate);
      for (int ch = 0; ch < numLanes; ++ch) {
        float* data = buffer.getWritePointer(ch);
        data[i] = lanes[ch].filter.lowpass(data[i]) * g;
      }
    }
  }

private:
  static constexpr float kMinGain = 4.0f;      // Distortion 0
  static constexpr float kMaxGain = 600.0f;    // Distortion 100
  static constexpr float kGainBandwidthHz = 300000.0f;
  static constexpr float kBandwidthFloorHz = 1500.0f;
  static constexpr float kBandwidthCeilingHz = 16000.0f;
  static constexpr float kDiodeLevel = 0.3f;   // the clipping level, in full-scale units
  static constexpr float kFilterLowHz = 700.0f;
  static constexpr float kFilterHighHz = 12000.0f;
  // Output scale that brings the default setting to about unity at guitar level.
  static constexpr float kMakeup = 0.355f;

  struct Lane {
    fx::OnePole highpass, bandwidth;
    fx::StateVariableFilter filter;
  };

  Lane lanes[kMaxLanes];
  Oversampler4x oversampler;
  ControlTick osTick{kControlGridOs};
  ControlTick tick{kControlGrid};
  fx::SmoothedParam gain, filter, level;
  double sampleRate{48000.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(Distortion) {
  addEffectKind<DistortionProcessor>(registry, "Distortion", BlockCategory::Drive);
}

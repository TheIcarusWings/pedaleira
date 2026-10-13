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
// Voicing picks how that filter behaves. Classic is what is described above: a
// second order Filter (12 dB per octave) and a gain-bandwidth roll-off that
// darkens the sound as Distortion rises. Open keeps the same clipping but makes
// the response independent of Distortion and gentler: the pre-clip bandwidth is
// fixed at its ceiling, the input high-pass drops to 20 Hz so the lows stay
// full, and the Filter is a first order low-pass (6 dB per octave) that sweeps
// from about 470 Hz to 11.7 kHz (about 2.35 kHz at the middle).
//
//   in -> [4x: high-pass -> gain -> GBW roll-off -> hard clip] -> filter low-pass -> level
#include "effects/drive/DriveCommon.h"

namespace {

using namespace drivefx;

class DistortionProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "drive.distortion";

  enum Param { kDistortionPercent = 0, kFilterPercent, kLevelDb, kVoicing };
  enum Voicing { kClassic = 0, kOpen };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(
          BlockParamSpec::floatParam("distortionPercent", "Distortion", "%", 0.0f, 100.0f, 50.0f));
      t.push_back(BlockParamSpec::floatParam("filterPercent", "Filter", "%", 0.0f, 100.0f, 60.0f));
      t.push_back(BlockParamSpec::floatParam("levelDb", "Level", "dB", -48.0f, 18.0f, 0.0f));
      t.push_back(BlockParamSpec::choiceParam("voicing", "Voicing", juce::StringArray{"Classic", "Open"},
                                              kClassic)
                      .notAutomatable());
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
      lane.highpass.setCutoff(kHighpassHz, osRate);
    appliedVoicing = -1;
    resetEffect();
  }

  void resetEffect() noexcept override {
    oversampler.reset();
    for (Lane& lane : lanes) {
      lane.highpass.reset();
      lane.bandwidth.reset();
      lane.filter.reset();
      lane.pole.reset();
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
    const bool open = param(kVoicing) > 0.5f;
    if (static_cast<int>(open) != appliedVoicing) {
      for (Lane& lane : lanes) {
        lane.highpass.setCutoff(open ? kOpenHighpassHz : kHighpassHz, osRate);
        lane.filter.reset();
        lane.pole.reset();
      }
      appliedVoicing = static_cast<int>(open);
    }
    oversampler.run(buffer, [&](float* const* io, int frames, int numReal) {
      for (int i = 0; i < frames; ++i) {
        const float amount = gain.next();
        if (osTick.step()) {
          const float bandwidthHz = open ? kBandwidthCeilingHz
                                         : juce::jlimit(kBandwidthFloorHz, kBandwidthCeilingHz,
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
      if (tick.step()) {
        if (open)
          for (Lane& lane : lanes)
            lane.pole.setCutoff(expSweep(kOpenFilterLowHz, kOpenFilterHighHz, f), sampleRate);
        else
          for (Lane& lane : lanes)
            lane.filter.set(expSweep(kFilterLowHz, kFilterHighHz, f), 0.7071, sampleRate);
      }
      for (int ch = 0; ch < numLanes; ++ch) {
        float* data = buffer.getWritePointer(ch);
        Lane& lane = lanes[ch];
        data[i] = (open ? lane.pole.lowpass(data[i]) : lane.filter.lowpass(data[i])) * g;
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
  static constexpr float kHighpassHz = 90.0f;
  // The Open voicing: a one-pole Filter and a high-pass down in the sub-bass.
  static constexpr float kOpenHighpassHz = 20.0f;
  static constexpr float kOpenFilterLowHz = 471.0f;
  static constexpr float kOpenFilterHighHz = 11730.0f;
  // Output scale that brings the default setting to about unity at guitar level.
  static constexpr float kMakeup = 0.355f;

  struct Lane {
    fx::OnePole highpass, bandwidth;
    fx::StateVariableFilter filter;
    fx::OnePole pole;
  };

  Lane lanes[kMaxLanes];
  Oversampler4x oversampler;
  ControlTick osTick{kControlGridOs};
  ControlTick tick{kControlGrid};
  fx::SmoothedParam gain, filter, level;
  int appliedVoicing{-1};
  double sampleRate{48000.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(Distortion) {
  addEffectKind<DistortionProcessor>(registry, "Distortion", BlockCategory::Drive);
}

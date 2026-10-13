// drive.overdrive: a mid-forward overdrive built the way a single op-amp
// stage with clipping diodes in its feedback path behaves.
//
// The stage has unity gain for the signal itself and an extra gain, set by
// Drive, for the part of the signal above about 700 Hz (the high-pass in the
// gain network). The diodes sit across the feedback resistor, so they limit
// only that extra part:
//
//   y = x + L * softSat(k * highpass(x, 700 Hz) / L)
//
// At low Drive k is small, the extra part stays below the diode threshold and
// the stage is nearly linear (partly clean); at high Drive the extra part is
// squashed against +-L while the direct signal, lows included, still passes.
// Together with the roll-off above 6 kHz the result is a mid hump. The Tone
// control is a low-pass after the stage. The clipping runs at 4x.
//
//   in -> [4x: stage -> 6 kHz op-amp roll-off] -> tone low-pass -> level
#include "effects/drive/DriveCommon.h"

namespace {

using namespace drivefx;

class OverdriveProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "drive.overdrive";

  enum Param { kDrivePercent = 0, kTonePercent, kLevelDb };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::floatParam("drivePercent", "Drive", "%", 0.0f, 100.0f, 35.0f));
      t.push_back(BlockParamSpec::floatParam("tonePercent", "Tone", "%", 0.0f, 100.0f, 50.0f));
      t.push_back(BlockParamSpec::floatParam("levelDb", "Level", "dB", -18.0f, 18.0f, 0.0f));
      return t;
    }();
    return table;
  }

  OverdriveProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The half-band filters of the 4x oversampler delay every path by a few
  // samples (drive/DriveCommon.h).
  int latencySamples() const noexcept override { return kLatencySamples; }

  double tailSeconds() const noexcept override { return 0.02; }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    oversampler.prepare(spec.maxBlockSize);
    const double osRate = sampleRate * kOversampleFactor;
    drive.prepare(osRate, 0.02);
    tone.prepare(sampleRate, 0.03);
    level.prepare(sampleRate, 0.02);
    for (Lane& lane : lanes) {
      lane.highpass.setCutoff(700.0, osRate);
      lane.opamp.setCutoff(6000.0, osRate);
    }
    resetEffect();
  }

  void resetEffect() noexcept override {
    oversampler.reset();
    for (Lane& lane : lanes) {
      lane.highpass.reset();
      lane.opamp.reset();
      lane.tone.reset();
    }
    tick.reset();
    drive.snapOnNextTarget();
    tone.snapOnNextTarget();
    level.snapOnNextTarget();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext&) override {
    const int numSamples = buffer.getNumSamples();
    const int numLanes = juce::jmin(buffer.getNumChannels(), kMaxLanes);
    drive.setTarget(param(kDrivePercent) * 0.01f);
    tone.setTarget(param(kTonePercent) * 0.01f);
    level.setTarget(fx::dbToGain(param(kLevelDb)) * kMakeup);

    oversampler.run(buffer, [&](float* const* io, int frames, int numReal) {
      for (int i = 0; i < frames; ++i) {
        const float d = drive.next();
        const float extraGain = kMinGain + kGainSpan * d * d;
        for (int ch = 0; ch < numReal; ++ch) {
          Lane& lane = lanes[ch];
          const float x = io[ch][i];
          const float extra = lane.highpass.highpass(x);
          const float stage = x + kDiodeLevel * softSat(extraGain * extra * (1.0f / kDiodeLevel));
          io[ch][i] = lane.opamp.lowpass(stage);
        }
      }
    });

    for (int i = 0; i < numSamples; ++i) {
      const float t = tone.next();
      const float g = level.next();
      if (tick.step())
        for (Lane& lane : lanes)
          lane.tone.set(expSweep(kToneLowHz, kToneHighHz, t), 0.6, sampleRate);
      for (int ch = 0; ch < numLanes; ++ch) {
        float* data = buffer.getWritePointer(ch);
        data[i] = lanes[ch].tone.lowpass(data[i]) * g;
      }
    }
  }

private:
  // The extra gain runs from 0.5 (Drive 0) to 80.5 (Drive 100), quadratically,
  // so the lower half of the knob stays usable for light crunch.
  static constexpr float kMinGain = 0.5f;
  static constexpr float kGainSpan = 80.0f;
  // Where the feedback diodes limit the extra part, in full-scale units.
  static constexpr float kDiodeLevel = 0.3f;
  static constexpr float kToneLowHz = 500.0f;
  static constexpr float kToneHighHz = 8000.0f;
  // Output scale that brings the default setting to about unity at guitar level.
  static constexpr float kMakeup = 0.42f;

  struct Lane {
    fx::OnePole highpass, opamp;
    fx::StateVariableFilter tone;
  };

  Lane lanes[kMaxLanes];
  Oversampler4x oversampler;
  ControlTick tick{kControlGrid};
  fx::SmoothedParam drive, tone, level;
  double sampleRate{48000.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(Overdrive) {
  addEffectKind<OverdriveProcessor>(registry, "Overdrive", BlockCategory::Drive);
}

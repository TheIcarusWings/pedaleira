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
//
// Voicing picks the circuit. Classic is the stage above. The others are
// measured voicings (drive/DriveCommon.h, VoiceShape), each fitted to a family
// of pedals on a unit at the middle of their knobs: Drive sets the gain into
// the clipper, Tone moves the low-pass after it over 2.5 octaves (4 for
// Heavy) around the fitted corner, and the response, the distortion at 100 Hz and 1 kHz and how
// they follow Drive come from the fit.
//
//   Focused  a clean path plus a soft clipped, high-passed path, a gentle
//            low-mid lift and a top that falls from about 2 kHz: the mid hump
//            family, partly clean, with the level nearly fixed by the clipping.
//   Warm     the same layout with a broad lift below 500 Hz and a darker top.
//   Heavy    a fully clipped path with a large bass lift after it: loud lows,
//            a level that does not follow the input, the top set by Tone.
//
//   in -> [4x: voiced clipping] -> tone low-pass, peaks, high-pass -> level
#include "effects/drive/DriveCommon.h"

namespace {

using namespace drivefx;

class OverdriveProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "drive.overdrive";

  enum Param { kDrivePercent = 0, kTonePercent, kLevelDb, kVoicing };
  enum Voicing { kClassic = 0, kFocused, kWarm, kHeavy };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::floatParam("drivePercent", "Drive", "%", 0.0f, 100.0f, 35.0f));
      t.push_back(BlockParamSpec::floatParam("tonePercent", "Tone", "%", 0.0f, 100.0f, 50.0f));
      t.push_back(BlockParamSpec::floatParam("levelDb", "Level", "dB", -48.0f, 18.0f, 0.0f));
      t.push_back(BlockParamSpec::choiceParam(
                      "voicing", "Voicing",
                      juce::StringArray{"Classic", "Focused", "Warm", "Heavy"}, kClassic)
                      .notAutomatable()
                      .withAffectsTail());
      return t;
    }();
    return table;
  }

  OverdriveProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The half-band filters of the 4x oversampler delay every path by a few
  // samples (drive/DriveCommon.h).
  int latencySamples() const noexcept override { return kLatencySamples; }

  // The measured voicings carry high-passes down to a few Hz.
  double tailSeconds() const noexcept override { return param(kVoicing) > 0.5f ? 0.1 : 0.02; }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    oversampler.prepare(spec.maxBlockSize);
    const double osRate = sampleRate * kOversampleFactor;
    drive.prepare(osRate, 0.02);
    tone.prepare(sampleRate, 0.03);
    level.prepare(sampleRate, 0.02);
    gain.prepare(osRate, 0.02);
    voiced.prepare(sampleRate);
    for (Lane& lane : lanes) {
      lane.highpass.setCutoff(700.0, osRate);
      lane.opamp.setCutoff(6000.0, osRate);
    }
    appliedVoicing = -1;
    resetEffect();
  }

  void resetEffect() noexcept override {
    oversampler.reset();
    for (Lane& lane : lanes) {
      lane.highpass.reset();
      lane.opamp.reset();
      lane.tone.reset();
    }
    voiced.reset();
    tick.reset();
    gain.snapOnNextTarget();
    drive.snapOnNextTarget();
    tone.snapOnNextTarget();
    level.snapOnNextTarget();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext&) override {
    const int numSamples = buffer.getNumSamples();
    const int numLanes = juce::jmin(buffer.getNumChannels(), kMaxLanes);
    const int voicing = juce::jlimit(0, 3, static_cast<int>(param(kVoicing) + 0.5f));
    if (voicing != kClassic) {
      processVoiced(buffer, voicing, numSamples, numLanes);
      return;
    }
    appliedVoicing = kClassic;
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
  void processVoiced(juce::AudioBuffer<float>& buffer, int voicing, int numSamples, int numLanes) {
    if (voicing != appliedVoicing) {
      voiced.setShape(kShapes[voicing - 1]);
      appliedVoicing = voicing;
    }
    gain.setTarget(voiced.gainFor(param(kDrivePercent) * 0.01f));
    tone.setTarget(param(kTonePercent) * 0.01f);
    level.setTarget(fx::dbToGain(param(kLevelDb)));

    oversampler.run(buffer, [&](float* const* io, int frames, int numReal) {
      for (int i = 0; i < frames; ++i) {
        const float g = gain.next();
        for (int ch = 0; ch < numReal; ++ch)
          io[ch][i] = voiced.clipSample(ch, io[ch][i], g);
      }
    });

    for (int i = 0; i < numSamples; ++i) {
      const float t = tone.next();
      const float g = level.next();
      if (tick.step())
        voiced.setTone(t);
      for (int ch = 0; ch < numLanes; ++ch) {
        float* data = buffer.getWritePointer(ch);
        data[i] = voiced.postSample(ch, data[i]) * g;
      }
    }
  }

  // The measured voicings, in the order of the choice (Focused, Warm, Heavy).
  // Fitted to the unit (CorOS d14e) at the middle of each pedal's knobs.
  // clang-format off
  static constexpr VoiceShape kShapes[3] = {
      // Focused
      {.inputHz = 0.0f, .splitHz = 567.0f, .preLowpassHz = 20491.0f, .minGain = 9.84f,
       .maxGain = 100.9f, .clipLevel = 0.09527f, .knee = 2, .bias = -0.05335f, .clean = 1.029f,
       .secondHz = 0.0f, .secondGain = 1.0f, .secondLevel = 0.3f, .toneHz = 8108.0f, .toneQ = 0.3f,
       .toneOctaves = 2.5f, .lowPeakHz = 221.3f, .lowPeakQ = 0.15f, .lowPeakDb = 7.226f,
       .midPeakHz = 1000.0f, .midPeakQ = 0.7f, .midPeakDb = 0.0f, .highShelfHz = 1000.0f,
       .highShelfDb = 0.0f, .postHighpassHz = 37.73f, .makeup = 0.1778f},
      // Warm
      {.inputHz = 0.0f, .splitHz = 317.9f, .preLowpassHz = 22890.0f, .minGain = 6.517f,
       .maxGain = 138.0f, .clipLevel = 0.2397f, .knee = 2, .bias = 0.3902f, .clean = 1.404f,
       .secondHz = 0.0f, .secondGain = 1.0f, .secondLevel = 0.3f, .toneHz = 9487.0f, .toneQ = 0.3f,
       .toneOctaves = 2.5f, .lowPeakHz = 147.0f, .lowPeakQ = 0.15f, .lowPeakDb = 11.24f,
       .midPeakHz = 1000.0f, .midPeakQ = 0.7f, .midPeakDb = 0.0f, .highShelfHz = 1000.0f,
       .highShelfDb = 0.0f, .postHighpassHz = 5.0f, .makeup = 0.1f},
      // Heavy
      {.inputHz = 0.0f, .splitHz = 79.52f, .preLowpassHz = 23000.0f, .minGain = 21.23f,
       .maxGain = 102.9f, .clipLevel = 0.846f, .knee = 2, .bias = -0.00459f, .clean = 0.05665f,
       .secondHz = 0.0f, .secondGain = 1.0f, .secondLevel = 0.3f, .toneHz = 9020.0f, .toneQ = 0.5f,
       .toneOctaves = 4.0f, .lowPeakHz = 98.3f, .lowPeakQ = 0.3f, .lowPeakDb = 8.881f,
       .midPeakHz = 1000.0f, .midPeakQ = 0.7f, .midPeakDb = 0.0f, .highShelfHz = 1000.0f,
       .highShelfDb = 0.0f, .postHighpassHz = 8.843f, .makeup = 0.1084f},
  };
  // clang-format on

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
  fx::SmoothedParam gain;
  VoicedStage voiced;
  int appliedVoicing{-1};
  double sampleRate{48000.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(Overdrive) {
  addEffectKind<OverdriveProcessor>(registry, "Overdrive", BlockCategory::Drive);
}

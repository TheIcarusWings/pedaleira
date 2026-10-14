// drive.boost: a clean boost. A gain, a bass and a treble shelf, and a soft
// limit that only acts close to full scale, so the boost stays transparent
// until it is pushed against the ceiling. The limiter is a nonlinearity, so
// it runs at 4x (drive/DriveCommon.h).
//
//   in -> gain -> bass shelf (150 Hz) -> treble shelf (3 kHz) -> [4x soft limit]
//
// Voicing: Classic is the clean boost above. Warm and Hot are measured
// voicings (drive/DriveCommon.h, VoiceShape) for boosts that clip on their own
// when pushed: Gain drives a clipping stage fitted to the unit, then the two
// shelves shape the result.
//
//   Warm  a soft clipped boost with a broad lift below 500 Hz and a top that
//         falls from about 2 kHz; its level barely follows Gain.
//   Hot   a full range boost with a low cut near 70 Hz into a soft clipper:
//         clean at low Gain, gently clipping from the middle up.
//
//   in -> [4x: voiced clipping] -> post filters -> bass shelf -> treble shelf
#include "effects/drive/DriveCommon.h"

namespace {

using namespace drivefx;

class CleanBoostProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "drive.boost";

  enum Param { kGainDb = 0, kBassDb, kTrebleDb, kVoicing };
  enum Voicing { kClassic = 0, kWarm, kHot };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::floatParam("gainDb", "Gain", "dB", 0.0f, 20.0f, 2.0f));
      t.push_back(BlockParamSpec::floatParam("bassDb", "Bass", "dB", -12.0f, 12.0f, 0.0f));
      t.push_back(BlockParamSpec::floatParam("trebleDb", "Treble", "dB", -12.0f, 12.0f, 0.0f));
      t.push_back(BlockParamSpec::choiceParam("voicing", "Voicing",
                                              juce::StringArray{"Classic", "Warm", "Hot"}, kClassic)
                      .notAutomatable()
                      .withAffectsTail());
      return t;
    }();
    return table;
  }

  CleanBoostProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The half-band filters of the 4x oversampler delay every path by a few
  // samples (drive/DriveCommon.h).
  int latencySamples() const noexcept override { return kLatencySamples; }

  // The shelves are one-pole: the longest memory is the 150 Hz corner.
  // The measured voicings carry high-passes down to a few Hz.
  double tailSeconds() const noexcept override { return param(kVoicing) > 0.5f ? 0.1 : 0.015; }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    oversampler.prepare(spec.maxBlockSize);
    gain.prepare(sampleRate, 0.02);
    bass.prepare(sampleRate, 0.02);
    treble.prepare(sampleRate, 0.02);
    drive.prepare(sampleRate * kOversampleFactor, 0.02);
    voiced.prepare(sampleRate);
    appliedVoicing = -1;
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
    voiced.reset();
    drive.snapOnNextTarget();
    gain.snapOnNextTarget();
    bass.snapOnNextTarget();
    treble.snapOnNextTarget();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext&) override {
    const int numSamples = buffer.getNumSamples();
    const int numLanes = juce::jmin(buffer.getNumChannels(), kMaxLanes);
    const int voicing = juce::jlimit(0, 2, static_cast<int>(param(kVoicing) + 0.5f));
    if (voicing != kClassic) {
      processVoiced(buffer, voicing, numSamples, numLanes);
      return;
    }
    appliedVoicing = kClassic;
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
  // Gain 0..20 dB is the drive of the measured voicings, 0..1.
  void processVoiced(juce::AudioBuffer<float>& buffer, int voicing, int numSamples, int numLanes) {
    if (voicing != appliedVoicing) {
      voiced.setShape(kShapes[voicing - 1]);
      appliedVoicing = voicing;
    }
    drive.setTarget(voiced.gainFor(param(kGainDb) * (1.0f / 20.0f)));
    bass.setTarget(fx::dbToGain(param(kBassDb)));
    treble.setTarget(fx::dbToGain(param(kTrebleDb)));

    oversampler.run(buffer, [&](float* const* io, int frames, int numReal) {
      for (int i = 0; i < frames; ++i) {
        const float g = drive.next();
        for (int ch = 0; ch < numReal; ++ch)
          io[ch][i] = voiced.clipSample(ch, io[ch][i], g);
      }
    });

    for (int i = 0; i < numSamples; ++i) {
      const float b = bass.next();
      const float t = treble.next();
      gain.skip(1);
      for (int ch = 0; ch < numLanes; ++ch) {
        Lane& lane = lanes[ch];
        float* data = buffer.getWritePointer(ch);
        data[i] = lane.treble.high(lane.bass.low(voiced.postSample(ch, data[i]), b), t);
      }
    }
  }

  // The measured voicings, in the order of the choice (Warm, Hot). Fitted to
  // the unit (CorOS d14e) at the middle of each pedal's knobs.
  // clang-format off
  static constexpr VoiceShape kShapes[2] = {
      // Warm
      {.inputHz = 0.0f, .splitHz = 317.9f, .preLowpassHz = 22890.0f, .minGain = 6.517f,
       .maxGain = 138.0f, .clipLevel = 0.2397f, .knee = 2, .bias = 0.3902f, .clean = 1.404f,
       .secondHz = 0.0f, .secondGain = 1.0f, .secondLevel = 0.3f, .toneHz = 9487.0f, .toneQ = 0.3f,
       .toneOctaves = 2.5f, .lowPeakHz = 147.0f, .lowPeakQ = 0.15f, .lowPeakDb = 11.24f,
       .midPeakHz = 1000.0f, .midPeakQ = 0.7f, .midPeakDb = 0.0f, .highShelfHz = 1000.0f,
       .highShelfDb = 0.0f, .postHighpassHz = 5.0f, .makeup = 0.1303f},
      // Hot
      {.inputHz = 111.1f, .splitHz = 0.0f, .preLowpassHz = 0.0f, .minGain = 1.468f,
       .maxGain = 34.48f, .clipLevel = 0.1052f, .knee = 2, .bias = -0.001808f, .clean = 0.5f,
       .secondHz = 0.0f, .secondGain = 1.0f, .secondLevel = 0.3f, .toneHz = 21709.0f, .toneQ = 0.3f,
       .toneOctaves = 2.5f, .lowPeakHz = 100.0f, .lowPeakQ = 0.7f, .lowPeakDb = 0.0f,
       .midPeakHz = 4103.0f, .midPeakQ = 1.354f, .midPeakDb = -8.0f, .highShelfHz = 1000.0f,
       .highShelfDb = 0.0f, .postHighpassHz = 0.0f, .makeup = 0.7219f},
  };
  // clang-format on

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
  fx::SmoothedParam drive;
  VoicedStage voiced;
  int appliedVoicing{-1};
  double sampleRate{48000.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(CleanBoost) {
  addEffectKind<CleanBoostProcessor>(registry, "Clean Boost", BlockCategory::Drive);
}

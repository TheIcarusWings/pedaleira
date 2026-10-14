// drive.bass: an overdrive for bass that never touches the low end. The input
// is split at the Crossover frequency into a low band and a high band. The low
// band stays clean; only the high band is driven, and the driven band is
// blended with the clean high band by Blend.
//
//   low  = lowpass4(x)    highpass4(x) = high   (Linkwitz-Riley: two cascaded
//                                                 second order sections each)
//   y    = low + (1 - blend) * high + blend * L * asymSoft(k * high / L)
//
// The two bands of a Linkwitz-Riley split add up to a flat magnitude (an
// all-pass in phase), so with Blend at 0 the block changes no level; and the
// high band is a true 24 dB per octave high-pass, so a note well below the
// crossover reaches the clipper 40 dB or more down. Presence is a
// high shelf at 2.5 kHz after the sum. The split and the clipping run at 4x,
// together, so the clean and the driven paths stay aligned.
//
//   in -> [4x: split -> drive high band -> sum] -> presence shelf -> DC block -> level
//
// Voicing: Classic is the split drive above. DI, Boost and Grit are measured
// voicings (drive/DriveCommon.h, VoiceShape) fitted to bass pedals on a unit:
//
//   DI     a preamp that stays nearly clean: a fixed EQ (a low lift, a
//          presence lift) and Drive as its gain, with a soft clip that only
//          acts on loud input.
//   Boost  a full range boost with a low cut and a soft top, clean at bass
//          level and clipping gently when pushed; Drive is its gain.
//   Grit   a clean path plus a clipped, high-passed path, so the lows stay
//          clean while the mids and highs break up.
//
// In all three, Blend at its middle is the fitted mix of the clean and the
// clipped paths (it scales the clipped path from none at 0 to twice at 100;
// Boost has no clean path, so below the middle Blend fades from the boosted
// signal to the input, and above it changes nothing). In Grit, Crossover
// moves the high-pass in front of the clipped path from its fitted corner (at
// the default 200 Hz) by the same ratio. Presence is the same shelf as in
// Classic.
//
//   in -> [4x: voiced clipping] -> post filters -> presence shelf -> DC block -> level
#include "effects/drive/DriveCommon.h"

namespace {

using namespace drivefx;

class BassDriveProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "drive.bass";

  enum Param { kCrossoverHz = 0, kDrivePercent, kPresenceDb, kBlendPercent, kLevelDb, kVoicing };
  enum Voicing { kClassic = 0, kDi, kBoost, kGrit };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(
          BlockParamSpec::floatParam("crossoverHz", "Crossover", "Hz", 60.0f, 800.0f, 200.0f)
              .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("drivePercent", "Drive", "%", 0.0f, 100.0f, 40.0f));
      t.push_back(BlockParamSpec::floatParam("presenceDb", "Presence", "dB", -12.0f, 12.0f, 0.0f));
      t.push_back(BlockParamSpec::floatParam("blendPercent", "Blend", "%", 0.0f, 100.0f, 70.0f));
      t.push_back(BlockParamSpec::floatParam("levelDb", "Level", "dB", -48.0f, 18.0f, 0.0f));
      t.push_back(BlockParamSpec::choiceParam("voicing", "Voicing",
                                              juce::StringArray{"Classic", "DI", "Boost", "Grit"},
                                              kClassic)
                      .notAutomatable());
      return t;
    }();
    return table;
  }

  BassDriveProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The half-band filters of the 4x oversampler delay every path by a few
  // samples (drive/DriveCommon.h).
  int latencySamples() const noexcept override { return kLatencySamples; }

  // The lowest crossover (60 Hz, fourth order) is the longest memory.
  double tailSeconds() const noexcept override { return 0.15; }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    oversampler.prepare(spec.maxBlockSize);
    const double osRate = sampleRate * kOversampleFactor;
    crossover.prepare(osRate, 0.05);
    drive.prepare(osRate, 0.02);
    blend.prepare(osRate, 0.02);
    presence.prepare(sampleRate, 0.02);
    level.prepare(sampleRate, 0.02);
    shaper.setBias(kBias);
    for (Lane& lane : lanes) {
      lane.shelf.setCorner(2500.0, sampleRate);
      lane.dc.setCutoff(20.0, sampleRate);
    }
    gain.prepare(osRate, 0.02);
    voiced.prepare(sampleRate);
    appliedVoicing = -1;
    resetEffect();
  }

  void resetEffect() noexcept override {
    oversampler.reset();
    for (Lane& lane : lanes) {
      lane.lowA.reset();
      lane.lowB.reset();
      lane.highA.reset();
      lane.highB.reset();
      lane.shelf.reset();
      lane.dc.reset();
    }
    voiced.reset();
    tick.reset();
    gain.snapOnNextTarget();
    crossover.snapOnNextTarget();
    drive.snapOnNextTarget();
    blend.snapOnNextTarget();
    presence.snapOnNextTarget();
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
    crossover.setTarget(param(kCrossoverHz));
    drive.setTarget(param(kDrivePercent) * 0.01f);
    blend.setTarget(param(kBlendPercent) * 0.01f);
    presence.setTarget(fx::dbToGain(param(kPresenceDb)));
    level.setTarget(fx::dbToGain(param(kLevelDb)) * kMakeup);

    const double osRate = sampleRate * kOversampleFactor;
    oversampler.run(buffer, [&](float* const* io, int frames, int numReal) {
      for (int i = 0; i < frames; ++i) {
        const float corner = crossover.next();
        const float d = drive.next();
        const float b = blend.next();
        const float k = kMinGain + kGainSpan * d * d;
        if (tick.step())
          for (Lane& lane : lanes) {
            lane.lowA.set(corner, 0.7071, osRate);
            lane.lowB.set(corner, 0.7071, osRate);
            lane.highA.set(corner, 0.7071, osRate);
            lane.highB.set(corner, 0.7071, osRate);
          }
        for (int ch = 0; ch < numReal; ++ch) {
          Lane& lane = lanes[ch];
          const float x = io[ch][i];
          const float low = lane.lowB.lowpass(lane.lowA.lowpass(x));
          const float high = lane.highB.highpass(lane.highA.highpass(x));
          const float driven = kLevel * shaper(k * high * (1.0f / kLevel));
          io[ch][i] = low + (1.0f - b) * high + b * driven;
        }
      }
    });

    for (int i = 0; i < numSamples; ++i) {
      const float p = presence.next();
      const float g = level.next();
      for (int ch = 0; ch < numLanes; ++ch) {
        Lane& lane = lanes[ch];
        float* data = buffer.getWritePointer(ch);
        data[i] = lane.dc.process(lane.shelf.high(data[i], p)) * g;
      }
    }
  }

private:
  void processVoiced(juce::AudioBuffer<float>& buffer, int voicing, int numSamples, int numLanes) {
    const VoiceShape& shape = kShapes[voicing - 1];
    if (voicing != appliedVoicing) {
      voiced.setShape(shape);
      appliedVoicing = voicing;
    }
    crossover.setTarget(param(kCrossoverHz));
    gain.setTarget(voiced.gainFor(param(kDrivePercent) * 0.01f));
    blend.setTarget(param(kBlendPercent) * 0.02f);
    presence.setTarget(fx::dbToGain(param(kPresenceDb)));
    level.setTarget(fx::dbToGain(param(kLevelDb)));

    oversampler.run(buffer, [&](float* const* io, int frames, int numReal) {
      for (int i = 0; i < frames; ++i) {
        const float corner = crossover.next();
        const float g = gain.next();
        const float wet = blend.next();
        if (shape.splitHz > 0.0f && tick.step())
          voiced.setSplitHz(shape.splitHz * corner * (1.0f / 200.0f));
        for (int ch = 0; ch < numReal; ++ch) {
          const float x = io[ch][i];
          io[ch][i] = shape.clean > 0.0f
                          ? voiced.clipSample(ch, x, g, wet)
                          : voiced.clipSample(ch, x, g, juce::jmin(1.0f, wet)) +
                                juce::jmax(0.0f, 1.0f - wet) * x;
        }
      }
    });

    for (int i = 0; i < numSamples; ++i) {
      const float p = presence.next();
      const float g = level.next();
      for (int ch = 0; ch < numLanes; ++ch) {
        Lane& lane = lanes[ch];
        float* data = buffer.getWritePointer(ch);
        data[i] = lane.dc.process(lane.shelf.high(voiced.postSample(ch, data[i]), p)) * g;
      }
    }
  }

  // The measured voicings, in the order of the choice (DI, Boost, Grit).
  // Fitted to the unit (CorOS d14e) at the middle of each pedal's knobs.
  // clang-format off
  static constexpr VoiceShape kShapes[3] = {
      // DI
      {.inputHz = 58.21f, .splitHz = 0.0f, .preLowpassHz = 0.0f, .minGain = 0.3f, .maxGain = 1.667f,
       .clipLevel = 0.1863f, .knee = 2, .bias = 0.0f, .clean = 1.0f, .secondHz = 0.0f,
       .secondGain = 1.0f, .secondLevel = 0.3f, .toneHz = 8390.0f, .toneQ = 1.3f,
       .toneOctaves = 2.5f, .lowPeakHz = 93.3f, .lowPeakQ = 0.9717f, .lowPeakDb = 11.62f,
       .midPeakHz = 2221.0f, .midPeakQ = 1.963f, .midPeakDb = 9.251f, .highShelfHz = 1000.0f,
       .highShelfDb = 0.0f, .postHighpassHz = 0.0f, .makeup = 0.5242f},
      // Boost
      {.inputHz = 41.78f, .splitHz = 0.0f, .preLowpassHz = 0.0f, .minGain = 2.865f,
       .maxGain = 18.07f, .clipLevel = 0.6693f, .knee = 8, .bias = 0.0f, .clean = 0.0f,
       .secondHz = 0.0f, .secondGain = 1.0f, .secondLevel = 0.3f, .toneHz = 15036.0f,
       .toneQ = 0.9602f, .toneOctaves = 2.5f, .lowPeakHz = 220.4f, .lowPeakQ = 0.612f,
       .lowPeakDb = 6.275f, .midPeakHz = 1614.0f, .midPeakQ = 0.3002f, .midPeakDb = 5.097f,
       .highShelfHz = 1000.0f, .highShelfDb = 0.0f, .postHighpassHz = 0.0f, .makeup = 0.2366f},
      // Grit
      {.inputHz = 0.0f, .splitHz = 237.6f, .preLowpassHz = 21901.0f, .minGain = 1.89f,
       .maxGain = 3.617f, .clipLevel = 0.01731f, .knee = 2, .bias = 0.004923f, .clean = 0.9708f,
       .secondHz = 0.0f, .secondGain = 1.0f, .secondLevel = 0.3f, .toneHz = 14592.0f, .toneQ = 0.3f,
       .toneOctaves = 2.5f, .lowPeakHz = 197.8f, .lowPeakQ = 0.6882f, .lowPeakDb = 12.0f,
       .midPeakHz = 4713.0f, .midPeakQ = 0.4382f, .midPeakDb = 3.466f, .highShelfHz = 1000.0f,
       .highShelfDb = 0.0f, .postHighpassHz = 0.0f, .makeup = 0.5012f},
  };
  // clang-format on

  static constexpr float kMinGain = 1.0f;
  static constexpr float kGainSpan = 40.0f;
  // Where the driven band tops out, in full-scale units, and its slight
  // asymmetry.
  static constexpr float kLevel = 0.25f;
  static constexpr float kBias = 0.15f;
  // Output scale that brings the default setting to about unity at guitar level.
  static constexpr float kMakeup = 0.63f;

  struct Lane {
    fx::StateVariableFilter lowA, lowB, highA, highB;
    OnePoleShelf shelf;
    DcBlocker dc;
  };

  Lane lanes[kMaxLanes];
  Oversampler4x oversampler;
  AsymmetricSoft shaper;
  ControlTick tick{kControlGridOs};
  fx::SmoothedParam crossover, drive, blend, presence, level;
  fx::SmoothedParam gain;
  VoicedStage voiced;
  int appliedVoicing{-1};
  double sampleRate{48000.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(BassDrive) {
  addEffectKind<BassDriveProcessor>(registry, "Bass Drive", BlockCategory::Drive);
}

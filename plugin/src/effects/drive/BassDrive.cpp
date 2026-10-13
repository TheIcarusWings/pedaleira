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
#include "effects/drive/DriveCommon.h"

namespace {

using namespace drivefx;

class BassDriveProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "drive.bass";

  enum Param { kCrossoverHz = 0, kDrivePercent, kPresenceDb, kBlendPercent, kLevelDb };

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
    tick.reset();
    crossover.snapOnNextTarget();
    drive.snapOnNextTarget();
    blend.snapOnNextTarget();
    presence.snapOnNextTarget();
    level.snapOnNextTarget();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext&) override {
    const int numSamples = buffer.getNumSamples();
    const int numLanes = juce::jmin(buffer.getNumChannels(), kMaxLanes);
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
  double sampleRate{48000.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(BassDrive) {
  addEffectKind<BassDriveProcessor>(registry, "Bass Drive", BlockCategory::Drive);
}

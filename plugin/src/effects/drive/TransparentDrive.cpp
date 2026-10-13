// drive.transparent: a drive that keeps the player's own tone on top. Two
// paths are summed: the clean input, untouched and full range, and a clipped
// path that is the input high-passed at 120 Hz (so the lows never reach the
// clipper), amplified, soft clipped and rolled off above 8 kHz.
//
//   y = x + w * L * softSat(k * lowpass(highpass(x, 120 Hz), 8 kHz) / L)
//
// Drive raises both the gain k and the share w of the clipped path. At low
// Drive the clipped path is a light compression-like texture around a clean
// signal; at high Drive it dominates the mids and highs while the lows stay
// as clean and full as they came in. Treble is a high shelf at 3 kHz after
// the sum. The clipping runs at 4x.
//
//   in -> [4x: clean + clipped path] -> treble shelf -> level
#include "effects/drive/DriveCommon.h"

namespace {

using namespace drivefx;

class TransparentDriveProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "drive.transparent";

  enum Param { kDrivePercent = 0, kTrebleDb, kLevelDb };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::floatParam("drivePercent", "Drive", "%", 0.0f, 100.0f, 35.0f));
      t.push_back(BlockParamSpec::floatParam("trebleDb", "Treble", "dB", -12.0f, 12.0f, 0.0f));
      t.push_back(BlockParamSpec::floatParam("levelDb", "Level", "dB", -18.0f, 18.0f, 0.0f));
      return t;
    }();
    return table;
  }

  TransparentDriveProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The half-band filters of the 4x oversampler delay every path by a few
  // samples (drive/DriveCommon.h).
  int latencySamples() const noexcept override { return kLatencySamples; }

  double tailSeconds() const noexcept override { return 0.03; }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    oversampler.prepare(spec.maxBlockSize);
    const double osRate = sampleRate * kOversampleFactor;
    drive.prepare(osRate, 0.02);
    treble.prepare(sampleRate, 0.02);
    level.prepare(sampleRate, 0.02);
    for (Lane& lane : lanes) {
      lane.highpass.setCutoff(120.0, osRate);
      lane.rolloff.setCutoff(8000.0, osRate);
      lane.shelf.setCorner(3000.0, sampleRate);
    }
    resetEffect();
  }

  void resetEffect() noexcept override {
    oversampler.reset();
    for (Lane& lane : lanes) {
      lane.highpass.reset();
      lane.rolloff.reset();
      lane.shelf.reset();
    }
    drive.snapOnNextTarget();
    treble.snapOnNextTarget();
    level.snapOnNextTarget();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext&) override {
    const int numSamples = buffer.getNumSamples();
    const int numLanes = juce::jmin(buffer.getNumChannels(), kMaxLanes);
    drive.setTarget(param(kDrivePercent) * 0.01f);
    treble.setTarget(fx::dbToGain(param(kTrebleDb)));
    level.setTarget(fx::dbToGain(param(kLevelDb)) * kMakeup);

    oversampler.run(buffer, [&](float* const* io, int frames, int numReal) {
      for (int i = 0; i < frames; ++i) {
        const float d = drive.next();
        const float k = kMinGain + kGainSpan * d * d;
        const float share = kMinShare + (1.0f - kMinShare) * d;
        const float compensation = 1.0f / (1.0f + kCompensation * d);
        for (int ch = 0; ch < numReal; ++ch) {
          Lane& lane = lanes[ch];
          const float x = io[ch][i];
          const float u = lane.rolloff.lowpass(lane.highpass.highpass(x));
          const float clipped = kClipLevel * softSat(k * u * (1.0f / kClipLevel));
          io[ch][i] = (x + share * clipped) * compensation;
        }
      }
    });

    for (int i = 0; i < numSamples; ++i) {
      const float t = treble.next();
      const float g = level.next();
      for (int ch = 0; ch < numLanes; ++ch) {
        float* data = buffer.getWritePointer(ch);
        data[i] = lanes[ch].shelf.high(data[i], t) * g;
      }
    }
  }

private:
  static constexpr float kMinGain = 1.0f;
  static constexpr float kGainSpan = 40.0f;
  static constexpr float kMinShare = 0.3f;
  // Where the clipped path tops out, in full-scale units.
  static constexpr float kClipLevel = 0.25f;
  // How much of the loudness the extra clipped path adds is taken back as
  // Drive rises, so the knob changes the texture more than the volume.
  static constexpr float kCompensation = 1.0f;
  // Output scale that brings the default setting to about unity at guitar level.
  static constexpr float kMakeup = 0.68f;

  struct Lane {
    fx::OnePole highpass, rolloff;
    OnePoleShelf shelf;
  };

  Lane lanes[kMaxLanes];
  Oversampler4x oversampler;
  fx::SmoothedParam drive, treble, level;
  double sampleRate{48000.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(TransparentDrive) {
  addEffectKind<TransparentDriveProcessor>(registry, "Transparent Drive", BlockCategory::Drive);
}

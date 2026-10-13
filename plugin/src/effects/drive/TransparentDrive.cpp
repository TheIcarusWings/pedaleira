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
//
// Voicing picks the layout. Classic is the one described above. Smooth is a
// softer, cleaner-mixed drive with a long top-end roll-off:
//
//   v = highpass(x, 20 Hz)
//   y = lowpass(limit(g * (v + w * L * softSat(k * highpass(v, 32 Hz) / L))))
//
// The clean path is the whole input (a 20 Hz high-pass only), and the clipped
// path is full range too (a 32 Hz high-pass in front of its clipper) with a
// low ceiling L (about -25 dBFS before the level control), so it adds a gritty
// edge to a mostly clean sum. Drive raises the clipper gain k and the share w
// of the clipped path; the clean path keeps the same gain g. A gentle soft limiter on the sum keeps loud notes
// in check, and the roll-off comes last: a one-pole low-pass at about 1.45 kHz
// when Treble is 0 dB, moving a sixth of an octave per dB (5.8 kHz at +12 dB,
// 360 Hz at -12 dB). Because the roll-off is after the clipping, the response
// to a sweep at a fixed level is the same at any level and at any Drive; the
// price is that the harmonics of a mid note are rolled off with it, so a tone of
// 1 kHz measures about half the distortion of a tone of 100 Hz.
//
//   in -> [4x: high-pass -> clean + low-level clip -> limiter -> treble low-pass] -> level
#include "effects/drive/DriveCommon.h"

namespace {

using namespace drivefx;

class TransparentDriveProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "drive.transparent";

  enum Param { kDrivePercent = 0, kTrebleDb, kLevelDb, kVoicing };
  enum Voicing { kClassic = 0, kSmooth };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::floatParam("drivePercent", "Drive", "%", 0.0f, 100.0f, 35.0f));
      t.push_back(BlockParamSpec::floatParam("trebleDb", "Treble", "dB", -12.0f, 12.0f, 0.0f));
      t.push_back(BlockParamSpec::floatParam("levelDb", "Level", "dB", -48.0f, 18.0f, 0.0f));
      t.push_back(BlockParamSpec::choiceParam("voicing", "Voicing", juce::StringArray{"Classic", "Smooth"},
                                              kClassic)
                      .notAutomatable()
                      .withAffectsTail());
      return t;
    }();
    return table;
  }

  TransparentDriveProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The half-band filters of the 4x oversampler delay every path by a few
  // samples (drive/DriveCommon.h).
  int latencySamples() const noexcept override { return kLatencySamples; }

  // Smooth has high-passes down at 20 Hz and 32 Hz, which ring out for longer.
  double tailSeconds() const noexcept override { return param(kVoicing) > 0.5f ? 0.08 : 0.03; }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    oversampler.prepare(spec.maxBlockSize);
    const double osRate = sampleRate * kOversampleFactor;
    drive.prepare(osRate, 0.02);
    corner.prepare(osRate, 0.02);
    treble.prepare(sampleRate, 0.02);
    level.prepare(sampleRate, 0.02);
    for (Lane& lane : lanes) {
      lane.highpass.setCutoff(kHighpassHz, osRate);
      lane.rolloff.setCutoff(kRolloffHz, osRate);
      lane.shelf.setCorner(3000.0, sampleRate);
      lane.sidechain.setCutoff(kSmoothSidechainHz, osRate);
    }
    appliedVoicing = -1;
    resetEffect();
  }

  void resetEffect() noexcept override {
    oversampler.reset();
    for (Lane& lane : lanes) {
      lane.highpass.reset();
      lane.rolloff.reset();
      lane.sidechain.reset();
      lane.shelf.reset();
    }
    drive.snapOnNextTarget();
    corner.snapOnNextTarget();
    osTick.reset();
    treble.snapOnNextTarget();
    level.snapOnNextTarget();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext&) override {
    const int numSamples = buffer.getNumSamples();
    const int numLanes = juce::jmin(buffer.getNumChannels(), kMaxLanes);
    drive.setTarget(param(kDrivePercent) * 0.01f);
    treble.setTarget(fx::dbToGain(param(kTrebleDb)));
    const bool smooth = param(kVoicing) > 0.5f;
    level.setTarget(fx::dbToGain(param(kLevelDb)) * (smooth ? 1.0f : kMakeup));
    corner.setTarget(kSmoothCornerHz * std::pow(2.0f, param(kTrebleDb) * (1.0f / 6.0f)));

    const double osRate = sampleRate * kOversampleFactor;
    if (static_cast<int>(smooth) != appliedVoicing) {
      for (Lane& lane : lanes) {
        lane.highpass.setCutoff(smooth ? kSmoothHighpassHz : kHighpassHz, osRate);
        lane.rolloff.setCutoff(smooth ? kSmoothCornerHz : kRolloffHz, osRate);
        lane.highpass.reset();
        lane.rolloff.reset();
        lane.sidechain.reset();
      }
      appliedVoicing = static_cast<int>(smooth);
    }

    if (smooth) {
      oversampler.run(buffer, [&](float* const* io, int frames, int numReal) {
        for (int i = 0; i < frames; ++i) {
          const float d = drive.next();
          const float cornerHz = corner.next();
          if (osTick.step())
            for (Lane& lane : lanes)
              lane.rolloff.setCutoff(cornerHz, osRate);
          const float k = 1.0f + kSmoothGainSpan * d * d;
          const float share = kMinShare + (1.0f - kMinShare) * d;
          for (int ch = 0; ch < numReal; ++ch) {
            Lane& lane = lanes[ch];
            const float v = lane.highpass.highpass(io[ch][i]);
            const float u = lane.sidechain.highpass(v);
            const float clipped = kSmoothClipLevel * softSat(k * u * (1.0f / kSmoothClipLevel));
            const float sum = (v + share * clipped) * kSmoothClean;
            io[ch][i] = lane.rolloff.lowpass(kSmoothLimit * softSat(sum * (1.0f / kSmoothLimit)));
          }
        }
      });
      for (int i = 0; i < numSamples; ++i) {
        const float g = level.next();
        treble.skip(1);
        for (int ch = 0; ch < numLanes; ++ch) {
          float* data = buffer.getWritePointer(ch);
          data[i] *= g;
        }
      }
      return;
    }

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
  static constexpr float kHighpassHz = 120.0f;
  static constexpr float kRolloffHz = 8000.0f;
  // The Smooth voicing (see the header comment).
  static constexpr float kSmoothHighpassHz = 20.0f;
  static constexpr float kSmoothSidechainHz = 32.0f;
  static constexpr float kSmoothCornerHz = 1450.0f;
  static constexpr float kSmoothGainSpan = 15.3f;
  static constexpr float kSmoothClipLevel = 0.0551f;
  // The gain of the clean path (before the level control), whatever Drive is.
  static constexpr float kSmoothClean = 1.503f;
  static constexpr float kSmoothLimit = 0.2945f;
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
    fx::OnePole highpass, rolloff, sidechain;
    OnePoleShelf shelf;
  };

  Lane lanes[kMaxLanes];
  Oversampler4x oversampler;
  ControlTick osTick{kControlGridOs};
  fx::SmoothedParam drive, corner, treble, level;
  int appliedVoicing{-1};
  double sampleRate{48000.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(TransparentDrive) {
  addEffectKind<TransparentDriveProcessor>(registry, "Transparent Drive", BlockCategory::Drive);
}

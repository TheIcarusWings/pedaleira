// drive.bassfuzz: a fuzz for bass that keeps the low end. The input is split
// at the Low Retain frequency by a Linkwitz-Riley crossover; the low band
// always goes through clean, and only the band above it is fuzzed. Blend
// moves the upper band from clean to fuzz, so the two bands always add up
// to a flat magnitude with the Blend at 0, and the fundamental of a low note
// is carried by the clean band however hard the fuzz is pushed.
//
//   low  = lowpass4(x)    highpass4(x) = high   (two cascaded Butterworth
//                                                 sections each)
//   fuzz = highpass2(tone(roll(stage2(couple(stage1(g * gate(high)))))))
//   y    = low + (1 - blend) * high + blend * F * fuzz
//
// The fuzz is two clipping stages, the first hard and lopsided, the second
// soft, with a coupling high-pass between them. The second order high-pass
// at the same Low Retain corner after the fuzz keeps the fuzz out of the
// clean band: below the corner the output is the clean signal, above it the
// fuzz. At the lowest Low Retain (30 Hz) the whole bass range is fuzzed.
//
// Gate is the dead zone of a stage that runs out of bias, v^3 / (v^2 + z^2)
// on the input of the fuzz (so its threshold does not move with Drive). The
// zone widens as the output of the first stage dies away, so a low note
// (40 to 100 Hz, the slow swing of a bass string) sputters and cuts off
// before it fades, the way a starved fuzz gates on a decaying bass note,
// while the clean low band underneath carries on.
//
// Everything from the split to the sum runs at 4x, so the clean band and the
// fuzz see the same delay through the oversampler; the Level is applied at
// the base rate. Nothing clips outside the 4x section.
//
//   in -> [4x: split -> gate -> two stage fuzz -> tone -> high-pass -> sum] -> level
#include "effects/drive/DriveCommon.h"

namespace {

using namespace drivefx;

class BassFuzzProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "drive.bassfuzz";

  enum Param { kDrivePercent = 0, kTonePercent, kLowRetainHz, kBlendPercent, kGatePercent, kLevelDb };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::floatParam("drivePercent", "Drive", "%", 0.0f, 100.0f, 60.0f));
      t.push_back(BlockParamSpec::floatParam("tonePercent", "Tone", "%", 0.0f, 100.0f, 50.0f));
      t.push_back(
          BlockParamSpec::floatParam("lowRetainHz", "Low Retain", "Hz", 30.0f, 300.0f, 100.0f)
              .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("blendPercent", "Blend", "%", 0.0f, 100.0f, 80.0f));
      t.push_back(BlockParamSpec::floatParam("gatePercent", "Gate", "%", 0.0f, 100.0f, 30.0f));
      t.push_back(BlockParamSpec::floatParam("levelDb", "Level", "dB", -48.0f, 18.0f, 0.0f));
      return t;
    }();
    return table;
  }

  BassFuzzProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The half-band filters of the 4x oversampler delay every path by a few
  // samples (drive/DriveCommon.h).
  int latencySamples() const noexcept override { return kLatencySamples; }

  // The lowest crossover (30 Hz, fourth order) rings longest, and the fuzz
  // lifts what is left of it until the gate closes (measured about 0.02 s
  // to -60 dB at 150 Hz; the margin is for the lowest corner).
  double tailSeconds() const noexcept override { return 0.2; }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    oversampler.prepare(spec.maxBlockSize);
    const double osRate = sampleRate * kOversampleFactor;
    retain.prepare(osRate, 0.05);
    gain.prepare(osRate, 0.02);
    blend.prepare(osRate, 0.02);
    zone.prepare(osRate, 0.02);
    tone.prepare(osRate, 0.03);
    level.prepare(sampleRate, 0.02);
    starveAttack = static_cast<float>(1.0 - std::exp(-1.0 / (0.005 * osRate)));
    starveRelease = static_cast<float>(1.0 - std::exp(-1.0 / (0.12 * osRate)));
    for (Lane& lane : lanes) {
      lane.coupling.setCutoff(kCouplingHz, osRate);
      lane.rolloff.setCutoff(kRolloffHz, osRate);
    }
    shiftOffset = hardSat(kShift);
    resetEffect();
  }

  void resetEffect() noexcept override {
    oversampler.reset();
    for (Lane& lane : lanes) {
      lane.lowA.reset();
      lane.lowB.reset();
      lane.highA.reset();
      lane.highB.reset();
      lane.coupling.reset();
      lane.rolloff.reset();
      lane.toneLow.reset();
      lane.post.reset();
      lane.starve = 0.0f;
    }
    tick.reset();
    retain.snapOnNextTarget();
    gain.snapOnNextTarget();
    blend.snapOnNextTarget();
    zone.snapOnNextTarget();
    tone.snapOnNextTarget();
    level.snapOnNextTarget();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext&) override {
    const int numSamples = buffer.getNumSamples();
    const int numLanes = juce::jmin(buffer.getNumChannels(), kMaxLanes);
    const float drive = param(kDrivePercent) * 0.01f;
    const float gate = param(kGatePercent) * 0.01f;
    retain.setTarget(param(kLowRetainHz));
    gain.setTarget(kMinGain * std::pow(kMaxGain / kMinGain, drive));
    blend.setTarget(param(kBlendPercent) * 0.01f);
    zone.setTarget(kGateZone * gate * gate);
    tone.setTarget(param(kTonePercent) * 0.01f);
    level.setTarget(fx::dbToGain(param(kLevelDb)));

    const double osRate = sampleRate * kOversampleFactor;
    oversampler.run(buffer, [&](float* const* io, int frames, int numReal) {
      for (int i = 0; i < frames; ++i) {
        const float corner = retain.next();
        const float g = gain.next();
        const float b = blend.next();
        const float z = zone.next();
        const float t = tone.next();
        if (tick.step()) {
          const float toneHz = expSweep(kToneLowHz, kToneHighHz, t);
          for (Lane& lane : lanes) {
            lane.lowA.set(corner, kButterworthQ, osRate);
            lane.lowB.set(corner, kButterworthQ, osRate);
            lane.highA.set(corner, kButterworthQ, osRate);
            lane.highB.set(corner, kButterworthQ, osRate);
            lane.post.set(corner, kButterworthQ, osRate);
            lane.toneLow.set(toneHz, kButterworthQ, osRate);
          }
        }
        for (int ch = 0; ch < numReal; ++ch) {
          Lane& lane = lanes[ch];
          const float x = io[ch][i];
          const float low = lane.lowB.lowpass(lane.lowA.lowpass(x));
          const float high = lane.highB.highpass(lane.highA.highpass(x));

          // The starving stage: the quieter the recent output, the wider
          // the dead zone.
          const float fed = juce::jmin(1.0f, lane.starve * (1.0f / kFirstLevel));
          const float width = z * (1.0f + kStarveWiden * (1.0f - fed));
          const float gated = high * high * high / (high * high + width * width + 1.0e-12f);

          const float first = kFirstLevel * (hardSat(g * gated * (1.0f / kFirstLevel) + kShift) - shiftOffset);
          const float magnitude = std::abs(first);
          lane.starve += (magnitude > lane.starve ? starveAttack : starveRelease) * (magnitude - lane.starve);
          lane.starve = fx::flushDenormal(lane.starve);

          const float coupled = lane.coupling.highpass(first);
          const float second = kSecondLevel * softSat(kSecondGain * coupled * (1.0f / kSecondLevel));
          const float fuzz = lane.post.highpass(lane.toneLow.lowpass(lane.rolloff.lowpass(second)));

          io[ch][i] = low + (1.0f - b) * high + b * kFuzzOut * fuzz;
        }
      }
    });

    for (int i = 0; i < numSamples; ++i) {
      const float g = level.next();
      for (int ch = 0; ch < numLanes; ++ch)
        buffer.getWritePointer(ch)[i] *= g;
    }
  }

private:
  static constexpr double kButterworthQ = 0.7071067811865476;
  // Gain into the first stage over the Drive travel (exponential).
  static constexpr float kMinGain = 3.0f;
  static constexpr float kMaxGain = 300.0f;
  // First stage: where it tops out (full-scale units) and its lopsidedness.
  static constexpr float kFirstLevel = 0.3f;
  static constexpr float kShift = 0.12f;
  // Second stage, after the coupling high-pass.
  static constexpr float kSecondGain = 3.0f;
  static constexpr float kSecondLevel = 0.25f;
  static constexpr double kCouplingHz = 30.0;
  static constexpr double kRolloffHz = 6500.0;
  static constexpr float kToneLowHz = 500.0f;
  static constexpr float kToneHighHz = 8000.0f;
  // The dead zone at Gate 100 %, in full-scale units of the input, and how
  // much wider it gets once the output has died away.
  static constexpr float kGateZone = 0.02f;
  static constexpr float kStarveWiden = 3.0f;
  // Scale of the fuzz against the clean band: the default setting comes out
  // at about unity at bass level.
  static constexpr float kFuzzOut = 0.4f;

  struct Lane {
    fx::StateVariableFilter lowA, lowB, highA, highB;
    fx::OnePole coupling, rolloff;
    fx::StateVariableFilter toneLow, post;
    float starve{0.0f};
  };

  Lane lanes[kMaxLanes];
  Oversampler4x oversampler;
  ControlTick tick{kControlGridOs};
  fx::SmoothedParam retain, gain, blend, zone, tone, level;
  float starveAttack{0.01f}, starveRelease{0.001f};
  float shiftOffset{0.0f};
  double sampleRate{48000.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(BassFuzz) {
  addEffectKind<BassFuzzProcessor>(registry, "Bass Fuzz", BlockCategory::Drive);
}

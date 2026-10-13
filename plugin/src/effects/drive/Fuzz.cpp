// drive.fuzz: a fuzz with three voicings, each a different way a transistor
// stage goes wrong in a pleasing way.
//
//   Silicon    a bright, tight fuzz: a lot of gain into a hard, slightly
//              lopsided clip.
//   Germanium  a softer, lower gain fuzz that is strongly asymmetric. At low
//              Bias the stage sits near cut-off: small signals fall in a dead
//              zone and are gated, and the dead zone widens as the note
//              decays (the stage starves), so the tail sputters and dies
//              before it fades.
//   Sustain    two clipping stages in series with a coupling filter between
//              them: a smooth, compressed, very long sustain. Its Tone is a
//              low-pass and a high-pass mixed (a scoop in the middle when they
//              are balanced), not a single low-pass.
//
// The dead zone is a smooth function, v^3 / (v^2 + dz^2), which is v for a
// large input and falls away as v^3 below dz, so it adds no sharp corner of
// its own. Bias is the supply of the stage: 100 % is well fed and symmetric
// enough, lower pushes the dead zone and the asymmetry up (a little for the
// silicon and sustain voicings, a lot for germanium). The clipping runs at
// 4x; the asymmetry leaves DC, which is blocked after the tone.
//
//   in -> [4x: high-pass -> gain -> dead zone -> clip (-> coupling -> clip)
//         -> roll-off] -> tone -> DC block -> level
#include "effects/drive/DriveCommon.h"

namespace {

using namespace drivefx;

class FuzzProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "drive.fuzz";

  enum Param { kVoicing = 0, kFuzzPercent, kBiasPercent, kTonePercent, kLevelDb };
  enum Voicing { kSilicon = 0, kGermanium, kSustain };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::choiceParam(
                      "voicing", "Voicing", juce::StringArray{"Silicon", "Germanium", "Sustain"},
                      kSilicon)
                      .notAutomatable());
      t.push_back(BlockParamSpec::floatParam("fuzzPercent", "Fuzz", "%", 0.0f, 100.0f, 60.0f));
      t.push_back(BlockParamSpec::floatParam("biasPercent", "Bias", "%", 0.0f, 100.0f, 70.0f));
      t.push_back(BlockParamSpec::floatParam("tonePercent", "Tone", "%", 0.0f, 100.0f, 50.0f));
      t.push_back(BlockParamSpec::floatParam("levelDb", "Level", "dB", -48.0f, 18.0f, 0.0f));
      return t;
    }();
    return table;
  }

  FuzzProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The half-band filters of the 4x oversampler delay every path by a few
  // samples (drive/DriveCommon.h).
  int latencySamples() const noexcept override { return kLatencySamples; }

  double tailSeconds() const noexcept override { return 0.06; }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    oversampler.prepare(spec.maxBlockSize);
    const double osRate = sampleRate * kOversampleFactor;
    gain.prepare(osRate, 0.02);
    tone.prepare(sampleRate, 0.03);
    level.prepare(sampleRate, 0.02);
    starveAttack = static_cast<float>(1.0 - std::exp(-1.0 / (0.005 * osRate)));
    starveRelease = static_cast<float>(1.0 - std::exp(-1.0 / (0.15 * osRate)));
    for (Lane& lane : lanes) {
      lane.input.setCutoff(70.0, osRate);
      lane.coupling.setCutoff(250.0, osRate);
      lane.dc.setCutoff(25.0, sampleRate);
      lane.scoopLow.set(600.0, 0.7071, sampleRate);
      lane.scoopHigh.set(1800.0, 0.7071, sampleRate);
    }
    appliedVoicing = -1;
    resetEffect();
  }

  void resetEffect() noexcept override {
    oversampler.reset();
    for (Lane& lane : lanes) {
      lane.input.reset();
      lane.coupling.reset();
      lane.rolloff.reset();
      lane.starve = 0.0f;
      lane.toneLow.reset();
      lane.scoopLow.reset();
      lane.scoopHigh.reset();
      lane.dc.reset();
    }
    tick.reset();
    gain.snapOnNextTarget();
    tone.snapOnNextTarget();
    level.snapOnNextTarget();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext&) override {
    const int numSamples = buffer.getNumSamples();
    const int numLanes = juce::jmin(buffer.getNumChannels(), kMaxLanes);
    const int voicing = juce::jlimit(0, 2, static_cast<int>(param(kVoicing) + 0.5f));
    const float fuzz = param(kFuzzPercent) * 0.01f;
    const float starved = 1.0f - param(kBiasPercent) * 0.01f;  // 0 well fed .. 1 starved

    const Voicings& v = kVoicings[voicing];
    if (voicing != appliedVoicing) {
      const double osRate = sampleRate * kOversampleFactor;
      for (Lane& lane : lanes)
        lane.rolloff.setCutoff(v.rolloffHz, osRate);
      appliedVoicing = voicing;
    }
    gain.setTarget(v.minGain * std::pow(v.maxGain / v.minGain, fuzz));
    tone.setTarget(param(kTonePercent) * 0.01f);
    level.setTarget(fx::dbToGain(param(kLevelDb)) * v.makeup);

    const float deadZone = v.deadZone * starved * starved;
    const float shift = v.shift + v.starveShift * starved;
    const float shiftOffset = hardSat(shift);

    oversampler.run(buffer, [&](float* const* io, int frames, int numReal) {
      for (int i = 0; i < frames; ++i) {
        const float amount = gain.next();
        for (int ch = 0; ch < numReal; ++ch) {
          Lane& lane = lanes[ch];
          const float driven = amount * lane.input.highpass(io[ch][i]);

          // The stage starves as the note dies away: the louder the recent
          // output, the smaller the dead zone.
          const float fed = juce::jmin(1.0f, lane.starve * (1.0f / v.level));
          const float zone = deadZone * (1.0f + kStarveWiden * (1.0f - fed));
          const float gated = driven * driven * driven / (driven * driven + zone * zone + 1.0e-9f);

          float y;
          if (voicing == kSustain) {
            const float first = v.level * hardSat(gated * (1.0f / v.level) + shift) - v.level * shiftOffset;
            const float coupled = lane.coupling.highpass(first);
            y = kSecondLevel * softSat(kSecondGain * coupled * (1.0f / kSecondLevel));
          } else if (voicing == kGermanium) {
            y = v.level * (softSat(gated * (1.0f / v.level) + shift) - softSat(shift));
          } else {
            y = v.level * (hardSat(gated * (1.0f / v.level) + shift) - shiftOffset);
          }
          y = lane.rolloff.lowpass(y);

          const float magnitude = std::abs(y);
          lane.starve += (magnitude > lane.starve ? starveAttack : starveRelease) * (magnitude - lane.starve);
          io[ch][i] = y;
        }
      }
    });

    for (int i = 0; i < numSamples; ++i) {
      const float t = tone.next();
      const float g = level.next();
      if (voicing != kSustain && tick.step())
        for (Lane& lane : lanes)
          lane.toneLow.set(expSweep(kToneLowHz, kToneHighHz, t), 0.7071, sampleRate);
      for (int ch = 0; ch < numLanes; ++ch) {
        Lane& lane = lanes[ch];
        float* data = buffer.getWritePointer(ch);
        float x = data[i];
        if (voicing == kSustain)
          x = (1.0f - t) * lane.scoopLow.lowpass(x) + t * lane.scoopHigh.highpass(x);
        else
          x = lane.toneLow.lowpass(x);
        data[i] = lane.dc.process(x) * g;
      }
    }
  }

private:
  struct Voicings {
    float minGain, maxGain;  // gain range of the Fuzz control
    float level;             // clipping level of the (first) stage, full-scale units
    float deadZone;          // dead zone at Bias 0, in the amplified domain
    float shift;             // asymmetry of a well fed stage
    float starveShift;       // extra asymmetry at Bias 0
    float rolloffHz;
    float makeup;            // brings the default setting to about unity at guitar level
  };
  static constexpr Voicings kVoicings[3] = {
      // Silicon
      {4.0f, 320.0f, 0.25f, 0.08f, 0.05f, 0.10f, 9000.0f, 0.436f},
      // Germanium
      {2.0f, 50.0f, 0.30f, 0.60f, 0.20f, 0.30f, 5000.0f, 0.39f},
      // Sustain
      {6.0f, 600.0f, 0.40f, 0.05f, 0.00f, 0.05f, 8000.0f, 0.72f},
  };
  static constexpr float kSecondGain = 4.0f;
  static constexpr float kSecondLevel = 0.3f;
  // The dead zone grows by up to this factor when the output has died away.
  static constexpr float kStarveWiden = 3.0f;
  static constexpr float kToneLowHz = 600.0f;
  static constexpr float kToneHighHz = 9000.0f;

  struct Lane {
    fx::OnePole input, coupling, rolloff;
    float starve{0.0f};
    fx::StateVariableFilter toneLow, scoopLow, scoopHigh;
    DcBlocker dc;
  };

  Lane lanes[kMaxLanes];
  Oversampler4x oversampler;
  ControlTick tick{kControlGrid};
  fx::SmoothedParam gain, tone, level;
  float starveAttack{0.01f}, starveRelease{0.001f};
  int appliedVoicing{-1};
  double sampleRate{48000.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(Fuzz) {
  addEffectKind<FuzzProcessor>(registry, "Fuzz", BlockCategory::Drive);
}

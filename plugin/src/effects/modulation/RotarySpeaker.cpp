// modulation.rotary: a Leslie-style rotating speaker. A drive stage, then a
// Linkwitz-Riley crossover into a horn band and a drum band. Each band sees a
// rotor with its own speed and inertia: the sound is louder when the rotor
// points at the microphone (amplitude modulation) and arrives a fraction of a
// millisecond early or late as the source moves towards or away from it
// (Doppler, a short modulated delay). In a true stereo image two microphones
// sit at different angles round the cabinet.
#include <algorithm>
#include <cmath>
#include "effects/EffectDsp.h"

namespace {

class RotaryProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "modulation.rotary";

  // The first three are the standard rate parameters (fx::appendRateParams);
  // here the rate is the horn's fast speed.
  enum Param { kSync = 0, kDivision, kRateHz, kSpeed, kDepth, kDrive, kHornDb, kWidth };
  enum Speed { kSlow = 0, kFast, kBrake };

  // Time the rotors take to go from slow to fast (and back). The horn is
  // light and gets there in about a second; the drum is heavy.
  static constexpr double kHornRampSeconds = 1.2;
  static constexpr double kDrumRampSeconds = 3.0;
  // Slow speed as a share of fast (a Leslie 122: 48 rpm against 400 rpm), and
  // the drum's speed as a share of the horn's (342 rpm against 400 rpm).
  static constexpr double kSlowShare = 0.12;
  static constexpr double kDrumShare = 0.855;

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      fx::RateParamOptions rate;
      rate.name = "Fast Rate";
      rate.minHz = 1.0f;
      rate.maxHz = 12.0f;
      rate.defaultHz = 6.7f;
      fx::appendRateParams(t, rate);
      t.push_back(BlockParamSpec::choiceParam("speed", "Speed",
                                              juce::StringArray{"Slow", "Fast", "Brake"}, kSlow));
      t.push_back(BlockParamSpec::floatParam("depth", "Depth", "%", 0.0f, 100.0f, 80.0f));
      t.push_back(BlockParamSpec::floatParam("drive", "Drive", "%", 0.0f, 100.0f, 25.0f));
      t.push_back(BlockParamSpec::floatParam("hornDb", "Horn Level", "dB", -12.0f, 12.0f, 0.0f));
      t.push_back(BlockParamSpec::floatParam("width", "Width", "%", 0.0f, 100.0f, 60.0f));
      return t;
    }();
    return table;
  }

  RotaryProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The crossover's ringing (a few milliseconds) and the longest Doppler
  // delay (1.5 ms); the rotors' speed has no bearing on it.
  double tailSeconds() const noexcept override { return 0.03; }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    const int channels = juce::jlimit(1, kMaxChannels, spec.numChannels);
    const int lineSamples = static_cast<int>(std::ceil(kLongestDelaySeconds * sampleRate)) + 4;
    hornLine.prepare(channels, lineSamples);
    drumLine.prepare(channels, lineSamples);
    hornLfo.prepare(sampleRate);
    drumLfo.prepare(sampleRate);
    const fx::BiquadCoeffs low = fx::BiquadCoeffs::lowpass(kCrossoverHz, 0.70710678, sampleRate);
    const fx::BiquadCoeffs high = fx::BiquadCoeffs::highpass(kCrossoverHz, 0.70710678, sampleRate);
    for (auto& source : sources) {
      source.lowA.setCoeffs(low);
      source.lowB.setCoeffs(low);
      source.highA.setCoeffs(high);
      source.highB.setCoeffs(high);
    }
    depth.prepare(sampleRate, 0.03);
    drive.prepare(sampleRate, 0.03);
    hornGain.prepare(sampleRate, 0.03);
    width.prepare(sampleRate, 0.1);
    resetEffect();
  }

  void resetEffect() noexcept override {
    hornLine.clear();
    drumLine.clear();
    for (auto& source : sources) {
      source.lowA.reset();
      source.lowB.reset();
      source.highA.reset();
      source.highB.reset();
    }
    hornLfo.reset();
    drumLfo.reset();
    depth.snapOnNextTarget();
    drive.snapOnNextTarget();
    hornGain.snapOnNextTarget();
    width.snapOnNextTarget();
    // A block that comes back on turns at the speed it is set to, rather
    // than running up to it.
    snapRotors = true;
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), hornLine.numChannels(), kMaxChannels);
    const bool stereo = blockHasStereoImage(buffer.getNumChannels(), ctx);

    // Rotor targets and how fast they may move towards them (Hz per sample).
    const double fastHorn =
        fx::resolveRateHz(param(kSync), param(kDivision), param(kRateHz), ctx.transport);
    const double fastDrum = fastHorn * kDrumShare;
    const int speed = static_cast<int>(std::lround(param(kSpeed)));
    const double hornTarget = speed == kFast ? fastHorn : speed == kSlow ? fastHorn * kSlowShare : 0.0;
    const double drumTarget = speed == kFast ? fastDrum : speed == kSlow ? fastDrum * kSlowShare : 0.0;
    const double hornStep = fastHorn * (1.0 - kSlowShare) / (kHornRampSeconds * sampleRate);
    const double drumStep = fastDrum * (1.0 - kSlowShare) / (kDrumRampSeconds * sampleRate);
    if (snapRotors) {
      hornRate = hornTarget;
      drumRate = drumTarget;
      snapRotors = false;
    }

    depth.setTarget(param(kDepth) * 0.01f);
    drive.setTarget(param(kDrive) * 0.01f);
    hornGain.setTarget(fx::dbToGain(param(kHornDb)));
    width.setTarget(stereo ? param(kWidth) * 0.01f : 0.0f);

    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    for (int i = 0; i < numSamples; ++i) {
      hornRate = approach(hornRate, hornTarget, hornStep);
      drumRate = approach(drumRate, drumTarget, drumStep);
      hornLfo.setRateHz(hornRate);
      drumLfo.setRateHz(drumRate);

      const float amount = depth.next();
      const float push = drive.next();
      const float horn = hornGain.next();
      const float spread = width.next();
      // How far the sources' microphones are from facing the same way: 0.25
      // turns is "cosine" in valueAt's sine; left and right sit half a turn
      // apart at full width, centred on the cabinet's axis.
      const float micLeft = 0.25f - 0.25f * spread;
      const float micRight = 0.25f + 0.25f * spread;

      // A true stereo buffer is one cabinet with two microphones; anything
      // else is one cabinet per channel, so nothing crosses.
      const int numSources = stereo ? 1 : numChannels;
      float mixed[kMaxChannels] = {0.0f, 0.0f};
      for (int src = 0; src < numSources; ++src) {
        const float in = stereo ? 0.5f * (channels[0][i] + channels[1][i]) : channels[src][i];
        // Drive: a saturating stage with the small-signal gain of its own
        // setting, so turning it up thickens before it levels off. At 0 the
        // stage is out of the way entirely.
        const float gain = 1.0f + 9.0f * push;
        const float driven = in + push * (fx::fastTanh(gain * in) / std::sqrt(gain) - in);

        Source& band = sources[src];
        const float low = band.lowB.process(band.lowA.process(driven));
        const float high = band.highB.process(band.highA.process(driven));

        if (stereo) {
          mixed[0] = horn * microphone(hornLine, hornLfo, 0, micLeft, amount, kHornSwingSeconds, kHornDip) +
                     microphone(drumLine, drumLfo, 0, micLeft, amount, kDrumSwingSeconds, kDrumDip);
          mixed[1] = horn * microphone(hornLine, hornLfo, 0, micRight, amount, kHornSwingSeconds, kHornDip) +
                     microphone(drumLine, drumLfo, 0, micRight, amount, kDrumSwingSeconds, kDrumDip);
        } else {
          mixed[src] = horn * microphone(hornLine, hornLfo, src, 0.25f, amount, kHornSwingSeconds, kHornDip) +
                       microphone(drumLine, drumLfo, src, 0.25f, amount, kDrumSwingSeconds, kDrumDip);
        }
        hornLine.write(src, high);
        drumLine.write(src, low);
      }
      for (int ch = 0; ch < numChannels; ++ch)
        channels[ch][i] = mixed[ch];
      hornLfo.advance();
      drumLfo.advance();
    }
  }

private:
  static constexpr int kMaxChannels = 2;
  static constexpr double kCrossoverHz = 800.0;
  // The source's distance to the microphone changes by this much either side
  // of its mean as it goes round (the radius of the rotor over the speed of
  // sound: 15 cm and 19 cm), at full depth.
  static constexpr float kHornSwingSeconds = 0.00044f;
  static constexpr float kDrumSwingSeconds = 0.00055f;
  static constexpr float kCentreDelaySeconds = 0.001f;
  static constexpr double kLongestDelaySeconds = 0.002;
  // How much quieter the band is, at full depth, when the rotor points away
  // from the microphone than when it points at it: the horn is directional,
  // the drum (a baffle in front of a speaker) much less.
  static constexpr float kHornDip = 0.7f;
  static constexpr float kDrumDip = 0.45f;
  struct Source {
    fx::Biquad lowA, lowB, highA, highB;
  };

  static double approach(double value, double target, double step) noexcept {
    return value < target ? std::min(target, value + step) : std::max(target, value - step);
  }

  // One band heard by one microphone: the delayed and weighted line of a
  // source. `micTurns` is the microphone's angle round the cabinet in
  // valueAt's units (0.25 faces the rotor at phase 0).
  float microphone(const fx::DelayLine& line, const fx::Lfo& rotor, int source, float micTurns,
                   float amount, float swingSeconds, float dip) const noexcept {
    // 1 when the rotor points at the microphone, -1 when it points away.
    const float facing = rotor.valueAt(micTurns);
    const float samplesPerSecond = static_cast<float>(sampleRate);
    // Nearer when facing, so the delay is shortest there.
    const float delay = (kCentreDelaySeconds - swingSeconds * amount * facing) * samplesPerSecond;
    const float louder = 1.0f - dip * amount * 0.5f * (1.0f - facing);
    // The average gain over a turn is restored so the knob moves the
    // movement, not the level.
    const float level = louder / (1.0f - dip * amount * 0.5f);
    return level * line.readHermite(source, delay);
  }

  fx::DelayLine hornLine;
  fx::DelayLine drumLine;
  fx::Lfo hornLfo;
  fx::Lfo drumLfo;
  Source sources[kMaxChannels];
  fx::SmoothedParam depth;
  fx::SmoothedParam drive;
  fx::SmoothedParam hornGain;
  fx::SmoothedParam width;
  double hornRate{0.0};
  double drumRate{0.0};
  bool snapRotors{true};
  double sampleRate{48000.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(RotarySpeaker) {
  // A Leslie replaces the sound of the guitar with the sound of the cabinet:
  // crossfade, fully wet. The Doppler delay and the crossover's phase make a
  // dry blend comb-filter.
  addEffectKind<RotaryProcessor>(registry, "Rotary", BlockCategory::Modulation, 1.0f,
                                 BlockMixLaw::Crossfade);
}

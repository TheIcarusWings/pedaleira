// drive.metal: a high gain distortion with two cascaded clipping stages, a
// tight control, a 3 band EQ after the clipping and an optional noise gate.
//
//   in -> gate -> [4x: tight high-pass -> stage 1 (soft clip) -> coupling
//         filters -> stage 2 (hard clip) -> 7 kHz roll-off] -> low shelf ->
//         mid band (sweepable) -> high shelf -> level
//
// Tight is a 12 dB per octave high-pass before the first stage: the higher
// it is, the less low end reaches the clipping, which keeps palm mutes
// and low chords from turning to mud. The first stage is soft and carries the
// Distortion gain; the second is a fixed gain into a hard clip, so even the
// lowest setting is heavily saturated. The EQ comes after the clipping, where
// it shapes the finished sound. The gate is on the input, a threshold with a
// little hysteresis and a smooth gain; at its lowest setting it is off. The
// clipping runs at 4x.
//
// Voicing: Standard is the layout above. Resonant fixes a voice into the signal
// after the user EQ (so Low, Middle, High and Mid Freq still work as
// corrections on top of it): a low-frequency resonance (a narrow peak at about
// 105 Hz on two low shelves, about 5.6 dB below 500 Hz and 2.5 dB below
// 110 Hz), a presence peak at about 5 kHz, and a gentle one-pole top roll-off
// from about 11 kHz in place of the two poles at 7 kHz. The coupling high-pass
// between the two clipping stages drops from 150 Hz to 60 Hz so a low note
// stays a clean square wave into the resonance. With the three EQ knobs flat
// the response is therefore not flat but carries that shape, and the harmonics
// of a mid note are lifted by the presence peak.
#include "effects/drive/DriveCommon.h"

namespace {

using namespace drivefx;

class MetalDistortionProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "drive.metal";

  enum Param {
    kTightHz = 0,
    kDistortionPercent,
    kLowDb,
    kMidDb,
    kMidHz,
    kHighDb,
    kLevelDb,
    kGateDb,
    kVoicing
  };
  enum Voicing { kStandard = 0, kResonant };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::floatParam("tightHz", "Tight", "Hz", 60.0f, 600.0f, 150.0f)
                      .withSkew(0.4f));
      t.push_back(
          BlockParamSpec::floatParam("distortionPercent", "Distortion", "%", 0.0f, 100.0f, 60.0f));
      t.push_back(BlockParamSpec::floatParam("lowDb", "Low", "dB", -12.0f, 12.0f, 0.0f));
      t.push_back(BlockParamSpec::floatParam("midDb", "Middle", "dB", -12.0f, 12.0f, 0.0f));
      t.push_back(BlockParamSpec::floatParam("midHz", "Mid Freq", "Hz", 200.0f, 3000.0f, 700.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("highDb", "High", "dB", -12.0f, 12.0f, 0.0f));
      t.push_back(BlockParamSpec::floatParam("levelDb", "Level", "dB", -48.0f, 18.0f, 0.0f));
      // The lowest value switches the gate off.
      t.push_back(BlockParamSpec::floatParam("gateDb", "Gate", "dB", -90.0f, -30.0f, -90.0f));
      t.push_back(BlockParamSpec::choiceParam("voicing", "Voicing",
                                              juce::StringArray{"Standard", "Resonant"}, kStandard)
                      .notAutomatable());
      return t;
    }();
    return table;
  }

  MetalDistortionProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The half-band filters of the 4x oversampler delay every path by a few
  // samples (drive/DriveCommon.h).
  int latencySamples() const noexcept override { return kLatencySamples; }

  // The gate's release and the low shelf are the longest memory.
  double tailSeconds() const noexcept override { return 0.1; }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    oversampler.prepare(spec.maxBlockSize);
    const double osRate = sampleRate * kOversampleFactor;
    tight.prepare(osRate, 0.03);
    distortion.prepare(osRate, 0.02);
    lowGain.prepare(sampleRate, 0.02);
    midGain.prepare(sampleRate, 0.02);
    midHz.prepare(sampleRate, 0.03);
    highGain.prepare(sampleRate, 0.02);
    level.prepare(sampleRate, 0.02);
    envelopeAttack = coefficientFor(0.0005);
    envelopeRelease = coefficientFor(0.02);
    gateOpen = coefficientFor(0.002);
    gateClose = coefficientFor(0.03);
    for (Lane& lane : lanes) {
      lane.couplingHigh.setCutoff(kCouplingHz, osRate);
      lane.couplingLow.setCutoff(5000.0, osRate);
      lane.rolloffA.setCutoff(kRolloffHz, osRate);
      lane.rolloffB.setCutoff(kRolloffHz, osRate);
      lane.lowShelf.setCorner(100.0, sampleRate);
      lane.highShelf.setCorner(4000.0, sampleRate);
      lane.voiceShelfA.setCorner(kVoiceShelfAHz, sampleRate);
      lane.voiceShelfB.setCorner(kVoiceShelfBHz, sampleRate);
      lane.voicePeakLow.setCoeffs(fx::BiquadCoeffs::peak(kVoicePeakLowHz, kVoicePeakLowQ, kVoicePeakLowDb, sampleRate));
      lane.voicePeakHigh.setCoeffs(fx::BiquadCoeffs::peak(kVoicePeakHighHz, kVoicePeakHighQ, kVoicePeakHighDb, sampleRate));
      lane.voiceSubHighpass.setCutoff(kVoiceSubHz, sampleRate);
    }
    voiceShelfAGain = fx::dbToGain(kVoiceShelfADb);
    voiceShelfBGain = fx::dbToGain(kVoiceShelfBDb);
    appliedVoicing = -1;
    resetEffect();
  }

  void resetEffect() noexcept override {
    oversampler.reset();
    for (Lane& lane : lanes) {
      lane.envelope = 0.0f;
      lane.gateGain = 1.0f;
      lane.open = true;
      lane.tight.reset();
      lane.couplingHigh.reset();
      lane.couplingLow.reset();
      lane.rolloffA.reset();
      lane.rolloffB.reset();
      lane.lowShelf.reset();
      lane.highShelf.reset();
      lane.mid.reset();
      lane.voiceShelfA.reset();
      lane.voiceShelfB.reset();
      lane.voicePeakLow.reset();
      lane.voicePeakHigh.reset();
      lane.voiceSubHighpass.reset();
    }
    osTick.reset();
    tick.reset();
    tight.snapOnNextTarget();
    distortion.snapOnNextTarget();
    lowGain.snapOnNextTarget();
    midGain.snapOnNextTarget();
    midHz.snapOnNextTarget();
    highGain.snapOnNextTarget();
    level.snapOnNextTarget();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext&) override {
    const int numSamples = buffer.getNumSamples();
    const int numLanes = juce::jmin(buffer.getNumChannels(), kMaxLanes);
    tight.setTarget(param(kTightHz));
    distortion.setTarget(kMinGain * std::pow(kMaxGain / kMinGain, param(kDistortionPercent) * 0.01f));
    lowGain.setTarget(fx::dbToGain(param(kLowDb)));
    midGain.setTarget(fx::dbToGain(param(kMidDb)));
    midHz.setTarget(param(kMidHz));
    highGain.setTarget(fx::dbToGain(param(kHighDb)));
    level.setTarget(fx::dbToGain(param(kLevelDb)) * kMakeup);

    const float gateDb = param(kGateDb);
    const bool gateOn = gateDb > -89.5f;
    const float openLevel = fx::dbToGain(gateDb);
    const float closeLevel = 0.6f * openLevel;

    if (gateOn) {
      for (int i = 0; i < numSamples; ++i)
        for (int ch = 0; ch < numLanes; ++ch) {
          Lane& lane = lanes[ch];
          float* data = buffer.getWritePointer(ch);
          const float level0 = std::abs(data[i]);
          const float c = level0 > lane.envelope ? envelopeAttack : envelopeRelease;
          lane.envelope += c * (level0 - lane.envelope);
          if (lane.envelope > openLevel)
            lane.open = true;
          else if (lane.envelope < closeLevel)
            lane.open = false;
          const float target = lane.open ? 1.0f : 0.0f;
          lane.gateGain += (target > lane.gateGain ? gateOpen : gateClose) * (target - lane.gateGain);
          data[i] *= lane.gateGain;
        }
    } else {
      // Off: forget the state, so switching it on starts from an open gate.
      for (Lane& lane : lanes) {
        lane.envelope = 0.0f;
        lane.gateGain = 1.0f;
        lane.open = true;
      }
    }

    const double osRate = sampleRate * kOversampleFactor;
    const bool resonant = param(kVoicing) > 0.5f;
    if (static_cast<int>(resonant) != appliedVoicing) {
      for (Lane& lane : lanes) {
        lane.couplingHigh.setCutoff(resonant ? kResonantCouplingHz : kCouplingHz, osRate);
        lane.rolloffA.setCutoff(resonant ? kResonantRolloffHz : kRolloffHz, osRate);
        lane.voiceShelfA.reset();
        lane.voiceShelfB.reset();
        lane.voicePeakLow.reset();
        lane.voicePeakHigh.reset();
        lane.voiceSubHighpass.reset();
      }
      appliedVoicing = static_cast<int>(resonant);
    }
    oversampler.run(buffer, [&](float* const* io, int frames, int numReal) {
      for (int i = 0; i < frames; ++i) {
        const float cornerHz = tight.next();
        const float firstGain = distortion.next();
        if (osTick.step())
          for (Lane& lane : lanes)
            lane.tight.set(cornerHz, 0.7071, osRate);
        for (int ch = 0; ch < numReal; ++ch) {
          Lane& lane = lanes[ch];
          const float u = lane.tight.highpass(io[ch][i]);
          const float first = kFirstLevel * softSat(firstGain * u * (1.0f / kFirstLevel));
          const float coupled = lane.couplingLow.lowpass(lane.couplingHigh.highpass(first));
          const float second = kSecondLevel * hardSat(kSecondGain * coupled * (1.0f / kSecondLevel));
          io[ch][i] = resonant ? lane.rolloffA.lowpass(second)
                               : lane.rolloffB.lowpass(lane.rolloffA.lowpass(second));
        }
      }
    });

    for (int i = 0; i < numSamples; ++i) {
      const float low = lowGain.next();
      const float mid = midGain.next();
      const float centre = midHz.next();
      const float high = highGain.next();
      const float g = level.next();
      if (tick.step())
        for (Lane& lane : lanes)
          lane.mid.set(centre, kMidQ, sampleRate);
      for (int ch = 0; ch < numLanes; ++ch) {
        Lane& lane = lanes[ch];
        float* data = buffer.getWritePointer(ch);
        float x = lane.lowShelf.low(data[i], low);
        // The band output of the filter peaks at Q; dividing by it makes the
        // added band exactly unity at the centre, so `mid` is the gain there.
        x += (mid - 1.0f) * (1.0f / kMidQ) * lane.mid.bandpass(x);
        x = lane.highShelf.high(x, high);
        if (resonant) {
          x = lane.voiceShelfB.low(lane.voiceShelfA.low(x, voiceShelfAGain), voiceShelfBGain);
          x = lane.voicePeakHigh.process(lane.voicePeakLow.process(x));
          x = lane.voiceSubHighpass.highpass(x) * kResonantTrim;
        }
        data[i] = x * g;
      }
    }
  }

private:
  float coefficientFor(double seconds) const noexcept {
    return static_cast<float>(1.0 - std::exp(-1.0 / (seconds * sampleRate)));
  }

  static constexpr float kMinGain = 3.0f;     // Distortion 0
  static constexpr float kMaxGain = 180.0f;   // Distortion 100
  static constexpr float kFirstLevel = 0.35f;
  static constexpr float kSecondGain = 8.0f;
  static constexpr float kSecondLevel = 0.3f;
  static constexpr float kMidQ = 0.9f;
  static constexpr float kCouplingHz = 150.0f;
  static constexpr float kRolloffHz = 7000.0f;
  // The Resonant voicing: the coupling and roll-off it changes, then the fixed
  // shape (two low shelves, a low peak, a presence peak and a sub-bass
  // high-pass) and the trim that keeps its 1 kHz level where Standard has it.
  static constexpr float kResonantCouplingHz = 60.0f;
  static constexpr float kResonantRolloffHz = 11467.0f;
  static constexpr float kVoiceShelfAHz = 506.6f;
  static constexpr float kVoiceShelfADb = 5.61f;
  static constexpr float kVoiceShelfBHz = 108.5f;
  static constexpr float kVoiceShelfBDb = 2.45f;
  static constexpr float kVoicePeakLowHz = 105.6f;
  static constexpr float kVoicePeakLowQ = 2.53f;
  static constexpr float kVoicePeakLowDb = 5.52f;
  static constexpr float kVoicePeakHighHz = 4935.0f;
  static constexpr float kVoicePeakHighQ = 1.05f;
  static constexpr float kVoicePeakHighDb = 6.65f;
  static constexpr float kVoiceSubHz = 8.4f;
  static constexpr float kResonantTrim = 0.752f;
  // Output scale that brings the default setting to about unity at guitar level.
  static constexpr float kMakeup = 0.36f;

  struct Lane {
    float envelope{0.0f};
    float gateGain{1.0f};
    bool open{true};
    fx::StateVariableFilter tight, mid;
    fx::OnePole couplingHigh, couplingLow, rolloffA, rolloffB;
    OnePoleShelf lowShelf, highShelf;
    OnePoleShelf voiceShelfA, voiceShelfB;
    fx::Biquad voicePeakLow, voicePeakHigh;
    fx::OnePole voiceSubHighpass;
  };

  Lane lanes[kMaxLanes];
  Oversampler4x oversampler;
  ControlTick osTick{kControlGridOs};
  ControlTick tick{kControlGrid};
  fx::SmoothedParam tight, distortion, lowGain, midGain, midHz, highGain, level;
  float envelopeAttack{0.01f}, envelopeRelease{0.001f}, gateOpen{0.01f}, gateClose{0.001f};
  float voiceShelfAGain{1.0f}, voiceShelfBGain{1.0f};
  int appliedVoicing{-1};
  double sampleRate{48000.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(MetalDistortion) {
  addEffectKind<MetalDistortionProcessor>(registry, "Metal Distortion", BlockCategory::Drive);
}

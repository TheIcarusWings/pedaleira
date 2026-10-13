// pitch.subsynth "Sub Synth": a synthesizer voice one or two octaves under the
// note being played, in the spirit of a bass-synth pedal. The note is followed
// by the pitch family's tracker (detail/PitchTracker.h, McLeod's method) and
// an oscillator plays it down: a sine, a square or a saw (band-limited by a
// polynomial correction of the edges), through a resonant low-pass whose
// cutoff and resonance are the controls, with the level following the
// envelope of the guitar, so the synth plays when the guitar plays and dies
// with it. The release time is the one knob on that envelope.
//
// The block's wet signal is the synth alone and the common mix places it beside
// the guitar (DryPreserving, 50 % by default: the guitar at unity with the synth
// under it).
//
// Tracking, honestly. The synth knows the note only after the tracker has heard
// two or more periods of it, so a new note is joined by its sub at the right
// pitch about 15 ms later for high notes and 35 ms later for the low strings
// (plugin tests measure it); until then it plays the previous note (a low E
// before the first) at about a third of its level. While the tracker is not
// sure of a pitch (the gap between two notes, a chord, noise) the synth keeps
// the last note at that reduced level; it is shut only by silence. It is monophonic: play
// single notes. The block has no latency (the guitar is not delayed); the synth
// follows by the tracking time.
//
// Readings: the detected note (a MIDI number with the cents as a fraction), its
// frequency and the tracker's confidence.
#include <algorithm>
#include <cmath>

#include "effects/EffectDsp.h"
#include "detail/PitchTracker.h"
#include "detail/PitchVoice.h"

namespace {

class SubSynthProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "pitch.subsynth";

  enum Param { kOctave = 0, kWave, kCutoffHz, kResonance, kReleaseMs };
  enum Reading { kDetectedNote = 0, kDetectedHz, kConfidence };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::choiceParam("octave", "Octave", {"-1 octave", "-2 octaves"}, 0));
      t.push_back(BlockParamSpec::choiceParam("wave", "Wave", {"Sine", "Square", "Saw"}, 0));
      t.push_back(BlockParamSpec::floatParam("cutoffHz", "Cutoff", "Hz", 40.0f, 4000.0f, 600.0f)
                      .withSkew(0.4f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("resonance", "Resonance", "%", 0.0f, 100.0f, 20.0f)
                      .withAffectsTail());
      // How long the synth rings after the guitar's envelope drops.
      t.push_back(BlockParamSpec::floatParam("releaseMs", "Release", "ms", 20.0f, 1000.0f, 200.0f)
                      .withSkew(0.5f));
      return t;
    }();
    return table;
  }

  static const BlockReadingTable& readingSpecs() {
    static const BlockReadingTable table = {
        {"detectedNote", "Note", "note", 0.0f, 127.0f},
        {"detectedHz", "Frequency", "Hz", 0.0f, 1500.0f},
        {"confidence", "Confidence", "%", 0.0f, 100.0f},
    };
    return table;
  }

  SubSynthProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs(), readingSpecs()) {}

  // The synth stops with the guitar: the tracker hears the silence within a
  // window, the gate closes after its hold and its 8 ms fall (60 dB in 55 ms),
  // and what is left is the filter ringing out, a second-order low-pass whose
  // amplitude decays with a time constant of 2 Q / (2 pi fc).
  double tailSeconds() const noexcept override {
    const double cutoff = std::max(40.0, static_cast<double>(param(kCutoffHz)));
    const double q = 0.6 + 0.01 * static_cast<double>(param(kResonance)) * kMaxResonanceQ;
    return 0.12 + 6.9 * 2.0 * q / (juce::MathConstants<double>::twoPi * cutoff);
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    for (auto& lane : lanes)
      lane.prepare(sampleRate);
    cutoff.prepare(sampleRate, kSmoothSeconds);
    resonance.prepare(sampleRate, kSmoothSeconds);
    resetEffect();
  }

  void resetEffect() noexcept override {
    for (auto& lane : lanes)
      lane.reset();
    cutoff.snapOnNextTarget();
    resonance.snapOnNextTarget();
    grid.reset();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), 2);
    if (numChannels < 1 || numSamples < 1)
      return;
    const bool dual = ctx.dualMono && numChannels == 2;
    const int laneCount = dual ? 2 : 1;

    cutoff.setTarget(param(kCutoffHz));
    resonance.setTarget(param(kResonance));
    const float ratio = param(kOctave) >= 0.5f ? 0.25f : 0.5f;
    const int wave = static_cast<int>(std::lround(param(kWave)));
    for (auto& lane : lanes)
      lane.follower.setTimes(kAttackMs, param(kReleaseMs));

    float* io[2] = {buffer.getWritePointer(0), numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    grid.run(numSamples, [&](int start, int length, bool boundary) {
      if (boundary) {
        const float q = 0.6f + 0.01f * resonance.current() * kMaxResonanceQ;
        for (int l = 0; l < laneCount; ++l)
          lanes[l].control(cutoff.current(), q, ratio, sampleRate);
      }
      for (int i = start; i < start + length; ++i) {
        cutoff.next();
        resonance.next();
        if (dual) {
          const float a = lanes[0].tick(io[0][i], wave, sampleRate);
          const float b = lanes[1].tick(io[1][i], wave, sampleRate);
          io[0][i] = a;
          io[1][i] = b;
        } else {
          const float mono = numChannels > 1 ? 0.5f * (io[0][i] + io[1][i]) : io[0][i];
          const float y = lanes[0].tick(mono, wave, sampleRate);
          for (int ch = 0; ch < numChannels; ++ch)
            io[ch][i] = y;
        }
      }
    });

    const pitchfx::PitchTracker& tracker = lanes[0].tracker;
    setReading(kDetectedNote, tracker.hasNote() ? tracker.midiNote() : 0.0f);
    setReading(kDetectedHz, tracker.hasNote() ? tracker.frequencyHz() : 0.0f);
    setReading(kConfidence, 100.0f * tracker.confidence());
  }

private:
  static constexpr double kSmoothSeconds = 0.02;
  static constexpr float kAttackMs = 2.0f;
  static constexpr float kMaxResonanceQ = 8.0f;

  struct Lane {
    void prepare(double rate) {
      holdSamples = static_cast<int>(std::lround(kHoldSeconds * rate));
      gateUp = static_cast<float>(1.0 - std::exp(-1.0 / (kGateUpSeconds * rate)));
      gateDown = static_cast<float>(1.0 - std::exp(-1.0 / (kGateDownSeconds * rate)));
      slide = static_cast<float>(1.0 - std::exp(-1.0 / (kSlideSeconds * rate)));
      tracker.prepare(rate);
      follower.prepare(rate);
      follower.setMode(fx::EnvelopeFollower::Mode::Peak);
      reset();
    }

    void reset() noexcept {
      tracker.reset();
      follower.reset();
      filter.reset();
      phase = 0.0;
      frequency = 0.0f;
      targetFrequency = 0.0f;
      gate = 0.0f;
      gateTarget = 0.0f;
      samplesSinceVoiced = holdSamples + 1;
    }

    // Every control step.
    void control(float cutoffHz, float q, float ratio, double rate) noexcept {
      tracker.step();
      // The note held is the last confident one (a default low E before the
      // first). The synth plays at full level while the tracker is sure, at
      // kUnsureLevel while it is not (the gap between two notes, a chord, a
      // pick scrape: the bass does not drop out, and does not follow noise
      // either), and not at all when the guitar is silent.
      targetFrequency = (tracker.hasNote() ? tracker.frequencyHz() : kDefaultHz) * ratio;
      const bool silent = tracker.rms() < kSilenceRms;
      gateTarget = silent ? 0.0f : (samplesSinceVoiced <= holdSamples ? 1.0f : kUnsureLevel);
      filter.set(cutoffHz, q, rate);
    }

    float tick(float input, int wave, double rate) noexcept {
      tracker.push(input);
      if (tracker.isVoiced())
        samplesSinceVoiced = 0;
      else if (samplesSinceVoiced <= holdSamples)
        ++samplesSinceVoiced;

      const float envelope = follower.process(input);
      gate += (gateTarget > gate ? gateUp : gateDown) * (gateTarget - gate);
      // A new note is a jump of frequency; the gate is shut across it (the
      // tracker is not sure while two notes are in its window), and the
      // frequency slides the rest of the way.
      frequency += slide * (targetFrequency - frequency);
      if (frequency < 1.0f)
        frequency = targetFrequency;

      const double increment = static_cast<double>(frequency) / rate;
      float osc = 0.0f;
      if (frequency > 1.0f) {
        phase += increment;
        phase -= std::floor(phase);
        osc = oscillator(wave, phase, increment);
      }
      return filter.lowpass(osc) * envelope * gate;
    }

    static float polyBlep(double t, double dt) noexcept {
      if (t < dt) {
        const double x = t / dt;
        return static_cast<float>(x + x - x * x - 1.0);
      }
      if (t > 1.0 - dt) {
        const double x = (t - 1.0) / dt;
        return static_cast<float>(x * x + x + x + 1.0);
      }
      return 0.0f;
    }

    static float oscillator(int wave, double t, double dt) noexcept {
      switch (wave) {
        case 1: {  // square
          float v = t < 0.5 ? 1.0f : -1.0f;
          v += polyBlep(t, dt);
          double shifted = t + 0.5;
          shifted -= std::floor(shifted);
          v -= polyBlep(shifted, dt);
          return v * kSquareTrim;
        }
        case 2:  // saw
          return (static_cast<float>(2.0 * t - 1.0) - polyBlep(t, dt)) * kSawTrim;
        default:
          return static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * t));
      }
    }

    // The gate closes this long after the last confident frame (the tracker
    // answers every 5 ms, so this is a few frames), and opens and closes with
    // one-pole smoothing.
    static constexpr double kHoldSeconds = 0.015;
    static constexpr float kUnsureLevel = 0.35f;
    static constexpr float kDefaultHz = 82.407f;
    static constexpr float kSilenceRms = 0.0015f;
    static constexpr double kGateUpSeconds = 0.002;
    static constexpr double kGateDownSeconds = 0.008;
    static constexpr double kSlideSeconds = 0.005;
    static constexpr float kSquareTrim = 0.7f;
    static constexpr float kSawTrim = 0.8f;

    pitchfx::PitchTracker tracker;
    fx::EnvelopeFollower follower;
    fx::StateVariableFilter filter;
    double phase{0.0};
    float frequency{0.0f}, targetFrequency{0.0f}, gate{0.0f};
    float gateTarget{0.0f};
    int holdSamples{720};
    float gateUp{0.01f}, gateDown{0.0026f}, slide{0.004f};
    int samplesSinceVoiced{721};
  };

  double sampleRate{48000.0};
  Lane lanes[2];
  fx::SmoothedParam cutoff, resonance;
  pitchfx::ControlGrid grid;
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(SubSynth) {
  // The synth is heard beside the guitar: dry at unity up to 50 %.
  addEffectKind<SubSynthProcessor>(registry, "Sub Synth", BlockCategory::Pitch, 0.5f,
                                   BlockMixLaw::DryPreserving);
}

// pitch.octavedown "Analog Octave": the classic analog octave-down sound by
// frequency division, as an octave pedal of the 1980s did it, built here from
// the textbook circuit rather than the pitch-shifting engine of the other kinds.
//
//   input -> low-pass (tracks the note) -> Schmitt trigger -> flip-flop /2
//                                                          -> flip-flop /4
//
// The input is filtered so that only the fundamental is left to cross zero
// (a guitar note has stronger harmonics than most notes have a fundamental, and
// each extra zero crossing would divide at the wrong rate), squared by a
// comparator with hysteresis (a Schmitt trigger, whose thresholds follow the
// level of the note so a soft note and a hard one switch alike, and which
// ignores noise under about -68 dBFS), and the rising edges of that square wave
// toggle a flip-flop: its output is a square wave at half the frequency, one
// octave down. A second flip-flop fed by the first halves it again, two octaves
// down. The squares are scaled by the level of the playing (the envelope of the
// filtered input, so the octave swells and dies with the note, as the real
// circuit's output does), run through a low-pass to take the edge off (the tone
// control), and mixed with the dry signal.
//
// The low-pass in front is an adaptive one: its cutoff follows 1.6 times the
// frequency measured from the comparator's edges, four poles. That is the
// "tracked, filtered and squared" of the brief: with it a low E (82 Hz) and a
// high B string (247 Hz) both divide cleanly.
//
// MONOPHONIC BY NATURE. A flip-flop divides one zero-crossing train, so it
// follows one note at a time: play single notes, with the neck pickup and the
// tone rolled off if you like. A chord or two strings ringing together give
// the comparator crossings of several periods and the octave warbles, which is
// the sound of the original and not a bug. (pitch.octaver is the polyphonic
// octave.) Notes below about 60 Hz and above about 1.5 kHz are not followed.
//
// The block is a blend (like pitch.octaver): the dry signal is one of the three
// level knobs and the common mix, which crossfades this blend against the
// chain's own dry, is left at 100 %. There is no latency: the block does not
// delay the dry signal and the octaves follow the input by a half cycle of the
// note, which is what a divider does. The octave starts one cycle after the
// note does, and the first cycle is divided from the second onward.
#include <algorithm>
#include <cmath>

#include "effects/EffectDsp.h"

namespace {

class AnalogOctaveProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "pitch.octavedown";

  enum Param { kDryDb = 0, kOctaveDb, kSubDb, kToneHz, kTracking };
  enum Reading { kTrackedHz = 0 };

  // The level knobs run to this floor, which means "off".
  static constexpr float kOffDb = -60.0f;

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::floatParam("dryDb", "Dry", "dB", kOffDb, 6.0f, 0.0f).withStep(0.5f));
      // One octave down: the /2 flip-flop.
      t.push_back(BlockParamSpec::floatParam("octaveDb", "Octave", "dB", kOffDb, 6.0f, -3.0f).withStep(0.5f));
      // Two octaves down: the /4 flip-flop.
      t.push_back(BlockParamSpec::floatParam("subDb", "Sub", "dB", kOffDb, 6.0f, -9.0f).withStep(0.5f));
      // The low-pass on the divided tone.
      t.push_back(BlockParamSpec::floatParam("toneHz", "Tone", "Hz", 100.0f, 5000.0f, 1000.0f).withSkew(0.4f));
      // How eagerly the comparator switches: its hysteresis as a share of the
      // note's level. High is touchy (quiet notes, more false triggers), low
      // is steady.
      t.push_back(BlockParamSpec::floatParam("tracking", "Tracking", "%", 0.0f, 100.0f, 50.0f));
      return t;
    }();
    return table;
  }

  static const BlockReadingTable& readingSpecs() {
    static const BlockReadingTable table = {
        {"trackedHz", "Tracked", "Hz", 0.0f, 1500.0f},
    };
    return table;
  }

  AnalogOctaveProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs(), readingSpecs()) {}

  // The octaves ring out with the level of the playing: its release (40 ms,
  // 63 %) to 60 dB down is 6.9 times that, and the tone filter's own ring is
  // far shorter.
  double tailSeconds() const noexcept override { return 0.35; }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    for (auto& smoother : levels)
      smoother.prepare(sampleRate, kLevelRampSeconds);
    tone.prepare(sampleRate, kLevelRampSeconds);
    for (auto& channel : channels)
      channel.prepare(sampleRate);
    resetEffect();
  }

  void resetEffect() noexcept override {
    for (auto& channel : channels)
      channel.reset();
    for (auto& smoother : levels)
      smoother.snapOnNextTarget();
    tone.snapOnNextTarget();
    sinceControl = 0;
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext&) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), 2);
    if (numChannels < 1 || numSamples < 1)
      return;
    // Each channel is its own instrument: a divider follows one zero-crossing
    // train, so nothing is shared (and in dual mono nothing may be).
    const float dbs[3] = {param(kDryDb), param(kOctaveDb), param(kSubDb)};
    for (int i = 0; i < 3; ++i)
      levels[i].setTarget(dbs[i] <= kOffDb + 0.01f ? 0.0f : fx::dbToGain(dbs[i]));
    tone.setTarget(param(kToneHz));
    const float tracking = juce::jlimit(0.0f, 100.0f, param(kTracking)) * 0.01f;
    // Hysteresis as a share of the note's level: 50 % of the setting's range
    // is 0.30, the knob moves it between 0.50 and 0.10.
    const float hysteresis = 0.5f - 0.4f * tracking;

    float* io[2] = {buffer.getWritePointer(0), numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    for (int i = 0; i < numSamples; ++i) {
      if (sinceControl == 0)
        for (int ch = 0; ch < numChannels; ++ch)
          channels[ch].updateControls(tone.current(), sampleRate);
      sinceControl = (sinceControl + 1) % kControlGrid;
      const float dry = levels[0].next();
      const float oct = levels[1].next();
      const float sub = levels[2].next();
      tone.next();
      for (int ch = 0; ch < numChannels; ++ch) {
        const float in = io[ch][i];
        float divided1 = 0.0f, divided2 = 0.0f;
        channels[ch].process(in, hysteresis, divided1, divided2);
        io[ch][i] = dry * in + oct * divided1 + sub * divided2;
      }
    }
    setReading(kTrackedHz, channels[0].locked() ? channels[0].trackedHz() : 0.0f);
  }

private:
  static constexpr int kControlGrid = 16;
  static constexpr float kOutputReleaseMs = 40.0f;
  static constexpr double kLevelRampSeconds = 0.02;

  // One signal's divider. All the state of one instrument; plain floats.
  struct Channel {
    void prepare(double rate) noexcept {
      sampleRate = rate;
      // The level of the playing, for the output: quick up, a decay that rings
      // a note out; and a slower one for the comparator thresholds, which must
      // not collapse in the middle of a cycle.
      outLevel.prepare(rate);
      outLevel.setTimes(1.0f, kOutputReleaseMs);
      thresholdLevel.prepare(rate);
      thresholdLevel.setTimes(2.0f, 150.0f);
      dc.setCutoff(kDcHz, rate);
      reset();
    }

    void reset() noexcept {
      dc.reset();
      for (auto& filter : prefilter)
        filter.reset();
      for (auto& filter : toneFilter)
        filter.reset();
      outLevel.reset();
      thresholdLevel.reset();
      high = false;
      flip1 = flip2 = false;
      sinceEdge = 0;
      estimatedHz = 0.0f;
      cutoffHz = kStartCutoffHz;
      outliers = 0;
      edges = 0;
      setPrefilter(kStartCutoffHz);
    }

    /** Every control step: the pre-filter follows the note, the tone filter the
        knob. */
    void updateControls(float toneHz, double rate) noexcept {
      if (estimatedHz > 0.0f) {
        const float target = juce::jlimit(kMinCutoffHz, kMaxCutoffHz, kFollowRatio * estimatedHz);
        cutoffHz += 0.15f * (target - cutoffHz);
      }
      setPrefilter(cutoffHz);
      for (auto& filter : toneFilter)
        filter.set(toneHz, kButterworthQ, rate);
    }

    void process(float input, float hysteresis, float& octave, float& sub) noexcept {
      const float x = dc.highpass(input);
      float y = x;
      for (auto& filter : prefilter)
        y = filter.lowpass(y);

      const float follow = outLevel.process(y);
      const float reference = thresholdLevel.process(y);
      const float threshold = std::max(hysteresis * reference, kNoiseFloor);

      // No edge for a quarter of a second: the note is over, the estimate with it.
      if (++sinceEdge > static_cast<int>(0.25 * sampleRate) && estimatedHz > 0.0f) {
        estimatedHz = 0.0f;
        edges = 0;
        outliers = 0;
      }
      if (!high && y > threshold) {
        high = true;
        risingEdge();
      } else if (high && y < -threshold) {
        high = false;
      }

      // Squares at unity peak scaled by the level of the playing, then the
      // tone filter. The same trim for both so the levels of the two knobs
      // compare.
      const float shape = follow * kSquareTrim;
      float a = (flip1 ? shape : -shape);
      float b = (flip2 ? shape : -shape);
      a = toneFilter[1].lowpass(toneFilter[0].lowpass(a));
      b = toneFilter[3].lowpass(toneFilter[2].lowpass(b));
      octave = a;
      sub = b;
    }

    bool locked() const noexcept { return estimatedHz > 0.0f && edges >= 3; }
    float trackedHz() const noexcept { return estimatedHz; }

  private:
    void risingEdge() noexcept {
      // The frequency from the time since the last rising edge, with a guard
      // against a stray crossing: a period within a factor of 1.5 of the
      // current estimate is taken in; a different one has to repeat before it
      // replaces it.
      const float measured = static_cast<float>(sampleRate) / static_cast<float>(std::max(1, sinceEdge));
      sinceEdge = 0;
      ++edges;
      if (measured >= kMinTrackHz && measured <= kMaxTrackHz) {
        if (estimatedHz <= 0.0f || (measured < 1.5f * estimatedHz && measured > estimatedHz / 1.5f)) {
          estimatedHz = estimatedHz <= 0.0f ? measured : estimatedHz + 0.35f * (measured - estimatedHz);
          outliers = 0;
        } else if (++outliers >= 3) {
          estimatedHz = measured;
          outliers = 0;
        }
      }
      // Rising edge of the comparator: the first flip-flop toggles; its own
      // rising edge toggles the second.
      flip1 = !flip1;
      if (flip1)
        flip2 = !flip2;
    }

    void setPrefilter(float hz) noexcept {
      for (auto& filter : prefilter)
        filter.set(hz, kButterworthQ, sampleRate);
    }

    static constexpr double kDcHz = 20.0;
    static constexpr float kStartCutoffHz = 500.0f;
    static constexpr float kMinCutoffHz = 120.0f;
    static constexpr float kMaxCutoffHz = 2400.0f;
    static constexpr float kFollowRatio = 1.6f;
    static constexpr float kMinTrackHz = 55.0f;
    static constexpr float kMaxTrackHz = 1500.0f;
    static constexpr float kNoiseFloor = 4.0e-4f;  // -68 dBFS
    static constexpr float kSquareTrim = 0.7f;
    static constexpr double kButterworthQ = 0.7071;

    double sampleRate{48000.0};
    fx::OnePole dc;
    fx::StateVariableFilter prefilter[2];
    fx::StateVariableFilter toneFilter[4];
    fx::EnvelopeFollower outLevel, thresholdLevel;
    bool high{false}, flip1{false}, flip2{false};
    int sinceEdge{0};
    int outliers{0};
    int edges{0};
    float estimatedHz{0.0f};
    float cutoffHz{kStartCutoffHz};
  };

  double sampleRate{48000.0};
  fx::SmoothedParam levels[3];
  fx::SmoothedParam tone;
  Channel channels[2];
  int sinceControl{0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(AnalogOctave) {
  // A blend with its own dry level (see the top of the file): the common mix
  // stays at 100 % and replaces the dry signal with this blend.
  addEffectKind<AnalogOctaveProcessor>(registry, "Analog Octave", BlockCategory::Pitch, 1.0f,
                                       BlockMixLaw::Crossfade);
}

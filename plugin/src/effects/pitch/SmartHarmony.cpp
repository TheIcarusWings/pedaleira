// pitch.harmony "Smart Harmony": a key- and scale-aware harmonizer. Two voices
// (four with the `voices` switch) sing a chosen number of SCALE STEPS above or
// below the note being played, in a chosen key and scale: a third up is a major
// third on C in C major and a minor third on D, which is what makes it
// "smart". The voices are the pitch family's shifting engine (detail/PitchVoice.h,
// one engine per voice and signal); the note is found by the family's tracker
// (detail/PitchTracker.h, McLeod's method), which also gives the confidence.
//
//  - Each voice has an interval in scale steps (+2: the note two steps up the
//    scale, a third in a seven-note scale; -7: an octave down in one; the
//    steps are of the chosen scale, see detail/ScaleMap.h), a level and a pan.
//    The ten scales are major, natural minor, harmonic minor, melodic minor,
//    dorian, mixolydian, pentatonic major, pentatonic minor, blues and
//    chromatic. A note that is not in the scale is treated as the scale note
//    just below it and keeps its distance from it.
//  - The interval is applied as a ratio for the detected note, with a 15 ms
//    glide so a note change bends the voice rather than clicking it. The
//    tracker holds the last good note when its confidence drops (a chord, the
//    gap between two notes), so the voices keep their interval then; before
//    the first note they are silent.
//  - The dry signal has its own level and is delayed by the block's latency so
//    it lines up with the voices. The block is a blend, like pitch.octaver:
//    the common mix is left at 100 %.
//  - Pans work where there is a stereo image to place voices in (true stereo
//    with no amp model later in the lane); elsewhere every voice is centred. In
//    dual mono every voice follows its own channel with its own tracker.
//
// Latency is the engine's constant attack latency (PitchShift.h): an attack or
// an impulse arrives, dry and voices alike, exactly `latencySamples()` after it
// went in. The voices find their note about 15 to 30 ms after an attack (the
// tracker's latency, see its header), so a new note's first moments are sung at
// the interval of the note before it, bending to the new one.
//
// Cost: each voice is an engine of the family (about 0.2 to 0.3 % of a core at
// 48 kHz, see the tests' printout), so two voices plus the tracker cost about
// three engines and four voices about five; the tracker is cheap (it runs
// only while a voice does).
#include <algorithm>
#include <cmath>

#include "effects/EffectDsp.h"
#include "detail/PitchTracker.h"
#include "detail/PitchVoice.h"
#include "detail/ScaleMap.h"

namespace {

class SmartHarmonyProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "pitch.harmony";

  static constexpr int kMaxVoices = 4;
  static constexpr int kMaxSteps = 14;
  static constexpr int kMaxSemitones = 24;
  static constexpr int kParamsPerVoice = 3;  // degree, level, pan

  enum Param {
    kKey = 0,
    kScale,
    kVoices,
    kVoice1Steps,
    kVoice1Db,
    kVoice1Pan,
    kVoice2Steps,
    kVoice2Db,
    kVoice2Pan,
    kVoice3Steps,
    kVoice3Db,
    kVoice3Pan,
    kVoice4Steps,
    kVoice4Db,
    kVoice4Pan,
    kDryDb,
    kWindow
  };
  enum Reading { kDetectedNote = 0, kDetectedHz, kConfidence };

  // A level knob at its floor means "off" (no engine runs for it).
  static constexpr float kOffDb = -60.0f;

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::choiceParam("key", "Key", pitchfx::keyNames(), 0));
      t.push_back(BlockParamSpec::choiceParam("scale", "Scale", pitchfx::scaleNames(), 0));
      t.push_back(BlockParamSpec::choiceParam("voices", "Voices", {"2 voices", "4 voices"}, 0));
      // Voices 1 and 2 are a third and a fifth up (a triad over the note), 3
      // and 4 a third below and an octave up.
      addVoice(t, 1, 2, -3.0f, -50.0f);
      addVoice(t, 2, 4, -6.0f, 50.0f);
      addVoice(t, 3, -2, -6.0f, -25.0f);
      addVoice(t, 4, 7, -9.0f, 25.0f);
      t.push_back(BlockParamSpec::floatParam("dryDb", "Dry", "dB", kOffDb, 6.0f, 0.0f).withStep(0.5f));
      t.push_back(BlockParamSpec::choiceParam("window", "Window", pitchfx::windowLabels(),
                                              static_cast<int>(PitchShift::kDefaultWindow))
                      .withAffectsLatency()
                      .withAffectsTail()
                      .notAutomatable());
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

  SmartHarmonyProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs(), readingSpecs()) {}

  int latencySamples() const noexcept override { return latch.latencySamples(); }
  bool latencyChangePending() const noexcept override { return latch.pending(param(kWindow)); }
  void commitLatency() override { latch.commit(param(kWindow)); }
  double tailSeconds() const noexcept override { return latch.tailSeconds(); }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    latch.prepare(sampleRate, param(kWindow), kMaxSemitones);
    for (auto& voice : voices)
      voice.prepare(sampleRate, spec.maxBlockSize, latch.current(), latch.latencySamples());
    for (auto& copy : scratch)
      copy.setSize(2, juce::jmax(1, spec.maxBlockSize), false, true, false);
    for (auto& tracker : trackers)
      tracker.prepare(sampleRate);
    for (auto& smoother : gains)
      smoother.prepare(sampleRate, kLevelRampSeconds);
    dryGain.prepare(sampleRate, kLevelRampSeconds);
    for (auto& perVoice : glides)
      for (auto& glide : perVoice)
        glide.prepare(sampleRate, kGlideSeconds);
    dryLine.prepare(2, PitchShift::windowSamples(PitchShift::Window::ms60, sampleRate) + 16);
    armCoeff = static_cast<float>(1.0 - std::exp(-1.0 / (kLevelRampSeconds * sampleRate)));
    clearState();
  }

  void resetEffect() noexcept override {
    for (auto& voice : voices)
      voice.markCold();
    clearState();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), 2);
    if (numChannels < 1 || numSamples < 1)
      return;
    const bool dual = ctx.dualMono && numChannels == 2;
    const bool image = blockHasStereoImage(numChannels, ctx);
    const int lanes = dual ? 2 : 1;
    const int voiceCount = param(kVoices) >= 0.5f ? kMaxVoices : 2;

    // Gains: each side of a voice is level times its pan. (A voice also waits
    // for its lane's first note, it has no interval before that: armLevel,
    // moved on the control grid so that it does not depend on the block size.)
    bool running[kMaxVoices];
    for (int v = 0; v < kMaxVoices; ++v) {
      const int base = kVoice1Steps + v * kParamsPerVoice;
      const float db = param(base + 1);
      const float level = (v < voiceCount && db > kOffDb + 0.01f) ? fx::dbToGain(db) : 0.0f;
      float side[2] = {level, level};
      if (image) {
        const float angle = (juce::jlimit(-100.0f, 100.0f, param(base + 2)) * 0.01f + 1.0f) *
                            juce::MathConstants<float>::pi * 0.25f;
        side[0] = level * juce::MathConstants<float>::sqrt2 * std::cos(angle);
        side[1] = level * juce::MathConstants<float>::sqrt2 * std::sin(angle);
      }
      bool smoothing = false;
      for (int ch = 0; ch < 2; ++ch) {
        gains[v * 2 + ch].setTarget(side[ch]);
        smoothing = smoothing || gains[v * 2 + ch].isSmoothing();
      }
      // A voice with no level, once its fade is over, is not run, and starts
      // from rest when it comes back. One that is only waiting for a note is.
      running[v] = level > 0.0f || smoothing;
      if (running[v])
        copyInput(buffer, scratch[v], numChannels, numSamples);
      else
        voices[v].markCold();
    }
    dryGain.setTarget(param(kDryDb) <= kOffDb + 0.01f ? 0.0f : fx::dbToGain(param(kDryDb)));
    // The tracker runs when a voice does.
    bool tracking = false;
    for (int v = 0; v < kMaxVoices; ++v)
      tracking = tracking || (running[v] && v < voiceCount);
    if (!tracking && !trackersClean) {
      for (auto& tracker : trackers)
        tracker.reset();
      trackersClean = true;
    }
    if (tracking)
      trackersClean = false;

    const PitchShift::Window window = latch.current();
    const int delay = latch.latencySamples();
    float* io[2] = {buffer.getWritePointer(0), numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    const int key = static_cast<int>(std::lround(param(kKey)));
    const int scaleIndex = juce::jlimit(0, pitchfx::kNumScales - 1, static_cast<int>(std::lround(param(kScale))));

    grid.run(numSamples, [&](int start, int length, bool boundary) {
      if (boundary) {
        for (int lane = 0; lane < lanes; ++lane) {
          if (tracking)
            trackers[lane].step();
          const int note = trackers[lane].note();
          armTarget[lane] = note >= 0 ? 1.0f : 0.0f;
          for (int v = 0; v < kMaxVoices; ++v) {
            if (note >= 0) {
              const int steps = juce::jlimit(-kMaxSteps, kMaxSteps,
                                             static_cast<int>(std::lround(param(kVoice1Steps + v * kParamsPerVoice))));
              const int semitones = juce::jlimit(-kMaxSemitones, kMaxSemitones,
                                                 pitchfx::harmonyInterval(note, key, scaleIndex, steps));
              glides[v][lane].setTarget(static_cast<float>(semitones));
            }
            laneSemitones[v][lane] = glides[v][lane].tick();
          }
        }
      }
      for (int v = 0; v < kMaxVoices; ++v) {
        if (!running[v])
          continue;
        float* pointers[2] = {scratch[v].getWritePointer(0),
                              numChannels > 1 ? scratch[v].getWritePointer(1) : nullptr};
        voices[v].process(pointers, numChannels, dual, start, length, laneSemitones[v], window, delay);
      }
      mixSegment(io, numChannels, start, length, tracking, dual, delay, running);
    });

    const pitchfx::PitchTracker& tracker = trackers[0];
    setReading(kDetectedNote, tracker.hasNote() ? tracker.midiNote() : 0.0f);
    setReading(kDetectedHz, tracker.hasNote() ? tracker.frequencyHz() : 0.0f);
    setReading(kConfidence, 100.0f * tracker.confidence());
  }

private:
  void clearState() noexcept {
    for (auto& tracker : trackers)
      tracker.reset();
    trackersClean = true;
    for (auto& smoother : gains)
      smoother.snapOnNextTarget();
    dryGain.snapOnNextTarget();
    // Back to unison, and the first note takes the interval at once: a glide
    // keeps the value it had until it is given a target.
    for (auto& perVoice : glides)
      for (auto& glide : perVoice) {
        glide.snap();
        glide.setTarget(0.0f);
        glide.snap();
      }
    for (auto& perVoice : laneSemitones)
      for (auto& value : perVoice)
        value = 0.0f;
    armTarget[0] = armTarget[1] = armLevel[0] = armLevel[1] = 0.0f;
    dryLine.clear();
    grid.reset();
  }

  static constexpr double kLevelRampSeconds = 0.02;
  static constexpr double kGlideSeconds = 0.015;

  static void addVoice(BlockParamTable& t, int number, int defaultSteps, float db, float pan) {
    const juce::String n(number);
    // Scale steps: 1 is the next note of the scale, 7 an octave in a seven-note
    // scale. Two octaves either way in one.
    t.push_back(BlockParamSpec::floatParam("voice" + n + "Steps", "Voice " + n + " Interval", "steps",
                                           -static_cast<float>(kMaxSteps), static_cast<float>(kMaxSteps),
                                           static_cast<float>(defaultSteps))
                    .withStep(1.0f));
    t.push_back(BlockParamSpec::floatParam("voice" + n + "Db", "Voice " + n + " Level", "dB", kOffDb, 6.0f, db)
                    .withStep(0.5f));
    t.push_back(BlockParamSpec::floatParam("voice" + n + "Pan", "Voice " + n + " Pan", "%", -100.0f, 100.0f, pan)
                    .withStep(1.0f));
  }

  static void copyInput(const juce::AudioBuffer<float>& from, juce::AudioBuffer<float>& to,
                        int numChannels, int numSamples) noexcept {
    for (int ch = 0; ch < numChannels; ++ch)
      std::copy_n(from.getReadPointer(ch), numSamples, to.getWritePointer(ch));
  }

  void mixSegment(float* const* io, int numChannels, int start, int length, bool tracking, bool dual,
                  int delay, const bool (&running)[kMaxVoices]) noexcept {
    for (int i = start; i < start + length; ++i) {
      float gain[kMaxVoices][2];
      for (int v = 0; v < kMaxVoices; ++v) {
        gain[v][0] = gains[v * 2].next();
        gain[v][1] = gains[v * 2 + 1].next();
      }
      const float dry = dryGain.next();
      for (int l = 0; l < 2; ++l)
        armLevel[l] += armCoeff * (armTarget[l] - armLevel[l]);
      if (tracking) {
        if (dual) {
          trackers[0].push(io[0][i]);
          trackers[1].push(io[1][i]);
        } else {
          trackers[0].push(numChannels > 1 ? 0.5f * (io[0][i] + io[1][i]) : io[0][i]);
        }
      }
      for (int ch = 0; ch < numChannels; ++ch) {
        const float late = dryLine.read(ch, delay);
        dryLine.write(ch, io[ch][i]);
        float out = dry * late;
        for (int v = 0; v < kMaxVoices; ++v)
          if (running[v])
            out += gain[v][ch] * armLevel[dual ? ch : 0] * scratch[v].getReadPointer(ch)[i];
        io[ch][i] = out;
      }
    }
  }

  pitchfx::PitchVoice voices[kMaxVoices];
  juce::AudioBuffer<float> scratch[kMaxVoices];
  pitchfx::PitchTracker trackers[2];
  bool trackersClean{true};
  fx::SmoothedParam gains[kMaxVoices * 2];
  fx::SmoothedParam dryGain;
  pitchfx::Glide glides[kMaxVoices][2];
  float laneSemitones[kMaxVoices][pitchfx::kMaxLanes] = {};
  fx::DelayLine dryLine;
  float armTarget[2] = {0.0f, 0.0f}, armLevel[2] = {0.0f, 0.0f}, armCoeff{0.001f};
  pitchfx::ControlGrid grid;
  pitchfx::WindowLatch latch;
  double sampleRate{48000.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(SmartHarmony) {
  // A blend with its own dry level, like pitch.octaver: the common mix stays at
  // 100 % and replaces the dry signal with this blend.
  addEffectKind<SmartHarmonyProcessor>(registry, "Smart Harmony", BlockCategory::Pitch, 1.0f,
                                       BlockMixLaw::Crossfade);
}

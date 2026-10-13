// pitch.harmonizer: two harmony voices over the played note, built on the
// polyphonic engine in PitchShift.h (one engine per voice).
//
//  - Chromatic mode: each voice is a fixed number of semitones.
//  - Scale mode: each voice is a scale degree above or below the played note
//    in a chosen key (a third up is a major third on C in C major and a minor
//    third on D). That needs the note, so a small monophonic pitch detector
//    (YIN on a decimated copy of the input) runs beside the engines and maps
//    the stable note to the diatonic interval. While no stable pitch is heard
//    (noise, silence, a chord that confuses it) the voices fall back to their
//    chromatic interval, so the block never goes quiet or wild.
//
// The wet signal is the voices alone and the common mix places them beside the
// dry signal (DryPreserving, default 50 %): the guitar stays at unity while the
// harmonies come in, which is how a harmonizer pedal's blend knob behaves. Each
// voice has its own level, and on a true stereo image its own pan (a balance
// of the source's two sides, centred at unity), so the harmonies can sit left
// and right of the guitar. Dual mono has no image to place voices in: every
// voice then follows its own channel, with its own note tracking.
//
// The detector is not TunerDetector: that one is polled from the message
// thread and returns a juce::var, neither of which an audio-thread effect may
// do.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "effects/EffectDsp.h"
#include "detail/PitchVoice.h"

namespace {

// ---------------------------------------------------------------------------
// Scale arithmetic
// ---------------------------------------------------------------------------

constexpr int kMajorScale[7] = {0, 2, 4, 5, 7, 9, 11};
constexpr int kMinorScale[7] = {0, 2, 3, 5, 7, 8, 10};  // natural minor

constexpr int floorDiv(int a, int b) noexcept {
  const int q = a / b;
  return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

// Semitones above the tonic of the scale note `index` steps from it (any
// integer: 7 is the tonic an octave up, -1 the 7th below it).
constexpr int scaleOffset(const int (&scale)[7], int index) noexcept {
  const int octave = floorDiv(index, 7);
  return octave * 12 + scale[index - octave * 7];
}

/** The interval in semitones from the played MIDI note to the scale note
    `steps` degrees away from it. A note outside the scale is treated as the
    scale degree just below it, so the voice keeps its distance from the note
    that was played. */
constexpr int diatonicInterval(int note, int tonic, bool minor, int steps) noexcept {
  const int(&scale)[7] = minor ? kMinorScale : kMajorScale;
  const int relative = note - tonic;
  const int octave = floorDiv(relative, 12);
  const int pitchClass = relative - octave * 12;
  int degree = 0;
  for (int d = 6; d > 0; --d)
    if (scale[d] <= pitchClass) {
      degree = d;
      break;
    }
  const int index = octave * 7 + degree;
  return scaleOffset(scale, index + steps) - scaleOffset(scale, index);
}

// Self-checks of the table at compile time: C major, a third up from C is E
// (+4), from D is F (+3); the same from A in A minor is C (+3).
static_assert(diatonicInterval(60, 0, false, 2) == 4, "major third above C in C major");
static_assert(diatonicInterval(62, 0, false, 2) == 3, "minor third above D in C major");
static_assert(diatonicInterval(67, 0, false, 4) == 7, "fifth above G in C major");
static_assert(diatonicInterval(71, 0, false, 4) == 6, "diminished fifth above B in C major");
static_assert(diatonicInterval(60, 0, false, -2) == -3, "third below C is A");
static_assert(diatonicInterval(69, 9, true, 2) == 3, "minor third above A in A minor");
static_assert(diatonicInterval(48, 0, false, 7) == 12, "octave");

// ---------------------------------------------------------------------------
// Note tracking
// ---------------------------------------------------------------------------

/** A lightweight monophonic note tracker for the audio thread.
    The input is low-passed and decimated to about 12 kHz; every ~10 ms a
    frame is analysed with YIN (de Cheveigne and Kawahara): the difference
    function, its cumulative-mean normalisation, the first dip under a
    threshold. The difference function is the cost (window x lags
    multiply-adds), so it is computed a few lags at a time on each control
    step instead of in one burst. The note a block acts on is the stable one:
    a new note has to be heard twice in a row to take over and a bend within
    65 cents of the held note does not change it; a lost pitch is held for
    kHoldFrames analyses and then dropped, which is the cue to fall back. */
class NoteTracker {
public:
  void prepare(double sampleRate) {
    decimation = juce::jmax(1, static_cast<int>(std::lround(sampleRate / kAnalysisRate)));
    analysisRate = sampleRate / decimation;
    window = static_cast<int>(std::lround(kWindowSeconds * analysisRate));
    lagMin = juce::jmax(2, static_cast<int>(std::lround(analysisRate / kMaxHz)));
    lagMax = static_cast<int>(std::lround(analysisRate / kMinHz));
    frameLength = window + lagMax + 2;
    hop = juce::jmax(1, static_cast<int>(std::lround(kHopSeconds * analysisRate)));
    ringSize = juce::nextPowerOfTwo(frameLength + hop + 8);
    ring.assign(static_cast<size_t>(ringSize), 0.0f);
    frame.assign(static_cast<size_t>(frameLength), 0.0f);
    normalised.assign(static_cast<size_t>(lagMax + 2), 1.0f);
    // Above ~1.8 kHz nothing is a fundamental we track, and the decimation
    // must not fold it back in.
    lowpass.setCoeffs(fx::BiquadCoeffs::lowpass(kLowpassHz, 0.7071, sampleRate));
    reset();
  }

  void reset() noexcept {
    std::fill(ring.begin(), ring.end(), 0.0f);
    lowpass.reset();
    phase = 0;
    written = 0;
    sinceFrame = 0;
    analysing = false;
    haveNote = false;
    note = 0;
    candidate = 0;
    candidateCount = 0;
    unvoiced = 0;
  }

  /** One input sample, every sample. */
  void push(float x) noexcept {
    const float y = lowpass.process(x);
    if (++phase >= decimation) {
      phase = 0;
      ring[static_cast<size_t>(written) & static_cast<size_t>(ringSize - 1)] = y;
      ++written;
      ++sinceFrame;
    }
  }

  /** One slice of the analysis, on every control-grid boundary. */
  void step() noexcept {
    if (!analysing) {
      if (written < frameLength || sinceFrame < hop)
        return;
      beginFrame();
    }
    for (int i = 0; i < kLagsPerStep && nextLag <= lagMax; ++i, ++nextLag)
      measureLag(nextLag);
    if (nextLag > lagMax) {
      finishFrame();
      analysing = false;
    }
  }

  /** The stable MIDI note, or -1 when there is none (fall back). */
  int stableNote() const noexcept { return haveNote ? note : -1; }

private:
  static constexpr double kAnalysisRate = 12000.0;
  static constexpr double kWindowSeconds = 0.027;
  static constexpr double kHopSeconds = 0.008;
  static constexpr double kMinHz = 38.0;    // below a 4-string bass low E (41 Hz)
  static constexpr double kMaxHz = 1400.0;  // above the highest guitar fret
  static constexpr double kLowpassHz = 1800.0;
  static constexpr float kThreshold = 0.13f;
  static constexpr float kSilenceRms = 0.0015f;  // about -56 dBFS
  static constexpr int kLagsPerStep = 12;
  static constexpr int kHoldFrames = 8;

  void beginFrame() noexcept {
    const size_t mask = static_cast<size_t>(ringSize - 1);
    const int64_t first = written - frameLength;
    for (int i = 0; i < frameLength; ++i)
      frame[static_cast<size_t>(i)] = ring[static_cast<size_t>(first + i) & mask];
    sinceFrame = 0;
    nextLag = 1;
    cumulative = 0.0;
    analysing = true;
  }

  // d(tau) = sum over the window of (x[j] - x[j + tau])^2, and its YIN
  // normalisation d(tau) * tau / sum of d(1..tau).
  void measureLag(int lag) noexcept {
    const float* a = frame.data();
    const float* b = frame.data() + lag;
    float sum = 0.0f;
    for (int j = 0; j < window; ++j) {
      const float diff = a[j] - b[j];
      sum += diff * diff;
    }
    cumulative += static_cast<double>(sum);
    normalised[static_cast<size_t>(lag)] =
        cumulative > 1.0e-12 ? static_cast<float>(static_cast<double>(sum) * lag / cumulative) : 1.0f;
  }

  void finishFrame() noexcept {
    double energy = 0.0;
    for (int j = 0; j < window; ++j)
      energy += static_cast<double>(frame[static_cast<size_t>(j)]) * frame[static_cast<size_t>(j)];
    bool voiced = std::sqrt(energy / window) > kSilenceRms;

    float frequency = 0.0f;
    if (voiced) {
      int lag = -1;
      for (int t = lagMin; t < lagMax; ++t)
        if (normalised[static_cast<size_t>(t)] < kThreshold) {
          lag = t;
          // Follow the dip down to its bottom.
          while (lag + 1 < lagMax &&
                 normalised[static_cast<size_t>(lag + 1)] < normalised[static_cast<size_t>(lag)])
            ++lag;
          break;
        }
      if (lag < 0) {
        voiced = false;
      } else {
        // Parabolic interpolation around the minimum.
        const float y0 = normalised[static_cast<size_t>(lag - 1)];
        const float y1 = normalised[static_cast<size_t>(lag)];
        const float y2 = normalised[static_cast<size_t>(lag + 1)];
        const float denominator = y0 - 2.0f * y1 + y2;
        const float shift = std::abs(denominator) > 1.0e-9f ? 0.5f * (y0 - y2) / denominator : 0.0f;
        const double refined = static_cast<double>(lag) + static_cast<double>(juce::jlimit(-1.0f, 1.0f, shift));
        frequency = static_cast<float>(analysisRate / refined);
      }
    }
    if (!voiced) {
      if (++unvoiced >= kHoldFrames) {
        haveNote = false;
        candidateCount = 0;
      }
      return;
    }
    unvoiced = 0;
    const double midi = 69.0 + 12.0 * std::log2(static_cast<double>(frequency) / 440.0);
    if (haveNote && std::abs(midi - note) <= 0.65) {
      candidateCount = 0;
      return;
    }
    const int nearest = static_cast<int>(std::lround(midi));
    if (candidateCount > 0 && nearest == candidate)
      ++candidateCount;
    else {
      candidate = nearest;
      candidateCount = 1;
    }
    if (candidateCount >= 2) {
      note = candidate;
      haveNote = true;
    }
  }

  double analysisRate{12000.0};
  int decimation{4}, window{0}, lagMin{0}, lagMax{0}, frameLength{0}, hop{0}, ringSize{0};
  std::vector<float> ring, frame, normalised;
  fx::Biquad lowpass;
  int phase{0};
  int64_t written{0};
  int sinceFrame{0};
  bool analysing{false};
  int nextLag{1};
  double cumulative{0.0};
  bool haveNote{false};
  int note{0}, candidate{0}, candidateCount{0}, unvoiced{0};
};

// ---------------------------------------------------------------------------
// The kind
// ---------------------------------------------------------------------------

constexpr int kMaxDegreeSteps = 14;  // two octaves of scale steps either way
constexpr int kMaxSemitones = 24;

juce::String ordinalName(int number) {
  switch (number) {
    case 1: return "Unison";
    case 2: return "2nd";
    case 3: return "3rd";
    case 8: return "Octave";
    default: break;
  }
  const int lastTwo = number % 100;
  const int last = number % 10;
  const char* suffix = (lastTwo >= 11 && lastTwo <= 13) ? "th"
                       : last == 1                       ? "st"
                       : last == 2                       ? "nd"
                       : last == 3                       ? "rd"
                                                         : "th";
  return juce::String(number) + suffix;
}

// "5th above", "3rd below", "Unison": index 0..28 for -14..+14 scale steps.
juce::StringArray degreeLabels() {
  juce::StringArray labels;
  for (int steps = -kMaxDegreeSteps; steps <= kMaxDegreeSteps; ++steps) {
    if (steps == 0)
      labels.add("Unison");
    else
      labels.add(ordinalName(std::abs(steps) + 1) + (steps > 0 ? " above" : " below"));
  }
  return labels;
}

class HarmonizerProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "pitch.harmonizer";

  enum Param {
    kMode = 0,
    kKey,
    kScale,
    kVoice1Semitones,
    kVoice1Degree,
    kVoice1Db,
    kVoice1Pan,
    kVoice2Semitones,
    kVoice2Degree,
    kVoice2Db,
    kVoice2Pan,
    kWindow
  };
  static constexpr int kParamsPerVoice = 4;

  // A level knob at its floor means "voice off" (no engine runs for it).
  static constexpr float kOffDb = -60.0f;

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::choiceParam("mode", "Mode", {"Chromatic", "Scale"}, 0));
      t.push_back(BlockParamSpec::choiceParam(
          "key", "Key", {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}, 0));
      t.push_back(BlockParamSpec::choiceParam("scale", "Scale", {"Major", "Minor"}, 0));
      // Voice 1: a third up; voice 2: a fifth up (a triad over the note).
      addVoice(t, 1, 4.0f, 2, -3.0f, -50.0f);
      addVoice(t, 2, 7.0f, 4, -6.0f, 50.0f);
      t.push_back(BlockParamSpec::choiceParam("window", "Window", pitchfx::windowLabels(),
                                              static_cast<int>(PitchShift::kDefaultWindow))
                      .withAffectsLatency()
                      .withAffectsTail()
                      .notAutomatable());
      return t;
    }();
    return table;
  }

  HarmonizerProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  int latencySamples() const noexcept override { return latch.latencySamples(); }
  bool latencyChangePending() const noexcept override { return latch.pending(param(kWindow)); }
  void commitLatency() override { latch.commit(param(kWindow)); }
  double tailSeconds() const noexcept override { return latch.tailSeconds(); }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    latch.prepare(sampleRate, param(kWindow), 24.0);
    for (auto& voice : voices)
      voice.prepare(sampleRate, spec.maxBlockSize, latch.current(), latch.latencySamples());
    for (auto& copy : scratch)
      copy.setSize(2, juce::jmax(1, spec.maxBlockSize), false, true, false);
    for (auto& tracker : trackers)
      tracker.prepare(sampleRate);
    for (auto& smoother : gains)
      smoother.prepare(sampleRate, kLevelRampSeconds);
    for (auto& perVoice : glides)
      for (auto& glide : perVoice)
        glide.prepare(sampleRate, kGlideSeconds);
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
    // Voices may be placed only where a stereo image survives (no amp model
    // later in the lane, no dual mono); elsewhere every voice is centred.
    const bool image = blockHasStereoImage(numChannels, ctx);
    const bool scaleMode = param(kMode) >= 0.5f;
    const int lanes = dual ? 2 : 1;

    // Gains: level, and for the image a balance law that is unity at the
    // centre and sends the voice to one side at the extremes.
    bool running[kNumVoices];
    for (int v = 0; v < kNumVoices; ++v) {
      const int base = kVoice1Semitones + v * kParamsPerVoice;
      const float db = param(base + 2);
      const float level = db <= kOffDb + 0.01f ? 0.0f : fx::dbToGain(db);
      float left = level, right = level;
      if (image) {
        const float angle = (juce::jlimit(-100.0f, 100.0f, param(base + 3)) * 0.01f + 1.0f) *
                            juce::MathConstants<float>::pi * 0.25f;
        left = level * juce::MathConstants<float>::sqrt2 * std::cos(angle);
        right = level * juce::MathConstants<float>::sqrt2 * std::sin(angle);
      }
      gains[v * 2].setTarget(left);
      gains[v * 2 + 1].setTarget(right);
      running[v] = level > 0.0f || gains[v * 2].isSmoothing() || gains[v * 2 + 1].isSmoothing();
      if (running[v])
        copyInput(buffer, scratch[v], numChannels, numSamples);
      else
        voices[v].markCold();
    }
    // The tracker only runs when something needs the note.
    const bool tracking = scaleMode && (running[0] || running[1]);
    if (!tracking && !trackersClean) {
      for (auto& tracker : trackers)
        tracker.reset();
      trackersClean = true;
    }
    if (tracking)
      trackersClean = false;

    const PitchShift::Window window = latch.current();
    float* io[2] = {buffer.getWritePointer(0), numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    const int key = static_cast<int>(std::lround(param(kKey)));
    const bool minor = param(kScale) >= 0.5f;

    grid.run(numSamples, [&](int start, int length, bool boundary) {
      if (boundary) {
        for (int lane = 0; lane < lanes; ++lane) {
          if (tracking)
            trackers[lane].step();
          // The note this lane's player is on, or none: the voices then use
          // their chromatic interval.
          const int note = tracking ? trackers[lane].stableNote() : -1;
          for (int v = 0; v < kNumVoices; ++v) {
            const int base = kVoice1Semitones + v * kParamsPerVoice;
            float target = param(base);
            if (note >= 0) {
              const int steps = static_cast<int>(std::lround(param(base + 1))) - kMaxDegreeSteps;
              target = static_cast<float>(
                  juce::jlimit(-kMaxSemitones, kMaxSemitones, diatonicInterval(note, key, minor, steps)));
            }
            glides[v][lane].setTarget(target);
            laneSemitones[v][lane] = glides[v][lane].tick();
          }
        }
      }
      for (int v = 0; v < kNumVoices; ++v) {
        if (!running[v])
          continue;
        float* pointers[2] = {scratch[v].getWritePointer(0),
                              numChannels > 1 ? scratch[v].getWritePointer(1) : nullptr};
        voices[v].process(pointers, numChannels, dual, start, length, laneSemitones[v], window, latch.latencySamples());
      }
      mixSegment(io, numChannels, start, length, tracking, dual, running);
    });
  }

private:
  void clearState() noexcept {
    for (auto& tracker : trackers)
      tracker.reset();
    trackersClean = true;
    for (auto& smoother : gains)
      smoother.snapOnNextTarget();
    for (auto& perVoice : glides)
      for (auto& glide : perVoice)
        glide.snap();
    grid.reset();
  }

  static constexpr int kNumVoices = 2;
  static constexpr double kLevelRampSeconds = 0.02;
  static constexpr double kGlideSeconds = 0.015;

  static void addVoice(BlockParamTable& t, int number, float semitones, int defaultSteps, float db,
                       float pan) {
    const juce::String n(number);
    t.push_back(BlockParamSpec::floatParam("voice" + n + "Semitones", "Voice " + n + " Interval",
                                           "st", -24.0f, 24.0f, semitones)
                    .withStep(1.0f));
    t.push_back(BlockParamSpec::choiceParam("voice" + n + "Degree", "Voice " + n + " Degree",
                                            degreeLabels(), kMaxDegreeSteps + defaultSteps));
    t.push_back(BlockParamSpec::floatParam("voice" + n + "Db", "Voice " + n + " Level", "dB", kOffDb,
                                           6.0f, db)
                    .withStep(0.5f));
    t.push_back(BlockParamSpec::floatParam("voice" + n + "Pan", "Voice " + n + " Pan", "%", -100.0f,
                                           100.0f, pan)
                    .withStep(1.0f));
  }

  static void copyInput(const juce::AudioBuffer<float>& from, juce::AudioBuffer<float>& to,
                        int numChannels, int numSamples) noexcept {
    for (int ch = 0; ch < numChannels; ++ch)
      std::copy_n(from.getReadPointer(ch), numSamples, to.getWritePointer(ch));
  }

  void mixSegment(float* const* io, int numChannels, int start, int length, bool tracking,
                  bool dual, const bool (&running)[kNumVoices]) noexcept {
    for (int i = start; i < start + length; ++i) {
      float gainL[kNumVoices], gainR[kNumVoices];
      for (int v = 0; v < kNumVoices; ++v) {
        gainL[v] = gains[v * 2].next();
        gainR[v] = gains[v * 2 + 1].next();
      }
      if (tracking) {
        if (dual) {
          trackers[0].push(io[0][i]);
          trackers[1].push(io[1][i]);
        } else {
          trackers[0].push(numChannels > 1 ? 0.5f * (io[0][i] + io[1][i]) : io[0][i]);
        }
      }
      for (int ch = 0; ch < numChannels; ++ch) {
        float out = 0.0f;
        for (int v = 0; v < kNumVoices; ++v)
          if (running[v])
            out += (ch == 0 ? gainL[v] : gainR[v]) * scratch[v].getReadPointer(ch)[i];
        io[ch][i] = out;
      }
    }
  }

  pitchfx::PitchVoice voices[kNumVoices];
  juce::AudioBuffer<float> scratch[kNumVoices];
  NoteTracker trackers[2];
  bool trackersClean{true};
  fx::SmoothedParam gains[kNumVoices * 2];
  pitchfx::Glide glides[kNumVoices][2];
  float laneSemitones[kNumVoices][pitchfx::kMaxLanes] = {};
  pitchfx::ControlGrid grid;
  pitchfx::WindowLatch latch;
  double sampleRate{48000.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(Harmonizer) {
  // The voices are heard beside the guitar, so the mix keeps the dry signal
  // at unity up to 50 % (where the voices reach their own set level) and only
  // then fades it. A new block starts there.
  addEffectKind<HarmonizerProcessor>(registry, "Harmonizer", BlockCategory::Pitch, 0.5f,
                                     BlockMixLaw::DryPreserving);
}

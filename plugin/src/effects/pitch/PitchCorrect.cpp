// pitch.correct "Pitch Correct": moves a monophonic note (guitar, bass, a
// voice-like line) toward the nearest note of a chosen key and scale. The note
// is found by the pitch family's tracker (detail/PitchTracker.h, McLeod's
// method) and moved by the family's shifting engine (detail/PitchVoice.h, the
// correlation-spliced delay line of PitchShift.h); this file is the control law
// between the two.
//
// The target. Each tracker frame (every 5 ms) gives the played pitch as a
// continuous MIDI note. Detune moves the whole grid of target notes by up to
// half a semitone (a reference other than A = 440 Hz), and the target is the
// nearest note of the scale on that grid. A note that sits near the midpoint of
// two scale notes keeps the target it had until it is 15 cents past the
// midpoint, so a held note on the border does not flip between two targets.
// The scales are chromatic, the nine scales of detail/ScaleMap.h in the chosen
// key, and Custom: twelve switches, one per note name (C to B, absolute, the key
// is not used), for any set of notes. A custom set with no note on corrects
// nothing.
//
// The correction is computed in cents and is octave blind: a tracker frame that
// reads a note an octave off asks for the same correction, because the nearest
// scale note is an octave off too.
//
//  - Amount scales the correction: 50 % moves a note half way, in cents.
//  - Humanize keeps a sustained note slightly loose. Over the first 150 ms of a
//    note the correction is whole; after that the note's own movement around
//    its centre (a slow average of the correction, 150 ms) is let through up
//    to a bend allowance of 25 cents at 100 %, softly limited (A tanh(v / A)),
//    while the centre itself stays corrected. A vibrato or a small bend
//    survives, the note stays in tune on average.
//  - Speed is the retune time: how long the output takes to cover 10 % to 90 %
//    of a change of the correction (a new note, a jump to another target). It
//    is a one-pole in cents with a time constant of speed / ln 9; 0 is instant
//    (hard tune). The frame-rate correction is first interpolated linearly over
//    one frame, so a hard tune does not step every 5 ms.
//
// Unvoiced and uncertain input passes through unshifted. A note is corrected
// once two frames in a row are confident (so a stray confident frame in noise
// does nothing) and is let go after three that are not; the block crossfades
// between the engine and the dry signal (delayed by the block's latency) over
// 20 ms either way, and the correction starts again from zero for the next
// note. A note that needs less than half a cent of correction is played by the
// delayed dry signal itself (it takes 1.5 cents to go back to the engine), so a
// note in tune is untouched, bit for bit.
//
// Tracking range: Guitar (70 Hz to 1.4 kHz, the tracker's own setting), Bass
// (28 Hz to 1 kHz: a five-string's low B, with a longer window, so a low note is
// known later) and Voice (60 Hz to 1.4 kHz, accepting a less periodic signal).
//
// Latency is the engine's constant attack latency (PitchShift.h) for the latched
// window and corrections of up to 7 semitones; the dry path is delayed by the
// same amount. The correction follows the tracker, whose analysis window ends
// at the newest sample, so the shift applied to a note lags the audio it
// corrects by about the tracker's window less the block's latency: a few ms on
// guitar and voice, about 25 ms for the lowest bass notes. Formants are not
// preserved (a guitar does not need it); the engine is the same as every other
// pitch kind's, so is its sound.
//
// Mono in, mono or stereo out like the other pitch kinds: a stereo buffer is
// tracked on its mid and shifted with one control path; in dual mono each
// channel has its own tracker, engine and correction.
//
// Readings: the detected note and frequency, the tracker's confidence and the
// target note (with the detune as a fraction); the correction applied is the
// difference of the last two.
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "effects/EffectDsp.h"
#include "detail/PitchTracker.h"
#include "detail/PitchVoice.h"
#include "detail/ScaleMap.h"

namespace {

class PitchCorrectProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "pitch.correct";

  static constexpr int kNoteNames = 12;
  // Chromatic, then the nine scales of ScaleMap.h other than its chromatic one,
  // then Custom.
  static constexpr int kChromaticChoice = 0;
  static constexpr int kCustomChoice = pitchfx::kNumScales;
  static constexpr int kNumRanges = 3;

  enum Param {
    kKey = 0,
    kScale,
    kSpeedMs,
    kAmount,
    kHumanize,
    kDetune,
    kRange,
    kNoteFirst,  // twelve switches, C to B
    kWindow = kNoteFirst + kNoteNames
  };
  enum Reading { kDetectedNote = 0, kDetectedHz, kConfidence, kTargetNote };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::choiceParam("key", "Key", pitchfx::keyNames(), 0));
      t.push_back(BlockParamSpec::choiceParam("scale", "Scale", scaleChoices(), kChromaticChoice));
      // The retune time, 10 % to 90 %: 0 is a hard tune.
      t.push_back(BlockParamSpec::floatParam("speedMs", "Speed", "ms", 0.0f, 400.0f, 20.0f)
                      .withSkew(0.5f)
                      .withStep(1.0f));
      t.push_back(BlockParamSpec::floatParam("amount", "Amount", "%", 0.0f, 100.0f, 100.0f).withStep(1.0f));
      t.push_back(BlockParamSpec::floatParam("humanize", "Humanize", "%", 0.0f, 100.0f, 0.0f).withStep(1.0f));
      // The grid of target notes, moved up or down.
      t.push_back(BlockParamSpec::floatParam("detuneCents", "Detune", "ct", -50.0f, 50.0f, 0.0f).withStep(1.0f));
      t.push_back(BlockParamSpec::choiceParam("range", "Range", {"Guitar", "Bass", "Voice"}, 0).notAutomatable());
      // The Custom scale: which note names are targets. The white keys by
      // default (C major, A minor).
      const juce::StringArray names = pitchfx::keyNames();
      const char* ids[kNoteNames] = {"noteC",  "noteCs", "noteD",  "noteDs", "noteE",  "noteF",
                                     "noteFs", "noteG",  "noteGs", "noteA",  "noteAs", "noteB"};
      for (int n = 0; n < kNoteNames; ++n)
        t.push_back(BlockParamSpec::boolParam(ids[n], "Custom " + names[n], names[n].length() == 1));
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
        {"targetNote", "Target", "note", 0.0f, 127.0f},
    };
    return table;
  }

  static_assert(pitchfx::kScales[pitchfx::kNumScales - 1].size == 12,
                "the scale choices put chromatic first and the others in ScaleMap.h's order");

  static juce::StringArray scaleChoices() {
    juce::StringArray names{"Chromatic"};
    for (int s = 0; s < pitchfx::kNumScales; ++s)
      if (pitchfx::kScales[static_cast<size_t>(s)].size < 12)
        names.add(pitchfx::kScales[static_cast<size_t>(s)].name);
    names.add("Custom");
    return names;
  }

  PitchCorrectProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs(), readingSpecs()) {}

  int latencySamples() const noexcept override { return latch.latencySamples(); }
  bool latencyChangePending() const noexcept override { return latch.pending(param(kWindow)); }
  void commitLatency() override { latch.commit(param(kWindow)); }
  double tailSeconds() const noexcept override { return latch.tailSeconds(); }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    latch.prepare(sampleRate, param(kWindow), kMaxCorrectionSemitones);
    voice.prepare(sampleRate, spec.maxBlockSize, latch.current(), latch.latencySamples());
    const pitchfx::PitchTracker::Range ranges[kNumRanges] = {
        pitchfx::PitchTracker::Range::guitar(), pitchfx::PitchTracker::Range::bass(),
        pitchfx::PitchTracker::Range::voice()};
    for (auto& lane : lanes)
      for (int r = 0; r < kNumRanges; ++r)
        lane.trackers[r].prepare(sampleRate, ranges[r]);
    dryLine.prepare(2, PitchShift::windowSamples(PitchShift::Window::ms60, sampleRate) + 16);
    input.setSize(2, juce::jmax(1, spec.maxBlockSize), false, true, false);
    aligned.setSize(2, juce::jmax(1, spec.maxBlockSize), false, true, false);
    controlSeconds = pitchfx::kControlGrid / sampleRate;
    wetStep = static_cast<float>(1.0 / (kWetFadeSeconds * sampleRate));
    centreCoeff = static_cast<float>(1.0 - std::exp(-controlSeconds / kCentreSeconds));
    clearState();
  }

  void resetEffect() noexcept override {
    voice.markCold();
    clearState();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), 2);
    if (numChannels < 1 || numSamples < 1)
      return;
    const bool dual = ctx.dualMono && numChannels == 2;
    const int laneCount = dual ? 2 : 1;

    // A new range starts its trackers from rest.
    const int range = juce::jlimit(0, kNumRanges - 1, static_cast<int>(std::lround(param(kRange))));
    if (range != activeRange) {
      activeRange = range;
      for (auto& lane : lanes)
        lane.restart(range);
    }
    Settings settings = readSettings();

    const PitchShift::Window window = latch.current();
    const int delay = latch.latencySamples();
    float* io[2] = {buffer.getWritePointer(0), numChannels > 1 ? buffer.getWritePointer(1) : nullptr};

    // The input as it came, for the trackers, and the dry signal as late as
    // the block reports, before the engine overwrites the buffer.
    for (int ch = 0; ch < numChannels; ++ch) {
      std::copy_n(io[ch], numSamples, input.getWritePointer(ch));
      float* late = aligned.getWritePointer(ch);
      for (int i = 0; i < numSamples; ++i) {
        late[i] = dryLine.read(ch, delay);
        dryLine.write(ch, io[ch][i]);
      }
    }

    grid.run(numSamples, [&](int start, int length, bool boundary) {
      if (boundary)
        for (int l = 0; l < laneCount; ++l)
          control(lanes[l], settings);
      const float semitones[pitchfx::kMaxLanes] = {0.01f * lanes[0].shift, 0.01f * lanes[1].shift};
      voice.process(io, numChannels, dual, start, length, semitones, window, delay);
      for (int i = start; i < start + length; ++i) {
        const float* in0 = input.getReadPointer(0);
        if (dual) {
          lanes[0].tracker().push(in0[i]);
          lanes[1].tracker().push(input.getReadPointer(1)[i]);
        } else {
          lanes[0].tracker().push(numChannels > 1 ? 0.5f * (in0[i] + input.getReadPointer(1)[i]) : in0[i]);
        }
        float wet[2];
        for (int l = 0; l < laneCount; ++l) {
          Lane& lane = lanes[l];
          lane.wet += juce::jlimit(-wetStep, wetStep, lane.wetTarget - lane.wet);
          wet[l] = lane.wet;
        }
        for (int ch = 0; ch < numChannels; ++ch) {
          const float w = wet[dual ? ch : 0];
          const float late = aligned.getReadPointer(ch)[i];
          // Exact at both ends: the delayed dry, bit for bit, at 0.
          io[ch][i] = io[ch][i] * w + late * (1.0f - w);
        }
      }
    });

    const Lane& first = lanes[0];
    const pitchfx::PitchTracker& tracker = first.trackers[activeRange];
    setReading(kDetectedNote, tracker.hasNote() ? tracker.midiNote() : 0.0f);
    setReading(kDetectedHz, tracker.hasNote() ? tracker.frequencyHz() : 0.0f);
    setReading(kConfidence, 100.0f * tracker.confidence());
    setReading(kTargetNote, first.haveTarget ? static_cast<float>(first.target) + settings.gridOffset : 0.0f);
  }

private:
  // What a block's controls come to, read once per block.
  struct Settings {
    uint32_t mask{0};       // bit n: note name n (C = 0) is a target
    float gridOffset{0.0f}; // semitones the target grid is moved by
    float amount{1.0f};
    float allowanceCents{0.0f};
    float retuneCoeff{1.0f};  // per control step
  };

  struct Lane {
    pitchfx::PitchTracker trackers[kNumRanges];
    int range{0};
    int lastFrames{0};
    int voicedRun{0}, unvoicedRun{0};
    int stepsSinceFrame{0};
    bool active{false};  // a confirmed note is being corrected
    bool haveTarget{false};
    int target{0};       // the target note on the grid
    float centre{0.0f};  // the slow centre of the correction (humanize), cents
    float sustain{0.0f}; // seconds on the current target
    float frameDesired{0.0f}, desired{0.0f}, rampStep{0.0f};
    int rampLeft{0};
    float shift{0.0f};  // the correction applied, cents
    bool unison{true};
    float wet{0.0f}, wetTarget{0.0f};

    pitchfx::PitchTracker& tracker() noexcept { return trackers[range]; }

    void restart(int newRange) noexcept {
      range = newRange;
      trackers[range].reset();
      lastFrames = 0;
      voicedRun = unvoicedRun = stepsSinceFrame = 0;
      active = haveTarget = false;
      target = 0;
      centre = sustain = 0.0f;
      frameDesired = desired = rampStep = 0.0f;
      rampLeft = 0;
      unison = true;
      wetTarget = 0.0f;
    }
  };

  Settings readSettings() const noexcept {
    Settings s;
    const int choice = juce::jlimit(0, kCustomChoice, static_cast<int>(std::lround(param(kScale))));
    if (choice == kCustomChoice) {
      for (int n = 0; n < kNoteNames; ++n)
        if (param(kNoteFirst + n) >= 0.5f)
          s.mask |= 1u << n;
    } else if (choice == kChromaticChoice) {
      s.mask = 0xfffu;
    } else {
      const int key = juce::jlimit(0, 11, static_cast<int>(std::lround(param(kKey))));
      const pitchfx::ScaleDef& scale = pitchfx::kScales[static_cast<size_t>(choice - 1)];
      for (int i = 0; i < scale.size; ++i)
        s.mask |= 1u << ((key + scale.offsets[i]) % 12);
    }
    s.gridOffset = 0.01f * juce::jlimit(-50.0f, 50.0f, param(kDetune));
    s.amount = 0.01f * juce::jlimit(0.0f, 100.0f, param(kAmount));
    s.allowanceCents = 0.01f * juce::jlimit(0.0f, 100.0f, param(kHumanize)) * kLooseCents;
    const double speed = 0.001 * juce::jlimit(0.0f, 400.0f, param(kSpeedMs));
    // A one-pole whose 10 % to 90 % time is `speed`.
    s.retuneCoeff = speed > 1.0e-6 ? static_cast<float>(1.0 - std::exp(-controlSeconds * std::log(9.0) / speed))
                                   : 1.0f;
    return s;
  }

  // Once per control step, for one lane.
  void control(Lane& lane, const Settings& s) noexcept {
    pitchfx::PitchTracker& tracker = lane.tracker();
    tracker.step();
    ++lane.stepsSinceFrame;
    if (tracker.frames() != lane.lastFrames) {
      lane.lastFrames = tracker.frames();
      onFrame(lane, tracker, s);
    }
    if (lane.rampLeft > 0) {
      lane.desired += lane.rampStep;
      if (--lane.rampLeft == 0)
        lane.desired = lane.frameDesired;
    }
    if (lane.active) {
      lane.shift += s.retuneCoeff * (lane.desired - lane.shift);
      lane.shift = juce::jlimit(-kMaxCorrectionCents, kMaxCorrectionCents, lane.shift);
      if (lane.unison && std::abs(lane.frameDesired) > kLeaveUnisonCents)
        lane.unison = false;
      else if (!lane.unison && std::abs(lane.frameDesired) < kEnterUnisonCents &&
               std::abs(lane.shift) < kEnterUnisonCents)
        lane.unison = true;
      lane.wetTarget = lane.unison ? 0.0f : 1.0f;
    } else {
      lane.wetTarget = 0.0f;
      // Once the dry signal has taken over, the next note starts from no
      // correction at all.
      if (lane.wet <= 0.0f) {
        lane.shift = lane.desired = lane.frameDesired = 0.0f;
        lane.rampLeft = 0;
        lane.haveTarget = false;
        lane.unison = true;
      }
    }
  }

  void onFrame(Lane& lane, const pitchfx::PitchTracker& tracker, const Settings& s) noexcept {
    const int steps = std::max(1, lane.stepsSinceFrame);
    lane.stepsSinceFrame = 0;
    if (tracker.isVoiced()) {
      ++lane.voicedRun;
      lane.unvoicedRun = 0;
    } else {
      ++lane.unvoicedRun;
      lane.voicedRun = 0;
    }
    if (!lane.active && lane.voicedRun >= kConfirmFrames)
      lane.active = true;
    if (lane.active && lane.unvoicedRun >= kReleaseFrames)
      lane.active = false;
    if (!lane.active || !tracker.isVoiced())
      return;  // an uncertain frame inside a note keeps the correction

    const double played = 69.0 + 12.0 * std::log2(static_cast<double>(tracker.frequencyHz()) / 440.0);
    const double onGrid = played - static_cast<double>(s.gridOffset);
    float correction = 0.0f;
    if (s.mask != 0u) {
      const int chosen = nearestTarget(lane, onGrid, s.mask);
      const bool newNote = !lane.haveTarget || chosen != lane.target;
      lane.target = chosen;
      lane.haveTarget = true;
      const float full = static_cast<float>(100.0 * (chosen - onGrid));
      const float elapsed = static_cast<float>(steps * controlSeconds);
      if (newNote) {
        lane.centre = full;
        lane.sustain = 0.0f;
      } else {
        lane.sustain += elapsed;
        const float coeff = 1.0f - std::pow(1.0f - centreCoeff, static_cast<float>(steps));
        lane.centre += coeff * (full - lane.centre);
      }
      correction = full;
      const float allowance =
          s.allowanceCents * juce::jlimit(0.0f, 1.0f, lane.sustain / static_cast<float>(kSustainSeconds));
      if (allowance > 1.0e-3f) {
        // The movement around the centre, let through up to the allowance.
        const float around = full - lane.centre;
        correction = full - allowance * std::tanh(around / allowance);
      }
    } else {
      lane.haveTarget = false;
    }
    lane.frameDesired = s.amount * correction;
    // Reached over as long as the last frame took to come.
    lane.rampLeft = steps;
    lane.rampStep = (lane.frameDesired - lane.desired) / static_cast<float>(steps);
  }

  // C = 0 ... B = 11, for any note number.
  static constexpr int noteName(int note) noexcept { return note - 12 * pitchfx::floorDivide(note, 12); }

  // The nearest target to `onGrid` (a note number on the grid), keeping the
  // current one while it is within the hysteresis of the nearest.
  static int nearestTarget(const Lane& lane, double onGrid, uint32_t mask) noexcept {
    const int centreNote = static_cast<int>(std::lround(onGrid));
    int best = centreNote;
    double bestDistance = 1.0e9;
    for (int k = -7; k <= 7; ++k) {
      const int note = centreNote + k;
      if ((mask & (1u << noteName(note))) == 0u)
        continue;
      const double distance = std::abs(onGrid - note);
      if (distance < bestDistance) {
        bestDistance = distance;
        best = note;
      }
    }
    if (lane.haveTarget && best != lane.target) {
      const int held = lane.target;
      if ((mask & (1u << noteName(held))) != 0u && std::abs(onGrid - held) < bestDistance + kHysteresisSemitones)
        return held;
    }
    return best;
  }

  void clearState() noexcept {
    for (auto& lane : lanes) {
      for (auto& tracker : lane.trackers)
        tracker.reset();
      lane.restart(activeRange);
      lane.shift = 0.0f;
      lane.wet = 0.0f;
    }
    dryLine.clear();
    grid.reset();
  }

  // Corrections reach at most half the widest gap of a scale (a custom set of
  // one note: six semitones) plus the detune.
  static constexpr double kMaxCorrectionSemitones = 7.0;
  static constexpr float kMaxCorrectionCents = 700.0f;
  static constexpr double kWetFadeSeconds = 0.02;
  static constexpr int kConfirmFrames = 2;
  static constexpr int kReleaseFrames = 3;
  static constexpr double kHysteresisSemitones = 0.15;
  static constexpr float kLooseCents = 25.0f;
  static constexpr double kCentreSeconds = 0.15;
  static constexpr double kSustainSeconds = 0.15;
  static constexpr float kEnterUnisonCents = 0.5f;
  static constexpr float kLeaveUnisonCents = 1.5f;

  pitchfx::PitchVoice voice;
  pitchfx::WindowLatch latch;
  pitchfx::ControlGrid grid;
  Lane lanes[2];
  int activeRange{0};
  fx::DelayLine dryLine;
  juce::AudioBuffer<float> input, aligned;
  double sampleRate{48000.0};
  double controlSeconds{pitchfx::kControlGrid / 48000.0};
  float wetStep{0.001f};
  float centreCoeff{0.1f};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(PitchCorrect) {
  // The corrected signal replaces the dry one, 100 % by default; the common mix
  // blends the dry back in (delayed by the latency, as for every latent kind).
  addEffectKind<PitchCorrectProcessor>(registry, "Pitch Correct", BlockCategory::Pitch, 1.0f,
                                       BlockMixLaw::Crossfade);
}

// delay.crystal: two pitch-shifted echo taps, each on its own delay line with
// its own time, each shifting again on every trip round its own feedback
// loop. Tap 1 is the crystal: an octave up by default, so a note comes back an
// octave higher, then two, then three, climbing into a shimmer. Tap 2 is a
// detuned double: a few cents flat by default, so its repeats drift a little
// further from the note each time, like a slow chorus that walks away. Each tap
// has a level in dB and a pan; the pan mode puts both in the middle instead.
//
// The shifter is the pitch family's engine (PitchShift, through PitchVoice):
// a correlation-spliced delay line whose joints land a whole number of
// periods apart, the cleanest engine the repo has. A loop applies a shifter
// dozens of times to the same note, and blind grains turn into an audible
// flutter after a few passes where matched splices stay smooth. The engine
// works in a loop because it is per-sample and its attack latency is a
// constant (constant-latency mode): the loop reads its input that many
// samples early, so the shifted repeat lands on the set time, and the loop
// runs in 16-sample segments, far shorter than the shortest time, so every
// sample a segment reads was written before it. The times are when the echo
// is heard: the shifter's latency is inside them, not added to them.
//
// One pass round each loop (tap 1 shown; tap 2 is the same without the
// harmony and the direct share):
//
//   line --read at T - L--> band limit --> tap 1 shifter ----------+
//                                     \--> harmony (off, -12, +7) --+--> voices
//   line --read at T-----------------------------------------------> direct
//   repeat = softLimit(voices * (1 - direct share) + direct * direct share)
//   line <- softLimit(lowCut(tone(input + feedback * repeat)))
//   out  += level * pan * repeat
//
// The tone and the low cut sit where the line is written, so the first echo is
// shaped as well as every repeat after it, and each pass is shaped again.
//
// Keeping each pass clean and the loop bounded:
//  - the band limit before the shifters follows the interval: the top is
//    held to 12 kHz / ratio (an upward loop cannot fold fizz into the top
//    octave, and what climbs out of the band is gone after a pass or two),
//    the bottom to 30 Hz / ratio (a falling loop cannot pile rumble or DC);
//  - tap 1's two voices are summed at a level normalised for their
//    correlation (equal power when their intervals differ, equal amplitude
//    when they coincide), and the direct share is a crossfade, so the loop
//    gain is the feedback at most;
//  - the soft limit of the family (DelayCommon.h) is the ceiling for a hot
//    input at 95 %;
//  - at exactly 0 semitones and 0 cents a tap crossfades to the plain read,
//    so a unison loop is an ordinary clean delay.
//
// Levels and pans are applied after the loop, so they never change how long
// it rings. The pan is constant power with the tap at cos / sin of an angle
// running 0 to 90 degrees across the range: a tap in the middle is 3 dB down on
// each side, a tap hard to one side is at its level on that side.
//
// Layouts. In a true stereo image the two sides are summed to one signal that
// feeds both lines, and the taps are panned. Everywhere else (a mono buffer,
// dual mono, a stereo image an amp model later in the lane would throw away)
// every channel runs both taps on its own input and hears what a mono fold of
// the panned pair would give (the average of the two sides), so the levels
// stay what they are in stereo.
//
// The dry path is untouched (no latency); everything heard is the wet.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <optional>

#include "../pitch/detail/PitchVoice.h"
#include "DelayCommon.h"
#include "effects/EffectDsp.h"

namespace {

using namespace delayfx;

class CrystalDelayProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "delay.crystal";

  // Table order. The table is matched to saved state by id, never by
  // position: the ids of the single-tap version (sync, division, timeMs,
  // feedback, pitch, voice2, voice2Level, direct, toneHz, lowCutHz,
  // modDepth) keep their meaning, and tap 2 and the levels and pans are new.
  enum Param {
    kSync = 0,
    kDivision,
    kTimeMs,
    kPitch,
    kDetune1,
    kTap1Db,
    kTap1Pan,
    kTap2Sync,
    kTap2Division,
    kTap2TimeMs,
    kPitch2,
    kDetune2,
    kTap2Db,
    kTap2Pan,
    kPanMode,
    kFeedback,
    kToneHz,
    kLowCutHz,
    kModDepth,
    kHarmony,
    kHarmonyLevel,
    kDirect,
    kDuckDb,
    kDuckReleaseMs,
    kSpillover
  };

  enum PanMode { kPanCentre = 0, kPanPanned };

  static constexpr double kMaxDelaySeconds = 6.0;
  // The read swings by +-this at a modulation depth of 100 %: a few cents of
  // drift that builds up gently over the repeats.
  static constexpr double kMaxModSeconds = 0.001;
  static constexpr double kModRateHz = 0.45;
  // A tap level at the bottom of its range is off: its shifter stops.
  static constexpr float kOffDb = -60.0f;

  // The state layout: 1 is the single-tap block, 2 adds tap 2, the levels,
  // the pans and the pan mode (see setKindState).
  static constexpr int kStateVersion = 2;

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      fx::TimeParamOptions time1;
      time1.minMs = 50.0f;
      time1.maxMs = static_cast<float>(kMaxDelaySeconds * 1000.0);
      time1.defaultMs = 400.0f;
      time1.affectsTail = true;
      time1.name = "Time 1";
      fx::appendTimeParams(t, time1);
      t.push_back(
          BlockParamSpec::floatParam("pitch", "Pitch 1", "st", -12.0f, 12.0f, 12.0f).withStep(1.0f));
      t.push_back(BlockParamSpec::floatParam("detune1", "Detune 1", "cents", -50.0f, 50.0f, 0.0f)
                      .withStep(0.1f));
      t.push_back(BlockParamSpec::floatParam("tap1Db", "Level 1", "dB", kOffDb, 6.0f, 0.0f));
      t.push_back(BlockParamSpec::floatParam("tap1Pan", "Pan 1", "%", -100.0f, 100.0f, -50.0f)
                      .withStep(1.0f));
      fx::TimeParamOptions time2 = time1;
      time2.defaultMs = 600.0f;
      time2.defaultDivision = fx::kDivQuarter;
      time2.name = "Time 2";
      time2.idPrefix = "tap2";
      fx::appendTimeParams(t, time2);
      t.push_back(
          BlockParamSpec::floatParam("pitch2", "Pitch 2", "st", -12.0f, 12.0f, 0.0f).withStep(1.0f));
      t.push_back(BlockParamSpec::floatParam("detune2", "Detune 2", "cents", -50.0f, 50.0f, -6.0f)
                      .withStep(0.1f));
      t.push_back(BlockParamSpec::floatParam("tap2Db", "Level 2", "dB", kOffDb, 6.0f, 0.0f));
      t.push_back(BlockParamSpec::floatParam("tap2Pan", "Pan 2", "%", -100.0f, 100.0f, 50.0f)
                      .withStep(1.0f));
      t.push_back(BlockParamSpec::choiceParam("panMode", "Pan Mode", juce::StringArray{"Centre", "Panned"},
                                              kPanPanned));
      t.push_back(BlockParamSpec::floatParam("feedback", "Feedback", "%", 0.0f, 95.0f, 50.0f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("toneHz", "Tone", "Hz", 1000.0f, 20000.0f, 16000.0f)
                      .withSkew(0.4f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("lowCutHz", "Low Cut", "Hz", 20.0f, 2000.0f, 50.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("modDepth", "Mod Depth", "%", 0.0f, 100.0f, 20.0f)
                      .withAffectsTail());
      // A second shifter on tap 1's line, in its loop (the single-tap
      // version's "Voice 2": same id, same choices).
      t.push_back(BlockParamSpec::choiceParam("voice2", "Harmony",
                                              juce::StringArray{"Off", "Octave Down", "Fifth"}, 0));
      t.push_back(BlockParamSpec::floatParam("voice2Level", "Harmony Level", "%", 0.0f, 100.0f, 50.0f));
      t.push_back(BlockParamSpec::floatParam("direct", "Direct", "%", 0.0f, 100.0f, 0.0f));
      appendDuckParams(t);
      t.push_back(spilloverParam());
      return t;
    }();
    return table;
  }

  CrystalDelayProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  int kindStateVersion() const noexcept override { return kStateVersion; }

  // A block saved by the single-tap version had one voice, heard at unity on
  // both sides. It loads as that: tap 2 off, tap 1 in the middle and 3 dB up
  // (the pan law's middle is 3 dB down on each side), so it sounds where it
  // did.
  void setKindState(const juce::ValueTree& kindState) override {
    ParameterisedBlockProcessor::setKindState(kindState);
    if (kindState.hasProperty("version") && static_cast<int>(kindState.getProperty("version")) < 2) {
      setParameter(kTap2Db, kOffDb);
      setParameter(kPanMode, static_cast<float>(kPanCentre));
      setParameter(kTap1Db, 20.0f * std::log10(juce::MathConstants<float>::sqrt2));
    }
  }

  // Until the repeats are 60 dB down. The voice mix, the filters and the
  // limit only lower the loop gain, so the feedback sets the count, and the
  // longer of the two lines the time per pass. Each pass can run late by the
  // shifter's wander (a sustained note is read anywhere between the window's
  // floor and its end) plus the modulation and the tone filter's group
  // delay; a time crossfade and the shifter's own ring add once. Atomics and
  // arithmetic only.
  double tailSeconds() const noexcept override {
    const double bpm = lastBpm.load(std::memory_order_relaxed);
    const double feedback = static_cast<double>(param(kFeedback)) * 0.01;
    const double excursion = static_cast<double>(param(kModDepth)) * 0.01 * kMaxModSeconds;
    const double filterDelay = lowpassDelaySeconds(static_cast<double>(param(kToneHz)), 2);
    const double pass = std::max(tapSeconds(kSync, bpm), tapSeconds(kTap2Sync, bpm));
    return feedbackTailSeconds(pass, feedback, excursion + filterDelay + windowSeconds(),
                               kFadeSeconds + 2.0 * windowSeconds() + 0.01);
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    onsetDelay = PitchShift::minOnsetDelaySamples(kWindow, sampleRate, kMaxAbsSemitones);
    const int lineSamples = static_cast<int>(std::ceil((kMaxDelaySeconds + 0.01) * sampleRate));
    for (auto& line : lines)
      line.prepare(kMaxChannels, lineSamples);
    for (auto& voice : voices)
      voice.prepare(sampleRate, pitchfx::kControlGrid, kWindow, onsetDelay);
    for (auto& per : scratch)
      for (auto& lane : per)
        lane.fill(0.0f);
    for (auto& tap : taps)
      tap.prepare(sampleRate, kFadeSeconds);
    for (auto& glide : glides)
      glide.prepare(sampleRate, kGlideSeconds);
    for (auto& level : levels)
      level.prepare(sampleRate, 0.03);
    for (auto& side : panGain)
      for (auto& gain : side)
        gain.prepare(sampleRate, 0.03);
    feedbackGain.prepare(sampleRate, 0.02);
    harmonyGain.prepare(sampleRate, 0.03);
    directShare.prepare(sampleRate, 0.02);
    for (auto& u : unison)
      u.prepare(sampleRate, 0.02);
    toneHz.prepare(sampleRate, 0.02);
    lowCutHz.prepare(sampleRate, 0.02);
    modSamples.prepare(sampleRate, 0.05);
    lfo.prepare(sampleRate);
    for (auto& duck : ducker)
      duck.prepare(sampleRate);
    clearState();
  }

  void resetEffect() noexcept override {
    for (auto& voice : voices)
      voice.markCold();
    clearState();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxChannels);
    if (numChannels < 1 || numSamples < 1)
      return;
    const double bpm = ctx.transport.bpm;
    lastBpm.store(bpm, std::memory_order_relaxed);

    // In a stereo image the lines carry the sum of the two sides and the
    // taps are panned; otherwise every channel is its own lane. A change of
    // layout empties the loops (it comes with a change of the chain).
    const bool image = blockHasStereoImage(buffer.getNumChannels(), ctx);
    const int lanes = image ? 1 : numChannels;
    if (image != lastImage) {
      if (lastImage.has_value())
        restartLoops();
      lastImage = image;
    }
    const bool dual = !image && ctx.dualMono && numChannels == 2;

    // Whole samples, so an unmodulated direct read is the stored sample.
    taps[kTap1].setTarget(std::round(static_cast<float>(tapSeconds(kSync, bpm) * sampleRate)));
    taps[kTap2].setTarget(std::round(static_cast<float>(tapSeconds(kTap2Sync, bpm) * sampleRate)));
    const float interval1 = param(kPitch) + param(kDetune1) * 0.01f;
    const float interval2 = param(kPitch2) + param(kDetune2) * 0.01f;
    glides[kGlide1].setTarget(interval1);
    glides[kGlide2].setTarget(interval2);
    const int harmonyChoice = juce::jlimit(0, 2, static_cast<int>(std::lround(param(kHarmony))));
    glides[kGlideHarmony].setTarget(harmonyChoice == 2 ? 7.0f : -12.0f);
    harmonyGain.setTarget(harmonyChoice == 0 ? 0.0f : param(kHarmonyLevel) * 0.01f);
    unison[kTap1].setTarget(std::abs(interval1) < 1.0e-4f ? 1.0f : 0.0f);
    unison[kTap2].setTarget(std::abs(interval2) < 1.0e-4f ? 1.0f : 0.0f);
    feedbackGain.setTarget(param(kFeedback) * 0.01f);
    directShare.setTarget(param(kDirect) * 0.01f);
    toneHz.setTarget(param(kToneHz));
    lowCutHz.setTarget(param(kLowCutHz));
    modSamples.setTarget(param(kModDepth) * 0.01f * static_cast<float>(kMaxModSeconds * sampleRate));
    lfo.setRateHz(kModRateHz);
    const bool centred = std::lround(param(kPanMode)) == kPanCentre;
    for (int k = 0; k < kNumTaps; ++k) {
      const float db = param(k == kTap1 ? kTap1Db : kTap2Db);
      levels[k].setTarget(db <= kOffDb + 0.01f ? 0.0f : fx::dbToGain(db));
      // 0 degrees is hard left, 90 hard right.
      const float pan = centred ? 0.0f : param(k == kTap1 ? kTap1Pan : kTap2Pan);
      const float angle = (pan + 100.0f) * 0.005f * juce::MathConstants<float>::halfPi;
      panGain[k][0].setTarget(std::cos(angle));
      panGain[k][1].setTarget(std::sin(angle));
    }
    for (auto& duck : ducker)
      duck.setTarget(param(kDuckDb), param(kDuckReleaseMs));

    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    grid.run(numSamples, [&](int start, int length, bool boundary) {
      if (boundary)
        controlTick();
      runSegment(channels, numChannels, lanes, image, dual, start, length, boundary);
    });
  }

private:
  // 40 ms: holds a full period of a low E an octave down, splices half as
  // often as the 20 ms guitar window, and its landing latency is short.
  static constexpr PitchShift::Window kWindow = PitchShift::Window::ms40;
  // An octave and the widest detune either way.
  static constexpr double kMaxAbsSemitones = 12.5;
  static constexpr double kFadeSeconds = 0.04;
  static constexpr double kGlideSeconds = 0.03;
  // The shifters' band: the top at kFizzHz / ratio, the bottom at
  // kRumbleHz / ratio (see the file comment).
  static constexpr double kFizzHz = 12000.0;
  static constexpr double kRumbleHz = 30.0;
  enum Tap { kTap1 = 0, kTap2, kNumTaps };
  enum Voice { kVoice1 = 0, kVoiceHarmony, kVoice2, kNumVoices };
  enum GlideId { kGlide1 = 0, kGlideHarmony, kGlide2, kNumGlides };
  enum Scratch { kShifted1 = 0, kHarmonyLane, kShifted2, kDirect1, kDirect2, kNumScratch };

  static double windowSeconds() noexcept { return PitchShift::windowMs(kWindow) * 0.001; }

  // The time of the tap whose three time parameters start at `syncIndex`.
  double tapSeconds(int syncIndex, double bpm) const noexcept {
    BlockTransport transport;
    transport.bpm = bpm;
    return juce::jmin(kMaxDelaySeconds,
                      fx::resolveTimeSeconds(param(syncIndex), param(syncIndex + 1),
                                             param(syncIndex + 2), transport));
  }

  void clearFilters() noexcept {
    for (int t = 0; t < kNumTaps; ++t)
      for (int ch = 0; ch < kMaxChannels; ++ch) {
        lowCutFilter[t][ch].reset();
        toneFilter[t][ch][0].reset();
        toneFilter[t][ch][1].reset();
        bandLow[t][ch].reset();
        bandHigh[t][ch].reset();
      }
  }

  // The loops start empty and the shifters from rest.
  void restartLoops() noexcept {
    for (auto& line : lines)
      line.clear();
    for (auto& voice : voices)
      voice.markCold();
    clearFilters();
  }

  void clearState() noexcept {
    for (auto& line : lines)
      line.clear();
    clearFilters();
    for (auto& tap : taps)
      tap.snapOnNextTarget();
    for (auto& glide : glides)
      glide.snap();
    for (auto& level : levels)
      level.snapOnNextTarget();
    for (auto& side : panGain)
      for (auto& gain : side)
        gain.snapOnNextTarget();
    feedbackGain.snapOnNextTarget();
    harmonyGain.snapOnNextTarget();
    directShare.snapOnNextTarget();
    for (auto& u : unison)
      u.snapOnNextTarget();
    toneHz.snapOnNextTarget();
    lowCutHz.snapOnNextTarget();
    modSamples.snapOnNextTarget();
    lfo.reset();
    grid.reset();
    for (auto& r : running)
      r = false;
    for (auto& ratio : bandRatioHigh)
      ratio = -1.0f;
    for (auto& ratio : bandRatioLow)
      ratio = -1.0f;
    for (auto& duck : ducker)
      duck.reset();
  }

  static bool heard(const fx::SmoothedParam& gain) noexcept {
    return gain.target() > 0.0f || gain.current() > 0.0f;
  }

  // The band of tap `t`'s shifters: from 30 Hz / lowest ratio to 12 kHz /
  // highest, counting unison.
  void setBand(int t, float highest, float lowest) noexcept {
    if (juce::exactlyEqual(highest, bandRatioHigh[t]) && juce::exactlyEqual(lowest, bandRatioLow[t]))
      return;
    bandRatioHigh[t] = highest;
    bandRatioLow[t] = lowest;
    const double top = std::min(kFizzHz / static_cast<double>(highest), 0.45 * sampleRate);
    const auto lowpass = fx::BiquadCoeffs::lowpass(top, 0.7071, sampleRate);
    const auto highpass =
        fx::BiquadCoeffs::highpass(kRumbleHz / static_cast<double>(lowest), 0.7071, sampleRate);
    for (int ch = 0; ch < kMaxChannels; ++ch) {
      bandHigh[t][ch].setCoeffs(lowpass);
      bandLow[t][ch].setCoeffs(highpass);
    }
  }

  // On every 16-sample grid line: the glides step, the voices that are heard
  // (or about to be) run, and the bands follow the intervals in use. A voice
  // whose level has faded to nothing stops, and starts from rest when it is
  // next needed.
  void controlTick() noexcept {
    running[kVoice1] = heard(levels[kTap1]);
    running[kVoiceHarmony] = running[kVoice1] && heard(harmonyGain);
    running[kVoice2] = heard(levels[kTap2]);
    for (auto& glide : glides)
      glide.tick();
    const float ratio1 = std::exp2(glides[kGlide1].value() / 12.0f);
    const float ratioH =
        running[kVoiceHarmony] ? std::exp2(glides[kGlideHarmony].value() / 12.0f) : 1.0f;
    const float ratio2 = std::exp2(glides[kGlide2].value() / 12.0f);
    setBand(kTap1, std::max({1.0f, ratio1, ratioH}), std::min({1.0f, ratio1, ratioH}));
    setBand(kTap2, std::max(1.0f, ratio2), std::min(1.0f, ratio2));
  }

  void runSegment(float* const* channels, int numChannels, int lanes, bool image, bool dual, int start,
                  int length, bool boundary) {
    // 1. Read the whole segment from the lines before writing any of it. The
    //    write head moves one sample per sample, so sample j of the segment
    //    reads j samples nearer than it would once j more were written; the
    //    shortest read (time minus the shifter's latency) is far longer than
    //    a segment, so all of it is already in the line.
    const float latency = static_cast<float>(onsetDelay);
    for (int j = 0; j < length; ++j) {
      const float depth = modSamples.next();
      float swing[kNumTaps] = {0.0f, 0.0f};
      if (depth > 0.0f) {
        swing[kTap1] = depth * lfo.valueAt(0.0f);
        swing[kTap2] = depth * lfo.valueAt(0.25f);
      }
      lfo.advance();
      const float back = static_cast<float>(j);
      const size_t k = static_cast<size_t>(j);
      for (int t = 0; t < kNumTaps; ++t) {
        const CrossfadeTap::Taps at = taps[t].next();
        const fx::DelayLine& line = lines[t];
        for (int lane = 0; lane < lanes; ++lane) {
          const float from = at.from + swing[t] - back;
          float direct = line.readHermite(lane, from);
          float early = line.readHermite(lane, from - latency);
          if (at.toWeight > 0.0f) {
            const float to = at.to + swing[t] - back;
            direct += at.toWeight * (line.readHermite(lane, to) - direct);
            early += at.toWeight * (line.readHermite(lane, to - latency) - early);
          }
          const float banded = bandHigh[t][lane].process(bandLow[t][lane].process(early));
          if (t == kTap1) {
            scratch[kDirect1][static_cast<size_t>(lane)][k] = direct;
            scratch[kShifted1][static_cast<size_t>(lane)][k] = banded;
            scratch[kHarmonyLane][static_cast<size_t>(lane)][k] = banded;
          } else {
            scratch[kDirect2][static_cast<size_t>(lane)][k] = direct;
            scratch[kShifted2][static_cast<size_t>(lane)][k] = banded;
          }
        }
      }
    }

    // 2. The shifters, in place on their copies.
    const auto shift = [&](int voice, int lane, int glide) {
      if (!running[voice]) {
        voices[voice].markCold();
        return;
      }
      const float semitones[pitchfx::kMaxLanes] = {glides[glide].value(), glides[glide].value()};
      float* io[kMaxChannels] = {scratch[lane][0].data(), scratch[lane][1].data()};
      voices[voice].process(io, lanes, dual, 0, length, semitones, kWindow, onsetDelay);
    };
    shift(kVoice1, kShifted1, kGlide1);
    shift(kVoiceHarmony, kHarmonyLane, kGlideHarmony);
    shift(kVoice2, kShifted2, kGlide2);

    // 3. Mix each pass, feed it back, and play it.
    // How alike tap 1's two voices are: 1 at the same interval, 0 a semitone
    // or more apart. Constant over a segment (the glides step on the grid).
    const float alike = juce::jlimit(
        0.0f, 1.0f, 1.0f - std::abs(glides[kGlide1].value() - glides[kGlideHarmony].value()));
    const bool on1 = running[kVoice1];
    const bool onH = running[kVoiceHarmony];
    const bool on2 = running[kVoice2];
    for (int j = 0; j < length; ++j) {
      const int i = start + j;
      const size_t k = static_cast<size_t>(j);
      const float feedback = feedbackGain.next();
      const float harmony = harmonyGain.next();
      const float share = directShare.next();
      const float toDirect1 = unison[kTap1].next();
      const float toDirect2 = unison[kTap2].next();
      const float tone = toneHz.next();
      const float cut = lowCutHz.next();
      const float level1 = levels[kTap1].next();
      const float level2 = levels[kTap2].next();
      const float left1 = level1 * panGain[kTap1][0].next();
      const float right1 = level1 * panGain[kTap1][1].next();
      const float left2 = level2 * panGain[kTap2][0].next();
      const float right2 = level2 * panGain[kTap2][1].next();
      if (j == 0 && boundary) {
        for (int t = 0; t < kNumTaps; ++t)
          for (int lane = 0; lane < lanes; ++lane) {
            toneFilter[t][lane][0].setCutoff(tone, sampleRate);
            toneFilter[t][lane][1].setCutoff(tone, sampleRate);
            lowCutFilter[t][lane].setCutoff(cut, sampleRate);
          }
      }
      const float norm = 1.0f / std::sqrt(1.0f + harmony * harmony + 2.0f * alike * harmony);

      float input[kMaxChannels] = {channels[0][i], numChannels > 1 ? channels[1][i] : 0.0f};
      float repeat[kNumTaps][kMaxChannels] = {{0.0f, 0.0f}, {0.0f, 0.0f}};
      for (int lane = 0; lane < lanes; ++lane) {
        if (on1) {
          const float direct = scratch[kDirect1][static_cast<size_t>(lane)][k];
          float main = scratch[kShifted1][static_cast<size_t>(lane)][k];
          main += toDirect1 * (direct - main);
          float voicesOut = main;
          if (onH)
            voicesOut = (main + harmony * scratch[kHarmonyLane][static_cast<size_t>(lane)][k]) * norm;
          // The ceiling on the pass as well as on the line: the band filters
          // and two voices at a peak together can overshoot what the line
          // holds, and the repeat is both what is heard and what goes round.
          repeat[kTap1][lane] = softLimit(voicesOut + share * (direct - voicesOut));
        }
        if (on2) {
          const float direct = scratch[kDirect2][static_cast<size_t>(lane)][k];
          const float main = scratch[kShifted2][static_cast<size_t>(lane)][k];
          repeat[kTap2][lane] = softLimit(main + toDirect2 * (direct - main));
        }
        const float in = image ? 0.5f * (input[0] + input[1]) : input[lane];
        for (int t = 0; t < kNumTaps; ++t) {
          const float x = in + feedback * repeat[t][lane];
          const float shaped =
              lowCutFilter[t][lane].highpass(toneFilter[t][lane][1].lowpass(toneFilter[t][lane][0].lowpass(x)));
          lines[t].write(lane, softLimit(shaped));
        }
      }

      if (image) {
        const float outs[2] = {left1 * repeat[kTap1][0] + left2 * repeat[kTap2][0],
                               right1 * repeat[kTap1][0] + right2 * repeat[kTap2][0]};
        for (int ch = 0; ch < 2; ++ch)
          channels[ch][i] = outs[ch] * ducker[ch].next(input[ch], input[ch]);
      } else {
        const float fold1 = 0.5f * (left1 + right1);
        const float fold2 = 0.5f * (left2 + right2);
        for (int ch = 0; ch < numChannels; ++ch) {
          const float out = fold1 * repeat[kTap1][ch] + fold2 * repeat[kTap2][ch];
          channels[ch][i] = out * ducker[ch].next(input[ch], input[ch]);
        }
      }
    }
  }

  fx::DelayLine lines[kNumTaps];
  pitchfx::PitchVoice voices[kNumVoices];
  std::array<std::array<float, pitchfx::kControlGrid>, kMaxChannels> scratch[kNumScratch]{};
  pitchfx::Glide glides[kNumGlides];
  pitchfx::ControlGrid grid;
  CrossfadeTap taps[kNumTaps];
  fx::Biquad bandLow[kNumTaps][kMaxChannels];
  fx::Biquad bandHigh[kNumTaps][kMaxChannels];
  fx::OnePole lowCutFilter[kNumTaps][kMaxChannels];
  fx::OnePole toneFilter[kNumTaps][kMaxChannels][2];
  fx::SmoothedParam levels[kNumTaps];
  fx::SmoothedParam panGain[kNumTaps][2];
  fx::SmoothedParam unison[kNumTaps];
  fx::SmoothedParam feedbackGain, harmonyGain, directShare, toneHz, lowCutHz, modSamples;
  fx::Lfo lfo;
  Ducker ducker[kMaxChannels];
  double sampleRate{48000.0};
  int onsetDelay{0};
  bool running[kNumVoices] = {false, false, false};
  float bandRatioHigh[kNumTaps] = {-1.0f, -1.0f};
  float bandRatioLow[kNumTaps] = {-1.0f, -1.0f};
  std::optional<bool> lastImage;
  std::atomic<double> lastBpm{120.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(CrystalDelay) {
  // Heard beside the dry signal: the dry stays at unity up to 50 % mix, and a
  // new block brings the repeats in at 35 % under it.
  addEffectKind<CrystalDelayProcessor>(registry, "Crystal Delay", BlockCategory::Delay, 0.35f,
                                       BlockMixLaw::DryPreserving);
}

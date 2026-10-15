// delay.crystal: a delay whose repeats are pitch shifted inside the feedback
// loop, so every trip round it moves the repeat a further interval: at +12 the
// echoes climb an octave at a time into a shimmer, at -12 they fall away.
//
// The shifter is the pitch family's engine (PitchShift, through PitchVoice):
// a correlation-spliced delay line whose joints land a whole number of
// periods apart, the cleanest engine the repo has. The reverb's grain
// shifter (FdnTank::readShifted) is passive by construction but splices
// blindly on a fixed raised-cosine grain; a loop applies a shifter dozens of
// times to the same note, and blind grains turn into an audible flutter
// after a few passes where matched splices stay smooth. The engine works in
// a loop because it is per-sample and its attack latency is a constant
// (constant-latency mode): the loop reads its input that many samples
// early, so the shifted repeat lands on the set time, and the loop runs in
// 16-sample segments, far shorter than the shortest time, so every sample a
// segment reads was written before it.
//
// One pass round the loop:
//
//   line --read at T - L--> band limit --> voice 1 (pitch) ----+
//                                     \--> voice 2 (off, -12, +7) --+--> voices
//   line --read at T-----------------------------------------> direct
//   repeat = softLimit(voices * (1 - direct share) + direct * direct share)
//   line <- softLimit(input + feedback * lowCut(tone(repeat)))
//
// Keeping each pass clean and the loop bounded:
//  - the band limit before the shifters follows the interval: the top is
//    held to 12 kHz / ratio (an upward loop cannot fold fizz into the top
//    octave, and what climbs out of the band is gone after a pass or two),
//    the bottom to 30 Hz / ratio (a falling loop cannot pile rumble or DC);
//  - the two voices are summed at a level normalised for their correlation
//    (equal power when their intervals differ, equal amplitude when they
//    coincide), and the direct share is a crossfade, so the loop gain is the
//    feedback at most;
//  - the soft limit of the family (DelayCommon.h) is the ceiling for a hot
//    input at 95 %;
//  - at exactly 0 semitones voice 1 crossfades to the plain read, so a
//    unison loop is an ordinary clean delay.
//
// The dry path is untouched (no latency); everything heard is the wet.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

#include "../pitch/detail/PitchVoice.h"
#include "DelayCommon.h"
#include "effects/EffectDsp.h"

namespace {

using namespace delayfx;

class CrystalDelayProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "delay.crystal";

  enum Param {
    kSync = 0,
    kDivision,
    kTimeMs,
    kFeedback,
    kPitch,
    kVoice2,
    kVoice2Level,
    kDirect,
    kToneHz,
    kLowCutHz,
    kModDepth,
    kDuckDb,
    kDuckReleaseMs,
    kSpillover
  };

  static constexpr double kMaxDelaySeconds = 4.0;
  // The read swings by +-this at a modulation depth of 100 %: a few cents of
  // drift that builds up gently over the repeats.
  static constexpr double kMaxModSeconds = 0.001;
  static constexpr double kModRateHz = 0.45;

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      fx::TimeParamOptions time;
      time.minMs = 50.0f;
      time.maxMs = 2000.0f;
      time.defaultMs = 400.0f;
      time.affectsTail = true;
      fx::appendTimeParams(t, time);
      t.push_back(BlockParamSpec::floatParam("feedback", "Feedback", "%", 0.0f, 95.0f, 50.0f)
                      .withAffectsTail());
      t.push_back(
          BlockParamSpec::floatParam("pitch", "Pitch", "st", -12.0f, 12.0f, 12.0f).withStep(1.0f));
      t.push_back(BlockParamSpec::choiceParam("voice2", "Voice 2",
                                              juce::StringArray{"Off", "Octave Down", "Fifth"}, 0));
      t.push_back(BlockParamSpec::floatParam("voice2Level", "Voice 2 Level", "%", 0.0f, 100.0f, 50.0f));
      t.push_back(BlockParamSpec::floatParam("direct", "Direct", "%", 0.0f, 100.0f, 0.0f));
      t.push_back(BlockParamSpec::floatParam("toneHz", "Tone", "Hz", 1000.0f, 20000.0f, 8000.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("lowCutHz", "Low Cut", "Hz", 20.0f, 2000.0f, 100.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("modDepth", "Mod Depth", "%", 0.0f, 100.0f, 20.0f)
                      .withAffectsTail());
      appendDuckParams(t);
      t.push_back(spilloverParam());
      return t;
    }();
    return table;
  }

  CrystalDelayProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // Until the repeats are 60 dB down. The voice mix, the filters and the
  // limit only lower the loop gain, so the feedback sets the count. Each pass
  // can run late by the shifter's wander (a sustained note is read anywhere
  // between the window's floor and its end) plus the modulation and the tone
  // filter's group delay; a time crossfade and the shifter's own ring add
  // once. Atomics and arithmetic only.
  double tailSeconds() const noexcept override {
    const double feedback = static_cast<double>(param(kFeedback)) * 0.01;
    const double excursion = static_cast<double>(param(kModDepth)) * 0.01 * kMaxModSeconds;
    const double filterDelay = lowpassDelaySeconds(static_cast<double>(param(kToneHz)), 2);
    return feedbackTailSeconds(delaySeconds(lastBpm.load(std::memory_order_relaxed)), feedback,
                               excursion + filterDelay + windowSeconds(),
                               kFadeSeconds + 2.0 * windowSeconds() + 0.01);
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    onsetDelay = PitchShift::minOnsetDelaySamples(kWindow, sampleRate, kMaxAbsSemitones);
    line.prepare(kMaxChannels, static_cast<int>(std::ceil((kMaxDelaySeconds + 0.01) * sampleRate)));
    for (auto& voice : voices)
      voice.prepare(sampleRate, pitchfx::kControlGrid, kWindow, onsetDelay);
    for (auto& per : scratch)
      for (auto& lane : per)
        lane.fill(0.0f);
    tap.prepare(sampleRate, kFadeSeconds);
    glide.prepare(sampleRate, kGlideSeconds);
    glide2.prepare(sampleRate, kGlideSeconds);
    feedbackGain.prepare(sampleRate, 0.02);
    voice2Gain.prepare(sampleRate, 0.03);
    directShare.prepare(sampleRate, 0.02);
    unison.prepare(sampleRate, 0.02);
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
    lastBpm.store(ctx.transport.bpm, std::memory_order_relaxed);
    const bool dual = ctx.dualMono && numChannels == 2;

    // Whole samples, so an unmodulated direct read is the stored sample.
    const float delayTarget =
        std::round(static_cast<float>(delaySeconds(ctx.transport.bpm) * sampleRate));
    tap.setTarget(delayTarget);
    const float pitch = param(kPitch);
    glide.setTarget(pitch);
    const int voice2Choice = juce::jlimit(0, 2, static_cast<int>(std::lround(param(kVoice2))));
    glide2.setTarget(voice2Choice == 2 ? 7.0f : -12.0f);
    voice2Gain.setTarget(voice2Choice == 0 ? 0.0f : param(kVoice2Level) * 0.01f);
    feedbackGain.setTarget(param(kFeedback) * 0.01f);
    directShare.setTarget(param(kDirect) * 0.01f);
    unison.setTarget(std::abs(pitch) < 1.0e-4f ? 1.0f : 0.0f);
    toneHz.setTarget(param(kToneHz));
    lowCutHz.setTarget(param(kLowCutHz));
    modSamples.setTarget(param(kModDepth) * 0.01f * static_cast<float>(kMaxModSeconds * sampleRate));
    lfo.setRateHz(kModRateHz);
    for (auto& duck : ducker)
      duck.setTarget(param(kDuckDb), param(kDuckReleaseMs));
    const float rightPhase = blockHasStereoImage(buffer.getNumChannels(), ctx) ? 0.25f : 0.0f;

    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    grid.run(numSamples, [&](int start, int length, bool boundary) {
      if (boundary)
        controlTick();
      runSegment(channels, numChannels, dual, start, length, boundary, rightPhase);
    });
  }

private:
  // 40 ms: holds a full period of a low E an octave down, splices half as
  // often as the 20 ms guitar window, and its landing latency is short.
  static constexpr PitchShift::Window kWindow = PitchShift::Window::ms40;
  static constexpr double kMaxAbsSemitones = 12.0;
  static constexpr double kFadeSeconds = 0.04;
  static constexpr double kGlideSeconds = 0.03;
  // The shifters' band: the top at kFizzHz / ratio, the bottom at
  // kRumbleHz / ratio (see the file comment).
  static constexpr double kFizzHz = 12000.0;
  static constexpr double kRumbleHz = 30.0;
  enum Voice { kVoiceMain = 0, kVoiceSecond, kNumVoices };
  enum Scratch { kShifted = 0, kSecond, kDirectRead, kNumScratch };

  static double windowSeconds() noexcept { return PitchShift::windowMs(kWindow) * 0.001; }

  double delaySeconds(double bpm) const noexcept {
    BlockTransport transport;
    transport.bpm = bpm;
    return juce::jmin(kMaxDelaySeconds, fx::resolveTimeSeconds(param(kSync), param(kDivision),
                                                               param(kTimeMs), transport));
  }

  void clearState() noexcept {
    line.clear();
    for (int ch = 0; ch < kMaxChannels; ++ch) {
      lowCutFilter[ch].reset();
      toneFilter[ch][0].reset();
      toneFilter[ch][1].reset();
      bandLow[ch].reset();
      bandHigh[ch].reset();
    }
    tap.snapOnNextTarget();
    glide.snap();
    glide2.snap();
    feedbackGain.snapOnNextTarget();
    voice2Gain.snapOnNextTarget();
    directShare.snapOnNextTarget();
    unison.snapOnNextTarget();
    toneHz.snapOnNextTarget();
    lowCutHz.snapOnNextTarget();
    modSamples.snapOnNextTarget();
    lfo.reset();
    grid.reset();
    voice2Running = false;
    bandRatioHigh = bandRatioLow = -1.0f;
    for (auto& duck : ducker)
      duck.reset();
  }

  // On every 16-sample grid line: the glides step and the band follows the
  // intervals in use.
  void controlTick() noexcept {
    // Voice 2 runs while it is heard or about to be; once its level has
    // faded to nothing its engine stops and starts from rest when next needed.
    voice2Running = voice2Gain.target() > 0.0f || voice2Gain.current() > 0.0f;
    glide.tick();
    glide2.tick();
    const float ratio1 = std::exp2(glide.value() / 12.0f);
    const float ratio2 = voice2Running ? std::exp2(glide2.value() / 12.0f) : 1.0f;
    const float highest = std::max({1.0f, ratio1, ratio2});
    const float lowest = std::min({1.0f, ratio1, ratio2});
    if (!juce::exactlyEqual(highest, bandRatioHigh) || !juce::exactlyEqual(lowest, bandRatioLow)) {
      bandRatioHigh = highest;
      bandRatioLow = lowest;
      const double top = std::min(kFizzHz / static_cast<double>(highest), 0.45 * sampleRate);
      const auto lowpass = fx::BiquadCoeffs::lowpass(top, 0.7071, sampleRate);
      const auto highpass =
          fx::BiquadCoeffs::highpass(kRumbleHz / static_cast<double>(lowest), 0.7071, sampleRate);
      for (int ch = 0; ch < kMaxChannels; ++ch) {
        bandHigh[ch].setCoeffs(lowpass);
        bandLow[ch].setCoeffs(highpass);
      }
    }
  }

  void runSegment(float* const* channels, int numChannels, bool dual, int start, int length,
                  bool boundary, float rightPhase) {
    // 1. Read the whole segment from the line before writing any of it. The
    //    write head moves one sample per sample, so sample j of the segment
    //    reads j samples nearer than it would once j more were written; the
    //    shortest read (time minus the shifter's latency) is far longer than
    //    a segment, so all of it is already in the line.
    const float latency = static_cast<float>(onsetDelay);
    for (int j = 0; j < length; ++j) {
      const CrossfadeTap::Taps t = tap.next();
      const float depth = modSamples.next();
      float swing[kMaxChannels] = {0.0f, 0.0f};
      if (depth != 0.0f) {
        swing[0] = depth * lfo.valueAt(0.0f);
        swing[1] = depth * lfo.valueAt(rightPhase);
      }
      lfo.advance();
      const float back = static_cast<float>(j);
      for (int ch = 0; ch < numChannels; ++ch) {
        const float from = t.from + swing[ch] - back;
        float direct = line.readHermite(ch, from);
        float early = line.readHermite(ch, from - latency);
        if (t.toWeight > 0.0f) {
          const float to = t.to + swing[ch] - back;
          direct += t.toWeight * (line.readHermite(ch, to) - direct);
          early += t.toWeight * (line.readHermite(ch, to - latency) - early);
        }
        const float banded = bandHigh[ch].process(bandLow[ch].process(early));
        scratch[kDirectRead][ch][static_cast<size_t>(j)] = direct;
        scratch[kShifted][ch][static_cast<size_t>(j)] = banded;
        scratch[kSecond][ch][static_cast<size_t>(j)] = banded;
      }
    }

    // 2. The shifters, in place on their copies.
    const float lane1[pitchfx::kMaxLanes] = {glide.value(), glide.value()};
    float* shifted[kMaxChannels] = {scratch[kShifted][0].data(), scratch[kShifted][1].data()};
    voices[kVoiceMain].process(shifted, numChannels, dual, 0, length, lane1, kWindow, onsetDelay);
    if (voice2Running) {
      const float lane2[pitchfx::kMaxLanes] = {glide2.value(), glide2.value()};
      float* second[kMaxChannels] = {scratch[kSecond][0].data(), scratch[kSecond][1].data()};
      voices[kVoiceSecond].process(second, numChannels, dual, 0, length, lane2, kWindow, onsetDelay);
    } else {
      voices[kVoiceSecond].markCold();
    }

    // 3. Mix the pass, play it, and feed it back.
    // How alike the two voices are: 1 at the same interval, 0 a semitone or
    // more apart. Constant over a segment (the glides step on the grid).
    const float alike = juce::jlimit(0.0f, 1.0f, 1.0f - std::abs(glide.value() - glide2.value()));
    for (int j = 0; j < length; ++j) {
      const int i = start + j;
      const size_t k = static_cast<size_t>(j);
      const float feedback = feedbackGain.next();
      const float level2 = voice2Gain.next();
      const float share = directShare.next();
      const float toDirect = unison.next();
      const float tone = toneHz.next();
      const float cut = lowCutHz.next();
      if (j == 0 && boundary) {
        for (int ch = 0; ch < numChannels; ++ch) {
          toneFilter[ch][0].setCutoff(tone, sampleRate);
          toneFilter[ch][1].setCutoff(tone, sampleRate);
          lowCutFilter[ch].setCutoff(cut, sampleRate);
        }
      }
      const float norm = 1.0f / std::sqrt(1.0f + level2 * level2 + 2.0f * alike * level2);
      for (int ch = 0; ch < numChannels; ++ch) {
        const float direct = scratch[kDirectRead][ch][k];
        float main = scratch[kShifted][ch][k];
        main += toDirect * (direct - main);
        float voicesOut = main;
        if (level2 > 0.0f)
          voicesOut = (main + level2 * scratch[kSecond][ch][k]) * norm;
        // The ceiling on the pass as well as on the line: the band filters
        // and two voices at a peak together can overshoot what the line
        // holds, and the repeat is both what is heard and what goes round.
        const float repeat = softLimit(voicesOut + share * (direct - voicesOut));

        const float filtered =
            lowCutFilter[ch].highpass(toneFilter[ch][1].lowpass(toneFilter[ch][0].lowpass(repeat)));
        const float input = channels[ch][i];
        line.write(ch, softLimit(input + feedback * filtered));
        channels[ch][i] = repeat * ducker[ch].next(input, input);
      }
    }
  }

  fx::DelayLine line;
  pitchfx::PitchVoice voices[kNumVoices];
  std::array<std::array<float, pitchfx::kControlGrid>, kMaxChannels> scratch[kNumScratch]{};
  pitchfx::Glide glide, glide2;
  pitchfx::ControlGrid grid;
  CrossfadeTap tap;
  fx::Biquad bandLow[kMaxChannels];
  fx::Biquad bandHigh[kMaxChannels];
  fx::OnePole lowCutFilter[kMaxChannels];
  fx::OnePole toneFilter[kMaxChannels][2];
  fx::SmoothedParam feedbackGain, voice2Gain, directShare, unison, toneHz, lowCutHz, modSamples;
  fx::Lfo lfo;
  Ducker ducker[kMaxChannels];
  double sampleRate{48000.0};
  int onsetDelay{0};
  bool voice2Running{false};
  float bandRatioHigh{-1.0f}, bandRatioLow{-1.0f};
  std::atomic<double> lastBpm{120.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(CrystalDelay) {
  // Heard beside the dry signal: the dry stays at unity up to 50 % mix, and a
  // new block brings the repeats in at 35 % under it.
  addEffectKind<CrystalDelayProcessor>(registry, "Crystal Delay", BlockCategory::Delay, 0.35f,
                                       BlockMixLaw::DryPreserving);
}

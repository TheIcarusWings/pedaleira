// pitch.shifter (plugin/src/effects/pitch/PitchShifterBlock.cpp): the
// conformance test every effect runs, then what is specific to a shifter:
// the interval it produces, in cents, at several intervals and fine
// settings; the latency it reports against what is measured; the dry path
// at a mix below 100 %; a clean start; and the output staying bounded and
// free of steps when the interval moves under audio.
#include "PitchShift.h"
#include "effects/pitch_test_signals.h"

#include <gtest/gtest.h>

using namespace pitchtest;

namespace {

constexpr const char* kKind = "pitch.shifter";

// The window choices and what the engine says their latency is.
int engineLatency(int windowIndex) {
  return PitchShift::latencySamples(PitchShift::windowFromIndex(windowIndex), kRate);
}

const BlockParamSpec& specOf(const BlockKindInfo& info, const char* id) {
  for (const auto& spec : info.params)
    if (spec.id == id)
      return spec;
  ADD_FAILURE() << "no parameter " << id;
  return info.params.front();
}

}  // namespace

TEST(PitchShifterTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"semitones", -5.0}, {"cents", 10.0}};
  options.stateParams = {{"semitones", 7.0}, {"cents", -23.0}, {"window", 2.0}};
  // The conformance latency check plays an impulse and expects the wet path to
  // start exactly at the reported latency (MatchesOnset), or no earlier
  // (AtMostOnset). Neither fits a shifter whose tap is not at a constant
  // delay: an impulse after a start meets the tap at the floor and arrives
  // after 2 ms / ratio (96 samples at unison, 384 at -24 st, 24 at +24 st),
  // always before the mean the block reports, which is a long-run average.
  // So the generic check is skipped, and the latency is tested below in
  // three ways: exactly at unison (a pure delay), as the engine's mean at
  // other intervals, and as the bound on how late an attack can be.
  options.latency = LatencyCheck::Skip;
  options.cpuAverageBudgetNs = 25000.0;
  options.cpuWorstBudgetNs = 250000.0;
  expectKindConforms("pitch.shifter", options);
}

TEST(PitchShifterTest, TableMatchesTheBrief) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kKind);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->name, "Pitch Shifter");
  EXPECT_EQ(info->category, BlockCategory::Pitch);
  // 100 % wet by default, the wet replacing the dry.
  EXPECT_FLOAT_EQ(info->defaultMix, 1.0f);
  EXPECT_EQ(info->mixLaw, BlockMixLaw::Crossfade);
  EXPECT_FALSE(info->spillover);

  const BlockParamSpec& interval = specOf(*info, "semitones");
  EXPECT_EQ(interval.unit, "st");
  EXPECT_FLOAT_EQ(interval.min, -24.0f);
  EXPECT_FLOAT_EQ(interval.max, 24.0f);
  EXPECT_FLOAT_EQ(interval.step, 1.0f);  // stepped
  EXPECT_FLOAT_EQ(interval.defaultValue, 0.0f);
  const BlockParamSpec& fine = specOf(*info, "cents");
  EXPECT_EQ(fine.unit, "ct");
  EXPECT_FLOAT_EQ(fine.min, -50.0f);
  EXPECT_FLOAT_EQ(fine.max, 50.0f);
  const BlockParamSpec& window = specOf(*info, "window");
  EXPECT_EQ(window.type, BlockParamType::Choice);
  EXPECT_EQ(window.choices.size(), 4);
  EXPECT_TRUE(window.affectsLatency);
  EXPECT_TRUE(window.affectsTail);
  EXPECT_FALSE(window.automatable);  // not click-free: the framework splices it in
  EXPECT_EQ(info->params.size(), 3u);
}

// ---------------------------------------------------------------------------
// The interval
// ---------------------------------------------------------------------------

TEST(PitchShifterTest, TheOutputIsTheInputTimesTwoToTheSemitonesOverTwelve) {
  struct Case {
    double semitones, cents, hz;
    bool harmonic;
  };
  // A sine and a guitar-like harmonic tone; low and high notes; the intervals
  // of the brief (+12, -12, +7, -5, +3), the extremes of the range, and fine
  // settings on their own and on top of an interval.
  const Case cases[] = {{12, 0, 196.0, true},  {-12, 0, 330.0, true},  {7, 0, 220.0, false},
                        {-5, 0, 196.0, true},  {3, 0, 330.0, true},    {24, 0, 110.0, true},
                        {-24, 0, 330.0, true}, {0, 50, 196.0, true},   {0, -50, 196.0, true},
                        {0, 17, 220.0, false}, {7, -30, 196.0, true},  {-12, 25, 330.0, true}};
  for (const Case& c : cases) {
    SCOPED_TRACE("semitones " + std::to_string(c.semitones) + ", cents " + std::to_string(c.cents) +
                 ", " + std::to_string(c.hz) + " Hz");
    EffectRig rig(kKind, {{"semitones", c.semitones}, {"cents", c.cents}, {"mix", 1.0}});
    const std::vector<float> in = c.harmonic ? harmonicTone(2.5, c.hz, 0.3f)
                                             : sine(2.5, c.hz, 0.3f, kRate);
    const std::vector<float> out = rig.render(in);
    const double total = c.semitones + c.cents * 0.01;
    const double target = shifted(c.hz, total);
    const size_t from = static_cast<size_t>(1.0 * kRate), to = out.size();

    // Within a few cents of the exact ratio ...
    EXPECT_NEAR(centsFrom(out, from, to, target, 60), 0.0, 4.0);
    // ... at the level of the input's own line (the shift is not a gain) ...
    EXPECT_NEAR(lineDb(out, from, to, target), lineDb(in, from, to, c.hz), 1.0);
    // ... and the original pitch is gone once the shift is large enough to
    // tell it from the fine settings' sidebands (unless the tone is a harmonic
    // one shifted by whole octaves, whose partials land on the old ones).
    if (std::abs(total) >= 3.0 && !(c.harmonic && std::fmod(std::abs(total), 12.0) < 1.0e-9))
      EXPECT_LT(lineDb(out, from, to, c.hz), lineDb(in, from, to, c.hz) - 35.0);
  }
}

TEST(PitchShifterTest, MonoStereoAndDualMonoAllShiftTheSame) {
  const std::vector<float> in = harmonicTone(2.0, 196.0, 0.3f);
  for (const Layout layout : {Layout::Mono, Layout::Stereo, Layout::DualMono}) {
    SCOPED_TRACE(layoutName(layout));
    EffectRig rig(kKind, {{"semitones", 7.0}, {"mix", 1.0}}, rigFor(layout));
    const Stereo out = rig.renderBoth(in);
    EXPECT_NEAR(centsFrom(out.left, 48000, out.left.size(), shifted(196.0, 7.0), 60), 0.0, 4.0);
    if (layout != Layout::Mono)
      EXPECT_NEAR(centsFrom(out.right, 48000, out.right.size(), shifted(196.0, 7.0), 60), 0.0, 4.0);
  }
}

TEST(PitchShifterTest, DualMonoShiftsEachInstrumentOnItsOwnWithoutLeaking) {
  // Two guitars through one rig: a low note on the left, a higher one on the
  // right (not a multiple of it, so no partial of one is a partial of the
  // other). Each comes out shifted, and nothing of one is heard in the other.
  EffectRig rig(kKind, {{"semitones", 7.0}, {"mix", 1.0}}, rigFor(Layout::DualMono));
  const Stereo out = rig.render(Stereo{harmonicTone(2.0, 110.0, 0.3f), harmonicTone(2.0, 270.0, 0.3f)});
  const size_t from = 48000, to = out.left.size();
  EXPECT_NEAR(centsFrom(out.left, from, to, shifted(110.0, 7.0), 60), 0.0, 4.0);
  EXPECT_NEAR(centsFrom(out.right, from, to, shifted(270.0, 7.0), 60), 0.0, 4.0);
  const double leftLine = lineDb(out.left, from, to, shifted(110.0, 7.0));
  const double rightLine = lineDb(out.right, from, to, shifted(270.0, 7.0));
  // The other instrument's shifted note, in each side.
  EXPECT_LT(lineDb(out.left, from, to, shifted(270.0, 7.0)), leftLine - 40.0);
  EXPECT_LT(lineDb(out.right, from, to, shifted(110.0, 7.0)), rightLine - 40.0);
}

TEST(PitchShifterTest, ATrueStereoImageIsKeptWhenTheSidesDiffer) {
  // Behind a stereo effect the sides differ. They share one engine's control
  // path, which must not smear the image: each side is its own signal,
  // shifted.
  DirectKind kind(kKind, {{"semitones", 7.0}}, Layout::Stereo);
  const Stereo out = kind.render(Stereo{harmonicTone(2.0, 110.0, 0.3f), harmonicTone(2.0, 270.0, 0.3f)});
  const size_t from = 48000, to = out.left.size();
  // The two sides share one control path (the splices are placed on their
  // mean), so each side is shifted by the right ratio on average but is spliced
  // at places that only suit the mix of them: a few cents looser than a mono
  // signal.
  EXPECT_NEAR(centsFrom(out.left, from, to, shifted(110.0, 7.0), 60), 0.0, 15.0);
  EXPECT_NEAR(centsFrom(out.right, from, to, shifted(270.0, 7.0), 60), 0.0, 15.0);
  EXPECT_LT(lineDb(out.left, from, to, shifted(270.0, 7.0)),
            lineDb(out.left, from, to, shifted(110.0, 7.0)) - 30.0);
  EXPECT_LT(lineDb(out.right, from, to, shifted(110.0, 7.0)),
            lineDb(out.right, from, to, shifted(270.0, 7.0)) - 30.0);

  // Identical sides stay identical, bit for bit.
  EffectRig rig(kKind, {{"semitones", -5.0}, {"mix", 1.0}});
  const Stereo same = rig.renderBoth(harmonicTone(1.0, 196.0, 0.3f));
  EXPECT_EQ(same.left, same.right);
}

// ---------------------------------------------------------------------------
// Latency
// ---------------------------------------------------------------------------

TEST(PitchShifterTest, ReportsTheEnginesLatencyForEachWindowAndLatchesIt) {
  EffectRig rig(kKind);
  EXPECT_EQ(rig.latencySamples(), engineLatency(1));  // the 30 ms default: 16 ms
  EXPECT_EQ(rig.latencySamples(), 768);
  rig.set("window", 3.0);
  // The wet path and the reported latency change in one instant: until the
  // framework commits the change, the old figure stands (and the wet path
  // still runs the old window).
  EXPECT_EQ(rig.latencySamples(), engineLatency(1));
  rig.settle();
  EXPECT_EQ(rig.latencySamples(), engineLatency(3));
  EXPECT_EQ(rig.latencySamples(), 1488);
  for (int w = 0; w < 4; ++w) {
    rig.set("window", w);
    rig.settle();
    EXPECT_EQ(rig.latencySamples(), engineLatency(w)) << "window " << w;
  }
}

TEST(PitchShifterTest, AtUnisonTheLatencyIsExactAndTheDryIsAligned) {
  // 0 st and 0 ct: the block is a pure delay by the reported latency, so the
  // reported figure can be checked to the sample, at every window, and a mix
  // below 100 % neither combs nor changes the level.
  for (int w = 0; w < 4; ++w) {
    SCOPED_TRACE("window " + std::to_string(w));
    EffectRig rig(kKind, {{"window", static_cast<double>(w)}, {"mix", 1.0}});
    EffectRig reference{juce::String()};
    const int latency = rig.latencySamples();
    ASSERT_EQ(latency, engineLatency(w));

    const std::vector<float> ir = captureImpulseResponse(rig, 0.2);
    const int origin = onsetIndex(captureImpulseResponse(reference, 0.05));
    reference.restart();  // its DC blocker still holds the impulse
    EXPECT_EQ(onsetIndex(ir) - origin, latency) << "the impulse arrives at the reported latency";
    EXPECT_EQ(peakIndexOf(ir) - origin, latency);

    // At 50 % the output is the same pure delay: the dry share the chain
    // delays and the wet path line up exactly.
    rig.set("mix", 0.5);
    rig.restart();
    const std::vector<float> in = noise(1.0, 31u, 0.25f, kRate);
    const std::vector<float> out = rig.render(in);
    const std::vector<float> dry = reference.render(in);
    double worst = 0.0;
    for (size_t i = static_cast<size_t>(latency); i < in.size(); ++i)
      worst = std::max(worst, static_cast<double>(std::abs(out[i] - dry[i - static_cast<size_t>(latency)])));
    EXPECT_LT(worst, 1.0e-6);
  }
}

TEST(PitchShifterTest, TheDryShareIsDelayedByTheReportedLatencyAtAnyInterval) {
  for (const double semitones : {-12.0, 7.0}) {
    EffectRig rig(kKind, {{"semitones", semitones}, {"mix", 0.0}});
    const std::vector<float> in = noise(0.5, 41u, 0.25f, kRate);
    const std::vector<float> out = rig.render(in);
    EXPECT_EQ(estimateDelaySamples(out, in, 0, 4000), rig.latencySamples());
  }
}

TEST(PitchShifterTest, AtHalfMixTheDryAndTheShiftedNoteAreBothThereAtHalfLevel) {
  EffectRig rig(kKind, {{"semitones", 12.0}, {"mix", 0.5}});
  const std::vector<float> in = sine(2.0, 196.0, 0.3f, kRate);
  const std::vector<float> out = rig.render(in);
  const size_t from = 48000, to = out.size();
  const double input = lineDb(in, from, to, 196.0);
  // Crossfade law: each half of the mix at -6 dB, one at the pitch played and
  // one an octave up.
  EXPECT_NEAR(lineDb(out, from, to, 196.0), input - 6.02, 0.7);
  EXPECT_NEAR(lineDb(out, from, to, 392.0), input - 6.02, 1.0);
}

TEST(PitchShifterTest, TheAverageTapDelayIsTheReportedLatencyWithinTheEnginesSpread) {
  // The latency a shifter really has is the delay of its tap, which wanders
  // between the floor and the window. Its time average is measured from the
  // loudness of a triangle-shaped swell (meanDelayFromEnvelopeSamples) and
  // compared with the report, which is the engine's own mean. The two agree
  // within the engine's spread: a down-shift runs about 1.8 times the report
  // (the tap keeps drifting while it fades to its next position, and the
  // fades are long), an up-shift 0.5 to 0.8. Measured with this engine at
  // the 30 ms window: -12 st 1.80, -5 st 1.81, +3 st 0.62, +7 st 0.78,
  // +12 st 0.49.
  const double seconds = 14.0;
  std::vector<float> in = harmonicTone(seconds, 196.0, 0.4f);
  applyTriangleEnvelope(in);
  double ratioSum = 0.0;
  int count = 0;
  struct Case {
    double window, semitones;
  };
  for (const Case& c : {Case{1, -12}, Case{1, -5}, Case{1, 3}, Case{1, 7}, Case{1, 12},
                        Case{3, -12}, Case{3, 12}, Case{0, 7}}) {
    EffectRig rig(kKind, {{"semitones", c.semitones}, {"window", c.window}, {"mix", 1.0}});
    const std::vector<float> out = rig.render(in);
    const double measured = meanDelayFromEnvelopeSamples(out, in, 4.0);
    const double reported = rig.latencySamples();
    const double ratio = measured / reported;
    std::cout << "[   MEAN   ] window " << c.window << ", " << c.semitones << " st: measured "
              << static_cast<int>(measured) << " samples, reported " << static_cast<int>(reported)
              << " (" << juce::String(ratio, 2) << "x)" << std::endl;
    EXPECT_GT(ratio, 0.4) << "window " << c.window << ", " << c.semitones << " st";
    EXPECT_LT(ratio, 2.2) << "window " << c.window << ", " << c.semitones << " st";
    if (c.window == 1) {
      ratioSum += ratio;
      ++count;
    }
  }
  // Across the intervals the report is the right order: the mean of the
  // ratios at the default window is near one.
  EXPECT_NEAR(ratioSum / count, 1.1, 0.35);
}

TEST(PitchShifterTest, AnAttackAfterASteadyNoteArrivesWithinTheWindow) {
  // The felt latency is the attacks', which the engine pulls back to its
  // floor when it can (plugin/docs/pitch-shift.md). A 20 ms noise burst over a
  // steady tone, at four moments: the burst's onset in the output is never
  // later than the window plus a margin, and in an up-shift (where fades are
  // short and the tap is near the head) within about 17 ms.
  for (const double semitones : {7.0, 12.0, -5.0, -12.0}) {
    SCOPED_TRACE(semitones);
    EffectRig rig(kKind, {{"semitones", semitones}, {"mix", 1.0}});
    std::vector<float> in = sine(6.0, 196.0, 0.1f, kRate);
    const std::vector<float> burst = noise(0.02, 3u, 0.5f, kRate);
    const size_t starts[] = {48000 * 2 + 777, 48000 * 3 + 12345, 48000 * 4 + 31, 48000 * 5 + 20000};
    for (const size_t s : starts)
      for (size_t i = 0; i < burst.size(); ++i)
        in[s + i] += burst[i];
    const std::vector<float> out = rig.render(in);
    for (const size_t s : starts) {
      int onset = -1;
      for (size_t i = s; i < s + 4000; ++i)
        if (std::abs(out[i]) > 0.2f) {
          onset = static_cast<int>(i - s);
          break;
        }
      ASSERT_GE(onset, 0) << "the attack never arrived";
      std::cout << "[  ATTACK  ] " << semitones << " st: " << onset << " samples ("
                << juce::String(onset / 48.0, 1) << " ms)" << std::endl;
      EXPECT_LE(onset, 1440 + 480) << "later than the window and a margin";
      if (semitones > 0.0)
        EXPECT_LE(onset, 800);
    }
  }
}

// ---------------------------------------------------------------------------
// A clean start, bounded output, steps
// ---------------------------------------------------------------------------

TEST(PitchShifterTest, AStartIsTheShiftedSignalAloneNotTheDryOneFadingOut) {
  // The engine, switched on, plays the dry signal until its tap has audio and
  // then blends the shift in over 25 ms. A block is wet only (the mix is the
  // chain's), so the block starts the engine from rest before it plays. After
  // a bypass and back, and after a prepare, the pitch played does not leak.
  const std::vector<float> in = harmonicTone(0.6, 220.0, 0.3f);
  const auto leak = [&](EffectRig& rig, size_t from, size_t to) {
    const std::vector<float> out = rig.render(in);
    return lineDb(out, from, to, 220.0) - lineDb(out, from, to, 440.0);
  };
  // Right after a prepare: from 40 ms on (a 25 ms fade the chain adds when
  // switching a block in is the only thing the first samples hide).
  EffectRig rig(kKind, {{"semitones", 12.0}, {"mix", 1.0}});
  EXPECT_LT(leak(rig, 1920, 28800), -40.0);

  // Bypassed for a while and back on.
  rig.render(harmonicTone(0.5, 220.0, 0.3f));
  rig.setEnabled(false);
  rig.render(harmonicTone(0.3, 220.0, 0.3f));
  rig.setEnabled(true);
  EXPECT_LT(leak(rig, 1920, 28800), -40.0);
}

TEST(PitchShifterTest, SilenceNoiseAndSweepsStayFiniteAndBounded) {
  for (const double semitones : {-24.0, -7.0, 5.0, 12.0, 24.0}) {
    SCOPED_TRACE(semitones);
    EffectRig rig(kKind, {{"semitones", semitones}, {"mix", 1.0}});
    // Silence in, silence out.
    EXPECT_EQ(peakOf(rig.renderSilence(24000)), 0.0f);
    // Noise: the engine's fades are power-normalised, so the output may
    // exceed the input's peak (0.5) a little, but not by much.
    const std::vector<float> noisy = rig.render(noise(2.0, 51u, 0.5f, kRate));
    expectCleanSignal(noisy, 0.8f, "noise");
    // A sweep through the guitar's range and well beyond, quiet and hot.
    for (const float level : {0.3f, 0.9f}) {
      rig.restart();
      const std::vector<float> swept = rig.render(logSweep(3.0, 40.0, 9000.0, level));
      expectCleanSignal(swept, level * 1.5f, "sweep");
      EXPECT_GT(peakOf(swept), level * 0.2f) << "the sweep went missing";
    }
    // A hard cut from a loud tone to silence and back.
    rig.restart();
    std::vector<float> bursts = sine(0.3, 150.0, 0.8f, kRate);
    bursts.resize(bursts.size() * 2, 0.0f);
    const std::vector<float> again = harmonicTone(0.3, 250.0, 0.8f);
    bursts.insert(bursts.end(), again.begin(), again.end());
    expectCleanSignal(rig.render(bursts), 1.2f, "bursts");
  }
}

namespace {

// Renders `tone` through a shifter at `start` whose interval is set to each
// of `sequence` in turn, half a second apart; returns everything after the
// first 100 ms. `tone` must be (sequence.size() + 1) half seconds long.
std::vector<float> renderIntervalChanges(const std::vector<float>& tone, double start,
                                         const std::vector<double>& sequence) {
  EffectRig rig(kKind, {{"semitones", start}, {"mix", 1.0}});
  const size_t chunk = 24000;  // half a second, a whole number of host blocks
  const auto piece = [&](size_t index) {
    return std::vector<float>(tone.begin() + static_cast<std::ptrdiff_t>(index * chunk),
                              tone.begin() + static_cast<std::ptrdiff_t>((index + 1) * chunk));
  };
  std::vector<float> out = rig.render(piece(0));
  for (size_t i = 0; i < sequence.size(); ++i) {
    rig.set("semitones", sequence[i]);
    const std::vector<float> part = rig.render(piece(i + 1));
    out.insert(out.end(), part.begin(), part.end());
  }
  return std::vector<float>(out.begin() + 4800, out.end());
}

}  // namespace

TEST(PitchShifterTest, MovingTheIntervalUnderAudioStaysWithinAStatedStep) {
  // A 220 Hz sine at 0.25 moves 0.0072 per sample; shifted up an octave
  // 0.0144. The interval glides rather than jumps and the engine's tap is
  // continuous, so a change is a bend, with the engine's own splices on top.
  // For the changes below (through unison, which is an engine-to-delay
  // crossfade, up, down, and across the octaves) the largest step stays
  // within 0.05: under 3.5 times the slope of the fastest tone involved and
  // a fifth of the signal's peak.
  const std::vector<double> sequence = {7, -5, 12, 0, -12, 3, 0, 12, -24, 5};
  const std::vector<float> tone =
      sine(0.5 * (static_cast<double>(sequence.size()) + 1.0), 220.0, 0.25f, kRate);
  const std::vector<float> out = renderIntervalChanges(tone, 0.0, sequence);
  expectCleanSignal(out, 0.25f * 1.15f, "interval changes");
  const float step = maxStep(out, 0, out.size());
  std::cout << "[   STEP   ] largest step across ten interval changes: " << step
            << " (a 440 Hz sine of this level moves 0.0144 per sample)" << std::endl;
  EXPECT_LT(step, 0.05f) << "an interval change stepped the waveform";
}

TEST(PitchShifterTest, AnyIntervalChangeIsBoundedByTheSignalItself) {
  // The engine's weakest spot: a jump to a large up-shift while its tap is
  // close to the write head (it parks 2 ms behind at unison, and passes by
  // there after a splice) is spliced late and can click. This is the bound
  // that holds for every pair of intervals, measured with a 330 Hz sine
  // (worst of the pairs: 0 to +12 st, a step of 1.4 times the peak): never a
  // step beyond the signal's whole range, never an excursion beyond a bit
  // over its peak. What the engine settles to is the interval tests' business.
  const double intervals[] = {-24, -12, -5, 0, 5, 12, 24};
  float worst = 0.0f;
  for (const double from : intervals) {
    for (const double to : intervals) {
      if (from == to)
        continue;
      EffectRig rig(kKind, {{"semitones", from}, {"mix", 1.0}});
      const std::vector<float> tone = sine(1.5, 330.0, 0.25f, kRate);
      std::vector<float> out = rig.render(std::vector<float>(tone.begin(), tone.begin() + 48000));
      rig.set("semitones", to);
      const std::vector<float> after = rig.render(std::vector<float>(tone.begin() + 48000, tone.end()));
      out.insert(out.end(), after.begin(), after.end());
      expectCleanSignal(out, 0.25f * 1.5f, "interval pair");
      worst = std::max(worst, maxStep(out, 48000, out.size()));
    }
  }
  std::cout << "[   STEP   ] largest step over every pair of intervals: " << worst
            << " (peak 0.25, range 0.5)" << std::endl;
  EXPECT_LT(worst, 0.5f);
}

TEST(PitchShifterTest, TheTailFollowsTheWindowAndCoversTheMeasuredDecay) {
  for (int w = 0; w < 4; ++w) {
    for (const double semitones : {-24.0, -12.0, 12.0, 24.0}) {
      SCOPED_TRACE("window " + std::to_string(w) + ", " + std::to_string(semitones) + " st");
      const Params p = {{"window", static_cast<double>(w)}, {"semitones", semitones}};
      const TailMeasurement tail = expectSilenceAfterReportedTail(kKind, p);
      EXPECT_FALSE(tail.capped);
      // Reported 2 windows: close to the measured decay (1.0 to 1.6 windows),
      // never below it.
      const double windowSeconds = PitchShift::windowMs(PitchShift::windowFromIndex(w)) * 0.001;
      EXPECT_NEAR(tail.reportedSeconds, 2.0 * windowSeconds, 1.0e-9);
      EXPECT_LE(tail.measuredSeconds, tail.reportedSeconds + tail.latencySamples / kRate + 0.05);
    }
  }
}

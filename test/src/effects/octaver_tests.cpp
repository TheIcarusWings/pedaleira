// pitch.octaver (plugin/src/effects/pitch/Octaver.cpp): the conformance test
// every effect runs, then what is specific to an octaver: the sub voices at
// half and a quarter of the played frequency, their levels against the direct
// signal, the low-pass on the subs, the direct signal aligned with the
// reported latency, and voices switched on and off under audio.
#include "PitchShift.h"
#include "effects/pitch_test_signals.h"

#include <gtest/gtest.h>

using namespace pitchtest;

namespace {

constexpr const char* kKind = "pitch.octaver";

// Everything off but what a test turns on.
Params voices(double dry, double down1, double down2, double up1, double toneHz = 4000.0) {
  return {{"dryLevelDb", dry}, {"octDown1Db", down1}, {"octDown2Db", down2},
          {"octUp1Db", up1},   {"subToneHz", toneHz}, {"mix", 1.0}};
}

constexpr double kOff = -60.0;

}  // namespace

TEST(OctaverTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"octDown1Db", -6.0}, {"octDown2Db", -9.0}, {"octUp1Db", -12.0}};
  options.stateParams = {{"dryLevelDb", -3.0}, {"octDown1Db", -10.0}, {"octDown2Db", -4.0},
                         {"octUp1Db", -8.0},   {"subToneHz", 450.0},  {"window", 3.0}};
  // As for the shifter (see pitch_shifter_tests.cpp): the voices are read
  // through a tap whose delay is an average, so an impulse does not arrive at
  // the reported latency. The exact figure is tested below with the voices
  // off, where the block is the direct signal and a delay line.
  options.latency = LatencyCheck::Skip;
  expectKindConforms("pitch.octaver", options);
}

TEST(OctaverTest, TableMatchesTheBrief) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kKind);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->name, "Octaver");
  EXPECT_EQ(info->category, BlockCategory::Pitch);
  EXPECT_FLOAT_EQ(info->defaultMix, 1.0f);
  EXPECT_EQ(info->mixLaw, BlockMixLaw::Crossfade);

  struct Expected {
    const char* id;
    const char* unit;
    float min, max, def;
  };
  // A classic octaver: the direct signal at unity, one octave down at about
  // -6 dB, the others off (the floor of their knobs).
  const Expected table[] = {{"dryLevelDb", "dB", -60.0f, 6.0f, 0.0f},
                            {"octDown1Db", "dB", -60.0f, 6.0f, -6.0f},
                            {"octDown2Db", "dB", -60.0f, 6.0f, -60.0f},
                            {"octUp1Db", "dB", -60.0f, 6.0f, -60.0f},
                            {"subToneHz", "Hz", 100.0f, 4000.0f, 700.0f}};
  ASSERT_EQ(info->params.size(), 6u);
  for (size_t i = 0; i < 5; ++i) {
    const BlockParamSpec& spec = info->params[i];
    EXPECT_EQ(spec.id, table[i].id);
    EXPECT_EQ(spec.unit, table[i].unit);
    EXPECT_FLOAT_EQ(spec.min, table[i].min);
    EXPECT_FLOAT_EQ(spec.max, table[i].max);
    EXPECT_FLOAT_EQ(spec.defaultValue, table[i].def) << spec.id;
  }
  const BlockParamSpec& window = info->params[5];
  EXPECT_EQ(window.id, "window");
  EXPECT_TRUE(window.affectsLatency);
  EXPECT_FALSE(window.automatable);
}

TEST(OctaverTest, TheDefaultsAreADirectSignalAtUnityWithTheOctaveSixDbDown) {
  EffectRig rig(kKind, {{"mix", 1.0}});  // everything else as a new block has it
  const std::vector<float> in = sine(2.0, 220.0, 0.25f, kRate);
  const std::vector<float> out = rig.render(in);
  const size_t from = 48000, to = out.size();
  const double input = lineDb(in, from, to, 220.0);
  EXPECT_NEAR(lineDb(out, from, to, 220.0), input, 0.3);
  EXPECT_NEAR(lineDb(out, from, to, 110.0), input - 6.0, 0.6);
  // Nothing else is on.
  EXPECT_LT(lineDb(out, from, to, 55.0), input - 50.0);
  EXPECT_LT(lineDb(out, from, to, 440.0), input - 50.0);
}

TEST(OctaverTest, TheSubVoicesAreAtHalfAndAQuarterOfThePlayedFrequency) {
  const std::vector<float> sine220 = sine(2.5, 220.0, 0.25f, kRate);
  EffectRig rig(kKind, voices(kOff, 0.0, 0.0, 0.0));
  const std::vector<float> out = rig.render(sine220);
  const size_t from = 48000, to = out.size();
  const double input = lineDb(sine220, from, to, 220.0);
  // Down one octave, down two, up one, each where it belongs and at the
  // input's own level (0 dB knobs); the direct signal is off.
  EXPECT_NEAR(lineDb(out, from, to, 110.0), input, 0.7);
  EXPECT_NEAR(lineDb(out, from, to, 55.0), input, 0.7);
  EXPECT_NEAR(lineDb(out, from, to, 440.0), input, 0.7);
  EXPECT_LT(lineDb(out, from, to, 220.0), input - 40.0);

  // A guitar-like tone: with only the first sub on, the dominant frequency is
  // half the played one, then a quarter with only the second.
  const std::vector<float> note = harmonicTone(2.5, 196.0, 0.3f);
  {
    EffectRig octave(kKind, voices(kOff, 0.0, kOff, kOff));
    const std::vector<float> down = octave.render(note);
    EXPECT_NEAR(centsFrom(down, from, to, 98.0), 0.0, 4.0);
    // The strongest line is the sub's fundamental; the played pitch is only
    // there as the sub's own second partial (half the amplitude).
    EXPECT_LT(lineDb(down, from, to, 196.0), lineDb(down, from, to, 98.0) - 4.0);
  }
  {
    EffectRig two(kKind, voices(kOff, kOff, 0.0, kOff));
    const std::vector<float> down = two.render(note);
    EXPECT_NEAR(centsFrom(down, from, to, 49.0), 0.0, 4.0);
  }
}

TEST(OctaverTest, TheLevelsFollowTheKnobs) {
  const std::vector<float> in = sine(2.0, 220.0, 0.25f, kRate);
  const size_t from = 48000, to = in.size();
  const double input = lineDb(in, from, to, 220.0);
  struct Setting {
    double dry, down1, down2, up1;
  };
  for (const Setting& s : {Setting{0.0, -6.0, -12.0, -18.0}, Setting{-10.0, 0.0, -3.0, -24.0},
                           Setting{6.0, -20.0, 3.0, 0.0}}) {
    SCOPED_TRACE(std::to_string(s.dry) + " " + std::to_string(s.down1) + " " +
                 std::to_string(s.down2) + " " + std::to_string(s.up1));
    EffectRig rig(kKind, voices(s.dry, s.down1, s.down2, s.up1));
    const std::vector<float> out = rig.render(in);
    EXPECT_NEAR(lineDb(out, from, to, 220.0), input + s.dry, 0.5);
    EXPECT_NEAR(lineDb(out, from, to, 110.0), input + s.down1, 0.8);
    EXPECT_NEAR(lineDb(out, from, to, 55.0), input + s.down2, 0.8);
    EXPECT_NEAR(lineDb(out, from, to, 440.0), input + s.up1, 0.8);
  }
}

TEST(OctaverTest, TheSubToneIsALowPassOnTheSubVoicesOnly) {
  // 800 Hz in: the sub is at 400 Hz, the octave up at 1600 Hz.
  const std::vector<float> in = sine(2.0, 800.0, 0.25f, kRate);
  const size_t from = 48000, to = in.size();
  const auto render = [&](double toneHz) {
    EffectRig rig(kKind, voices(kOff, 0.0, kOff, 0.0, toneHz));
    return rig.render(in);
  };
  const std::vector<float> open = render(4000.0);
  const std::vector<float> dark = render(100.0);
  const std::vector<float> corner = render(400.0);
  // Two octaves above the corner, a second-order low-pass is down 24 dB.
  EXPECT_LT(lineDb(dark, from, to, 400.0), lineDb(open, from, to, 400.0) - 20.0);
  // At the corner of a Butterworth, -3 dB.
  EXPECT_NEAR(lineDb(corner, from, to, 400.0), lineDb(open, from, to, 400.0) - 3.0, 1.0);
  // The octave up is not filtered.
  EXPECT_NEAR(lineDb(dark, from, to, 1600.0), lineDb(open, from, to, 1600.0), 0.5);
}

TEST(OctaverTest, TheDirectSignalIsDelayedByTheReportedLatencyAtEveryWindow) {
  // With the voices off the block is the direct signal and a delay by the
  // reported latency, so that figure is exact, and the voices (whose delay is
  // an average, see the shifter tests) are blended with a direct signal that
  // is as late as they are on average.
  for (int w = 0; w < 4; ++w) {
    SCOPED_TRACE("window " + std::to_string(w));
    Params p = voices(0.0, kOff, kOff, kOff);
    p.emplace_back("window", static_cast<double>(w));
    EffectRig rig(kKind, p);
    EffectRig reference{juce::String()};
    const int latency = rig.latencySamples();
    EXPECT_EQ(latency, PitchShift::latencySamples(PitchShift::windowFromIndex(w), kRate));

    const std::vector<float> ir = captureImpulseResponse(rig, 0.2);
    const int origin = onsetIndex(captureImpulseResponse(reference, 0.05));
    reference.restart();
    EXPECT_EQ(onsetIndex(ir) - origin, latency);
    EXPECT_EQ(peakIndexOf(ir) - origin, latency);

    rig.restart();
    const std::vector<float> in = noise(1.0, 17u, 0.25f, kRate);
    const std::vector<float> out = rig.render(in);
    const std::vector<float> dry = reference.render(in);
    double worst = 0.0;
    for (size_t i = static_cast<size_t>(latency); i < in.size(); ++i)
      worst = std::max(worst, static_cast<double>(std::abs(out[i] - dry[i - static_cast<size_t>(latency)])));
    EXPECT_LT(worst, 1.0e-6);
  }
}

TEST(OctaverTest, ALatencyChangeIsLatchedAndMovesTheDirectSignalWithIt) {
  EffectRig rig(kKind, voices(0.0, kOff, kOff, kOff));
  EXPECT_EQ(rig.latencySamples(), 768);
  rig.set("window", 3.0);
  EXPECT_EQ(rig.latencySamples(), 768);
  rig.settle();
  EXPECT_EQ(rig.latencySamples(), 1488);
  const std::vector<float> ir = captureImpulseResponse(rig, 0.2);
  EffectRig reference{juce::String()};
  EXPECT_EQ(onsetIndex(ir) - onsetIndex(captureImpulseResponse(reference, 0.05)), 1488);
}

TEST(OctaverTest, TheMixCrossfadesTheBlendAgainstTheChainsDirectSignal) {
  // The block is a blend (its own direct level), so the common mix is not the
  // balance between guitar and octave: at 50 % the direct signal is still at
  // unity (half from the chain's copy, half from the blend's) and the octave
  // is at half its level.
  const std::vector<float> in = sine(2.0, 220.0, 0.25f, kRate);
  const size_t from = 48000, to = in.size();
  EffectRig rig(kKind, {{"mix", 0.5}});
  const std::vector<float> out = rig.render(in);
  const double input = lineDb(in, from, to, 220.0);
  EXPECT_NEAR(lineDb(out, from, to, 220.0), input, 0.3);
  EXPECT_NEAR(lineDb(out, from, to, 110.0), input - 6.0 - 6.02, 0.8);
}

TEST(OctaverTest, TwoInstrumentsInDualMonoGetTheirOwnOctaves) {
  EffectRig rig(kKind, [] {
    Params p = voices(0.0, -3.0, kOff, kOff);
    return p;
  }(), rigFor(Layout::DualMono));
  const Stereo out = rig.render(Stereo{sine(2.0, 220.0, 0.25f, kRate), sine(2.0, 330.0, 0.25f, kRate)});
  const size_t from = 48000, to = out.left.size();
  const double input = lineDb(sine(2.0, 220.0, 0.25f, kRate), from, to, 220.0);
  EXPECT_NEAR(lineDb(out.left, from, to, 110.0), input - 3.0, 0.8);
  EXPECT_NEAR(lineDb(out.right, from, to, 165.0), input - 3.0, 0.8);
  EXPECT_LT(lineDb(out.left, from, to, 165.0), input - 50.0);
  EXPECT_LT(lineDb(out.right, from, to, 110.0), input - 50.0);
}

TEST(OctaverTest, SilenceNoiseAndSweepsStayFiniteAndBounded) {
  // Peak limits: the sum of the levels in use times the input's peak, and a
  // margin for the engine's power-normalised fades.
  struct Setting {
    Params params;
    float gainSum;
  };
  const Setting settings[] = {{{{"mix", 1.0}}, 1.5f},
                              {voices(-6.0, -6.0, -6.0, -6.0), 2.0f},
                              {voices(0.0, 0.0, 0.0, 0.0, 200.0), 4.0f}};
  for (const Setting& setting : settings) {
    EffectRig rig(kKind, setting.params);
    EXPECT_EQ(peakOf(rig.renderSilence(24000)), 0.0f);
    expectCleanSignal(rig.render(noise(2.0, 61u, 0.5f, kRate)), 0.5f * setting.gainSum * 1.3f, "noise");
    rig.restart();
    expectCleanSignal(rig.render(logSweep(3.0, 30.0, 8000.0, 0.4f)), 0.4f * setting.gainSum * 1.3f,
                      "sweep");
  }
}

TEST(OctaverTest, SwitchingAVoiceOnAndOffUnderAudioDoesNotClick) {
  // A 220 Hz sine at 0.25 moves 0.0072 per sample; the sub octave adds at
  // most 0.0036 at unity. A voice that is switched on starts its engine from
  // rest and fades in over 20 ms; switched off, it fades out and stops. The
  // output never steps by more than the two tones together move.
  EffectRig rig(kKind, voices(0.0, kOff, kOff, kOff));
  const std::vector<float> tone = sine(4.0, 220.0, 0.25f, kRate);
  const size_t chunk = 24000;
  std::vector<float> out;
  const double sequence[] = {-6.0, kOff, 0.0, kOff, -3.0, kOff, 0.0};
  out = rig.render(std::vector<float>(tone.begin(), tone.begin() + static_cast<std::ptrdiff_t>(chunk)));
  size_t at = chunk;
  for (const double level : sequence) {
    if (at + chunk > tone.size())
      break;
    rig.set("octDown1Db", level);
    rig.set("octUp1Db", level);
    const std::vector<float> part = rig.render(std::vector<float>(
        tone.begin() + static_cast<std::ptrdiff_t>(at), tone.begin() + static_cast<std::ptrdiff_t>(at + chunk)));
    out.insert(out.end(), part.begin(), part.end());
    at += chunk;
  }
  expectCleanSignal(out, 0.25f * 3.0f, "voice switching");
  const float step = maxStep(out, 4800, out.size());
  std::cout << "[   STEP   ] largest step switching voices on and off: " << step << std::endl;
  // Direct (0.0072) + octave down (0.0036) + octave up (0.0144) at most.
  EXPECT_LT(step, 0.0072f + 0.0036f + 0.0144f + 0.01f);
}

TEST(OctaverTest, TheTailFollowsTheWindowAndCoversTheMeasuredDecay) {
  for (int w = 0; w < 4; ++w) {
    SCOPED_TRACE("window " + std::to_string(w));
    Params p = {{"octDown1Db", -3.0}, {"octDown2Db", -3.0}, {"octUp1Db", -3.0}};
    p.emplace_back("window", static_cast<double>(w));
    const TailMeasurement tail = expectSilenceAfterReportedTail(kKind, p);
    EXPECT_FALSE(tail.capped);
    EXPECT_LE(tail.measuredSeconds, tail.reportedSeconds + tail.latencySamples / kRate + 0.05);
  }
}

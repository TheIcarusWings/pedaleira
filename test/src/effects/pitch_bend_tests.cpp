// pitch.whammy "Pitch Bend" (plugin/src/effects/pitch/PitchBend.cpp): the
// conformance test, then the pedal: the heel and toe intervals, the shift
// following the position (statically and in a sweep), a click-free sweep, the
// two modes, the exact constant latency of the attack at every position, the
// reading, and the position being a scene-assignable parameter.
#include "effects/pitch_test_signals.h"

#include <gtest/gtest.h>

using namespace pitchtest;

namespace {

constexpr const char* kKind = "pitch.whammy";

const BlockParamSpec& specOf(const BlockKindInfo& info, const char* id) {
  for (const auto& spec : info.params)
    if (spec.id == id)
      return spec;
  ADD_FAILURE() << "no parameter " << id;
  return info.params.front();
}

}  // namespace

TEST(PitchBendTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"toeSt", 12.0}, {"heelSt", -5.0}, {"position", 60.0}};
  options.stateParams = {{"toeSt", 7.0}, {"heelSt", -3.0}, {"position", 42.0}, {"mode", 1.0}};
  options.cpuAverageBudgetNs = 25000.0;
  options.cpuWorstBudgetNs = 250000.0;
  expectKindConforms("pitch.whammy", options);
}

TEST(PitchBendTest, TableMatchesTheBrief) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kKind);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->name, "Pitch Bend");
  EXPECT_EQ(info->category, BlockCategory::Pitch);
  EXPECT_FLOAT_EQ(info->defaultMix, 1.0f);
  EXPECT_EQ(info->mixLaw, BlockMixLaw::Crossfade);

  const BlockParamSpec& toe = specOf(*info, "toeSt");
  EXPECT_FLOAT_EQ(toe.min, -24.0f);
  EXPECT_FLOAT_EQ(toe.max, 24.0f);
  EXPECT_FLOAT_EQ(toe.defaultValue, 12.0f);
  EXPECT_EQ(toe.unit, "st");
  const BlockParamSpec& heel = specOf(*info, "heelSt");
  EXPECT_FLOAT_EQ(heel.min, -24.0f);
  EXPECT_FLOAT_EQ(heel.max, 24.0f);
  EXPECT_FLOAT_EQ(heel.defaultValue, 0.0f);
  const BlockParamSpec& position = specOf(*info, "position");
  EXPECT_FLOAT_EQ(position.min, 0.0f);
  EXPECT_FLOAT_EQ(position.max, 100.0f);
  EXPECT_EQ(position.unit, "%");
  EXPECT_TRUE(position.automatable);  // the expression pedal, MIDI and the host can drive it
  EXPECT_FALSE(position.affectsLatency);
  const BlockParamSpec& mode = specOf(*info, "mode");
  EXPECT_EQ(mode.type, BlockParamType::Choice);
  EXPECT_EQ(mode.choices.size(), 2);
  EXPECT_EQ(mode.choices[0], "Classic");
  EXPECT_EQ(mode.choices[1], "Detune-drop");
  EXPECT_EQ(info->params.size(), 4u);
  ASSERT_EQ(info->readings.size(), 1u);
  EXPECT_EQ(info->readings[0].id, "shiftSt");
}

TEST(PitchBendTest, PositionIsSceneAssignableAndTheOtherSettingsToo) {
  EffectRig rig(kKind);
  const juce::String id(rig.blockId());
  const auto args = [&](const char* param) {
    return "{\"blockId\":\"" + id + "\",\"param\":\"" + juce::String(param) + "\"}";
  };
  for (const char* param : {"position", "toeSt", "heelSt", "mode"})
    rig.must("scene.assignParam", args(param));
  rig.must("scene.add");
  rig.must("scene.setParamValue", "{\"scene\":\"B\",\"blockId\":\"" + id + "\",\"param\":\"position\",\"value\":100}");
  rig.must("scene.select", "{\"scene\":\"B\"}");
  EXPECT_NEAR(static_cast<double>(rig.blockState()["kindParams"]["position"]), 100.0, 1e-4);
  rig.must("scene.select", "{\"scene\":\"A\"}");
  EXPECT_NEAR(static_cast<double>(rig.blockState()["kindParams"]["position"]), 0.0, 1e-4);

  // A position set aside for the expression pedal leaves the scenes.
  rig.must("expression.assignParam", args("position"));
  const CommandResult refused =
      rig.commands.registry().run("scene.assignParam", juce::JSON::parse(args("position")));
  EXPECT_FALSE(refused.ok);
}

namespace {

// The shift the pedal asks for at a position.
double shiftAt(double heel, double toe, double position) { return heel + (toe - heel) * position / 100.0; }

}  // namespace

TEST(PitchBendTest, TheHeelAndToeIntervalsAndEverythingBetweenAreExact) {
  struct Case {
    double heel, toe, position, hz;
  };
  // Within a cent of the pedal's interval on a sine, at the extremes and in
  // between, with heel and toe on either side of zero.
  const Case cases[] = {{0, 12, 0, 220}, {0, 12, 100, 220}, {0, 12, 50, 220},   {-7, 12, 0, 196},
                        {-7, 12, 100, 196}, {-7, 12, 25, 196}, {-12, 5, 80, 330}, {3, -9, 100, 220},
                        {-24, 24, 75, 110}, {0, 24, 100, 110}};
  double worst = 0.0;
  for (const Case& c : cases) {
    SCOPED_TRACE("heel " + std::to_string(c.heel) + ", toe " + std::to_string(c.toe) + ", position " +
                 std::to_string(c.position) + ", " + std::to_string(c.hz) + " Hz");
    EffectRig rig(kKind, {{"heelSt", c.heel}, {"toeSt", c.toe}, {"position", c.position}, {"mix", 1.0}});
    const std::vector<float> out = rig.render(sine(2.5, c.hz, 0.3f, kRate));
    const double target = shifted(c.hz, shiftAt(c.heel, c.toe, c.position));
    const double measured = peakFrequencyHz(out, 48000, out.size(), target);
    const double error = centsBetween(measured, target);
    worst = std::max(worst, std::abs(error));
    EXPECT_NEAR(error, 0.0, 1.0);
    EXPECT_NEAR(lineDb(out, 48000, out.size(), target), lineDb(sine(2.5, c.hz, 0.3f, kRate), 48000, 120000, c.hz),
                1.0);
  }
  std::cout << "[  PEDAL   ] worst interval error over ten pedal settings: " << juce::String(worst, 3) << " ct"
            << std::endl;
}

TEST(PitchBendTest, TheSweepFollowsThePositionAtEveryMoment) {
  // The position moves from heel to toe over four seconds, written every
  // block as a controller does; the pitch of the output, read in 200 ms
  // windows, is the pitch the pedal asks for a moment (the engine's
  // wandering delay, 2 to 30 ms) earlier.
  const double heel = -5.0, toe = 12.0, hz = 220.0;
  EffectRig rig(kKind, {{"heelSt", heel}, {"toeSt", toe}, {"position", 0.0}, {"mix", 1.0}});
  const double seconds = 4.0;
  const std::vector<float> in = sine(seconds + 0.5, hz, 0.3f, kRate);
  std::vector<float> out;
  const size_t block = static_cast<size_t>(rig.blockSize());
  for (size_t at = 0; at < in.size(); at += block) {
    const double t = static_cast<double>(at) / kRate;
    rig.set("position", juce::jlimit(0.0, 100.0, 100.0 * t / seconds));
    const size_t n = std::min(block, in.size() - at);
    const std::vector<float> part = rig.render(std::vector<float>(in.begin() + static_cast<std::ptrdiff_t>(at),
                                                                  in.begin() + static_cast<std::ptrdiff_t>(at + n)));
    out.insert(out.end(), part.begin(), part.end());
  }
  double worst = 0.0;
  for (const double centre : {0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5}) {
    const size_t from = static_cast<size_t>((centre - 0.1) * kRate), to = static_cast<size_t>((centre + 0.1) * kRate);
    const double asked = shifted(hz, shiftAt(heel, toe, 100.0 * (centre - 0.016) / seconds));
    const double measured = peakFrequencyHz(out, from, to, asked, 150.0);
    const double error = centsBetween(measured, asked);
    worst = std::max(worst, std::abs(error));
    std::cout << "[  PEDAL   ] sweep at " << centre << " s: " << juce::String(measured, 2) << " Hz, the pedal asks "
              << juce::String(asked, 2) << " Hz (" << juce::String(error, 1) << " ct)" << std::endl;
    EXPECT_NEAR(error, 0.0, 12.0) << "at " << centre << " s";
  }
  std::cout << "[  PEDAL   ] worst deviation from the pedal along the sweep: " << juce::String(worst, 1) << " ct"
            << std::endl;
}

TEST(PitchBendTest, SweepingThePositionInSmallStepsMakesNoZipperNoise) {
  // The position moves 1 % a block (0.13 st at heel 0, toe 12) up and back
  // down over a 220 Hz sine at 0.25. The output is a sine that bends; a step in
  // the pitch at block rate would be a step in the waveform. The largest step
  // between neighbouring samples stays within what the highest note involved
  // (440 Hz at 0.25: 0.0144 per sample) explains, with room for the engine's
  // own splices, and the sweep adds no energy off the moving line.
  EffectRig rig(kKind, {{"heelSt", 0.0}, {"toeSt", 12.0}, {"position", 0.0}, {"mix", 1.0}});
  const std::vector<float> in = sine(2.0, 220.0, 0.25f, kRate);
  std::vector<float> out;
  const size_t block = static_cast<size_t>(rig.blockSize());
  int step = 0;
  for (size_t at = 0; at < in.size(); at += block, ++step) {
    const int up = step < 100 ? step : 200 - step;
    rig.set("position", juce::jlimit(0.0, 100.0, static_cast<double>(up)));
    const size_t n = std::min(block, in.size() - at);
    const std::vector<float> part = rig.render(std::vector<float>(in.begin() + static_cast<std::ptrdiff_t>(at),
                                                                  in.begin() + static_cast<std::ptrdiff_t>(at + n)));
    out.insert(out.end(), part.begin(), part.end());
  }
  expectCleanSignal(out, 0.25f * 1.2f, "sweep");
  const float largest = maxStep(out, 4800, out.size());
  std::cout << "[  PEDAL   ] largest step across a 1 %-per-block sweep: " << largest
            << " (a 440 Hz sine of this level moves 0.0144 per sample)" << std::endl;
  EXPECT_LT(largest, 0.03f);
}

TEST(PitchBendTest, AtTheHeelOfAZeroHeelTheBlockIsAnExactDelay) {
  EffectRig rig(kKind, {{"heelSt", 0.0}, {"toeSt", 12.0}, {"position", 0.0}, {"mix", 1.0}});
  EffectRig reference{juce::String()};
  const std::vector<float> in = noise(1.0, 31u, 0.25f, kRate);
  const std::vector<float> out = rig.render(in);
  const std::vector<float> dry = reference.render(in);
  const int latency = rig.latencySamples();
  double worst = 0.0;
  for (size_t i = static_cast<size_t>(latency); i < in.size(); ++i)
    worst = std::max(worst, static_cast<double>(std::abs(out[i] - dry[i - static_cast<size_t>(latency)])));
  EXPECT_LT(worst, 1.0e-6);
}

TEST(PitchBendTest, TheAttackArrivesAtTheReportedLatencyAtEveryPosition) {
  // The constant latency of the engine (PitchShift.h): an impulse measured at
  // the output arrives exactly `latencySamples()` after it went in, whatever the
  // pedal is doing, in both modes (the dry share of the second is delayed by the
  // same amount).
  for (const double mode : {0.0, 1.0}) {
    for (const double position : {0.0, 10.0, 33.0, 50.0, 71.0, 100.0}) {
      SCOPED_TRACE("mode " + std::to_string(mode) + ", position " + std::to_string(position));
      EffectRig rig(kKind, {{"heelSt", -12.0}, {"toeSt", 24.0}, {"position", position}, {"mode", mode}, {"mix", 1.0}});
      EffectRig reference{juce::String()};
      const std::vector<float> ir = captureImpulseResponse(rig, 0.3);
      const int origin = onsetIndex(captureImpulseResponse(reference, 0.05));
      EXPECT_EQ(peakIndexOf(ir) - origin, rig.latencySamples());
      // The 1 % onset can come a few samples before the peak: the engine reads
      // between samples (cubic interpolation, +-2 input samples of spread).
      EXPECT_LE(onsetIndex(ir) - origin, rig.latencySamples());
      EXPECT_GE(onsetIndex(ir) - origin, rig.latencySamples() - 4);
    }
  }
  EffectRig rig(kKind);
  std::cout << "[ LATENCY  ] pitch.whammy reports " << rig.latencySamples() << " samples ("
            << juce::String(rig.latencySamples() / 48.0, 2) << " ms)" << std::endl;
}

TEST(PitchBendTest, ClassicIsTheShiftedSignalAndDetuneDropAddsTheGuitarBesideIt) {
  const std::vector<float> in = sine(2.0, 220.0, 0.3f, kRate);
  const double input = lineDb(in, 48000, 96000, 220.0);
  {
    EffectRig rig(kKind, {{"heelSt", 0.0}, {"toeSt", -12.0}, {"position", 100.0}, {"mode", 0.0}});
    const std::vector<float> out = rig.render(in);
    EXPECT_NEAR(lineDb(out, 48000, out.size(), 110.0), input, 1.0);
    EXPECT_LT(lineDb(out, 48000, out.size(), 220.0), input - 35.0);
  }
  {
    EffectRig rig(kKind, {{"heelSt", 0.0}, {"toeSt", -12.0}, {"position", 100.0}, {"mode", 1.0}});
    const std::vector<float> out = rig.render(in);
    // Half and half: each at -6 dB, the guitar and the octave under it.
    EXPECT_NEAR(lineDb(out, 48000, out.size(), 110.0), input - 6.02, 1.0);
    EXPECT_NEAR(lineDb(out, 48000, out.size(), 220.0), input - 6.02, 1.0);
  }
  {
    // Level neutral where there is nothing to detune: the heel at 0 st.
    EffectRig rig(kKind, {{"heelSt", 0.0}, {"toeSt", -12.0}, {"position", 0.0}, {"mode", 1.0}});
    const std::vector<float> out = rig.render(in);
    EXPECT_NEAR(lineDb(out, 48000, out.size(), 220.0), input, 0.3);
  }
  {
    // A detune: the pedal at +0.2 st (20 cents) beats against the guitar, two
    // lines 20 cents apart at the same level.
    EffectRig rig(kKind, {{"heelSt", 0.0}, {"toeSt", 1.0}, {"position", 20.0}, {"mode", 1.0}});
    const std::vector<float> out = rig.render(in);
    EXPECT_NEAR(lineDb(out, 48000, out.size(), 220.0), input - 6.02, 1.5);
    EXPECT_NEAR(centsBetween(peakFrequencyHz(out, 48000, out.size(), shifted(220.0, 0.2), 6.0), 220.0), 20.0, 1.0);
  }
}

TEST(PitchBendTest, MonoStereoAndDualMonoAllFollowThePedal) {
  const std::vector<float> in = harmonicTone(2.0, 196.0, 0.3f);
  for (const Layout layout : {Layout::Mono, Layout::Stereo, Layout::DualMono}) {
    SCOPED_TRACE(layoutName(layout));
    EffectRig rig(kKind, {{"heelSt", 0.0}, {"toeSt", 7.0}, {"position", 100.0}, {"mix", 1.0}}, rigFor(layout));
    const Stereo out = rig.renderBoth(in);
    EXPECT_NEAR(centsBetween(peakFrequencyHz(out.left, 48000, out.left.size(), shifted(196.0, 7.0)), shifted(196.0, 7.0)),
                0.0, 3.0);
    if (layout != Layout::Mono)
      EXPECT_NEAR(centsBetween(peakFrequencyHz(out.right, 48000, out.right.size(), shifted(196.0, 7.0)), shifted(196.0, 7.0)),
                  0.0, 3.0);
  }
}

TEST(PitchBendTest, TheShiftReadingIsTheIntervalInUse) {
  DirectKind kind(kKind, {{"heelSt", -4.0}, {"toeSt", 10.0}, {"position", 50.0}}, Layout::Mono);
  EXPECT_FLOAT_EQ(kind.engine().getReading(0), -24.0f) << "nothing processed yet: the resting value is the range's minimum";
  kind.render(noise(0.5, 3u, 0.2f, kRate));
  EXPECT_NEAR(kind.engine().getReading(0), 3.0, 0.02);
  const int position = kind.engine().findParameter("position");
  kind.engine().setParameter(position, 100.0f);
  kind.render(noise(0.5, 4u, 0.2f, kRate));
  EXPECT_NEAR(kind.engine().getReading(0), 10.0, 0.02);
  kind.engine().reset();
  EXPECT_FLOAT_EQ(kind.engine().getReading(0), -24.0f) << "a reset puts the reading back to rest";
}

TEST(PitchBendTest, TheTailCoversTheMeasuredDecay) {
  for (const double position : {0.0, 100.0}) {
    const Params p = {{"toeSt", -24.0}, {"heelSt", 12.0}, {"position", position}};
    const TailMeasurement tail = expectSilenceAfterReportedTail(kKind, p);
    EXPECT_FALSE(tail.capped);
    EXPECT_LE(tail.measuredSeconds, tail.reportedSeconds + tail.latencySamples / kRate + 0.05);
  }
}

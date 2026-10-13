// pitch.octavedown "Analog Octave" (plugin/src/effects/pitch/AnalogOctave.cpp):
// the conformance test, then what a frequency divider does: the output
// frequency of the octave and of the sub-octave against the played note on
// sines and on guitar-like tones, the levels, the tone filter, the envelope
// following the playing, silence and noise, the tracked-frequency reading, and
// the dual mono and mono layouts. The block is monophonic by nature; one test
// shows what two notes at once do to it.
#include "effects/pitch_test_signals.h"

#include <gtest/gtest.h>

using namespace pitchtest;

namespace {

constexpr const char* kKind = "pitch.octavedown";

const BlockParamSpec& specOf(const BlockKindInfo& info, const char* id) {
  for (const auto& spec : info.params)
    if (spec.id == id)
      return spec;
  ADD_FAILURE() << "no parameter " << id;
  return info.params.front();
}

// The octave alone (-1 octave), or the sub alone (-2 octaves), no dry, a high
// tone cutoff so the divided square is not coloured by the filter.
Params alone(bool sub) {
  return {{"dryDb", -60.0}, {"octaveDb", sub ? -60.0 : 0.0}, {"subDb", sub ? 0.0 : -60.0},
          {"toneHz", 5000.0}, {"mix", 1.0}};
}

// The frequency of the fundamental of a square wave of known frequency: the
// strongest line near it.
double fundamentalOf(const std::vector<float>& out, size_t from, double expectedHz) {
  return peakFrequencyHz(out, from, out.size(), expectedHz, 50.0);
}

}  // namespace

TEST(AnalogOctaveTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"octaveDb", -3.0}, {"subDb", -6.0}, {"toneHz", 1500.0}, {"tracking", 60.0}};
  options.stateParams = {{"dryDb", -4.0}, {"octaveDb", -8.0}, {"subDb", -2.5}, {"toneHz", 700.0}, {"tracking", 20.0}};
  options.cpuAverageBudgetNs = 25000.0;
  options.cpuWorstBudgetNs = 250000.0;
  expectKindConforms("pitch.octavedown", options);
}

TEST(AnalogOctaveTest, TableMatchesTheBrief) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kKind);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->name, "Analog Octave");
  EXPECT_EQ(info->category, BlockCategory::Pitch);
  EXPECT_EQ(info->mixLaw, BlockMixLaw::Crossfade);
  EXPECT_FLOAT_EQ(info->defaultMix, 1.0f);
  EXPECT_EQ(specOf(*info, "dryDb").unit, "dB");
  EXPECT_FLOAT_EQ(specOf(*info, "dryDb").defaultValue, 0.0f);
  EXPECT_EQ(specOf(*info, "octaveDb").unit, "dB");
  EXPECT_EQ(specOf(*info, "subDb").unit, "dB");
  EXPECT_EQ(specOf(*info, "toneHz").unit, "Hz");
  EXPECT_EQ(specOf(*info, "tracking").unit, "%");
  EXPECT_EQ(info->params.size(), 5u);
  ASSERT_EQ(info->readings.size(), 1u);
  EXPECT_EQ(info->readings[0].id, "trackedHz");
  EXPECT_FALSE(info->spillover);
}

TEST(AnalogOctaveTest, TheOctaveIsHalfTheFrequencyAndTheSubAQuarterOfIt) {
  // A sine and a guitar-like harmonic tone at the open strings and a few
  // notes up the neck: the divided fundamental is exactly half and a quarter
  // of the played one (a flip-flop divides by counting, so the error is the
  // measurement's).
  double worstOctave = 0.0, worstSub = 0.0;
  for (const double hz : {82.407, 110.0, 146.832, 195.998, 246.942, 329.628, 440.0}) {
    for (const bool harmonic : {false, true}) {
      const std::vector<float> in = harmonic ? harmonicTone(2.0, hz, 0.3f) : sine(2.0, hz, 0.3f, kRate);
      {
        EffectRig rig(kKind, alone(false));
        const std::vector<float> out = rig.render(in);
        const double measured = fundamentalOf(out, 48000, hz / 2.0);
        const double error = centsBetween(measured, hz / 2.0);
        worstOctave = std::max(worstOctave, std::abs(error));
        EXPECT_NEAR(error, 0.0, 1.0) << hz << " Hz, " << (harmonic ? "harmonic" : "sine") << ": octave";
        // It is a clean division: nothing at the played pitch to speak of.
        EXPECT_GT(lineDb(out, 48000, out.size(), hz / 2.0), lineDb(out, 48000, out.size(), hz) + 10.0);
      }
      {
        EffectRig rig(kKind, alone(true));
        const std::vector<float> out = rig.render(in);
        const double measured = fundamentalOf(out, 48000, hz / 4.0);
        const double error = centsBetween(measured, hz / 4.0);
        worstSub = std::max(worstSub, std::abs(error));
        EXPECT_NEAR(error, 0.0, 1.0) << hz << " Hz, " << (harmonic ? "harmonic" : "sine") << ": sub-octave";
        EXPECT_GT(lineDb(out, 48000, out.size(), hz / 4.0), lineDb(out, 48000, out.size(), hz / 2.0) + 10.0);
      }
    }
  }
  std::cout << "[ OCTAVE   ] worst error of the divided frequency over 7 notes x 2 tones: octave "
            << juce::String(worstOctave, 3) << " ct, sub-octave " << juce::String(worstSub, 3) << " ct" << std::endl;
}

TEST(AnalogOctaveTest, ThePlayedNoteIsFoundByTheTrackingFilterWhateverItsStrongestHarmonic) {
  // A tone whose second harmonic is twice the fundamental: a comparator on the
  // raw signal would cross zero twice a cycle and divide to the wrong note.
  for (const double hz : {110.0, 196.0, 293.66}) {
    std::vector<float> x(static_cast<size_t>(2.0 * kRate));
    for (size_t i = 0; i < x.size(); ++i) {
      const double t = static_cast<double>(i) / kRate;
      x[i] = static_cast<float>(0.12 * std::sin(2.0 * kPi * hz * t) + 0.24 * std::sin(2.0 * kPi * 2.0 * hz * t + 0.7) +
                                0.08 * std::sin(2.0 * kPi * 3.0 * hz * t + 1.9));
    }
    EffectRig rig(kKind, alone(false));
    const std::vector<float> out = rig.render(x);
    EXPECT_NEAR(centsBetween(fundamentalOf(out, 48000, hz / 2.0), hz / 2.0), 0.0, 1.0) << hz;
  }
}

TEST(AnalogOctaveTest, TheDividedToneIsASquareWaveWithTheLowPassOnIt) {
  // 196 Hz played: the octave is a square at 98 Hz, with odd harmonics only at
  // 1/n; the tone control takes the upper ones off.
  const std::vector<float> in = sine(2.0, 196.0, 0.3f, kRate);
  Params open = alone(false);
  open.push_back({"toneHz", 5000.0});
  EffectRig rig(kKind, open);
  const std::vector<float> out = rig.render(in);
  const double fundamental = lineDb(out, 48000, out.size(), 98.0);
  EXPECT_NEAR(lineDb(out, 48000, out.size(), 294.0) - fundamental, -9.54, 1.5);   // the third: 1/3
  EXPECT_NEAR(lineDb(out, 48000, out.size(), 490.0) - fundamental, -13.98, 1.5);  // the fifth: 1/5
  EXPECT_LT(lineDb(out, 48000, out.size(), 196.0), fundamental - 35.0);           // no even harmonics
  EXPECT_LT(lineDb(out, 48000, out.size(), 392.0), fundamental - 35.0);

  Params dark = alone(false);
  dark.push_back({"toneHz", 150.0});
  EffectRig darkRig(kKind, dark);
  const std::vector<float> softened = darkRig.render(in);
  const double softFundamental = lineDb(softened, 48000, softened.size(), 98.0);
  // The third harmonic (294 Hz) is at least 12 dB lower than in the open tone,
  // relative to the fundamental.
  EXPECT_LT(lineDb(softened, 48000, softened.size(), 294.0) - softFundamental, (lineDb(out, 48000, out.size(), 294.0) - fundamental) - 12.0);
}

TEST(AnalogOctaveTest, TheThreeLevelsAreTheLevelsOfTheThreeSignals) {
  const std::vector<float> in = sine(2.0, 196.0, 0.3f, kRate);
  const double input = lineDb(in, 48000, 96000, 196.0);
  // Dry only: the input, bit for bit (the dry level at 0 dB, the octaves off).
  {
    EffectRig rig(kKind, {{"octaveDb", -60.0}, {"subDb", -60.0}, {"mix", 1.0}});
    EffectRig reference{juce::String()};
    const std::vector<float> out = rig.render(in);
    const std::vector<float> dry = reference.render(in);
    double worst = 0.0;
    for (size_t i = 0; i < in.size(); ++i)
      worst = std::max(worst, static_cast<double>(std::abs(out[i] - dry[i])));
    EXPECT_LT(worst, 1.0e-6);
  }
  // Dry and octave together: each line where its knob puts it.
  {
    EffectRig rig(kKind, {{"dryDb", -6.0}, {"octaveDb", 0.0}, {"subDb", -60.0}, {"toneHz", 5000.0}, {"mix", 1.0}});
    const std::vector<float> out = rig.render(in);
    EXPECT_NEAR(lineDb(out, 48000, out.size(), 196.0), input - 6.02, 0.5);
    const double octave = lineDb(out, 48000, out.size(), 98.0);
    rig.set("octaveDb", -6.0);
    rig.restart();
    EXPECT_NEAR(lineDb(rig.render(in), 48000, 96000, 98.0), octave - 6.02, 0.6);
    std::cout << "[ OCTAVE   ] the octave at 0 dB on a 0.3 sine: " << juce::String(octave - input, 2)
              << " dB against the input's line" << std::endl;
  }
  // The sub's own knob.
  {
    EffectRig rig(kKind, {{"dryDb", -60.0}, {"octaveDb", -60.0}, {"subDb", 0.0}, {"toneHz", 5000.0}, {"mix", 1.0}});
    const std::vector<float> a = rig.render(in);
    rig.set("subDb", -12.0);
    rig.restart();
    const std::vector<float> b = rig.render(in);
    EXPECT_NEAR(lineDb(b, 48000, b.size(), 49.0) - lineDb(a, 48000, a.size(), 49.0), -12.0, 0.6);
  }
}

TEST(AnalogOctaveTest, TheOutputFollowsTheLevelOfThePlaying) {
  // The same note at 0.4 and at 0.1 (12 dB apart): the octave is 12 dB apart.
  double level[2];
  for (int i = 0; i < 2; ++i) {
    EffectRig rig(kKind, alone(false));
    const std::vector<float> out = rig.render(sine(2.0, 196.0, i == 0 ? 0.4f : 0.1f, kRate));
    level[i] = lineDb(out, 48000, out.size(), 98.0);
  }
  EXPECT_NEAR(level[0] - level[1], 12.04, 0.8);

  // A note that decays takes its octave with it: 200 ms after the note ends
  // the octave is more than 30 dB down (the release is 40 ms).
  EffectRig rig(kKind, alone(false));
  const std::vector<float> in = withSilence(sine(1.0, 196.0, 0.3f, kRate), 24000);
  const std::vector<float> out = rig.render(in);
  EXPECT_GT(rmsOf(out, 40000, 48000), 0.1);
  EXPECT_LT(rmsOf(out, 48000 + 9600, 48000 + 12000), 0.02 * rmsOf(out, 40000, 48000));
}

TEST(AnalogOctaveTest, SilenceAndNoiseStayQuietAndBounded) {
  EffectRig rig(kKind);
  EXPECT_EQ(peakOf(rig.renderSilence(24000)), 0.0f);
  // Hiss under the comparator's floor (-68 dBFS) is not divided.
  EffectRig hiss(kKind, alone(false));
  EXPECT_LT(peakOf(hiss.render(noise(1.0, 5u, 0.0003f, kRate))), 1.0e-4f);
  // Louder noise switches the comparator at random: the output is bounded by
  // the level of the playing, not runaway.
  EffectRig loud(kKind);
  expectCleanSignal(loud.render(noise(2.0, 6u, 0.5f, kRate)), 1.0f, "noise");
}

TEST(AnalogOctaveTest, TheTrackedFrequencyReadingFollowsTheNote) {
  DirectKind kind(kKind, {}, Layout::Mono);
  EXPECT_FLOAT_EQ(kind.engine().getReading(0), 0.0f);
  for (const double hz : {110.0, 196.0, 330.0}) {
    kind.engine().reset();
    kind.render(harmonicTone(0.6, hz, 0.3f));
    EXPECT_NEAR(kind.engine().getReading(0), hz, hz * 0.01) << hz;
  }
  kind.engine().reset();
  EXPECT_FLOAT_EQ(kind.engine().getReading(0), 0.0f);
}

TEST(AnalogOctaveTest, MonoStereoAndDualMonoEachDivideTheirOwnNote) {
  for (const Layout layout : {Layout::Mono, Layout::Stereo}) {
    EffectRig rig(kKind, alone(false), rigFor(layout));
    const Stereo out = rig.renderBoth(harmonicTone(2.0, 196.0, 0.3f));
    EXPECT_NEAR(centsBetween(fundamentalOf(out.left, 48000, 98.0), 98.0), 0.0, 1.0) << layoutName(layout);
    EXPECT_NEAR(centsBetween(fundamentalOf(out.right, 48000, 98.0), 98.0), 0.0, 1.0) << layoutName(layout);
  }
  // Two instruments through one rig: a low E on the left, an A on the right.
  EffectRig rig(kKind, alone(false), rigFor(Layout::DualMono));
  const Stereo out = rig.render(Stereo{harmonicTone(2.0, 82.407, 0.3f), harmonicTone(2.0, 220.0, 0.3f)});
  EXPECT_NEAR(centsBetween(fundamentalOf(out.left, 48000, 41.2035), 41.2035), 0.0, 1.5);
  EXPECT_NEAR(centsBetween(fundamentalOf(out.right, 48000, 110.0), 110.0), 0.0, 1.0);
  EXPECT_LT(lineDb(out.left, 48000, out.left.size(), 110.0), lineDb(out.left, 48000, out.left.size(), 41.2035) - 30.0);
  EXPECT_LT(lineDb(out.right, 48000, out.right.size(), 41.2035), lineDb(out.right, 48000, out.right.size(), 110.0) - 30.0);
}

TEST(AnalogOctaveTest, ItIsMonophonicOnlyOneOfTwoNotesAtOnceIsDivided) {
  // The documented limit of a divider: it follows one zero-crossing train. A
  // fifth (196 and 294 Hz): the octave of the lower note comes out, the octave
  // of the other is not there at all.
  EffectRig single(kKind, alone(false));
  const std::vector<float> one = single.render(sine(2.0, 196.0, 0.3f, kRate));
  const double clean = lineDb(one, 48000, one.size(), 98.0);
  std::vector<float> two = sine(2.0, 196.0, 0.15f, kRate);
  const std::vector<float> fifth = sine(2.0, 293.66, 0.15f, kRate);
  for (size_t i = 0; i < two.size(); ++i)
    two[i] += fifth[i];
  EffectRig chord(kKind, alone(false));
  const std::vector<float> out = chord.render(two);
  expectCleanSignal(out, 1.0f, "chord");
  std::cout << "[ OCTAVE   ] two notes: octave of the lower " << juce::String(lineDb(out, 48000, out.size(), 98.0) - clean, 1)
            << " dB, of the upper " << juce::String(lineDb(out, 48000, out.size(), 146.83) - clean, 1)
            << " dB against a single note's octave" << std::endl;
  EXPECT_LT(lineDb(out, 48000, out.size(), 146.83), clean - 30.0);
}

TEST(AnalogOctaveTest, ItHasNoLatencyAndTheDryPassesStraightThrough) {
  EffectRig rig(kKind);
  EXPECT_EQ(rig.latencySamples(), 0);
  EXPECT_NEAR(rig.tailSeconds(), 0.35, 1.0e-9);
  EffectRig reference{juce::String()};
  const std::vector<float> ir = captureImpulseResponse(rig, 0.2);
  const int origin = onsetIndex(captureImpulseResponse(reference, 0.05));
  EXPECT_EQ(onsetIndex(ir), origin);
}

TEST(AnalogOctaveTest, TheTrackingKnobTradesNoiseImmunityForEagerness) {
  // Whatever the tracking knob, a loud clean note is divided exactly.
  const std::vector<float> loud = sine(2.0, 196.0, 0.3f, kRate);
  for (const double tracking : {0.0, 50.0, 100.0}) {
    Params p = alone(false);
    p.push_back({"tracking", tracking});
    EffectRig rig(kKind, p);
    const std::vector<float> out = rig.render(loud);
    EXPECT_NEAR(centsBetween(fundamentalOf(out, 48000, 98.0), 98.0), 0.0, 1.0) << "tracking " << tracking;
  }
}

// The two mix laws (BlockMixLaw, BlockKindInfo::mixLaw) in the shared audio
// loop: what the mix knob does to the dry and the wet signal, on its own and
// together with Out Gain, the bypass fade and a spillover release.
//
// The kinds measured are gains, so with the wet gain at +1 and then at -1 the
// rig's level against the empty rig is (dry + wet) and (dry - wet): half the
// sum and half the difference are the two coefficients, exactly.
#include "effects/effect_test_helpers.h"
#include "test_block_kinds.h"

#include <gtest/gtest.h>

using namespace fxtest;

namespace {

constexpr double kRate = 48000.0;

// The rig's gain against the empty rig for the same input: the projection of
// one output on the other (everything around the chain is linear).
double gainAgainst(const std::vector<float>& out, const std::vector<float>& reference,
                   size_t skip = 4800) {
  double cross = 0.0, power = 0.0;
  for (size_t i = skip; i < out.size(); ++i) {
    cross += static_cast<double>(out[i]) * static_cast<double>(reference[i]);
    power += static_cast<double>(reference[i]) * static_cast<double>(reference[i]);
  }
  return cross / power;
}

struct Shares {
  double dry;
  double wet;
};

// `positive` and `negative` set the kind's wet gain to +1 and -1.
Shares measureShares(const char* kind, Params params, const Params& positive,
                     const Params& negative, double mix, double outputGain = 0.5) {
  params.emplace_back("mix", mix);
  params.emplace_back("outputGain", outputGain);
  const std::vector<float> in = noise(0.5, 17u, 0.25f, kRate);
  EffectRig reference{juce::String()};
  const std::vector<float> dry = reference.render(in);

  double level[2] = {0.0, 0.0};
  int index = 0;
  for (const Params* polarity : {&positive, &negative}) {
    Params all = params;
    all.insert(all.end(), polarity->begin(), polarity->end());
    EffectRig rig(kind, all);
    level[index++] = gainAgainst(rig.render(in), dry);
  }
  return {0.5 * (level[0] + level[1]), 0.5 * (level[0] - level[1])};
}

const double kMixes[] = {0.0, 0.35, 0.5, 0.75, 1.0};

}  // namespace

TEST(MixLawTest, CrossfadeIsOneMinusMixAndMix) {
  // The loop every kind before the effects runs (utility.gain; polarity is
  // its wet gain of -1), and the general loop a spillover kind takes, with
  // its switch on and off: the same law, to the same numbers.
  struct Case {
    const char* kind;
    Params base, positive, negative;
  };
  const Case cases[] = {
      {"utility.gain", {}, {{"phaseInvert", 0.0}}, {{"phaseInvert", 1.0}}},
      {"test.spill", {{"spillover", 1.0}}, {{"wetGain", 1.0}}, {{"wetGain", -1.0}}},
      {"test.spill", {{"spillover", 0.0}}, {{"wetGain", 1.0}}, {{"wetGain", -1.0}}},
  };
  for (const Case& c : cases)
    for (const double mix : kMixes) {
      SCOPED_TRACE(std::string(c.kind) + " at mix " + std::to_string(mix));
      const Shares shares = measureShares(c.kind, c.base, c.positive, c.negative, mix);
      EXPECT_NEAR(shares.dry, 1.0 - mix, 1e-5);
      EXPECT_NEAR(shares.wet, mix, 1e-5);
    }
}

TEST(MixLawTest, DryPreservingKeepsTheDrySignalAtUnityUpToHalfWay) {
  //   mix   0     0.35  0.5   0.75  1
  //   dry   1     1     1     0.5   0      min(1, 2(1 - mix))
  //   wet   0     0.7   1     1     1      min(1, 2 mix)
  const double expectedDry[] = {1.0, 1.0, 1.0, 0.5, 0.0};
  const double expectedWet[] = {0.0, 0.7, 1.0, 1.0, 1.0};
  for (const double spillover : {1.0, 0.0})
    for (size_t i = 0; i < std::size(kMixes); ++i) {
      SCOPED_TRACE("mix " + std::to_string(kMixes[i]) + ", spillover " + std::to_string(spillover));
      const Shares shares = measureShares("test.spill.dry", {{"spillover", spillover}},
                                          {{"wetGain", 1.0}}, {{"wetGain", -1.0}}, kMixes[i]);
      EXPECT_NEAR(shares.dry, expectedDry[i], 1e-5);
      EXPECT_NEAR(shares.wet, expectedWet[i], 1e-5);
    }

  // The shares as the loop computes them, at the corners.
  EXPECT_FLOAT_EQ(blockMixDryShare(BlockMixLaw::DryPreserving, 0.5f), 1.0f);
  EXPECT_FLOAT_EQ(blockMixWetShare(BlockMixLaw::DryPreserving, 0.5f), 1.0f);
  EXPECT_FLOAT_EQ(blockMixDryShare(BlockMixLaw::DryPreserving, 1.0f), 0.0f);
  EXPECT_FLOAT_EQ(blockMixWetShare(BlockMixLaw::DryPreserving, 0.0f), 0.0f);
  EXPECT_FLOAT_EQ(blockMixDryShare(BlockMixLaw::Crossfade, 0.35f), 0.65f);
  EXPECT_FLOAT_EQ(blockMixWetShare(BlockMixLaw::Crossfade, 0.35f), 0.35f);
}

TEST(MixLawTest, OutGainScalesBothSharesUnderEitherLaw) {
  // +6 dB out (knob 0.625) is the block's output fader: it moves the dry
  // share of the mix with the wet one.
  const double outGain = juce::Decibels::decibelsToGain(6.0);
  const Shares crossfade = measureShares("test.spill", {}, {{"wetGain", 1.0}}, {{"wetGain", -1.0}},
                                         0.35, 0.625);
  EXPECT_NEAR(crossfade.dry, 0.65 * outGain, 1e-4);
  EXPECT_NEAR(crossfade.wet, 0.35 * outGain, 1e-4);
  const Shares preserving = measureShares("test.spill.dry", {}, {{"wetGain", 1.0}},
                                          {{"wetGain", -1.0}}, 0.35, 0.625);
  EXPECT_NEAR(preserving.dry, 1.0 * outGain, 1e-4);
  EXPECT_NEAR(preserving.wet, 0.7 * outGain, 1e-4);
}

TEST(MixLawTest, ADryPreservingBlockSwitchedOffIsTheEmptyRig) {
  for (const Layout layout : {Layout::Mono, Layout::Stereo, Layout::DualMono}) {
    expectBypassNull("test.spill.dry", {{"mix", 0.35}, {"wetGain", 2.0}}, rigFor(layout));
    expectBypassNull("test.spill.dry", {{"mix", 0.75}, {"spillover", 0.0}}, rigFor(layout));
  }
  expectBypassNull("delay.simple", {{"mix", 0.35}});
}

TEST(MixLawTest, TheBypassFadeGlidesFromTheMixedLevelToDryWithoutLeavingTheRange) {
  // Fully on at mix 0.35 a dry-preserving pass-through plays 1 + 0.7 times
  // the signal; switched off it plays 1. With spillover the input closes
  // (1 + 0.7u); without it the wet term fades ((1 + 0.7t) with Out Gain at
  // unity). Either way the level falls from 1.7 to 1 and never below.
  for (const double spillover : {1.0, 0.0}) {
    SCOPED_TRACE(spillover);
    EffectRig rig("test.spill.dry", {{"mix", 0.35}, {"spillover", spillover}, {"tailMs", 100.0}});
    EffectRig reference{juce::String()};
    const auto levelOver = [&](int blocks) {
      const std::vector<float> in = makeSine(blocks * 256, 440.0, 0.25f);
      return rmsOf(rig.render(in)) / rmsOf(reference.render(in));
    };
    levelOver(20);
    EXPECT_NEAR(levelOver(20), 1.7, 1e-3);
    rig.setEnabled(false);
    double previous = 1.7;
    for (int block = 0; block < 5; ++block) {
      const double level = levelOver(1);
      EXPECT_LT(level, previous + 0.01);
      EXPECT_GT(level, 0.99);
      previous = level;
    }
    EXPECT_NEAR(levelOver(20), 1.0, 1e-3);
    rig.setEnabled(true);
    levelOver(6);
    EXPECT_NEAR(levelOver(20), 1.7, 1e-3);
  }
}

TEST(MixLawTest, AReleasedDryPreservingDelayKeepsDryAtUnityAndItsTailAtItsLevel) {
  // delay.simple at mix 0.35: dry at unity, echoes at 0.7. What the rig adds
  // to the dry signal must be 0.7 times the delay's own wet output, while it
  // plays and, unchanged, after it is switched off and rings out. The input
  // stops at the switch, so the wet output is that of the kind run alone on
  // the same burst followed by silence.
  const Params settings = {{"timeMs", 150.0}, {"feedback", 70.0}, {"toneHz", 20000.0}};
  Params rigSettings = settings;
  rigSettings.emplace_back("mix", 0.35);
  EffectRig rig("delay.simple", rigSettings);
  EffectRig reference{juce::String()};
  DirectKind alone("delay.simple", settings);

  const int burstFrames = 60 * 256;
  const std::vector<float> burst = makeSine(burstFrames, 330.0, 0.25f);
  const std::vector<float> silence(static_cast<size_t>(2.0 * kRate), 0.0f);

  std::vector<float> out = rig.render(burst);
  std::vector<float> dry = reference.render(burst);
  rig.setEnabled(false);
  const std::vector<float> outTail = rig.render(silence), dryTail = reference.render(silence);
  out.insert(out.end(), outTail.begin(), outTail.end());
  dry.insert(dry.end(), dryTail.begin(), dryTail.end());
  const std::vector<float> wet = alone.render(withSilence(burst, static_cast<int>(silence.size())));

  // Compared as levels over stretches of 50 ms: the rig's DC blocker shifts
  // the waveform by a hair, not its level.
  const size_t hop = 2400;
  for (size_t from = hop; from + hop <= out.size(); from += hop) {
    double added = 0.0, expected = 0.0;
    for (size_t i = from; i < from + hop; ++i) {
      const double a = static_cast<double>(out[i]) - static_cast<double>(dry[i]);
      added += a * a;
      expected += 0.49 * static_cast<double>(wet[i]) * static_cast<double>(wet[i]);
    }
    if (expected < 1e-7 * static_cast<double>(hop))
      continue;  // the tail has faded into the DC blocker's own residue
    EXPECT_NEAR(std::sqrt(added / expected), 1.0, 0.01)
        << "at " << static_cast<double>(from) / kRate << " s (switched off at "
        << burstFrames / kRate << " s)";
  }
  // There was a tail to compare: half a second after the switch it is still
  // a tenth of the burst.
  EXPECT_GT(peakOf(wet, static_cast<size_t>(burstFrames) + 24000), 0.02f);
}

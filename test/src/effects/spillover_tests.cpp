// Tail spillover in the shared audio loop
// (TONE3000Processor::processChainOnBuffer, ChainBlock.h), watched through
// test.spill: a pass-through kind that declares the capability and counts
// how the chain treats it. What a tail SOUNDS like is tested where there is
// one (simple_delay_tests.cpp); this is the state machine: the input closes,
// the block keeps running for its reported tail, then it is reset and costs
// nothing.
#include "effects/effect_test_helpers.h"
#include "test_block_kinds.h"

#include <gtest/gtest.h>

using namespace fxtest;

namespace {

constexpr int kBlock = 256;
constexpr double kRate = 48000.0;

TestSpillProcessor::Seen& seen() { return TestSpillProcessor::seen(); }

std::vector<float> tone(int blocks) { return makeSine(blocks * kBlock, 440.0, 0.25f); }

// Renders `blocks` blocks of the tone and returns how many of them the kind
// was processed for.
int processedBlocks(EffectRig& rig, int blocks) {
  const int before = seen().processCalls.load();
  rig.render(tone(blocks));
  return seen().processCalls.load() - before;
}

}  // namespace

TEST(SpilloverTest, AReleasedBlockRunsForItsTailWithItsInputClosedThenGoesIdle) {
  EffectRig rig("test.spill", {{"tailMs", 500.0}});
  seen().clear();

  EXPECT_EQ(processedBlocks(rig, 20), 20);
  EXPECT_NEAR(seen().lastInputPeak.load(), 0.25f, 0.01f);
  EXPECT_EQ(seen().resetCalls.load(), 0);

  rig.setEnabled(false);
  // The input fade is 25 ms (1200 frames): closed within five blocks, while
  // the rig's own input keeps playing.
  EXPECT_EQ(processedBlocks(rig, 6), 6);
  EXPECT_EQ(seen().lastInputPeak.load(), 0.0f) << "the released block still hears its input";
  EXPECT_EQ(seen().resetCalls.load(), 0);

  // Processed until the tail (500 ms) and the fades around it have passed:
  // 25200 frames of release, then 1200 more for the tail gain to close, so
  // 104 blocks in all; then not at all.
  const int ringing = 6 + processedBlocks(rig, 200);
  EXPECT_GE(ringing, 99);
  EXPECT_LE(ringing, 106);
  EXPECT_EQ(seen().resetCalls.load(), 1) << "the engine is reset once when it goes idle";
  EXPECT_EQ(processedBlocks(rig, 50), 0) << "an idle block costs nothing";
  EXPECT_EQ(seen().resetCalls.load(), 1);

  // Back on: processed again, the input open again.
  rig.setEnabled(true);
  EXPECT_EQ(processedBlocks(rig, 10), 10);
  EXPECT_NEAR(seen().lastInputPeak.load(), 0.25f, 0.01f);
}

TEST(SpilloverTest, WithTheSwitchOffABypassIsThePlainFade) {
  EffectRig rig("test.spill", {{"tailMs", 500.0}, {"spillover", 0.0}});
  seen().clear();
  processedBlocks(rig, 20);
  rig.setEnabled(false);
  // The 25 ms wet fade (five blocks), with the input left open as for any
  // other kind, then idle.
  const int fading = processedBlocks(rig, 3);
  EXPECT_EQ(fading, 3);
  EXPECT_NEAR(seen().lastInputPeak.load(), 0.25f, 0.01f);
  EXPECT_LE(fading + processedBlocks(rig, 100), 6);
  EXPECT_EQ(seen().resetCalls.load(), 1);
}

TEST(SpilloverTest, TheSwitchAndTheTailLengthAreReadWhileTheTailRings) {
  EffectRig rig("test.spill", {{"tailMs", 500.0}});
  seen().clear();
  processedBlocks(rig, 20);
  rig.setEnabled(false);
  EXPECT_EQ(processedBlocks(rig, 20), 20);

  // A longer tail asked for mid-release is honoured: 2 s is 97200 frames
  // with the fade, 380 blocks; 20 are gone.
  rig.set("tailMs", 2000.0);
  EXPECT_EQ(processedBlocks(rig, 300), 300);
  // Switching spillover off mid-tail closes it in one fade.
  rig.set("spillover", 0.0);
  EXPECT_LE(processedBlocks(rig, 100), 6);
  EXPECT_EQ(seen().resetCalls.load(), 1);

  // Switching it back on while idle does not wake the block up.
  rig.set("spillover", 1.0);
  EXPECT_EQ(processedBlocks(rig, 20), 0);
}

TEST(SpilloverTest, ReEnablingMidTailReopensTheInputAndStartsTheClockAgain) {
  EffectRig rig("test.spill", {{"tailMs", 500.0}});
  seen().clear();
  processedBlocks(rig, 20);
  rig.setEnabled(false);
  processedBlocks(rig, 60);  // most of the tail
  rig.setEnabled(true);
  processedBlocks(rig, 10);
  EXPECT_NEAR(seen().lastInputPeak.load(), 0.25f, 0.01f);
  EXPECT_EQ(seen().resetCalls.load(), 0) << "it never went idle";

  // Off again: a whole new tail, not the remainder of the first.
  rig.setEnabled(false);
  const int ringing = processedBlocks(rig, 300);
  EXPECT_GE(ringing, 99);
  EXPECT_LE(ringing, 106);
}

TEST(SpilloverTest, TheDrySignalPassesAtUnityThroughEveryTransition) {
  // The kind is a pass-through, so with the block on (fully wet) the output
  // is the dry signal, with it released it is the dry signal plus a tail of
  // nothing, and across the fades the closing input and the returning dry
  // share add up to exactly one. The rig must therefore equal the empty rig
  // the whole way, through each switch.
  EffectRig rig("test.spill", {{"tailMs", 300.0}, {"mix", 1.0}});
  EffectRig reference{juce::String()};
  seen().clear();

  std::vector<float> out, dry;
  const auto run = [&](int blocks) {
    const std::vector<float> in = noise(blocks * kBlock / kRate, 31u + static_cast<unsigned>(out.size()),
                                        0.25f, kRate);
    const std::vector<float> a = rig.render(in), b = reference.render(in);
    out.insert(out.end(), a.begin(), a.end());
    dry.insert(dry.end(), b.begin(), b.end());
  };
  run(20);
  rig.setEnabled(false);
  run(2);  // mid-fade
  rig.setEnabled(true);
  run(20);
  rig.proc.setBlockReleased(rig.blockId(), true);
  run(100);  // through the tail and into idle
  rig.proc.setBlockReleased(rig.blockId(), false);
  run(20);
  rig.set("mix", 0.4);
  run(20);
  rig.setEnabled(false);
  run(100);

  // 2e-5 is 0.0007 dB at this level: the fades' own float rounding, which
  // the plain bypass fade of any other kind shows as well. A fade that left
  // the dry share short, or doubled it, would be off by a hundredth or more.
  float worst = 0.0f;
  for (size_t i = 0; i < out.size(); ++i)
    worst = std::max(worst, std::abs(out[i] - dry[i]));
  EXPECT_LT(worst, 2.0e-5f) << "the dry level moved during a spillover transition";
  EXPECT_GT(seen().processCalls.load(), 100);
}

TEST(SpilloverTest, OutputGainComesOffTheDryShareInOneGlide) {
  // Fully wet at +6 dB out, a pass-through kind doubles the signal. Released,
  // the dry share is back at unity whatever the Out Gain says. In between the
  // level glides from one to the other without leaving that range (see the
  // spillover mix: 1 + 2t - t^2 for this case).
  EffectRig rig("test.spill", {{"tailMs", 300.0}, {"mix", 1.0}, {"outputGain", 0.625}});
  EffectRig reference{juce::String()};
  const float outGain = juce::Decibels::decibelsToGain(6.0f);

  // Level of the rig against the empty rig over a stretch of a steady sine.
  const auto levelOver = [&](int blocks) {
    const std::vector<float> in = tone(blocks);
    return static_cast<float>(rmsOf(rig.render(in)) / rmsOf(reference.render(in)));
  };
  levelOver(40);
  EXPECT_NEAR(levelOver(20), outGain, 0.01f);

  rig.setEnabled(false);
  float previous = outGain;
  for (int block = 0; block < 5; ++block) {
    const float level = levelOver(1);
    EXPECT_LT(level, previous + 0.02f) << "the level rose during the release";
    EXPECT_GT(level, 0.98f);
    previous = level;
  }
  EXPECT_NEAR(levelOver(20), 1.0f, 0.01f);

  // And back on, from the tail and later from idle.
  rig.setEnabled(true);
  levelOver(6);
  EXPECT_NEAR(levelOver(20), outGain, 0.01f);
  rig.setEnabled(false);
  levelOver(200);
  rig.setEnabled(true);
  previous = 1.0f;
  for (int block = 0; block < 5; ++block) {
    const float level = levelOver(1);
    EXPECT_GT(level, previous - 0.02f) << "the level dipped on the way back in";
    EXPECT_LT(level, outGain + 0.02f);
    previous = level;
  }
  EXPECT_NEAR(levelOver(20), outGain, 0.01f);
}

TEST(SpilloverTest, AReleaseLastsNoLongerThanTheTailCap) {
  // A kind may say it rings for ten minutes; the chain takes
  // kMaxBlockTailSeconds of it (BlockProcessor::reportedTailSeconds), for
  // the host's tail and for how long a released block keeps running.
  EffectRig rig("test.spill", {{"tailMs", 600000.0}});
  seen().clear();
  EXPECT_DOUBLE_EQ(rig.tailSeconds(), kMaxBlockTailSeconds);
  EXPECT_DOUBLE_EQ(rig.proc.getTailLengthSeconds(), kMaxBlockTailSeconds);
  DirectKind alone("test.spill", {{"tailMs", 600000.0}});
  EXPECT_DOUBLE_EQ(alone.engine().tailSeconds(), 600.0);
  EXPECT_DOUBLE_EQ(alone.tailSeconds(), kMaxBlockTailSeconds);

  processedBlocks(rig, 20);
  rig.setEnabled(false);
  // The cap plus the two 25 ms fades: 1,442,400 frames, 5635 blocks.
  const int capBlocks = static_cast<int>((kMaxBlockTailSeconds + 0.05) * kRate) / kBlock + 1;
  const int ringing = processedBlocks(rig, capBlocks + 400);
  EXPECT_GE(ringing, capBlocks - 6);
  EXPECT_LE(ringing, capBlocks + 2);
  EXPECT_EQ(seen().resetCalls.load(), 1);
  EXPECT_EQ(processedBlocks(rig, 50), 0);

  // Under the cap a tail is reported as it is, and nonsense is not passed on.
  rig.set("tailMs", 29000.0);
  EXPECT_DOUBLE_EQ(rig.tailSeconds(), 29.0);
}

// Splits, mixers and row feeds, checked by arithmetic:
// rows built from utility.gain, the test latency kind and the
// test spill kind, impulses, noise and sines in, exact relations out.
//
// "Exact" survives the output strip for the same reason as in
// grid_routing_tests.cpp: every relation is either two rigs running the same
// operations on the same samples, or a factor of two or minus one, which a
// linear filter in floating point carries without rounding. A factor of
// three is not carried exactly (every bus runs a DC blocker), so the
// three-way sum is held to the last bits instead.

#include "SplitJunction.h"
#include "grid_test_helpers.h"
#include "test_block_kinds.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kFrames = 4096;
constexpr int kLatency = TestLatencyProcessor::kDefaultLatency;

const char* kToGrid = R"({"to":"grid"})";

// One row from input 1 to output pair 2 (no tone stack, no master level), so
// what the row sends is what comes out.
void buildRow(GridRig& rig) {
  rig.run("grid.convert", kToGrid);
  rig.run("row.setInput", R"({"row":0,"source":"device","channel":1})");
  rig.run("row.setOutput", R"({"row":0,"target":"device","pair":2})");
}

void expectScaled(const std::vector<float>& out, const std::vector<float>& reference, float factor,
                  const char* what) {
  ASSERT_EQ(out.size(), reference.size());
  ASSERT_GT(gridPeak(reference), 0.01f) << what;
  for (size_t i = 0; i < out.size(); ++i)
    ASSERT_EQ(out[i], factor * reference[i]) << what << ", frame " << i;
}

// ---------------------------------------------------------------------------
// Split and mix
// ---------------------------------------------------------------------------

TEST(GridSplitTest, SplitAndMixWithEmptyRegionsIsExactlyTwice) {
  const GridChannels in{makeNoise(kFrames, 51, 0.2f), gridSilence(kFrames)};
  for (const char* format : {"stereo", "mono", "dual"}) {
    SCOPED_TRACE(format);
    const auto build = [&](GridRig& rig, bool split, bool invertB) {
      buildRow(rig);
      rig.run("row.setFormat", juce::String("{\"row\":0,\"format\":\"") + format + "\"}");
      rig.addKind("utility.gain", 0, 0, "gainDb=3");
      if (split) {
        const juce::var added = rig.run("split.add", R"({"row":0,"splitAt":2,"mixAt":6})");
        EXPECT_EQ(static_cast<int>(added["split"]["splitAt"]), 2);
        EXPECT_EQ(static_cast<int>(added["split"]["mixAt"]), 6);
        EXPECT_EQ(added["split"]["type"].toString(), "balance");
        if (invertB)
          rig.run("split.setParam", R"({"row":0,"param":"mix.invertB","value":1})");
      }
      rig.settle();
    };

    GridRig plain(2, 4);
    build(plain, false, false);
    const GridChannels reference = plain.process(in);

    GridRig doubled(2, 4);
    build(doubled, true, false);
    EXPECT_TRUE(static_cast<bool>(doubled.row(1)["routed"]));
    EXPECT_EQ(doubled.row(1)["source"].toString(), "split");
    EXPECT_EQ(doubled.row(1)["sink"].toString(), "mix");
    EXPECT_EQ(doubled.row(1)["runsAs"].toString(), format) << "the lower row took the row's format";
    const GridChannels twice = doubled.process(in);
    expectScaled(twice[2], reference[2], 2.0f, "left");
    if (std::string(format) != "mono") {
      expectScaled(twice[3], reference[3], 2.0f, "right");
    }
    EXPECT_EQ(doubled.proc.unresolvedPlanRefsForTesting(), 0);
    EXPECT_EQ(doubled.proc.stalePlanCallbacksForTesting(), 0);

    // The same path against itself with the polarity flipped: exact zeros.
    GridRig nulled(2, 4);
    build(nulled, true, true);
    const GridChannels none = nulled.process(in);
    EXPECT_EQ(gridPeak(none[2]), 0.0f);
    EXPECT_EQ(gridPeak(none[3]), 0.0f);
  }
}

// A latent block in one path: the mixer delays the other by exactly as much,
// which the null proves to the sample.
TEST(GridSplitTest, ALatentBlockInOnePathIsAlignedAtTheMixer) {
  const GridChannels in{makeNoise(kFrames, 52, 0.2f), gridSilence(kFrames)};
  for (const bool latentInLower : {false, true}) {
    SCOPED_TRACE(latentInLower ? "latent block in B" : "latent block in A");
    const auto build = [&](GridRig& rig, bool split, bool invertB) {
      buildRow(rig);
      if (split) {
        rig.run("split.add", R"({"row":0,"splitAt":2,"mixAt":6,"type":"ab"})");
        if (invertB)
          rig.run("split.setParam", R"({"row":0,"param":"mix.invertB","value":1})");
      }
      rig.addKind(TestLatencyProcessor::kKindId, split && latentInLower ? 1 : 0, 3);
      rig.settle();
    };

    GridRig plain(2, 4);
    build(plain, false, false);
    const GridChannels reference = plain.process(in);

    GridRig doubled(2, 4);
    build(doubled, true, false);
    const juce::var split = doubled.proc.getGridState()["splits"][0];
    EXPECT_EQ(static_cast<int>(split["alignment"]["a"]), latentInLower ? kLatency : 0);
    EXPECT_EQ(static_cast<int>(split["alignment"]["b"]), latentInLower ? 0 : kLatency);
    EXPECT_EQ(static_cast<int>(doubled.row(0)["latencySamples"]), kLatency);
    EXPECT_EQ(doubled.proc.getLatencySamples(), kLatency) << "the later path sets the latency";
    expectScaled(doubled.process(in)[2], reference[2], 2.0f, "sum");

    GridRig nulled(2, 4);
    build(nulled, true, true);
    EXPECT_EQ(gridPeak(nulled.process(in)[2]), 0.0f);
  }
}

// Row 0 into row 1 and, inside that, row 1 into row 2: three paths of
// unequal latency, one arrival, three times the signal.
TEST(GridSplitTest, ThreeWayNestArrivesTogetherAndSumsToThree) {
  const auto build = [](GridRig& rig, bool nest) {
    buildRow(rig);
    // Late in A, later in the middle path, nothing in the lowest one.
    rig.addKind(TestLatencyProcessor::kKindId, 0, 4);
    if (nest) {
      rig.run("split.add", R"({"row":0,"splitAt":1,"mixAt":7,"type":"ab"})");
      rig.run("split.add", R"({"row":1,"splitAt":2,"mixAt":6,"type":"ab"})");
      const juce::String late = rig.addKind(TestLatencyProcessor::kKindId, 1, 3);
      EXPECT_TRUE(rig.proc.setBlockParam(late.toStdString(), "latencySamples", 100.0));
      rig.addKind("utility.gain", 2, 4, "gainDb=0");
    } else {
      // The same total latency in one row, for the reference.
      const juce::String late = rig.addKind(TestLatencyProcessor::kKindId, 0, 5);
      EXPECT_TRUE(rig.proc.setBlockParam(late.toStdString(), "latencySamples", 100.0 - kLatency));
    }
    rig.settle();
  };

  GridRig plain(2, 4);
  build(plain, false);
  GridRig nested(2, 4);
  build(nested, true);
  ASSERT_EQ(nested.proc.getGridState()["splits"].size(), 2);
  ASSERT_EQ(nested.proc.getGridState()["groups"].size(), 1);
  EXPECT_EQ(nested.proc.getGridState()["groups"][0].size(), 3);
  EXPECT_EQ(nested.proc.getLatencySamples(), 100);
  std::printf("[plan] three-way nest\n%s", nested.proc.describeActivePlanForTesting().c_str());

  // An impulse: one arrival, at the reported latency, three times as high.
  const GridChannels impulse{gridImpulse(kFrames, 100, 0.25f), gridSilence(kFrames)};
  const GridChannels single = plain.process(impulse);
  const GridChannels tripled = nested.process(impulse);
  EXPECT_EQ(gridPeakIndex(single[2]), 200);
  EXPECT_EQ(gridPeakIndex(tripled[2]), 200);
  // (Not exactly an impulse: every output bus runs the 5 Hz DC blocker.)
  EXPECT_NEAR(tripled[2][200], 0.75f, 1e-3f);
  for (size_t i = 0; i < tripled[2].size(); ++i)
    ASSERT_NEAR(tripled[2][i], 3.0f * single[2][i], 1e-6f) << "frame " << i;

  // Each path on its own (the other two off at the mixers) lands on the
  // same frame.
  const auto only = [&](const char* inner, const char* outer) {
    GridRig rig(2, 4);
    build(rig, true);
    rig.run("split.setParam", outer);
    rig.run("split.setParam", inner);
    rig.settle();
    const GridChannels out = rig.process(impulse);
    EXPECT_EQ(gridPeakIndex(out[2]), 200);
    EXPECT_EQ(out[2], single[2]) << "one path alone is the row alone";
  };
  only(R"({"row":1,"param":"mix.levelB","value":-60})", R"({"row":0,"param":"mix.levelA","value":-60})");
  only(R"({"row":1,"param":"mix.levelA","value":-60})", R"({"row":0,"param":"mix.levelA","value":-60})");
  only(R"({"row":1,"param":"mix.levelB","value":0})", R"({"row":0,"param":"mix.levelB","value":-60})");
}

TEST(GridSplitTest, NoRejoinSendsTheTwoPathsToTwoOutputsInStep) {
  const GridChannels in{gridImpulse(kFrames, 100), gridSilence(kFrames)};
  GridRig rig(2, 4);
  rig.run("grid.convert", kToGrid);
  rig.run("row.setInput", R"({"row":0,"source":"device","channel":1})");
  rig.run("row.setFormat", R"({"row":0,"format":"mono"})");
  rig.run("row.setMix", R"({"row":0,"pan":0})");
  rig.run("split.add", R"({"row":0,"splitAt":2,"type":"ab"})");
  // Without an output the lower path leads nowhere and is not run.
  EXPECT_FALSE(static_cast<bool>(rig.row(1)["routed"]));
  EXPECT_FALSE(static_cast<bool>(rig.proc.getGridState()["splits"][0]["lowerActive"]));
  rig.run("row.setOutput", R"({"row":1,"target":"device","pair":2})");
  rig.run("row.setMix", R"({"row":1,"pan":0})");
  rig.addKind(TestLatencyProcessor::kKindId, 0, 4);  // in A, after the fork
  rig.addKind("utility.gain", 1, 5, "gainDb=-6");
  rig.settle();

  EXPECT_TRUE(static_cast<bool>(rig.row(1)["routed"]));
  EXPECT_EQ(rig.row(1)["sink"].toString(), "device");
  EXPECT_TRUE(rig.proc.getGridState()["splits"][0]["mixAt"].isVoid());
  EXPECT_EQ(static_cast<int>(rig.row(1)["padSamples"]), kLatency)
      << "two outputs of one split stay in step, also outside a plugin";
  const GridChannels out = rig.process(in);
  EXPECT_EQ(gridPeakIndex(out[0]), 100 + kLatency);
  EXPECT_EQ(gridPeakIndex(out[2]), 100 + kLatency);
  EXPECT_NEAR(out[2][100 + kLatency] / 0.5f, std::pow(10.0, -6.0 / 20.0), 1e-3);
  EXPECT_EQ(gridPeak(out[3]), 0.0f) << "a mono row panned hard left";
  EXPECT_EQ(rig.proc.getLatencySamples(), kLatency);

  // A split at the very end of the row: the finished row on a second pair.
  rig.run("split.move", R"({"row":0,"splitAt":8})");
  rig.settle();
  const GridChannels ended = rig.process(in);
  EXPECT_EQ(gridPeakIndex(ended[2]), 100 + kLatency);
  EXPECT_NEAR(ended[2][100 + kLatency], 0.5f, 1e-3f) << "row 1 runs no slot past boundary 8";
}

// ---------------------------------------------------------------------------
// Row feeds
// ---------------------------------------------------------------------------

TEST(GridFeedTest, ARowFeedIsTheSameAudioAsOneRow) {
  const GridChannels in{makeNoise(kFrames, 53, 0.2f), makeNoise(kFrames, 54, 0.2f)};
  const auto build = [](GridRig& rig, bool feed) {
    rig.run("grid.convert", kToGrid);
    rig.addKind("utility.gain", 0, 0, "gainDb=3");
    rig.addKind(TestLatencyProcessor::kKindId, 0, 2);
    if (feed) {
      rig.run("row.setOutput", R"({"row":1,"target":"device","pair":1})");
      rig.run("row.setOutput", R"({"row":0,"target":"row","toRow":1})");
    }
    rig.addKind("utility.gain", feed ? 1 : 0, feed ? 0 : 4, "gainDb=-4,pan=0.3");
    rig.settle();
  };
  GridRig one;
  build(one, false);
  GridRig fed;
  build(fed, true);
  EXPECT_EQ(fed.row(0)["sink"].toString(), "row");
  EXPECT_EQ(fed.row(1)["source"].toString(), "rows");
  EXPECT_EQ(fed.proc.getGridState()["feeds"].size(), 1);
  EXPECT_EQ(fed.proc.getGridState()["groups"][0].size(), 2);
  EXPECT_EQ(fed.proc.getLatencySamples(), kLatency);
  const GridChannels expected = one.process(in), out = fed.process(in);
  ASSERT_GT(gridPeak(expected[0]), 0.05f);
  EXPECT_EQ(out[0], expected[0]);
  EXPECT_EQ(out[1], expected[1]);
  EXPECT_EQ(fed.proc.unresolvedPlanRefsForTesting(), 0);
}

// The continuation rule: an amp that hands over to an amp in the row it
// feeds does so at calibrated level, exactly as inside one row.
TEST(GridFeedTest, AmpIntoAmpAcrossAFeedKeepsTheCalibratedHandOff) {
  const GridChannels in{makeNoise(kFrames, 55, 0.05f), makeNoise(kFrames, 55, 0.05f)};
  const juce::String ampA = testFile("a2-amp-test.nam").getFullPathName().replace("\\", "/");
  const juce::String ampB = testFile("a2-am-test-2.nam").getFullPathName().replace("\\", "/");
  const auto build = [&](GridRig& rig, bool feed, bool calibrate) {
    rig.proc.parameters.getParameter("calibrateInput")->setValueNotifyingHost(calibrate ? 1.0f : 0.0f);
    rig.run("grid.convert", kToGrid);
    if (feed) {
      rig.run("row.setOutput", R"({"row":1,"target":"device","pair":1})");
      rig.run("row.setOutput", R"({"row":0,"target":"row","toRow":1})");
    }
    rig.run("chain.addToneFromFiles", "{\"paths\":[\"" + ampA + "\"],\"row\":0,\"slot\":1}");
    rig.run("chain.addToneFromFiles", "{\"paths\":[\"" + ampB + "\"],\"row\":" +
                                          juce::String(feed ? 1 : 0) + ",\"slot\":" +
                                          juce::String(feed ? 2 : 5) + "}");
    rig.settle();
  };
  for (const bool calibrate : {true, false}) {
    SCOPED_TRACE(calibrate ? "calibration on" : "calibration off");
    GridRig one;
    build(one, false, calibrate);
    GridRig fed;
    build(fed, true, calibrate);
    EXPECT_NE(fed.proc.describeActivePlanForTesting().find("then row 1 [0, 8)"), std::string::npos)
        << fed.proc.describeActivePlanForTesting();
    const GridChannels expected = one.process(in), out = fed.process(in);
    ASSERT_GT(gridPeak(expected[0]), 1e-3f);
    EXPECT_EQ(out[0], expected[0]);
    EXPECT_EQ(out[1], expected[1]);
  }
  // And the rule does something: with calibration on, the first amp alone
  // in its row (nothing after it) is not what it is before a second amp.
  GridRig alone;
  alone.proc.parameters.getParameter("calibrateInput")->setValueNotifyingHost(1.0f);
  alone.run("grid.convert", kToGrid);
  alone.run("chain.addToneFromFiles", "{\"paths\":[\"" + ampA + "\"],\"row\":0,\"slot\":1}");
  alone.settle();
  EXPECT_EQ(alone.proc.describeActivePlanForTesting().find("then row"), std::string::npos);
}

// The same across a rejoin: an amp in the lower path of a split whose upper
// row holds an amp after the mix point.
TEST(GridFeedTest, TheContinuationCrossesARejoin) {
  GridRig rig;
  rig.run("grid.convert", kToGrid);
  rig.run("split.add", R"({"row":0,"splitAt":1,"mixAt":4})");
  rig.settle();
  const std::string plan = rig.proc.describeActivePlanForTesting();
  // The lower row's segment looks on into row 0 from the mix point.
  EXPECT_NE(plan.find("RunBlocks row 1 [1, 4) on row1 ch0-1, stereo, scan [1, 4) then row 0 [4, 8)"),
            std::string::npos)
      << plan;
  // The upper row never looks into the lower one.
  EXPECT_NE(plan.find("RunBlocks row 0 [1, 4) on row0 ch0-1, stereo, scan [0, 8), latency"),
            std::string::npos)
      << plan;
}

TEST(GridFeedTest, FeedersOfUnequalLatencyArriveTogether) {
  const GridChannels in{gridImpulse(kFrames, 100, 0.25f), gridSilence(kFrames)};
  GridRig rig(2, 4);
  rig.run("grid.convert", kToGrid);
  rig.run("row.setInput", R"({"row":0,"source":"device","channel":1})");
  rig.run("row.setInput", R"({"row":1,"source":"device","channel":1})");
  rig.run("row.setOutput", R"({"row":2,"target":"device","pair":2})");
  rig.run("row.setOutput", R"({"row":0,"target":"row","toRow":2})");
  rig.run("row.setOutput", R"({"row":1,"target":"row","toRow":2})");
  rig.addKind(TestLatencyProcessor::kKindId, 0, 3);
  rig.addKind(TestLatencyProcessor::kKindId, 2, 0);
  rig.settle();

  const juce::var grid = rig.proc.getGridState();
  ASSERT_EQ(grid["feeds"].size(), 2);
  EXPECT_EQ(static_cast<int>(grid["feeds"][0]["padSamples"]), 0);
  EXPECT_EQ(static_cast<int>(grid["feeds"][1]["padSamples"]), kLatency);
  EXPECT_EQ(grid["groups"].size(), 1);
  EXPECT_EQ(rig.proc.getLatencySamples(), 2 * kLatency);
  // The two feeders are forked, then the fed row runs.
  EXPECT_NE(rig.proc.describeActivePlanForTesting().find("Fork"), std::string::npos);

  // One arrival, twice one feeder's (the tail is the bus's 5 Hz DC blocker).
  const size_t arrival = static_cast<size_t>(100 + 2 * kLatency);
  const GridChannels out = rig.process(in);
  EXPECT_NEAR(out[2][arrival], 0.5f, 1e-3f);
  for (size_t i = 0; i < out[2].size(); ++i)
    if (i != arrival) {
      ASSERT_LT(std::abs(out[2][i]), 1e-3f) << "frame " << i;
    }

  // A feeding row goes in at its volume, with its mute and polarity.
  rig.run("row.setMix", R"({"row":1,"invert":true})");
  rig.settle();
  EXPECT_EQ(gridPeak(rig.process(in)[2]), 0.0f);
  rig.run("row.setMix", R"({"row":1,"invert":false,"mute":true})");
  rig.settle();
  EXPECT_NEAR(rig.process(in)[2][arrival], 0.25f, 1e-3f);
  // Solo on a feeding row keeps the row it feeds audible.
  rig.run("row.setMix", R"({"row":1,"mute":false,"solo":true})");
  rig.settle();
  EXPECT_NEAR(rig.process(in)[2][arrival], 0.5f, 1e-3f);
}

// ---------------------------------------------------------------------------
// Formats and splitter types
// ---------------------------------------------------------------------------

TEST(GridSplitTest, StereoSplitSendsLeftToAAndRightToB) {
  const GridChannels leftOnly{makeNoise(kFrames, 56, 0.2f), gridSilence(kFrames)};
  const GridChannels rightOnly{gridSilence(kFrames), makeNoise(kFrames, 57, 0.2f)};
  GridRig rig(2, 4);
  rig.run("grid.convert", kToGrid);
  rig.run("row.setInput", R"({"row":0,"source":"device","channel":1,"stereo":true})");
  rig.run("split.add", R"({"row":0,"splitAt":0})");
  rig.run("row.setOutput", R"({"row":1,"target":"device","pair":2})");
  rig.run("split.setParam", R"({"row":0,"param":"split.stereo","value":1})");
  rig.settle();

  GridChannels out = rig.process(leftOnly);
  EXPECT_GT(gridPeak(out[0]), 0.05f);
  EXPECT_EQ(out[0], out[1]) << "A carries the left channel on both sides";
  EXPECT_EQ(gridPeak(out[2]), 0.0f) << "B is the right channel, which is silent";
  EXPECT_EQ(gridPeak(out[3]), 0.0f);

  rig.settle();
  out = rig.process(rightOnly);
  EXPECT_GT(gridPeak(out[2]), 0.05f);
  EXPECT_EQ(out[2], out[3]);

  // A dual-mono row forces the lower row to dual mono, whatever it is set
  // to, and ignores the stereo split: its channels are two signals.
  rig.run("row.setFormat", R"({"row":1,"format":"mono"})");
  rig.run("row.setFormat", R"({"row":0,"format":"dual"})");
  rig.settle();
  EXPECT_EQ(rig.row(1)["format"].toString(), "mono");
  EXPECT_EQ(rig.row(1)["runsAs"].toString(), "dual");
  out = rig.process(leftOnly);
  EXPECT_GT(gridPeak(out[2]), 0.05f) << "B has the left signal on its left";
  EXPECT_EQ(gridPeak(out[3]), 0.0f);
  EXPECT_EQ(gridPeak(out[1]), 0.0f);
}

// An amp carries as many voices as the format its row runs in asks for,
// also when that format is forced on the row by the split above it.
TEST(GridSplitTest, NamVoicesFollowTheFormatASplitForces) {
  GridRig rig;
  juce::ValueTree state("ChainSnapshot");
  juce::ValueTree lane("ChainBlocks");
  lane.appendChild(makeNamBlockTree("blk-amp", 1, 100), nullptr);
  state.appendChild(lane, nullptr);
  rig.proc.restoreFromTree(state);
  ASSERT_TRUE(waitForChainLoaded(rig.proc));
  rig.run("grid.convert", kToGrid);
  rig.run("row.setInput", R"({"row":0,"source":"device","channel":1,"stereo":true})");
  rig.run("grid.moveBlock", R"({"blockId":"blk-amp","row":1,"slot":3})");
  rig.settle();
  EXPECT_EQ(rig.proc.namEngineVoiceCount("blk-amp"), 1);

  // A split from a stereo row: the lower row takes its format, one voice.
  rig.run("split.add", R"({"row":0,"splitAt":2,"mixAt":6})");
  rig.settle();
  EXPECT_EQ(rig.proc.namEngineVoiceCount("blk-amp"), 1);
  // The upper row goes dual mono: the lower row has to, and the amp gets
  // its second voice though nobody touched its row.
  rig.run("row.setFormat", R"({"row":0,"format":"dual"})");
  rig.settle();
  EXPECT_EQ(rig.row(1)["format"].toString(), "stereo");
  EXPECT_EQ(rig.row(1)["runsAs"].toString(), "dual");
  EXPECT_EQ(rig.proc.namEngineVoiceCount("blk-amp"), 2);
  const GridChannels out = rig.process({makeNoise(kFrames, 60, 0.1f), gridSilence(kFrames)});
  // Left and right stay apart through both paths: the right side is what
  // the same rig makes of silence on both inputs (the amp's own idle
  // output), to the sample.
  rig.settle();
  const GridChannels idle = rig.process({gridSilence(kFrames), gridSilence(kFrames)});
  EXPECT_GT(gridPeak(out[0]), 10.0f * gridPeak(out[1]));
  EXPECT_EQ(out[1], idle[1]);
  // Without the split the row is its own again.
  rig.run("split.remove", R"({"row":0})");
  rig.settle();
  EXPECT_EQ(rig.proc.namEngineVoiceCount("blk-amp"), 1);
  ASSERT_TRUE(rig.proc.undoChain());
  rig.settle();
  EXPECT_EQ(rig.proc.namEngineVoiceCount("blk-amp"), 2);
}

// A mono row under a stereo one folds on the way down and is placed at the
// centre on the way back.
TEST(GridSplitTest, MixedFormatsFollowTheJunctionsChannelRules) {
  const std::vector<float> noise = makeNoise(kFrames, 58, 0.2f);
  const auto build = [](GridRig& rig, const char* upper, const char* lower, bool split) {
    rig.run("grid.convert", kToGrid);
    rig.run("row.setInput", R"({"row":0,"source":"device","channel":1,"stereo":true})");
    rig.run("row.setOutput", R"({"row":0,"target":"device","pair":2})");
    rig.run("row.setFormat", juce::String("{\"row\":0,\"format\":\"") + upper + "\"}");
    if (split) {
      rig.run("split.add", R"({"row":0,"splitAt":2,"mixAt":5})");
      rig.run("row.setFormat", juce::String("{\"row\":1,\"format\":\"") + lower + "\"}");
    }
    rig.settle();
  };
  for (const auto& [upper, lower] : {std::pair<const char*, const char*>{"stereo", "mono"},
                                     {"mono", "stereo"}}) {
    SCOPED_TRACE(std::string(upper) + " over " + lower);
    GridRig plain(2, 4), joined(2, 4);
    build(plain, upper, lower, false);
    build(joined, upper, lower, true);
    EXPECT_EQ(joined.row(1)["runsAs"].toString(), lower);
    // The same signal on both inputs: the fold keeps its level, and the sum
    // is twice the row alone.
    const GridChannels reference = plain.process({noise, noise});
    const GridChannels out = joined.process({noise, noise});
    expectScaled(out[2], reference[2], 2.0f, "left");
  }
}

TEST(GridSplitTest, CrossoverRecombinesFlat) {
  const auto build = [](GridRig& rig, bool split) {
    buildRow(rig);
    if (split) {
      rig.run("split.add", R"({"row":0,"splitAt":2,"mixAt":6,"type":"crossover"})");
      rig.run("split.setParam", R"({"row":0,"param":"split.crossoverHz","value":700})");
    }
    rig.settle();
  };
  GridRig plain(2, 4), crossed(2, 4);
  build(plain, false);
  build(crossed, true);
  const int frames = 48000 / 2;
  for (const double hz : {60.0, 250.0, 700.0, 1900.0, 6000.0}) {
    const GridChannels in{makeSine(frames, hz, 0.25f), gridSilence(frames)};
    plain.settle();
    crossed.settle();
    const GridChannels reference = plain.process(in), out = crossed.process(in);
    // Low plus high is an allpass: the level is the row's own (not twice
    // it), whatever the frequency. Measured once the filters have settled.
    const size_t from = static_cast<size_t>(frames / 2);
    const double level = goertzelPower(out[2].data() + from, out[2].size() - from, hz);
    const double wanted = goertzelPower(reference[2].data() + from, reference[2].size() - from, hz);
    EXPECT_NEAR(10.0 * std::log10(level / wanted), 0.0, 0.01) << hz << " Hz";
  }
  // Each band alone is 6 dB down at the crossover frequency.
  crossed.run("split.setParam", R"({"row":0,"param":"mix.levelB","value":-60})");
  const GridChannels in{makeSine(frames, 700.0, 0.25f), gridSilence(frames)};
  plain.settle();
  crossed.settle();
  const GridChannels reference = plain.process(in), low = crossed.process(in);
  const size_t from = static_cast<size_t>(frames / 2);
  EXPECT_NEAR(10.0 * std::log10(goertzelPower(low[2].data() + from, low[2].size() - from, 700.0) /
                                goertzelPower(reference[2].data() + from,
                                              reference[2].size() - from, 700.0)),
              -6.02, 0.05);
}

// ---------------------------------------------------------------------------
// Parameters and edits under audio
// ---------------------------------------------------------------------------

TEST(GridSplitTest, SplitterAndMixerParameterMovesRampWithoutAStep) {
  const int total = 48000 / 4;
  const int half = (total / 2 / 64) * 64;
  const std::vector<float> sine = makeSine(total, 997.0, 0.25f);
  const GridChannels firstHalf{std::vector<float>(sine.begin(), sine.begin() + half),
                               gridSilence(half)};
  const GridChannels secondHalf{std::vector<float>(sine.begin() + half, sine.end()),
                                gridSilence(total - half)};
  struct Case {
    const char* name;
    const char* args;
    double startGain, endGain;
  };
  // An empty split and mix is 2x; each move takes one path's share away or
  // changes it.
  const Case cases[] = {
      {"mixer B off", R"({"row":0,"param":"mix.levelB","value":-60})", 2.0, 1.0},
      {"mixer mute", R"({"row":0,"param":"mix.mute","value":1})", 2.0, 0.0},
      {"mixer level", R"({"row":0,"param":"mix.level","value":-6})", 2.0, 2.0 * std::pow(10.0, -0.3)},
      {"mixer B polarity", R"({"row":0,"param":"mix.invertB","value":1})", 2.0, 0.0},
      {"splitter balance to A", R"({"row":0,"param":"split.balance","value":-1})", 2.0, 1.0},
      {"splitter balance to B", R"({"row":0,"param":"split.balance","value":1})", 2.0, 1.0},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    GridRig reference(2, 4), moved(2, 4);
    buildRow(reference);
    reference.settle();
    buildRow(moved);
    moved.run("split.add", R"({"row":0,"splitAt":2,"mixAt":6})");
    moved.settle();
    const GridChannels refA = reference.process(firstHalf);
    const GridChannels refB = reference.process(secondHalf);
    expectScaled(moved.process(firstHalf)[2], refA[2], 2.0f, "before the move");
    const int revision = static_cast<int>(moved.proc.getCurrentChainRevision());
    moved.run("split.setParam", c.args);  // no settle: the audio is running
    EXPECT_EQ(moved.proc.validateActivePlanForTesting(), "");
    juce::ignoreUnused(revision);
    const GridChannels outB = moved.process(secondHalf);

    double previous = c.startGain, largestStep = 0.0, last = c.startGain;
    for (size_t i = 0; i < refB[2].size(); ++i) {
      if (std::abs(refB[2][i]) < 0.05f)
        continue;
      const double gain = static_cast<double>(outB[2][i]) / refB[2][i];
      largestStep = std::max(largestStep, std::abs(gain - previous));
      previous = gain;
      last = gain;
    }
    EXPECT_LT(largestStep, 0.1) << "the gain jumped";
    EXPECT_NEAR(last, c.endGain, 4e-3);
    const size_t early = 96;  // 2 ms in: still on its way
    if (std::abs(refB[2][early]) >= 0.05f && std::abs(c.startGain - c.endGain) > 0.1) {
      const double gain = static_cast<double>(outB[2][early]) / refB[2][early];
      EXPECT_GT(std::abs(gain - c.endGain), 0.2 * std::abs(c.startGain - c.endGain));
    }
  }
}

// Lower-row slots outside the split's range are kept, reported and not run.
TEST(GridSplitTest, LowerRowSlotsOutsideTheRangeDoNotProcess) {
  GridRig rig(2, 4);
  buildRow(rig);
  rig.run("split.add", R"({"row":0,"splitAt":2,"mixAt":6})");
  const juce::String outside = rig.addKind(TestSpillProcessor::kKindId, 1, 0);
  rig.settle();
  juce::var row = rig.row(1);
  EXPECT_FALSE(static_cast<bool>(row["active"][0])) << "before the fork";
  EXPECT_EQ(static_cast<int>(row["activeSlots"][0]), 2);
  EXPECT_EQ(static_cast<int>(row["activeSlots"][1]), 6);

  const GridChannels in{makeNoise(kFrames, 59, 0.2f), gridSilence(kFrames)};
  TestSpillProcessor::seen().clear();
  rig.process(in);
  EXPECT_EQ(TestSpillProcessor::seen().processCalls.load(), 0) << "a slot outside the range ran";
  const juce::var meters = rig.run("meters.get");
  EXPECT_LE(static_cast<double>(meters["blocks"][juce::Identifier(outside)]["in"]), -60.0);

  // Moved inside the range, the same block runs.
  rig.run("grid.moveBlock", "{\"blockId\":\"" + outside + "\",\"row\":1,\"slot\":3}");
  rig.settle();
  row = rig.row(1);
  EXPECT_TRUE(static_cast<bool>(row["active"][3]));
  TestSpillProcessor::seen().clear();
  rig.process(in);
  EXPECT_GT(TestSpillProcessor::seen().processCalls.load(), 0);

  // And outside again when the split moves away from it.
  rig.run("split.move", R"({"row":0,"splitAt":4,"mixAt":8})");
  rig.settle();
  EXPECT_FALSE(static_cast<bool>(rig.row(1)["active"][3]));
  TestSpillProcessor::seen().clear();
  rig.process(in);
  EXPECT_EQ(TestSpillProcessor::seen().processCalls.load(), 0);
  // The split meters: what went down each path and what reached the mixer.
  const juce::var levels = rig.run("meters.get")["splits"];
  ASSERT_EQ(levels.size(), 1);
  EXPECT_GT(static_cast<double>(levels[0]["split"][0]), -40.0);
  EXPECT_GT(static_cast<double>(levels[0]["split"][1]), -40.0);
  EXPECT_GT(static_cast<double>(levels[0]["mix"][1]), -40.0);
}

// Adding, moving, retyping and removing a split while audio runs: the edit
// lands inside the chain mute, so the output never jumps by more than a
// sine of its level does from one sample to the next.
TEST(GridSplitTest, StructuralSplitEditsUnderAudioLeaveNoDiscontinuity) {
  constexpr int kLiveBlock = 64;
  GridRig rig(2, 4, 48000.0, kLiveBlock);
  buildRow(rig);
  rig.addKind("utility.gain", 0, 1, "gainDb=-3");
  rig.addKind("utility.gain", 1, 3, "gainDb=2");
  rig.settle();

  const double hz = 375.0;  // 128 samples per cycle: whole cycles per block pair
  std::atomic<bool> running{true};
  std::atomic<int> blocks{0};
  std::vector<float> captured;
  captured.reserve(48000 * 20);
  std::thread audio([&] {
    juce::AudioBuffer<float> buffer(4, kLiveBlock);
    juce::MidiBuffer midi;
    juce::int64 position = 0;
    while (running.load()) {
      buffer.clear();
      for (int i = 0; i < kLiveBlock; ++i)
        buffer.setSample(0, i, 0.25f * static_cast<float>(std::sin(2.0 * kPi * hz * (position + i) / 48000.0)));
      position += kLiveBlock;
      rig.proc.processBlock(buffer, midi);
      if (captured.size() + kLiveBlock <= captured.capacity())
        captured.insert(captured.end(), buffer.getReadPointer(2), buffer.getReadPointer(2) + kLiveBlock);
      blocks.fetch_add(1);
      // About real time, so the edit fade's waits see callbacks arrive.
      std::this_thread::sleep_for(std::chrono::microseconds(1000));
    }
  });
  const auto waitBlocks = [&](int count) {
    const int target = blocks.load() + count;
    while (blocks.load() < target)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  };

  waitBlocks(40);
  const char* edits[][2] = {
      {"split.add", R"({"row":0,"splitAt":2,"mixAt":6})"},
      {"split.setType", R"({"row":0,"type":"crossover"})"},
      {"split.move", R"({"row":0,"splitAt":1,"mixAt":4})"},
      {"split.setType", R"({"row":0,"type":"ab"})"},
      {"split.move", R"({"row":0,"rejoin":false})"},
      {"row.setOutput", R"({"row":1,"target":"device","pair":2})"},
      {"split.move", R"({"row":0,"mixAt":7})"},
      {"split.remove", R"({"row":0})"},
  };
  for (const auto& edit : edits) {
    rig.run(edit[0], edit[1]);
    EXPECT_EQ(rig.proc.validateActivePlanForTesting(), "") << edit[0];
    waitBlocks(60);
  }
  running.store(false);
  audio.join();
  EXPECT_EQ(rig.proc.unresolvedPlanRefsForTesting(), 0);
  EXPECT_EQ(rig.proc.stalePlanCallbacksForTesting(), 0);

  float peak = 0.0f, largestJump = 0.0f;
  for (size_t i = 1; i < captured.size(); ++i) {
    peak = std::max(peak, std::abs(captured[i]));
    largestJump = std::max(largestJump, std::abs(captured[i] - captured[i - 1]));
  }
  ASSERT_GT(peak, 0.1f);
  // A sine of amplitude `peak` moves by at most peak * 2 pi f / fs per
  // sample; the fade's own slope and the crossover's phase add a little.
  const float sineSlope = peak * static_cast<float>(2.0 * kPi * hz / 48000.0);
  std::printf("  split edits under audio: peak %g, largest sample-to-sample move %g (a sine of "
              "that peak moves %g)\n",
              static_cast<double>(peak), static_cast<double>(largestJump),
              static_cast<double>(sineSlope));
  EXPECT_LT(largestJump, 1.5f * sineSlope);
}

// ---------------------------------------------------------------------------
// Serial against parallel
// ---------------------------------------------------------------------------

// Split forks nest inside the group fork (and the NAM phase forks inside
// those at 2x): however the pool schedules them, the samples are the same.
TEST(GridSplitMultiCoreTest, SplitRigsSerialVersusParallel) {
  const int frames = 96 * 128;
  GridChannels in;
  for (unsigned ch = 0; ch < 2; ++ch)
    in.push_back(makeNoise(frames, 200 + ch, 0.1f));
  const juce::String ampA = testFile("a2-amp-test.nam").getFullPathName().replace("\\", "/");
  const juce::String ampB = testFile("a2-am-test-2.nam").getFullPathName().replace("\\", "/");
  const auto amp = [&](GridRig& rig, const juce::String& file, int row, int slot) {
    rig.run("chain.addToneFromFiles", "{\"paths\":[\"" + file + "\"],\"row\":" + juce::String(row) +
                                          ",\"slot\":" + juce::String(slot) + "}");
  };

  struct Shape {
    const char* name;
    int oversample;
    std::function<void(GridRig&)> build;
  };
  const Shape shapes[] = {
      {"two amps in parallel, rejoined", 1,
       [&](GridRig& rig) {
         rig.run("split.add", R"({"row":0,"splitAt":1,"mixAt":4,"type":"ab"})");
         amp(rig, ampA, 0, 1);
         amp(rig, ampB, 1, 2);
       }},
      {"three-way nest of amps, 2x", 2,
       [&](GridRig& rig) {
         rig.run("split.add", R"({"row":0,"splitAt":1,"mixAt":7})");
         rig.run("split.add", R"({"row":1,"splitAt":2,"mixAt":6})");
         amp(rig, ampA, 0, 3);
         amp(rig, ampB, 1, 3);
         amp(rig, ampA, 2, 4);
       }},
      // The slot budget's worst case with splits: three strands, each with
      // seven phase jobs, and the two forks that made them: 23 in flight.
      {"three-way nest of amps, 8x", 8,
       [&](GridRig& rig) {
         rig.run("split.add", R"({"row":0,"splitAt":1,"mixAt":7})");
         rig.run("split.add", R"({"row":1,"splitAt":2,"mixAt":6})");
         amp(rig, ampA, 0, 3);
         amp(rig, ampB, 1, 3);
         amp(rig, ampA, 2, 4);
       }},
      {"split beside an independent row, feeding a fourth", 1,
       [&](GridRig& rig) {
         rig.run("split.add", R"({"row":0,"splitAt":1})");
         rig.run("row.setOutput", R"({"row":1,"target":"row","toRow":3})");
         rig.run("row.setOutput", R"({"row":3,"target":"device","pair":2})");
         rig.run("row.setInput", R"({"row":2,"source":"device","channel":2})");
         rig.run("row.setOutput", R"({"row":2,"target":"device","pair":2})");
         amp(rig, ampA, 0, 3);
         amp(rig, ampB, 1, 3);
         amp(rig, ampA, 2, 0);
         amp(rig, ampB, 3, 6);
       }},
  };
  for (const Shape& shape : shapes) {
    SCOPED_TRACE(shape.name);
    const auto measure = [&](bool multiCore, GridChannels& out) {
      GridRig rig(2, 4, 48000.0, 128, shape.oversample);
      rig.proc.setMultiCoreEnabled(multiCore, /*persist=*/false);
      rig.run("grid.convert", kToGrid);
      shape.build(rig);
      rig.settle();
      EXPECT_NE(rig.proc.describeActivePlanForTesting().find("Fork"), std::string::npos);
      rig.process(in);  // warm-up
      rig.settle();
      const auto start = juce::Time::getHighResolutionTicks();
      out = rig.process(in);
      EXPECT_EQ(rig.proc.unresolvedPlanRefsForTesting(), 0);
      return juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - start);
    };
    GridChannels serialOut, parallelOut;
    const double serial = measure(false, serialOut);
    const double parallel = measure(true, parallelOut);
    EXPECT_GT(gridPeak(serialOut[0]), 1e-3f);
    for (size_t ch = 0; ch < serialOut.size(); ++ch)
      EXPECT_EQ(serialOut[ch], parallelOut[ch]) << "channel " << ch;
    std::printf("  %s, %.2f s audio: serial %.1f ms, parallel %.1f ms (%.2fx)\n", shape.name,
                frames / 48000.0, serial * 1000.0, parallel * 1000.0, serial / parallel);
  }
}

}  // namespace

// Grid mode with independent rows, checked by arithmetic rather than by ear:
// rows built from utility.gain and the
// test latency kind, impulses and noise in, exact relations out.
//
// What makes "exact" possible through the output strips: every relation
// here is either a comparison of two rigs that run the same operations on
// the same samples, or a factor of two or minus one, which a linear filter
// in floating point carries without rounding.

#include "PitchShift.h"
#include "grid_test_helpers.h"
#include "test_block_kinds.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr int kFrames = 4096;

const char* kToGrid = R"({"to":"grid"})";

// Row 0 from input 1 to Out 1/2 and row 1 from input 2 to Out 3/4, a gain
// block in each. `rows` is a bit mask of the rows that get an input.
void buildTwoRows(GridRig& rig, int rows) {
  rig.run("grid.convert", kToGrid);
  rig.run("row.setInput", (rows & 1) != 0 ? R"({"row":0,"source":"device","channel":1})"
                                          : R"({"row":0,"source":"none"})");
  if ((rows & 2) != 0) {
    rig.run("row.setInput", R"({"row":1,"source":"device","channel":2})");
    rig.run("row.setOutput", R"({"row":1,"target":"device","pair":2})");
  }
  rig.addKind("utility.gain", 0, 0, "gainDb=3");
  rig.addKind("utility.gain", 1, 0, "gainDb=-4");
  rig.settle();
}

TEST(GridRoutingTest, TwoRowsFromTwoInputsToTwoOutputsStayIndependent) {
  const GridChannels in{makeNoise(kFrames, 11, 0.2f), makeNoise(kFrames, 22, 0.2f)};

  GridRig both(2, 4);
  buildTwoRows(both, 3);
  const GridChannels out = both.process(in);
  EXPECT_GT(gridPeak(out[0]), 0.05f);
  EXPECT_GT(gridPeak(out[2]), 0.05f);
  EXPECT_EQ(both.proc.unresolvedPlanRefsForTesting(), 0);
  EXPECT_EQ(both.proc.stalePlanCallbacksForTesting(), 0);

  // Each row alone, in a rig of its own: the same samples, to the bit.
  GridRig first(2, 4);
  buildTwoRows(first, 1);
  const GridChannels firstOut = first.process(in);
  EXPECT_EQ(out[0], firstOut[0]);
  EXPECT_EQ(out[1], firstOut[1]);
  EXPECT_EQ(gridPeak(firstOut[2]), 0.0f);
  EXPECT_EQ(gridPeak(firstOut[3]), 0.0f);

  GridRig second(2, 4);
  buildTwoRows(second, 2);
  const GridChannels secondOut = second.process(in);
  EXPECT_EQ(out[2], secondOut[2]);
  EXPECT_EQ(out[3], secondOut[3]);
  EXPECT_EQ(gridPeak(secondOut[0]), 0.0f);

  // No leakage: an input that is silent leaves its row's output at exact
  // zero while the other row plays.
  GridRig quiet(2, 4);
  buildTwoRows(quiet, 3);
  const GridChannels onlyFirst = quiet.process({in[0], gridSilence(kFrames)});
  EXPECT_GT(gridPeak(onlyFirst[0]), 0.05f);
  EXPECT_EQ(gridPeak(onlyFirst[2]), 0.0f);
  EXPECT_EQ(gridPeak(onlyFirst[3]), 0.0f);
}

TEST(GridRoutingTest, TwoRowsOnOneBusSumExactly) {
  const GridChannels in{makeNoise(kFrames, 5, 0.2f), gridSilence(kFrames)};
  const auto build = [](GridRig& rig, bool second, bool invertSecond) {
    rig.run("grid.convert", kToGrid);
    rig.run("row.setInput", R"({"row":0,"source":"device","channel":1})");
    if (second) {
      rig.run("row.setInput", R"({"row":1,"source":"device","channel":1})");
      rig.run("row.setOutput", R"({"row":1,"target":"device","pair":1})");
      if (invertSecond)
        rig.run("row.setMix", R"({"row":1,"invert":true})");
    }
    rig.settle();
  };

  GridRig one;
  build(one, false, false);
  const GridChannels single = one.process(in);

  GridRig two;
  build(two, true, false);
  const GridChannels doubled = two.process(in);
  ASSERT_GT(gridPeak(single[0]), 0.05f);
  for (size_t i = 0; i < single[0].size(); ++i) {
    ASSERT_EQ(doubled[0][i], 2.0f * single[0][i]) << "frame " << i;
    ASSERT_EQ(doubled[1][i], 2.0f * single[1][i]) << "frame " << i;
  }

  // The same row against itself with the polarity flipped: exact zeros.
  GridRig null;
  build(null, true, true);
  const GridChannels nulled = null.process(in);
  EXPECT_EQ(gridPeak(nulled[0]), 0.0f);
  EXPECT_EQ(gridPeak(nulled[1]), 0.0f);
}

TEST(GridRoutingTest, RowsOfUnequalLatencyArriveTogetherOnOneBus) {
  const GridChannels in{gridImpulse(kFrames, 100), gridSilence(kFrames)};
  const auto build = [](GridRig& rig, bool second) {
    rig.run("grid.convert", kToGrid);
    rig.run("row.setInput", R"({"row":0,"source":"device","channel":1})");
    rig.addKind(TestLatencyProcessor::kKindId, 0, 2);
    if (second) {
      rig.run("row.setInput", R"({"row":1,"source":"device","channel":1})");
      rig.run("row.setOutput", R"({"row":1,"target":"device","pair":1})");
    }
    rig.settle();
  };

  GridRig latent;
  build(latent, false);
  const GridChannels single = latent.process(in);
  EXPECT_EQ(gridPeakIndex(single[0]), 100 + TestLatencyProcessor::kDefaultLatency);
  EXPECT_EQ(latent.proc.getLatencySamples(), TestLatencyProcessor::kDefaultLatency);

  // A second row without the latent block waits for the first: the two
  // impulses land on the same frame and the bus carries exactly twice one.
  GridRig pair;
  build(pair, true);
  EXPECT_EQ(static_cast<int>(pair.row(0)["latencySamples"]), TestLatencyProcessor::kDefaultLatency);
  EXPECT_EQ(static_cast<int>(pair.row(0)["padSamples"]), 0);
  EXPECT_EQ(static_cast<int>(pair.row(1)["latencySamples"]), 0);
  EXPECT_EQ(static_cast<int>(pair.row(1)["padSamples"]), TestLatencyProcessor::kDefaultLatency);
  EXPECT_EQ(pair.proc.getLatencySamples(), TestLatencyProcessor::kDefaultLatency);
  const GridChannels summed = pair.process(in);
  EXPECT_EQ(gridPeakIndex(summed[0]), 100 + TestLatencyProcessor::kDefaultLatency);
  for (size_t i = 0; i < single[0].size(); ++i)
    ASSERT_EQ(summed[0][i], 2.0f * single[0][i]) << "frame " << i;
}

TEST(GridRoutingTest, CrossBusPaddingIsThePluginRuleAndPerBusTheStandaloneRule) {
  const GridChannels in{gridImpulse(kFrames, 100), gridImpulse(kFrames, 100)};
  GridRig rig(2, 4);
  rig.run("grid.convert", kToGrid);
  rig.run("row.setInput", R"({"row":0,"source":"device","channel":1})");
  rig.addKind(TestLatencyProcessor::kKindId, 0, 0);
  rig.run("row.setInput", R"({"row":1,"source":"device","channel":2})");
  rig.run("row.setOutput", R"({"row":1,"target":"device","pair":2})");
  rig.settle();

  // Outside a plugin wrapper (the standalone rule): only rows that share an
  // output are aligned, so the second output is not held back.
  GridChannels out = rig.process(in);
  EXPECT_EQ(gridPeakIndex(out[0]), 100 + TestLatencyProcessor::kDefaultLatency);
  EXPECT_EQ(gridPeakIndex(out[2]), 100);
  EXPECT_EQ(static_cast<int>(rig.row(1)["padSamples"]), 0);

  // As a plugin: every output is as late as the reported latency says.
  rig.proc.setGridCrossBusPaddingForTesting(1);
  rig.settle();
  EXPECT_EQ(static_cast<int>(rig.row(1)["padSamples"]), TestLatencyProcessor::kDefaultLatency);
  EXPECT_EQ(rig.proc.getLatencySamples(), TestLatencyProcessor::kDefaultLatency);
  out = rig.process(in);
  EXPECT_EQ(gridPeakIndex(out[0]), 100 + TestLatencyProcessor::kDefaultLatency);
  EXPECT_EQ(gridPeakIndex(out[2]), 100 + TestLatencyProcessor::kDefaultLatency);

  rig.proc.setGridCrossBusPaddingForTesting(0);
  rig.settle();
  EXPECT_EQ(gridPeakIndex(rig.process(in)[2]), 100);
}

// One row on Out 3/4 (no tone stack, no master level): its send's gains can
// be read off the output against the same row at unity.
struct MixRig : GridRig {
  explicit MixRig(const char* format) : GridRig(2, 4) {
    run("grid.convert", kToGrid);
    run("row.setInput", R"({"row":1,"source":"device","channel":1,"stereo":true})");
    run("row.setOutput", R"({"row":1,"target":"device","pair":2})");
    run("row.setFormat", juce::String("{\"row\":1,\"format\":\"") + format + "\"}");
    settle();
  }
  GridChannels play(const char* mixJson) {
    run("row.setMix", mixJson);
    settle();  // snaps the smoothers: the gains are at rest from frame 0
    return process({makeNoise(kFrames, 7, 0.2f), makeNoise(kFrames, 8, 0.2f)});
  }
};

void expectGains(const GridChannels& out, const GridChannels& reference, double left, double right,
                 const char* what) {
  SCOPED_TRACE(what);
  const GridGainFit l = gridFitGain(out[2], reference[2]);
  const GridGainFit r = gridFitGain(out[3], reference[3]);
  EXPECT_NEAR(l.gain, left, 2e-6);
  EXPECT_NEAR(r.gain, right, 2e-6);
  EXPECT_LT(l.residual, 1e-5);
  EXPECT_LT(r.residual, 1e-5);
}

TEST(GridRoutingTest, MonoRowUsesTheConstantPowerLaw) {
  MixRig rig("mono");
  // Hard left at unity is the identity: the row itself on the left, nothing
  // on the right. The reference for both sides is that left channel.
  const GridChannels hardLeft = rig.play(R"({"row":1,"pan":0})");
  EXPECT_GT(gridPeak(hardLeft[2]), 0.05f);
  EXPECT_EQ(gridPeak(hardLeft[3]), 0.0f);
  const GridChannels reference{{}, {}, hardLeft[2], hardLeft[2]};

  const GridChannels hardRight = rig.play(R"({"row":1,"pan":1})");
  EXPECT_EQ(gridPeak(hardRight[2]), 0.0f);
  EXPECT_EQ(hardRight[3], hardLeft[2]);

  const double centre = std::cos(0.25 * kPi);
  expectGains(rig.play(R"({"row":1,"pan":0.5})"), reference, centre, centre, "centre");
  expectGains(rig.play(R"({"row":1,"pan":0.25})"), reference, std::cos(0.125 * kPi),
              std::sin(0.125 * kPi), "quarter left");
  const double minusSix = std::pow(10.0, -6.0 / 20.0);
  expectGains(rig.play(R"({"row":1,"pan":0.5,"volumeDb":-6})"), reference, centre * minusSix,
              centre * minusSix, "-6 dB");
  expectGains(rig.play(R"({"row":1,"pan":0.5,"volumeDb":0,"invert":true})"), reference, -centre,
              -centre, "inverted");
}

TEST(GridRoutingTest, StereoRowUsesTheBalanceLaw) {
  MixRig rig("stereo");
  const GridChannels reference = rig.play(R"({"row":1,"pan":0.5})");
  EXPECT_GT(gridPeak(reference[2]), 0.05f);
  EXPECT_GT(gridPeak(reference[3]), 0.05f);
  EXPECT_NE(reference[2], reference[3]) << "the pair is two different signals";

  expectGains(rig.play(R"({"row":1,"pan":0.75})"), reference, 0.5, 1.0, "three quarters right");
  expectGains(rig.play(R"({"row":1,"pan":0.25})"), reference, 1.0, 0.5, "three quarters left");
  expectGains(rig.play(R"({"row":1,"pan":1})"), reference, 0.0, 1.0, "hard right");
  expectGains(rig.play(R"({"row":1,"pan":0})"), reference, 1.0, 0.0, "hard left");
  const double plusSix = std::pow(10.0, 6.0 / 20.0);
  expectGains(rig.play(R"({"row":1,"pan":0.5,"volumeDb":6})"), reference, plusSix, plusSix, "+6 dB");
  expectGains(rig.play(R"({"row":1,"volumeDb":0,"invert":true})"), reference, -1.0, -1.0,
              "inverted");
  // The bottom of the volume range is off, not -60 dB.
  const GridChannels off = rig.play(R"({"row":1,"volumeDb":-60,"invert":false})");
  EXPECT_EQ(gridPeak(off[2]), 0.0f);
  EXPECT_EQ(gridPeak(off[3]), 0.0f);
}

TEST(GridRoutingTest, MuteAndSoloActAtTheOutputs) {
  const GridChannels in{makeNoise(kFrames, 3, 0.2f), makeNoise(kFrames, 4, 0.2f)};
  GridRig rig(2, 4);
  buildTwoRows(rig, 3);
  const GridChannels open = rig.process(in);

  const auto play = [&](const char* mixJson) {
    rig.run("row.setMix", mixJson);
    rig.settle();
    return rig.process(in);
  };

  GridChannels out = play(R"({"row":1,"mute":true})");
  EXPECT_EQ(gridPeak(out[2]), 0.0f);
  EXPECT_EQ(gridPeak(out[3]), 0.0f);
  EXPECT_EQ(out[0], open[0]) << "the other row is untouched";

  // Solo on row 1 silences row 0; the muted row stays muted even when it is
  // the soloed one.
  out = play(R"({"row":1,"mute":false,"solo":true})");
  EXPECT_EQ(gridPeak(out[0]), 0.0f);
  EXPECT_EQ(out[2], open[2]);
  out = play(R"({"row":0,"solo":true})");
  EXPECT_EQ(out[0], open[0]);
  EXPECT_EQ(out[2], open[2]);
  out = play(R"({"row":1,"solo":false})");
  EXPECT_EQ(out[0], open[0]);
  EXPECT_EQ(gridPeak(out[2]), 0.0f);
  out = play(R"({"row":0,"solo":false})");
  EXPECT_EQ(out[2], open[2]);

  // The row meters: what the row sent, per row; silent while muted.
  EXPECT_GT(static_cast<double>(rig.run("meters.get")["rows"][1]["out"][0]), -40.0);
  play(R"({"row":1,"mute":true})");
  const juce::var meters = rig.run("meters.get");
  EXPECT_LE(static_cast<double>(meters["rows"][1]["out"][0]), -60.0);
  EXPECT_GT(static_cast<double>(meters["rows"][1]["in"][0]), -40.0) << "the input meter is pre-mute";
  EXPECT_GT(static_cast<double>(meters["rows"][0]["out"][0]), -40.0);
  ASSERT_EQ(meters["outputs"].size(), 2);
  EXPECT_EQ(static_cast<int>(meters["outputs"][1]["pair"]), 2);
}

// A mix or trim move with audio running glides: the gain the output shows
// (against the same rig left alone) never jumps.
TEST(GridRoutingTest, MixAndTrimMovesRampWithoutAStep) {
  const int total = 48000 / 4;
  const int half = (total / 2 / 64) * 64;
  const GridChannels in{makeSine(total, 997.0, 0.25f), gridSilence(total)};
  const GridChannels firstHalf{std::vector<float>(in[0].begin(), in[0].begin() + half),
                               gridSilence(half)};
  const GridChannels secondHalf{std::vector<float>(in[0].begin() + half, in[0].end()),
                                gridSilence(total - half)};

  struct Case {
    const char* name;
    const char* command;
    const char* args;
    double endGain;
  };
  const Case cases[] = {
      {"volume to off", "row.setMix", R"({"row":1,"volumeDb":-60})", 0.0},
      {"mute", "row.setMix", R"({"row":1,"mute":true})", 0.0},
      {"volume +6 dB", "row.setMix", R"({"row":1,"volumeDb":6})", std::pow(10.0, 0.3)},
      {"trim -12 dB", "row.setInputStage", R"({"row":1,"trimDb":-12})", std::pow(10.0, -0.6)},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    const auto build = [](GridRig& rig) {
      rig.run("grid.convert", kToGrid);
      rig.run("row.setInput", R"({"row":1,"source":"device","channel":1})");
      rig.run("row.setOutput", R"({"row":1,"target":"device","pair":2})");
      rig.settle();
    };
    GridRig reference(2, 4), moved(2, 4);
    build(reference);
    build(moved);
    const GridChannels refA = reference.process(firstHalf);
    const GridChannels refB = reference.process(secondHalf);
    const GridChannels outA = moved.process(firstHalf);
    EXPECT_EQ(outA[2], refA[2]);
    moved.run(c.command, c.args);  // no settle: the audio is running
    EXPECT_TRUE(moved.proc.isGridMode());
    const GridChannels outB = moved.process(secondHalf);

    // The gain per sample, wherever the reference is far enough from a zero
    // crossing to divide by.
    double previous = 1.0, largestStep = 0.0, last = 1.0;
    for (size_t i = 0; i < refB[2].size(); ++i) {
      if (std::abs(refB[2][i]) < 0.05f)
        continue;
      const double gain = static_cast<double>(outB[2][i]) / refB[2][i];
      largestStep = std::max(largestStep, std::abs(gain - previous));
      previous = gain;
      last = gain;
    }
    // 20 ms is 960 samples; between two samples that can be divided the
    // gain moves by a handful of ramp steps at most.
    EXPECT_LT(largestStep, 0.05 * std::max(1.0, c.endGain)) << "the gain jumped";
    EXPECT_NEAR(last, c.endGain, 2e-3);
    // And the move did take time: 2 ms in, the gain is still on its way.
    const size_t early = 96;
    if (std::abs(refB[2][early]) >= 0.05f) {
      const double gain = static_cast<double>(outB[2][early]) / refB[2][early];
      EXPECT_GT(std::abs(gain - c.endGain), 0.2 * std::abs(1.0 - c.endGain));
    }
  }
}

TEST(GridRoutingTest, MissingInputIsSilentAndFlagged) {
  const GridChannels in{makeNoise(kFrames, 9, 0.2f), makeNoise(kFrames, 10, 0.2f)};
  GridRig plain;
  plain.run("grid.convert", kToGrid);
  plain.settle();
  const GridChannels expected = plain.process(in);

  // A second row on an input the two-channel device does not have: it runs
  // (from silence), adds nothing, and says so. The binding is kept.
  GridRig rig;
  rig.run("grid.convert", kToGrid);
  rig.run("row.setInput", R"({"row":1,"source":"device","channel":5})");
  rig.run("row.setOutput", R"({"row":1,"target":"device","pair":1})");
  rig.addKind("utility.gain", 1, 0, "gainDb=6");
  rig.settle();
  EXPECT_TRUE(static_cast<bool>(rig.row(1)["inputMissing"]));
  EXPECT_FALSE(static_cast<bool>(rig.row(0)["inputMissing"]));
  EXPECT_TRUE(static_cast<bool>(rig.row(1)["routed"]));
  EXPECT_EQ(static_cast<int>(rig.row(1)["input"]["channel"]), 5);
  const GridChannels out = rig.process(in);
  EXPECT_EQ(out[0], expected[0]);
  EXPECT_EQ(out[1], expected[1]);
  EXPECT_EQ(rig.proc.unresolvedPlanRefsForTesting(), 0);

  bool listed = false;
  const juce::var inputs = rig.proc.getGridState()["io"]["inputs"];
  for (int i = 0; i < inputs.size(); ++i)
    if (static_cast<int>(inputs[i]["channel"]) == 5) {
      listed = true;
      EXPECT_FALSE(static_cast<bool>(inputs[i]["available"]));
    }
  EXPECT_TRUE(listed);

  // On a device that has the channel the row plays, without any edit.
  rig.numIns = 6;
  rig.proc.setPlayConfigDetails(6, 2, rig.rate, rig.block);
  rig.settle();
  EXPECT_FALSE(static_cast<bool>(rig.row(1)["inputMissing"]));
  GridChannels wide(6, gridSilence(kFrames));
  wide[4] = makeNoise(kFrames, 12, 0.2f);
  EXPECT_GT(gridPeak(rig.process(wide)[0]), 0.05f);
}

TEST(GridRoutingTest, MissingOutputPairFallsBackToTheMainPairAndIsFlagged) {
  const GridChannels in{makeNoise(kFrames, 13, 0.2f), gridSilence(kFrames)};
  GridRig rig(2, 2);
  rig.run("grid.convert", kToGrid);
  rig.run("row.setInput", R"({"row":0,"source":"none"})");
  rig.run("row.setInput", R"({"row":1,"source":"device","channel":1})");
  rig.run("row.setOutput", R"({"row":1,"target":"device","pair":2})");
  rig.settle();

  EXPECT_TRUE(static_cast<bool>(rig.row(1)["outputFallback"]));
  EXPECT_EQ(static_cast<int>(rig.row(1)["output"]["pair"]), 2) << "the binding is not rewritten";
  const GridChannels folded = rig.process(in);
  EXPECT_GT(gridPeak(folded[0]), 0.05f) << "heard on Out 1/2 instead of dropped";
  EXPECT_NE(rig.proc.describeActivePlanForTesting().find("fallback"), std::string::npos);

  // The interface with four outputs: the same rig, now on Out 3/4.
  rig.numOuts = 4;
  rig.proc.setPlayConfigDetails(2, 4, rig.rate, rig.block);
  rig.settle();
  EXPECT_FALSE(static_cast<bool>(rig.row(1)["outputFallback"]));
  const GridChannels out = rig.process(in);
  EXPECT_EQ(gridPeak(out[0]), 0.0f);
  EXPECT_GT(gridPeak(out[2]), 0.05f);
}

// A one-channel device: the pair is folded to its one output, half of
// (L + R), so a centred row passes at its own level.
TEST(GridRoutingTest, MonoDeviceFoldsThePair) {
  const std::vector<float> noise = makeNoise(kFrames, 14, 0.2f);
  GridRig stereo(2, 2);
  stereo.run("grid.convert", kToGrid);
  stereo.settle();
  const GridChannels reference = stereo.process({noise, noise});

  GridRig mono(1, 1);
  mono.run("grid.convert", kToGrid);
  mono.settle();
  EXPECT_FALSE(static_cast<bool>(mono.row(0)["inputMissing"]));
  EXPECT_FALSE(static_cast<bool>(mono.row(0)["outputFallback"]));
  EXPECT_EQ(mono.process({noise})[0], reference[0]);
}

TEST(GridRoutingTest, RowFormatsCarryWhatTheySay) {
  const GridChannels leftOnly{makeNoise(kFrames, 15, 0.2f), gridSilence(kFrames)};
  const auto render = [&](const char* inputJson, const char* format) {
    GridRig rig;
    rig.run("grid.convert", kToGrid);
    rig.run("row.setInput", inputJson);
    rig.run("row.setFormat", juce::String("{\"row\":0,\"format\":\"") + format + "\"}");
    rig.addKind("utility.gain", 0, 0, "gainDb=2");
    rig.settle();
    return rig.process(leftOnly);
  };

  // Dual mono on the unsummed pair: the left take stays on the left.
  const GridChannels dual = render(R"({"row":0,"source":"device","channel":1,"stereo":true})", "dual");
  EXPECT_GT(gridPeak(dual[0]), 0.05f);
  EXPECT_EQ(gridPeak(dual[1]), 0.0f);
  // Stereo on the unsummed pair: likewise a real pair.
  const GridChannels stereo =
      render(R"({"row":0,"source":"device","channel":1,"stereo":true})", "stereo");
  EXPECT_EQ(gridPeak(stereo[1]), 0.0f);
  EXPECT_EQ(stereo[0], dual[0]);
  // Summed: half of the left input on both sides.
  const GridChannels summed =
      render(R"({"row":0,"source":"device","channel":1,"stereo":true,"fold":"sum"})", "stereo");
  EXPECT_EQ(summed[0], summed[1]);
  EXPECT_NEAR(gridFitGain(summed[0], stereo[0]).gain, 0.5, 1e-6);
  // Mono from input 2, which is silent: nothing. From input 1, centred:
  // both sides at -3 dB.
  EXPECT_EQ(gridPeak(render(R"({"row":0,"source":"device","channel":2})", "mono")[0]), 0.0f);
  const GridChannels mono = render(R"({"row":0,"source":"device","channel":1})", "mono");
  EXPECT_EQ(mono[0], mono[1]);
  EXPECT_NEAR(gridFitGain(mono[0], stereo[0]).gain, std::cos(0.25 * kPi), 1e-5);
}

TEST(GridRoutingTest, NamVoicesFollowTheRowFormatAcrossMoves) {
  GridRig rig;
  juce::ValueTree state("ChainSnapshot");
  juce::ValueTree lane("ChainBlocks");
  lane.appendChild(makeNamBlockTree("blk-amp", 1, 100), nullptr);
  state.appendChild(lane, nullptr);
  rig.proc.restoreFromTree(state);
  ASSERT_TRUE(waitForChainLoaded(rig.proc));
  rig.run("grid.convert", kToGrid);
  rig.settle();
  EXPECT_EQ(rig.proc.namEngineVoiceCount("blk-amp"), 1);

  // The row becomes dual mono: every amp in it gets a second voice.
  rig.run("row.setFormat", R"({"row":0,"format":"dual"})");
  rig.settle();
  EXPECT_EQ(rig.proc.namEngineVoiceCount("blk-amp"), 2);

  // Into a mono row and back: rebuilt each way through the usual loader.
  rig.run("row.setFormat", R"({"row":1,"format":"mono"})");
  rig.run("grid.moveBlock", R"({"blockId":"blk-amp","row":1,"slot":3})");
  rig.settle();
  EXPECT_EQ(rig.proc.gridSlotBlockId(1, 3), "blk-amp");
  EXPECT_EQ(rig.proc.namEngineVoiceCount("blk-amp"), 1);
  rig.run("grid.moveBlock", R"({"blockId":"blk-amp","row":0,"slot":0,"mode":"insert"})");
  rig.settle();
  EXPECT_EQ(rig.proc.gridSlotBlockId(0, 0), "blk-amp");
  EXPECT_EQ(rig.proc.namEngineVoiceCount("blk-amp"), 2);

  // Undo of the move puts it back in the mono row with one voice again
  // (reconciled by id across rows, then revoiced).
  ASSERT_TRUE(rig.proc.undoChain());
  rig.settle();
  EXPECT_EQ(rig.proc.gridSlotBlockId(1, 3), "blk-amp");
  EXPECT_EQ(rig.proc.namEngineVoiceCount("blk-amp"), 1);
}

// A strip that does not run the pitch shifter waits as long as the shifter
// makes the other strip late, so the reported latency holds for every row.
TEST(GridRoutingTest, UnshiftedStripsWaitForThePitchShifter) {
  GridRig rig(2, 4);
  rig.run("grid.convert", kToGrid);
  rig.run("row.setInput", R"({"row":0,"source":"device","channel":1})");
  rig.run("row.setInput", R"({"row":1,"source":"device","channel":2})");
  rig.run("row.setOutput", R"({"row":1,"target":"device","pair":2})");
  rig.settle();
  const GridChannels in{gridSilence(kFrames), gridImpulse(kFrames, 100)};
  EXPECT_EQ(gridPeakIndex(rig.process(in)[2]), 100) << "shifter off: nobody waits";

  rig.proc.parameters.getParameter("pitchEnabled")->setValueNotifyingHost(1.0f);
  rig.settle();
  const int pitchLatency = rig.proc.getLatencySamples();
  EXPECT_EQ(pitchLatency,
            PitchShift::latencySamples(PitchShift::kDefaultWindow, 48000.0));
  EXPECT_EQ(gridPeakIndex(rig.process(in)[2]), 100 + pitchLatency);

  // The row may ask for the shifter itself; then row 0 is the one padded.
  rig.run("row.setInputStage", R"({"row":0,"pitch":false})");
  rig.run("row.setInputStage", R"({"row":1,"pitch":true})");
  rig.settle();
  const GridChannels first{gridImpulse(kFrames, 100), gridSilence(kFrames)};
  EXPECT_EQ(gridPeakIndex(rig.process(first)[0]), 100 + pitchLatency);
}

// A host that hands over more frames than it promised: a grid rig takes
// them in slices of the promised size and loses nothing (stage 3b silenced
// every pair but the main one).
TEST(GridRoutingTest, OversizedHostBlocksAreSliced) {
  const GridChannels in{makeNoise(kFrames, 16, 0.2f), makeNoise(kFrames, 17, 0.2f)};
  GridRig reference(2, 4);
  buildTwoRows(reference, 3);
  const GridChannels expected = reference.process(in);

  GridRig rig(2, 4);
  buildTwoRows(rig, 3);
  rig.block = 320;  // prepared for 64
  const GridChannels out = rig.process(in);
  for (int ch = 0; ch < 4; ++ch)
    EXPECT_EQ(out[static_cast<size_t>(ch)], expected[static_cast<size_t>(ch)]) << "channel " << ch;
  EXPECT_EQ(rig.proc.unresolvedPlanRefsForTesting(), 0);
}

// The same two rows at 44.1 kHz with 2x oversampling: the boundary and the
// oversampler run two ports each way and the rows still do not meet.
TEST(GridRoutingTest, RowsStayIndependentThroughTheBoundaryAndTheOversampler) {
  const GridChannels in{makeNoise(kFrames, 18, 0.2f), makeNoise(kFrames, 19, 0.2f)};
  GridRig both(2, 4, 44100.0, 64, 2);
  buildTwoRows(both, 3);
  const GridChannels out = both.process(in);
  GridRig first(2, 4, 44100.0, 64, 2);
  buildTwoRows(first, 1);
  GridRig second(2, 4, 44100.0, 64, 2);
  buildTwoRows(second, 2);
  EXPECT_GT(gridPeak(out[0]), 0.05f);
  EXPECT_GT(gridPeak(out[2]), 0.05f);
  EXPECT_EQ(out[0], first.process(in)[0]);
  EXPECT_EQ(out[2], second.process(in)[2]);
  EXPECT_EQ(both.proc.unresolvedPlanRefsForTesting(), 0);
}

// A second strip and a second bus arriving while the device keeps running:
// no prepareToPlay follows the edit, so the boundary (44.1 kHz) has to be
// reopened for two ports each way by the edit itself, off the audio thread.
TEST(GridRoutingTest, PortCountChangeReopensTheBoundaryWithoutAPrepare) {
  const GridChannels in{makeNoise(kFrames, 23, 0.2f), makeNoise(kFrames, 24, 0.2f)};
  for (const int oversample : {1, 2}) {
    SCOPED_TRACE("oversampling x" + std::to_string(oversample));
    GridRig rig(2, 4, 44100.0, 64, oversample);
    const int latency = rig.proc.getLatencySamples();
    EXPECT_GT(latency, 0) << "the boundary is engaged";
    rig.process(in);  // the legacy rig, one port each way

    rig.run("grid.convert", kToGrid);
    rig.run("row.setInput", R"({"row":0,"source":"device","channel":1})");
    rig.run("row.setInput", R"({"row":1,"source":"device","channel":2})");
    rig.run("row.setOutput", R"({"row":1,"target":"device","pair":2})");
    EXPECT_TRUE(static_cast<bool>(rig.run("state.waitUntilLoaded")["loaded"]));
    EXPECT_EQ(rig.proc.getLatencySamples(), latency) << "the same for any number of ports";

    GridChannels out = rig.process(in);
    EXPECT_GT(gridPeak(out[0]), 0.05f);
    EXPECT_GT(gridPeak(out[2]), 0.05f) << "the second bus came up without a prepare";
    EXPECT_EQ(rig.proc.unresolvedPlanRefsForTesting(), 0);
    EXPECT_EQ(rig.proc.stalePlanCallbacksForTesting(), 0);
    // The rows are still apart: with input 2 gone quiet, all that is left
    // on the second bus is the 5 Hz DC blocker letting go of what the row
    // played before.
    out = rig.process({in[0], gridSilence(kFrames)});
    out = rig.process({in[0], gridSilence(kFrames)});
    EXPECT_GT(gridPeak(out[0]), 0.05f);
    EXPECT_LT(gridPeak(out[2]), 1e-3f);

    // And back to one bus.
    rig.run("row.setOutput", R"({"row":1,"target":"none"})");
    out = rig.process(in);
    out = rig.process(in);
    EXPECT_GT(gridPeak(out[0]), 0.05f);
    EXPECT_EQ(gridPeak(out[2]), 0.0f);
    EXPECT_EQ(rig.proc.unresolvedPlanRefsForTesting(), 0);
  }
}

// Four rows, each its own amp from its own input to its own output: the
// root fork. Serial and parallel give the same samples; the time is printed.
TEST(GridMultiCoreTest, FourIndependentRowsSerialVersusParallel) {
  const int frames = 96 * 128;
  GridChannels in;
  for (unsigned ch = 0; ch < 4; ++ch)
    in.push_back(makeNoise(frames, 100 + ch, 0.1f));
  const juce::String amp = testFile("a2-amp-test.nam").getFullPathName().replace("\\", "/");

  const auto measure = [&](bool multiCore, GridChannels& out) {
    GridRig rig(4, 8, 48000.0, 128);
    rig.proc.setMultiCoreEnabled(multiCore, /*persist=*/false);
    rig.run("grid.convert", kToGrid);
    for (int row = 0; row < 4; ++row) {
      rig.run("row.setInput", "{\"row\":" + juce::String(row) + ",\"source\":\"device\",\"channel\":" +
                                  juce::String(row + 1) + "}");
      rig.run("row.setOutput", "{\"row\":" + juce::String(row) + ",\"target\":\"device\",\"pair\":" +
                                   juce::String(row + 1) + "}");
      rig.run("chain.addToneFromFiles", "{\"paths\":[\"" + amp + "\"],\"row\":" +
                                            juce::String(row) + ",\"slot\":0}");
    }
    rig.settle();
    EXPECT_NE(rig.proc.describeActivePlanForTesting().find("Fork"), std::string::npos);
    rig.process(in);  // warm-up: fades settle, caches warm
    rig.settle();
    const auto start = juce::Time::getHighResolutionTicks();
    out = rig.process(in);
    const double seconds =
        juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - start);
    EXPECT_EQ(rig.proc.unresolvedPlanRefsForTesting(), 0);

    // The plan CPU value: four rows of about the same cost. When they really
    // run side by side one thread carries about a quarter, but on a loaded
    // machine the pool can run forks inline and the critical path is then the
    // whole plan, so only the bound that always holds is asserted.
    const juce::var cpu = rig.proc.getPlanCpu();
    EXPECT_GT(static_cast<double>(cpu["total"]), 0.0);
    EXPECT_LE(static_cast<double>(cpu["critical"]), 1.0001 * static_cast<double>(cpu["total"]));
    return seconds;
  };

  GridChannels serialOut, parallelOut;
  const double serial = measure(false, serialOut);
  const double parallel = measure(true, parallelOut);
  for (size_t ch = 0; ch < serialOut.size(); ++ch) {
    EXPECT_GT(gridPeak(serialOut[ch]), 1e-3f) << "channel " << ch;
    EXPECT_EQ(serialOut[ch], parallelOut[ch]) << "channel " << ch;
  }
  std::printf("  four NAM rows, %.2f s audio: serial %.1f ms, parallel %.1f ms (%.2fx)\n",
              frames / 48000.0, serial * 1000.0, parallel * 1000.0, serial / parallel);
}

// The plan CPU value (section 12): seeded, measured while the block runs,
// and left alone while it is bypassed.
TEST(GridPlanCpuTest, BlockCostIsMeasuredAndKeptWhileBypassed) {
  GridRig rig(2, 4);
  buildTwoRows(rig, 3);
  const std::string first = rig.proc.gridSlotBlockId(0, 0);
  const double seeded = rig.proc.blockCpuCostForTesting(first);
  EXPECT_GT(seeded, 0.0) << "a block has a cost before any audio has passed";
  EXPECT_LT(rig.proc.blockCpuCostForTesting("nope"), 0.0);

  const GridChannels in{makeNoise(kFrames, 20, 0.2f), makeNoise(kFrames, 21, 0.2f)};
  rig.process(in);
  const double measured = rig.proc.blockCpuCostForTesting(first);
  EXPECT_GT(measured, 0.0);
  EXPECT_LT(measured, 1.0);
  EXPECT_NE(measured, seeded) << "the audio thread refines the seed";

  juce::var cpu = rig.proc.getPlanCpu();
  const double total = cpu["total"];
  EXPECT_GT(total, 0.0);
  EXPECT_LE(static_cast<double>(cpu["critical"]), total);
  EXPECT_GT(static_cast<double>(cpu["rows"][0]), 0.0);
  EXPECT_GT(static_cast<double>(cpu["rows"][1]), 0.0);
  EXPECT_EQ(static_cast<double>(cpu["rows"][2]), 0.0);
  // Two rows side by side: one thread carries the dearer one, not both.
  EXPECT_LT(static_cast<double>(cpu["critical"]), total);

  // Bypassed, once its fade has landed, the block is not measured any more
  // and keeps its figure: switching it on later does not move the total.
  ASSERT_TRUE(rig.proc.setBlockParam(first, "enabled", 0.0));
  rig.process(in);
  const double bypassed = rig.proc.blockCpuCostForTesting(first);
  rig.process(in);
  rig.process(in);
  EXPECT_EQ(rig.proc.blockCpuCostForTesting(first), bypassed);
  EXPECT_GT(bypassed, 0.0);

  const juce::var meters = rig.run("meters.get");
  EXPECT_GT(static_cast<double>(meters["planCpu"]["total"]), 0.0);
  EXPECT_GT(static_cast<double>(rig.run("grid.get")["cpu"]["total"]), 0.0);
  EXPECT_GT(static_cast<double>(rig.row(0)["cpu"]), 0.0);
}

}  // namespace

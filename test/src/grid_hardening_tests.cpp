// The grid engine against what it was not built for: a pitch shifter that no
// input runs, a device restart racing an edit, a state file that holds
// numbers that are not numbers or a thousand splits, a measurement left
// running when the rig changes shape, a host that is slow to take a
// parameter change, and commands that name nothing to do.

#include "PitchShift.h"
#include "grid_test_helpers.h"
#include "test_block_kinds.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {

using InputMode = TONE3000Processor::InputMode;

constexpr int kFrames = 4096;
const char* kToGrid = R"({"to":"grid"})";
const char* kToLegacy = R"({"to":"legacy"})";

juce::MemoryBlock savedBytes(TONE3000Processor& proc) {
  juce::MemoryBlock saved;
  proc.getStateInformation(saved);
  return saved;
}

juce::ValueTree parseSaved(const juce::MemoryBlock& saved) {
  if (saved.getSize() <= 4 || std::string(static_cast<const char*>(saved.getData()), 4) != "T3KB")
    return {};
  return juce::ValueTree::readFromData(static_cast<const char*>(saved.getData()) + 4,
                                       saved.getSize() - 4);
}

juce::MemoryBlock frame(const juce::ValueTree& tree) {
  juce::MemoryBlock data;
  juce::MemoryOutputStream out(data, false);
  out.write("T3KB", 4);
  tree.writeToStream(out);
  out.flush();
  return data;
}

juce::ValueTree gainBlock(const char* id, double gainDb = 0.0) {
  return makeKindBlockTree(id, "utility.gain", "Gain",
                           {{"gainDb", gainDb}, {"pan", 0.0}, {"phaseInvert", 0.0}});
}

juce::ValueTree lanes(const std::vector<juce::ValueTree>& left,
                      const std::vector<juce::ValueTree>& right, bool stereo) {
  juce::ValueTree state("ChainSnapshot");
  if (stereo)
    state.setProperty("stereoEnabled", true, nullptr);
  juce::ValueTree leftLane("ChainBlocks");
  for (const auto& block : left)
    leftLane.appendChild(block, nullptr);
  state.appendChild(leftLane, nullptr);
  juce::ValueTree rightLane("RightChainBlocks");
  for (const auto& block : right)
    rightLane.appendChild(block, nullptr);
  state.appendChild(rightLane, nullptr);
  return state;
}

void load(GridRig& rig, const juce::ValueTree& state) {
  rig.proc.restoreFromTree(state);
  ASSERT_TRUE(waitForChainLoaded(rig.proc));
}

bool allFinite(const GridChannels& audio) {
  for (const auto& channel : audio)
    for (const float v : channel)
      if (!std::isfinite(v))
        return false;
  return true;
}

// Every number anywhere in the tree is a finite one.
bool treeIsFinite(const juce::ValueTree& tree, juce::String& where) {
  for (int i = 0; i < tree.getNumProperties(); ++i) {
    const juce::var value = tree.getProperty(tree.getPropertyName(i));
    if (value.isDouble() && !std::isfinite(static_cast<double>(value))) {
      where = tree.getType().toString() + "." + tree.getPropertyName(i).toString();
      return false;
    }
    if (value.isString() && (value.toString().containsIgnoreCase("nan") ||
                             value.toString().containsIgnoreCase("inf"))) {
      where = tree.getType().toString() + "." + tree.getPropertyName(i).toString();
      return false;
    }
  }
  for (int i = 0; i < tree.getNumChildren(); ++i)
    if (!treeIsFinite(tree.getChild(i), where))
      return false;
  return true;
}

// ---------------------------------------------------------------------------
// 1. Reported latency and the pitch shifter
// ---------------------------------------------------------------------------

// Two rows from two inputs to two outputs; which of them asks for the
// shifter is the test's to say.
void buildPitchRig(GridRig& rig, bool firstAsks, bool secondAsks) {
  rig.run("grid.convert", kToGrid);
  rig.run("row.setInput", R"({"row":0,"source":"device","channel":1})");
  rig.run("row.setInput", R"({"row":1,"source":"device","channel":2})");
  rig.run("row.setOutput", R"({"row":1,"target":"device","pair":2})");
  rig.run("row.setInputStage", firstAsks ? R"({"row":0,"pitch":true})" : R"({"row":0,"pitch":false})");
  rig.run("row.setInputStage", secondAsks ? R"({"row":1,"pitch":true})" : R"({"row":1,"pitch":false})");
}

// The delay an impulse on `input` shows on `output`, against what the host
// was told.
void expectArrivalMatchesReport(GridRig& rig, int input, int output, const char* what) {
  SCOPED_TRACE(what);
  rig.settle();
  GridChannels in(static_cast<size_t>(rig.numIns), gridSilence(kFrames));
  in[static_cast<size_t>(input)] = gridImpulse(kFrames, 100);
  const GridChannels out = rig.process(in);
  ASSERT_GT(gridPeak(out[static_cast<size_t>(output)]), 0.1f);
  EXPECT_EQ(gridPeakIndex(out[static_cast<size_t>(output)]), 100 + rig.proc.getLatencySamples());
}

TEST(GridPitchLatencyTest, NoStripRunsTheShifter) {
  const int pitchLatency = PitchShift::latencySamples(PitchShift::kDefaultWindow, 48000.0);
  GridRig rig(2, 4);
  buildPitchRig(rig, false, false);
  rig.settle();
  EXPECT_EQ(rig.proc.getLatencySamples(), 0);
  expectArrivalMatchesReport(rig, 0, 0, "shifter off");

  // Powered, and no row asks for it: the host is told the shifter's
  // latency (that follows the parameter), so every row waits it.
  rig.proc.parameters.getParameter("pitchEnabled")->setValueNotifyingHost(1.0f);
  rig.settle();
  EXPECT_EQ(rig.proc.getLatencySamples(), pitchLatency);
  expectArrivalMatchesReport(rig, 0, 0, "row 0");
  expectArrivalMatchesReport(rig, 1, 2, "row 1");
  EXPECT_NE(rig.proc.describeActivePlanForTesting().find("strip 0"), std::string::npos);
  EXPECT_EQ(rig.proc.describeActivePlanForTesting().find("pitch"), std::string::npos)
      << "no strip runs the shifter";

  // One row alone, and it does not ask either.
  rig.run("row.setOutput", R"({"row":1,"target":"none"})");
  expectArrivalMatchesReport(rig, 0, 0, "a single strip without the shifter");

  // Off again: nobody waits.
  rig.proc.parameters.getParameter("pitchEnabled")->setValueNotifyingHost(0.0f);
  rig.settle();
  EXPECT_EQ(rig.proc.getLatencySamples(), 0);
  expectArrivalMatchesReport(rig, 0, 0, "shifter off again");
}

TEST(GridPitchLatencyTest, ThePitchRowsInputIsMissingOnTheDevice) {
  const int pitchLatency = PitchShift::latencySamples(PitchShift::kDefaultWindow, 48000.0);
  GridRig rig(2, 4);
  rig.run("grid.convert", kToGrid);
  // The row that asks for the shifter sits on an input this device lacks:
  // it has no strip, so no strip runs the shifter.
  rig.run("row.setInput", R"({"row":0,"source":"device","channel":6})");
  rig.run("row.setInputStage", R"({"row":0,"pitch":true})");
  rig.run("row.setInput", R"({"row":1,"source":"device","channel":1})");
  rig.run("row.setOutput", R"({"row":1,"target":"device","pair":2})");
  rig.proc.parameters.getParameter("pitchEnabled")->setValueNotifyingHost(1.0f);
  rig.settle();
  ASSERT_TRUE(static_cast<bool>(rig.row(0)["inputMissing"]));
  EXPECT_EQ(rig.proc.getLatencySamples(), pitchLatency);
  expectArrivalMatchesReport(rig, 0, 2, "the row that is there");

  // On a device that has the input the same rig runs the shifter on it, and
  // the other row still waits.
  rig.numIns = 6;
  rig.proc.setPlayConfigDetails(6, 4, rig.rate, rig.block);
  rig.settle();
  EXPECT_FALSE(static_cast<bool>(rig.row(0)["inputMissing"]));
  EXPECT_EQ(rig.proc.getLatencySamples(), pitchLatency);
  expectArrivalMatchesReport(rig, 0, 2, "beside the shifter");
}

TEST(GridPitchLatencyTest, TogglingTheShifterPerRowUnderAudio) {
  constexpr int kLiveBlock = 64;
  const int pitchLatency = PitchShift::latencySamples(PitchShift::kDefaultWindow, 48000.0);
  GridRig rig(2, 4, 48000.0, kLiveBlock);
  buildPitchRig(rig, true, false);
  rig.proc.parameters.getParameter("pitchEnabled")->setValueNotifyingHost(1.0f);
  rig.settle();

  struct Step {
    const char* args;
    int measureInput, measureOutput;  // a row that does not run the shifter afterwards
  };
  const Step steps[] = {
      {R"({"row":0,"pitch":false})", 0, 0},  // nobody runs it
      {R"({"row":1,"pitch":true})", 0, 0},   // row 1 does
      {R"({"row":1,"pitch":false})", 1, 2},  // nobody again
      {R"({"row":0,"pitch":true})", 1, 2},   // row 0 does
  };
  for (const Step& step : steps) {
    SCOPED_TRACE(step.args);
    // The edit lands while callbacks run.
    std::atomic<bool> running{true};
    std::atomic<int> blocks{0};
    std::atomic<bool> finite{true};
    std::thread audio([&] {
      juce::AudioBuffer<float> buffer(4, kLiveBlock);
      juce::MidiBuffer midi;
      juce::int64 position = 0;
      while (running.load()) {
        buffer.clear();
        for (int ch = 0; ch < 2; ++ch)
          for (int i = 0; i < kLiveBlock; ++i)
            buffer.setSample(ch, i,
                             0.2f * static_cast<float>(std::sin(2.0 * kPi * 375.0 * (position + i) / 48000.0)));
        position += kLiveBlock;
        rig.proc.processBlock(buffer, midi);
        for (int ch = 0; ch < 4; ++ch)
          for (int i = 0; i < kLiveBlock; ++i)
            if (!std::isfinite(buffer.getSample(ch, i)))
              finite.store(false);
        blocks.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::microseconds(1000));
      }
    });
    const auto waitBlocks = [&](int count) {
      const int target = blocks.load() + count;
      while (blocks.load() < target)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    waitBlocks(30);
    rig.run("row.setInputStage", step.args);
    waitBlocks(60);
    running.store(false);
    audio.join();
    EXPECT_TRUE(finite.load());
    EXPECT_EQ(rig.proc.unresolvedPlanRefsForTesting(), 0);
    EXPECT_EQ(rig.proc.stalePlanCallbacksForTesting(), 0);
    EXPECT_EQ(rig.proc.getLatencySamples(), pitchLatency) << "the report follows the parameter";
    expectArrivalMatchesReport(rig, step.measureInput, step.measureOutput, "after the toggle");
  }
}

// ---------------------------------------------------------------------------
// 2. A device restart racing grid edits
// ---------------------------------------------------------------------------

TEST(GridPrepareRaceTest, PrepareToPlayAgainstPortCountEdits) {
  // 44.1 kHz: the boundary is engaged, and has to be open for exactly the
  // plan's strips and buses.
  GridRig rig(2, 4, 44100.0, 64);
  rig.run("grid.convert", kToGrid);
  rig.run("row.setInput", R"({"row":0,"source":"device","channel":1})");
  rig.settle();
  ASSERT_TRUE(rig.proc.boundaryMatchesPlanForTesting());

  std::atomic<bool> running{true};
  std::atomic<int> prepares{0};
  std::thread device([&] {
    while (running.load()) {
      rig.proc.prepareToPlay(44100.0, 64);
      // The map is read while the other thread compiles plans from it.
      const TONE3000Processor::DeviceChannelMap map = rig.proc.getDeviceChannelMap();
      EXPECT_EQ(map.numInputs, 2);
      EXPECT_EQ(map.inputNames.size(), 2);
      prepares.fetch_add(1);
    }
  });

  // One strip and one bus, two and two, back: every edit reopens the
  // boundary for other counts.
  int edits = 0;
  while (prepares.load() < 150 || edits < 60) {
    rig.run("row.setInput", R"({"row":1,"source":"device","channel":2})");
    rig.run("row.setOutput", R"({"row":1,"target":"device","pair":2})");
    EXPECT_TRUE(rig.proc.boundaryMatchesPlanForTesting());
    rig.run("row.setOutput", R"({"row":1,"target":"none"})");
    rig.run("row.setInput", R"({"row":1,"source":"none"})");
    EXPECT_TRUE(rig.proc.boundaryMatchesPlanForTesting());
    edits += 4;
    ASSERT_LT(edits, 100000) << "the device thread never got a turn";
  }
  rig.run("row.setInput", R"({"row":1,"source":"device","channel":2})");
  rig.run("row.setOutput", R"({"row":1,"target":"device","pair":2})");
  running.store(false);
  device.join();

  // No prepare follows: what the race left is what plays.
  EXPECT_TRUE(rig.proc.boundaryMatchesPlanForTesting());
  EXPECT_EQ(rig.proc.validateActivePlanForTesting(), "");
  const GridChannels in{makeNoise(kFrames, 91, 0.2f), makeNoise(kFrames, 92, 0.2f)};
  rig.process(in);  // the callback that takes the layout over
  const GridChannels out = rig.process(in);
  EXPECT_GT(gridPeak(out[0]), 0.05f) << "Out 1/2 is silent";
  EXPECT_GT(gridPeak(out[2]), 0.05f) << "Out 3/4 is silent";
  EXPECT_EQ(rig.proc.unresolvedPlanRefsForTesting(), 0);
  std::printf("  %d prepares against %d edits\n", prepares.load(), edits);
}

// ---------------------------------------------------------------------------
// 3. Numbers that are not numbers
// ---------------------------------------------------------------------------

// A mono row from input 1, and rows 2 and 3 as a crossover split that
// rejoins, with the numbers of each off their defaults.
void buildNumbersRig(GridRig& rig) {
  rig.run("grid.convert", kToGrid);
  rig.run("row.setInput", R"({"row":0,"source":"device","channel":1})");
  rig.run("row.setFormat", R"({"row":0,"format":"mono"})");
  rig.run("row.setMix", R"({"row":0,"volumeDb":-3,"pan":0.3})");
  rig.run("row.setInputStage", R"({"row":0,"trimDb":2})");
  rig.run("row.setInput", R"({"row":2,"source":"device","channel":2})");
  rig.run("row.setOutput", R"({"row":2,"target":"device","pair":1})");
  rig.run("split.add", R"({"row":2,"splitAt":2,"mixAt":6,"type":"crossover"})");
  rig.run("split.setParam", R"({"row":2,"param":"mix.levelB","value":-4})");
  rig.addKind("utility.gain", 0, 1, "gainDb=1");
  rig.addKind("utility.gain", 3, 3, "gainDb=-1");
  ASSERT_TRUE(waitForChainLoaded(rig.proc));
}

TEST(GridHostileStateTest, NaNAndInfinityLoadAsDefaults) {
  GridRig source;
  buildNumbersRig(source);
  const juce::ValueTree good = parseSaved(savedBytes(source.proc));
  ASSERT_TRUE(good.isValid());

  struct Field {
    const char* name;
    // Finds the node in a copy of the tree and the property to poison.
    std::function<juce::ValueTree(juce::ValueTree&)> node;
    const char* property;
    // What the grid reports for it once loaded.
    std::function<void(GridRig&)> expectDefault;
  };
  const auto grid = [](juce::ValueTree& tree) {
    return tree.getChildWithName("ChainSnapshot").getChildWithName("Grid");
  };
  const auto splitNode = [grid](juce::ValueTree& tree) { return grid(tree).getChildWithName("Split"); };
  const Field fields[] = {
      {"row pan", [grid](juce::ValueTree& t) { return grid(t).getChild(0).getChildWithName("Output"); },
       "pan",
       [](GridRig& rig) { EXPECT_EQ(static_cast<double>(rig.row(0)["output"]["pan"]), 0.5); }},
      {"row volume",
       [grid](juce::ValueTree& t) { return grid(t).getChild(0).getChildWithName("Output"); }, "volumeDb",
       [](GridRig& rig) { EXPECT_EQ(static_cast<double>(rig.row(0)["output"]["volumeDb"]), 0.0); }},
      {"row output pair",
       [grid](juce::ValueTree& t) { return grid(t).getChild(0).getChildWithName("Output"); }, "pair",
       [](GridRig& rig) { EXPECT_EQ(static_cast<int>(rig.row(0)["output"]["pair"]), 1); }},
      {"row feed target",
       [grid](juce::ValueTree& t) { return grid(t).getChild(0).getChildWithName("Output"); }, "toRow",
       [](GridRig& rig) { EXPECT_EQ(rig.row(0)["output"]["type"].toString(), "device"); }},
      {"row trim", [grid](juce::ValueTree& t) { return grid(t).getChild(0).getChildWithName("Input"); },
       "trimDb",
       [](GridRig& rig) { EXPECT_EQ(static_cast<double>(rig.row(0)["input"]["trimDb"]), 0.0); }},
      {"row input channel",
       [grid](juce::ValueTree& t) { return grid(t).getChild(2).getChildWithName("Input"); }, "channel",
       [](GridRig& rig) { EXPECT_EQ(static_cast<int>(rig.row(2)["input"]["channel"]), 1); }},
      {"split column", splitNode, "splitCol",
       [](GridRig& rig) {
         // Boundary 0 is the default; with the rejoin at 6 that still holds.
         EXPECT_EQ(static_cast<int>(rig.proc.getGridState()["splits"][0]["splitAt"]), 0);
       }},
      {"mix column", splitNode, "mixCol",
       [](GridRig& rig) { EXPECT_TRUE(rig.proc.getGridState()["splits"][0]["mixAt"].isVoid()); }},
      {"split row", splitNode, "upperRow",
       [](GridRig& rig) {
         // Row 0 is the default, and row 1 below it has no input: it runs there.
         EXPECT_EQ(rig.proc.getGridState()["splits"].size(), 1);
       }},
      {"split parameter",
       [splitNode](juce::ValueTree& t) { return splitNode(t).getChildWithProperty("id", "mix.levelB"); },
       "value",
       [](GridRig& rig) {
         EXPECT_EQ(static_cast<double>(rig.proc.getGridState()["splits"][0]["params"]["mix.levelB"]), 0.0);
       }},
      {"crossover frequency",
       [splitNode](juce::ValueTree& t) {
         return splitNode(t).getChildWithProperty("id", "split.crossoverHz");
       },
       "value",
       [](GridRig& rig) {
         EXPECT_EQ(static_cast<double>(rig.proc.getGridState()["splits"][0]["params"]["split.crossoverHz"]),
                   500.0);
       }},
      {"grid format", grid, "minReader", [](GridRig& rig) { EXPECT_TRUE(rig.proc.isGridMode()); }},
      {"grid columns", grid, "cols", [](GridRig& rig) { EXPECT_TRUE(rig.proc.isGridMode()); }},
      {"row index", [grid](juce::ValueTree& t) { return grid(t).getChild(3); }, "index",
       [](GridRig& rig) { EXPECT_TRUE(rig.proc.isGridMode()); }},
  };
  const double poisons[] = {std::numeric_limits<double>::quiet_NaN(),
                            std::numeric_limits<double>::infinity(),
                            -std::numeric_limits<double>::infinity(), 1.0e300};
  const GridChannels in{makeNoise(kFrames, 93, 0.2f), makeNoise(kFrames, 94, 0.2f)};
  for (const Field& field : fields)
    for (const double poison : poisons) {
      SCOPED_TRACE(std::string(field.name) + " = " + std::to_string(poison));
      juce::ValueTree tree = good.createCopy();
      juce::ValueTree node = field.node(tree);
      ASSERT_TRUE(node.isValid());
      node.setProperty(field.property, poison, nullptr);
      const juce::MemoryBlock hostile = frame(tree);

      GridRig rig;
      rig.proc.setStateInformation(hostile.getData(), static_cast<int>(hostile.getSize()));
      ASSERT_TRUE(waitForChainLoaded(rig.proc));
      ASSERT_TRUE(rig.proc.isGridMode());
      rig.settle();
      field.expectDefault(rig);
      const GridChannels out = rig.process(in);
      EXPECT_TRUE(allFinite(out)) << "a number that is none reached the output";
      // And the filters after the rows are clean: the next block is too.
      EXPECT_TRUE(allFinite(rig.process(in)));
      // A row index that is no index makes the row a node this build does
      // not know, which is written back as it came; everything this build
      // understands is written clean.
      if (std::string(field.name) != "row index") {
        juce::String where;
        EXPECT_TRUE(treeIsFinite(parseSaved(savedBytes(rig.proc)).getChildWithName("ChainSnapshot"), where))
            << "re-saved with a non-number at " << where;
      }
    }
}

// The same through the text the held image controls are saved as, and
// through a parameter move that carries one.
TEST(GridHostileStateTest, NaNInTheLegacyControlsAndInAMovedControl) {
  GridRig source;
  load(source, lanes({gainBlock("blk-l")}, {gainBlock("blk-r")}, true));
  source.run("grid.convert", kToGrid);
  juce::ValueTree tree = parseSaved(savedBytes(source.proc));
  tree.getChildWithName("ChainSnapshot")
      .getChildWithName("Grid")
      .setProperty("legacyControls", "chainPanLeft=nan;outputBalance=inf;spreadEnabled=-inf", nullptr);
  const juce::MemoryBlock hostile = frame(tree);

  GridRig rig;
  rig.proc.setStateInformation(hostile.getData(), static_cast<int>(hostile.getSize()));
  ASSERT_TRUE(waitForChainLoaded(rig.proc));
  rig.run("grid.convert", kToLegacy);
  for (const char* id : {"chainPanLeft", "chainPanRight", "outputBalance", "spreadEnabled"})
    EXPECT_TRUE(std::isfinite(rig.proc.parameters.getRawParameterValue(id)->load())) << id;
  rig.settle();
  const GridChannels in{makeNoise(kFrames, 95, 0.2f), makeNoise(kFrames, 96, 0.2f)};
  EXPECT_TRUE(allFinite(rig.process(in)));

  // A command cannot put one in either.
  rig.run("grid.convert", kToGrid);
  juce::DynamicObject::Ptr args = new juce::DynamicObject();
  args->setProperty("row", 0);
  args->setProperty("pan", std::numeric_limits<double>::quiet_NaN());
  EXPECT_FALSE(rig.commands.registry().run("row.setMix", juce::var(args.get())).ok);
  args = new juce::DynamicObject();
  args->setProperty("row", 0);
  args->setProperty("trimDb", std::numeric_limits<double>::infinity());
  EXPECT_FALSE(rig.commands.registry().run("row.setInputStage", juce::var(args.get())).ok);
  EXPECT_TRUE(rig.proc.setRowMix(0, {std::numeric_limits<float>::quiet_NaN(), {}, {}, {}, {}}).failed());
  EXPECT_TRUE(rig.proc.setSplitParam(0, "mix.level", std::numeric_limits<double>::quiet_NaN()).failed());
  rig.settle();
  EXPECT_TRUE(allFinite(rig.process(in)));
  juce::String where;
  EXPECT_TRUE(treeIsFinite(parseSaved(savedBytes(rig.proc)).getChildWithName("ChainSnapshot"), where))
      << where;
}

// ---------------------------------------------------------------------------
// 4. Far more splits than a grid can have
// ---------------------------------------------------------------------------

TEST(GridHostileStateTest, ManyBadSplitNodes) {
  for (const int count : {12, 1000}) {
    SCOPED_TRACE(std::to_string(count) + " Split nodes");
    GridRig source(2, 4);
    source.run("grid.convert", kToGrid);
    source.addKind("utility.gain", 0, 0, "gainDb=2");
    source.run("row.setInput", R"({"row":3,"source":"device","channel":2})");
    source.run("row.setOutput", R"({"row":3,"target":"device","pair":2})");
    juce::ValueTree tree = parseSaved(savedBytes(source.proc));
    juce::ValueTree gridNode = tree.getChildWithName("ChainSnapshot").getChildWithName("Grid");
    // Every kind of wrong: a row taken many times over, rows that do not
    // exist, the last row, a lower row with an input of its own, ranges
    // that are none, a nest outside its parent.
    const int rows[] = {0, 0, 1, 2, 3, -1, 99, 0, 1, 2, 0, 0};
    const int splits[] = {2, 3, 0, 1, 2, 2, 2, 9, 7, 4, -4, 5};
    const int mixes[] = {6, 4, 8, 3, 5, 5, 5, 3, 2, 4, 2, -1};
    for (int i = 0; i < count; ++i) {
      juce::ValueTree split("Split");
      split.setProperty("id", "split-" + juce::String(i), nullptr);
      split.setProperty("upperRow", rows[i % 12], nullptr);
      split.setProperty("splitCol", splits[i % 12], nullptr);
      split.setProperty("mixCol", mixes[i % 12], nullptr);
      split.setProperty("type", i % 3 == 0 ? "ab" : i % 3 == 1 ? "crossover" : "balance", nullptr);
      gridNode.appendChild(split, nullptr);
    }
    const juce::MemoryBlock hostile = frame(tree);

    // Over a rig that is playing something else: the lanes are rebuilt, so
    // the plan has to be too.
    GridRig rig(2, 4);
    load(rig, lanes({gainBlock("blk-old-a"), gainBlock("blk-old-b")}, {gainBlock("blk-old-c")}, true));
    rig.proc.setStateInformation(hostile.getData(), static_cast<int>(hostile.getSize()));
    ASSERT_TRUE(waitForChainLoaded(rig.proc));
    ASSERT_TRUE(rig.proc.isGridMode());
    EXPECT_EQ(rig.proc.validateActivePlanForTesting(), "") << "the plan does not match the rows";

    // At most one split per row runs; the rest is listed inert.
    const juce::var listed = rig.proc.getGridState()["splits"];
    EXPECT_EQ(listed.size(), count);
    int live = 0;
    std::vector<int> liveRows;
    for (int i = 0; i < listed.size(); ++i)
      if (!static_cast<bool>(listed[i]["inert"])) {
        ++live;
        liveRows.push_back(static_cast<int>(listed[i]["row"]));
      }
    EXPECT_LE(live, kGridRows - 1);
    for (size_t a = 0; a < liveRows.size(); ++a)
      for (size_t b = a + 1; b < liveRows.size(); ++b)
        EXPECT_NE(liveRows[a], liveRows[b]);

    // It plays, and the plan the audio thread runs is the rig's.
    rig.settle();
    const GridChannels in{makeNoise(kFrames, 97, 0.2f), makeNoise(kFrames, 98, 0.2f)};
    const GridChannels out = rig.process(in);
    EXPECT_GT(gridPeak(out[0]), 0.05f);
    EXPECT_TRUE(allFinite(out));
    EXPECT_EQ(rig.proc.stalePlanCallbacksForTesting(), 0);
    EXPECT_EQ(rig.proc.unresolvedPlanRefsForTesting(), 0);

    // Later edits are not stuck rolling back onto a broken rig.
    rig.run("row.setMix", R"({"row":0,"volumeDb":-2})");
    const juce::String id = rig.addKind("utility.gain", 0, 5);
    EXPECT_EQ(rig.proc.gridSlotBlockId(0, 5), id.toStdString());
    rig.run("row.setOutput", R"({"row":0,"target":"device","pair":2})");
    EXPECT_EQ(static_cast<int>(rig.row(0)["output"]["pair"]), 2);
    for (int row = 0; row < kGridRows - 1; ++row)
      if (rig.tryRun("split.remove", ("{\"row\":" + std::to_string(row) + "}").c_str()).ok) {
        EXPECT_EQ(rig.proc.validateActivePlanForTesting(), "");
      }
    // What was set aside stays aside: no split comes back by itself, not
    // through the roll-back of a refused edit either.
    for (int i = 0; i < rig.proc.getGridState()["splits"].size(); ++i)
      EXPECT_TRUE(static_cast<bool>(rig.proc.getGridState()["splits"][i]["inert"]));
    rig.run("split.add", R"({"row":0,"splitAt":1,"mixAt":3})");
    EXPECT_EQ(rig.proc.validateActivePlanForTesting(), "");
    ASSERT_TRUE(rig.proc.undoChain());
    EXPECT_EQ(rig.proc.validateActivePlanForTesting(), "");

    // Nothing of the file was thrown away while it was only loaded.
    GridRig again(2, 4);
    again.proc.setStateInformation(hostile.getData(), static_cast<int>(hostile.getSize()));
    ASSERT_TRUE(waitForChainLoaded(again.proc));
    const juce::ValueTree resaved =
        parseSaved(savedBytes(again.proc)).getChildWithName("ChainSnapshot").getChildWithName("Grid");
    int splitNodes = 0;
    for (int i = 0; i < resaved.getNumChildren(); ++i)
      splitNodes += resaved.getChild(i).hasType("Split") ? 1 : 0;
    EXPECT_EQ(splitNodes, count);
    // And the re-saved state loads to the same rig.
    const juce::MemoryBlock second = savedBytes(again.proc);
    GridRig third(2, 4);
    third.proc.setStateInformation(second.getData(), static_cast<int>(second.getSize()));
    ASSERT_TRUE(waitForChainLoaded(third.proc));
    EXPECT_EQ(third.proc.validateActivePlanForTesting(), "");
    EXPECT_EQ(third.proc.getGridState()["splits"].size(), count);
  }
}

// ---------------------------------------------------------------------------
// 5. A measurement in flight when the rig becomes a grid
// ---------------------------------------------------------------------------

TEST(GridMeasurementTest, AutoAlignAndAutoBalanceEndWhenTheRigBecomesAGrid) {
  const GridChannels in{makeNoise(kFrames, 99, 0.2f), makeNoise(kFrames, 100, 0.2f)};
  const juce::File store = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile("pedaleira-measure-preset-" + juce::Uuid().toString());
  struct Cleanup {
    juce::File dir;
    ~Cleanup() { dir.deleteRecursively(); }
  } cleanup{store};

  struct Way {
    const char* name;
    // Takes a legacy stereo rig with a measurement running to a grid.
    std::function<void(GridRig&)> toGrid;
  };
  juce::String gridPresetId;
  const Way ways[] = {
      {"grid.convert", [](GridRig& rig) { rig.run("grid.convert", kToGrid); }},
      {"a row command", [](GridRig& rig) { rig.run("row.setMix", R"({"row":0,"pan":0.1})"); }},
      {"redo of a conversion",
       [](GridRig& rig) { ASSERT_TRUE(rig.proc.redoChain()); }},
      {"a grid preset", [&](GridRig& rig) { ASSERT_TRUE(rig.proc.loadPreset(gridPresetId)); }},
  };
  for (const Way& way : ways)
    for (const bool withAudio : {true, false}) {
      SCOPED_TRACE(std::string(way.name) + (withAudio ? ", audio running" : ", no audio"));
      GridRig rig;
      rig.proc.setPresetStoreForTesting(store);
      load(rig, lanes({gainBlock("blk-l")}, {gainBlock("blk-r")}, true));
      // What the two later ways need: a conversion to redo, a grid preset.
      rig.run("grid.convert", kToGrid);
      if (gridPresetId.isEmpty())
        gridPresetId = rig.proc.savePreset("Grid")["id"].toString();
      ASSERT_TRUE(rig.proc.undoChain());
      ASSERT_TRUE(waitForChainLoaded(rig.proc));
      ASSERT_FALSE(rig.proc.isGridMode());
      rig.settle();
      const GridChannels playing = rig.process(in);
      ASSERT_GT(gridPeak(playing[0]), 0.05f);

      rig.proc.startAutoOffset();
      rig.proc.startAutoBalance();
      if (withAudio)
        rig.process({makeNoise(256, 1, 0.2f), makeNoise(256, 2, 0.2f)});
      ASSERT_EQ(rig.proc.pollAutoOffset()["state"].toString(), "listening");
      ASSERT_EQ(rig.proc.pollAutoBalance()["state"].toString(), "listening");

      way.toGrid(rig);
      ASSERT_TRUE(waitForChainLoaded(rig.proc));
      ASSERT_TRUE(rig.proc.isGridMode());
      // Ended, by the edit itself: no callback has to come first.
      EXPECT_EQ(rig.proc.pollAutoOffset()["state"].toString(), "idle");
      EXPECT_EQ(rig.proc.pollAutoBalance()["state"].toString(), "idle");
      // Neither starts in a grid.
      rig.proc.startAutoOffset();
      rig.proc.startAutoBalance();
      EXPECT_EQ(rig.proc.pollAutoOffset()["state"].toString(), "idle");
      EXPECT_EQ(rig.proc.pollAutoBalance()["state"].toString(), "idle");

      // And Out 1/2 is not left muted.
      rig.settle();
      rig.process(in);
      const GridChannels out = rig.process(in);
      EXPECT_GT(gridPeak(out[0]), 0.05f) << "the probe's mute never lifted";
      EXPECT_GT(gridPeak(out[1]), 0.05f);
    }
}

// ---------------------------------------------------------------------------
// 6. Parameter writes and the chain lock
// ---------------------------------------------------------------------------

// A host that takes its time over a parameter change (here: one that asks
// the processor something that needs the chain lock, from another thread,
// and waits for the answer). If the write were made under the lock, the
// answer could not come while the listener waits.
struct LockProbe : juce::AudioProcessorValueTreeState::Listener {
  TONE3000Processor& proc;
  std::atomic<int> notifications{0};
  std::atomic<int> madeUnderTheLock{0};

  explicit LockProbe(TONE3000Processor& p) : proc(p) {}
  void parameterChanged(const juce::String&, float) override {
    notifications.fetch_add(1);
    std::atomic<bool> answered{false};
    std::thread other([&] {
      (void)proc.getGridState();  // takes chainMutex
      answered.store(true);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    while (!answered.load() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!answered.load())
      madeUnderTheLock.fetch_add(1);
    other.join();  // once this returns, whoever held the lock has let go... or will
  }
};

TEST(GridLockTest, LegacyControlWritesAreMadeOffTheChainLock) {
  GridRig rig;
  load(rig, lanes({gainBlock("blk-l")}, {gainBlock("blk-r")}, true));
  LockProbe probe(rig.proc);
  const char* ids[] = {"chainPanLeft",    "chainPanRight",    "chainSoloLeft",
                       "chainSoloRight",  "chainInvertLeft",  "chainInvertRight",
                       "outputBalance",   "spreadEnabled",    "alignEnabled"};
  for (const char* id : ids)
    rig.proc.parameters.addParameterListener(id, &probe);
  struct Remove {
    GridRig& rig;
    LockProbe& probe;
    const char* const* ids;
    ~Remove() {
      for (int i = 0; i < 9; ++i)
        rig.proc.parameters.removeParameterListener(ids[i], &probe);
    }
  } remove{rig, probe, ids};

  const auto step = [&](const char* what, int atLeast) {
    SCOPED_TRACE(what);
    EXPECT_GE(probe.notifications.load(), atLeast) << "nothing was written: the step tests nothing";
    EXPECT_EQ(probe.madeUnderTheLock.load(), 0);
    probe.notifications.store(0);
    probe.madeUnderTheLock.store(0);
  };

  rig.proc.parameters.getParameter("alignEnabled")->setValueNotifyingHost(1.0f);
  step("the host's own move", 1);
  rig.run("grid.convert", kToGrid);  // Align is held off
  step("promotion", 1);
  // The value path, with no fade armed: a row's mix moves three controls.
  rig.run("row.setMix", R"({"row":0,"pan":0.3,"invert":true,"volumeDb":-2})");
  rig.run("row.setMix", R"({"row":1,"volumeDb":2,"solo":true})");
  step("row.setMix", 3);
  EXPECT_NEAR(rig.proc.parameters.getRawParameterValue("chainPanLeft")->load(), 0.3f, 1e-6f);
  // (The parameter keeps four decimals.)
  EXPECT_NEAR(rig.proc.parameters.getRawParameterValue("outputBalance")->load(), 0.5f + 2.0f / 24.0f,
              1e-4f);
  // A held control put back on the message thread.
  rig.proc.parameters.getParameter("spreadEnabled")->setValueNotifyingHost(1.0f);
  juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
  step("a held control snapping back", 2);
  EXPECT_LT(rig.proc.parameters.getRawParameterValue("spreadEnabled")->load(), 0.5f);
  // Undo, redo, demotion, a refused edit rolled back.
  ASSERT_TRUE(rig.proc.undoChain());
  step("undo", 1);
  ASSERT_TRUE(rig.proc.redoChain());
  step("redo", 1);
  rig.run("row.setMix", R"({"row":0,"pan":0.45})");
  rig.run("grid.convert", kToLegacy);
  step("demotion", 1);
  EXPECT_GE(rig.proc.parameters.getRawParameterValue("alignEnabled")->load(), 0.5f)
      << "what it was before the grid";
  EXPECT_FALSE(rig.tryRun("split.move", R"({"row":0,"splitAt":2})").ok);  // promotes, fails, rolls back
  EXPECT_FALSE(rig.proc.isGridMode());
  EXPECT_EQ(probe.madeUnderTheLock.load(), 0);
  // A state load into a grid and out of it.
  rig.run("grid.convert", kToGrid);
  const juce::MemoryBlock gridBytes = savedBytes(rig.proc);
  rig.run("grid.convert", kToLegacy);
  probe.notifications.store(0);
  rig.proc.setStateInformation(gridBytes.getData(), static_cast<int>(gridBytes.getSize()));
  ASSERT_TRUE(waitForChainLoaded(rig.proc));
  EXPECT_EQ(probe.madeUnderTheLock.load(), 0);
  EXPECT_TRUE(savedBytes(rig.proc) == gridBytes);
}

// ---------------------------------------------------------------------------
// 7. Commands that name an occupied slot, or nothing to do
// ---------------------------------------------------------------------------

// `row` and `slot` are another way to say `target`, and mean what it means:
// an empty slot takes the tone, a block in the slot has its tone replaced in
// place (same id, same position). Either way nothing lands anywhere else.
TEST(GridAddressingTest, AToneIntoAnOccupiedSlotReplacesInPlace) {
  const juce::String amp = testFile("a2-amp-test.nam").getFullPathName().replace("\\", "/");
  const juce::String cab = testFile("cab-ir-test.wav").getFullPathName().replace("\\", "/");
  GridRig rig(2, 4);
  rig.run("grid.convert", kToGrid);
  const juce::String gain = rig.addKind("utility.gain", 2, 3);
  const auto slots = [&] {
    juce::String all;
    for (int row = 0; row < kGridRows; ++row)
      all += juce::JSON::toString(rig.row(row)["slots"], true);
    return all;
  };
  const auto kindAt = [&](int row, int slot) {
    return rig.proc.getChainState(-1)["grid"]["rows"][row]["blocks"][slot]["kindId"].toString();
  };
  const juce::String before = slots();

  // Onto a block of a registry kind, then onto the tone that replaced it.
  juce::var result = rig.run("chain.addToneFromFiles",
                             "{\"paths\":[\"" + amp + "\"],\"row\":2,\"slot\":3}");
  rig.settle();
  EXPECT_EQ(result["blockId"].toString(), gain) << "the block in the slot, not a new one";
  EXPECT_EQ(slots(), before) << "something landed in another slot";
  EXPECT_EQ(kindAt(2, 3), "nam");
  result = rig.run("chain.addToneFromFiles", "{\"paths\":[\"" + cab + "\"],\"row\":2,\"slot\":3}");
  rig.settle();
  EXPECT_EQ(result["blockId"].toString(), gain);
  EXPECT_EQ(slots(), before);
  EXPECT_EQ(kindAt(2, 3), "ir");
  // The same by block id, as the legacy `target` does it.
  result = rig.run("chain.addToneFromFiles", "{\"paths\":[\"" + amp + "\"],\"target\":\"" + gain + "\"}");
  rig.settle();
  EXPECT_EQ(result["blockId"].toString(), gain);
  EXPECT_EQ(slots(), before);
  EXPECT_EQ(kindAt(2, 3), "nam");

  // An empty slot takes the tone and gets its id; the other slots stay.
  result = rig.run("chain.addToneFromFiles", "{\"paths\":[\"" + cab + "\"],\"row\":1,\"slot\":6}");
  rig.settle();
  EXPECT_EQ(rig.proc.gridSlotBlockId(1, 6), result["blockId"].toString().toStdString());
  EXPECT_EQ(rig.proc.gridSlotBlockId(2, 3), gain.toStdString());
  // A full row and an occupied slot: still in place, never in another row.
  for (int slot = 0; slot < kGridCols; ++slot)
    if (slot != 3)
      rig.addKind("utility.gain", 2, slot);
  const juce::String full = slots();
  result = rig.run("chain.addToneFromFiles", "{\"paths\":[\"" + cab + "\"],\"row\":2,\"slot\":3}");
  rig.settle();
  EXPECT_EQ(result["blockId"].toString(), gain);
  EXPECT_EQ(slots(), full);
  EXPECT_EQ(rig.proc.validateActivePlanForTesting(), "");
}

TEST(GridAddressingTest, AnEditWithNoFieldsDoesNothing) {
  GridRig rig;
  load(rig, lanes({gainBlock("blk-a")}, {}, false));
  const juce::MemoryBlock legacyBytes = savedBytes(rig.proc);
  const int revision = static_cast<int>(rig.proc.getCurrentChainRevision());
  for (const char* command : {"row.setMix", "row.setInputStage"}) {
    SCOPED_TRACE(command);
    const CommandResult result = rig.tryRun(command, R"({"row":0})");
    EXPECT_TRUE(result.ok) << result.errorMessage;
    EXPECT_EQ(result.value["mode"].toString(), "legacy");
    EXPECT_FALSE(rig.proc.isGridMode()) << "a command with nothing to set converted the rig";
    EXPECT_FALSE(static_cast<bool>(rig.proc.getChainState(-1)["canUndo"]));
    EXPECT_TRUE(savedBytes(rig.proc) == legacyBytes);
    EXPECT_EQ(static_cast<int>(rig.proc.getCurrentChainRevision()), revision);
  }
  // With a field it converts, as before. On a grid the empty call records
  // nothing either: what can be redone stays.
  rig.run("row.setMix", R"({"row":0,"volumeDb":-1})");
  ASSERT_TRUE(rig.proc.isGridMode());
  rig.run("row.setFormat", R"({"row":1,"format":"mono"})");
  ASSERT_TRUE(rig.proc.undoChain());
  ASSERT_TRUE(static_cast<bool>(rig.proc.getChainState(-1)["canRedo"]));
  const juce::MemoryBlock gridBytes = savedBytes(rig.proc);
  EXPECT_TRUE(rig.tryRun("row.setMix", R"({"row":0})").ok);
  EXPECT_TRUE(rig.tryRun("row.setInputStage", R"({"row":0})").ok);
  EXPECT_TRUE(static_cast<bool>(rig.proc.getChainState(-1)["canRedo"]));
  EXPECT_TRUE(savedBytes(rig.proc) == gridBytes);
}

}  // namespace

// Grid mode as state: the conversion to and from the legacy rig (promotion
// and the projection back), undo across it, the saved Grid node with its
// forward-compatibility rules, presets, scenes and stomps, and what a client
// that only knows the legacy shape sees.

#include "grid_test_helpers.h"

#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <vector>

namespace {

using InputMode = TONE3000Processor::InputMode;

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

// The chain state without what changes from one call to the next whatever
// the rig is: the revision and the undo flags.
juce::String stableChainJson(TONE3000Processor& proc) {
  const juce::var state = proc.getChainState(-1);
  juce::DynamicObject::Ptr copy = new juce::DynamicObject();
  for (const auto& property : state.getDynamicObject()->getProperties())
    if (property.name.toString() != "revision" && property.name.toString() != "canUndo" &&
        property.name.toString() != "canRedo")
      copy->setProperty(property.name, property.value);
  return juce::JSON::toString(juce::var(copy.get()));
}

// The same for a grid, without what only a grid reports: what is left is
// what a client that knows nothing of grids reads.
juce::String legacyViewJson(TONE3000Processor& proc) {
  const juce::var state = proc.getChainState(-1);
  juce::DynamicObject::Ptr copy = new juce::DynamicObject();
  for (const auto& property : state.getDynamicObject()->getProperties()) {
    const juce::String name = property.name.toString();
    if (name != "revision" && name != "canUndo" && name != "canRedo" && name != "grid" &&
        name != "legacyEditable")
      copy->setProperty(property.name, property.value);
  }
  return juce::JSON::toString(juce::var(copy.get()));
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

juce::ValueTree gainBlock(const char* id, double gainDb = 0.0) {
  return makeKindBlockTree(id, "utility.gain", "Gain",
                           {{"gainDb", gainDb}, {"pan", 0.0}, {"phaseInvert", 0.0}});
}

void load(GridRig& rig, const juce::ValueTree& state) {
  rig.proc.restoreFromTree(state);
  ASSERT_TRUE(waitForChainLoaded(rig.proc));
}

void setParam(GridRig& rig, const char* id, float realValue) {
  auto* param = rig.proc.parameters.getParameter(id);
  ASSERT_NE(param, nullptr) << id;
  param->setValueNotifyingHost(param->convertTo0to1(realValue));
}

// ---------------------------------------------------------------------------
// The mapping table
// ---------------------------------------------------------------------------

TEST(GridMappingTest, MonoRigBecomesOneSummedStereoRow) {
  GridRig rig;
  load(rig, lanes({gainBlock("blk-a"), makeIrBlockTree("blk-cab", 2, 200)}, {gainBlock("blk-r")},
                  false));
  const juce::var converted = rig.run("grid.convert", kToGrid);
  EXPECT_EQ(converted["mode"].toString(), "grid");
  EXPECT_EQ(converted["lossy"].size(), 0);
  EXPECT_TRUE(static_cast<bool>(converted["legacyProjectable"]));

  const juce::var first = rig.row(0);
  EXPECT_EQ(first["format"].toString(), "stereo");
  EXPECT_EQ(first["input"]["type"].toString(), "device");
  EXPECT_EQ(static_cast<int>(first["input"]["channel"]), 1);
  EXPECT_TRUE(static_cast<bool>(first["input"]["stereo"]));
  EXPECT_EQ(first["input"]["fold"].toString(), "sum");
  EXPECT_TRUE(static_cast<bool>(first["input"]["gate"]));
  EXPECT_TRUE(static_cast<bool>(first["input"]["pitch"]));
  EXPECT_EQ(first["output"]["type"].toString(), "device");
  EXPECT_EQ(static_cast<int>(first["output"]["pair"]), 1);
  EXPECT_EQ(static_cast<double>(first["output"]["volumeDb"]), 0.0);
  EXPECT_EQ(static_cast<double>(first["output"]["pan"]), 0.5);
  // Lane index i is column i, and the row is padded to eight slots.
  ASSERT_EQ(first["slots"].size(), kGridCols);
  EXPECT_EQ(first["slots"][0].toString(), "blk-a");
  EXPECT_EQ(first["slots"][1].toString(), "blk-cab");
  EXPECT_TRUE(static_cast<bool>(first["routed"]));

  // The dormant right lane's blocks are kept in row 1, which is not heard.
  const juce::var second = rig.row(1);
  EXPECT_EQ(second["input"]["type"].toString(), "none");
  EXPECT_EQ(second["output"]["type"].toString(), "none");
  EXPECT_FALSE(static_cast<bool>(second["routed"]));
  EXPECT_EQ(second["slots"][0].toString(), "blk-r");
  EXPECT_FALSE(static_cast<bool>(second["active"][0]));
  for (int row = 2; row < kGridRows; ++row) {
    EXPECT_EQ(rig.row(row)["slots"].size(), kGridCols);
    EXPECT_FALSE(static_cast<bool>(rig.row(row)["routed"]));
  }
  EXPECT_EQ(rig.proc.validateActivePlanForTesting(), "");
  EXPECT_EQ(rig.proc.getGridState()["imageStage"].toString(), "none");
}

TEST(GridMappingTest, InputModesBecomeRowInputs) {
  struct Case {
    InputMode mode;
    int ins, outs;
    int channel;
    bool stereo;
    const char* fold;
    const char* format;
  };
  const Case cases[] = {
      {InputMode::Stereo, 2, 2, 1, true, "sum", "stereo"},
      {InputMode::Left, 2, 2, 1, false, "none", "stereo"},
      {InputMode::Right, 2, 2, 2, false, "none", "stereo"},
      {InputMode::DualMono, 2, 2, 1, true, "none", "dual"},
      // On a mono device every mode is "channel 1, mono".
      {InputMode::Stereo, 1, 1, 1, false, "none", "stereo"},
      {InputMode::DualMono, 1, 1, 1, false, "none", "stereo"},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(TONE3000Processor::inputModeToString(c.mode).toStdString() + " on " +
                 std::to_string(c.ins) + " inputs");
    GridRig rig(c.ins, c.outs);
    load(rig, lanes({gainBlock("blk-a")}, {}, false));
    rig.proc.setInputMode(c.mode);
    ASSERT_TRUE(waitForChainLoaded(rig.proc));
    rig.run("grid.convert", kToGrid);
    const juce::var first = rig.row(0);
    EXPECT_EQ(static_cast<int>(first["input"]["channel"]), c.channel);
    EXPECT_EQ(static_cast<bool>(first["input"]["stereo"]), c.stereo);
    EXPECT_EQ(first["input"]["fold"].toString(), c.fold);
    EXPECT_EQ(first["format"].toString(), c.format);
    EXPECT_FALSE(static_cast<bool>(rig.row(1)["routed"]));
  }
}

TEST(GridMappingTest, StereoRigBecomesTwoPannedMonoRows) {
  GridRig rig;
  load(rig, lanes({gainBlock("blk-l")}, {gainBlock("blk-r")}, true));
  setParam(rig, "chainPanLeft", 0.2f);
  setParam(rig, "chainPanRight", 0.7f);
  setParam(rig, "chainInvertRight", 1.0f);
  // The balance is plus or minus 12 dB, opposed: 0.75 is 6 dB toward the right.
  rig.proc.parameters.getParameter("outputBalance")->setValueNotifyingHost(0.75f);

  const juce::var converted = rig.run("grid.convert", kToGrid);
  bool saidAuto = false;
  for (int i = 0; i < converted["lossy"].size(); ++i)
    saidAuto = saidAuto || converted["lossy"][i].toString().contains("auto-align");
  EXPECT_TRUE(saidAuto) << juce::JSON::toString(converted["lossy"]);

  const juce::var first = rig.row(0), second = rig.row(1);
  for (const juce::var& row : {first, second}) {
    EXPECT_EQ(row["format"].toString(), "mono");
    EXPECT_FALSE(static_cast<bool>(row["input"]["stereo"]));
    EXPECT_EQ(static_cast<int>(row["output"]["pair"]), 1);
    EXPECT_TRUE(static_cast<bool>(row["routed"]));
  }
  EXPECT_EQ(static_cast<int>(first["input"]["channel"]), 1);
  EXPECT_EQ(static_cast<int>(second["input"]["channel"]), 2);
  EXPECT_NEAR(static_cast<double>(first["output"]["pan"]), 0.2, 1e-6);
  EXPECT_NEAR(static_cast<double>(second["output"]["pan"]), 0.7, 1e-6);
  EXPECT_NEAR(static_cast<double>(first["output"]["volumeDb"]), -6.0, 1e-4);
  EXPECT_NEAR(static_cast<double>(second["output"]["volumeDb"]), 6.0, 1e-4);
  EXPECT_FALSE(static_cast<bool>(first["output"]["invert"]));
  EXPECT_TRUE(static_cast<bool>(second["output"]["invert"]));
  EXPECT_EQ(first["slots"][0].toString(), "blk-l");
  EXPECT_EQ(second["slots"][0].toString(), "blk-r");

  // Input mode left: both rows take the same channel.
  GridRig left;
  load(left, lanes({gainBlock("blk-l")}, {gainBlock("blk-r")}, true));
  left.proc.setInputMode(InputMode::Left);
  left.run("grid.convert", kToGrid);
  EXPECT_EQ(static_cast<int>(left.row(0)["input"]["channel"]), 1);
  EXPECT_EQ(static_cast<int>(left.row(1)["input"]["channel"]), 1);
}

// The two lanes with pans, balance or polarity off their defaults: the same
// gains at the row sends as in the legacy image matrix, so the same image
// and level. Not bit for bit, though, and the test pins why:
//  - One lane alone is exact at 1x (each output sample is one product).
//  - With both lanes on one output the matrix adds two products in one
//    expression (which the compiler fuses into one rounding) and the sends
//    multiply and add in two steps: the last bit differs now and then.
//  - That last bit then passes the main output's tone stack, whose
//    low-frequency sections amplify round-off several hundred times. With
//    the tone stack off the difference stays at the last bit.
//  - With oversampling the sends also sit before the decimator and the
//    matrix after it, which reorders the rounding the same way.
// Far below audibility either way (about -100 dB against the signal).
TEST(GridMappingTest, StereoRigSoundsTheSameWithPansAndBalance) {
  const int frames = 8192;
  const GridChannels in{makeNoise(frames, 31, 0.2f), makeNoise(frames, 32, 0.2f)};
  const GridChannels onlyLeft{in[0], gridSilence(frames)};
  struct Case {
    const char* name;
    float panLeft, panRight, balance;
    bool invertRight;
  };
  const Case cases[] = {
      {"pans", 0.2f, 0.7f, 0.5f, false},
      {"balance", 0.0f, 1.0f, 0.6f, false},
      {"polarity", 0.0f, 1.0f, 0.5f, true},
      {"pans, balance and polarity", 0.2f, 0.7f, 0.6f, true},
  };
  for (const Case& c : cases)
    for (const int oversample : {1, 2}) {
      SCOPED_TRACE(std::string(c.name) + ", oversampling x" + std::to_string(oversample));
      const auto render = [&](bool promote, bool toneStack, const GridChannels& input) {
        GridRig rig(2, 2, 48000.0, 64, oversample);
        load(rig, lanes({gainBlock("blk-l", 2.0)}, {gainBlock("blk-r", -3.0)}, true));
        setParam(rig, "chainPanLeft", c.panLeft);
        setParam(rig, "chainPanRight", c.panRight);
        setParam(rig, "chainInvertRight", c.invertRight ? 1.0f : 0.0f);
        setParam(rig, "toneEqEnabled", toneStack ? 1.0f : 0.0f);
        rig.proc.parameters.getParameter("outputBalance")->setValueNotifyingHost(c.balance);
        if (promote)
          rig.run("grid.convert", kToGrid);
        rig.settle();
        EXPECT_EQ(rig.proc.isGridMode(), promote);
        return rig.process(input);
      };
      const auto difference = [](const GridChannels& a, const GridChannels& b) {
        return std::max(gridMaxDiff(a[0], b[0]), gridMaxDiff(a[1], b[1]));
      };

      const GridChannels expected = render(false, true, in);
      ASSERT_GT(gridPeak(expected[0]), 0.05f);
      const float diff = difference(render(true, true, in), expected);
      const float flatDiff = difference(render(true, false, in), render(false, false, in));
      const float oneLaneDiff = difference(render(true, true, onlyLeft), render(false, true, onlyLeft));
      std::printf("  stereo rig, %s, x%d: promoted against legacy, max abs diff %g (tone stack "
                  "off %g, left lane alone %g), peak %g\n",
                  c.name, oversample, static_cast<double>(diff), static_cast<double>(flatDiff),
                  static_cast<double>(oneLaneDiff), static_cast<double>(gridPeak(expected[0])));
      EXPECT_LT(diff, 2e-5f);
      EXPECT_LT(flatDiff, 2e-6f) << "more than the last bits before the tone stack";
      if (oversample == 1) {
        EXPECT_EQ(oneLaneDiff, 0.0f);
      }
    }
}

TEST(GridMappingTest, WhatCannotBeConvertedIsRefusedWithAReason) {
  // A live branch whose lane does not fit behind the tap: the branch lane
  // becomes the lower row of a split and starts at the split's column.
  GridRig branched;
  std::vector<juce::ValueTree> trunkBlocks, branchBlocks;
  for (int i = 0; i < 6; ++i)
    trunkBlocks.push_back(gainBlock(("blk-t" + std::to_string(i)).c_str()));
  for (int i = 0; i < 3; ++i)
    branchBlocks.push_back(gainBlock(("blk-b" + std::to_string(i)).c_str()));
  juce::ValueTree state = lanes(trunkBlocks, branchBlocks, true);
  state.setProperty("branchSide", "left", nullptr);
  state.setProperty("branchAfterBlockId", "blk-t5", nullptr);
  load(branched, state);
  const juce::String before = stableChainJson(branched.proc);
  CommandResult result = branched.tryRun("grid.convert", kToGrid);
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.errorCode, juce::String(CommandError::failed));
  EXPECT_TRUE(result.errorMessage.contains("branch")) << result.errorMessage;
  EXPECT_FALSE(branched.proc.isGridMode());
  EXPECT_EQ(stableChainJson(branched.proc), before);
  // A row command does not get further, and leaves no undo step behind.
  result = branched.tryRun("row.setMix", R"({"row":0,"volumeDb":-3})");
  EXPECT_FALSE(result.ok);
  EXPECT_FALSE(branched.proc.isGridMode());
  EXPECT_FALSE(static_cast<bool>(branched.proc.getChainState(-1)["canUndo"]));

  // A dormant branch (mono mode) is dropped, and the conversion says so.
  GridRig dormant;
  state = lanes({gainBlock("blk-l")}, {gainBlock("blk-r")}, false);
  state.setProperty("branchSide", "left", nullptr);
  state.setProperty("branchAfterBlockId", "blk-l", nullptr);
  load(dormant, state);
  const juce::var converted = dormant.run("grid.convert", kToGrid);
  ASSERT_EQ(converted["lossy"].size(), 1);
  EXPECT_TRUE(converted["lossy"][0].toString().contains("dormant branch"));

  // Nine blocks in a lane do not fit a row of eight.
  GridRig crowded;
  std::vector<juce::ValueTree> nine;
  for (int i = 0; i < 9; ++i)
    nine.push_back(gainBlock(("blk-" + std::to_string(i)).c_str()));
  load(crowded, lanes(nine, {}, false));
  result = crowded.tryRun("grid.convert", kToGrid);
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(result.errorMessage.contains("9 blocks")) << result.errorMessage;
  EXPECT_FALSE(crowded.proc.isGridMode());

  // Eight do, and their placeholders make way.
  GridRig full;
  nine.pop_back();
  load(full, lanes(nine, {}, false));
  full.run("grid.convert", kToGrid);
  for (int i = 0; i < 8; ++i)
    EXPECT_EQ(full.row(0)["slots"][i].toString(), "blk-" + juce::String(i));
}

// ---------------------------------------------------------------------------
// There and back
// ---------------------------------------------------------------------------

TEST(GridMappingTest, PromoteThenDemoteGivesTheSameLegacyRig) {
  struct Case {
    const char* name;
    int ins, outs;
    std::function<void(GridRig&)> build;
  };
  const Case cases[] = {
      {"mono", 2, 2,
       [](GridRig& rig) {
         load(rig, lanes({makeNamBlockTree("blk-amp", 1, 100), makeIrBlockTree("blk-cab", 2, 200)},
                         {}, false));
       }},
      {"mono with a dormant right lane", 2, 2,
       [](GridRig& rig) {
         load(rig, lanes({gainBlock("blk-a")}, {gainBlock("blk-r"), gainBlock("blk-r2")}, false));
       }},
      {"mono, input left", 2, 2,
       [](GridRig& rig) {
         load(rig, lanes({gainBlock("blk-a")}, {}, false));
         rig.proc.setInputMode(InputMode::Left);
       }},
      {"mono, input right", 2, 2,
       [](GridRig& rig) {
         load(rig, lanes({gainBlock("blk-a")}, {}, false));
         rig.proc.setInputMode(InputMode::Right);
       }},
      {"dual mono", 2, 2,
       [](GridRig& rig) {
         load(rig, lanes({makeNamBlockTree("blk-amp", 1, 100)}, {}, false));
         rig.proc.setInputMode(InputMode::DualMono);
       }},
      {"mono device", 1, 1,
       [](GridRig& rig) { load(rig, lanes({gainBlock("blk-a")}, {}, false)); }},
      {"stereo", 2, 2,
       [](GridRig& rig) {
         load(rig, lanes({gainBlock("blk-l")}, {gainBlock("blk-r"), gainBlock("blk-r2")}, true));
       }},
      {"stereo with pans, balance, polarity and solo", 2, 2,
       [](GridRig& rig) {
         load(rig, lanes({gainBlock("blk-l")}, {gainBlock("blk-r")}, true));
         setParam(rig, "chainPanLeft", 0.15f);
         setParam(rig, "chainPanRight", 0.8f);
         setParam(rig, "chainInvertLeft", 1.0f);
         setParam(rig, "chainSoloRight", 1.0f);
         rig.proc.parameters.getParameter("outputBalance")->setValueNotifyingHost(0.37f);
       }},
      {"stereo, input right", 2, 2,
       [](GridRig& rig) {
         load(rig, lanes({gainBlock("blk-l")}, {gainBlock("blk-r")}, true));
         rig.proc.setInputMode(InputMode::Right);
       }},
      {"branched", 2, 2,
       [](GridRig& rig) {
         juce::ValueTree state = lanes({gainBlock("blk-t0"), gainBlock("blk-t1"), gainBlock("blk-t2")},
                                       {gainBlock("blk-b0")}, true);
         state.setProperty("branchSide", "left", nullptr);
         state.setProperty("branchAfterBlockId", "blk-t1", nullptr);
         load(rig, state);
       }},
      {"branched after the last block, with pans, balance and polarity, input right", 2, 2,
       [](GridRig& rig) {
         juce::ValueTree state = lanes({makeNamBlockTree("blk-amp", 1, 100)},
                                       {makeIrBlockTree("blk-cab", 2, 200), gainBlock("blk-b1")}, true);
         state.setProperty("branchSide", "left", nullptr);
         state.setProperty("branchAfterBlockId", "blk-amp", nullptr);
         load(rig, state);
         rig.proc.setInputMode(InputMode::Right);
         setParam(rig, "chainPanLeft", 0.3f);
         setParam(rig, "chainPanRight", 0.6f);
         setParam(rig, "chainInvertRight", 1.0f);
         rig.proc.parameters.getParameter("outputBalance")->setValueNotifyingHost(0.62f);
       }},
      {"mono with Spread on (held off in the grid, back afterwards)", 2, 2,
       [](GridRig& rig) {
         load(rig, lanes({gainBlock("blk-a")}, {}, false));
         setParam(rig, "spreadEnabled", 1.0f);
       }},
      {"stereo with Align on", 2, 2,
       [](GridRig& rig) {
         load(rig, lanes({gainBlock("blk-l")}, {gainBlock("blk-r")}, true));
         setParam(rig, "alignEnabled", 1.0f);
       }},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    GridRig rig(c.ins, c.outs);
    c.build(rig);
    ASSERT_TRUE(waitForChainLoaded(rig.proc));
    const juce::String legacyJson = stableChainJson(rig.proc);
    const juce::MemoryBlock legacyBytes = savedBytes(rig.proc);
    ASSERT_EQ(static_cast<int>(parseSaved(legacyBytes).getProperty("schemaVersion")), 1);

    rig.run("grid.convert", kToGrid);
    ASSERT_TRUE(waitForChainLoaded(rig.proc));
    EXPECT_TRUE(rig.proc.isGridMode());
    EXPECT_TRUE(static_cast<bool>(rig.proc.getGridState()["legacyProjectable"]))
        << rig.proc.legacyProjectionBlockers().joinIntoString("; ");
    EXPECT_NE(stableChainJson(rig.proc), legacyJson);
    // The projection: a client that only knows the legacy rig is shown
    // exactly the legacy rig, lanes, input mode and all.
    EXPECT_EQ(legacyViewJson(rig.proc), legacyJson);
    EXPECT_TRUE(static_cast<bool>(rig.proc.getChainState(-1)["legacyEditable"]));
    EXPECT_EQ(static_cast<int>(parseSaved(savedBytes(rig.proc)).getProperty("schemaVersion")), 2);

    rig.run("grid.convert", kToLegacy);
    ASSERT_TRUE(waitForChainLoaded(rig.proc));
    EXPECT_FALSE(rig.proc.isGridMode());
    EXPECT_EQ(stableChainJson(rig.proc), legacyJson);
    EXPECT_TRUE(savedBytes(rig.proc) == legacyBytes) << "the saved state moved across the round trip";
    EXPECT_EQ(rig.proc.validateActivePlanForTesting(), "");
  }
}

TEST(GridMappingTest, DemoteWritesTheLegacyParameters) {
  GridRig rig;
  load(rig, lanes({gainBlock("blk-l")}, {gainBlock("blk-r")}, true));
  rig.run("grid.convert", kToGrid);
  rig.run("row.setMix", R"({"row":0,"pan":0.3,"volumeDb":-3,"invert":true})");
  rig.run("row.setMix", R"({"row":1,"pan":0.9,"volumeDb":3,"solo":true})");
  rig.run("grid.convert", kToLegacy);
  const auto value = [&](const char* id) {
    return rig.proc.parameters.getRawParameterValue(id)->load();
  };
  EXPECT_NEAR(value("chainPanLeft"), 0.3f, 1e-6f);
  EXPECT_NEAR(value("chainPanRight"), 0.9f, 1e-6f);
  EXPECT_NEAR(value("outputBalance"), 0.5f + 3.0f / 24.0f, 1e-6f);
  EXPECT_GE(value("chainInvertLeft"), 0.5f);
  EXPECT_LT(value("chainInvertRight"), 0.5f);
  EXPECT_LT(value("chainSoloLeft"), 0.5f);
  EXPECT_GE(value("chainSoloRight"), 0.5f);
  EXPECT_TRUE(rig.proc.isStereoMode());
}

TEST(GridMappingTest, AGridOutsideTheLegacyShapesSaysWhy) {
  struct Case {
    const char* command;
    const char* args;
    const char* reason;
  };
  const Case cases[] = {
      {"row.setOutput", R"({"row":0,"target":"device","pair":2})", "output pair 2"},
      {"row.setInput", R"({"row":0,"source":"device","channel":3})", "past the first two"},
      {"row.setInput", R"({"row":2,"source":"device","channel":1})", "row 3 has an input"},
      {"row.setMix", R"({"row":0,"mute":true})", "muted"},
      {"row.setMix", R"({"row":0,"volumeDb":-3})", "volume, pan or polarity"},
      {"row.setInputStage", R"({"row":0,"trimDb":2})", "input trim"},
      {"row.setInputStage", R"({"row":0,"gate":false})", "noise gate"},
      {"row.setFormat", R"({"row":0,"format":"mono"})", "single mono row"},
      {"chain.addBlock", R"({"kind":"utility.gain","row":3,"slot":2})", "row 4 holds blocks"},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.args);
    GridRig rig(2, 4);
    rig.run("grid.convert", kToGrid);
    ASSERT_TRUE(static_cast<bool>(rig.proc.getGridState()["legacyProjectable"]));
    rig.run(c.command, c.args);
    EXPECT_FALSE(static_cast<bool>(rig.proc.getGridState()["legacyProjectable"]));
    const juce::String blockers = rig.proc.legacyProjectionBlockers().joinIntoString("; ");
    EXPECT_TRUE(blockers.contains(c.reason)) << blockers;
    const CommandResult result = rig.tryRun("grid.convert", kToLegacy);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.errorMessage.contains(c.reason)) << result.errorMessage;
    EXPECT_TRUE(rig.proc.isGridMode());
  }
}

// ---------------------------------------------------------------------------
// Undo
// ---------------------------------------------------------------------------

TEST(GridUndoTest, UndoAcrossAConversionRestoresTheMode) {
  GridRig rig;
  load(rig, lanes({makeIrBlockTree("blk-cab", 2, 200)}, {}, false));
  const juce::String legacyJson = stableChainJson(rig.proc);

  rig.run("grid.convert", kToGrid);
  const juce::String gridJson = stableChainJson(rig.proc);
  ASSERT_TRUE(rig.proc.undoChain());
  ASSERT_TRUE(waitForChainLoaded(rig.proc));
  EXPECT_FALSE(rig.proc.isGridMode());
  EXPECT_EQ(stableChainJson(rig.proc), legacyJson);
  EXPECT_EQ(rig.proc.validateActivePlanForTesting(), "");
  EXPECT_EQ(static_cast<int>(parseSaved(savedBytes(rig.proc)).getProperty("schemaVersion")), 1);

  ASSERT_TRUE(rig.proc.redoChain());
  ASSERT_TRUE(waitForChainLoaded(rig.proc));
  EXPECT_TRUE(rig.proc.isGridMode());
  EXPECT_EQ(stableChainJson(rig.proc), gridJson);
  EXPECT_EQ(rig.proc.validateActivePlanForTesting(), "");

  // And back to legacy by conversion, then undo that: grid again.
  rig.run("grid.convert", kToLegacy);
  EXPECT_FALSE(rig.proc.isGridMode());
  ASSERT_TRUE(rig.proc.undoChain());
  EXPECT_TRUE(rig.proc.isGridMode());
  EXPECT_EQ(stableChainJson(rig.proc), gridJson);
}

// A block that a restore brings back from another row keeps its engine: a
// tone block rebuilt from scratch would have to fetch its model again, and
// an undo snapshot carries no model bytes.
TEST(GridUndoTest, EnginesSurviveMovesAcrossRowsAndModes) {
  GridRig rig;
  load(rig, lanes({makeIrBlockTree("blk-cab", 2, 200)}, {}, false));
  const auto cab = [&] {
    const juce::var state = rig.proc.getChainState(-1);
    std::vector<juce::var> lists{state["chain"], state["chainRight"]};
    if (const auto* rows = state["grid"]["rows"].getArray())
      for (const auto& row : *rows)
        lists.push_back(row["blocks"]);
    for (const juce::var& list : lists)
      for (int i = 0; i < list.size(); ++i)
        if (list[i]["blockId"].toString() == "blk-cab")
          return list[i];
    return juce::var();
  };
  const auto expectLoaded = [&](const char* when) {
    SCOPED_TRACE(when);
    const juce::var block = cab();
    ASSERT_TRUE(block.isObject());
    EXPECT_TRUE(static_cast<bool>(block["loaded"]));
    EXPECT_FALSE(static_cast<bool>(block["modelLoading"])) << "the engine was rebuilt";
    EXPECT_FALSE(static_cast<bool>(block["loadFailed"]));
  };

  // A move on the legacy rig converts it and moves the block to row 2 in
  // one step; the undo brings it back to the left lane of a legacy rig.
  rig.run("grid.moveBlock", R"({"blockId":"blk-cab","row":2,"slot":4})");
  expectLoaded("after the move");
  ASSERT_TRUE(rig.proc.undoChain());
  EXPECT_FALSE(rig.proc.isGridMode());
  expectLoaded("after the undo into legacy");
  ASSERT_TRUE(rig.proc.redoChain());
  EXPECT_TRUE(rig.proc.isGridMode());
  EXPECT_EQ(rig.proc.gridSlotBlockId(2, 4), "blk-cab");
  expectLoaded("after the redo into the grid");

  // Within the grid, across rows.
  rig.run("grid.moveBlock", R"({"blockId":"blk-cab","row":1,"slot":0,"mode":"insert"})");
  ASSERT_TRUE(rig.proc.undoChain());
  EXPECT_EQ(rig.proc.gridSlotBlockId(2, 4), "blk-cab");
  expectLoaded("after the undo of a cross-row move");
  EXPECT_EQ(rig.proc.validateActivePlanForTesting(), "");
}

TEST(GridUndoTest, ARowEditOnALegacyRigIsOneUndoStepWithItsConversion) {
  GridRig rig(2, 4);
  load(rig, lanes({gainBlock("blk-a")}, {}, false));
  const juce::String legacyJson = stableChainJson(rig.proc);
  ASSERT_FALSE(static_cast<bool>(rig.proc.getChainState(-1)["canUndo"]));

  const juce::var edited = rig.run("row.setOutput", R"({"row":0,"target":"device","pair":2})");
  EXPECT_EQ(edited["mode"].toString(), "grid");
  EXPECT_EQ(static_cast<int>(edited["row"]["output"]["pair"]), 2);
  EXPECT_TRUE(rig.proc.isGridMode());

  ASSERT_TRUE(rig.proc.undoChain());
  EXPECT_FALSE(rig.proc.isGridMode());
  EXPECT_EQ(stableChainJson(rig.proc), legacyJson);
  EXPECT_FALSE(static_cast<bool>(rig.proc.getChainState(-1)["canUndo"])) << "one step, not two";

  // Row edits in grid mode are undo steps of their own; a run of mix moves
  // on one row is one.
  rig.run("grid.convert", kToGrid);
  rig.run("row.setMix", R"({"row":0,"volumeDb":-3})");
  rig.run("row.setMix", R"({"row":0,"volumeDb":-6})");
  rig.run("row.setMix", R"({"row":0,"volumeDb":-9})");
  ASSERT_TRUE(rig.proc.undoChain());
  EXPECT_TRUE(rig.proc.isGridMode());
  EXPECT_EQ(static_cast<double>(rig.row(0)["output"]["volumeDb"]), 0.0);
  ASSERT_TRUE(rig.proc.redoChain());
  EXPECT_NEAR(static_cast<double>(rig.row(0)["output"]["volumeDb"]), -9.0, 1e-6);
}

// ---------------------------------------------------------------------------
// Saved state
// ---------------------------------------------------------------------------

// A grid with two rows on two outputs, a block in a row that is not heard,
// and every row setting off its default somewhere.
void buildSavedRig(GridRig& rig) {
  load(rig, lanes({makeIrBlockTree("blk-cab", 2, 200), gainBlock("blk-a", 3.0)}, {}, false));
  rig.run("grid.convert", kToGrid);
  rig.run("row.setInput", R"({"row":1,"source":"device","channel":2})");
  rig.run("row.setOutput", R"({"row":1,"target":"device","pair":2})");
  rig.run("row.setFormat", R"({"row":1,"format":"mono"})");
  rig.run("row.setMix", R"({"row":1,"volumeDb":-4.5,"pan":0.25,"invert":true,"solo":true})");
  rig.run("row.setInputStage", R"({"row":1,"trimDb":3.5,"gate":false,"pitch":true})");
  rig.run("row.setMix", R"({"row":0,"mute":true})");
  rig.addKind("utility.gain", 1, 4, "gainDb=-2");
  rig.addKind("utility.gain", 3, 7, "gainDb=1");
  ASSERT_TRUE(waitForChainLoaded(rig.proc));
}

// The grid as reported, without the figures that are measured.
juce::String stableGridJson(TONE3000Processor& proc) {
  const juce::var state = proc.getGridState();
  juce::DynamicObject::Ptr copy = new juce::DynamicObject();
  for (const auto& property : state.getDynamicObject()->getProperties())
    if (property.name.toString() != "cpu" && property.name.toString() != "rows")
      copy->setProperty(property.name, property.value);
  juce::Array<juce::var> rows;
  for (int i = 0; i < state["rows"].size(); ++i) {
    juce::DynamicObject::Ptr row = new juce::DynamicObject();
    for (const auto& property : state["rows"][i].getDynamicObject()->getProperties())
      if (property.name.toString() != "cpu")
        row->setProperty(property.name, property.value);
    rows.add(juce::var(row.get()));
  }
  copy->setProperty("rows", rows);
  return juce::JSON::toString(juce::var(copy.get()));
}

TEST(GridStateTest, SavedShapeIsSchemaTwoWithAGridNode) {
  GridRig rig(2, 4);
  buildSavedRig(rig);
  const juce::MemoryBlock saved = savedBytes(rig.proc);
  EXPECT_TRUE(saved == savedBytes(rig.proc)) << "saving twice gives the same bytes";

  const juce::ValueTree tree = parseSaved(saved);
  ASSERT_TRUE(tree.isValid());
  EXPECT_EQ(static_cast<int>(tree.getProperty("schemaVersion")), 2);
  const juce::ValueTree snapshot = tree.getChildWithName("ChainSnapshot");
  EXPECT_EQ(snapshot.getProperty("layout").toString(), "grid");
  EXPECT_FALSE(snapshot.hasProperty("stereoEnabled"));
  EXPECT_FALSE(snapshot.getChildWithName("ChainBlocks").isValid());

  const juce::ValueTree gridNode = snapshot.getChildWithName("Grid");
  ASSERT_TRUE(gridNode.isValid());
  EXPECT_EQ(static_cast<int>(gridNode.getProperty("version")), 1);
  EXPECT_EQ(static_cast<int>(gridNode.getProperty("minReader")), 1);
  EXPECT_EQ(static_cast<int>(gridNode.getProperty("rows")), 4);
  EXPECT_EQ(static_cast<int>(gridNode.getProperty("cols")), 8);
  ASSERT_EQ(gridNode.getNumChildren(), 4);
  for (int row = 0; row < 4; ++row) {
    const juce::ValueTree rowNode = gridNode.getChild(row);
    EXPECT_TRUE(rowNode.hasType("Row"));
    EXPECT_EQ(static_cast<int>(rowNode.getProperty("index")), row);
    EXPECT_TRUE(rowNode.getChildWithName("Input").isValid());
    EXPECT_TRUE(rowNode.getChildWithName("Output").isValid());
    const juce::ValueTree slots = rowNode.getChildWithName("Slots");
    ASSERT_EQ(slots.getNumChildren(), 8) << "exactly cols block nodes";
    for (int s = 0; s < 8; ++s)
      EXPECT_TRUE(slots.getChild(s).hasType("ChainBlock"));
  }
  const juce::ValueTree second = gridNode.getChild(1);
  EXPECT_EQ(second.getProperty("format").toString(), "mono");
  const juce::ValueTree input = second.getChildWithName("Input");
  EXPECT_EQ(input.getProperty("type").toString(), "device");
  EXPECT_EQ(static_cast<int>(input.getProperty("channel")), 1) << "0-based in the file";
  EXPECT_FALSE(static_cast<bool>(input.getProperty("stereo")));
  EXPECT_NEAR(static_cast<double>(input.getProperty("trimDb")), 3.5, 1e-6);
  EXPECT_FALSE(static_cast<bool>(input.getProperty("gate")));
  EXPECT_TRUE(static_cast<bool>(input.getProperty("pitch")));
  const juce::ValueTree output = second.getChildWithName("Output");
  EXPECT_EQ(output.getProperty("type").toString(), "device");
  EXPECT_EQ(static_cast<int>(output.getProperty("pair")), 1);
  EXPECT_NEAR(static_cast<double>(output.getProperty("volumeDb")), -4.5, 1e-6);
  EXPECT_NEAR(static_cast<double>(output.getProperty("pan")), 0.25, 1e-6);
  EXPECT_TRUE(static_cast<bool>(output.getProperty("invert")));
  EXPECT_TRUE(static_cast<bool>(output.getProperty("solo"))) << "solo is saved with the session";
  EXPECT_EQ(second.getChildWithName("Slots").getChild(4).getProperty("type").toString(), "kind");
  EXPECT_EQ(second.getChildWithName("Slots").getChild(0).getProperty("type").toString(), "insert");
  // The IR's model bytes ride its block node, as in a legacy save.
  EXPECT_TRUE(gridNode.getChild(0).getChildWithName("Slots").getChild(0).getChildWithName("ModelCache").isValid());
  std::cout << "[grid state] " << gridNode.getChild(1).createCopy().toXmlString().toStdString();
}

TEST(GridStateTest, SaveRestoreRoundTrip) {
  GridRig rig(2, 4);
  buildSavedRig(rig);
  const juce::MemoryBlock saved = savedBytes(rig.proc);
  const juce::String gridJson = stableGridJson(rig.proc);

  GridRig second(2, 4);
  second.proc.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
  ASSERT_TRUE(waitForChainLoaded(second.proc));
  EXPECT_TRUE(second.proc.isGridMode());
  EXPECT_EQ(stableGridJson(second.proc), gridJson);
  EXPECT_EQ(second.proc.validateActivePlanForTesting(), "");
  EXPECT_TRUE(savedBytes(second.proc) == saved) << "save, restore, save moved the bytes";
  EXPECT_FALSE(static_cast<bool>(second.proc.getChainState(-1)["canUndo"]));

  // Over a processor that already holds the rig: reconciled, same bytes.
  second.proc.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
  ASSERT_TRUE(waitForChainLoaded(second.proc));
  EXPECT_TRUE(savedBytes(second.proc) == saved);

  // And the audio is the same rig.
  rig.settle();
  second.settle();
  const GridChannels in{makeNoise(4096, 41, 0.2f), makeNoise(4096, 42, 0.2f)};
  const GridChannels a = rig.process(in), b = second.process(in);
  EXPECT_GT(gridPeak(a[2]), 0.01f);
  for (size_t ch = 0; ch < a.size(); ++ch)
    EXPECT_EQ(a[ch], b[ch]) << "channel " << ch;

  // A legacy state loaded over a grid leaves grid mode behind entirely.
  GridRig legacy;
  load(legacy, lanes({gainBlock("blk-x")}, {}, false));
  const juce::MemoryBlock legacyBytes = savedBytes(legacy.proc);
  second.proc.setStateInformation(legacyBytes.getData(), static_cast<int>(legacyBytes.getSize()));
  ASSERT_TRUE(waitForChainLoaded(second.proc));
  EXPECT_FALSE(second.proc.isGridMode());
  EXPECT_TRUE(savedBytes(second.proc) == legacyBytes);
  EXPECT_EQ(second.proc.validateActivePlanForTesting(), "");
}

TEST(GridStateTest, ANewerGridFormatIsRefusedWhole) {
  GridRig rig(2, 4);
  buildSavedRig(rig);
  juce::ValueTree tree = parseSaved(savedBytes(rig.proc));
  ASSERT_TRUE(tree.isValid());
  tree.getChildWithName("ChainSnapshot").getChildWithName("Grid").setProperty("minReader", 3, nullptr);
  // It carries a parameter move too, which must not be half applied.
  juce::ValueTree params = tree.getChildWithName("PARAMETERS");
  ASSERT_TRUE(params.isValid());
  const juce::MemoryBlock newer = frame(tree);

  GridRig target;
  load(target, lanes({gainBlock("blk-keep")}, {}, false));
  target.proc.parameters.getParameter("inputLevel")->setValueNotifyingHost(0.7f);
  const juce::String before = stableChainJson(target.proc);
  target.proc.setStateInformation(newer.getData(), static_cast<int>(newer.getSize()));
  EXPECT_FALSE(target.proc.isGridMode());
  EXPECT_EQ(stableChainJson(target.proc), before);
  EXPECT_NEAR(target.proc.parameters.getParameter("inputLevel")->getValue(), 0.7f, 1e-6f);

  // The same node at minReader 1 loads.
  tree.getChildWithName("ChainSnapshot").getChildWithName("Grid").setProperty("minReader", 1, nullptr);
  const juce::MemoryBlock readable = frame(tree);
  target.proc.setStateInformation(readable.getData(), static_cast<int>(readable.getSize()));
  ASSERT_TRUE(waitForChainLoaded(target.proc));
  EXPECT_TRUE(target.proc.isGridMode());
}

TEST(GridStateTest, UnknownTypesLoadInertAndAreWrittenBack) {
  GridRig rig(2, 4);
  buildSavedRig(rig);
  juce::ValueTree tree = parseSaved(savedBytes(rig.proc));
  juce::ValueTree gridNode = tree.getChildWithName("ChainSnapshot").getChildWithName("Grid");
  ASSERT_TRUE(gridNode.isValid());

  // What a later build might write: a split of a type this one has not got,
  // an input and an output of new types, a format name, and properties
  // nobody here has heard of.
  juce::ValueTree split("Split");
  split.setProperty("id", "split-1", nullptr);
  split.setProperty("upperRow", 2, nullptr);
  split.setProperty("splitCol", 2, nullptr);
  split.setProperty("mixCol", 6, nullptr);
  split.setProperty("type", "multiband", nullptr);
  juce::ValueTree splitParam("Param");
  splitParam.setProperty("id", "split.crossoverHz", nullptr);
  splitParam.setProperty("value", 250.0, nullptr);
  split.appendChild(splitParam, nullptr);
  gridNode.appendChild(split, nullptr);

  juce::ValueTree thirdRow = gridNode.getChild(2);
  thirdRow.getChildWithName("Input").setProperty("type", "network", nullptr);
  thirdRow.getChildWithName("Input").setProperty("address", "10.0.0.7", nullptr);
  thirdRow.getChildWithName("Output").setProperty("type", "recorder", nullptr);
  thirdRow.setProperty("format", "quad", nullptr);
  gridNode.setProperty("futureThing", 42, nullptr);
  gridNode.getChild(0).getChildWithName("Output").setProperty("futureKnob", 0.3, nullptr);
  const juce::MemoryBlock future = frame(tree);

  GridRig target(2, 4);
  target.proc.setStateInformation(future.getData(), static_cast<int>(future.getSize()));
  ASSERT_TRUE(waitForChainLoaded(target.proc));
  ASSERT_TRUE(target.proc.isGridMode());
  EXPECT_EQ(target.proc.validateActivePlanForTesting(), "");
  // The row with the unknown ends does not run; the rest of the rig does.
  EXPECT_FALSE(static_cast<bool>(target.row(2)["routed"]));
  EXPECT_EQ(target.row(2)["input"]["type"].toString(), "network");
  EXPECT_EQ(target.row(2)["output"]["type"].toString(), "recorder");
  EXPECT_EQ(target.row(2)["format"].toString(), "quad");
  EXPECT_TRUE(static_cast<bool>(target.row(1)["routed"]));
  EXPECT_FALSE(static_cast<bool>(target.proc.getGridState()["legacyProjectable"]));

  const juce::ValueTree again =
      parseSaved(savedBytes(target.proc)).getChildWithName("ChainSnapshot").getChildWithName("Grid");
  const juce::ValueTree keptSplit = again.getChildWithName("Split");
  ASSERT_TRUE(keptSplit.isValid()) << "the split was dropped";
  EXPECT_TRUE(keptSplit.isEquivalentTo(split));
  // It is listed, and runs nothing: row 3 below it is not fed.
  const juce::var listed = target.proc.getGridState()["splits"];
  ASSERT_EQ(listed.size(), 1);
  EXPECT_TRUE(static_cast<bool>(listed[0]["inert"]));
  EXPECT_EQ(listed[0]["type"].toString(), "multiband");
  EXPECT_FALSE(static_cast<bool>(target.row(3)["routed"]));
  EXPECT_EQ(static_cast<int>(again.getProperty("minReader")), 2)
      << "a rig with a split is not for a build that knows none";
  const juce::ValueTree keptRow = again.getChild(2);
  EXPECT_EQ(keptRow.getProperty("format").toString(), "quad");
  EXPECT_TRUE(keptRow.getChildWithName("Input").isEquivalentTo(thirdRow.getChildWithName("Input")));
  EXPECT_TRUE(keptRow.getChildWithName("Output").isEquivalentTo(thirdRow.getChildWithName("Output")));
  // Unknown properties are ignored, not kept.
  EXPECT_FALSE(again.hasProperty("futureThing"));

  // Setting the row's input by command replaces the unknown node.
  target.run("row.setInput", R"({"row":2,"source":"device","channel":1})");
  EXPECT_EQ(target.row(2)["input"]["type"].toString(), "device");
}

TEST(GridStateTest, PresetsCarryTheGridButNotSolo) {
  const juce::File store = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile("pedaleira-grid-preset-" + juce::Uuid().toString());
  struct Cleanup {
    juce::File dir;
    ~Cleanup() { dir.deleteRecursively(); }
  } cleanup{store};

  GridRig rig(2, 4);
  rig.proc.setPresetStoreForTesting(store);
  buildSavedRig(rig);
  ASSERT_TRUE(static_cast<bool>(rig.row(1)["output"]["solo"]));
  const juce::var preset = rig.proc.savePreset("Grid Rig");
  ASSERT_TRUE(preset.isObject());
  const juce::String presetId = preset["id"].toString();
  const juce::String gridJson = stableGridJson(rig.proc).replace("\"solo\": true", "\"solo\": false");

  // From the factory default (a legacy rig) into the grid preset, and back
  // out with one undo.
  ASSERT_TRUE(rig.proc.resetToDefault());
  EXPECT_FALSE(rig.proc.isGridMode());
  ASSERT_TRUE(rig.proc.loadPreset(presetId));
  ASSERT_TRUE(waitForChainLoaded(rig.proc));
  EXPECT_TRUE(rig.proc.isGridMode());
  EXPECT_EQ(stableGridJson(rig.proc), gridJson);
  EXPECT_FALSE(static_cast<bool>(rig.row(1)["output"]["solo"]));
  EXPECT_EQ(static_cast<int>(rig.row(1)["input"]["channel"]), 2) << "device bindings are saved";
  EXPECT_EQ(rig.proc.validateActivePlanForTesting(), "");
  ASSERT_TRUE(rig.proc.undoChain());
  EXPECT_FALSE(rig.proc.isGridMode());

  // Loaded again, and then over itself (reconciled: the blocks keep their
  // ids and their engines).
  ASSERT_TRUE(rig.proc.loadPreset(presetId));
  ASSERT_TRUE(waitForChainLoaded(rig.proc));
  EXPECT_TRUE(rig.proc.isGridMode());
  ASSERT_TRUE(rig.proc.loadPreset(presetId));
  ASSERT_TRUE(waitForChainLoaded(rig.proc));
  EXPECT_EQ(stableGridJson(rig.proc), gridJson);
}

// ---------------------------------------------------------------------------
// Scenes and stomps
// ---------------------------------------------------------------------------

TEST(GridStateTest, ScenesAndStompsSurviveConversionAndWorkInGridMode) {
  GridRig rig(2, 4);
  load(rig, lanes({gainBlock("blk-a", 1.0), gainBlock("blk-b", 2.0)}, {}, false));
  // Scene B: blk-a off, and blk-b's gain has a value per scene.
  rig.run("scene.add");
  rig.run("scene.assignParam", R"({"blockId":"blk-b","param":"gainDb"})");
  rig.run("scene.select", R"({"scene":"B"})");
  ASSERT_TRUE(rig.proc.setBlockParam("blk-a", "enabled", 0.0));
  ASSERT_TRUE(rig.proc.setBlockParam("blk-b", "gainDb", -7.0));
  rig.run("scene.select", R"({"scene":"A"})");
  rig.run("stomp.assign", R"({"switch":"A","blockIds":["blk-b"]})");
  const juce::String scenes = juce::JSON::toString(rig.proc.getScenesState());
  const juce::String stomps = juce::JSON::toString(rig.proc.getStompsState());

  rig.run("grid.convert", kToGrid);
  EXPECT_EQ(juce::JSON::toString(rig.proc.getScenesState()), scenes);
  EXPECT_EQ(juce::JSON::toString(rig.proc.getStompsState()), stomps);

  const auto block = [&](const char* id) {
    const juce::var row = rig.proc.getChainState(-1)["grid"]["rows"];
    for (int r = 0; r < row.size(); ++r)
      for (int i = 0; i < row[r]["blocks"].size(); ++i)
        if (row[r]["blocks"][i]["blockId"].toString() == id)
          return row[r]["blocks"][i];
    return juce::var();
  };
  // A block moved to another row is still the same block to the scenes.
  rig.run("grid.moveBlock", R"({"blockId":"blk-b","row":2,"slot":5})");
  rig.run("scene.select", R"({"scene":"B"})");
  EXPECT_FALSE(static_cast<bool>(block("blk-a")["params"]["enabled"]));
  EXPECT_NEAR(static_cast<double>(block("blk-b")["kindParams"]["gainDb"]), -7.0, 1e-6);
  rig.run("scene.select", R"({"scene":"A"})");
  EXPECT_TRUE(static_cast<bool>(block("blk-a")["params"]["enabled"]));
  EXPECT_NEAR(static_cast<double>(block("blk-b")["kindParams"]["gainDb"]), 2.0, 1e-6);

  // Scenes ride the grid's saved state and its undo history.
  const juce::MemoryBlock saved = savedBytes(rig.proc);
  GridRig second(2, 4);
  second.proc.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
  ASSERT_TRUE(waitForChainLoaded(second.proc));
  EXPECT_EQ(juce::JSON::toString(second.proc.getScenesState()),
            juce::JSON::toString(rig.proc.getScenesState()));
  EXPECT_EQ(juce::JSON::toString(second.proc.getStompsState()),
            juce::JSON::toString(rig.proc.getStompsState()));

  // Removing the block takes it out of the scenes and the stomp, as ever.
  rig.run("chain.removeBlock", R"({"blockId":"blk-b"})");
  EXPECT_FALSE(juce::JSON::toString(rig.proc.getStompsState()).contains("blk-b"));
  EXPECT_EQ(rig.row(2)["slots"].size(), kGridCols) << "the slot emptied in place";
}

// ---------------------------------------------------------------------------
// What a legacy-shaped client sees
// ---------------------------------------------------------------------------

TEST(GridLegacyViewTest, ChainStateKeepsTheLegacyShape) {
  GridRig rig(2, 4);
  load(rig, lanes({gainBlock("blk-a")}, {gainBlock("blk-r")}, false));
  const juce::var legacy = rig.proc.getChainState(-1);
  EXPECT_TRUE(legacy["grid"].isVoid()) << "a legacy rig's chain state is unchanged";
  EXPECT_TRUE(legacy["legacyEditable"].isVoid());

  rig.run("grid.convert", kToGrid);
  juce::var state = rig.proc.getChainState(-1);
  // Projectable: the legacy client may still edit the routing.
  EXPECT_TRUE(static_cast<bool>(state["legacyEditable"]));
  EXPECT_EQ(state["inputMode"].toString(), "stereo");
  EXPECT_FALSE(static_cast<bool>(state["stereoEnabled"]));
  // The lane a legacy rig would have: the block and four empty slots, the
  // same ones (by id) the legacy rig listed.
  ASSERT_EQ(state["chain"].size(), legacy["chain"].size());
  for (int i = 0; i < state["chain"].size(); ++i)
    EXPECT_EQ(state["chain"][i]["blockId"].toString(), legacy["chain"][i]["blockId"].toString());
  EXPECT_EQ(state["grid"]["rows"][0]["blocks"].size(), kGridCols) << "the grid lists every slot";
  EXPECT_EQ(state["chain"][0]["blockId"].toString(), "blk-a");
  EXPECT_EQ(state["chain"][1]["kind"].toString(), "insert");
  EXPECT_TRUE(state["chainRight"].isVoid()) << "row 1 is not heard";
  EXPECT_TRUE(state["branch"].isVoid());
  EXPECT_FALSE(static_cast<bool>(state["atDefault"]));
  EXPECT_FALSE(static_cast<bool>(state["dualMonoActive"]));
  ASSERT_EQ(state["grid"]["rows"].size(), kGridRows);
  EXPECT_EQ(state["grid"]["rows"][1]["blocks"][0]["blockId"].toString(), "blk-r");

  // Not projectable: rows 0 and 1 as the two chains, rows 2 and 3 only in
  // the grid object, and the routing is read-only for a legacy client.
  rig.run("row.setInput", R"({"row":1,"source":"device","channel":2})");
  rig.run("row.setOutput", R"({"row":1,"target":"device","pair":2})");
  rig.addKind("utility.gain", 3, 0);
  state = rig.proc.getChainState(-1);
  EXPECT_FALSE(static_cast<bool>(state["legacyEditable"]));
  EXPECT_EQ(state["inputMode"].toString(), "grid");
  EXPECT_TRUE(static_cast<bool>(state["stereoEnabled"]));
  ASSERT_EQ(state["chainRight"].size(), kGridCols);
  EXPECT_EQ(state["chainRight"][0]["blockId"].toString(), "blk-r");
  EXPECT_EQ(state["grid"]["rows"][3]["blocks"][0]["kind"].toString(), "tone");

  // Legacy routing edits fail safely and change nothing.
  const juce::String before = stableChainJson(rig.proc);
  rig.proc.setStereoMode(false);
  rig.proc.setInputMode(InputMode::Left);
  EXPECT_FALSE(rig.proc.swapChains());
  EXPECT_FALSE(rig.proc.setChainBranch("left", "blk-a"));
  EXPECT_FALSE(rig.proc.clearChainBranch());
  EXPECT_FALSE(rig.proc.moveBlockToChain("blk-a", "right", 0));
  // An order that is not all of a row's slots is a legacy lane's, and this
  // grid is no legacy rig.
  EXPECT_FALSE(rig.proc.reorderChainBlocks({"blk-a"}));
  EXPECT_TRUE(rig.proc.isGridMode());
  EXPECT_EQ(stableChainJson(rig.proc), before);

  // Block operations by id keep working, in any row.
  EXPECT_TRUE(rig.proc.setBlockParam("blk-r", "gainDb", 4.0));
  EXPECT_TRUE(rig.proc.setBlockParam("blk-r", "enabled", 0.0));
  std::vector<std::string> order;
  for (int i = kGridCols - 1; i >= 0; --i)
    order.push_back(rig.row(0)["slots"][i].toString().toStdString());
  EXPECT_TRUE(rig.proc.reorderChainBlocks(order));
  EXPECT_EQ(rig.proc.gridSlotBlockId(0, kGridCols - 1), "blk-a");
  // A new block into a named empty slot, and into "the first empty slot".
  const std::string slot = rig.proc.gridSlotBlockId(1, 6);
  const juce::var added = rig.proc.addKindBlock("utility.gain", slot);
  ASSERT_TRUE(added["blockId"].toString().isNotEmpty()) << added["error"].toString();
  EXPECT_EQ(rig.proc.gridSlotBlockId(1, 6), added["blockId"].toString().toStdString());
  EXPECT_TRUE(rig.proc.addKindBlock("utility.gain")["blockId"].toString().isNotEmpty());
  // Removing empties the slot in place: the columns do not shift.
  EXPECT_TRUE(rig.proc.removeChainBlock("blk-a"));
  EXPECT_EQ(rig.row(0)["slots"].size(), kGridCols);
  EXPECT_NE(rig.proc.gridSlotBlockId(0, kGridCols - 1), "blk-a");
  // A duplicate lands in the row, which keeps its eight slots.
  EXPECT_FALSE(rig.proc.duplicateChainBlock("blk-r", "right", 1).empty());
  EXPECT_EQ(rig.row(1)["slots"].size(), kGridCols);
  EXPECT_EQ(rig.proc.validateActivePlanForTesting(), "");
  EXPECT_EQ(rig.proc.stalePlanCallbacksForTesting(), 0);

  // A full grid has no room, and says so instead of growing a row.
  GridRig full(2, 2);
  full.run("grid.convert", kToGrid);
  for (int i = 0; i < kGridRows * kGridCols; ++i)
    ASSERT_TRUE(full.proc.addKindBlock("utility.gain")["blockId"].toString().isNotEmpty()) << i;
  EXPECT_TRUE(full.proc.addKindBlock("utility.gain")["error"].toString().contains("no empty slot"));
  for (int row = 0; row < kGridRows; ++row)
    EXPECT_EQ(full.row(row)["slots"].size(), kGridCols);
  EXPECT_TRUE(full.proc.duplicateChainBlock(full.proc.gridSlotBlockId(0, 0), "left", 1).empty());
  EXPECT_EQ(full.proc.validateActivePlanForTesting(), "");
}

// A projectable grid: the legacy routing edits convert it back and then do
// what they always did.
TEST(GridLegacyViewTest, LegacyRoutingEditsConvertAProjectableGridBack) {
  GridRig rig;
  load(rig, lanes({gainBlock("blk-a")}, {gainBlock("blk-r")}, false));
  rig.run("grid.convert", kToGrid);
  rig.proc.setStereoMode(true);
  EXPECT_FALSE(rig.proc.isGridMode());
  EXPECT_TRUE(rig.proc.isStereoMode());
  EXPECT_EQ(rig.proc.validateActivePlanForTesting(), "");

  rig.run("grid.convert", kToGrid);
  ASSERT_TRUE(rig.proc.isGridMode());
  EXPECT_TRUE(rig.proc.swapChains());
  EXPECT_FALSE(rig.proc.isGridMode());
  EXPECT_EQ(rig.proc.getChainState(-1)["chain"][0]["blockId"].toString(), "blk-r");

  rig.run("grid.convert", kToGrid);
  rig.proc.setInputMode(InputMode::Right);
  EXPECT_FALSE(rig.proc.isGridMode());
  EXPECT_EQ(rig.proc.getInputMode(), InputMode::Right);
}

}  // namespace

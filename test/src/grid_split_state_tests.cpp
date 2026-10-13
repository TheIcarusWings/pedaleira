// Splits and row feeds as state and as commands: the split.* commands and
// what they refuse, the saved Split node and the format it raises the Grid
// node to, undo, presets, scenes and stomps around them, a branched legacy
// rig there and back, and what the current UI's image controls do to a grid.

#include "SplitJunction.h"
#include "grid_test_helpers.h"
#include "test_block_kinds.h"

#include <gtest/gtest.h>

#include <cmath>
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

juce::ValueTree gridNodeOf(TONE3000Processor& proc) {
  return parseSaved(savedBytes(proc)).getChildWithName("ChainSnapshot").getChildWithName("Grid");
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

float paramValue(GridRig& rig, const char* id) {
  return rig.proc.parameters.getRawParameterValue(id)->load();
}

void setParam(GridRig& rig, const char* id, float realValue) {
  auto* param = rig.proc.parameters.getParameter(id);
  ASSERT_NE(param, nullptr) << id;
  param->setValueNotifyingHost(param->convertTo0to1(realValue));
}

// The grid as reported, without what is measured.
juce::String stableGridJson(TONE3000Processor& proc) {
  // The latency a host is told follows a background install by a message
  // loop turn, which a test does not run: deliver it now, as the render
  // tool does.
  proc.flushDeferredBlockRefresh();
  const juce::var state = proc.getGridState();
  juce::DynamicObject::Ptr copy = new juce::DynamicObject();
  for (const auto& property : state.getDynamicObject()->getProperties())
    if (property.name.toString() != "cpu" && property.name.toString() != "rows" &&
        property.name.toString() != "splits")
      copy->setProperty(property.name, property.value);
  juce::Array<juce::var> rows, splits;
  for (int i = 0; i < state["rows"].size(); ++i) {
    juce::DynamicObject::Ptr row = new juce::DynamicObject();
    for (const auto& property : state["rows"][i].getDynamicObject()->getProperties())
      if (property.name.toString() != "cpu")
        row->setProperty(property.name, property.value);
    rows.add(juce::var(row.get()));
  }
  for (int i = 0; i < state["splits"].size(); ++i) {
    juce::DynamicObject::Ptr split = new juce::DynamicObject();
    for (const auto& property : state["splits"][i].getDynamicObject()->getProperties())
      if (property.name.toString() != "meters")
        split->setProperty(property.name, property.value);
    splits.add(juce::var(split.get()));
  }
  copy->setProperty("rows", rows);
  copy->setProperty("splits", splits);
  return juce::JSON::toString(juce::var(copy.get()));
}

// A refused command: the reason, and nothing changed (the grid, the plan the
// audio thread runs, the undo history).
void expectRefused(GridRig& rig, const char* command, const char* args, const char* code,
                   const char* messagePart) {
  SCOPED_TRACE(std::string(command) + " " + args);
  const juce::String grid = stableGridJson(rig.proc);
  const std::string plan = rig.proc.describeActivePlanForTesting();
  const bool canUndo = static_cast<bool>(rig.proc.getChainState(-1)["canUndo"]);
  const bool canRedo = static_cast<bool>(rig.proc.getChainState(-1)["canRedo"]);
  const CommandResult result = rig.tryRun(command, args);
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.errorCode, juce::String(code));
  EXPECT_TRUE(result.errorMessage.containsIgnoreCase(messagePart)) << result.errorMessage;
  EXPECT_EQ(stableGridJson(rig.proc), grid) << "a refused command changed the grid";
  // The plan is recompiled by the roll-back, so its revision moves; what it
  // runs does not.
  const auto body = [](const std::string& text) { return text.substr(text.find('\n')); };
  EXPECT_EQ(body(rig.proc.describeActivePlanForTesting()), body(plan));
  EXPECT_EQ(rig.proc.validateActivePlanForTesting(), "");
  EXPECT_EQ(static_cast<bool>(rig.proc.getChainState(-1)["canUndo"]), canUndo);
  EXPECT_EQ(static_cast<bool>(rig.proc.getChainState(-1)["canRedo"]), canRedo);
}

// Row 0 split into row 1 and rejoined, row 1 split into row 2 inside that:
// blocks in each path, and every junction with a setting off its default.
void buildSplitRig(GridRig& rig) {
  load(rig, lanes({makeIrBlockTree("blk-cab", 2, 200), gainBlock("blk-a", 3.0)}, {}, false));
  rig.run("grid.convert", kToGrid);
  rig.run("split.add", R"({"row":0,"splitAt":2,"mixAt":7,"type":"crossover"})");
  rig.run("split.add", R"({"row":1,"splitAt":3,"mixAt":5,"type":"ab"})");
  rig.run("split.setParam", R"({"row":0,"param":"split.crossoverHz","value":320})");
  rig.run("split.setParam", R"({"row":0,"param":"mix.levelB","value":-4.5})");
  rig.run("split.setParam", R"({"row":0,"param":"mix.panB","value":0.25})");
  rig.run("split.setParam", R"({"row":1,"param":"split.levelA","value":-2})");
  rig.run("split.setParam", R"({"row":1,"param":"mix.invertB","value":1})");
  rig.addKind("utility.gain", 1, 2, "gainDb=-2");
  rig.addKind(TestLatencyProcessor::kKindId, 1, 4);
  rig.addKind("utility.gain", 2, 3, "gainDb=1");
  ASSERT_TRUE(waitForChainLoaded(rig.proc));
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

TEST(GridSplitCommandsTest, AddMoveTypeParamListRemove) {
  GridRig rig(2, 4);
  rig.run("grid.convert", kToGrid);
  rig.addKind("utility.gain", 1, 3);
  juce::var result = rig.run("split.add", R"({"row":0,"splitAt":2,"mixAt":6})");
  juce::var split = result["split"];
  const juce::String id = split["id"].toString();
  EXPECT_TRUE(id.isNotEmpty());
  EXPECT_EQ(static_cast<int>(split["row"]), 0);
  EXPECT_EQ(static_cast<int>(split["lowerRow"]), 1);
  EXPECT_EQ(split["type"].toString(), "balance");
  EXPECT_TRUE(static_cast<bool>(split["active"]));
  EXPECT_TRUE(static_cast<bool>(split["lowerActive"]));
  EXPECT_EQ(static_cast<double>(split["params"]["mix.levelA"]), 0.0);
  EXPECT_EQ(static_cast<double>(split["params"]["split.crossoverHz"]), 500.0);
  EXPECT_TRUE(split["alignment"].hasProperty("a"));
  EXPECT_TRUE(split["meters"]["split"].isArray());
  ASSERT_EQ(result["splits"].size(), 1);
  EXPECT_TRUE(result.hasProperty("revision"));
  // The grid reports which slots of the lower row run.
  EXPECT_TRUE(static_cast<bool>(result["rows"][1]["active"][3]));
  EXPECT_TRUE(static_cast<bool>(result["rows"][1]["routed"]));
  ASSERT_EQ(result["groups"].size(), 1);
  EXPECT_EQ(result["groups"][0].size(), 2);

  // Move: either end, or the rejoin away and back. The id stays.
  result = rig.run("split.move", R"({"row":0,"splitAt":4})");
  EXPECT_EQ(static_cast<int>(result["split"]["splitAt"]), 4);
  EXPECT_EQ(static_cast<int>(result["split"]["mixAt"]), 6);
  EXPECT_FALSE(static_cast<bool>(result["rows"][1]["active"][3])) << "now before the fork";
  result = rig.run("split.move", R"({"row":0,"splitAt":1,"mixAt":8})");
  EXPECT_EQ(static_cast<int>(result["split"]["mixAt"]), 8);
  result = rig.run("split.move", R"({"row":0,"rejoin":false})");
  EXPECT_TRUE(result["split"]["mixAt"].isVoid());
  EXPECT_FALSE(static_cast<bool>(result["split"]["lowerActive"])) << "row 1 has no output";
  EXPECT_TRUE(static_cast<bool>(result["split"]["active"]));
  result = rig.run("split.move", R"({"row":0,"mixAt":5})");
  EXPECT_EQ(static_cast<int>(result["split"]["mixAt"]), 5);
  EXPECT_EQ(result["split"]["id"].toString(), id);
  EXPECT_EQ(rig.tryRun("split.move", R"({"row":0,"mixAt":5,"rejoin":false})").errorCode,
            juce::String(CommandError::invalidArgs));

  // Type and parameters.
  result = rig.run("split.setType", R"({"row":0,"type":"crossover"})");
  EXPECT_EQ(result["split"]["type"].toString(), "crossover");
  result = rig.run("split.setParam", R"({"row":0,"param":"split.crossoverHz","value":250})");
  EXPECT_EQ(static_cast<double>(result["split"]["params"]["split.crossoverHz"]), 250.0);
  result = rig.run("split.setParam", R"({"row":0,"param":"mix.levelB","value":99})");
  EXPECT_EQ(static_cast<double>(result["split"]["params"]["mix.levelB"]),
            static_cast<double>(SplitJunction::kLevelMaxDb))
      << "clamped to the range";
  EXPECT_EQ(rig.tryRun("split.setParam", R"({"row":0,"param":"mix.nope","value":1})").errorCode,
            juce::String(CommandError::invalidArgs));
  EXPECT_EQ(rig.tryRun("split.setParam", R"({"row":1,"param":"mix.level","value":1})").errorCode,
            juce::String(CommandError::notFound));
  EXPECT_EQ(rig.tryRun("split.setType", R"({"row":0,"type":"octave"})").errorCode,
            juce::String(CommandError::invalidArgs));
  EXPECT_EQ(rig.tryRun("split.add", R"({"row":3,"splitAt":1})").errorCode,
            juce::String(CommandError::invalidArgs))
      << "the last row has no row below it";

  const juce::var listed = rig.run("split.listParams", R"({"row":0})");
  EXPECT_EQ(listed["id"].toString(), id);
  EXPECT_EQ(listed["type"].toString(), "crossover");
  ASSERT_EQ(listed["params"].size(), static_cast<int>(SplitJunction::kNumParams));
  bool sawCrossover = false;
  for (int i = 0; i < listed["params"].size(); ++i) {
    const juce::var param = listed["params"][i];
    EXPECT_TRUE(param.hasProperty("id"));
    EXPECT_TRUE(param.hasProperty("value"));
    if (param["id"].toString() == "split.crossoverHz") {
      sawCrossover = true;
      EXPECT_EQ(static_cast<double>(param["value"]), 250.0);
      EXPECT_EQ(static_cast<double>(param["min"]), 20.0);
    }
  }
  EXPECT_TRUE(sawCrossover);
  EXPECT_EQ(rig.tryRun("split.listParams", R"({"row":2})").errorCode,
            juce::String(CommandError::notFound));

  // meters.get lists the split.
  const juce::var meters = rig.run("meters.get");
  ASSERT_EQ(meters["splits"].size(), 1);
  EXPECT_EQ(meters["splits"][0]["id"].toString(), id);
  EXPECT_TRUE(meters["splits"][0]["split"].isArray());

  // Remove: the lower row is fed by nothing, its blocks stay.
  result = rig.run("split.remove", R"({"row":0})");
  EXPECT_EQ(result["splits"].size(), 0);
  EXPECT_FALSE(static_cast<bool>(result["rows"][1]["routed"]));
  EXPECT_EQ(rig.proc.gridSlotBlockId(1, 3).empty(), false);
  EXPECT_EQ(rig.tryRun("split.remove", R"({"row":0})").errorCode,
            juce::String(CommandError::notFound));
  EXPECT_EQ(rig.tryRun("split.move", R"({"row":0,"splitAt":2})").errorCode,
            juce::String(CommandError::notFound));
  EXPECT_EQ(rig.proc.validateActivePlanForTesting(), "");
}

// Every shape the product rule forbids is refused with the reason, and the
// rig, the plan and the history stay as they were.
TEST(GridSplitCommandsTest, IllegalShapesAreRefusedAndChangeNothing) {
  GridRig rig(2, 4);
  rig.run("grid.convert", kToGrid);
  rig.addKind("utility.gain", 0, 0);
  rig.run("split.add", R"({"row":0,"splitAt":2,"mixAt":6})");
  rig.settle();

  expectRefused(rig, "split.add", R"({"row":0,"splitAt":1,"mixAt":3})", CommandError::failed,
                "already has a split");
  expectRefused(rig, "split.move", R"({"row":0,"splitAt":6})", CommandError::failed,
                "rejoin at a later one");
  expectRefused(rig, "split.move", R"({"row":0,"mixAt":2})", CommandError::failed,
                "rejoin at a later one");
  // A split of the lower row has to lie inside the one above it.
  expectRefused(rig, "split.add", R"({"row":1,"splitAt":1,"mixAt":4})", CommandError::failed,
                "inside the split above it");
  expectRefused(rig, "split.add", R"({"row":1,"splitAt":3,"mixAt":7})", CommandError::failed,
                "inside the split above it");
  // The lower row takes no input of its own, and nobody feeds it.
  expectRefused(rig, "row.setInput", R"({"row":1,"source":"device","channel":2})",
                CommandError::failed, "input of its own");
  expectRefused(rig, "row.setOutput", R"({"row":2,"target":"row","toRow":1})",
                CommandError::failed, "lower row of a split");
  // A row with an input cannot become a lower row.
  rig.run("row.setInput", R"({"row":3,"source":"device","channel":2})");
  expectRefused(rig, "split.add", R"({"row":2,"splitAt":1})", CommandError::failed,
                "row 3 has an input of its own");
  // Moving the parent so that the nested split falls outside it.
  rig.run("split.add", R"({"row":1,"splitAt":3,"mixAt":5})");
  expectRefused(rig, "split.move", R"({"row":0,"splitAt":4})", CommandError::failed,
                "inside the split above it");
  expectRefused(rig, "split.move", R"({"row":0,"mixAt":4})", CommandError::failed,
                "inside the split above it");
  // Feeds: no loops.
  rig.run("row.setInput", R"({"row":3,"source":"none"})");
  rig.run("row.setOutput", R"({"row":3,"target":"device","pair":2})");
  rig.run("row.setOutput", R"({"row":0,"target":"row","toRow":3})");
  expectRefused(rig, "row.setOutput", R"({"row":3,"target":"row","toRow":0})", CommandError::failed,
                "input of its own");
  rig.run("row.setInput", R"({"row":0,"source":"none"})");
  expectRefused(rig, "row.setOutput", R"({"row":3,"target":"row","toRow":0})", CommandError::failed,
                "loop");
  EXPECT_EQ(rig.proc.unresolvedPlanRefsForTesting(), 0);
}

// A split command on a legacy rig converts it first: one undo step with the
// edit, and none at all when the edit is refused.
TEST(GridSplitCommandsTest, ASplitCommandConvertsALegacyRigAsOneUndoStep) {
  GridRig rig;
  load(rig, lanes({gainBlock("blk-a")}, {}, false));
  const juce::MemoryBlock legacyBytes = savedBytes(rig.proc);
  ASSERT_FALSE(static_cast<bool>(rig.proc.getChainState(-1)["canUndo"]));

  // Nothing to move, type or remove in a rig that has no split: refused,
  // and the rig stays the legacy rig it was.
  for (const auto& [command, args] :
       {std::pair<const char*, const char*>{"split.move", R"({"row":0,"splitAt":2})"},
        {"split.setType", R"({"row":0,"type":"ab"})"},
        {"split.remove", R"({"row":0})"},
        {"split.setParam", R"({"row":0,"param":"mix.level","value":-3})"},
        {"split.add", R"({"row":0,"splitAt":5,"mixAt":2})"}}) {
    SCOPED_TRACE(command);
    EXPECT_FALSE(rig.tryRun(command, args).ok);
    EXPECT_FALSE(rig.proc.isGridMode());
    EXPECT_FALSE(static_cast<bool>(rig.proc.getChainState(-1)["canUndo"]));
    EXPECT_TRUE(savedBytes(rig.proc) == legacyBytes);
    EXPECT_EQ(rig.proc.validateActivePlanForTesting(), "");
  }

  const juce::var result = rig.run("split.add", R"({"row":0,"splitAt":1,"mixAt":4,"type":"ab"})");
  EXPECT_TRUE(rig.proc.isGridMode());
  EXPECT_EQ(result["mode"].toString(), "grid");
  ASSERT_EQ(result["splits"].size(), 1);
  ASSERT_TRUE(rig.proc.undoChain());
  ASSERT_TRUE(waitForChainLoaded(rig.proc));
  EXPECT_FALSE(rig.proc.isGridMode()) << "one step took back the split and the conversion";
  EXPECT_FALSE(static_cast<bool>(rig.proc.getChainState(-1)["canUndo"]));
  EXPECT_TRUE(savedBytes(rig.proc) == legacyBytes);
  ASSERT_TRUE(rig.proc.redoChain());
  ASSERT_TRUE(waitForChainLoaded(rig.proc));
  EXPECT_TRUE(rig.proc.isGridMode());
  EXPECT_EQ(rig.proc.getGridState()["splits"].size(), 1);
  EXPECT_EQ(rig.proc.validateActivePlanForTesting(), "");
}

// ---------------------------------------------------------------------------
// Saved state
// ---------------------------------------------------------------------------

TEST(GridSplitStateTest, SavedShapeHasSplitNodesAndRaisesTheFormat) {
  GridRig rig(2, 4);
  buildSplitRig(rig);
  const juce::MemoryBlock saved = savedBytes(rig.proc);
  EXPECT_TRUE(saved == savedBytes(rig.proc)) << "saving twice gives the same bytes";
  const juce::ValueTree gridNode =
      parseSaved(saved).getChildWithName("ChainSnapshot").getChildWithName("Grid");
  ASSERT_TRUE(gridNode.isValid());
  // A build that knows no splits must not play this rig as loose rows.
  EXPECT_EQ(static_cast<int>(gridNode.getProperty("version")), 2);
  EXPECT_EQ(static_cast<int>(gridNode.getProperty("minReader")), 2);
  ASSERT_EQ(gridNode.getNumChildren(), 6) << "four rows, then the splits in row order";
  const juce::ValueTree first = gridNode.getChild(4), second = gridNode.getChild(5);
  ASSERT_TRUE(first.hasType("Split"));
  ASSERT_TRUE(second.hasType("Split"));
  EXPECT_EQ(static_cast<int>(first.getProperty("upperRow")), 0);
  EXPECT_EQ(static_cast<int>(first.getProperty("splitCol")), 2);
  EXPECT_EQ(static_cast<int>(first.getProperty("mixCol")), 7);
  EXPECT_EQ(first.getProperty("type").toString(), "crossover");
  EXPECT_TRUE(first.getProperty("id").toString().isNotEmpty());
  EXPECT_EQ(first.getNumChildren(), static_cast<int>(SplitJunction::kNumParams));
  EXPECT_NEAR(static_cast<double>(
                  first.getChildWithProperty("id", "split.crossoverHz").getProperty("value")),
              320.0, 1e-6);
  EXPECT_NEAR(static_cast<double>(first.getChildWithProperty("id", "mix.levelB").getProperty("value")),
              -4.5, 1e-6);
  EXPECT_EQ(static_cast<int>(second.getProperty("upperRow")), 1);
  EXPECT_EQ(second.getProperty("type").toString(), "ab");
  std::cout << "[grid state] " << first.createCopy().toXmlString().toStdString();

  // Without a split or a feed the node is at format 1 again.
  rig.run("split.remove", R"({"row":1})");
  rig.run("split.remove", R"({"row":0})");
  EXPECT_EQ(static_cast<int>(gridNodeOf(rig.proc).getProperty("minReader")), 1);
  // A row feed raises it as well.
  rig.run("row.setOutput", R"({"row":0,"target":"row","toRow":3})");
  EXPECT_EQ(static_cast<int>(gridNodeOf(rig.proc).getProperty("minReader")), 2);
  EXPECT_EQ(gridNodeOf(rig.proc).getChild(0).getChildWithName("Output").getProperty("type").toString(),
            "row");
  EXPECT_EQ(static_cast<int>(
                gridNodeOf(rig.proc).getChild(0).getChildWithName("Output").getProperty("toRow")),
            3);
}

TEST(GridSplitStateTest, SaveRestoreRoundTrip) {
  for (const bool withFeed : {false, true}) {
    SCOPED_TRACE(withFeed ? "with a feed" : "splits only");
    GridRig rig(2, 4);
    buildSplitRig(rig);
    if (withFeed) {
      rig.run("row.setOutput", R"({"row":3,"target":"device","pair":2})");
      rig.run("row.setOutput", R"({"row":0,"target":"row","toRow":3})");
      rig.addKind("utility.gain", 3, 1, "gainDb=-1");
    }
    const juce::MemoryBlock saved = savedBytes(rig.proc);
    const juce::String gridJson = stableGridJson(rig.proc);

    GridRig second(2, 4);
    second.proc.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    ASSERT_TRUE(waitForChainLoaded(second.proc));
    EXPECT_TRUE(second.proc.isGridMode());
    EXPECT_EQ(stableGridJson(second.proc), gridJson);
    EXPECT_EQ(second.proc.validateActivePlanForTesting(), "");
    EXPECT_TRUE(savedBytes(second.proc) == saved) << "save, restore, save moved the bytes";
    // Over a processor that already holds the rig: reconciled, same bytes.
    second.proc.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    ASSERT_TRUE(waitForChainLoaded(second.proc));
    EXPECT_TRUE(savedBytes(second.proc) == saved);

    // And it is the same rig to the ear.
    rig.settle();
    second.settle();
    const GridChannels in{makeNoise(4096, 61, 0.2f), makeNoise(4096, 62, 0.2f)};
    const GridChannels a = rig.process(in), b = second.process(in);
    EXPECT_GT(gridPeak(a[withFeed ? 2 : 0]), 0.01f);
    for (size_t ch = 0; ch < a.size(); ++ch)
      EXPECT_EQ(a[ch], b[ch]) << "channel " << ch;
  }
}

// What an older reader does with it, and what this one does with a newer one.
TEST(GridSplitStateTest, MinReaderGuardsBothWays) {
  GridRig rig(2, 4);
  buildSplitRig(rig);
  juce::ValueTree tree = parseSaved(savedBytes(rig.proc));
  juce::ValueTree gridNode = tree.getChildWithName("ChainSnapshot").getChildWithName("Grid");
  ASSERT_EQ(static_cast<int>(gridNode.getProperty("minReader")), 2)
      << "a build that reads up to format 1 refuses this state whole (pinned for that build by "
         "GridStateTest.ANewerGridFormatIsRefusedWhole)";

  // A format this build does not read yet is refused whole, a readable one
  // loads.
  gridNode.setProperty("minReader", 3, nullptr);
  GridRig target;
  load(target, lanes({gainBlock("blk-keep")}, {}, false));
  const juce::MemoryBlock keep = savedBytes(target.proc);
  const juce::MemoryBlock newer = frame(tree);
  target.proc.setStateInformation(newer.getData(), static_cast<int>(newer.getSize()));
  EXPECT_FALSE(target.proc.isGridMode());
  EXPECT_TRUE(savedBytes(target.proc) == keep);

  gridNode.setProperty("minReader", 2, nullptr);
  const juce::MemoryBlock readable = frame(tree);
  target.proc.setStateInformation(readable.getData(), static_cast<int>(readable.getSize()));
  ASSERT_TRUE(waitForChainLoaded(target.proc));
  EXPECT_TRUE(target.proc.isGridMode());
  EXPECT_EQ(target.proc.getGridState()["splits"].size(), 2);
}

// A state whose splits or feeds break the rules (hand-edited, or from a
// build with other rules) still loads: what is at fault is set aside inert
// and written back, and the rest of the rig plays.
TEST(GridSplitStateTest, ASplitOrFeedThatBreaksTheRulesIsSetAsideNotPlayedWrong) {
  GridRig rig(2, 4);
  buildSplitRig(rig);
  juce::ValueTree tree = parseSaved(savedBytes(rig.proc));
  juce::ValueTree gridNode = tree.getChildWithName("ChainSnapshot").getChildWithName("Grid");
  // The inner split now lies outside its parent; row 3 feeds the lower row
  // of a split; and a third split names a row with no row below it.
  juce::ValueTree inner = gridNode.getChild(5);
  ASSERT_TRUE(inner.hasType("Split"));
  inner.setProperty("splitCol", 0, nullptr);
  juce::ValueTree fourthOutput = gridNode.getChild(3).getChildWithName("Output");
  fourthOutput.setProperty("type", "row", nullptr);
  fourthOutput.setProperty("toRow", 1, nullptr);
  juce::ValueTree stray("Split");
  stray.setProperty("id", "split-stray", nullptr);
  stray.setProperty("upperRow", 3, nullptr);
  stray.setProperty("splitCol", 1, nullptr);
  stray.setProperty("mixCol", 2, nullptr);
  stray.setProperty("type", "ab", nullptr);
  gridNode.appendChild(stray, nullptr);
  const juce::MemoryBlock broken = frame(tree);

  GridRig target(2, 4);
  target.proc.setStateInformation(broken.getData(), static_cast<int>(broken.getSize()));
  ASSERT_TRUE(waitForChainLoaded(target.proc));
  ASSERT_TRUE(target.proc.isGridMode());
  EXPECT_EQ(target.proc.validateActivePlanForTesting(), "");
  const juce::var splits = target.proc.getGridState()["splits"];
  ASSERT_EQ(splits.size(), 3);
  int inert = 0;
  for (int i = 0; i < splits.size(); ++i)
    inert += static_cast<bool>(splits[i]["inert"]) ? 1 : 0;
  EXPECT_EQ(inert, 2) << "the outer split runs, the other two are set aside";
  EXPECT_TRUE(static_cast<bool>(target.row(1)["routed"]));
  EXPECT_FALSE(static_cast<bool>(target.row(2)["routed"]));
  EXPECT_EQ(target.row(3)["output"]["type"].toString(), "row") << "reported as it was saved";
  EXPECT_FALSE(static_cast<bool>(target.row(3)["routed"]));

  // Written back: the placements and the feed are still in the file.
  const juce::ValueTree again = gridNodeOf(target.proc);
  int splitNodes = 0;
  bool sawStray = false, sawInner = false;
  for (int i = 0; i < again.getNumChildren(); ++i)
    if (again.getChild(i).hasType("Split")) {
      ++splitNodes;
      sawStray = sawStray || again.getChild(i).getProperty("id").toString() == "split-stray";
      sawInner = sawInner || (static_cast<int>(again.getChild(i).getProperty("upperRow")) == 1 &&
                              static_cast<int>(again.getChild(i).getProperty("splitCol")) == 0);
    }
  EXPECT_EQ(splitNodes, 3);
  EXPECT_TRUE(sawStray);
  EXPECT_TRUE(sawInner);
  EXPECT_EQ(again.getChild(3).getChildWithName("Output").getProperty("type").toString(), "row");
  EXPECT_EQ(static_cast<int>(again.getChild(3).getChildWithName("Output").getProperty("toRow")), 1);
  EXPECT_EQ(static_cast<int>(again.getProperty("minReader")), 2);
}

TEST(GridSplitStateTest, UndoAndRedoKeepTheJunctionsById) {
  GridRig rig(2, 4);
  buildSplitRig(rig);
  rig.settle();
  const juce::String outerId = rig.proc.getGridState()["splits"][0]["id"].toString();
  const juce::String before = stableGridJson(rig.proc);

  // A parameter move and its undo: the value goes back, the junction is the
  // same one (its id), and nothing is rebuilt.
  rig.run("split.setParam", R"({"row":0,"param":"mix.levelB","value":-12})");
  EXPECT_NE(stableGridJson(rig.proc), before);
  ASSERT_TRUE(rig.proc.undoChain());
  ASSERT_TRUE(waitForChainLoaded(rig.proc));
  EXPECT_EQ(stableGridJson(rig.proc), before);
  EXPECT_EQ(rig.proc.getGridState()["splits"][0]["id"].toString(), outerId);
  ASSERT_TRUE(rig.proc.redoChain());
  EXPECT_EQ(static_cast<double>(rig.proc.getGridState()["splits"][0]["params"]["mix.levelB"]), -12.0);
  ASSERT_TRUE(rig.proc.undoChain());

  // Several moves of one parameter in a row are one step.
  const bool hadUndo = static_cast<bool>(rig.proc.getChainState(-1)["canUndo"]);
  ASSERT_TRUE(hadUndo);
  rig.run("split.setParam", R"({"row":0,"param":"mix.panB","value":-0.2})");
  rig.run("split.setParam", R"({"row":0,"param":"mix.panB","value":-0.4})");
  rig.run("split.setParam", R"({"row":0,"param":"mix.panB","value":-0.6})");
  ASSERT_TRUE(rig.proc.undoChain());
  EXPECT_EQ(stableGridJson(rig.proc), before);

  // Structure: a move, a type change, a removal and a feed, undone in turn.
  rig.run("split.move", R"({"row":1,"splitAt":4,"mixAt":6})");
  const juce::String moved = stableGridJson(rig.proc);
  rig.run("split.setType", R"({"row":0,"type":"balance"})");
  const juce::String retyped = stableGridJson(rig.proc);
  rig.run("split.remove", R"({"row":0})");
  EXPECT_EQ(rig.proc.getGridState()["splits"].size(), 1);
  const juce::String removed = stableGridJson(rig.proc);
  rig.run("row.setOutput", R"({"row":0,"target":"row","toRow":3})");
  ASSERT_TRUE(rig.proc.undoChain());
  EXPECT_EQ(stableGridJson(rig.proc), removed);
  ASSERT_TRUE(rig.proc.undoChain());
  ASSERT_TRUE(waitForChainLoaded(rig.proc));
  EXPECT_EQ(stableGridJson(rig.proc), retyped);
  EXPECT_EQ(rig.proc.getGridState()["splits"][0]["id"].toString(), outerId) << "back with its id";
  ASSERT_TRUE(rig.proc.undoChain());
  EXPECT_EQ(stableGridJson(rig.proc), moved);
  ASSERT_TRUE(rig.proc.undoChain());
  ASSERT_TRUE(waitForChainLoaded(rig.proc));
  EXPECT_EQ(stableGridJson(rig.proc), before);
  EXPECT_EQ(rig.proc.validateActivePlanForTesting(), "");
  EXPECT_EQ(rig.proc.stalePlanCallbacksForTesting(), 0);

  // The audio after all that is the audio of the rig as built.
  GridRig fresh(2, 4);
  buildSplitRig(fresh);
  fresh.settle();
  rig.settle();
  const GridChannels in{makeNoise(4096, 63, 0.2f), makeNoise(4096, 64, 0.2f)};
  const GridChannels a = fresh.process(in), b = rig.process(in);
  EXPECT_GT(gridPeak(a[0]), 0.01f);
  EXPECT_EQ(a[0], b[0]);
  EXPECT_EQ(a[1], b[1]);
}

TEST(GridSplitStateTest, PresetsCarrySplitsAndFeeds) {
  const juce::File store = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile("pedaleira-split-preset-" + juce::Uuid().toString());
  struct Cleanup {
    juce::File dir;
    ~Cleanup() { dir.deleteRecursively(); }
  } cleanup{store};

  GridRig rig(2, 4);
  rig.proc.setPresetStoreForTesting(store);
  buildSplitRig(rig);
  rig.run("row.setOutput", R"({"row":3,"target":"device","pair":2})");
  rig.run("row.setOutput", R"({"row":0,"target":"row","toRow":3})");
  const juce::String gridJson = stableGridJson(rig.proc);
  const juce::var saved = rig.proc.savePreset("Split rig");
  ASSERT_TRUE(saved.isObject());

  // Over a legacy rig, and back by undo.
  load(rig, lanes({gainBlock("blk-other")}, {}, false));
  ASSERT_FALSE(rig.proc.isGridMode());
  ASSERT_TRUE(rig.proc.loadPreset(saved["id"].toString()));
  ASSERT_TRUE(waitForChainLoaded(rig.proc));
  EXPECT_TRUE(rig.proc.isGridMode());
  EXPECT_EQ(stableGridJson(rig.proc), gridJson);
  EXPECT_EQ(rig.proc.validateActivePlanForTesting(), "");
  ASSERT_TRUE(rig.proc.undoChain());
  ASSERT_TRUE(waitForChainLoaded(rig.proc));
  EXPECT_FALSE(rig.proc.isGridMode());
  EXPECT_EQ(rig.proc.getChainState(-1)["chain"][0]["blockId"].toString(), "blk-other");
}

// Scenes and stomps address blocks, wherever in the grid they sit: a block
// in the lower path of a split switches with its scene like any other.
TEST(GridSplitStateTest, ScenesAndStompsReachBlocksInEveryPath) {
  GridRig rig(2, 4);
  buildSplitRig(rig);
  const std::string lower = rig.proc.gridSlotBlockId(2, 3);
  ASSERT_FALSE(lower.empty());
  rig.run("scene.add");
  rig.run("scene.select", R"({"scene":"B"})");
  ASSERT_TRUE(rig.proc.setBlockParam(lower, "enabled", 0.0));
  rig.run("scene.select", R"({"scene":"A"})");
  const auto enabled = [&] {
    const juce::var blocks = rig.proc.getGridState();
    const juce::var state = rig.proc.getChainState(-1)["grid"]["rows"][2]["blocks"][3];
    return static_cast<bool>(state["params"]["enabled"]);
  };
  EXPECT_TRUE(enabled());
  rig.run("scene.select", R"({"scene":"B"})");
  EXPECT_FALSE(enabled());
  ASSERT_TRUE(rig.proc.assignStomp(0, {lower}).wasOk());
  rig.proc.footswitchPress(0);
  // (In scene mode a switch selects a scene; the stomp itself is what is
  // pinned here: it is part of the rig and survives the round trip.)
  const juce::MemoryBlock saved = savedBytes(rig.proc);
  GridRig second(2, 4);
  second.proc.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
  ASSERT_TRUE(waitForChainLoaded(second.proc));
  EXPECT_EQ(second.proc.getStompsState()["stomps"][0]["blockIds"][0].toString(), juce::String(lower));
  EXPECT_EQ(juce::JSON::toString(second.proc.getScenesState()),
            juce::JSON::toString(rig.proc.getScenesState()));
  // Splits change nothing about either: a move of the split leaves both.
  const juce::String scenes = juce::JSON::toString(rig.proc.getScenesState());
  rig.run("split.move", R"({"row":0,"splitAt":1})");
  rig.run("split.setParam", R"({"row":0,"param":"mix.level","value":-3})");
  EXPECT_EQ(juce::JSON::toString(rig.proc.getScenesState()), scenes);
}

// ---------------------------------------------------------------------------
// A branched legacy rig, there and back
// ---------------------------------------------------------------------------

juce::ValueTree branchedLanes(const char* trunkSide) {
  juce::ValueTree state = lanes({gainBlock("blk-l0", 2.0), gainBlock("blk-l1", -1.0)},
                                {gainBlock("blk-r0", 3.0), gainBlock("blk-r1", -2.0)}, true);
  state.setProperty("branchSide", trunkSide, nullptr);
  state.setProperty("branchAfterBlockId", juce::String(trunkSide) == "left" ? "blk-l0" : "blk-r0",
                    nullptr);
  return state;
}

TEST(GridBranchMappingTest, ABranchBecomesASplitThatNeverRejoins) {
  GridRig rig;
  load(rig, branchedLanes("left"));
  setParam(rig, "chainPanLeft", 0.2f);
  setParam(rig, "chainPanRight", 0.7f);
  setParam(rig, "chainInvertRight", 1.0f);
  rig.proc.parameters.getParameter("outputBalance")->setValueNotifyingHost(0.6f);
  ASSERT_EQ(rig.proc.getInputMode(), InputMode::Left) << "a branch has the one source";
  const juce::var converted = rig.run("grid.convert", kToGrid);
  ASSERT_TRUE(rig.proc.isGridMode());

  const juce::var grid = rig.proc.getGridState();
  ASSERT_EQ(grid["splits"].size(), 1);
  const juce::var split = grid["splits"][0];
  EXPECT_EQ(static_cast<int>(split["row"]), 0);
  EXPECT_EQ(static_cast<int>(split["splitAt"]), 1) << "after the tapped block";
  EXPECT_TRUE(split["mixAt"].isVoid());
  EXPECT_EQ(split["type"].toString(), "ab");
  EXPECT_EQ(static_cast<double>(split["params"]["split.levelA"]), 0.0);
  EXPECT_EQ(static_cast<double>(split["params"]["split.levelB"]), 0.0);
  // The trunk in row 0 at its lane positions; the branch lane in row 1 from
  // the split's column on.
  EXPECT_EQ(rig.proc.gridSlotBlockId(0, 0), "blk-l0");
  EXPECT_EQ(rig.proc.gridSlotBlockId(0, 1), "blk-l1");
  EXPECT_EQ(rig.proc.gridSlotBlockId(1, 1), "blk-r0");
  EXPECT_EQ(rig.proc.gridSlotBlockId(1, 2), "blk-r1");
  EXPECT_EQ(rig.row(0)["format"].toString(), "mono");
  EXPECT_EQ(rig.row(1)["format"].toString(), "mono");
  EXPECT_EQ(rig.row(0)["input"]["type"].toString(), "device");
  EXPECT_EQ(static_cast<int>(rig.row(0)["input"]["channel"]), 1);
  EXPECT_EQ(rig.row(1)["input"]["type"].toString(), "none");
  EXPECT_EQ(rig.row(1)["source"].toString(), "split");
  EXPECT_NEAR(static_cast<double>(rig.row(0)["output"]["pan"]), 0.2, 1e-6);
  EXPECT_NEAR(static_cast<double>(rig.row(1)["output"]["pan"]), 0.7, 1e-6);
  EXPECT_NEAR(static_cast<double>(rig.row(0)["output"]["volumeDb"]), -2.4, 1e-5);
  EXPECT_NEAR(static_cast<double>(rig.row(1)["output"]["volumeDb"]), 2.4, 1e-5);
  EXPECT_TRUE(static_cast<bool>(rig.row(1)["output"]["invert"]));
  EXPECT_TRUE(static_cast<bool>(grid["legacyProjectable"]))
      << rig.proc.legacyProjectionBlockers().joinIntoString("; ");
}

// The promoted branch is the same audio: bit for bit at the default pans
// (the golden test), and to the last bits with pans, balance and polarity,
// where the two engines do the same arithmetic at different rates (the
// legacy image matrix at the host rate after the output is back from the
// chain rate, the rows' sends at the chain rate before it).
TEST(GridBranchMappingTest, APromotedBranchSoundsTheSame) {
  const int frames = 8192;
  const GridChannels in{makeNoise(frames, 71, 0.2f), makeNoise(frames, 72, 0.2f)};
  struct Case {
    const char* name;
    const char* trunk;
    float panLeft, panRight, balance;
    bool invertRight;
    bool exact;
  };
  const Case cases[] = {
      {"trunk left, default pans", "left", 0.0f, 1.0f, 0.5f, false, true},
      {"trunk right, default pans", "right", 0.0f, 1.0f, 0.5f, false, true},
      {"trunk left, pans, balance and polarity", "left", 0.2f, 0.7f, 0.6f, true, false},
      {"trunk right, pans, balance and polarity", "right", 0.2f, 0.7f, 0.6f, true, false},
  };
  for (const Case& c : cases)
    for (const int oversample : {1, 2}) {
      SCOPED_TRACE(std::string(c.name) + ", oversampling x" + std::to_string(oversample));
      const auto render = [&](bool promote, bool toneStack = true) {
        GridRig rig(2, 2, 48000.0, 64, oversample);
        setParam(rig, "toneEqEnabled", toneStack ? 1.0f : 0.0f);
        juce::ValueTree state = branchedLanes(c.trunk);
        // A latent block in the trunk's remainder: the branch has to wait.
        state.getChildWithName(juce::String(c.trunk) == "left" ? "ChainBlocks" : "RightChainBlocks")
            .appendChild(makeKindBlockTree("blk-late", TestLatencyProcessor::kKindId, "Latency"),
                         nullptr);
        load(rig, state);
        setParam(rig, "chainPanLeft", c.panLeft);
        setParam(rig, "chainPanRight", c.panRight);
        setParam(rig, "chainInvertRight", c.invertRight ? 1.0f : 0.0f);
        rig.proc.parameters.getParameter("outputBalance")->setValueNotifyingHost(c.balance);
        if (promote)
          rig.run("grid.convert", kToGrid);
        rig.settle();
        EXPECT_EQ(rig.proc.isGridMode(), promote);
        // (Reported in host samples; the block counts at the chain rate.)
        EXPECT_EQ(rig.proc.getLatencySamples(), TestLatencyProcessor::kDefaultLatency / oversample);
        return rig.process(in);
      };
      const GridChannels expected = render(false), out = render(true);
      // (With the polarity flipped the two lanes, which carry the one
      // source, partly cancel on one side.)
      ASSERT_GT(std::max(gridPeak(expected[0]), gridPeak(expected[1])), 0.05f);
      const float diff = std::max(gridMaxDiff(out[0], expected[0]), gridMaxDiff(out[1], expected[1]));
      // The cause of what is left, pinned: without the tone stack (three
      // float biquads whose low shelf turns a rounding step in its input
      // into a far larger one in its output) the two renders differ in the
      // last bits only.
      const GridChannels flatExpected = render(false, false), flatOut = render(true, false);
      const float flatDiff = std::max(gridMaxDiff(flatOut[0], flatExpected[0]),
                                      gridMaxDiff(flatOut[1], flatExpected[1]));
      std::printf("  branched rig, %s, x%d: promoted against legacy, max abs diff %g (tone stack "
                  "off %g), peak %g\n",
                  c.name, oversample, static_cast<double>(diff), static_cast<double>(flatDiff),
                  static_cast<double>(std::max(gridPeak(expected[0]), gridPeak(expected[1]))));
      if (c.exact) {
        EXPECT_EQ(diff, 0.0f);
      }
      EXPECT_LT(diff, 2e-5f);
      EXPECT_LT(flatDiff, 2e-6f) << "more than the last bits before the tone stack";
    }
}

TEST(GridBranchMappingTest, TheBranchShapeDemotesBack) {
  // Built as a grid from nothing: a split of row 0 into row 1 that never
  // rejoins, A/B at unity, two mono rows on Out 1/2.
  GridRig rig;
  rig.run("grid.convert", kToGrid);
  const juce::String amp = rig.addKind("utility.gain", 0, 0, "gainDb=1");
  rig.addKind("utility.gain", 0, 3, "gainDb=2");
  rig.run("row.setInput", R"({"row":0,"source":"device","channel":1})");
  rig.run("row.setFormat", R"({"row":0,"format":"mono"})");
  rig.run("split.add", R"({"row":0,"splitAt":2,"type":"ab"})");
  rig.run("row.setOutput", R"({"row":1,"target":"device","pair":1})");
  const juce::String branchBlock = rig.addKind("utility.gain", 1, 4, "gainDb=-3");
  rig.run("row.setMix", R"({"row":0,"pan":0.1,"volumeDb":-1.5})");
  rig.run("row.setMix", R"({"row":1,"pan":0.9,"volumeDb":1.5,"invert":true})");
  ASSERT_TRUE(static_cast<bool>(rig.proc.getGridState()["legacyProjectable"]))
      << rig.proc.legacyProjectionBlockers().joinIntoString("; ");

  // What a legacy client is shown: two lanes and a branch after the last
  // block before the fork (the empty slot between them passes the signal
  // on), the branch lane starting at its first slot after the fork.
  juce::var state = rig.proc.getChainState(-1);
  EXPECT_TRUE(static_cast<bool>(state["stereoEnabled"]));
  EXPECT_TRUE(static_cast<bool>(state["legacyEditable"]));
  EXPECT_EQ(state["branch"]["side"].toString(), "left");
  EXPECT_EQ(state["branch"]["afterBlockId"].toString(), amp);
  EXPECT_EQ(state["inputMode"].toString(), "left");
  ASSERT_GE(state["chainRight"].size(), 3);
  EXPECT_EQ(state["chainRight"][0]["kind"].toString(), "insert") << "slots 2 and 3 of row 1";
  EXPECT_EQ(state["chainRight"][2]["blockId"].toString(), branchBlock);
  const juce::String projected = juce::JSON::toString(state["chainRight"]) +
                                 juce::JSON::toString(state["chain"]) +
                                 juce::JSON::toString(state["branch"]);

  rig.settle();
  const GridChannels in{makeNoise(4096, 73, 0.2f), gridSilence(4096)};
  const GridChannels asGrid = rig.process(in);

  rig.run("grid.convert", kToLegacy);
  ASSERT_FALSE(rig.proc.isGridMode());
  state = rig.proc.getChainState(-1);
  EXPECT_EQ(juce::JSON::toString(state["chainRight"]) + juce::JSON::toString(state["chain"]) +
                juce::JSON::toString(state["branch"]),
            projected)
      << "the legacy rig is the one the projection showed";
  EXPECT_NEAR(paramValue(rig, "chainPanLeft"), 0.1f, 1e-6f);
  EXPECT_NEAR(paramValue(rig, "chainPanRight"), 0.9f, 1e-6f);
  EXPECT_NEAR(paramValue(rig, "outputBalance"), 0.5f + 1.5f / 24.0f, 1e-6f);
  EXPECT_GE(paramValue(rig, "chainInvertRight"), 0.5f);
  EXPECT_EQ(rig.proc.getInputMode(), InputMode::Left);
  rig.settle();
  const GridChannels asLegacy = rig.process(in);
  ASSERT_GT(gridPeak(asLegacy[0]), 0.05f);
  EXPECT_LT(std::max(gridMaxDiff(asGrid[0], asLegacy[0]), gridMaxDiff(asGrid[1], asLegacy[1])), 2e-5f);

  // What keeps a split from being a legacy branch, each with its reason.
  struct Blocker {
    const char* command;
    const char* args;
    const char* reason;
  };
  const Blocker blockers[] = {
      {"split.move", R"({"row":0,"mixAt":6})", "rejoins"},
      {"split.setType", R"({"row":0,"type":"balance"})", "not an A/B split"},
      {"split.setParam", R"({"row":0,"param":"split.levelB","value":-3})", "not an A/B split"},
      {"split.setParam", R"({"row":0,"param":"split.stereo","value":1})", "not an A/B split"},
      {"split.move", R"({"row":0,"splitAt":0})", "before the first block"},
      {"split.move", R"({"row":0,"splitAt":6})", "blocks before the split"},
      {"row.setOutput", R"({"row":1,"target":"none"})", "no device output"},
      {"row.setFormat", R"({"row":1,"format":"stereo"})", "not mono"},
  };
  for (const Blocker& b : blockers) {
    SCOPED_TRACE(b.args);
    rig.run("grid.convert", kToGrid);
    ASSERT_TRUE(static_cast<bool>(rig.proc.getGridState()["legacyProjectable"]))
        << rig.proc.legacyProjectionBlockers().joinIntoString("; ");
    rig.run(b.command, b.args);
    const juce::String why = rig.proc.legacyProjectionBlockers().joinIntoString("; ");
    EXPECT_TRUE(why.contains(b.reason)) << why;
    EXPECT_FALSE(static_cast<bool>(rig.proc.getChainState(-1)["legacyEditable"]));
    EXPECT_TRUE(rig.proc.getChainState(-1)["branch"].isVoid());
    ASSERT_TRUE(rig.proc.undoChain());
    ASSERT_TRUE(rig.proc.undoChain());  // the conversion
    ASSERT_FALSE(rig.proc.isGridMode());
  }
}

// ---------------------------------------------------------------------------
// The legacy client's edits on a grid that is a legacy rig in shape
// ---------------------------------------------------------------------------

TEST(GridLegacyEditsTest, LaneShapedEditsTakeAProjectableGridBack) {
  const auto fresh = [](GridRig& rig) {
    load(rig, branchedLanes("left"));
    rig.run("grid.convert", kToGrid);
    ASSERT_TRUE(rig.proc.isGridMode());
  };
  const auto ids = [](TONE3000Processor& proc, const char* key) {
    std::vector<std::string> list;
    const juce::var lane = proc.getChainState(-1)[key];
    for (int i = 0; i < lane.size(); ++i)
      list.push_back(lane[i]["blockId"].toString().toStdString());
    return list;
  };

  // Reordering the lane the client was shown (shorter than the row).
  {
    GridRig rig;
    fresh(rig);
    std::vector<std::string> order = ids(rig.proc, "chainRight");
    ASSERT_LT(order.size(), static_cast<size_t>(kGridCols));
    std::swap(order[0], order[1]);
    EXPECT_TRUE(rig.proc.reorderChainBlocks(order));
    EXPECT_FALSE(rig.proc.isGridMode());
    EXPECT_EQ(ids(rig.proc, "chainRight"), order);
    EXPECT_EQ(rig.proc.getChainState(-1)["branch"]["afterBlockId"].toString(), "blk-l0");
    EXPECT_EQ(rig.proc.validateActivePlanForTesting(), "");
  }
  // A drag across the lanes, by lane position.
  {
    GridRig rig;
    fresh(rig);
    EXPECT_TRUE(rig.proc.moveBlockToChain("blk-r1", "left", 2));
    EXPECT_FALSE(rig.proc.isGridMode());
    EXPECT_EQ(ids(rig.proc, "chain")[2], "blk-r1");
  }
  // Clearing the branch, moving it, swapping the lanes, mono.
  {
    GridRig rig;
    fresh(rig);
    EXPECT_TRUE(rig.proc.clearChainBranch());
    EXPECT_FALSE(rig.proc.isGridMode());
    EXPECT_TRUE(rig.proc.getChainState(-1)["branch"].isVoid());
    EXPECT_TRUE(rig.proc.isStereoMode());
  }
  {
    GridRig rig;
    fresh(rig);
    EXPECT_TRUE(rig.proc.setChainBranch("left", "blk-l1"));
    EXPECT_FALSE(rig.proc.isGridMode());
    EXPECT_EQ(rig.proc.getChainState(-1)["branch"]["afterBlockId"].toString(), "blk-l1");
  }
  {
    GridRig rig;
    fresh(rig);
    EXPECT_TRUE(rig.proc.swapChains());
    EXPECT_FALSE(rig.proc.isGridMode());
    EXPECT_EQ(rig.proc.getChainState(-1)["branch"]["side"].toString(), "right");
    EXPECT_EQ(ids(rig.proc, "chainRight")[0], "blk-l0");
  }
  {
    GridRig rig;
    fresh(rig);
    rig.proc.setStereoMode(false);
    EXPECT_FALSE(rig.proc.isGridMode());
    EXPECT_FALSE(rig.proc.isStereoMode());
  }
  // Edits by block id leave the grid a grid, and the client sees them.
  {
    GridRig rig;
    fresh(rig);
    EXPECT_TRUE(rig.proc.setBlockParam("blk-r0", "gainDb", 5.0));
    EXPECT_TRUE(rig.proc.removeChainBlock("blk-r1"));
    // A duplicate by lane position lands where the client pointed: lane
    // position 1 of the branch lane is slot 2 of row 1.
    const std::string copy = rig.proc.duplicateChainBlock("blk-r0", "right", 1);
    ASSERT_FALSE(copy.empty());
    EXPECT_TRUE(rig.proc.isGridMode());
    EXPECT_EQ(rig.proc.gridSlotBlockId(1, 2), copy);
    const std::vector<std::string> lane = ids(rig.proc, "chainRight");
    EXPECT_EQ(lane[0], "blk-r0");
    EXPECT_EQ(lane[1], copy);
    // Into an empty slot the client was shown, by its id.
    const std::string slot = lane.back();
    const juce::var added = rig.proc.addKindBlock("utility.gain", slot);
    ASSERT_TRUE(added["blockId"].toString().isNotEmpty()) << added["error"].toString();
    EXPECT_TRUE(rig.proc.isGridMode());
    EXPECT_TRUE(static_cast<bool>(rig.proc.getChainState(-1)["legacyEditable"]));
    EXPECT_EQ(rig.proc.validateActivePlanForTesting(), "");
  }
}

// ---------------------------------------------------------------------------
// The legacy image controls on a grid
// ---------------------------------------------------------------------------

TEST(GridLegacyControlsTest, PansSolosPolarityAndBalanceAreTheRowsOfTwoMonoRows) {
  GridRig rig;
  load(rig, lanes({gainBlock("blk-l")}, {gainBlock("blk-r")}, true));
  rig.run("grid.convert", kToGrid);
  juce::var controls = rig.proc.getGridState()["legacyControls"];
  for (const char* key : {"pan", "solo", "invert", "balance"})
    EXPECT_EQ(controls[key].toString(), "rows") << key;
  EXPECT_EQ(controls["spread"].toString(), "held");
  EXPECT_EQ(controls["align"].toString(), "held");

  // The control moves the row.
  setParam(rig, "chainPanLeft", 0.25f);
  setParam(rig, "chainPanRight", 0.8f);
  setParam(rig, "chainInvertLeft", 1.0f);
  setParam(rig, "chainSoloRight", 1.0f);
  rig.proc.parameters.getParameter("outputBalance")->setValueNotifyingHost(0.75f);
  EXPECT_NEAR(static_cast<double>(rig.row(0)["output"]["pan"]), 0.25, 1e-6);
  EXPECT_NEAR(static_cast<double>(rig.row(1)["output"]["pan"]), 0.8, 1e-6);
  EXPECT_TRUE(static_cast<bool>(rig.row(0)["output"]["invert"]));
  EXPECT_FALSE(static_cast<bool>(rig.row(1)["output"]["invert"]));
  EXPECT_TRUE(static_cast<bool>(rig.row(1)["output"]["solo"]));
  EXPECT_NEAR(static_cast<double>(rig.row(0)["output"]["volumeDb"]), -6.0, 1e-5);
  EXPECT_NEAR(static_cast<double>(rig.row(1)["output"]["volumeDb"]), 6.0, 1e-5);
  EXPECT_TRUE(rig.proc.isGridMode());

  // The row moves the control.
  rig.run("row.setMix", R"({"row":0,"pan":0.4,"invert":false,"solo":true})");
  rig.run("row.setMix", R"({"row":1,"solo":false})");
  EXPECT_NEAR(paramValue(rig, "chainPanLeft"), 0.4f, 1e-6f);
  EXPECT_LT(paramValue(rig, "chainInvertLeft"), 0.5f);
  EXPECT_GE(paramValue(rig, "chainSoloLeft"), 0.5f);
  EXPECT_LT(paramValue(rig, "chainSoloRight"), 0.5f);
  EXPECT_NEAR(paramValue(rig, "outputBalance"), 0.75f, 1e-6f);

  // And it is heard: the legacy rig with the same controls is the same
  // audio (to the last bits, as for any stereo rig with pans).
  rig.run("row.setMix", R"({"row":0,"solo":false})");
  rig.settle();
  const GridChannels in{makeNoise(4096, 81, 0.2f), makeNoise(4096, 82, 0.2f)};
  const GridChannels asGrid = rig.process(in);
  rig.run("grid.convert", kToLegacy);
  EXPECT_NEAR(paramValue(rig, "chainPanLeft"), 0.4f, 1e-6f);
  EXPECT_NEAR(paramValue(rig, "outputBalance"), 0.75f, 1e-6f);
  rig.settle();
  const GridChannels asLegacy = rig.process(in);
  EXPECT_LT(std::max(gridMaxDiff(asGrid[0], asLegacy[0]), gridMaxDiff(asGrid[1], asLegacy[1])), 2e-5f);

  // Volumes that are no opposed pair: the balance has nothing to show and
  // rests at the centre; a move of it snaps back and moves no row.
  rig.run("grid.convert", kToGrid);
  rig.run("row.setMix", R"({"row":0,"volumeDb":-3})");
  EXPECT_EQ(rig.proc.getGridState()["legacyControls"]["balance"].toString(), "held");
  EXPECT_EQ(rig.proc.getGridState()["legacyControls"]["pan"].toString(), "rows");
  EXPECT_NEAR(paramValue(rig, "outputBalance"), 0.5f, 1e-6f);
  rig.proc.parameters.getParameter("outputBalance")->setValueNotifyingHost(0.9f);
  rig.proc.flushDeferredBlockRefresh();
  juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
  EXPECT_NEAR(paramValue(rig, "outputBalance"), 0.5f, 1e-6f) << "held at neutral";
  EXPECT_NEAR(static_cast<double>(rig.row(0)["output"]["volumeDb"]), -3.0, 1e-5);
  EXPECT_NEAR(static_cast<double>(rig.row(1)["output"]["volumeDb"]), 6.0, 1e-5);
}

TEST(GridLegacyControlsTest, WhatNoRowCarriesIsHeldAtNeutralAndComesBack) {
  GridRig rig;
  load(rig, lanes({gainBlock("blk-a")}, {}, false));
  setParam(rig, "spreadEnabled", 1.0f);
  rig.proc.parameters.getParameter("outputBalance")->setValueNotifyingHost(0.65f);
  const juce::MemoryBlock legacyBytes = savedBytes(rig.proc);

  const juce::var converted = rig.run("grid.convert", kToGrid);
  bool saidSo = false;
  for (int i = 0; i < converted["lossy"].size(); ++i)
    saidSo = saidSo || converted["lossy"][i].toString().contains("Spread");
  EXPECT_TRUE(saidSo);
  // One stereo row: nothing here is a pair of chains.
  const juce::var controls = rig.proc.getGridState()["legacyControls"];
  for (const char* key : {"pan", "solo", "invert", "balance", "spread", "align"})
    EXPECT_EQ(controls[key].toString(), "held") << key;
  EXPECT_LT(paramValue(rig, "spreadEnabled"), 0.5f) << "Spread does nothing here, and shows off";
  EXPECT_NEAR(paramValue(rig, "outputBalance"), 0.5f, 1e-6f);

  // A move snaps back (on the message thread, as for a host's automation).
  setParam(rig, "spreadEnabled", 1.0f);
  setParam(rig, "chainPanLeft", 0.3f);
  setParam(rig, "alignEnabled", 1.0f);
  juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
  EXPECT_LT(paramValue(rig, "spreadEnabled"), 0.5f);
  EXPECT_LT(paramValue(rig, "alignEnabled"), 0.5f);
  EXPECT_NEAR(paramValue(rig, "chainPanLeft"), 0.0f, 1e-6f);
  EXPECT_NEAR(static_cast<double>(rig.row(0)["output"]["pan"]), 0.5, 1e-6) << "no row was moved";

  // Saved and restored as a grid, the held values travel with it.
  const juce::MemoryBlock gridBytes = savedBytes(rig.proc);
  GridRig second;
  second.proc.setStateInformation(gridBytes.getData(), static_cast<int>(gridBytes.getSize()));
  ASSERT_TRUE(waitForChainLoaded(second.proc));
  EXPECT_TRUE(savedBytes(second.proc) == gridBytes);
  second.run("grid.convert", kToLegacy);
  EXPECT_GE(paramValue(second, "spreadEnabled"), 0.5f);
  EXPECT_NEAR(paramValue(second, "outputBalance"), 0.65f, 1e-6f);

  // Back as a legacy rig, by conversion or by undo, they are what they were.
  ASSERT_TRUE(rig.proc.undoChain());
  ASSERT_TRUE(waitForChainLoaded(rig.proc));
  ASSERT_FALSE(rig.proc.isGridMode());
  EXPECT_GE(paramValue(rig, "spreadEnabled"), 0.5f);
  EXPECT_NEAR(paramValue(rig, "outputBalance"), 0.65f, 1e-6f);
  EXPECT_TRUE(savedBytes(rig.proc) == legacyBytes);
  ASSERT_TRUE(rig.proc.redoChain());
  ASSERT_TRUE(waitForChainLoaded(rig.proc));
  EXPECT_LT(paramValue(rig, "spreadEnabled"), 0.5f);
  rig.run("grid.convert", kToLegacy);
  EXPECT_GE(paramValue(rig, "spreadEnabled"), 0.5f);
  EXPECT_TRUE(savedBytes(rig.proc) == legacyBytes);
}

// Rows that are not two mono rows on device outputs carry none of them,
// whatever else the grid holds.
TEST(GridLegacyControlsTest, BindingFollowsTheRows) {
  GridRig rig(2, 4);
  load(rig, lanes({gainBlock("blk-l")}, {gainBlock("blk-r")}, true));
  rig.run("grid.convert", kToGrid);
  const auto pan = [&] { return rig.proc.getGridState()["legacyControls"]["pan"].toString(); };
  EXPECT_EQ(pan(), "rows");
  // Another output pair is still two mono rows: bound, though no legacy rig.
  rig.run("row.setOutput", R"({"row":1,"target":"device","pair":2})");
  EXPECT_FALSE(static_cast<bool>(rig.proc.getGridState()["legacyProjectable"]));
  EXPECT_EQ(pan(), "rows");
  setParam(rig, "chainPanRight", 0.6f);
  EXPECT_NEAR(static_cast<double>(rig.row(1)["output"]["pan"]), 0.6, 1e-6);
  // A stereo row is placed by another law: held, and shown at neutral.
  rig.run("row.setFormat", R"({"row":1,"format":"stereo"})");
  EXPECT_EQ(pan(), "held");
  EXPECT_NEAR(paramValue(rig, "chainPanRight"), 1.0f, 1e-6f);
  EXPECT_NEAR(static_cast<double>(rig.row(1)["output"]["pan"]), 0.6, 1e-6) << "the row keeps its own";
  rig.run("row.setFormat", R"({"row":1,"format":"mono"})");
  EXPECT_EQ(pan(), "rows");
  EXPECT_NEAR(paramValue(rig, "chainPanRight"), 0.6f, 1e-6f);
  // A row that ends in a mixer has no send to pan.
  rig.run("row.setInput", R"({"row":1,"source":"none"})");
  rig.run("split.add", R"({"row":0,"splitAt":2,"mixAt":5})");
  EXPECT_EQ(pan(), "held");
}

}  // namespace

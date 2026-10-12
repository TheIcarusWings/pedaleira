// The grid's commands: grid.*, row.*, the
// row and slot arguments of the add commands, what the read commands gained,
// and the legacy routing commands meeting a grid.

#include "grid_test_helpers.h"

#include <gtest/gtest.h>

#include <string>

namespace {

const char* kToGrid = R"({"to":"grid"})";
const char* kToLegacy = R"({"to":"legacy"})";

void expectFails(GridRig& rig, const char* command, const char* args, const char* code,
                 const char* messagePart) {
  SCOPED_TRACE(std::string(command) + " " + args);
  const juce::String revision = juce::JSON::toString(rig.proc.getGridState()["rows"]);
  const CommandResult result = rig.tryRun(command, args);
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.errorCode, juce::String(code));
  EXPECT_TRUE(result.errorMessage.containsIgnoreCase(messagePart)) << result.errorMessage;
  EXPECT_EQ(juce::JSON::toString(rig.proc.getGridState()["rows"]), revision)
      << "a failed command changed the grid";
}

TEST(GridCommandsTest, EveryGridCommandIsRegisteredAndDescribed) {
  GridRig rig;
  for (const char* name : {"grid.get", "grid.convert", "grid.moveBlock", "row.setInput",
                           "row.setOutput", "row.setFormat", "row.setMix", "row.setInputStage",
                           "split.add", "split.move", "split.remove", "split.setType",
                           "split.setParam", "split.listParams"}) {
    EXPECT_TRUE(rig.commands.registry().contains(name)) << name;
    EXPECT_TRUE(rig.commands.registry().describe(name).isObject()) << name;
  }
  const juce::String reference = rig.commands.markdownReference();
  EXPECT_TRUE(reference.contains("row.setInputStage"));
  EXPECT_TRUE(reference.contains("split.setParam"));
}

TEST(GridCommandsTest, GridGetDescribesBothModes) {
  GridRig rig(2, 4);
  juce::var grid = rig.run("grid.get");
  EXPECT_EQ(grid["mode"].toString(), "legacy");
  EXPECT_TRUE(static_cast<bool>(grid["legacyProjectable"]));
  EXPECT_EQ(grid["rows"].size(), 0);
  EXPECT_EQ(grid["imageStage"].toString(), "legacy");
  EXPECT_TRUE(grid["cpu"]["total"].isDouble());
  EXPECT_EQ(grid["io"]["inputs"].size(), 2);
  EXPECT_EQ(grid["io"]["outputs"].size(), 2);
  EXPECT_EQ(static_cast<int>(grid["io"]["outputs"][1]["pair"]), 2);
  EXPECT_TRUE(grid.hasProperty("revision"));

  rig.run("grid.convert", kToGrid);
  rig.addKind("utility.gain", 0, 3);
  grid = rig.run("grid.get");
  EXPECT_EQ(grid["mode"].toString(), "grid");
  ASSERT_EQ(grid["rows"].size(), kGridRows);
  const juce::var row = grid["rows"][0];
  for (const char* key : {"index", "format", "input", "output", "slots", "active", "routed",
                          "latencySamples", "padSamples", "cpu", "inputMissing", "outputFallback"})
    EXPECT_TRUE(row.hasProperty(key)) << key;
  ASSERT_EQ(row["slots"].size(), kGridCols);
  ASSERT_EQ(row["active"].size(), kGridCols);
  EXPECT_TRUE(static_cast<bool>(row["active"][3]));
  EXPECT_FALSE(static_cast<bool>(row["active"][0])) << "an empty slot runs nothing";
  EXPECT_EQ(row["slots"][3].toString().toStdString(), rig.proc.gridSlotBlockId(0, 3));
  EXPECT_EQ(grid["splits"].size(), 0);
  ASSERT_EQ(grid["groups"].size(), 1);
  EXPECT_EQ(static_cast<int>(grid["groups"][0][0]), 0);
  EXPECT_EQ(grid["imageStage"].toString(), "none");
  EXPECT_EQ(static_cast<int>(grid["latencySamples"]), rig.proc.getLatencySamples());
  EXPECT_TRUE(grid.hasProperty("chainLatencySamples"));

  // state.get carries the same object; a chain command's result lists every
  // row's slots.
  const juce::var state = rig.run("state.get");
  EXPECT_EQ(state["grid"]["mode"].toString(), "grid");
  EXPECT_EQ(state["grid"]["rows"].size(), kGridRows);
  const juce::var undone = rig.run("chain.undo");
  ASSERT_EQ(undone["rows"].size(), kGridRows);
  EXPECT_EQ(undone["rows"][0].size(), kGridCols);
  // This grid is a legacy rig in shape, so `left` is the lane a legacy
  // client is shown: as long as a legacy lane, not as a row.
  EXPECT_EQ(undone["left"].size(), kMinLaneSlots);
}

TEST(GridCommandsTest, ConvertBothWaysAndIdempotent) {
  GridRig rig;
  juce::var result = rig.run("grid.convert", kToGrid);
  EXPECT_EQ(result["mode"].toString(), "grid");
  EXPECT_TRUE(result["lossy"].isArray());
  const int revision = result["revision"];
  // Already a grid: fine, and no undo step.
  result = rig.run("grid.convert", kToGrid);
  EXPECT_EQ(static_cast<int>(result["revision"]), revision);
  result = rig.run("grid.convert", kToLegacy);
  EXPECT_EQ(result["mode"].toString(), "legacy");
  EXPECT_FALSE(rig.proc.isGridMode());
  result = rig.run("grid.convert", kToLegacy);
  EXPECT_EQ(result["mode"].toString(), "legacy");

  EXPECT_EQ(rig.tryRun("grid.convert", R"({"to":"mesh"})").errorCode,
            juce::String(CommandError::invalidArgs));
  EXPECT_EQ(rig.tryRun("grid.convert").errorCode, juce::String(CommandError::invalidArgs));
}

TEST(GridCommandsTest, RowSetInput) {
  GridRig rig(4, 2);
  rig.run("grid.convert", kToGrid);
  juce::var result = rig.run("row.setInput", R"({"row":2,"source":"device","channel":3,"stereo":true,"fold":"sum"})");
  EXPECT_EQ(static_cast<int>(result["row"]["index"]), 2);
  EXPECT_EQ(result["row"]["input"]["type"].toString(), "device");
  EXPECT_EQ(static_cast<int>(result["row"]["input"]["channel"]), 3);
  EXPECT_TRUE(static_cast<bool>(result["row"]["input"]["stereo"]));
  EXPECT_EQ(result["row"]["input"]["fold"].toString(), "sum");
  EXPECT_FALSE(static_cast<bool>(result["row"]["routed"])) << "no output yet";
  EXPECT_TRUE(result.hasProperty("revision"));

  // The defaults: channel 1, mono; a fold on a single channel means nothing.
  result = rig.run("row.setInput", R"({"row":2,"source":"device","fold":"sum"})");
  EXPECT_EQ(static_cast<int>(result["row"]["input"]["channel"]), 1);
  EXPECT_FALSE(static_cast<bool>(result["row"]["input"]["stereo"]));
  EXPECT_EQ(result["row"]["input"]["fold"].toString(), "none");

  result = rig.run("row.setInput", R"({"row":0,"source":"none"})");
  EXPECT_EQ(result["row"]["input"]["type"].toString(), "none");
  EXPECT_FALSE(static_cast<bool>(result["row"]["routed"]));

  expectFails(rig, "row.setInput", R"({"row":4,"source":"device"})", CommandError::invalidArgs, "row");
  expectFails(rig, "row.setInput", R"({"row":0,"source":"device","channel":9})",
              CommandError::invalidArgs, "channel");
  expectFails(rig, "row.setInput", R"({"row":0,"source":"device","channel":0})",
              CommandError::invalidArgs, "channel");
  expectFails(rig, "row.setInput", R"({"row":0,"source":"device","channel":8,"stereo":true})",
              CommandError::invalidArgs, "pair");
  expectFails(rig, "row.setInput", R"({"row":0,"source":"row"})", CommandError::invalidArgs,
              "source");
  expectFails(rig, "row.setInput", R"({"row":0})", CommandError::invalidArgs, "source");
}

TEST(GridCommandsTest, RowSetOutput) {
  GridRig rig(2, 4);
  rig.run("grid.convert", kToGrid);
  juce::var result = rig.run("row.setOutput", R"({"row":0,"target":"device","pair":2})");
  EXPECT_EQ(result["row"]["output"]["type"].toString(), "device");
  EXPECT_EQ(static_cast<int>(result["row"]["output"]["pair"]), 2);
  EXPECT_FALSE(static_cast<bool>(result["row"]["outputFallback"]));
  result = rig.run("row.setOutput", R"({"row":0,"target":"device","pair":4})");
  EXPECT_TRUE(static_cast<bool>(result["row"]["outputFallback"])) << "Out 7/8 is not on the device";
  result = rig.run("row.setOutput", R"({"row":0,"target":"none"})");
  EXPECT_EQ(result["row"]["output"]["type"].toString(), "none");
  EXPECT_FALSE(static_cast<bool>(result["row"]["routed"]));

  // Another row: its input, as long as that row has none of its own.
  expectFails(rig, "row.setOutput", R"({"row":0,"target":"row"})", CommandError::invalidArgs,
              "toRow");
  expectFails(rig, "row.setOutput", R"({"row":0,"target":"row","toRow":0})", CommandError::failed,
              "cannot feed itself");
  rig.run("row.setInput", R"({"row":1,"source":"device","channel":2})");
  expectFails(rig, "row.setOutput", R"({"row":0,"target":"row","toRow":1})", CommandError::failed,
              "input of its own");
  rig.run("row.setInput", R"({"row":1,"source":"none"})");
  result = rig.run("row.setOutput", R"({"row":0,"target":"row","toRow":1})");
  EXPECT_EQ(result["row"]["output"]["type"].toString(), "row");
  EXPECT_EQ(static_cast<int>(result["row"]["output"]["toRow"]), 1);
  EXPECT_FALSE(static_cast<bool>(result["row"]["routed"])) << "row 1 leads nowhere yet";
  result = rig.run("row.setOutput", R"({"row":1,"target":"device","pair":1})");
  EXPECT_TRUE(static_cast<bool>(result["rows"][0]["routed"]));
  EXPECT_EQ(result["rows"][1]["source"].toString(), "rows");
  ASSERT_EQ(result["feeds"].size(), 1);
  EXPECT_EQ(static_cast<int>(result["feeds"][0]["from"]), 0);
  EXPECT_EQ(static_cast<int>(result["feeds"][0]["to"]), 1);
  // A loop is refused, and a fed row cannot take an input.
  expectFails(rig, "row.setOutput", R"({"row":1,"target":"row","toRow":0})", CommandError::failed,
              "input of its own");
  expectFails(rig, "row.setInput", R"({"row":1,"source":"device","channel":1})",
              CommandError::failed, "input of its own");
  rig.run("row.setOutput", R"({"row":0,"target":"none"})");
  expectFails(rig, "row.setOutput", R"({"row":0,"target":"device","pair":5})",
              CommandError::invalidArgs, "pair");
  expectFails(rig, "row.setOutput", R"({"row":-1,"target":"none"})", CommandError::invalidArgs,
              "row");
}

TEST(GridCommandsTest, RowSetFormatMixAndInputStage) {
  GridRig rig(2, 4);
  rig.run("grid.convert", kToGrid);
  for (const char* format : {"mono", "dual", "stereo"}) {
    const juce::var result =
        rig.run("row.setFormat", juce::String("{\"row\":1,\"format\":\"") + format + "\"}");
    EXPECT_EQ(result["row"]["format"].toString(), format);
  }
  expectFails(rig, "row.setFormat", R"({"row":1,"format":"quad"})", CommandError::invalidArgs,
              "format");

  juce::var result = rig.run("row.setMix", R"({"row":1,"volumeDb":-7.5,"pan":0.1,"mute":true,"solo":true,"invert":true})");
  const juce::var output = result["row"]["output"];
  EXPECT_NEAR(static_cast<double>(output["volumeDb"]), -7.5, 1e-6);
  EXPECT_NEAR(static_cast<double>(output["pan"]), 0.1, 1e-6);
  EXPECT_TRUE(static_cast<bool>(output["mute"]));
  EXPECT_TRUE(static_cast<bool>(output["solo"]));
  EXPECT_TRUE(static_cast<bool>(output["invert"]));
  // Fields left out stay as they are.
  result = rig.run("row.setMix", R"({"row":1,"mute":false})");
  EXPECT_FALSE(static_cast<bool>(result["row"]["output"]["mute"]));
  EXPECT_NEAR(static_cast<double>(result["row"]["output"]["volumeDb"]), -7.5, 1e-6);
  EXPECT_TRUE(static_cast<bool>(result["row"]["output"]["solo"]));
  expectFails(rig, "row.setMix", R"({"row":1,"volumeDb":13})", CommandError::invalidArgs, "volumeDb");
  expectFails(rig, "row.setMix", R"({"row":1,"volumeDb":-61})", CommandError::invalidArgs,
              "volumeDb");
  expectFails(rig, "row.setMix", R"({"row":1,"pan":1.5})", CommandError::invalidArgs, "pan");
  expectFails(rig, "row.setMix", R"({"row":9,"pan":0.5})", CommandError::invalidArgs, "row");

  result = rig.run("row.setInputStage", R"({"row":0,"trimDb":-5.5,"gate":false,"pitch":false})");
  EXPECT_NEAR(static_cast<double>(result["row"]["input"]["trimDb"]), -5.5, 1e-6);
  EXPECT_FALSE(static_cast<bool>(result["row"]["input"]["gate"]));
  EXPECT_FALSE(static_cast<bool>(result["row"]["input"]["pitch"]));
  result = rig.run("row.setInputStage", R"({"row":0,"pitch":true})");
  EXPECT_TRUE(static_cast<bool>(result["row"]["input"]["pitch"]));
  EXPECT_FALSE(static_cast<bool>(result["row"]["input"]["gate"]));
  expectFails(rig, "row.setInputStage", R"({"row":0,"trimDb":30})", CommandError::invalidArgs,
              "trimDb");
  EXPECT_EQ(rig.proc.validateActivePlanForTesting(), "");
}

TEST(GridCommandsTest, ARowCommandConvertsALegacyRigFirst) {
  for (const char* command : {"row.setInput", "row.setOutput", "row.setFormat", "row.setMix",
                              "row.setInputStage"}) {
    SCOPED_TRACE(command);
    GridRig rig(2, 4);
    const juce::String name(command);
    const char* args = name == "row.setInput" ? R"({"row":1,"source":"device","channel":2})"
                           : name == "row.setOutput" ? R"({"row":0,"target":"device","pair":2})"
                           : name == "row.setFormat" ? R"({"row":0,"format":"dual"})"
                           : name == "row.setMix" ? R"({"row":0,"pan":0.3})"
                                                  : R"({"row":0,"trimDb":3})";
    const juce::var result = rig.run(command, args);
    EXPECT_EQ(result["mode"].toString(), "grid");
    EXPECT_TRUE(rig.proc.isGridMode());
    // The conversion happened (row 0 is the legacy chain) and the edit too.
    EXPECT_EQ(rig.row(0)["input"]["type"].toString(), "device");
    ASSERT_TRUE(rig.proc.undoChain());
    EXPECT_FALSE(rig.proc.isGridMode()) << "conversion and edit are one undo step";
    EXPECT_FALSE(static_cast<bool>(rig.proc.getChainState(-1)["canUndo"]));
  }
}

TEST(GridCommandsTest, MoveBlock) {
  GridRig rig;
  rig.run("grid.convert", kToGrid);
  const juce::String a = rig.addKind("utility.gain", 0, 0);
  const juce::String b = rig.addKind("utility.gain", 0, 1);
  const juce::String c = rig.addKind("utility.gain", 0, 2);
  const auto at = [&](int row, int slot) { return juce::String(rig.proc.gridSlotBlockId(row, slot)); };

  // Swap with a block: the two exchange places.
  juce::var result = rig.run("grid.moveBlock", "{\"blockId\":\"" + a + "\",\"row\":0,\"slot\":2}");
  EXPECT_EQ(result["blockId"].toString(), a);
  EXPECT_EQ(result["rows"][0][2].toString(), a);
  EXPECT_EQ(at(0, 0), c);
  EXPECT_EQ(at(0, 1), b);
  // Swap with an empty slot of another row: the empty slot comes back.
  const juce::String empty = at(2, 6);
  rig.run("grid.moveBlock", "{\"blockId\":\"" + a + "\",\"row\":2,\"slot\":6,\"mode\":\"swap\"}");
  EXPECT_EQ(at(2, 6), a);
  EXPECT_EQ(at(0, 2), empty);

  // Insert before a block: the blocks from there on move toward the row's
  // nearest empty slot, and the source leaves an empty slot behind.
  rig.run("grid.moveBlock", "{\"blockId\":\"" + a + "\",\"row\":0,\"slot\":0,\"mode\":\"insert\"}");
  EXPECT_EQ(at(0, 0), a);
  EXPECT_EQ(at(0, 1), c);
  EXPECT_EQ(at(0, 2), b);
  EXPECT_EQ(rig.proc.getChainState(-1)["grid"]["rows"][2]["blocks"][6]["kind"].toString(), "insert");
  // Within a row, to the end: the others close the gap.
  rig.run("grid.moveBlock", "{\"blockId\":\"" + a + "\",\"row\":0,\"slot\":7,\"mode\":\"insert\"}");
  EXPECT_EQ(at(0, 7), a);
  for (int row = 0; row < kGridRows; ++row)
    EXPECT_EQ(rig.row(row)["slots"].size(), kGridCols);
  EXPECT_EQ(rig.proc.validateActivePlanForTesting(), "");

  expectFails(rig, "grid.moveBlock", R"({"blockId":"nope","row":0,"slot":0})", CommandError::notFound,
              "no block");
  expectFails(rig, "grid.moveBlock", ("{\"blockId\":\"" + at(1, 5) + "\",\"row\":1,\"slot\":0}").toRawUTF8(),
              CommandError::failed, "empty slot");
  expectFails(rig, "grid.moveBlock", ("{\"blockId\":\"" + a + "\",\"row\":4,\"slot\":0}").toRawUTF8(),
              CommandError::invalidArgs, "row");
  expectFails(rig, "grid.moveBlock", ("{\"blockId\":\"" + a + "\",\"row\":0,\"slot\":8}").toRawUTF8(),
              CommandError::invalidArgs, "slot");
  expectFails(rig, "grid.moveBlock", ("{\"blockId\":\"" + a + "\",\"row\":0,\"slot\":0,\"mode\":\"shove\"}").toRawUTF8(),
              CommandError::invalidArgs, "mode");

  // Inserting into a full row fails; swapping into it works.
  for (int slot = 0; slot < kGridCols; ++slot)
    rig.addKind("utility.gain", 3, slot);
  expectFails(rig, "grid.moveBlock", ("{\"blockId\":\"" + a + "\",\"row\":3,\"slot\":2,\"mode\":\"insert\"}").toRawUTF8(),
              CommandError::failed, "full");
  rig.run("grid.moveBlock", "{\"blockId\":\"" + a + "\",\"row\":3,\"slot\":2}");
  EXPECT_EQ(at(3, 2), a);

  // On a legacy rig the move converts first, as one undo step.
  GridRig legacy;
  const juce::String id = legacy.run("chain.addBlock", R"({"kind":"utility.gain"})")["blockId"].toString();
  legacy.run("grid.moveBlock", "{\"blockId\":\"" + id + "\",\"row\":1,\"slot\":7}");
  EXPECT_TRUE(legacy.proc.isGridMode());
  EXPECT_EQ(juce::String(legacy.proc.gridSlotBlockId(1, 7)), id);
  ASSERT_TRUE(legacy.proc.undoChain());
  EXPECT_FALSE(legacy.proc.isGridMode());
}

TEST(GridCommandsTest, AddAndDuplicateByRowAndSlot) {
  GridRig rig;
  // Row and slot address the grid: a legacy rig has to be converted first.
  expectFails(rig, "chain.addBlock", R"({"kind":"utility.gain","row":0,"slot":0})",
              CommandError::failed, "grid.convert");
  rig.run("grid.convert", kToGrid);

  juce::var added = rig.run("chain.addBlock", R"({"kind":"utility.gain","row":2,"slot":5})");
  const juce::String first = added["blockId"].toString();
  EXPECT_EQ(juce::String(rig.proc.gridSlotBlockId(2, 5)), first);
  EXPECT_EQ(added["block"]["blockId"].toString(), first) << "found in a row past the two lanes";
  // Onto a block: replaced in place, keeping the id (as with `target`).
  added = rig.run("chain.addBlock", R"({"kind":"utility.gain","row":2,"slot":5})");
  EXPECT_EQ(added["blockId"].toString(), first);

  expectFails(rig, "chain.addBlock", R"({"kind":"utility.gain","row":2})", CommandError::invalidArgs,
              "go together");
  expectFails(rig, "chain.addBlock", R"({"kind":"utility.gain","slot":2})", CommandError::invalidArgs,
              "go together");
  expectFails(rig, "chain.addBlock", R"({"kind":"utility.gain","row":2,"slot":1,"lane":"left"})",
              CommandError::invalidArgs, "not both");
  expectFails(rig, "chain.addBlock", R"({"kind":"utility.gain","row":4,"slot":1})",
              CommandError::invalidArgs, "row");
  expectFails(rig, "chain.addBlock", R"({"kind":"utility.gain","row":1,"slot":8})",
              CommandError::invalidArgs, "slot");

  // A tone from files into a slot.
  const juce::String ir = testFile("cab-ir-test.wav").getFullPathName().replace("\\", "/");
  added = rig.run("chain.addToneFromFiles", "{\"paths\":[\"" + ir + "\"],\"row\":1,\"slot\":3}");
  const juce::String tone = added["blockId"].toString();
  EXPECT_EQ(juce::String(rig.proc.gridSlotBlockId(1, 3)), tone);
  EXPECT_TRUE(static_cast<bool>(rig.run("state.waitUntilLoaded")["loaded"]))
      << "waits for blocks in every row";
  // Block commands reach a block in any row.
  rig.run("block.setParam", "{\"blockId\":\"" + first + "\",\"param\":\"gainDb\",\"value\":-3}");
  EXPECT_TRUE(rig.run("block.listParams", "{\"blockId\":\"" + first + "\"}")["params"].isArray());

  // A duplicate by row and slot: an empty slot is filled, a taken one pushes.
  juce::var copy = rig.run("chain.duplicateBlock", "{\"blockId\":\"" + first + "\",\"row\":3,\"slot\":0}");
  const juce::String second = copy["blockId"].toString();
  EXPECT_EQ(juce::String(rig.proc.gridSlotBlockId(3, 0)), second);
  copy = rig.run("chain.duplicateBlock", "{\"blockId\":\"" + first + "\",\"row\":3,\"slot\":0}");
  EXPECT_EQ(juce::String(rig.proc.gridSlotBlockId(3, 0)), copy["blockId"].toString());
  EXPECT_EQ(juce::String(rig.proc.gridSlotBlockId(3, 1)), second);
  EXPECT_EQ(rig.row(3)["slots"].size(), kGridCols);
  // Without a place: right after the source, in the source's row.
  copy = rig.run("chain.duplicateBlock", "{\"blockId\":\"" + first + "\"}");
  EXPECT_EQ(juce::String(rig.proc.gridSlotBlockId(2, 6)), copy["blockId"].toString());
  expectFails(rig, "chain.duplicateBlock", ("{\"blockId\":\"" + first + "\",\"row\":3,\"slot\":0,\"side\":\"left\"}").toRawUTF8(),
              CommandError::invalidArgs, "not both");

  // chain.removeBlock empties the slot in place.
  const juce::var removed = rig.run("chain.removeBlock", "{\"blockId\":\"" + second + "\"}");
  EXPECT_EQ(removed["rows"][3].size(), kGridCols);
  EXPECT_NE(removed["rows"][3][1].toString(), second);

  // chain.addPlugin takes the pair too; without a plugin to add it gets as
  // far as looking the plugin up.
  expectFails(rig, "chain.addPlugin", R"({"identifier":"no-such-plugin","row":0,"slot":0})",
              CommandError::notFound, "");
  expectFails(rig, "chain.addPlugin", R"({"identifier":"x","row":0})", CommandError::invalidArgs,
              "go together");
  EXPECT_EQ(rig.proc.validateActivePlanForTesting(), "");
}

TEST(GridCommandsTest, MetersGainRowsOutputsAndPlanCpu) {
  GridRig rig(2, 4);
  juce::var meters = rig.run("meters.get");
  EXPECT_EQ(meters["rows"].size(), 0) << "a legacy rig has no rows";
  ASSERT_EQ(meters["outputs"].size(), 1);
  EXPECT_TRUE(static_cast<bool>(meters["outputs"][0]["main"]));
  EXPECT_TRUE(meters["planCpu"]["total"].isDouble());
  EXPECT_TRUE(meters["planCpu"]["critical"].isDouble());
  EXPECT_TRUE(meters.hasProperty("cpu")) << "the measured load is still there";

  rig.run("grid.convert", kToGrid);
  rig.run("row.setInput", R"({"row":1,"source":"device","channel":2})");
  rig.run("row.setOutput", R"({"row":1,"target":"device","pair":2})");
  rig.settle();
  rig.process({makeNoise(2048, 1, 0.2f), gridSilence(2048)});
  meters = rig.run("meters.get");
  ASSERT_EQ(meters["rows"].size(), kGridRows);
  EXPECT_GT(static_cast<double>(meters["rows"][0]["in"][0]), -40.0);
  EXPECT_GT(static_cast<double>(meters["rows"][0]["out"][1]), -40.0);
  EXPECT_LE(static_cast<double>(meters["rows"][1]["in"][0]), -60.0) << "input 2 is silent";
  EXPECT_FALSE(static_cast<bool>(meters["rows"][2]["active"]));
  ASSERT_EQ(meters["outputs"].size(), 2);
  EXPECT_EQ(static_cast<int>(meters["outputs"][0]["pair"]), 1);
  EXPECT_GT(static_cast<double>(meters["outputs"][0]["level"][0]), -40.0);
  EXPECT_LE(static_cast<double>(meters["outputs"][1]["level"][0]), -60.0);
  EXPECT_GT(static_cast<double>(meters["output"][0]), -40.0) << "the faceplate meters are Out 1/2";
  EXPECT_GT(static_cast<double>(meters["input"][0]), -40.0);
}

TEST(GridCommandsTest, LegacyRoutingCommandsConvertAProjectableGridBack) {
  GridRig rig;
  rig.run("chain.addBlock", R"({"kind":"utility.gain"})");

  rig.run("grid.convert", kToGrid);
  juce::var result = rig.run("chain.setStereo", R"({"enabled":true})");
  EXPECT_FALSE(rig.proc.isGridMode());
  EXPECT_TRUE(static_cast<bool>(result["stereoEnabled"]));
  EXPECT_TRUE(result["rows"].isVoid()) << "a legacy result";

  rig.run("grid.convert", kToGrid);
  result = rig.run("chain.setInputMode", R"({"mode":"left"})");
  EXPECT_FALSE(rig.proc.isGridMode());
  EXPECT_EQ(result["inputMode"].toString(), "left");

  rig.run("grid.convert", kToGrid);
  rig.run("chain.swapLanes");
  EXPECT_FALSE(rig.proc.isGridMode());

  rig.run("grid.convert", kToGrid);
  const juce::String tap = rig.proc.getChainState(-1)["chainRight"][0]["blockId"].toString();
  ASSERT_EQ(rig.proc.getChainState(-1)["chainRight"][0]["kind"].toString(), "tone");
  result = rig.run("chain.setBranch", "{\"side\":\"right\",\"afterBlockId\":\"" + tap + "\"}");
  EXPECT_FALSE(rig.proc.isGridMode());
  EXPECT_EQ(result["branch"]["afterBlockId"].toString(), tap);
  // Branched, the rig converts to a split that never rejoins, and the
  // legacy command that clears a branch takes it back and clears it.
  result = rig.run("grid.convert", kToGrid);
  ASSERT_EQ(result["splits"].size(), 1);
  EXPECT_TRUE(static_cast<bool>(result["legacyProjectable"]));
  EXPECT_EQ(rig.proc.getChainState(-1)["branch"]["afterBlockId"].toString(), tap);
  result = rig.run("chain.clearBranch");
  EXPECT_FALSE(rig.proc.isGridMode());
  EXPECT_TRUE(static_cast<bool>(result["changed"]));
  EXPECT_TRUE(rig.proc.getChainState(-1)["branch"].isVoid());

  rig.run("grid.convert", kToGrid);
  result = rig.run("chain.clearBranch");
  EXPECT_FALSE(rig.proc.isGridMode());
  EXPECT_FALSE(static_cast<bool>(result["changed"]));
}

TEST(GridCommandsTest, LegacyRoutingCommandsFailOnAGridThatIsNotOne) {
  GridRig rig(2, 4);
  rig.run("grid.convert", kToGrid);
  const juce::String id = rig.addKind("utility.gain", 0, 0);
  rig.run("row.setOutput", R"({"row":0,"target":"device","pair":2})");
  for (const auto& [command, args] :
       {std::pair<const char*, const char*>{"chain.setStereo", R"({"enabled":true})"},
        {"chain.setInputMode", R"({"mode":"left"})"},
        {"chain.clearBranch", ""},
        {"chain.swapLanes", ""}}) {
    expectFails(rig, command, args, CommandError::failed, "output pair 2");
    EXPECT_TRUE(rig.proc.isGridMode());
  }
  expectFails(rig, "chain.setBranch", ("{\"side\":\"left\",\"afterBlockId\":\"" + id + "\"}").toRawUTF8(),
              CommandError::failed, "legacy routing command");
  expectFails(rig, "chain.moveBlock", ("{\"blockId\":\"" + id + "\",\"side\":\"right\",\"index\":0}").toRawUTF8(),
              CommandError::failed, "grid.moveBlock");

  // Everything that is not routing still works on it.
  rig.run("block.setParam", "{\"blockId\":\"" + id + "\",\"param\":\"gainDb\",\"value\":2}");
  rig.run("block.setEnabled", "{\"blockId\":\"" + id + "\",\"enabled\":false}");
  rig.run("chain.undo");
  rig.run("chain.redo");
  EXPECT_TRUE(rig.proc.isGridMode());
  EXPECT_EQ(rig.proc.validateActivePlanForTesting(), "");
}

}  // namespace

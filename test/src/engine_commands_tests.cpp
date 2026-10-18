// EngineCommands: the engine driven purely through named commands, the way an
// MCP server or the remote will drive it. A rig is built, edited, saved and
// undone through commands only, and state.get has to show every change; the
// last test renders a command-built rig against the same rig built by direct
// processor calls and requires identical samples.
//
// Like local_load_tests.cpp, adding tones from files writes their
// content-addressed copies into the LocalModels stash (a per-process temp dir,
// set in the test main). Presets go to a temp store per test.

#include "EngineCommands.h"
#include "chain_test_helpers.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

namespace {

juce::var parseJson(const juce::String& json) {
  juce::var value;
  EXPECT_TRUE(juce::JSON::parse(json, value).wasOk()) << json.toStdString();
  return value;
}

juce::String quoted(const juce::String& text) { return juce::JSON::toString(juce::var(text)); }

juce::String pathOf(const char* name) { return quoted(testFile(name).getFullPathName()); }

// A processor with its presets in a temp store and the command set over it.
struct CommandRig {
  CommandRig()
      : presetDir(juce::File::getSpecialLocation(juce::File::tempDirectory)
                      .getChildFile("t3k-command-tests-" + juce::Uuid().toString())) {
    EXPECT_TRUE(presetDir.createDirectory().wasOk());
    proc.setPresetStoreForTesting(presetDir);
    proc.setMultiCoreEnabled(false, /*persist=*/false);
  }
  ~CommandRig() { presetDir.deleteRecursively(); }

  CommandResult run(const char* name, const juce::String& argsJson = "{}") {
    return commands.registry().run(name, parseJson(argsJson));
  }

  // Runs a command that has to succeed and returns its result value.
  juce::var must(const char* name, const juce::String& argsJson = "{}") {
    const CommandResult result = run(name, argsJson);
    EXPECT_TRUE(result.ok) << name << " " << argsJson.toStdString() << ": "
                           << result.errorCode.toStdString() << ": "
                           << result.errorMessage.toStdString();
    return result.value;
  }

  // Adds one tone block from a test asset; returns its id.
  juce::String add(const char* fileName, const juce::String& extraArgs = {}) {
    return must("chain.addToneFromFiles", "{\"paths\":[" + pathOf(fileName) + "]" + extraArgs + "}")
        ["blockId"].toString();
  }

  void waitLoaded() {
    const juce::var result = must("state.waitUntilLoaded", R"({"timeoutMs":30000})");
    EXPECT_TRUE(static_cast<bool>(result["loaded"]));
  }

  juce::var state() { return must("state.get"); }

  // Preset files in the temp library (each setlist folder also keeps a
  // setlist.json).
  int presetFileCount() const {
    int count = 0;
    for (const auto& file : presetDir.getChildFile("Library").getChildFile("Setlists").findChildFiles(
             juce::File::findFiles, true, "*.json"))
      if (file.getFileName() != "setlist.json")
        ++count;
    return count;
  }

  // A block's entry in state.get's chain, from either lane; void when absent.
  juce::var block(const juce::String& blockId) {
    const juce::var chain = state()["chain"];
    for (const char* key : {"chain", "chainRight"})
      if (const auto* lane = chain[key].getArray())
        for (const auto& item : *lane)
          if (item["blockId"].toString() == blockId)
            return item;
    return {};
  }

  juce::File presetDir;
  TONE3000Processor proc;
  EngineCommands commands{proc};
};

juce::String blockArgs(const juce::String& blockId, const juce::String& rest = {}) {
  return "{\"blockId\":" + quoted(blockId) + rest + "}";
}

void expectError(const CommandResult& result, const char* code) {
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.errorCode, juce::String(code)) << result.errorMessage.toStdString();
  EXPECT_TRUE(result.errorMessage.isNotEmpty());
}

juce::StringArray idsOf(const juce::var& array) {
  juce::StringArray ids;
  if (const auto* items = array.getArray())
    for (const auto& item : *items)
      ids.add(item.toString());
  return ids;
}

// Ids of the tone blocks in one lane of a chain-state payload, in order.
juce::StringArray toneIds(const juce::var& chainState, const char* key) {
  juce::StringArray ids;
  if (const auto* lane = chainState[key].getArray())
    for (const auto& item : *lane)
      if (item["kind"].toString() == "tone")
        ids.add(item["blockId"].toString());
  return ids;
}

// Every block id in one lane of a chain-state payload, insert slots included.
juce::StringArray laneIds(const juce::var& chainState, const char* key) {
  juce::StringArray ids;
  if (const auto* lane = chainState[key].getArray())
    for (const auto& item : *lane)
      ids.add(item["blockId"].toString());
  return ids;
}

juce::var presetNamed(const juce::var& presetList, const char* name) {
  if (const auto* presets = presetList["presets"].getArray())
    for (const auto& preset : *presets)
      if (preset["name"].toString() == name)
        return preset;
  return {};
}

}  // namespace

TEST(EngineCommandsTest, BuildsARigAndStateGetReflectsEveryChange) {
  CommandRig rig;
  const int revisionAtStart = rig.must("state.revision")["revision"];

  const juce::var added = rig.must("chain.addToneFromFiles",
                                   "{\"paths\":[" + pathOf("a2-amp-test.nam") + "]}");
  const juce::String amp = added["blockId"].toString();
  ASSERT_TRUE(amp.isNotEmpty());
  // The edit answers with the block it made, still loading.
  EXPECT_EQ(added["block"]["blockId"].toString(), amp);
  EXPECT_EQ(added["block"]["tone"]["format"].toString(), juce::String("nam"));
  EXPECT_GT(static_cast<int>(added["revision"]), revisionAtStart);

  const juce::String cab = rig.add("cab-ir-test.wav");
  ASSERT_TRUE(cab.isNotEmpty());
  EXPECT_NE(amp, cab);

  rig.waitLoaded();

  rig.must("block.setParam", blockArgs(amp, R"(,"param":"inputGain","value":0.7)"));
  rig.must("block.setParam", blockArgs(amp, R"(,"param":"outputGain","value":0.4)"));
  rig.must("block.setParam", blockArgs(amp, R"(,"param":"normalize","value":0)"));
  const juce::var mixed = rig.must("block.setParam", blockArgs(cab, R"(,"param":"mix","value":0.25)"));
  EXPECT_NEAR(static_cast<double>(mixed["block"]["params"]["mix"]), 0.25, 1e-6);

  rig.must("block.setEqBand", blockArgs(cab, R"(,"band":2,"freqHz":800,"gainDb":6,"q":2)"));
  // A partial edit keeps the rest of the band.
  rig.must("block.setEqBand", blockArgs(cab, R"(,"band":2,"gainDb":-4.5)"));
  rig.must("block.setEqBand", blockArgs(cab, R"(,"band":0,"type":"lowcut","freqHz":90)"));
  rig.must("block.setEqPre", blockArgs(cab, R"(,"pre":true)"));
  rig.must("block.setEqEnabled", blockArgs(amp, R"(,"enabled":false)"));
  rig.must("block.setEnabled", blockArgs(cab, R"(,"enabled":false)"));
  rig.must("block.setSlimSize", blockArgs(amp, R"(,"slimSize":0.5)"));
  rig.waitLoaded();

  const juce::var state = rig.state();
  for (const char* section : {"chain", "params", "presets", "midi", "engine"})
    EXPECT_FALSE(state[section].isVoid()) << "state.get lacks " << section;

  // The amp took the first slot, the cab the next: signal order.
  const juce::StringArray tones = toneIds(state["chain"], "chain");
  ASSERT_EQ(tones.size(), 2);
  EXPECT_EQ(tones[0], amp);
  EXPECT_EQ(tones[1], cab);

  const juce::var ampBlock = rig.block(amp);
  EXPECT_TRUE(static_cast<bool>(ampBlock["loaded"]));
  EXPECT_TRUE(static_cast<bool>(ampBlock["tone"]["local"]));
  EXPECT_NEAR(static_cast<double>(ampBlock["params"]["inputGain"]), 0.7, 1e-6);
  EXPECT_NEAR(static_cast<double>(ampBlock["params"]["outputGain"]), 0.4, 1e-6);
  EXPECT_FALSE(static_cast<bool>(ampBlock["params"]["normalize"]));
  EXPECT_TRUE(static_cast<bool>(ampBlock["params"]["enabled"]));
  EXPECT_FALSE(static_cast<bool>(ampBlock["params"]["eq"]["enabled"]));
  EXPECT_NEAR(static_cast<double>(ampBlock["params"]["slimSize"]), 0.5, 1e-9);

  const juce::var cabBlock = rig.block(cab);
  EXPECT_TRUE(static_cast<bool>(cabBlock["loaded"]));
  EXPECT_EQ(cabBlock["tone"]["format"].toString(), juce::String("ir"));
  EXPECT_FALSE(static_cast<bool>(cabBlock["params"]["enabled"]));
  EXPECT_NEAR(static_cast<double>(cabBlock["params"]["mix"]), 0.25, 1e-6);
  EXPECT_TRUE(static_cast<bool>(cabBlock["params"]["eq"]["pre"]));
  const juce::var band = cabBlock["params"]["eq"]["bands"][2];
  EXPECT_NEAR(static_cast<double>(band["freqHz"]), 800.0, 1e-3);
  EXPECT_NEAR(static_cast<double>(band["gainDb"]), -4.5, 1e-6);
  EXPECT_NEAR(static_cast<double>(band["q"]), 2.0, 1e-6);
  EXPECT_EQ(cabBlock["params"]["eq"]["bands"][0]["type"].toString(), juce::String("lowcut"));
  EXPECT_NEAR(static_cast<double>(cabBlock["params"]["eq"]["bands"][0]["freqHz"]), 90.0, 1e-3);

  rig.must("block.resetEq", blockArgs(cab));
  const juce::var resetEq = rig.block(cab)["params"]["eq"];
  EXPECT_FALSE(static_cast<bool>(resetEq["pre"]));
  EXPECT_NEAR(static_cast<double>(resetEq["bands"][2]["gainDb"]), 0.0, 1e-9);
  EXPECT_EQ(resetEq["bands"][0]["type"].toString(), juce::String("lowshelf"));

  rig.must("block.setEnabled", blockArgs(cab, R"(,"enabled":true)"));
  EXPECT_TRUE(static_cast<bool>(rig.block(cab)["params"]["enabled"]));

  EXPECT_EQ(state["engine"]["oversampling"].toString(), juce::String("1"));
  EXPECT_FALSE(static_cast<bool>(state["engine"]["multiCore"]));
  EXPECT_TRUE(state["engine"]["version"].toString().containsChar('.'));
  EXPECT_FALSE(state["engine"]["latencySamples"].isVoid());
  EXPECT_FALSE(state["engine"]["sampleRate"].isVoid());
  EXPECT_FALSE(state["engine"]["blockSize"].isVoid());

  EXPECT_GT(static_cast<int>(rig.must("state.revision")["revision"]),
            static_cast<int>(added["revision"]));
}

// A capture loaded onto a block (the NAM editor's Load file..., a file dropped
// on a tile) replaces the tone and leaves everything the player set on it.
TEST(EngineCommandsTest, LoadingACaptureOnABlockKeepsItsSettings) {
  CommandRig rig;
  rig.must("grid.convert", R"({"to":"grid"})");
  const juce::String amp = rig.must("chain.addToneFromFiles",
                                    "{\"paths\":[" + pathOf("a2-amp-test.nam") + "],\"row\":0,\"slot\":2}")
                               ["blockId"].toString();
  rig.waitLoaded();
  rig.must("block.setParam", blockArgs(amp, R"(,"param":"inputGain","value":0.7)"));
  rig.must("block.setParam", blockArgs(amp, R"(,"param":"outputGain","value":0.4)"));
  rig.must("block.setParam", blockArgs(amp, R"(,"param":"normalize","value":0)"));
  rig.must("block.setEqBand", blockArgs(amp, R"(,"band":2,"freqHz":800,"gainDb":6,"q":2)"));
  rig.must("block.setEqEnabled", blockArgs(amp, R"(,"enabled":false)"));
  rig.must("block.setEnabled", blockArgs(amp, R"(,"enabled":false)"));
  rig.must("block.setSlimSize", blockArgs(amp, R"(,"slimSize":0.5)"));
  rig.waitLoaded();

  const auto expectKept = [&](const char* how) {
    const juce::var block = rig.block(amp);
    EXPECT_EQ(block["tone"]["title"].toString(), juce::String("a2-amp-cab-test")) << how;
    EXPECT_EQ(block["tone"]["format"].toString(), juce::String("nam")) << how;
    EXPECT_NEAR(static_cast<double>(block["params"]["inputGain"]), 0.7, 1e-6) << how;
    EXPECT_NEAR(static_cast<double>(block["params"]["outputGain"]), 0.4, 1e-6) << how;
    EXPECT_FALSE(static_cast<bool>(block["params"]["normalize"])) << how;
    EXPECT_FALSE(static_cast<bool>(block["params"]["enabled"])) << how;
    EXPECT_FALSE(static_cast<bool>(block["params"]["eq"]["enabled"])) << how;
    EXPECT_NEAR(static_cast<double>(block["params"]["eq"]["bands"][2]["gainDb"]), 6.0, 1e-6) << how;
    EXPECT_NEAR(static_cast<double>(block["params"]["slimSize"]), 0.5, 1e-9) << how;
  };

  // By block id, as the editor does.
  const juce::var byTarget = rig.must(
      "chain.addToneFromFiles",
      "{\"paths\":[" + pathOf("a2-amp-cab-test.nam") + "],\"target\":" + quoted(amp) + "}");
  EXPECT_EQ(byTarget["blockId"].toString(), amp);
  rig.waitLoaded();
  expectKept("by target");
  EXPECT_EQ(toneIds(rig.state()["chain"], "chain").size(), 1);

  // By row and slot, as a drop on the grid does.
  rig.must("chain.addToneFromFiles",
           "{\"paths\":[" + pathOf("a2-am-test-2.nam") + "],\"target\":" + quoted(amp) + "}");
  rig.waitLoaded();
  const juce::var bySlot = rig.must(
      "chain.addToneFromFiles",
      "{\"paths\":[" + pathOf("a2-amp-cab-test.nam") + "],\"row\":0,\"slot\":2}");
  EXPECT_EQ(bySlot["blockId"].toString(), amp);
  rig.waitLoaded();
  expectKept("by row and slot");
  EXPECT_EQ(toneIds(rig.state()["chain"], "chain").size(), 1);
}

TEST(EngineCommandsTest, AddToneFromFilesTargetsAndErrors) {
  CommandRig rig;

  // Several files of one kind are one block with that many models.
  const juce::var pack = rig.must(
      "chain.addToneFromFiles",
      "{\"paths\":[" + pathOf("a2-amp-test.nam") + "," + pathOf("a2-amp-cab-test.nam") + "]}");
  const juce::String packId = pack["blockId"].toString();
  rig.waitLoaded();
  EXPECT_EQ(rig.block(packId)["tone"]["models"].size(), 2);
  EXPECT_EQ(toneIds(rig.state()["chain"], "chain").size(), 1);

  // Targeting an existing tone block replaces its tone in place.
  const juce::var swapped = rig.must(
      "chain.addToneFromFiles",
      "{\"paths\":[" + pathOf("cab-ir-test.wav") + "],\"target\":" + quoted(packId) + "}");
  EXPECT_EQ(swapped["blockId"].toString(), packId);
  rig.waitLoaded();
  EXPECT_EQ(rig.block(packId)["tone"]["format"].toString(), juce::String("ir"));
  EXPECT_EQ(toneIds(rig.state()["chain"], "chain").size(), 1);

  // Targeting an empty insert slot puts the tone at that position (here the
  // lane's last slot, not the first free one an untargeted add would take).
  const juce::StringArray lane = laneIds(rig.state()["chain"], "chain");
  ASSERT_GE(lane.size(), 3);
  const int slotIndex = lane.size() - 1;
  const juce::String slot = lane[slotIndex];
  EXPECT_EQ(rig.block(slot)["kind"].toString(), juce::String("insert"));
  const juce::String placed = rig.add("cab-ir-test-2.wav", ",\"target\":" + quoted(slot));
  EXPECT_EQ(laneIds(rig.state()["chain"], "chain")[slotIndex], placed);
  rig.waitLoaded();

  const int tonesBefore = toneIds(rig.state()["chain"], "chain").size();
  expectError(rig.run("chain.addToneFromFiles", R"({"paths":[]})"), CommandError::invalidArgs);
  expectError(rig.run("chain.addToneFromFiles", R"({"paths":["relative/amp.nam"]})"),
              CommandError::invalidArgs);
  expectError(rig.run("chain.addToneFromFiles", R"({"paths":[42]})"), CommandError::invalidArgs);
  // Built from the temp dir so it is an absolute path on every platform
  // (a leading slash alone is not absolute on Windows).
  const juce::File missingModel = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                      .getChildFile("nonexistent-t3k")
                                      .getChildFile("missing.nam");
  expectError(rig.run("chain.addToneFromFiles",
                      "{\"paths\":[" + quoted(missingModel.getFullPathName()) + "]}"),
              CommandError::notFound);
  // An amp and a cab in one call would become one block of one kind.
  expectError(rig.run("chain.addToneFromFiles",
                      "{\"paths\":[" + pathOf("a2-amp-test.nam") + "," + pathOf("cab-ir-test.wav") +
                          "]}"),
              CommandError::invalidArgs);
  expectError(rig.run("chain.addToneFromFiles",
                      "{\"paths\":[" + pathOf("cab-ir-test.wav") + "],\"target\":\"no-such-block\"}"),
              CommandError::notFound);
  expectError(rig.run("chain.addToneFromFiles",
                      "{\"paths\":[" + pathOf("cab-ir-test.wav") + "],\"lane\":\"right\"}"),
              CommandError::failed);
  expectError(rig.run("chain.addToneFromFiles",
                      "{\"paths\":[" + pathOf("cab-ir-test.wav") + "],\"lane\":\"left\",\"target\":" +
                          quoted(packId) + "}"),
              CommandError::invalidArgs);

  // A file the engine does not load: its own message comes back as `failed`.
  const juce::File notes = rig.presetDir.getChildFile("notes.txt");
  ASSERT_TRUE(notes.replaceWithText("not a model"));
  const CommandResult refused = rig.run(
      "chain.addToneFromFiles", "{\"paths\":[" + quoted(notes.getFullPathName()) + "]}");
  expectError(refused, CommandError::failed);
  EXPECT_TRUE(refused.errorMessage.contains(".nam"));

  EXPECT_EQ(toneIds(rig.state()["chain"], "chain").size(), tonesBefore)
      << "a refused add changed the chain";
}

TEST(EngineCommandsTest, ParamSetClampsAndRejectsUnknownIds) {
  CommandRig rig;

  const juce::var listed = rig.must("param.list");
  for (const char* id : {"inputLevel", "gateThreshold", "osFactor", "toneBass"})
    EXPECT_TRUE(listed[id].isObject()) << "param.list lacks " << id;

  const juce::var gate = rig.must("param.get", R"({"id":"gateThreshold"})");
  EXPECT_EQ(gate["id"].toString(), juce::String("gateThreshold"));
  const double min = gate["min"], max = gate["max"];
  EXPECT_LT(min, max);
  for (const char* field : {"value", "normalized", "default", "name", "type"})
    EXPECT_FALSE(gate[field].isVoid()) << "parameter lacks " << field;

  const juce::var set = rig.must("param.set", R"({"id":"gateThreshold","value":-40})");
  EXPECT_NEAR(static_cast<double>(set["value"]), -40.0, 1e-4);
  EXPECT_FALSE(static_cast<bool>(set["clamped"]));
  EXPECT_NEAR(rig.proc.parameters.getRawParameterValue("gateThreshold")->load(), -40.0f, 1e-4f);
  EXPECT_NEAR(static_cast<double>(rig.state()["params"]["gateThreshold"]["value"]), -40.0, 1e-4);

  // Out of range lands on the nearest end and says so.
  const juce::var high = rig.must("param.set", R"({"id":"gateThreshold","value":1000})");
  // (A skewed range does not land on its end to the last bit.)
  EXPECT_NEAR(static_cast<double>(high["value"]), max, 1e-4);
  EXPECT_NEAR(static_cast<double>(high["normalized"]), 1.0, 1e-6);
  EXPECT_TRUE(static_cast<bool>(high["clamped"]));
  const juce::var low = rig.must("param.set", R"({"id":"gateThreshold","value":-1000})");
  EXPECT_NEAR(static_cast<double>(low["value"]), min, 1e-4);
  EXPECT_TRUE(static_cast<bool>(low["clamped"]));

  // A switch takes 0 / 1; a choice takes its index and lists its labels.
  const juce::var gateOff = rig.must("param.set", R"({"id":"gateEnabled","value":0})");
  EXPECT_EQ(gateOff["type"].toString(), juce::String("boolean"));
  EXPECT_NEAR(static_cast<double>(gateOff["value"]), 0.0, 1e-9);
  const juce::var factor = rig.must("param.set", R"({"id":"osFactor","value":1})");
  EXPECT_EQ(factor["type"].toString(), juce::String("choice"));
  EXPECT_NEAR(static_cast<double>(factor["value"]), 1.0, 1e-9);
  EXPECT_GE(factor["choices"].size(), 2);

  expectError(rig.run("param.set", R"({"id":"noSuchParam","value":1})"), CommandError::invalidArgs);
  expectError(rig.run("param.get", R"({"id":"noSuchParam"})"), CommandError::invalidArgs);
  expectError(rig.run("param.set", R"({"id":"gateThreshold","value":"loud"})"),
              CommandError::invalidArgs);
  expectError(rig.run("param.set", R"({"id":"gateThreshold"})"), CommandError::invalidArgs);
}

TEST(EngineCommandsTest, PresetSaveLoadRenameDeleteRoundTrip) {
  CommandRig rig;
  const juce::String cab = rig.add("cab-ir-test.wav");
  rig.waitLoaded();
  rig.must("block.setParam", blockArgs(cab, R"(,"param":"mix","value":0.6)"));
  rig.must("param.set", R"({"id":"gateThreshold","value":-33})");

  const juce::var saved = rig.must("preset.save", R"({"name":"Command Rig"})");
  const juce::String presetId = saved["id"].toString();
  ASSERT_TRUE(presetId.isNotEmpty());
  EXPECT_EQ(saved["name"].toString(), juce::String("Command Rig"));
  // Written to the temp store.
  EXPECT_EQ(rig.presetFileCount(), 1);

  EXPECT_EQ(presetNamed(rig.must("preset.list"), "Command Rig")["id"].toString(), presetId);
  EXPECT_EQ(rig.state()["chain"]["preset"]["id"].toString(), presetId);
  // state.get carries the same list, as a bare array.
  juce::DynamicObject::Ptr stateList = new juce::DynamicObject();
  stateList->setProperty("presets", rig.state()["presets"]);
  EXPECT_EQ(presetNamed(juce::var(stateList.get()), "Command Rig")["id"].toString(), presetId);

  // Wipe the rig, then get it back from the preset.
  EXPECT_TRUE(static_cast<bool>(rig.must("chain.reset")["changed"]));
  EXPECT_FALSE(static_cast<bool>(rig.must("chain.reset")["changed"]));
  EXPECT_EQ(toneIds(rig.state()["chain"], "chain").size(), 0);
  EXPECT_TRUE(rig.state()["chain"]["preset"].isVoid());

  const juce::var loaded = rig.must("preset.load", "{\"id\":" + quoted(presetId) + "}");
  EXPECT_EQ(loaded["preset"]["name"].toString(), juce::String("Command Rig"));
  rig.waitLoaded();
  const juce::StringArray tones = toneIds(rig.state()["chain"], "chain");
  ASSERT_EQ(tones.size(), 1);
  const juce::var restored = rig.block(tones[0]);
  EXPECT_TRUE(static_cast<bool>(restored["loaded"]));
  EXPECT_EQ(restored["tone"]["format"].toString(), juce::String("ir"));
  EXPECT_NEAR(static_cast<double>(restored["params"]["mix"]), 0.6, 1e-6);
  EXPECT_NEAR(static_cast<double>(rig.state()["params"]["gateThreshold"]["value"]), -33.0, 1e-4);

  const juce::var renamed = rig.must(
      "preset.rename", "{\"id\":" + quoted(presetId) + ",\"name\":\"Renamed Rig\"}");
  EXPECT_TRUE(presetNamed(renamed, "Command Rig").isVoid());
  const juce::String renamedId = presetNamed(renamed, "Renamed Rig")["id"].toString();
  ASSERT_TRUE(renamedId.isNotEmpty());

  // A second preset, so there is something to reorder.
  const juce::String otherId = rig.must("preset.save", R"({"name":"Another Rig"})")["id"].toString();
  const auto userOrder = [&] {
    juce::StringArray ids;
    const juce::var list = rig.must("preset.list")["presets"];
    for (const auto& preset : *list.getArray())
      if (!static_cast<bool>(preset["factory"]))
        ids.add(preset["id"].toString());
    return ids;
  };
  const juce::StringArray before = userOrder();
  ASSERT_EQ(before.size(), 2);
  const int delta = before[0] == otherId ? 1 : -1;
  rig.must("preset.move", "{\"id\":" + quoted(otherId) + ",\"delta\":" + juce::String(delta) + "}");
  const juce::StringArray moved = userOrder();
  EXPECT_EQ(moved[0], before[1]);
  EXPECT_EQ(moved[1], before[0]);
  // Already at that end of its section.
  expectError(rig.run("preset.move",
                      "{\"id\":" + quoted(otherId) + ",\"delta\":" + juce::String(delta) + "}"),
              CommandError::failed);

  const juce::var afterDelete = rig.must("preset.delete", "{\"id\":" + quoted(renamedId) + "}");
  EXPECT_TRUE(presetNamed(afterDelete, "Renamed Rig").isVoid());
  EXPECT_FALSE(presetNamed(afterDelete, "Another Rig").isVoid());
  rig.must("preset.delete", "{\"id\":" + quoted(otherId) + "}");
  EXPECT_EQ(rig.presetFileCount(), 0);

  for (const char* command : {"preset.load", "preset.delete"})
    expectError(rig.run(command, "{\"id\":" + quoted(renamedId) + "}"), CommandError::notFound);
  expectError(rig.run("preset.rename", R"({"id":"user:nope","name":"x"})"), CommandError::notFound);
  expectError(rig.run("preset.move", R"({"id":"user:nope","delta":1})"), CommandError::notFound);
  expectError(rig.run("preset.save", R"({"name":"   "})"), CommandError::invalidArgs);
}

TEST(EngineCommandsTest, UndoAndRedo) {
  CommandRig rig;
  expectError(rig.run("chain.undo"), CommandError::failed);
  expectError(rig.run("chain.redo"), CommandError::failed);

  const juce::String cab = rig.add("cab-ir-test.wav");
  rig.waitLoaded();
  rig.must("block.setEnabled", blockArgs(cab, R"(,"enabled":false)"));
  EXPECT_FALSE(static_cast<bool>(rig.block(cab)["params"]["enabled"]));

  juce::var undone = rig.must("chain.undo");
  EXPECT_TRUE(static_cast<bool>(undone["canUndo"]));
  EXPECT_TRUE(static_cast<bool>(undone["canRedo"]));
  EXPECT_TRUE(static_cast<bool>(rig.block(cab)["params"]["enabled"]));

  const juce::var redone = rig.must("chain.redo");
  EXPECT_FALSE(static_cast<bool>(redone["canRedo"]));
  EXPECT_FALSE(static_cast<bool>(rig.block(cab)["params"]["enabled"]));

  // Back past the add: the chain is empty again and there is nothing left.
  rig.must("chain.undo");
  undone = rig.must("chain.undo");
  EXPECT_FALSE(static_cast<bool>(undone["canUndo"]));
  EXPECT_EQ(toneIds(rig.state()["chain"], "chain").size(), 0);
  expectError(rig.run("chain.undo"), CommandError::failed);

  rig.must("chain.redo");
  rig.waitLoaded();
  EXPECT_EQ(toneIds(rig.state()["chain"], "chain").size(), 1);

  // Removing a block is one step back too.
  const juce::String restoredId = toneIds(rig.state()["chain"], "chain")[0];
  const juce::var removed = rig.must("chain.removeBlock", blockArgs(restoredId));
  EXPECT_EQ(removed["removedBlockId"].toString(), restoredId);
  EXPECT_FALSE(idsOf(removed["left"]).contains(restoredId));
  rig.must("chain.undo");
  rig.waitLoaded();
  EXPECT_TRUE(static_cast<bool>(rig.block(restoredId)["loaded"]));
}

TEST(EngineCommandsTest, StereoLanesBranchAndStructure) {
  CommandRig rig;
  const juce::String first = rig.add("cab-ir-test.wav");

  // Mono: the two-lane commands refuse.
  expectError(rig.run("chain.swapLanes"), CommandError::failed);
  expectError(rig.run("chain.setBranch",
                      "{\"side\":\"left\",\"afterBlockId\":" + quoted(first) + "}"),
              CommandError::failed);
  expectError(rig.run("chain.moveBlock", blockArgs(first, R"(,"side":"right","index":0)")),
              CommandError::failed);

  const juce::var stereo = rig.must("chain.setStereo", R"({"enabled":true})");
  EXPECT_TRUE(static_cast<bool>(stereo["stereoEnabled"]));
  EXPECT_TRUE(idsOf(stereo["left"]).contains(first));
  EXPECT_GT(idsOf(stereo["right"]).size(), 0);
  EXPECT_TRUE(static_cast<bool>(rig.state()["chain"]["stereoEnabled"]));

  const juce::String second = rig.add("cab-ir-test-2.wav", R"(,"lane":"right")");
  rig.waitLoaded();
  juce::var chain = rig.state()["chain"];
  EXPECT_EQ(toneIds(chain, "chain"), juce::StringArray(first));
  EXPECT_EQ(toneIds(chain, "chainRight"), juce::StringArray(second));

  // Branch: the right lane taps the left lane after `first`.
  const juce::var branched = rig.must(
      "chain.setBranch", "{\"side\":\"left\",\"afterBlockId\":" + quoted(first) + "}");
  EXPECT_EQ(branched["branch"]["side"].toString(), juce::String("left"));
  EXPECT_EQ(branched["branch"]["afterBlockId"].toString(), first);
  chain = rig.state()["chain"];
  EXPECT_EQ(chain["branch"]["afterBlockId"].toString(), first);
  // A branched chain has one source, so the engine moved off the stereo feed
  // and refuses to go back while branched.
  EXPECT_EQ(chain["inputMode"].toString(), juce::String("left"));
  expectError(rig.run("chain.setInputMode", R"({"mode":"stereo"})"), CommandError::failed);
  EXPECT_EQ(rig.must("chain.setInputMode", R"({"mode":"right"})")["inputMode"].toString(),
            juce::String("right"));
  // The tap must be a tone block of the lane named.
  expectError(rig.run("chain.setBranch",
                      "{\"side\":\"right\",\"afterBlockId\":" + quoted(first) + "}"),
              CommandError::failed);

  EXPECT_TRUE(static_cast<bool>(rig.must("chain.clearBranch")["changed"]));
  EXPECT_FALSE(static_cast<bool>(rig.must("chain.clearBranch")["changed"]));
  EXPECT_TRUE(rig.state()["chain"]["branch"].isVoid());
  EXPECT_EQ(rig.must("chain.setInputMode", R"({"mode":"stereo"})")["inputMode"].toString(),
            juce::String("stereo"));

  const juce::var swapped = rig.must("chain.swapLanes");
  EXPECT_TRUE(idsOf(swapped["left"]).contains(second));
  EXPECT_TRUE(idsOf(swapped["right"]).contains(first));

  // Duplicate lands right after its source by default, in the same lane.
  const juce::var duplicated = rig.must("chain.duplicateBlock", blockArgs(first));
  const juce::String clone = duplicated["blockId"].toString();
  ASSERT_TRUE(clone.isNotEmpty());
  EXPECT_NE(clone, first);
  EXPECT_EQ(duplicated["block"]["tone"]["format"].toString(), juce::String("ir"));
  rig.waitLoaded();
  EXPECT_EQ(toneIds(rig.state()["chain"], "chainRight"), juce::StringArray(first, clone));

  // Move the clone across; moving it "to" the lane it is in is refused.
  expectError(rig.run("chain.moveBlock", blockArgs(clone, R"(,"side":"right","index":0)")),
              CommandError::failed);
  const juce::var moved = rig.must("chain.moveBlock", blockArgs(clone, R"(,"side":"left","index":0)"));
  EXPECT_EQ(idsOf(moved["left"])[0], clone);
  EXPECT_EQ(toneIds(rig.state()["chain"], "chain"), juce::StringArray(clone, second));

  // Reorder the left lane: reverse the full id list, insert slots included.
  juce::StringArray order = idsOf(moved["left"]);
  std::reverse(order.begin(), order.end());
  juce::Array<juce::var> orderVar;
  for (const auto& id : order)
    orderVar.add(id);
  const juce::var reordered =
      rig.must("chain.reorder", "{\"order\":" + juce::JSON::toString(juce::var(orderVar), true) + "}");
  EXPECT_EQ(idsOf(reordered["left"]), order);
  EXPECT_EQ(toneIds(rig.state()["chain"], "chain"), juce::StringArray(second, clone));

  // Not a whole lane, and an id that does not exist.
  expectError(rig.run("chain.reorder", "{\"order\":[" + quoted(clone) + "]}"), CommandError::failed);
  expectError(rig.run("chain.reorder", R"({"order":["no-such-block"]})"), CommandError::notFound);

  const juce::var mono = rig.must("chain.setStereo", R"({"enabled":false})");
  EXPECT_FALSE(static_cast<bool>(mono["stereoEnabled"]));
  EXPECT_EQ(idsOf(mono["right"]).size(), 0);
  EXPECT_EQ(rig.must("chain.setInputMode", R"({"mode":"dual"})")["inputMode"].toString(),
            juce::String("dual"));
  rig.waitLoaded();
  expectError(rig.run("chain.setInputMode", R"({"mode":"sideways"})"), CommandError::invalidArgs);
}

TEST(EngineCommandsTest, UnknownBlockIdsAreNotFound) {
  CommandRig rig;
  rig.add("cab-ir-test.wav");
  rig.must("chain.setStereo", R"({"enabled":true})");
  rig.waitLoaded();
  const juce::var before = rig.state()["chain"];
  const juce::String ghost = "no-such-block";

  const std::vector<std::pair<const char*, juce::String>> calls = {
      {"block.setParam", blockArgs(ghost, R"(,"param":"mix","value":0.5)")},
      {"block.setEnabled", blockArgs(ghost, R"(,"enabled":true)")},
      {"block.setSlimSize", blockArgs(ghost, R"(,"slimSize":0.5)")},
      {"block.setEqBand", blockArgs(ghost, R"(,"band":1,"gainDb":3)")},
      {"block.setEqEnabled", blockArgs(ghost, R"(,"enabled":true)")},
      {"block.setEqPre", blockArgs(ghost, R"(,"pre":true)")},
      {"block.resetEq", blockArgs(ghost)},
      {"chain.removeBlock", blockArgs(ghost)},
      {"chain.duplicateBlock", blockArgs(ghost)},
      {"chain.moveBlock", blockArgs(ghost, R"(,"side":"right","index":0)")},
      {"chain.setBranch", R"({"side":"left","afterBlockId":"no-such-block"})"},
  };
  for (const auto& [command, args] : calls) {
    const CommandResult result = rig.run(command, args);
    EXPECT_FALSE(result.ok) << command;
    EXPECT_EQ(result.errorCode, juce::String(CommandError::notFound)) << command;
    EXPECT_TRUE(result.errorMessage.contains(ghost)) << command;
  }

  // An empty insert slot exists but is not a tone block: refused, not "not
  // found".
  juce::String slot;
  const juce::var leftLane = before["chain"];
  for (const auto& item : *leftLane.getArray())
    if (item["kind"].toString() == "insert")
      slot = item["blockId"].toString();
  ASSERT_TRUE(slot.isNotEmpty());
  expectError(rig.run("block.setEnabled", blockArgs(slot, R"(,"enabled":false)")),
              CommandError::failed);
  expectError(rig.run("chain.removeBlock", blockArgs(slot)), CommandError::failed);

  // Arguments outside a block command's spec never get as far as the lookup.
  expectError(rig.run("block.setParam", blockArgs(ghost, R"(,"param":"volume","value":0.5)")),
              CommandError::invalidArgs);
  expectError(rig.run("block.setParam", blockArgs(ghost, R"(,"param":"mix","value":1.5)")),
              CommandError::invalidArgs);
  expectError(rig.run("block.setEqBand", blockArgs(ghost, R"(,"band":6)")),
              CommandError::invalidArgs);

  // An IR block has no model size to set.
  const juce::String cab = toneIds(before, "chain")[0];
  expectError(rig.run("block.setSlimSize", blockArgs(cab, R"(,"slimSize":0.5)")),
              CommandError::failed);

  EXPECT_EQ(static_cast<int>(rig.state()["chain"]["revision"]), static_cast<int>(before["revision"]))
      << "a refused command changed the chain";
}

TEST(EngineCommandsTest, MidiMapCommands) {
  CommandRig rig;
  EXPECT_EQ(rig.must("midi.getMap")["mappings"].size(), 0);

  juce::var map = rig.must("midi.setCc", R"({"targetId":"gateThreshold","cc":20})");
  ASSERT_EQ(map["mappings"].size(), 1);
  EXPECT_EQ(map["mappings"][0]["targetId"].toString(), juce::String("gateThreshold"));
  EXPECT_EQ(map["mappings"][0]["source"].toString(), juce::String("cc"));
  EXPECT_EQ(static_cast<int>(map["mappings"][0]["number"]), 20);

  // Mapping the same target again replaces it; other targets add.
  rig.must("midi.setCc", R"({"targetId":"gateThreshold","cc":21})");
  map = rig.must("midi.setCc", R"({"targetId":"block1Power","cc":64})");
  ASSERT_EQ(map["mappings"].size(), 2);
  EXPECT_EQ(static_cast<int>(map["mappings"][0]["number"]), 21);
  EXPECT_EQ(rig.state()["midi"]["mappings"].size(), 2);

  EXPECT_EQ(static_cast<int>(rig.must("midi.setChannelFilter", R"({"channel":5})")["channel"]), 5);
  EXPECT_EQ(static_cast<int>(rig.must("midi.getMap")["channel"]), 5);

  map = rig.must("midi.remove", R"({"targetId":"gateThreshold"})");
  ASSERT_EQ(map["mappings"].size(), 1);
  EXPECT_EQ(map["mappings"][0]["targetId"].toString(), juce::String("block1Power"));
  expectError(rig.run("midi.remove", R"({"targetId":"gateThreshold"})"), CommandError::notFound);

  expectError(rig.run("midi.setCc", R"({"targetId":"noSuchTarget","cc":1})"),
              CommandError::invalidArgs);
  expectError(rig.run("midi.setCc", R"({"targetId":"gateThreshold","cc":128})"),
              CommandError::invalidArgs);
  expectError(rig.run("midi.setChannelFilter", R"({"channel":17})"), CommandError::invalidArgs);
}

TEST(EngineCommandsTest, LiveReadingsAndDescribe) {
  CommandRig rig;
  const juce::String cab = rig.add("cab-ir-test.wav");

  const juce::var meters = rig.must("meters.get");
  EXPECT_EQ(meters["input"].size(), 2);
  EXPECT_EQ(meters["output"].size(), 2);
  EXPECT_TRUE(meters["blocks"][juce::Identifier(cab)].isObject());
  EXPECT_TRUE(meters["blocks"][juce::Identifier(cab)]["readings"].isVoid())
      << "a kind without readings reports none";

  // A kind with readings reports them with its levels, by reading id.
  const juce::String compressor =
      rig.must("chain.addBlock", R"({"kind":"dynamics.compressor"})")["blockId"].toString();
  ASSERT_TRUE(compressor.isNotEmpty());
  const juce::var withReadings = rig.must("meters.get");
  const juce::var readings = withReadings["blocks"][juce::Identifier(compressor)]["readings"];
  ASSERT_TRUE(readings.isObject());
  EXPECT_DOUBLE_EQ(static_cast<double>(readings["gainReductionDb"]), 0.0);

  EXPECT_TRUE(static_cast<bool>(rig.must("tuner.setEnabled", R"({"enabled":true})")["enabled"]));
  const juce::var reading = rig.must("tuner.get");
  for (const char* field : {"frequency", "confidence", "level"})
    EXPECT_FALSE(reading[field].isVoid()) << "tuner reading lacks " << field;
  EXPECT_FALSE(static_cast<bool>(rig.must("tuner.setEnabled", R"({"enabled":false})")["enabled"]));

  // The command set describes itself through a command too.
  const juce::var all = rig.must("commands.describe");
  EXPECT_EQ(all.size(), rig.commands.registry().describe().size());
  EXPECT_GE(all.size(), 40);
  for (const auto& command : *all.getArray()) {
    EXPECT_TRUE(command["name"].toString().containsChar('.'));
    EXPECT_TRUE(command["summary"].toString().isNotEmpty());
    EXPECT_TRUE(command["mutates"].isBool());
  }
  const juce::var one = rig.must("commands.describe", R"({"name":"block.setParam"})");
  EXPECT_EQ(one["name"].toString(), juce::String("block.setParam"));
  EXPECT_TRUE(static_cast<bool>(one["mutates"]));
  EXPECT_EQ(one["params"].size(), 3);
  expectError(rig.run("commands.describe", R"({"name":"block.nope"})"), CommandError::notFound);

  // Reads are marked as such, so a read-only client can be told apart.
  for (const char* readOnly : {"state.get", "state.revision", "state.waitUntilLoaded",
                               "param.list", "param.get", "preset.list", "midi.getMap",
                               "meters.get", "tuner.get", "commands.describe"})
    EXPECT_FALSE(static_cast<bool>(rig.commands.registry().describe(readOnly)["mutates"]))
        << readOnly;
}

TEST(EngineCommandsTest, WaitUntilLoadedAndCallsFromAnotherThread) {
  CommandRig rig;
  // Nothing to wait for.
  EXPECT_TRUE(static_cast<bool>(rig.must("state.waitUntilLoaded", R"({"timeoutMs":0})")["loaded"]));

  // With no time allowed, a load still in flight is reported as unavailable
  // rather than waited for. (Whether this one is still in flight depends on
  // the machine, so only the shape of a refusal is pinned.)
  rig.add("a2-amp-test.nam");
  const CommandResult hurried = rig.run("state.waitUntilLoaded", R"({"timeoutMs":0})");
  if (!hurried.ok) {
    EXPECT_EQ(hurried.errorCode, juce::String(CommandError::unavailable));
    EXPECT_TRUE(hurried.errorMessage.contains("still loading"));
  }

  // The transport (stdio, a socket) is not on the message thread; the same
  // rig is reachable from there.
  std::atomic<bool> done{false};
  CommandResult fromThread;
  std::thread worker([&] {
    fromThread = rig.commands.registry().runFromAnyThread("state.revision", {}, 10000);
    done.store(true);
  });
  while (!done.load())
    juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
  worker.join();
  ASSERT_TRUE(fromThread.ok);
  EXPECT_GT(static_cast<int>(fromThread.value["revision"]), 0);

  rig.waitLoaded();
  const juce::var settled = rig.must("state.waitUntilLoaded");
  EXPECT_EQ(settled["failedBlockIds"].size(), 0);
}

// The reference in docs/ is generated from the registry; this fails when a
// command changed and the file was not regenerated:
//   build/tools/PedaleiraRender_artefacts/Release/PedaleiraRender --describe-markdown > docs/commands.md
TEST(EngineCommandsTest, CommandReferenceInDocsIsUpToDate) {
  const juce::File reference = juce::File(T3K_TEST_FILES_DIR)
                                   .getParentDirectory()
                                   .getParentDirectory()
                                   .getChildFile("docs")
                                   .getChildFile("commands.md");
  ASSERT_TRUE(reference.existsAsFile()) << "missing " << reference.getFullPathName();

  TONE3000Processor proc;
  const EngineCommands commands(proc);
  // A Windows checkout may have converted the line ends.
  const juce::String onDisk = reference.loadFileAsString().replace("\r\n", "\n");
  EXPECT_TRUE(onDisk == commands.markdownReference())
      << "docs/commands.md is out of date; regenerate it with\n"
         "  PedaleiraRender --describe-markdown > docs/commands.md";
}

// ---------------------------------------------------------------------------
// End to end: commands against direct calls
// ---------------------------------------------------------------------------

namespace {

// Drives a rig the way the golden render tests and PedaleiraRender do: prepare,
// build, settle, prepare again like a host starting transport, process.
std::vector<float> renderRig(const std::function<void(TONE3000Processor&)>& build,
                             const std::vector<float>& input) {
  constexpr int kBlock = 128;
  TONE3000Processor proc;
  proc.setMultiCoreEnabled(false, /*persist=*/false);
  proc.setPlayConfigDetails(1, 1, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);

  build(proc);
  EXPECT_TRUE(waitForChainLoaded(proc));
  proc.prepareToPlay(kFs, kBlock);
  EXPECT_TRUE(waitForChainLoaded(proc));

  std::vector<float> output(input.size(), 0.0f);
  juce::AudioBuffer<float> buffer(1, kBlock);
  juce::MidiBuffer midi;
  for (size_t start = 0; start + kBlock <= input.size(); start += kBlock) {
    buffer.copyFrom(0, 0, input.data() + start, kBlock);
    proc.processBlock(buffer, midi);
    std::copy(buffer.getReadPointer(0), buffer.getReadPointer(0) + kBlock,
              output.begin() + static_cast<std::ptrdiff_t>(start));
  }
  return output;
}

}  // namespace

TEST(EngineCommandsTest, CommandBuiltRigRendersBitExactWithDirectCalls) {
  // A decaying sine with some noise on top, so both the amp's nonlinearity
  // and the cab's response are exercised.
  std::vector<float> input = makeSine(48000, 196.0, 0.3f);
  const std::vector<float> noise = makeNoise(48000, 7, 0.05f);
  for (size_t i = 0; i < input.size(); ++i)
    input[i] = input[i] * std::exp(-static_cast<float>(i) / 30000.0f) + noise[i];

  // The existing path: the processor's own methods, files handed over the way
  // the UI's drop handler does (chain_test_helpers.h).
  const std::vector<float> direct = renderRig(
      [](TONE3000Processor& proc) {
        const std::string amp =
            proc.loadLocalTone("a2-amp-test", filesOf({testFileEntry("a2-amp-test.nam")}))["blockId"]
                .toString()
                .toStdString();
        const std::string cab =
            proc.loadLocalTone("cab-ir-test", filesOf({testFileEntry("cab-ir-test.wav")}))["blockId"]
                .toString()
                .toStdString();
        ASSERT_FALSE(amp.empty());
        ASSERT_FALSE(cab.empty());
        ASSERT_TRUE(waitForChainLoaded(proc));

        EXPECT_TRUE(proc.setBlockParam(amp, "inputGain", 0.62));
        EXPECT_TRUE(proc.setBlockParam(amp, "outputGain", 0.45));
        EXPECT_TRUE(proc.setBlockParam(cab, "mix", 0.8));
        EXPECT_TRUE(proc.setBlockEqBand(cab, 3, eqBandVar("bell", 2400.0, 4.0, 1.2)));
        EXPECT_TRUE(proc.setBlockEqBand(amp, 0, eqBandVar("lowcut", 110.0, 0.0, 0.71)));
        EXPECT_TRUE(proc.setBlockEqPre(amp, true));
        auto* gate = proc.parameters.getParameter("gateThreshold");
        gate->setValueNotifyingHost(gate->convertTo0to1(-70.0f));
        auto* bass = proc.parameters.getParameter("toneBass");
        bass->setValueNotifyingHost(bass->convertTo0to1(6.5f));
      },
      input);

  const std::vector<float> viaCommands = renderRig(
      [](TONE3000Processor& proc) {
        EngineCommands commands(proc);
        const auto run = [&](const char* name, const juce::String& args) {
          const CommandResult result = commands.registry().run(name, parseJson(args));
          EXPECT_TRUE(result.ok) << name << ": " << result.errorMessage.toStdString();
          return result.value;
        };
        const juce::String amp =
            run("chain.addToneFromFiles", "{\"paths\":[" + pathOf("a2-amp-test.nam") + "]}")
                ["blockId"].toString();
        const juce::String cab =
            run("chain.addToneFromFiles", "{\"paths\":[" + pathOf("cab-ir-test.wav") + "]}")
                ["blockId"].toString();
        EXPECT_TRUE(static_cast<bool>(run("state.waitUntilLoaded", "{}")["loaded"]));

        run("block.setParam", blockArgs(amp, R"(,"param":"inputGain","value":0.62)"));
        run("block.setParam", blockArgs(amp, R"(,"param":"outputGain","value":0.45)"));
        run("block.setParam", blockArgs(cab, R"(,"param":"mix","value":0.8)"));
        run("block.setEqBand", blockArgs(cab, R"(,"band":3,"freqHz":2400,"gainDb":4,"q":1.2)"));
        run("block.setEqBand", blockArgs(amp, R"(,"band":0,"type":"lowcut","freqHz":110)"));
        run("block.setEqPre", blockArgs(amp, R"(,"pre":true)"));
        run("param.set", R"({"id":"gateThreshold","value":-70})");
        run("param.set", R"({"id":"toneBass","value":6.5})");
      },
      input);

  ASSERT_EQ(direct.size(), viaCommands.size());

  // Guard the comparison itself: silence, or the dry input, would match
  // trivially without proving either rig ran.
  float peak = 0.0f, maxDryDiff = 0.0f;
  for (size_t i = 0; i < direct.size(); ++i) {
    peak = std::max(peak, std::abs(direct[i]));
    maxDryDiff = std::max(maxDryDiff, std::abs(direct[i] - input[i]));
  }
  ASSERT_GT(peak, 1e-3f) << "the direct render is silent";
  ASSERT_GT(maxDryDiff, 1e-3f) << "the direct render is the dry input";

  size_t mismatches = 0, firstMismatch = 0;
  for (size_t i = 0; i < direct.size(); ++i)
    if (direct[i] != viaCommands[i] && mismatches++ == 0)
      firstMismatch = i;
  EXPECT_EQ(mismatches, 0u) << "first differing sample at " << firstMismatch << ": "
                            << direct[firstMismatch] << " vs " << viaCommands[firstMismatch];
}

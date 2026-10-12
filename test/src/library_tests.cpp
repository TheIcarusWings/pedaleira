// The setlist library through the real processor and its commands
// (ProcessorLibrary.cpp, LibraryCommands.cpp, docs/library.md): real rigs
// through real presets. What has to hold:
//
//   - a .t3kpreset turned into a JSON preset and back is the same file, and
//     all three restore to byte-identical plugin state: legacy and grid rigs,
//     with NAM, IR, built-in effects, a hosted plugin, scenes and stomps,
//   - saving the same rig twice writes the same bytes,
//   - a preset whose asset is missing still loads: the block concerned comes
//     up failed (a model) or as a placeholder (a plugin's state) under its
//     name, and the rest plays,
//   - the preset list everything else uses (program changes, footswitch
//     banks, preset.*, state.get) is the current setlist by slot,
//   - favourites, recents, the position and the unsaved flag,
//   - the explicit import of the old .t3kpreset store,
//   - every library command, with its error cases,
//   - PedaleiraRender renders a JSON preset bit for bit like the .t3kpreset it
//     came from.
//
// The file layer and the format have their own suites
// (setlist_library_tests.cpp, preset_json_tests.cpp).
#include "EngineCommands.h"
#include "PresetFile.h"
#include "PresetJson.h"
#include "PresetManager.h"
#include "Processor.h"
#include "chain_test_helpers.h"
#include "hosted_plugin_test_helpers.h"

#include <gtest/gtest.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <cmath>
#include <functional>
#include <thread>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

namespace presetjson = t3k::presetjson;

constexpr double kRate = 48000.0;
constexpr int kBlock = 128;

juce::String quoted(const juce::String& text) { return juce::JSON::toString(juce::var(text)); }

struct TempDir {
  explicit TempDir(const char* what = "library")
      : dir(juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile(juce::String("t3k-") + what + "-tests-" + juce::Uuid().toString())) {
    dir.createDirectory();
  }
  ~TempDir() { dir.deleteRecursively(); }
  juce::File dir;
};

// The real processor with its stores in a temp folder (the library at
// <dir>/Library, the old .t3kpreset store at <dir>) and the commands over it.
struct LibraryRig {
  explicit LibraryRig(const juce::File& storeDir = {})
      : ownsStore(storeDir == juce::File()),
        store(ownsStore ? juce::File::getSpecialLocation(juce::File::tempDirectory)
                              .getChildFile("t3k-library-rig-" + juce::Uuid().toString())
                        : storeDir) {
    store.createDirectory();
    proc.setPresetStoreForTesting(store);
    proc.setMultiCoreEnabled(false, /*persist=*/false);
    proc.setFootswitchMode(FootswitchMode::Stomp, /*persist=*/false);
    proc.setFootswitchHybridRows(FootswitchRowRole::Scene, FootswitchRowRole::Stomp,
                                 /*persist=*/false);
    proc.setPlayConfigDetails(2, 2, kRate, kBlock);
    proc.prepareToPlay(kRate, kBlock);
  }
  ~LibraryRig() {
    if (ownsStore)
      store.deleteRecursively();
  }

  CommandResult run(const char* name, const juce::String& argsJson = "{}") {
    juce::var args;
    EXPECT_TRUE(juce::JSON::parse(argsJson, args).wasOk()) << argsJson.toStdString();
    return commands.registry().run(name, args);
  }

  juce::var must(const char* name, const juce::String& argsJson = "{}") {
    const CommandResult result = run(name, argsJson);
    EXPECT_TRUE(result.ok) << name << " " << argsJson.toStdString() << ": "
                           << result.errorCode.toStdString() << ": "
                           << result.errorMessage.toStdString();
    return result.value;
  }

  // Everything in flight has landed (models, kinds, the hosted plugin, which
  // is made on the message loop this pumps).
  void settle() {
    const juce::var result = must("state.waitUntilLoaded", R"({"timeoutMs":30000})");
    EXPECT_TRUE(static_cast<bool>(result["loaded"])) << juce::JSON::toString(result);
  }

  juce::String addTone(const char* fileName, const juce::String& extraArgs = {}) {
    const juce::String id =
        must("chain.addToneFromFiles",
             "{\"paths\":[" + quoted(testFile(fileName).getFullPathName()) + "]" + extraArgs + "}")
            ["blockId"].toString();
    EXPECT_TRUE(id.isNotEmpty());
    return id;
  }

  juce::String addKind(const char* kind, const juce::String& extraArgs = {}) {
    const juce::String id =
        must("chain.addBlock", "{\"kind\":" + quoted(kind) + extraArgs + "}")["blockId"].toString();
    EXPECT_TRUE(id.isNotEmpty());
    return id;
  }

  juce::String addPlugin(const juce::String& identifier, const juce::String& extraArgs = {}) {
    const juce::String id =
        must("chain.addPlugin", "{\"identifier\":" + quoted(identifier) + extraArgs + "}")
            ["blockId"].toString();
    EXPECT_TRUE(id.isNotEmpty());
    settle();
    return id;
  }

  void setParam(const juce::String& blockId, const juce::String& param, double value) {
    must("block.setParam", "{\"blockId\":" + quoted(blockId) + ",\"param\":" + quoted(param) +
                               ",\"value\":" + juce::String(value, 9) + "}");
  }

  // The id of a block's first kind parameter that is a plain number and can
  // be scene-assigned, with a value well inside its range.
  std::pair<juce::String, double> kindParam(const juce::String& blockId, double position = 0.3) {
    const juce::var listed = must("block.listParams", "{\"blockId\":" + quoted(blockId) + "}");
    if (const auto* params = listed["params"].getArray())
      for (const auto& param : *params)
        if (param["scope"].toString() == "kind" && param["type"].toString() == "float" &&
            !static_cast<bool>(param["affectsLatency"])) {
          const double low = param["min"], high = param["max"];
          return {param["id"].toString(), low + position * (high - low)};
        }
    ADD_FAILURE() << "no plain kind parameter on " << blockId;
    return {};
  }

  // A block's entry in the chain state, whichever lane or row it is in.
  juce::var block(const juce::String& blockId) {
    const juce::var state = proc.getChainState(-1);
    std::vector<juce::var> lists{state["chain"], state["chainRight"]};
    if (const auto* rows = state["grid"]["rows"].getArray())
      for (const auto& row : *rows)
        lists.push_back(row["blocks"]);
    for (const auto& list : lists)
      if (const auto* items = list.getArray())
        for (const auto& item : *items)
          if (item["blockId"].toString() == blockId)
            return item;
    return {};
  }

  juce::var library() { return must("library.get"); }
  bool unsaved() { return static_cast<bool>(library()["unsaved"]); }
  juce::String activePresetId() { return proc.getChainState(-1)["preset"]["id"].toString(); }
  juce::File libraryRoot() const { return store.getChildFile("Library"); }
  juce::File setlistFolder(const juce::String& name) const {
    return libraryRoot().getChildFile("Setlists").getChildFile(name);
  }

  float toneBass() { return proc.parameters.getRawParameterValue("toneBass")->load(); }
  void setToneBass(float value) {
    auto* p = proc.parameters.getParameter("toneBass");
    p->setValueNotifyingHost(p->convertTo0to1(value));
  }
  // Saves the rig as it is with toneBass at `bass`, so a later load is
  // visible on the faceplate. Returns the preset id.
  juce::String saveWithBass(const juce::String& name, float bass, int slot = 0,
                            const juce::String& setlistId = {}) {
    setToneBass(bass);
    juce::var saved;
    const juce::Result result = proc.savePresetToSlot(setlistId, slot, name, &saved);
    EXPECT_TRUE(result.wasOk()) << name << ": " << result.getErrorMessage();
    return saved["id"].toString();
  }

  const bool ownsStore;
  juce::File store;
  TONE3000Processor proc;
  EngineCommands commands{proc};
};

juce::MemoryBlock stateOf(TONE3000Processor& proc) {
  juce::MemoryBlock state;
  proc.getStateInformation(state);
  return state;
}

juce::MemoryBlock fileBytes(const juce::File& file) {
  juce::MemoryBlock bytes;
  EXPECT_TRUE(file.loadFileAsData(bytes)) << file.getFullPathName();
  return bytes;
}

void writeText(const juce::File& file, const juce::String& text) {
  file.deleteFile();
  ASSERT_TRUE(file.getParentDirectory().createDirectory());
  juce::FileOutputStream out(file);
  ASSERT_TRUE(out.openedOk()) << file.getFullPathName();
  out.write(text.toRawUTF8(), text.getNumBytesAsUTF8());
}

// The plugin state a fresh processor ends up with after loading `file`
// (a .t3kpreset or a JSON preset) the way the render tool does.
juce::MemoryBlock restoredState(const juce::File& file) {
  LibraryRig rig;
  EXPECT_TRUE(rig.proc.loadPresetFile(file)) << file.getFullPathName();
  rig.settle();
  // Nothing may be left failed or missing: a state compared equal because
  // both sides lost the same block would prove nothing.
  const juce::var state = rig.proc.getChainState(-1);
  std::vector<juce::var> lists{state["chain"], state["chainRight"]};
  if (const auto* rows = state["grid"]["rows"].getArray())
    for (const auto& row : *rows)
      lists.push_back(row["blocks"]);
  for (const auto& list : lists)
    if (const auto* items = list.getArray())
      for (const auto& item : *items)
        if (item["kind"].toString() == "tone") {
          EXPECT_TRUE(static_cast<bool>(item["loaded"])) << item["tone"]["title"].toString();
          EXPECT_FALSE(static_cast<bool>(item["loadFailed"]));
          EXPECT_FALSE(static_cast<bool>(item["missing"]));
        }
  return stateOf(rig.proc);
}

// Converts a .t3kpreset to a JSON preset (assets beside it) and that back to
// a .t3kpreset, and checks the whole chain of claims on the way.
void expectLosslessConversion(const juce::File& original, const juce::File& scratch,
                              bool compareRestoredState, juce::String* jsonOut = nullptr) {
  SCOPED_TRACE(original.getFileName().toStdString());
  const juce::ValueTree tree = t3k::presetfile::read(original);
  ASSERT_TRUE(tree.isValid());

  // To JSON.
  const juce::File jsonDir = scratch.getChildFile("json");
  const juce::File jsonFile = jsonDir.getChildFile(original.getFileNameWithoutExtension() + ".json");
  const presetjson::AssetStore store(jsonDir.getChildFile("Assets"));
  presetjson::WriteOptions writeOptions;
  writeOptions.assets = &store;
  juce::String error;
  const juce::String text = presetjson::toJson(tree, writeOptions, error);
  ASSERT_TRUE(text.isNotEmpty()) << error;
  writeText(jsonFile, text);
  if (jsonOut != nullptr)
    *jsonOut = text;
  // Text a person can read: no megabytes, every model in the store.
  EXPECT_LT(text.length(), 200000);
  EXPECT_FALSE(presetjson::referencedAssets(juce::JSON::parse(text)).empty());
  // Written again, the same bytes.
  EXPECT_EQ(presetjson::toJson(tree, writeOptions, error), text);

  // And back.
  presetjson::ReadOptions readOptions;
  readOptions.assets.push_back(&store);
  const presetjson::ReadResult read = presetjson::fromJson(jsonFile.loadFileAsString(), readOptions);
  ASSERT_TRUE(read.preset.isValid()) << read.error;
  EXPECT_TRUE(read.missing.empty());
  const juce::File backDir = scratch.getChildFile("back");
  backDir.createDirectory();
  const juce::File back = backDir.getChildFile(original.getFileName());
  ASSERT_TRUE(t3k::presetfile::write(back, read.preset));
  // The same .t3kpreset: every property, of the same kind, in the same
  // order, every model byte. (Against the original rewritten in today's
  // framing: a shipped file may still be in the older one.)
  const juce::File rewritten = scratch.getChildFile("rewritten").getChildFile(original.getFileName());
  rewritten.getParentDirectory().createDirectory();
  ASSERT_TRUE(t3k::presetfile::write(rewritten, tree));
  EXPECT_TRUE(fileBytes(back) == fileBytes(rewritten));
  // And the JSON of what came back is the JSON it came from.
  EXPECT_EQ(presetjson::toJson(read.preset, writeOptions, error), text);

  if (!compareRestoredState)
    return;
  // All three restore to the same plugin state, byte for byte.
  const juce::MemoryBlock fromOriginal = restoredState(original);
  ASSERT_GT(fromOriginal.getSize(), 1000u);
  EXPECT_TRUE(restoredState(jsonFile) == fromOriginal) << "the JSON preset restores differently";
  EXPECT_TRUE(restoredState(back) == fromOriginal) << "the converted-back preset restores differently";
}

juce::File factoryPresetsDir() {
  return juce::File(T3K_TEST_FILES_DIR)
      .getParentDirectory()
      .getParentDirectory()
      .getChildFile("resources")
      .getChildFile("factory-presets");
}

// Scenes and stomps on a rig with a delay, a reverb and (optionally) a
// plugin: names, per-scene block states, an assigned parameter with a value
// of its own in two scenes, an expression flag, two stomps.
void dressWithScenesAndStomps(LibraryRig& rig, const juce::String& delay, const juce::String& reverb,
                              const juce::String& plugin) {
  const auto [delayParam, delayValue] = rig.kindParam(delay, 0.3);
  rig.setParam(delay, delayParam, delayValue);
  rig.must("scene.rename", R"({"scene":"B","name":"Chorus"})");
  rig.must("scene.rename", R"({"scene":"D","name":"Solo é"})");
  rig.must("scene.assignParam",
           "{\"blockId\":" + quoted(delay) + ",\"param\":" + quoted(delayParam) + "}");
  rig.must("scene.assignParam", "{\"blockId\":" + quoted(reverb) + ",\"param\":\"mix\"}");
  const auto [otherParam, otherValue] = rig.kindParam(delay, 0.7);
  rig.must("scene.setParamValue",
           "{\"scene\":\"D\",\"blockId\":" + quoted(delay) + ",\"param\":" + quoted(otherParam) +
               ",\"value\":" + juce::String(otherValue, 9) + "}");
  rig.must("scene.select", R"({"scene":"C"})");
  rig.must("block.setParam", "{\"blockId\":" + quoted(reverb) + ",\"param\":\"enabled\",\"value\":0}");
  rig.setParam(reverb, "mix", 0.35);
  rig.must("expression.assignParam",
           "{\"blockId\":" + quoted(delay) + ",\"param\":\"outputGain\"}");
  rig.must("stomp.assign", "{\"switch\":\"A\",\"blockIds\":[" + quoted(reverb) +
                               "],\"label\":\"Verb\",\"momentary\":true}");
  juce::String both = quoted(delay);
  if (plugin.isNotEmpty())
    both << "," << quoted(plugin);
  rig.must("stomp.assign", "{\"switch\":\"F\",\"blockIds\":[" + both + "]}");
}

void setFaceplate(LibraryRig& rig) {
  rig.must("param.set", R"({"id":"toneBass","value":7.25})");
  rig.must("param.set", R"({"id":"gateThreshold","value":-47.5})");
  rig.must("param.set", R"({"id":"outputLevel","value":-3.3})");
  rig.must("param.set", R"({"id":"pitchSemitones","value":-2})");
}

// A legacy rig with one of everything: two lanes and a branch, NAM and IR,
// two built-in effects, the hosted test plugin, an EQ, scenes and stomps.
void buildLegacyRig(LibraryRig& rig, const juce::String& pluginIdentifier) {
  const juce::String amp = rig.addTone("a2-amp-test.nam");
  const juce::String cab = rig.addTone("cab-ir-test.wav");
  rig.settle();
  rig.must("chain.setStereo", R"({"enabled":true})");
  const juce::String delay = rig.addKind("delay.digital", ",\"lane\":\"right\"");
  const juce::String reverb = rig.addKind("reverb.plate", ",\"lane\":\"right\"");
  const juce::String plugin = rig.addPlugin(pluginIdentifier, ",\"lane\":\"right\"");
  rig.settle();
  rig.must("chain.setBranch", "{\"side\":\"left\",\"afterBlockId\":" + quoted(amp) + "}");

  rig.setParam(amp, "inputGain", 0.62);
  rig.setParam(amp, "outputGain", 0.41);
  rig.setParam(cab, "mix", 0.9);
  rig.must("block.setEqBand", "{\"blockId\":" + quoted(amp) +
                                  ",\"band\":2,\"freqHz\":812.5,\"gainDb\":-3.7,\"q\":1.3}");
  rig.must("block.setEqBand", "{\"blockId\":" + quoted(cab) + ",\"band\":0,\"type\":\"lowcut\",\"freqHz\":95}");
  // The plugin's own state: its gain, which lives in its chunk.
  const juce::var listed = rig.must("block.listParams", "{\"blockId\":" + quoted(plugin) + "}");
  juce::String gainId;
  for (const auto& param : *listed["params"].getArray())
    if (param["name"].toString() == "Gain")
      gainId = param["id"].toString();
  ASSERT_TRUE(gainId.isNotEmpty());
  rig.setParam(plugin, gainId, 0.5);

  dressWithScenesAndStomps(rig, delay, reverb, plugin);
  setFaceplate(rig);
  rig.settle();
}

// A grid rig with one of everything: a split with a rejoin, a row feeding
// another row, per-row I/O and mix, NAM and IR, effects, the hosted plugin,
// scenes and stomps.
void buildGridRig(LibraryRig& rig, const juce::String& pluginIdentifier) {
  const juce::String amp = rig.addTone("a2-amp-test.nam");
  rig.settle();
  rig.must("grid.convert", R"({"to":"grid"})");

  // Row 0 splits into row 1 around slots 1 to 3 and rejoins.
  rig.must("row.setInput", R"({"row":1,"source":"none"})");
  rig.must("split.add", R"({"row":0,"splitAt":1,"mixAt":4,"type":"crossover"})");
  rig.must("split.setParam", R"({"row":0,"param":"split.crossoverHz","value":640})");
  const juce::String delay = rig.addKind("delay.digital", ",\"row\":0,\"slot\":2");
  const juce::String cab = rig.addTone("cab-ir-test.wav", ",\"row\":1,\"slot\":1");
  const juce::String reverb = rig.addKind("reverb.plate", ",\"row\":0,\"slot\":5");

  // Row 2 takes the second input and feeds row 3, which goes to the outputs.
  rig.must("row.setInput", R"({"row":2,"source":"device","channel":2})");
  rig.must("row.setInput", R"({"row":3,"source":"none"})");
  rig.addKind("modulation.chorus", ",\"row\":2,\"slot\":0");
  const juce::String plugin = rig.addPlugin(pluginIdentifier, ",\"row\":2,\"slot\":3");
  rig.addKind("dynamics.compressor", ",\"row\":3,\"slot\":1");
  rig.must("row.setOutput", R"({"row":2,"target":"row","toRow":3})");
  rig.must("row.setOutput", R"({"row":3,"target":"device","pair":1})");
  rig.must("row.setMix", R"({"row":0,"volumeDb":-2.5,"pan":0.3})");
  rig.must("row.setMix", R"({"row":3,"volumeDb":-6,"invert":true,"solo":true})");
  rig.must("row.setMix", R"({"row":3,"solo":false})");
  rig.must("row.setInputStage", R"({"row":2,"trimDb":4.5,"gate":false})");
  rig.settle();

  rig.setParam(amp, "inputGain", 0.58);
  rig.must("block.setEqBand", "{\"blockId\":" + quoted(cab) + ",\"band\":3,\"gainDb\":2.25}");
  dressWithScenesAndStomps(rig, delay, reverb, plugin);
  setFaceplate(rig);
  rig.settle();
  ASSERT_EQ(rig.proc.getGridState()["mode"].toString(), juce::String("grid"));
  ASSERT_EQ(rig.proc.getGridState()["splits"].size(), 1);
  ASSERT_EQ(rig.proc.getGridState()["feeds"].size(), 1);
}

// A small rig for the tests that are about the library, not the format.
void buildSmallRig(LibraryRig& rig) {
  rig.addTone("cab-ir-test.wav");
  rig.addKind("delay.simple");
  rig.settle();
}

void pumpMessages(int ms = 20) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }

juce::String errorCodeOf(const CommandResult& result) {
  return result.ok ? juce::String("(ok)") : result.errorCode;
}

#define EXPECT_COMMAND_ERROR(result, code)                                              \
  do {                                                                                  \
    const CommandResult commandResult_ = (result);                                      \
    EXPECT_EQ(errorCodeOf(commandResult_), juce::String(code)) << commandResult_.errorMessage; \
  } while (false)

// "1:Name 4:Other" from a setlist.get result.
juce::String slotsOf(const juce::var& setlist) {
  juce::StringArray parts;
  if (const auto* slots = setlist["slots"].getArray())
    for (const auto& slot : *slots)
      parts.add(slot["slot"].toString() + ":" + slot["name"].toString());
  return parts.joinIntoString(" ");
}

juce::var slotAt(const juce::var& setlist, int number) {
  if (const auto* slots = setlist["slots"].getArray())
    for (const auto& slot : *slots)
      if (static_cast<int>(slot["slot"]) == number)
        return slot;
  return {};
}

juce::String setlistIdNamed(LibraryRig& rig, const juce::String& name) {
  const juce::var listed = rig.must("setlist.list");
  if (const auto* setlists = listed["setlists"].getArray())
    for (const auto& setlist : *setlists)
      if (setlist["name"].toString() == name)
        return setlist["id"].toString();
  return {};
}

juce::var footswitch(LibraryRig& rig, int index) {
  return rig.must("footswitch.getState")["switches"][index];
}

}  // namespace

// ---------------------------------------------------------------------------
// .t3kpreset <-> JSON, losslessly
// ---------------------------------------------------------------------------

TEST(LibraryRoundTripTest, FactoryPresetsConvertToJsonAndBack) {
  const juce::File dir = factoryPresetsDir();
  if (!dir.isDirectory())
    GTEST_SKIP() << "no resources/factory-presets directory in this checkout";
  auto files = dir.findChildFiles(juce::File::findFiles, false, "*.t3kpreset");
  files.sort();
  ASSERT_GT(files.size(), 0);
  // Real files made by upstream's builds: NAM captures and IRs, mono and
  // stereo, with Spread and the pitch shifter on the faceplate. Every one is
  // converted; the restored state is compared for a mono and a stereo one
  // (each comparison loads the rig three times).
  for (const auto& file : files) {
    TempDir scratch("factory-json");
    const bool compareState = file.getFileName().startsWith("Mesa Pitch Shift") ||
                              file.getFileName().startsWith("Dumble Spread");
    expectLosslessConversion(file, scratch.dir, compareState);
  }
}

TEST(LibraryRoundTripTest, ALegacyRigWithEverythingIsLossless) {
  ScannerSandbox sandbox;
  const juce::String identifier = scanTestPlugin();
  TempDir scratch("legacy-json");
  const juce::File original = scratch.dir.getChildFile("Everything.t3kpreset");
  {
    LibraryRig rig;
    buildLegacyRig(rig, identifier);
    const juce::Result exported = rig.proc.exportPresetJson({}, original, false, "Everything");
    ASSERT_TRUE(exported.wasOk()) << exported.getErrorMessage();
  }
  ASSERT_TRUE(original.existsAsFile());

  juce::String json;
  expectLosslessConversion(original, scratch.dir, true, &json);

  // The text shows the rig: both lanes and the branch, each block with its
  // settings, EQ and kind, the scenes, the stomps, the faceplate.
  const juce::var parsed = juce::JSON::parse(json);
  EXPECT_EQ(parsed["name"].toString(), juce::String("Everything"));
  EXPECT_EQ(static_cast<int>(parsed["schemaVersion"]), 1);
  EXPECT_TRUE(static_cast<bool>(parsed["chain"]["stereoEnabled"]));
  EXPECT_EQ(parsed["chain"]["branchSide"].toString(), juce::String("left"));
  EXPECT_TRUE(parsed["chain"]["branchAfterBlockId"].toString().isNotEmpty());
  EXPECT_TRUE(parsed["chain"]["layout"].isVoid()) << "a legacy rig has no layout property";
  juce::StringArray nodes;
  for (const auto& child : *parsed["chain"]["children"].getArray())
    nodes.add(child["node"].toString());
  EXPECT_EQ(nodes, juce::StringArray({"ChainBlocks", "RightChainBlocks", "Scenes", "Stomps"}));
  for (const char* expected : {"\"kind\": \"delay.digital\"", "\"kind\": \"reverb.plate\"",
                               "\"kind\": \"plugin\"", "\"type\": \"nam\"", "\"type\": \"ir\"",
                               "\"node\": \"Eq\"", "\"node\": \"Band\"", "\"name\": \"Chorus\"",
                               "\"label\": \"Verb\"", "\"momentary\": true",
                               "\"node\": \"Expression\"", "\"toneBass\": 7.25",
                               "\"gateThreshold\": -47.5", "\"pitchSemitones\": -2.0",
                               "\"freqHz\": 812.5", "\"active\": 2"})
    EXPECT_TRUE(json.contains(expected)) << expected;
  // Two models in the store (the capture and the IR), each named.
  const auto assets = presetjson::referencedAssets(parsed);
  std::set<juce::String> extensions;
  for (const auto& asset : assets) {
    extensions.insert(asset.ext);
    EXPECT_TRUE(asset.name.isNotEmpty());
    EXPECT_GT(asset.size, 0);
  }
  EXPECT_TRUE(extensions.count("nam") == 1 && extensions.count("wav") == 1);
  // The plugin's state is in the preset one way or the other.
  EXPECT_TRUE(json.contains("\"chunk\": { \"$base64\": ") || json.contains("\"chunk\": { \"$asset\": "))
      << "the hosted plugin's chunk is missing";
  EXPECT_FALSE(json.contains("\"data\": { \"$base64\"")) << "model bytes never go inline";
}

TEST(LibraryRoundTripTest, AGridRigWithEverythingIsLossless) {
  ScannerSandbox sandbox;
  const juce::String identifier = scanTestPlugin();
  TempDir scratch("grid-json");
  const juce::File original = scratch.dir.getChildFile("Grid Everything.t3kpreset");
  {
    LibraryRig rig;
    buildGridRig(rig, identifier);
    const juce::Result exported = rig.proc.exportPresetJson({}, original, false, "Grid Everything");
    ASSERT_TRUE(exported.wasOk()) << exported.getErrorMessage();
  }

  juce::String json;
  expectLosslessConversion(original, scratch.dir, true, &json);

  const juce::var parsed = juce::JSON::parse(json);
  EXPECT_EQ(static_cast<int>(parsed["schemaVersion"]), 2);
  EXPECT_EQ(parsed["chain"]["layout"].toString(), juce::String("grid"));
  // Rows with their I/O and slots, the split, and the feed as a row output.
  const juce::var grid = parsed["chain"]["children"][0];
  ASSERT_EQ(grid["node"].toString(), juce::String("Grid"));
  int rows = 0, splits = 0;
  for (const auto& child : *grid["children"].getArray()) {
    rows += child["node"].toString() == "Row" ? 1 : 0;
    splits += child["node"].toString() == "Split" ? 1 : 0;
  }
  EXPECT_EQ(rows, 4);
  EXPECT_EQ(splits, 1);
  for (const char* expected : {"\"node\": \"Input\"", "\"node\": \"Output\"", "\"node\": \"Slots\"",
                               "\"toRow\": 3", "\"type\": \"row\"", "\"trimDb\": 4.5",
                               "\"volumeDb\": -2.5", "\"type\": \"crossover\"", "\"splitCol\": 1",
                               "\"mixCol\": 4", "\"kind\": \"modulation.chorus\"",
                               "\"kind\": \"dynamics.compressor\"", "\"kind\": \"plugin\"",
                               "\"node\": \"Scenes\"", "\"node\": \"Stomps\""})
    EXPECT_TRUE(json.contains(expected)) << expected;
  // Solo is monitoring, not tone: never in a preset.
  EXPECT_FALSE(json.contains("\"solo\": true"));
}

TEST(LibraryRoundTripTest, SavingTwiceWritesTheSameBytes) {
  ScannerSandbox sandbox;
  const juce::String identifier = scanTestPlugin();
  LibraryRig rig;
  buildLegacyRig(rig, identifier);

  const juce::String id = rig.must("preset.save", R"({"name":"Stable"})")["id"].toString();
  const juce::File file = rig.setlistFolder("My Presets").getChildFile("001 Stable.json");
  ASSERT_TRUE(file.existsAsFile());
  const juce::String first = file.loadFileAsString();
  const int assetsAfterFirst =
      rig.libraryRoot().getChildFile("Assets").getNumberOfChildFiles(juce::File::findFiles);
  EXPECT_GE(assetsAfterFirst, 2);

  // Saved again unchanged: not one byte differs, and no asset is added.
  EXPECT_EQ(rig.must("preset.save", R"({"name":"Stable"})")["id"].toString(), id);
  EXPECT_EQ(file.loadFileAsString(), first);
  EXPECT_EQ(rig.libraryRoot().getChildFile("Assets").getNumberOfChildFiles(juce::File::findFiles),
            assetsAfterFirst);

  // Loaded and saved again (models reloaded from the store, the plugin made
  // anew from its chunk): still the same file. This is what keeps a git
  // history quiet: opening a preset and saving it is not a change.
  rig.must("chain.reset");
  rig.must("preset.load", "{\"id\":" + quoted(id) + "}");
  rig.settle();
  rig.must("preset.save", R"({"name":"Stable"})");
  EXPECT_EQ(file.loadFileAsString(), first);

  // A second preset with the same models shares their files.
  rig.must("preset.save", R"({"name":"Stable Too"})");
  EXPECT_EQ(rig.libraryRoot().getChildFile("Assets").getNumberOfChildFiles(juce::File::findFiles),
            assetsAfterFirst);

  // One knob moved is a diff of a line or two.
  const juce::String amp = rig.proc.getChainState(-1)["chain"][0]["blockId"].toString();
  rig.setParam(amp, "inputGain", 0.77);
  rig.must("preset.save", R"({"name":"Stable"})");
  const juce::StringArray before = juce::StringArray::fromLines(first);
  const juce::StringArray after = juce::StringArray::fromLines(file.loadFileAsString());
  ASSERT_EQ(before.size(), after.size());
  int changed = 0;
  for (int i = 0; i < before.size(); ++i)
    changed += before[i] != after[i] ? 1 : 0;
  EXPECT_GE(changed, 1);
  EXPECT_LE(changed, 2);
}

// ---------------------------------------------------------------------------
// Missing assets
// ---------------------------------------------------------------------------

TEST(LibraryMissingAssetTest, ABlockWhoseModelIsMissingFailsAloneAndTheRestPlays) {
  // The local-model stash stands in for "this machine has seen the file":
  // a scratch one here, emptied below, so the library's Assets folder is the
  // only place the capture could come from.
  TempDir stash("stash");
  const juce::File previousStash = TONE3000Processor::setLocalModelsDirForTesting(stash.dir);
  {
    LibraryRig rig;
    const juce::String amp = rig.addTone("a2-amp-test.nam");
    const juce::String cab = rig.addTone("cab-ir-test.wav");
    const juce::String delay = rig.addKind("delay.simple");
    rig.settle();
    const juce::String ampTitle = rig.block(amp)["tone"]["title"].toString();
    ASSERT_TRUE(ampTitle.isNotEmpty());
    const juce::String id = rig.must("preset.save", R"({"name":"Needs Assets"})")["id"].toString();
    const juce::File file = rig.setlistFolder("My Presets").getChildFile("001 Needs Assets.json");
    const juce::String savedText = file.loadFileAsString();

    // The capture's asset goes missing (the Assets folder was not copied).
    juce::File namAsset;
    for (const auto& asset : presetjson::referencedAssets(juce::JSON::parse(savedText)))
      if (asset.ext == "nam")
        namAsset = rig.proc.getLibraryRoot().getChildFile("Assets").getChildFile(
            asset.hash + "." + asset.ext);
    ASSERT_TRUE(namAsset.existsAsFile());
    const juce::File aside = namAsset.getSiblingFile("aside.keep");
    ASSERT_TRUE(namAsset.moveFileTo(aside));
    for (const auto& stashed : stash.dir.findChildFiles(juce::File::findFiles, true))
      stashed.deleteFile();

    rig.must("chain.reset");
    // The preset loads all the same.
    rig.must("preset.load", "{\"id\":" + quoted(id) + "}");
    const juce::var waited = rig.must("state.waitUntilLoaded", R"({"timeoutMs":30000})");
    // The amp: failed, under its name, in its place.
    const juce::var failed = rig.block(amp);
    ASSERT_FALSE(failed.isVoid()) << "the block must still be in the chain";
    EXPECT_TRUE(static_cast<bool>(failed["loadFailed"])) << juce::JSON::toString(failed);
    EXPECT_FALSE(static_cast<bool>(failed["loaded"]));
    EXPECT_EQ(failed["tone"]["title"].toString(), ampTitle);
    // The rest: loaded.
    EXPECT_TRUE(static_cast<bool>(rig.block(cab)["loaded"]));
    EXPECT_TRUE(static_cast<bool>(rig.block(delay)["loaded"]));
    EXPECT_EQ(rig.activePresetId(), id);

    // And it plays: the signal goes through the cab and the delay.
    rig.proc.prepareToPlay(kRate, kBlock);
    for (int i = 0; i < 200 && rig.proc.isChainEditFadeHeld(); ++i)
      pumpMessages(10);
    const std::vector<float> in = makeSine(static_cast<int>(kRate / 2), 220.0, 0.25f);
    const auto out = processStereo(rig.proc, in, kBlock);
    float peak = 0.0f;
    for (size_t i = in.size() / 2; i < in.size(); ++i)
      peak = std::max(peak, std::abs(out.first[i]));
    EXPECT_GT(peak, 1.0e-3f) << "a rig with a failed block went silent";

    // A hash that is there but wrong is the same as missing.
    ASSERT_TRUE(namAsset.replaceWithText("not the capture"));
    rig.must("chain.reset");
    rig.must("preset.load", "{\"id\":" + quoted(id) + "}");
    rig.must("state.waitUntilLoaded", R"({"timeoutMs":30000})");
    EXPECT_TRUE(static_cast<bool>(rig.block(amp)["loadFailed"]));
    EXPECT_TRUE(static_cast<bool>(rig.block(cab)["loaded"]));
    ASSERT_TRUE(namAsset.deleteFile());

    // The file comes back (the Assets folder is pulled): the preset is whole.
    ASSERT_TRUE(aside.moveFileTo(namAsset));
    rig.must("chain.reset");
    rig.must("preset.load", "{\"id\":" + quoted(id) + "}");
    rig.settle();
    EXPECT_TRUE(static_cast<bool>(rig.block(amp)["loaded"]));
    EXPECT_FALSE(static_cast<bool>(rig.block(amp)["loadFailed"]));
    // Untouched by all of it.
    EXPECT_EQ(file.loadFileAsString(), savedText);
    (void)waited;
  }
  TONE3000Processor::setLocalModelsDirForTesting(previousStash);
}

TEST(LibraryMissingAssetTest, APluginWhoseStateIsMissingIsAPlaceholderThatKeepsItsReference) {
  ScannerSandbox sandbox;
  const juce::String identifier = scanTestPlugin();
  LibraryRig rig;
  const juce::String cab = rig.addTone("cab-ir-test.wav");
  const juce::String plugin = rig.addPlugin(identifier);
  rig.settle();
  const juce::String id = rig.must("preset.save", R"({"name":"Hosted"})")["id"].toString();
  const juce::File file = rig.setlistFolder("My Presets").getChildFile("001 Hosted.json");
  juce::String text = file.loadFileAsString();

  // Give the plugin's state the shape a large one has: an asset reference,
  // to a file that is not there.
  const int start = text.indexOf("\"chunk\": { \"$base64\": ");
  if (start < 0) {
    ASSERT_TRUE(text.contains("\"chunk\": { \"$asset\": ")) << text;
    for (const auto& asset : presetjson::referencedAssets(juce::JSON::parse(text)))
      if (asset.ext == "bin")
        ASSERT_TRUE(rig.libraryRoot().getChildFile("Assets").getChildFile(asset.hash + ".bin").deleteFile());
  } else {
    const int end = text.indexOf(start, " }") + 2;
    const juce::String reference =
        "\"chunk\": { \"$asset\": \"" + juce::String::repeatedString("ab", 32) +
        "\", \"ext\": \"bin\", \"name\": \"PedaleiraTestFx chunk\", \"size\": 4096 }";
    text = text.substring(0, start) + reference + text.substring(end);
    writeText(file, text);
  }

  rig.must("chain.reset");
  rig.must("preset.load", "{\"id\":" + quoted(id) + "}");
  rig.must("state.waitUntilLoaded", R"({"timeoutMs":30000})");
  // Not a plugin running on default settings pretending to be the saved
  // one: a bypassed placeholder under the plugin's name.
  const juce::var placeholder = rig.block(plugin);
  ASSERT_FALSE(placeholder.isVoid());
  EXPECT_TRUE(static_cast<bool>(placeholder["missing"])) << juce::JSON::toString(placeholder);
  EXPECT_FALSE(static_cast<bool>(placeholder["loaded"]));
  EXPECT_EQ(placeholder["tone"]["title"].toString(), juce::String("PedaleiraTestFx"));
  EXPECT_TRUE(static_cast<bool>(rig.block(cab)["loaded"]));
  // It stays one: the plugin being installed is not what it is waiting for.
  pumpMessages(1500);
  EXPECT_TRUE(static_cast<bool>(rig.block(plugin)["missing"]));
  EXPECT_FALSE(rig.proc.isChainEditFadeHeld());

  // Saved again, the reference is still in the file, so the preset is whole
  // the day the asset turns up.
  rig.must("preset.save", R"({"name":"Hosted"})");
  const juce::String resaved = file.loadFileAsString();
  EXPECT_TRUE(resaved.contains("\"chunk\": { \"$asset\": ")) << resaved;
  EXPECT_FALSE(resaved.contains("pedaleira-missing-asset"));
  if (start >= 0)
    EXPECT_EQ(resaved, text);
}

// ---------------------------------------------------------------------------
// The preset list is the current setlist
// ---------------------------------------------------------------------------

TEST(LibraryIntegrationTest, AFreshLibraryShowsTheFactorySetlistAndTheFirstSaveMakesOne) {
  // A factory preset in the store's Factory folder, as an installer leaves it.
  TempDir store("fresh");
  juce::ValueTree factory(t3k::presetfile::kTag);
  factory.setProperty("schemaVersion", 1, nullptr);
  factory.setProperty("name", "Shipped Tone", nullptr);
  factory.setProperty("id", "shipped-1", nullptr);
  factory.appendChild(juce::ValueTree("ChainSnapshot"), nullptr);
  ASSERT_TRUE(store.dir.getChildFile("Factory").createDirectory());
  ASSERT_TRUE(t3k::presetfile::write(store.dir.getChildFile("Factory").getChildFile("Shipped Tone.t3kpreset"),
                                     factory));

  LibraryRig rig(store.dir);
  // Nothing of the library exists yet, and looking does not make it.
  EXPECT_FALSE(rig.libraryRoot().exists());
  juce::var list = rig.must("preset.list");
  EXPECT_EQ(list["setlist"]["id"].toString(), juce::String("factory"));
  EXPECT_TRUE(static_cast<bool>(list["setlist"]["readOnly"]));
  ASSERT_EQ(list["presets"].size(), 1);
  EXPECT_EQ(list["presets"][0]["id"].toString(), juce::String("factory:shipped-1"));
  EXPECT_TRUE(static_cast<bool>(list["presets"][0]["factory"]));
  EXPECT_EQ(static_cast<int>(list["presets"][0]["slot"]), 1);
  EXPECT_EQ(rig.proc.getProgramName(0), juce::String("Shipped Tone"));
  EXPECT_FALSE(rig.libraryRoot().exists());

  // The factory setlist is read-only, by every route.
  rig.must("preset.load", R"({"id":"factory:shipped-1"})");
  EXPECT_EQ(rig.proc.getCurrentProgram(), 0);
  EXPECT_COMMAND_ERROR(rig.run("preset.rename", R"({"id":"factory:shipped-1","name":"Mine"})"),
                       CommandError::failed);
  EXPECT_COMMAND_ERROR(rig.run("preset.delete", R"({"id":"factory:shipped-1"})"), CommandError::failed);
  EXPECT_COMMAND_ERROR(rig.run("preset.move", R"({"id":"factory:shipped-1","delta":1})"),
                       CommandError::failed);
  EXPECT_COMMAND_ERROR(rig.run("preset.saveToSlot", R"({"slot":1,"name":"x","setlist":"factory"})"),
                       CommandError::failed);
  EXPECT_COMMAND_ERROR(rig.run("preset.deleteSlot", R"({"slot":1,"setlist":"factory"})"),
                       CommandError::failed);
  EXPECT_COMMAND_ERROR(rig.run("preset.moveSlot", R"({"from":1,"to":2,"setlist":"factory"})"),
                       CommandError::failed);
  EXPECT_COMMAND_ERROR(rig.run("preset.duplicateSlot", R"({"slot":1,"setlist":"factory"})"),
                       CommandError::failed);
  EXPECT_COMMAND_ERROR(rig.run("setlist.rename", R"({"id":"factory","name":"Mine"})"),
                       CommandError::failed);
  EXPECT_COMMAND_ERROR(rig.run("setlist.delete", R"({"id":"factory"})"), CommandError::failed);
  EXPECT_COMMAND_ERROR(rig.run("setlist.reorder", R"({"id":"factory","index":0})"),
                       CommandError::failed);
  EXPECT_TRUE(store.dir.getChildFile("Factory").getChildFile("Shipped Tone.t3kpreset").existsAsFile());

  // The first save makes a setlist of the user's own and goes there.
  const juce::var saved = rig.must("preset.save", R"({"name":"First Of Mine"})");
  EXPECT_TRUE(saved["id"].toString().startsWith("user:"));
  EXPECT_EQ(static_cast<int>(saved["slot"]), 1);
  EXPECT_TRUE(rig.setlistFolder("My Presets").getChildFile("001 First Of Mine.json").existsAsFile());
  EXPECT_TRUE(rig.setlistFolder("My Presets").getChildFile("setlist.json").existsAsFile());
  list = rig.must("preset.list");
  EXPECT_EQ(list["setlist"]["name"].toString(), juce::String("My Presets"));
  EXPECT_FALSE(static_cast<bool>(list["setlist"]["readOnly"]));
  ASSERT_EQ(list["presets"].size(), 1);
  EXPECT_EQ(list["presets"][0]["id"].toString(), saved["id"].toString());
  // The factory setlist is still there, last, and its presets still load.
  const juce::var setlists = rig.must("setlist.list")["setlists"];
  ASSERT_EQ(setlists.size(), 2);
  EXPECT_EQ(setlists[0]["name"].toString(), juce::String("My Presets"));
  EXPECT_TRUE(static_cast<bool>(setlists[0]["current"]));
  EXPECT_EQ(setlists[1]["id"].toString(), juce::String("factory"));
  EXPECT_EQ(static_cast<int>(setlists[1]["presetCount"]), 1);
  rig.must("preset.load", R"({"id":"factory:shipped-1"})");
  EXPECT_EQ(rig.must("preset.list")["setlist"]["name"].toString(), juce::String("My Presets"))
      << "loading a preset by id does not change the setlist";

  // A factory preset can be copied into a setlist, where it is the user's.
  const juce::String mine = setlists[0]["id"].toString();
  const juce::var copied = rig.must(
      "preset.copyToSetlist", "{\"slot\":1,\"setlist\":\"factory\",\"to\":" + quoted(mine) + "}");
  EXPECT_EQ(slotsOf(copied), juce::String("1:First Of Mine 2:Shipped Tone"));
  EXPECT_TRUE(copied["preset"]["id"].toString().startsWith("user:"));
  rig.must("preset.loadSlot", R"({"slot":2})");
  EXPECT_EQ(rig.proc.getChainState(-1)["preset"]["name"].toString(), juce::String("Shipped Tone"));

  // Selected on purpose, the factory setlist is the list again; a save from
  // there still lands in the user's setlist, which becomes current.
  rig.must("setlist.select", R"({"id":"factory"})");
  EXPECT_EQ(rig.must("preset.list")["presets"][0]["id"].toString(), juce::String("factory:shipped-1"));
  rig.must("preset.save", R"({"name":"From Factory"})");
  EXPECT_EQ(rig.must("preset.list")["setlist"]["id"].toString(), mine);
  EXPECT_EQ(slotsOf(rig.must("setlist.get")), juce::String("1:First Of Mine 2:Shipped Tone 3:From Factory"));
}

TEST(LibraryIntegrationTest, ProgramChangesAreSlots) {
  LibraryRig rig;
  const juce::String setlist =
      rig.must("setlist.create", R"({"name":"Gig","select":true})")["id"].toString();
  // Slots 1, 2, 4 and 130 (beyond the 128 programs); 3 is a gap.
  const juce::String one = rig.saveWithBass("One", 1.0f, 1);
  const juce::String two = rig.saveWithBass("Two", 2.0f, 2);
  const juce::String four = rig.saveWithBass("Four", 4.0f, 4);
  const juce::String distant = rig.saveWithBass("Far", 9.0f, 130);
  EXPECT_EQ(rig.proc.getNumPrograms(), 128);

  // Program n is slot n + 1, by the host route...
  rig.proc.setCurrentProgram(0);
  EXPECT_NEAR(rig.toneBass(), 1.0f, 0.01f);
  EXPECT_EQ(rig.proc.getCurrentProgram(), 0);
  rig.proc.setCurrentProgram(3);
  EXPECT_NEAR(rig.toneBass(), 4.0f, 0.01f);
  EXPECT_EQ(rig.proc.getCurrentProgram(), 3);
  EXPECT_EQ(rig.activePresetId(), four);
  // ...an empty slot is an empty program: nothing happens...
  rig.proc.setCurrentProgram(2);
  EXPECT_NEAR(rig.toneBass(), 4.0f, 0.01f);
  EXPECT_EQ(rig.proc.getCurrentProgram(), 3);
  rig.proc.setCurrentProgram(60);
  EXPECT_EQ(rig.activePresetId(), four);
  // ...and by raw MIDI.
  juce::MidiBuffer midi;
  midi.addEvent(juce::MidiMessage::programChange(1, 1), 0);
  rig.proc.midiMapper.processMidi(midi);
  pumpMessages();
  EXPECT_NEAR(rig.toneBass(), 2.0f, 0.01f);
  EXPECT_EQ(rig.activePresetId(), two);
  midi.clear();
  midi.addEvent(juce::MidiMessage::programChange(1, 2), 0);  // the gap
  rig.proc.midiMapper.processMidi(midi);
  pumpMessages();
  EXPECT_EQ(rig.activePresetId(), two);

  // Names by program; gaps and the end read "(empty)".
  EXPECT_EQ(rig.proc.getProgramName(0), juce::String("One"));
  EXPECT_EQ(rig.proc.getProgramName(1), juce::String("Two"));
  EXPECT_EQ(rig.proc.getProgramName(2), juce::String("(empty)"));
  EXPECT_EQ(rig.proc.getProgramName(3), juce::String("Four"));
  EXPECT_EQ(rig.proc.getProgramName(127), juce::String("(empty)"));

  // preset.list: the presets, each with its slot; state.get the same.
  const juce::var list = rig.must("preset.list");
  ASSERT_EQ(list["presets"].size(), 4);
  EXPECT_EQ(static_cast<int>(list["presets"][2]["slot"]), 4);
  EXPECT_EQ(list["presets"][2]["id"].toString(), four);
  EXPECT_EQ(static_cast<int>(list["presets"][3]["slot"]), 130);
  const juce::var state = rig.must("state.get");
  EXPECT_EQ(state["presets"].size(), 4);
  EXPECT_EQ(state["library"]["current"]["setlist"].toString(), setlist);
  EXPECT_EQ(static_cast<int>(state["library"]["current"]["slot"]), 2);
  EXPECT_EQ(state["library"]["current"]["presetId"].toString(), two);

  // Next and previous pass over the gap and wrap, the far preset included.
  ASSERT_TRUE(rig.proc.loadPreset(two));
  rig.must("footswitch.setMode", R"({"mode":"scene"})");
  rig.must("footswitch.press", R"({"switch":"bankUp"})");
  rig.must("footswitch.release", R"({"switch":"bankUp"})");
  EXPECT_EQ(rig.activePresetId(), four);
  rig.must("footswitch.press", R"({"switch":"bankUp"})");
  EXPECT_EQ(rig.activePresetId(), distant);
  EXPECT_EQ(rig.proc.getCurrentProgram(), 0) << "beyond the program range reads as program 0";
  rig.must("footswitch.press", R"({"switch":"bankUp"})");
  EXPECT_EQ(rig.activePresetId(), one);
  rig.must("footswitch.press", R"({"switch":"bankDown"})");
  EXPECT_EQ(rig.activePresetId(), distant);

  // Deleting a preset leaves its slot empty: the programs after it do not
  // move (which is what slots are for).
  rig.must("preset.delete", "{\"id\":" + quoted(two) + "}");
  EXPECT_EQ(rig.proc.getProgramName(1), juce::String("(empty)"));
  EXPECT_EQ(rig.proc.getProgramName(3), juce::String("Four"));
  rig.proc.setCurrentProgram(3);
  EXPECT_EQ(rig.activePresetId(), four);
  // preset.move trades places among the slots in use.
  rig.must("preset.move", "{\"id\":" + quoted(four) + ",\"delta\":-1}");
  EXPECT_EQ(rig.proc.getProgramName(0), juce::String("Four"));
  EXPECT_EQ(rig.proc.getProgramName(3), juce::String("One"));
  EXPECT_EQ(rig.proc.getCurrentProgram(), 0) << "the active preset moved to slot 1";

  // Another setlist selected: the programs are its slots, at once.
  rig.must("setlist.create", R"({"name":"Other"})");
  const juce::String other = setlistIdNamed(rig, "Other");
  const juce::String elsewhere = rig.saveWithBass("Elsewhere", 6.0f, 5, other);
  EXPECT_EQ(rig.must("preset.list")["setlist"]["id"].toString(), other)
      << "saving into a setlist goes there";
  EXPECT_EQ(rig.proc.getProgramName(4), juce::String("Elsewhere"));
  EXPECT_EQ(rig.proc.getProgramName(0), juce::String("(empty)"));
  EXPECT_EQ(rig.proc.getCurrentProgram(), 4);
  rig.must("setlist.select", "{\"id\":" + quoted(setlist) + "}");
  EXPECT_EQ(rig.proc.getProgramName(0), juce::String("Four"));
  EXPECT_EQ(rig.proc.getCurrentProgram(), 0) << "the active preset is not in this setlist";
  rig.proc.setCurrentProgram(4);
  EXPECT_EQ(rig.activePresetId(), elsewhere) << "an empty program";
  rig.proc.setCurrentProgram(0);
  EXPECT_EQ(rig.activePresetId(), four);
}

TEST(LibraryIntegrationTest, FootswitchPresetBanksAreSlots) {
  LibraryRig rig;
  rig.must("setlist.create", R"({"name":"Gig","select":true})");
  // Bank 0: slots 1, 2, 4. Bank 1: slot 9. Bank 2: slot 17. Slot 3 is a gap.
  std::map<int, juce::String> ids;
  for (const int slot : {1, 2, 4, 9, 17})
    ids[slot] = rig.saveWithBass("Slot " + juce::String(slot), static_cast<float>(slot), slot);
  rig.must("footswitch.setMode", R"({"mode":"preset"})");

  juce::var state = rig.must("footswitch.getState");
  EXPECT_EQ(static_cast<int>(state["bank"]), 0);
  EXPECT_EQ(static_cast<int>(state["bankCount"]), 3);
  // A: slot 1. C: the gap. D: slot 4.
  EXPECT_EQ(footswitch(rig, 0)["label"].toString(), juce::String("Slot 1"));
  EXPECT_EQ(footswitch(rig, 0)["presetId"].toString(), ids[1]);
  EXPECT_FALSE(static_cast<bool>(footswitch(rig, 0)["empty"]));
  EXPECT_TRUE(static_cast<bool>(footswitch(rig, 2)["empty"]));
  EXPECT_TRUE(footswitch(rig, 2)["presetId"].isVoid());
  EXPECT_EQ(static_cast<int>(footswitch(rig, 2)["presetIndex"]), 2);
  EXPECT_EQ(footswitch(rig, 3)["label"].toString(), juce::String("Slot 4"));
  EXPECT_TRUE(static_cast<bool>(footswitch(rig, 4)["empty"]));
  // The last save is active and lit only where it is.
  EXPECT_FALSE(static_cast<bool>(footswitch(rig, 0)["lit"]));

  rig.must("footswitch.press", R"({"switch":"D"})");
  rig.must("footswitch.release", R"({"switch":"D"})");
  EXPECT_EQ(rig.activePresetId(), ids[4]);
  EXPECT_NEAR(rig.toneBass(), 4.0f, 0.01f);
  EXPECT_TRUE(static_cast<bool>(footswitch(rig, 3)["lit"]));
  // The gap does nothing.
  rig.must("footswitch.press", R"({"switch":"C"})");
  rig.must("footswitch.release", R"({"switch":"C"})");
  EXPECT_EQ(rig.activePresetId(), ids[4]);

  // Bank up: slots 9 to 16.
  rig.must("footswitch.press", R"({"switch":"bankUp"})");
  rig.must("footswitch.release", R"({"switch":"bankUp"})");
  EXPECT_EQ(static_cast<int>(rig.must("footswitch.getState")["bank"]), 1);
  EXPECT_EQ(rig.activePresetId(), ids[4]) << "paging loads nothing";
  EXPECT_EQ(footswitch(rig, 0)["label"].toString(), juce::String("Slot 9"));
  EXPECT_TRUE(static_cast<bool>(footswitch(rig, 1)["empty"]));
  rig.must("footswitch.press", R"({"switch":"A"})");
  EXPECT_EQ(rig.activePresetId(), ids[9]);
  // Up again: slot 17; once more wraps to the first bank; down wraps back.
  rig.must("footswitch.press", R"({"switch":"bankUp"})");
  EXPECT_EQ(footswitch(rig, 0)["label"].toString(), juce::String("Slot 17"));
  rig.must("footswitch.press", R"({"switch":"A"})");
  EXPECT_EQ(rig.activePresetId(), ids[17]);
  rig.must("footswitch.press", R"({"switch":"bankUp"})");
  EXPECT_EQ(static_cast<int>(rig.must("footswitch.getState")["bank"]), 0);
  rig.must("footswitch.press", R"({"switch":"bankDown"})");
  EXPECT_EQ(static_cast<int>(rig.must("footswitch.getState")["bank"]), 2);

  // Another setlist: its own banks, starting at the first.
  rig.must("setlist.create", R"({"name":"Short"})");
  const juce::String shortId = setlistIdNamed(rig, "Short");
  const juce::String only = rig.saveWithBass("Only", 7.0f, 2, shortId);
  state = rig.must("footswitch.getState");
  EXPECT_EQ(static_cast<int>(state["bank"]), 0);
  EXPECT_EQ(static_cast<int>(state["bankCount"]), 1);
  EXPECT_TRUE(static_cast<bool>(footswitch(rig, 0)["empty"]));
  EXPECT_EQ(footswitch(rig, 1)["presetId"].toString(), only);
  EXPECT_TRUE(static_cast<bool>(footswitch(rig, 1)["lit"]));

  // A hybrid with one preset row: banks of four slots.
  rig.must("setlist.select", "{\"id\":" + quoted(setlistIdNamed(rig, "Gig")) + "}");
  rig.must("footswitch.setMode", R"({"mode":"hybrid"})");
  rig.must("footswitch.setHybridRows", R"({"top":"preset","bottom":"stomp"})");
  EXPECT_EQ(static_cast<int>(rig.must("footswitch.getState")["bankCount"]), 5);
  rig.must("footswitch.press", R"({"switch":"bankUp"})");
  rig.must("footswitch.press", R"({"switch":"bankUp"})");
  EXPECT_EQ(footswitch(rig, 0)["label"].toString(), juce::String("Slot 9"));
}

// ---------------------------------------------------------------------------
// Favourites, recents, the position, unsaved changes
// ---------------------------------------------------------------------------

TEST(LibraryStateTest, FavouritesRecentsAndThePosition) {
  TempDir store("state");
  juce::String gig, practice, a, b, c, d;
  {
    LibraryRig rig(store.dir);
    gig = rig.must("setlist.create", R"({"name":"Gig","select":true})")["id"].toString();
    a = rig.saveWithBass("A", 1.0f, 1);
    b = rig.saveWithBass("B", 2.0f, 2);
    c = rig.saveWithBass("C", 3.0f, 5);
    practice = rig.must("setlist.create", R"({"name":"Practice"})")["id"].toString();
    d = rig.saveWithBass("D", 4.0f, 1, practice);
    rig.must("setlist.select", "{\"id\":" + quoted(gig) + "}");

    // Saving is not loading: nothing is recent yet.
    EXPECT_EQ(rig.must("recent.list")["recents"].size(), 0);

    // Loads, by id and by slot: most recent first, each once.
    rig.must("preset.load", "{\"id\":" + quoted(a) + "}");
    rig.must("preset.loadSlot", R"({"slot":5})");
    rig.must("preset.load", "{\"id\":" + quoted(b) + "}");
    rig.must("preset.load", "{\"id\":" + quoted(a) + "}");
    juce::var recents = rig.must("recent.list")["recents"];
    ASSERT_EQ(recents.size(), 3);
    EXPECT_EQ(recents[0]["id"].toString(), a);
    EXPECT_EQ(recents[1]["id"].toString(), b);
    EXPECT_EQ(recents[2]["id"].toString(), c);
    EXPECT_EQ(recents[2]["name"].toString(), juce::String("C"));
    EXPECT_EQ(recents[2]["setlist"].toString(), gig);
    EXPECT_EQ(static_cast<int>(recents[2]["slot"]), 5);

    // The position follows loads inside the current setlist...
    juce::var library = rig.library();
    EXPECT_EQ(library["current"]["setlist"].toString(), gig);
    EXPECT_EQ(static_cast<int>(library["current"]["slot"]), 1);
    EXPECT_EQ(library["current"]["presetId"].toString(), a);
    EXPECT_EQ(library["root"].toString(), rig.libraryRoot().getFullPathName());
    rig.must("preset.loadSlot", R"({"slot":5})");
    EXPECT_EQ(static_cast<int>(rig.library()["current"]["slot"]), 5);
    EXPECT_EQ(static_cast<int>(rig.must("setlist.get")["currentSlot"]), 5);
    // ...a preset of another setlist loaded by id plays, and leaves the
    // position where it was (the gig's setlist stays the gig's)...
    rig.must("preset.load", "{\"id\":" + quoted(d) + "}");
    EXPECT_NEAR(rig.toneBass(), 4.0f, 0.01f);
    library = rig.library();
    EXPECT_EQ(library["current"]["setlist"].toString(), gig);
    EXPECT_EQ(static_cast<int>(library["current"]["slot"]), 5);
    EXPECT_EQ(library["current"]["presetId"].toString(), d);
    EXPECT_EQ(rig.must("recent.list")["recents"][0]["setlist"].toString(), practice);
    // ...and loading a slot of another setlist goes there.
    rig.must("preset.loadSlot", "{\"slot\":1,\"setlist\":" + quoted(practice) + "}");
    EXPECT_EQ(rig.library()["current"]["setlist"].toString(), practice);
    EXPECT_EQ(static_cast<int>(rig.library()["current"]["slot"]), 1);
    rig.must("preset.loadSlot", "{\"slot\":2,\"setlist\":" + quoted(gig) + "}");

    // Favourites: a set of ids, shown on the slots.
    rig.must("favourite.set", "{\"id\":" + quoted(c) + ",\"favourite\":true}");
    const juce::var favourites =
        rig.must("favourite.set", "{\"id\":" + quoted(a) + ",\"favourite\":true}")["favourites"];
    EXPECT_EQ(favourites.size(), 2);
    juce::var setlist = rig.must("setlist.get");
    EXPECT_TRUE(static_cast<bool>(slotAt(setlist, 1)["favourite"]));
    EXPECT_FALSE(static_cast<bool>(slotAt(setlist, 2)["favourite"]));
    EXPECT_TRUE(static_cast<bool>(slotAt(setlist, 5)["favourite"]));
    rig.must("favourite.set", "{\"id\":" + quoted(a) + ",\"favourite\":false}");
    rig.must("favourite.set", "{\"id\":" + quoted(a) + ",\"favourite\":false}");
    EXPECT_EQ(rig.library()["favourites"].size(), 1);
    // A favourite survives its preset being renamed, moved and saved over.
    rig.must("preset.rename", "{\"id\":" + quoted(c) + ",\"name\":\"See\"}");
    rig.must("preset.moveSlot", R"({"from":5,"to":9})");
    rig.saveWithBass("See", 3.5f);
    setlist = rig.must("setlist.get");
    EXPECT_EQ(slotAt(setlist, 9)["id"].toString(), c);
    EXPECT_TRUE(static_cast<bool>(slotAt(setlist, 9)["favourite"]));
    EXPECT_EQ(static_cast<int>(setlist["currentSlot"]), 9);

    // A deleted preset drops out of the recents list (and comes back with
    // its file: the list in library.json is not rewritten for it).
    rig.must("preset.delete", "{\"id\":" + quoted(a) + "}");
    recents = rig.must("recent.list")["recents"];
    for (const auto& recent : *recents.getArray())
      EXPECT_NE(recent["id"].toString(), a);
    EXPECT_TRUE(rig.proc.getLibraryRoot().getChildFile("library.json").loadFileAsString().contains(a));

    // library.get counts what the old store still has to offer (nothing).
    EXPECT_EQ(static_cast<int>(rig.library()["legacyPresets"]), 0);
    EXPECT_COMMAND_ERROR(rig.run("favourite.set", R"({"id":" ","favourite":true})"),
                         CommandError::invalidArgs);
    EXPECT_COMMAND_ERROR(rig.run("favourite.set", R"({"id":"user:x"})"), CommandError::invalidArgs);
  }

  // The next launch: same setlist, same slot, same favourites and recents.
  LibraryRig next(store.dir);
  const juce::var library = next.library();
  EXPECT_EQ(library["current"]["setlist"].toString(), gig);
  EXPECT_EQ(static_cast<int>(library["current"]["slot"]), 9);
  EXPECT_TRUE(library["current"]["presetId"].toString().isEmpty()) << "nothing is loaded yet";
  ASSERT_EQ(library["favourites"].size(), 1);
  EXPECT_EQ(library["favourites"][0].toString(), c);
  EXPECT_EQ(library["recents"][0]["id"].toString(), b);
  EXPECT_EQ(slotsOf(next.must("setlist.get")), juce::String("2:B 9:See"));
  EXPECT_EQ(next.proc.getProgramName(8), juce::String("See"));
}

TEST(LibraryStateTest, UnsavedChanges) {
  ScannerSandbox sandbox;
  const juce::String identifier = scanTestPlugin();
  LibraryRig rig;

  // No preset: "unsaved" is "not the empty default" (upstream's atDefault).
  EXPECT_FALSE(rig.unsaved());
  EXPECT_TRUE(static_cast<bool>(rig.proc.getChainState(-1)["atDefault"]));
  const juce::String amp = rig.addTone("a2-amp-test.nam");
  const juce::String delay = rig.addKind("delay.digital");
  const juce::String plugin = rig.addPlugin(identifier);
  rig.settle();
  EXPECT_TRUE(rig.unsaved());

  // Saved: clean.
  const juce::String id = rig.must("preset.save", R"({"name":"Clean Slate"})")["id"].toString();
  EXPECT_FALSE(rig.unsaved());
  EXPECT_FALSE(rig.unsaved()) << "asking twice changes nothing";

  // A faceplate knob, and back.
  const float savedBass = rig.toneBass();
  rig.setToneBass(savedBass > 5.0f ? 1.5f : 8.5f);
  EXPECT_TRUE(rig.unsaved());
  rig.setToneBass(savedBass);
  EXPECT_FALSE(rig.unsaved());
  // Calibration is the user's setup, not the tone: not a change.
  rig.must("param.set", R"({"id":"calibrateInput","value":1})");
  EXPECT_FALSE(rig.unsaved());

  // A block setting; undo makes it clean again, redo dirty.
  rig.setParam(amp, "inputGain", 0.8);
  EXPECT_TRUE(rig.unsaved());
  rig.must("chain.undo");
  EXPECT_FALSE(rig.unsaved());
  rig.must("chain.redo");
  EXPECT_TRUE(rig.unsaved());
  // Saving over it: clean.
  rig.must("preset.saveToSlot", R"({"slot":1})");
  EXPECT_FALSE(rig.unsaved());
  EXPECT_EQ(rig.activePresetId(), id);

  // A kind parameter, an EQ band, a structural edit, a stomp, a scene name.
  const auto [param, value] = rig.kindParam(delay, 0.8);
  rig.setParam(delay, param, value);
  EXPECT_TRUE(rig.unsaved());
  rig.must("chain.undo");
  EXPECT_FALSE(rig.unsaved());
  rig.must("block.setEqBand", "{\"blockId\":" + quoted(amp) + ",\"band\":1,\"gainDb\":4}");
  EXPECT_TRUE(rig.unsaved());
  rig.must("chain.undo");
  const juce::String extra = rig.addKind("utility.gain");
  rig.settle();
  EXPECT_TRUE(rig.unsaved());
  EXPECT_FALSE(extra.isEmpty());
  rig.must("chain.undo");
  EXPECT_FALSE(rig.unsaved()) << "added and undone: the rig is the preset";
  rig.must("stomp.assign", "{\"switch\":\"B\",\"blockIds\":[" + quoted(delay) + "]}");
  EXPECT_TRUE(rig.unsaved());
  rig.must("chain.undo");
  rig.must("scene.rename", R"({"scene":"C","name":"Bridge"})");
  EXPECT_TRUE(rig.unsaved());
  rig.must("chain.undo");
  EXPECT_FALSE(rig.unsaved());
  // The hosted plugin's own parameter.
  juce::String gainId;
  const juce::var pluginParams = rig.must("block.listParams", "{\"blockId\":" + quoted(plugin) + "}");
  for (const auto& listed : *pluginParams["params"].getArray())
    if (listed["name"].toString() == "Gain")
      gainId = listed["id"].toString();
  rig.setParam(plugin, gainId, 0.25);
  pumpMessages(100);
  EXPECT_TRUE(rig.unsaved());
  rig.must("chain.undo");
  rig.settle();
  EXPECT_FALSE(rig.unsaved());

  // Loaded: clean while the rig is still arriving and once it has, for a
  // rig with a capture, an effect and a plugin (whose blocks write
  // themselves from live processors after the load).
  rig.must("chain.reset");
  EXPECT_FALSE(rig.unsaved()) << "the empty default";
  rig.must("preset.load", "{\"id\":" + quoted(id) + "}");
  EXPECT_FALSE(rig.unsaved());
  rig.settle();
  EXPECT_FALSE(rig.unsaved());
  pumpMessages(300);
  EXPECT_FALSE(rig.unsaved());
  // And an edit after a load is one.
  rig.setParam(amp, "mix", 0.5);
  EXPECT_TRUE(rig.unsaved());
  rig.must("chain.undo");
  EXPECT_FALSE(rig.unsaved());

  // A scene switch is not an undo step, but the rig is then not what the
  // preset opens as; back on the preset's scene it is.
  rig.must("scene.select", R"({"scene":"B"})");
  EXPECT_TRUE(rig.unsaved());
  rig.must("scene.select", R"({"scene":"A"})");
  EXPECT_FALSE(rig.unsaved());

  // A session restore: the session's rig is the reference from then on.
  rig.setParam(amp, "outputGain", 0.3);
  ASSERT_TRUE(rig.unsaved());
  const juce::MemoryBlock session = stateOf(rig.proc);
  rig.proc.setStateInformation(session.getData(), static_cast<int>(session.getSize()));
  rig.settle();
  EXPECT_EQ(rig.activePresetId(), id);
  EXPECT_FALSE(rig.unsaved());
  rig.setParam(amp, "outputGain", 0.6);
  EXPECT_TRUE(rig.unsaved());

  // Deleted: the rig is nobody's preset, and it is not the default.
  rig.must("preset.delete", "{\"id\":" + quoted(id) + "}");
  EXPECT_TRUE(rig.activePresetId().isEmpty());
  EXPECT_TRUE(rig.unsaved());
  // state.get carries the flag too.
  EXPECT_TRUE(static_cast<bool>(rig.must("state.get")["library"]["unsaved"]));
}

// ---------------------------------------------------------------------------
// The old store
// ---------------------------------------------------------------------------

// The old store as an existing user has it: two presets saved by a recent
// build, one from before readable names (<uuid>.t3kpreset in the old
// framing, no id inside), a custom order, and a factory preset. Each loads
// with a toneBass of its own.
struct OldStore {
  OldStore() {
    LibraryRig old;  // only here to make real preset trees
    buildSmallRig(old);
    const juce::File exported = old.store.getChildFile("scratch.t3kpreset");
    const PresetManager manager(store.dir);
    for (const auto& [name, bass, idOut] :
         {std::tuple<const char*, float, juce::String*>{"Old Crunch", 2.0f, &crunchId},
          {"Old Lead", 8.0f, &leadId}}) {
      old.setToneBass(bass);
      EXPECT_TRUE(old.proc.exportPresetJson({}, exported, false, name).wasOk());
      const PresetManager::Info info = manager.save(name, t3k::presetfile::read(exported));
      EXPECT_TRUE(info.id.startsWith("user:"));
      *idOut = info.id;
    }
    old.setToneBass(4.0f);
    EXPECT_TRUE(old.proc.exportPresetJson({}, exported, false, "Ancient").wasOk());
    juce::ValueTree ancient = t3k::presetfile::read(exported);
    ancient.removeProperty("id", nullptr);
    {
      juce::FileOutputStream out(store.dir.getChildFile("0123456789abcdef0123456789abcdef.t3kpreset"));
      EXPECT_TRUE(out.openedOk());
      out.write("T3KB", 4);
      ancient.writeToStream(out);
      out.flush();
    }
    old.setToneBass(6.0f);
    EXPECT_TRUE(old.proc.exportPresetJson({}, exported, false, "Shipped").wasOk());
    EXPECT_TRUE(store.dir.getChildFile("Factory").createDirectory());
    EXPECT_TRUE(exported.copyFileTo(store.dir.getChildFile("Factory").getChildFile("Shipped.t3kpreset")));
    // The player's own order, not the names': Old Lead first.
    EXPECT_TRUE(manager.move(leadId, -2));

    for (const auto& file : store.dir.findChildFiles(juce::File::findFiles, true, "*.t3kpreset"))
      files[file.getFullPathName()] = fileBytes(file);
    EXPECT_EQ(files.size(), 4u);
    // What host program n was before the library: entry n of this list.
    programs = PresetManager(store.dir).list();
  }

  void expectUntouched() const {
    for (const auto& [path, bytes] : files) {
      EXPECT_TRUE(juce::File(path).existsAsFile()) << path;
      EXPECT_TRUE(fileBytes(juce::File(path)) == bytes) << path;
    }
    EXPECT_EQ(store.dir.findChildFiles(juce::File::findFiles, true, "*.t3kpreset").size(), 4);
  }

  static float bassOf(const juce::String& name) {
    return name == "Old Crunch" ? 2.0f : name == "Old Lead" ? 8.0f : name == "Ancient" ? 4.0f : 6.0f;
  }

  TempDir store{"legacy"};
  juce::String crunchId, leadId;
  const juce::String ancientId = "user:0123456789abcdef0123456789abcdef";
  std::map<juce::String, juce::MemoryBlock> files;
  std::vector<PresetManager::Info> programs;
};

TEST(LibraryLegacyImportTest, TheOldPresetListBecomesTheDefaultSetlistProgramForProgram) {
  const OldStore old;
  ASSERT_EQ(old.programs.size(), 4u);
  // (User presets led the old list, in the player's order; factory last.)
  EXPECT_EQ(old.programs[0].name, juce::String("Old Lead"));
  EXPECT_EQ(old.programs[3].name, juce::String("Shipped"));
  EXPECT_TRUE(old.programs[3].factory);

  juce::String setlistId, shippedCopyId;
  {
    LibraryRig rig(old.store.dir);
    // The first look at the library brings the old list over: nothing to
    // ask for, and the preset list is the one the player had.
    const juce::var list = rig.must("preset.list");
    setlistId = list["setlist"]["id"].toString();
    EXPECT_EQ(list["setlist"]["name"].toString(), juce::String("My Presets"));
    EXPECT_FALSE(static_cast<bool>(list["setlist"]["readOnly"]));
    ASSERT_EQ(list["presets"].size(), 4);
    for (int n = 0; n < 4; ++n) {
      SCOPED_TRACE("program " + std::to_string(n));
      const PresetManager::Info& was = old.programs[static_cast<size_t>(n)];
      EXPECT_EQ(list["presets"][n]["name"].toString(), was.name);
      EXPECT_EQ(static_cast<int>(list["presets"][n]["slot"]), n + 1);
      EXPECT_EQ(rig.proc.getProgramName(n), was.name);
      // A preset of the player's keeps its id; a factory preset's copy is a
      // preset of the player's now.
      if (was.factory) {
        shippedCopyId = list["presets"][n]["id"].toString();
        EXPECT_TRUE(shippedCopyId.startsWith("user:"));
      } else {
        EXPECT_EQ(list["presets"][n]["id"].toString(), was.id);
      }
    }
    EXPECT_TRUE(rig.setlistFolder("My Presets").getChildFile("001 Old Lead.json").existsAsFile());
    EXPECT_TRUE(rig.setlistFolder("My Presets").getChildFile("004 Shipped.json").existsAsFile());
    // Their models are in the store once (all four share the IR).
    EXPECT_EQ(rig.libraryRoot().getChildFile("Assets").getNumberOfChildFiles(juce::File::findFiles), 1);

    // Host program n and MIDI program change n load what they always did.
    for (const int n : {2, 0, 3, 1}) {
      SCOPED_TRACE("program " + std::to_string(n));
      const PresetManager::Info& was = old.programs[static_cast<size_t>(n)];
      rig.proc.setCurrentProgram(n);
      rig.settle();
      EXPECT_NEAR(rig.toneBass(), OldStore::bassOf(was.name), 0.01f);
      EXPECT_EQ(rig.proc.getChainState(-1)["preset"]["name"].toString(), was.name);
      EXPECT_EQ(rig.proc.getCurrentProgram(), n);
    }
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::programChange(1, 0), 0);
    rig.proc.midiMapper.processMidi(midi);
    pumpMessages();
    rig.settle();
    EXPECT_NEAR(rig.toneBass(), 8.0f, 0.01f);

    // An id a session saved long ago loads, and the rig is the old rig:
    // byte for byte what the .t3kpreset restores to.
    rig.must("preset.load", "{\"id\":" + quoted(old.crunchId) + "}");
    rig.settle();
    EXPECT_NEAR(rig.toneBass(), 2.0f, 0.01f);
    {
      LibraryRig fromOld;
      ASSERT_TRUE(fromOld.proc.loadPresetFile(old.store.dir.getChildFile("Old Lead.t3kpreset")));
      fromOld.settle();
      // (Exported under the same file name: the file name is the id of a
      // preset loaded straight from a file.)
      const juce::File asJson = rig.store.getChildFile("exported").getChildFile("Old Lead.json");
      ASSERT_TRUE(rig.proc.exportPresetJson(old.leadId, asJson, false).wasOk());
      EXPECT_EQ(asJson.loadFileAsString(),
                rig.setlistFolder("My Presets").getChildFile("001 Old Lead.json").loadFileAsString());
      LibraryRig fromNew;
      ASSERT_TRUE(fromNew.proc.loadPresetFile(asJson));
      fromNew.settle();
      EXPECT_TRUE(stateOf(fromOld.proc) == stateOf(fromNew.proc));
    }

    // The old files are where they were, and the factory preset still is
    // the read-only one it was.
    old.expectUntouched();
    const juce::var factory = rig.must("setlist.get", R"({"setlist":"factory"})");
    EXPECT_EQ(slotsOf(factory), juce::String("1:Shipped"));
    EXPECT_TRUE(slotAt(factory, 1)["id"].toString().startsWith("factory:"));

    // It is on record, and there is nothing left to offer.
    EXPECT_TRUE(static_cast<bool>(
        juce::JSON::parse(rig.libraryRoot().getChildFile("library.json").loadFileAsString())
            ["legacyPresetsMigrated"]));
    EXPECT_EQ(static_cast<int>(rig.library()["legacyPresets"]), 0);
    const juce::var again = rig.must("library.importLegacyPresets");
    EXPECT_EQ(static_cast<int>(again["imported"]), 0);
    EXPECT_EQ(static_cast<int>(again["skipped"]), 3);

    // The player throws one out.
    rig.must("preset.delete", "{\"id\":" + quoted(old.ancientId) + "}");
  }

  // The next launch does not bring it back: its slot stays empty and the
  // others keep theirs.
  {
    LibraryRig rig(old.store.dir);
    const juce::var setlist = rig.must("setlist.get");
    EXPECT_EQ(setlist["id"].toString(), setlistId);
    EXPECT_EQ(setlist["slots"].size(), 3);
    for (size_t n = 0; n < old.programs.size(); ++n) {
      const juce::var slot = slotAt(setlist, static_cast<int>(n) + 1);
      if (old.programs[n].id == old.ancientId)
        EXPECT_TRUE(slot.isVoid());
      else
        EXPECT_EQ(slot["name"].toString(), old.programs[n].name);
    }
    // It is offered, though, and comes back when asked for.
    EXPECT_EQ(static_cast<int>(rig.library()["legacyPresets"]), 1);

    // Nor does emptying the library altogether.
    rig.must("setlist.delete", "{\"id\":" + quoted(setlistId) + "}");
  }
  {
    LibraryRig rig(old.store.dir);
    EXPECT_EQ(rig.must("preset.list")["setlist"]["id"].toString(), juce::String("factory"));
    EXPECT_EQ(rig.must("setlist.list")["setlists"].size(), 1);
    EXPECT_FALSE(rig.libraryRoot().getChildFile("Setlists").getChildFile("My Presets").exists());
    EXPECT_EQ(static_cast<int>(rig.library()["legacyPresets"]), 3);
    // The command is still there for whoever wants them after all, into a
    // setlist of its own and under the ids they had.
    const juce::var imported = rig.must("library.importLegacyPresets");
    EXPECT_EQ(static_cast<int>(imported["imported"]), 3);
    ASSERT_EQ(imported["setlists"].size(), 1);
    const juce::var setlist =
        rig.must("setlist.get", "{\"setlist\":" + quoted(imported["setlists"][0].toString()) + "}");
    EXPECT_EQ(setlist["name"].toString(), juce::String("Imported Presets"));
    EXPECT_EQ(slotsOf(setlist), juce::String("1:Old Lead 2:Ancient 3:Old Crunch"));
    EXPECT_EQ(slotAt(setlist, 2)["id"].toString(), old.ancientId);
    // One deleted from the library is offered again, and comes back alone,
    // into the setlist the caller names.
    rig.must("preset.delete", "{\"id\":" + quoted(old.ancientId) + "}");
    rig.must("setlist.create", R"({"name":"Keepers"})");
    const juce::var third = rig.must("library.importLegacyPresets", R"({"setlist":"Keepers"})");
    EXPECT_EQ(static_cast<int>(third["imported"]), 1);
    EXPECT_EQ(slotsOf(rig.must("setlist.get", "{\"setlist\":" + quoted(setlistIdNamed(rig, "Keepers")) + "}")),
              juce::String("1:Ancient"));
  }
  old.expectUntouched();
}

TEST(LibraryLegacyImportTest, ALibraryThatIsInUseIsLeftAsItIs) {
  const OldStore old;
  // A library somebody already put a setlist in (made by a build from before
  // the migration, or pulled from a repository): theirs, as it is.
  {
    SetlistLibrary library(old.store.dir.getChildFile("Library"));
    ASSERT_TRUE(library.createSetlist("Mine").wasOk());
  }
  LibraryRig rig(old.store.dir);
  const juce::var setlists = rig.must("setlist.list")["setlists"];
  ASSERT_EQ(setlists.size(), 2);
  EXPECT_EQ(setlists[0]["name"].toString(), juce::String("Mine"));
  EXPECT_EQ(static_cast<int>(setlists[0]["presetCount"]), 0);
  // The old presets are offered, not brought.
  EXPECT_EQ(static_cast<int>(rig.library()["legacyPresets"]), 3);
  EXPECT_COMMAND_ERROR(rig.run("preset.load", "{\"id\":" + quoted(old.crunchId) + "}"),
                       CommandError::notFound);
  old.expectUntouched();
}

TEST(LibraryLegacyImportTest, MoreThanASetlistHoldsSpillsIntoASecond) {
  TempDir store("legacy-many");
  // 260 small old presets.
  for (int i = 0; i < 260; ++i) {
    juce::ValueTree preset(t3k::presetfile::kTag);
    preset.setProperty("schemaVersion", 1, nullptr);
    preset.setProperty("name", "Old " + juce::String(i).paddedLeft('0', 3), nullptr);
    preset.setProperty("id", "old-" + juce::String(i), nullptr);
    preset.appendChild(juce::ValueTree("ChainSnapshot"), nullptr);
    ASSERT_TRUE(t3k::presetfile::write(
        store.dir.getChildFile("Old " + juce::String(i).paddedLeft('0', 3) + ".t3kpreset"), preset));
  }
  // Brought over on first use: the first 256 into the default setlist (the
  // 128 programs among them), the rest into a second.
  {
    LibraryRig rig(store.dir);
    const juce::var setlists = rig.must("setlist.list")["setlists"];
    ASSERT_EQ(setlists.size(), 3);
    const juce::var first = rig.must("setlist.get", "{\"setlist\":" + quoted(setlists[0]["id"].toString()) + "}");
    const juce::var second = rig.must("setlist.get", "{\"setlist\":" + quoted(setlists[1]["id"].toString()) + "}");
    EXPECT_EQ(first["name"].toString(), juce::String("My Presets"));
    EXPECT_TRUE(static_cast<bool>(first["current"]));
    EXPECT_EQ(first["slots"].size(), 256);
    EXPECT_EQ(second["name"].toString(), juce::String("My Presets 2"));
    EXPECT_EQ(second["slots"].size(), 4);
    EXPECT_EQ(slotAt(first, 256)["name"].toString(), juce::String("Old 255"));
    EXPECT_EQ(slotAt(second, 1)["id"].toString(), juce::String("user:old-256"));
    EXPECT_EQ(rig.proc.getProgramName(127), juce::String("Old 127"));
    EXPECT_EQ(static_cast<int>(rig.library()["legacyPresets"]), 0);
    rig.must("setlist.delete", "{\"id\":" + quoted(setlists[0]["id"].toString()) + "}");
    rig.must("setlist.delete", "{\"id\":" + quoted(setlists[1]["id"].toString()) + "}");
  }
  // The command, into setlists of the caller's naming, spills the same way.
  LibraryRig rig(store.dir);
  EXPECT_EQ(static_cast<int>(rig.library()["legacyPresets"]), 260);
  const juce::var imported = rig.must("library.importLegacyPresets", R"({"setlist":"From Before"})");
  EXPECT_EQ(static_cast<int>(imported["imported"]), 260);
  ASSERT_EQ(imported["setlists"].size(), 2);
  const juce::var first = rig.must("setlist.get", "{\"setlist\":" + quoted(imported["setlists"][0].toString()) + "}");
  const juce::var second = rig.must("setlist.get", "{\"setlist\":" + quoted(imported["setlists"][1].toString()) + "}");
  EXPECT_EQ(first["name"].toString(), juce::String("From Before"));
  EXPECT_EQ(first["slots"].size(), 256);
  EXPECT_EQ(second["name"].toString(), juce::String("From Before 2"));
  EXPECT_EQ(second["slots"].size(), 4);
  EXPECT_EQ(slotAt(first, 256)["name"].toString(), juce::String("Old 255"));
  EXPECT_EQ(slotAt(second, 1)["id"].toString(), juce::String("user:old-256"));
  EXPECT_EQ(static_cast<int>(rig.library()["legacyPresets"]), 0);
  EXPECT_EQ(store.dir.findChildFiles(juce::File::findFiles, false, "*.t3kpreset").size(), 260);
}

// ---------------------------------------------------------------------------
// A session a host restores
// ---------------------------------------------------------------------------

TEST(LibraryStateTest, ARestoredSessionKeepsItsOwnSetlistWhateverTheLibraryFileNames) {
  TempDir store("restore");
  juce::String gig, practice, c;
  juce::MemoryBlock session;
  {
    LibraryRig rig(store.dir);
    gig = rig.must("setlist.create", R"({"name":"Gig","select":true})")["id"].toString();
    rig.saveWithBass("A", 1.0f, 1);
    rig.saveWithBass("B", 2.0f, 2);
    c = rig.saveWithBass("C", 3.0f, 3);
    practice = rig.must("setlist.create", R"({"name":"Practice"})")["id"].toString();
    rig.saveWithBass("X", 7.0f, 1, practice);
    rig.saveWithBass("Y", 8.0f, 2, practice);
    rig.saveWithBass("Z", 9.0f, 3, practice);

    // The session: C of the gig setlist (program 2), with a knob moved
    // since it was loaded.
    rig.must("preset.loadSlot", "{\"slot\":3,\"setlist\":" + quoted(gig) + "}");
    rig.settle();
    rig.setToneBass(5.5f);
    ASSERT_EQ(rig.proc.getCurrentProgram(), 2);
    session = stateOf(rig.proc);

    // Later, in another project, the player moves on to the other setlist:
    // that is what library.json names from here on.
    rig.must("setlist.select", "{\"id\":" + quoted(practice) + "}");
  }
  const juce::File libraryFile = store.dir.getChildFile("Library").getChildFile("library.json");
  const juce::MemoryBlock libraryBefore = fileBytes(libraryFile);

  LibraryRig rig(store.dir);
  EXPECT_EQ(rig.must("preset.list")["setlist"]["id"].toString(), practice);
  rig.proc.setStateInformation(session.getData(), static_cast<int>(session.getSize()));
  rig.settle();
  ASSERT_EQ(rig.activePresetId(), c);
  EXPECT_NEAR(rig.toneBass(), 5.5f, 0.01f);

  // The instance is in the session's setlist again, so the program the host
  // holds for the session is the session's preset...
  EXPECT_EQ(rig.must("preset.list")["setlist"]["id"].toString(), gig);
  EXPECT_EQ(rig.proc.getCurrentProgram(), 2);
  EXPECT_EQ(rig.proc.getProgramName(2), juce::String("C"));
  // ...and the host echoing it back after the restore loads nothing: not
  // Z, the third preset of the setlist library.json names, and not C over
  // the session's own settings either.
  rig.proc.setCurrentProgram(2);
  pumpMessages();
  rig.settle();
  EXPECT_EQ(rig.activePresetId(), c) << "a program replay loaded another setlist's preset";
  EXPECT_NEAR(rig.toneBass(), 5.5f, 0.01f) << "a program replay reloaded over the session";

  // Restoring a session is not the player choosing a setlist: the file is
  // untouched, and a new instance still starts where the player left off.
  EXPECT_TRUE(fileBytes(libraryFile) == libraryBefore);
  {
    LibraryRig other(store.dir);
    EXPECT_EQ(other.must("preset.list")["setlist"]["id"].toString(), practice);
    // Nor does that instance moving about move this one any more.
    other.must("preset.loadSlot", "{\"slot\":1,\"setlist\":" + quoted(practice) + "}");
  }
  EXPECT_EQ(rig.must("preset.list")["setlist"]["id"].toString(), gig);

  // A program change the player does send is one of the session's setlist.
  rig.proc.setCurrentProgram(0);
  rig.settle();
  EXPECT_NEAR(rig.toneBass(), 1.0f, 0.01f);
  EXPECT_EQ(rig.proc.getChainState(-1)["preset"]["name"].toString(), juce::String("A"));

  // A session whose preset is in no setlist (deleted since) stays with the
  // setlist selected when it was restored (the gig, since the load above),
  // pinned there.
  {
    LibraryRig lost(store.dir);
    lost.must("preset.delete", "{\"id\":" + quoted(c) + "}");
    ASSERT_EQ(lost.must("preset.list")["setlist"]["id"].toString(), gig);
    lost.proc.setStateInformation(session.getData(), static_cast<int>(session.getSize()));
    lost.settle();
    EXPECT_EQ(lost.must("preset.list")["setlist"]["id"].toString(), gig);
    EXPECT_EQ(lost.proc.getCurrentProgram(), 0);
    rig.must("setlist.select", "{\"id\":" + quoted(practice) + "}");  // library.json moves on
    EXPECT_EQ(LibraryRig(store.dir).must("preset.list")["setlist"]["id"].toString(), practice);
    EXPECT_EQ(lost.must("preset.list")["setlist"]["id"].toString(), gig);
  }
}

// ---------------------------------------------------------------------------
// Files that mean harm, and writes that fail
// ---------------------------------------------------------------------------

struct ScopedFileFault {
  explicit ScopedFileFault(presetjson::FileFault fault)
      : previous(presetjson::setFileFaultForTesting(fault)) {}
  ~ScopedFileFault() { presetjson::setFileFaultForTesting(previous); }
  presetjson::FileFault previous;
};

// Empty when every number in the tree is a finite one, else where one is not.
juce::String firstNonFinite(const juce::ValueTree& node, const juce::String& path = {}) {
  const juce::String here = path + "/" + node.getType().toString();
  for (int i = 0; i < node.getNumProperties(); ++i) {
    const juce::var& value = node.getProperty(node.getPropertyName(i));
    if (value.isDouble() && !std::isfinite(static_cast<double>(value)))
      return here + "." + node.getPropertyName(i).toString();
  }
  for (int i = 0; i < node.getNumChildren(); ++i)
    if (const juce::String found = firstNonFinite(node.getChild(i), here); found.isNotEmpty())
      return found;
  return {};
}

juce::var typedDouble(const char* text) {
  juce::DynamicObject::Ptr object = new juce::DynamicObject();
  object->setProperty("$double", text);
  return juce::var(object.get());
}

// Sets every one of `keys` that `node` or a node below it holds to `value`.
// Returns how many were set.
int poison(const juce::var& node, const juce::StringArray& keys, const juce::var& value) {
  int count = 0;
  if (auto* object = node.getDynamicObject()) {
    for (const auto& key : keys)
      if (object->hasProperty(key)) {
        object->setProperty(key, value);
        ++count;
      }
    // A kind's parameter: { "node": "Param", "id": ..., "value": ... }.
    if (object->getProperty("node").toString() == "Param" && object->hasProperty("value")) {
      object->setProperty("value", value);
      ++count;
    }
    if (const auto* children = object->getProperty("children").getArray())
      for (const auto& child : *children)
        count += poison(child, keys, value);
  }
  return count;
}

TEST(LibraryHostileFileTest, NumbersThatAreNoneNeverReachTheRig) {
  TempDir outside("poison");
  const juce::File good = outside.dir.getChildFile("good.json");
  {
    LibraryRig rig;
    const juce::String ir = rig.addTone("cab-ir-test.wav");
    rig.addKind("delay.simple");
    rig.settle();
    rig.must("block.setEqBand", "{\"blockId\":" + quoted(ir) + ",\"band\":2,\"gainDb\":4.0}");
    rig.setParam(ir, "mix", 0.8);
    rig.must("param.set", R"({"id":"toneBass","value":7.25})");
    ASSERT_TRUE(rig.proc.exportPresetJson({}, good, true, "Poisoned").wasOk());
  }
  const juce::var parsedGood = juce::JSON::parse(good.loadFileAsString());
  ASSERT_TRUE(parsedGood["chain"].isObject());

  const std::vector<float> noise = makeNoise(4096, 7, 0.2f);
  for (const char* text : {"nan", "inf", "-inf"}) {
    SCOPED_TRACE(text);
    // Every gain, mix and size of every block, every EQ band and every kind
    // parameter says the non-number, as the format lets a file say it; the
    // faceplate parameters say an infinity the plain way.
    juce::var parsed = parsedGood.clone();
    const int poisoned = poison(
        parsed["chain"], {"inputGain", "outputGain", "mix", "slimSize", "freqHz", "gainDb", "q"},
        typedDouble(text));
    ASSERT_GT(poisoned, 20);
    juce::String json = presetjson::writeCanonical(parsed);
    ASSERT_TRUE(json.contains("\"toneBass\": 7.25"));
    json = json.replace("\"toneBass\": 7.25", juce::String("\"toneBass\": ") +
                                                  (text[0] == '-' ? "-1e999" : "1e999"));
    const juce::File file = outside.dir.getChildFile(juce::String("poisoned ") + text + ".json");
    writeText(file, json);
    // (The file does say it: the conversion itself keeps what it is given.)
    const presetjson::ReadResult read = presetjson::fromJson(json, {});
    ASSERT_TRUE(read.preset.isValid()) << read.error;
    ASSERT_TRUE(firstNonFinite(read.preset).isNotEmpty());

    LibraryRig rig;
    ASSERT_TRUE(rig.proc.loadPresetFile(file));
    rig.settle();
    // The faceplate parameter is its default, not a NaN handed to the host.
    auto* bass = rig.proc.parameters.getParameter("toneBass");
    EXPECT_TRUE(std::isfinite(rig.toneBass()));
    EXPECT_TRUE(std::isfinite(bass->getValue()));
    EXPECT_NEAR(bass->getValue(), bass->getDefaultValue(), 1.0e-6f);
    // The blocks' settings are the defaults.
    const juce::var chain = rig.proc.getChainState(-1)["chain"];
    ASSERT_GE(chain.size(), 2);
    int blocks = 0;
    for (int i = 0; i < chain.size(); ++i) {
      if (chain[i]["kind"].toString() == "insert" || !chain[i]["params"].isObject())
        continue;
      ++blocks;
      const juce::var params = chain[i]["params"];
      EXPECT_EQ(static_cast<double>(params["inputGain"]), 0.5) << "block " << i;
      EXPECT_EQ(static_cast<double>(params["outputGain"]), 0.5) << "block " << i;
      EXPECT_EQ(static_cast<double>(params["mix"]), 1.0) << "block " << i;
      EXPECT_EQ(static_cast<double>(params["slimSize"]), 0.0) << "block " << i;
    }
    EXPECT_GE(blocks, 2);
    // Nothing of it is in the rig: what a host would save is all numbers,
    // and so is the audio, this block and the next.
    const juce::MemoryBlock state = stateOf(rig.proc);
    const juce::ValueTree saved = juce::ValueTree::readFromData(
        static_cast<const char*>(state.getData()) + 4, state.getSize() - 4);
    ASSERT_TRUE(saved.isValid());
    EXPECT_EQ(firstNonFinite(saved), juce::String());
    for (int pass = 0; pass < 2; ++pass) {
      const auto [left, right] = processStereo(rig.proc, noise, kBlock);
      bool finite = true;
      for (size_t i = 0; i < left.size(); ++i)
        finite = finite && std::isfinite(left[i]) && std::isfinite(right[i]);
      EXPECT_TRUE(finite) << "a number that is none reached the output";
    }
  }
}

TEST(LibraryHostileFileTest, AFileNestedTooDeepIsRefusedByEveryRoute) {
  TempDir outside("deep");
  const juce::String bomb = "{\"format\":\"pedaleira.preset\",\"version\":1,\"name\":\"Bomb\","
                            "\"chain\":{\"children\":" +
                            juce::String::repeatedString("[", 200000) + "}}";
  const juce::File file = outside.dir.getChildFile("bomb.json");
  writeText(file, bomb);

  LibraryRig rig;
  buildSmallRig(rig);
  rig.must("setlist.create", R"({"name":"Gig","select":true})");
  rig.saveWithBass("Good", 3.0f, 1);
  // Loaded as a file, imported, and dropped into a setlist's folder.
  EXPECT_FALSE(rig.proc.loadPresetFile(file));
  const CommandResult imported =
      rig.run("preset.importJson", "{\"path\":" + quoted(file.getFullPathName()) + "}");
  EXPECT_COMMAND_ERROR(imported, CommandError::failed);
  EXPECT_TRUE(imported.errorMessage.contains("nested deeper")) << imported.errorMessage;
  ASSERT_TRUE(file.copyFileTo(rig.setlistFolder("Gig").getChildFile("002 Bomb.json")));
  const juce::var setlist = rig.must("library.rescan")["setlists"];
  const juce::var listed = rig.must("setlist.get");
  EXPECT_EQ(slotsOf(listed), juce::String("1:Good"));
  ASSERT_EQ(listed["ignored"].size(), 1);
  EXPECT_EQ(listed["ignored"][0]["file"].toString(), juce::String("002 Bomb.json"));
  EXPECT_TRUE(listed["ignored"][0]["reason"].toString().contains("nested deeper than 64 levels"));
  EXPECT_COMMAND_ERROR(rig.run("preset.loadSlot", R"({"slot":2})"), CommandError::notFound);
  // The rig is what it was, and the good preset still loads.
  rig.must("preset.loadSlot", R"({"slot":1})");
  rig.settle();
  EXPECT_NEAR(rig.toneBass(), 3.0f, 0.01f);
}

TEST(LibraryHostileFileTest, AnExportThatFailsKeepsTheFileThatWasThere) {
  TempDir outside("export-fail");
  LibraryRig rig;
  buildSmallRig(rig);
  const juce::File file = outside.dir.getChildFile("Travels.json");
  const juce::String args = "{\"path\":" + quoted(file.getFullPathName()) + ",\"name\":\"Travels\"}";
  rig.must("preset.exportJson", args);
  const juce::MemoryBlock first = fileBytes(file);
  ASSERT_GT(first.getSize(), 100u);

  // The rig changes, and the next export over the same file cannot be
  // written (a full disk) or cannot take its place (a locked file).
  rig.setToneBass(9.0f);
  for (const auto fault : {presetjson::FileFault::write, presetjson::FileFault::replace}) {
    const ScopedFileFault scoped(fault);
    const CommandResult result = rig.run("preset.exportJson", args);
    EXPECT_COMMAND_ERROR(result, CommandError::failed);
    EXPECT_TRUE(result.errorMessage.contains(file.getFileName())) << result.errorMessage;
    ASSERT_TRUE(file.existsAsFile()) << "the export deleted the file it could not replace";
    EXPECT_TRUE(fileBytes(file) == first);
    EXPECT_EQ(outside.dir.findChildFiles(juce::File::findFiles, false, "*.tmp").size(), 0);
  }
  // And once it can, the file is the new export.
  rig.must("preset.exportJson", args);
  EXPECT_FALSE(fileBytes(file) == first);
  EXPECT_TRUE(file.loadFileAsString().contains("\"toneBass\": 9.0"));
}

// ---------------------------------------------------------------------------
// The chain lock
// ---------------------------------------------------------------------------

TEST(LibraryCommandsTest, TheLibraryIsNeverReadUnderTheChainLock) {
  TempDir outside("lock");
  LibraryRig rig;
  buildSmallRig(rig);

  // Every time the library reads a folder, another thread asks for the
  // chain lock the way the audio thread does (a try). It must be free: a
  // folder scan under it is a block of silence.
  std::atomic<int> scans{0}, underLock{0};
  struct Restore {
    ~Restore() { SetlistLibrary::setScanObserverForTesting(previous); }
    std::function<void()> previous;
  } restore{SetlistLibrary::setScanObserverForTesting([&] {
    ++scans;
    // (Asked for a while: a background model load may hold the lock for a
    // moment of its own. The thread reading the folder waits here, so if it
    // is the holder the lock never comes free.)
    bool lockFree = false;
    std::thread asker([&] {
      const auto deadline = juce::Time::getMillisecondCounter() + 2000;
      while (!(lockFree = rig.proc.chainLockFreeForTesting()) &&
             juce::Time::getMillisecondCounter() < deadline)
        juce::Thread::sleep(1);
    });
    asker.join();
    if (!lockFree)
      ++underLock;
  })};
  const auto step = [&](const char* what) {
    EXPECT_EQ(underLock.exchange(0), 0) << "the library was read under the chain lock by " << what;
  };

  const juce::String gig = rig.must("setlist.create", R"({"name":"Gig","select":true})")["id"].toString();
  const juce::String other = rig.must("setlist.create", R"({"name":"Other"})")["id"].toString();
  step("setlist.create");
  const juce::String a = rig.saveWithBass("A", 1.0f, 1);
  const juce::String b = rig.saveWithBass("B", 2.0f, 2);
  rig.must("preset.save", R"({"name":"C"})");
  step("saving");
  for (const char* command : {"library.get", "preset.list", "setlist.list", "setlist.get",
                              "recent.list", "state.get", "footswitch.getState"}) {
    rig.must(command);
    step(command);
  }
  rig.must("preset.loadSlot", R"({"slot":1})");
  rig.settle();
  step("preset.loadSlot");
  rig.must("preset.load", "{\"id\":" + quoted(b) + "}");
  rig.settle();
  step("preset.load");
  rig.proc.setCurrentProgram(0);
  rig.must("footswitch.getState");
  rig.settle();
  (void)rig.proc.getCurrentProgram();
  (void)rig.proc.getProgramName(1);
  step("the host program calls");
  rig.must("preset.rename", "{\"id\":" + quoted(b) + ",\"name\":\"Bee\"}");
  step("preset.rename");
  rig.must("preset.move", "{\"id\":" + quoted(b) + ",\"delta\":-1}");
  step("preset.move");
  rig.must("preset.moveSlot", R"({"from":1,"to":4})");
  step("preset.moveSlot");
  rig.must("preset.duplicateSlot", R"({"slot":4})");
  step("preset.duplicateSlot");
  rig.must("preset.copyToSetlist", "{\"slot\":4,\"to\":" + quoted(other) + "}");
  step("preset.copyToSetlist");
  rig.must("favourite.set", "{\"id\":" + quoted(a) + ",\"favourite\":true}");
  step("favourite.set");
  const juce::File exported = outside.dir.getChildFile("out.json");
  rig.must("preset.exportJson", "{\"path\":" + quoted(exported.getFullPathName()) + ",\"id\":" + quoted(a) + "}");
  rig.must("preset.importJson", "{\"path\":" + quoted(exported.getFullPathName()) + "}");
  step("export and import");
  rig.must("setlist.select", "{\"id\":" + quoted(other) + "}");
  rig.must("setlist.select", "{\"id\":" + quoted(gig) + "}");
  rig.must("setlist.rename", "{\"id\":" + quoted(other) + ",\"name\":\"Others\"}");
  rig.must("setlist.reorder", "{\"id\":" + quoted(other) + ",\"index\":0}");
  step("the setlist commands");
  const juce::MemoryBlock state = stateOf(rig.proc);
  rig.proc.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
  rig.settle();
  step("saving and restoring the session");

  // Deleting the slot of the active preset: whether a copy of it is left
  // elsewhere is a scan of every setlist.
  const juce::String active = rig.activePresetId();
  ASSERT_TRUE(active.isNotEmpty());
  const juce::var setlist = rig.must("setlist.get");
  int activeSlot = 0;
  if (const auto* slots = setlist["slots"].getArray())
    for (const auto& slot : *slots)
      if (slot["id"].toString() == active)
        activeSlot = slot["slot"];
  ASSERT_GT(activeSlot, 0);
  underLock.store(0);
  rig.must("preset.deleteSlot", "{\"slot\":" + juce::String(activeSlot) + "}");
  step("preset.deleteSlot");
  EXPECT_EQ(rig.activePresetId(), juce::String());
  rig.must("preset.delete", "{\"id\":" + quoted(b) + "}");
  step("preset.delete");
  rig.must("library.rescan", R"({"tidy":true})");
  rig.must("library.importLegacyPresets");
  rig.must("setlist.delete", "{\"id\":" + quoted(other) + "}");
  step("rescan, import and setlist.delete");
  EXPECT_GT(scans.load(), 50);
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

TEST(LibraryCommandsTest, SetlistCommands) {
  LibraryRig rig;
  // Only the factory setlist to begin with.
  juce::var listed = rig.must("setlist.list");
  ASSERT_EQ(listed["setlists"].size(), 1);
  EXPECT_EQ(listed["current"].toString(), juce::String("factory"));

  const juce::String rehearsal = rig.must("setlist.create", R"({"name":"Rehearsal"})")["id"].toString();
  const juce::String gig = rig.must("setlist.create", R"({"name":"Gig"})")["id"].toString();
  ASSERT_TRUE(rehearsal.isNotEmpty() && gig.isNotEmpty() && rehearsal != gig);
  listed = rig.must("setlist.list");
  ASSERT_EQ(listed["setlists"].size(), 3);
  EXPECT_EQ(listed["setlists"][0]["name"].toString(), juce::String("Rehearsal"));
  EXPECT_FALSE(static_cast<bool>(listed["setlists"][0]["readOnly"]));
  EXPECT_EQ(static_cast<int>(listed["setlists"][0]["presetCount"]), 0);
  EXPECT_EQ(listed["setlists"][2]["id"].toString(), juce::String("factory"));
  // The first setlist of the user's own is the current one without asking.
  EXPECT_EQ(listed["current"].toString(), rehearsal);

  // select, with create's own flag and by command.
  const juce::String encore =
      rig.must("setlist.create", R"({"name":"Encore","select":true})")["id"].toString();
  EXPECT_EQ(rig.must("setlist.list")["current"].toString(), encore);
  const juce::var selected = rig.must("setlist.select", "{\"id\":" + quoted(gig) + "}");
  EXPECT_EQ(selected["id"].toString(), gig);
  EXPECT_TRUE(static_cast<bool>(selected["current"]));
  EXPECT_EQ(static_cast<int>(selected["capacity"]), 256);
  EXPECT_EQ(selected["slots"].size(), 0);
  EXPECT_EQ(rig.must("setlist.list")["current"].toString(), gig);

  // get: the current one by default, any by id.
  EXPECT_EQ(rig.must("setlist.get")["id"].toString(), gig);
  const juce::var got = rig.must("setlist.get", "{\"setlist\":" + quoted(rehearsal) + "}");
  EXPECT_EQ(got["name"].toString(), juce::String("Rehearsal"));
  EXPECT_FALSE(static_cast<bool>(got["current"]));
  EXPECT_EQ(got["folder"].toString(), rig.setlistFolder("Rehearsal").getFullPathName());
  EXPECT_EQ(rig.must("setlist.get", R"({"setlist":"factory"})")["name"].toString(), juce::String("Factory"));

  // rename, reorder.
  rig.must("setlist.rename", "{\"id\":" + quoted(rehearsal) + ",\"name\":\"Rehearsal Room\"}");
  EXPECT_TRUE(rig.setlistFolder("Rehearsal Room").isDirectory());
  const juce::var reordered =
      rig.must("setlist.reorder", "{\"id\":" + quoted(encore) + ",\"index\":0}");
  EXPECT_EQ(reordered["setlists"][0]["id"].toString(), encore);
  EXPECT_EQ(reordered["setlists"][1]["name"].toString(), juce::String("Rehearsal Room"));
  EXPECT_EQ(reordered["setlists"][3]["id"].toString(), juce::String("factory"));

  // delete: to the trash; the current one falls back to the first.
  const juce::var deleted = rig.must("setlist.delete", "{\"id\":" + quoted(gig) + "}");
  EXPECT_EQ(deleted["deletedId"].toString(), gig);
  EXPECT_EQ(deleted["setlists"].size(), 3);
  EXPECT_EQ(deleted["current"].toString(), encore);
  EXPECT_FALSE(rig.setlistFolder("Gig").exists());

  // Errors.
  EXPECT_COMMAND_ERROR(rig.run("setlist.create", R"({"name":"  "})"), CommandError::invalidArgs);
  EXPECT_COMMAND_ERROR(rig.run("setlist.create", R"({})"), CommandError::invalidArgs);
  EXPECT_COMMAND_ERROR(rig.run("setlist.create", R"({"name":"encore"})"), CommandError::failed);
  EXPECT_COMMAND_ERROR(rig.run("setlist.rename", "{\"id\":" + quoted(encore) + ",\"name\":\"\"}"),
                       CommandError::invalidArgs);
  EXPECT_COMMAND_ERROR(
      rig.run("setlist.rename", "{\"id\":" + quoted(encore) + ",\"name\":\"REHEARSAL ROOM\"}"),
      CommandError::failed);
  for (const char* command : {"setlist.delete", "setlist.select"})
    EXPECT_COMMAND_ERROR(rig.run(command, R"({"id":"nope"})"), CommandError::notFound);
  EXPECT_COMMAND_ERROR(rig.run("setlist.rename", R"({"id":"nope","name":"x"})"), CommandError::notFound);
  EXPECT_COMMAND_ERROR(rig.run("setlist.reorder", R"({"id":"nope","index":0})"), CommandError::notFound);
  EXPECT_COMMAND_ERROR(rig.run("setlist.reorder", "{\"id\":" + quoted(encore) + ",\"index\":-1}"),
                       CommandError::invalidArgs);
  EXPECT_COMMAND_ERROR(rig.run("setlist.get", R"({"setlist":"nope"})"), CommandError::notFound);
  EXPECT_COMMAND_ERROR(rig.run("setlist.get", R"({"id":"x"})"), CommandError::invalidArgs);
}

TEST(LibraryCommandsTest, SlotCommands) {
  LibraryRig rig;
  buildSmallRig(rig);
  const juce::String gig = rig.must("setlist.create", R"({"name":"Gig"})")["id"].toString();
  const juce::String spare = rig.must("setlist.create", R"({"name":"Spare"})")["id"].toString();

  // saveToSlot: into a slot, with the id, setlist and slot in the answer.
  rig.setToneBass(1.0f);
  const juce::var a = rig.must("preset.saveToSlot", R"({"slot":3,"name":"A"})");
  EXPECT_EQ(static_cast<int>(a["slot"]), 3);
  EXPECT_EQ(a["setlist"].toString(), gig);
  EXPECT_EQ(a["name"].toString(), juce::String("A"));
  EXPECT_FALSE(a["revision"].isVoid());
  EXPECT_EQ(rig.activePresetId(), a["id"].toString());
  rig.setToneBass(2.0f);
  const juce::var b = rig.must("preset.saveToSlot", R"({"slot":4,"name":"B"})");
  // Over an occupied slot: the id stays; the name too when none is given.
  rig.setToneBass(3.0f);
  const juce::var over = rig.must("preset.saveToSlot", R"({"slot":3})");
  EXPECT_EQ(over["id"].toString(), a["id"].toString());
  EXPECT_EQ(over["name"].toString(), juce::String("A"));
  const juce::var renamed = rig.must("preset.saveToSlot", R"({"slot":3,"name":"A2"})");
  EXPECT_EQ(renamed["id"].toString(), a["id"].toString());
  EXPECT_TRUE(rig.setlistFolder("Gig").getChildFile("003 A2.json").existsAsFile());
  EXPECT_FALSE(rig.setlistFolder("Gig").getChildFile("003 A.json").exists());
  // Into another setlist, which becomes current.
  rig.must("preset.saveToSlot", "{\"slot\":1,\"name\":\"S\",\"setlist\":" + quoted(spare) + "}");
  EXPECT_EQ(rig.must("setlist.list")["current"].toString(), spare);
  rig.must("setlist.select", "{\"id\":" + quoted(gig) + "}");

  // setlist.get describes the slots.
  juce::var setlist = rig.must("setlist.get");
  EXPECT_EQ(slotsOf(setlist), juce::String("3:A2 4:B"));
  const juce::var slot = slotAt(setlist, 3);
  EXPECT_EQ(slot["id"].toString(), a["id"].toString());
  EXPECT_EQ(static_cast<int>(slot["sceneCount"]), 1);
  EXPECT_TRUE(slot["tempo"].isVoid());
  EXPECT_EQ(slot["tags"].size(), 0);
  EXPECT_FALSE(static_cast<bool>(slot["favourite"]));
  EXPECT_EQ(slot["file"].toString(), juce::String("003 A2.json"));
  EXPECT_EQ(setlist["ignored"].size(), 0);

  // loadSlot.
  const juce::var loaded = rig.must("preset.loadSlot", R"({"slot":4})");
  EXPECT_EQ(loaded["preset"]["id"].toString(), b["id"].toString());
  EXPECT_EQ(static_cast<int>(loaded["slot"]), 4);
  EXPECT_FALSE(loaded["revision"].isVoid());
  EXPECT_NEAR(rig.toneBass(), 2.0f, 0.01f);
  rig.settle();

  // moveSlot: insert (the default) and swap; the position follows.
  setlist = rig.must("preset.moveSlot", R"({"from":4,"to":1})");
  EXPECT_EQ(slotsOf(setlist), juce::String("1:B 4:A2"));
  EXPECT_EQ(static_cast<int>(setlist["currentSlot"]), 1);
  EXPECT_EQ(rig.proc.getCurrentProgram(), 0);
  setlist = rig.must("preset.moveSlot", R"({"from":1,"to":4,"mode":"swap"})");
  EXPECT_EQ(slotsOf(setlist), juce::String("1:A2 4:B"));
  EXPECT_EQ(rig.proc.getCurrentProgram(), 3);
  EXPECT_TRUE(rig.setlistFolder("Gig").getChildFile("001 A2.json").existsAsFile());
  EXPECT_TRUE(rig.setlistFolder("Gig").getChildFile("004 B.json").existsAsFile());

  // duplicateSlot.
  setlist = rig.must("preset.duplicateSlot", R"({"slot":1})");
  EXPECT_EQ(slotsOf(setlist), juce::String("1:A2 2:A2 copy 4:B"));
  EXPECT_EQ(static_cast<int>(setlist["preset"]["slot"]), 2);
  EXPECT_NE(setlist["preset"]["id"].toString(), a["id"].toString());
  setlist = rig.must("preset.duplicateSlot", R"({"slot":4,"to":200})");
  EXPECT_EQ(slotAt(setlist, 200)["name"].toString(), juce::String("B copy"));
  // The copy is a preset of its own: it loads, and it is the rig of A2.
  rig.must("preset.loadSlot", R"({"slot":2})");
  EXPECT_NEAR(rig.toneBass(), 3.0f, 0.01f);

  // copyToSetlist.
  juce::var target = rig.must("preset.copyToSetlist", "{\"slot\":4,\"to\":" + quoted(spare) + "}");
  EXPECT_EQ(target["id"].toString(), spare);
  EXPECT_EQ(slotsOf(target), juce::String("1:S 2:B"));
  target = rig.must("preset.copyToSetlist",
                    "{\"slot\":1,\"to\":" + quoted(spare) + ",\"toSlot\":10}");
  EXPECT_EQ(slotsOf(target), juce::String("1:S 2:B 10:A2"));
  EXPECT_NE(slotAt(target, 10)["id"].toString(), a["id"].toString());
  // From a named setlist back into the current one.
  target = rig.must("preset.copyToSetlist", "{\"slot\":1,\"setlist\":" + quoted(spare) +
                                                ",\"to\":" + quoted(gig) + ",\"toSlot\":8}");
  EXPECT_EQ(slotAt(target, 8)["name"].toString(), juce::String("S"));

  // deleteSlot: the slot is empty, the others stay.
  setlist = rig.must("preset.deleteSlot", R"({"slot":2})");
  EXPECT_EQ(static_cast<int>(setlist["deletedSlot"]), 2);
  EXPECT_EQ(slotsOf(setlist), juce::String("1:A2 4:B 8:S 200:B copy"));
  EXPECT_TRUE(rig.activePresetId().isEmpty()) << "the active preset was the one deleted";
  EXPECT_FALSE(rig.setlistFolder("Gig").getChildFile("002 A2 copy.json").exists());

  // The older preset.* commands work on the same presets.
  rig.must("preset.rename", "{\"id\":" + quoted(b["id"].toString()) + ",\"name\":\"Bee\"}");
  EXPECT_TRUE(rig.setlistFolder("Gig").getChildFile("004 Bee.json").existsAsFile());
  EXPECT_EQ(rig.must("preset.save", R"({"name":"bee"})")["id"].toString(), b["id"].toString());
  EXPECT_EQ(static_cast<int>(rig.must("preset.save", R"({"name":"Newcomer"})")["slot"]), 2);

  // Errors: arguments, things that are not there, things the engine refuses.
  for (const char* args : {R"({"slot":0,"name":"x"})", R"({"slot":257,"name":"x"})",
                           R"({"slot":"one","name":"x"})", R"({"name":"x"})"})
    EXPECT_COMMAND_ERROR(rig.run("preset.saveToSlot", args), CommandError::invalidArgs);
  // An empty slot needs a name.
  EXPECT_COMMAND_ERROR(rig.run("preset.saveToSlot", R"({"slot":50})"), CommandError::failed);
  EXPECT_COMMAND_ERROR(rig.run("preset.saveToSlot", R"({"slot":50,"name":"x","setlist":"nope"})"),
                       CommandError::notFound);
  EXPECT_COMMAND_ERROR(rig.run("preset.loadSlot", R"({"slot":50})"), CommandError::notFound);
  EXPECT_COMMAND_ERROR(rig.run("preset.loadSlot", R"({"slot":0})"), CommandError::invalidArgs);
  EXPECT_COMMAND_ERROR(rig.run("preset.loadSlot", R"({"slot":1,"setlist":"nope"})"),
                       CommandError::notFound);
  EXPECT_COMMAND_ERROR(rig.run("preset.moveSlot", R"({"from":50,"to":1})"), CommandError::notFound);
  EXPECT_COMMAND_ERROR(rig.run("preset.moveSlot", R"({"from":1,"to":300})"), CommandError::invalidArgs);
  EXPECT_COMMAND_ERROR(rig.run("preset.moveSlot", R"({"from":1,"to":2,"mode":"shuffle"})"),
                       CommandError::invalidArgs);
  EXPECT_COMMAND_ERROR(rig.run("preset.duplicateSlot", R"({"slot":50})"), CommandError::notFound);
  // The target is taken.
  EXPECT_COMMAND_ERROR(rig.run("preset.duplicateSlot", R"({"slot":1,"to":4})"), CommandError::failed);
  EXPECT_COMMAND_ERROR(rig.run("preset.deleteSlot", R"({"slot":50})"), CommandError::notFound);
  EXPECT_COMMAND_ERROR(rig.run("preset.copyToSetlist", R"({"slot":1,"to":"nope"})"),
                       CommandError::notFound);
  EXPECT_COMMAND_ERROR(rig.run("preset.copyToSetlist", "{\"slot\":50,\"to\":" + quoted(spare) + "}"),
                       CommandError::notFound);
  EXPECT_COMMAND_ERROR(
      rig.run("preset.copyToSetlist", "{\"slot\":1,\"to\":" + quoted(spare) + ",\"toSlot\":1}"),
      CommandError::failed);
  EXPECT_COMMAND_ERROR(rig.run("preset.copyToSetlist", R"({"slot":1,"to":"factory"})"),
                       CommandError::failed);
  EXPECT_COMMAND_ERROR(rig.run("preset.copyToSetlist", R"({"slot":1})"), CommandError::invalidArgs);
  // Nothing of the above changed anything.
  EXPECT_EQ(slotsOf(rig.must("setlist.get")), juce::String("1:A2 2:Newcomer 4:bee 8:S 200:B copy"));
}

TEST(LibraryCommandsTest, ExportImportAndRescan) {
  TempDir outside("export");
  juce::String exportedId;
  juce::MemoryBlock exportedState;
  {
    LibraryRig rig;
    rig.addTone("a2-amp-test.nam");
    rig.addTone("cab-ir-test.wav");
    rig.addKind("delay.digital");
    rig.settle();
    rig.must("param.set", R"({"id":"toneBass","value":6.5})");
    exportedId = rig.must("preset.save", R"({"name":"Travels"})")["id"].toString();

    // By id, with its assets in a folder beside the file.
    const juce::File withAssets = outside.dir.getChildFile("shared").getChildFile("Travels.json");
    const juce::var exported = rig.must(
        "preset.exportJson",
        "{\"path\":" + quoted(withAssets.getFullPathName()) + ",\"id\":" + quoted(exportedId) + "}");
    EXPECT_EQ(exported["path"].toString(), withAssets.getFullPathName());
    ASSERT_TRUE(withAssets.existsAsFile());
    EXPECT_EQ(withAssets.getSiblingFile("Assets").getNumberOfChildFiles(juce::File::findFiles), 2);
    EXPECT_LT(withAssets.getSize(), 100000);
    // The same text as the library's own file: one format.
    EXPECT_EQ(withAssets.loadFileAsString(),
              rig.setlistFolder("My Presets").getChildFile("001 Travels.json").loadFileAsString());

    // The current rig, unsaved edits and all, as one self-contained file.
    rig.must("param.set", R"({"id":"toneBass","value":9})");
    const juce::File single = outside.dir.getChildFile("single.json");
    rig.must("preset.exportJson", "{\"path\":" + quoted(single.getFullPathName()) +
                                      ",\"name\":\"Travels Loud\",\"embedAssets\":true}");
    ASSERT_TRUE(single.existsAsFile());
    EXPECT_GT(single.getSize(), 100000);
    EXPECT_FALSE(outside.dir.getChildFile("Assets").exists());
    const juce::var parsedSingle = juce::JSON::parse(single.loadFileAsString());
    EXPECT_EQ(parsedSingle["name"].toString(), juce::String("Travels Loud"));
    EXPECT_TRUE(presetjson::referencedAssets(parsedSingle).empty());
    // The same path with the old extension gives the old format.
    const juce::File binary = outside.dir.getChildFile("Travels.t3kpreset");
    rig.must("preset.exportJson", "{\"path\":" + quoted(binary.getFullPathName()) + ",\"id\":" +
                                      quoted(exportedId) + "}");
    EXPECT_TRUE(t3k::presetfile::read(binary).isValid());
    // Exporting changed nothing here.
    EXPECT_EQ(rig.must("preset.list")["presets"].size(), 1);

    rig.must("preset.load", "{\"id\":" + quoted(exportedId) + "}");
    rig.settle();
    exportedState = stateOf(rig.proc);

    EXPECT_COMMAND_ERROR(rig.run("preset.exportJson", R"({"path":"x.json","id":"user:nope"})"),
                         CommandError::notFound);
    EXPECT_COMMAND_ERROR(rig.run("preset.exportJson", R"({})"), CommandError::invalidArgs);
    EXPECT_COMMAND_ERROR(rig.run("preset.exportJson", R"({"path":"  "})"), CommandError::invalidArgs);
  }

  // On another machine: another library, nothing in its store.
  LibraryRig rig;
  const juce::String setlist =
      rig.must("setlist.create", R"({"name":"Received","select":true})")["id"].toString();
  const juce::File withAssets = outside.dir.getChildFile("shared").getChildFile("Travels.json");
  juce::var imported = rig.must(
      "preset.importJson", "{\"path\":" + quoted(withAssets.getFullPathName()) + ",\"slot\":7}");
  EXPECT_EQ(slotsOf(imported), juce::String("7:Travels"));
  // Under the id it had (a favourite or a session that names it still finds it).
  EXPECT_EQ(imported["preset"]["id"].toString(), exportedId);
  EXPECT_EQ(imported["preset"]["missingAssets"].size(), 0);
  // Its models were copied into this library: the export folder may go.
  EXPECT_EQ(rig.libraryRoot().getChildFile("Assets").getNumberOfChildFiles(juce::File::findFiles), 2);
  EXPECT_TRUE(rig.activePresetId().isEmpty()) << "importing loads nothing";
  rig.must("preset.loadSlot", R"({"slot":7})");
  rig.settle();
  EXPECT_NEAR(rig.toneBass(), 6.5f, 0.01f);
  // The same rig as where it came from, to the byte.
  EXPECT_TRUE(stateOf(rig.proc) == exportedState);

  // The self-contained file, into the first empty slot; a .t3kpreset too.
  imported = rig.must("preset.importJson",
                      "{\"path\":" + quoted(outside.dir.getChildFile("single.json").getFullPathName()) + "}");
  EXPECT_EQ(slotsOf(imported), juce::String("1:Travels Loud 7:Travels"));
  EXPECT_NE(imported["preset"]["id"].toString(), exportedId) << "the id was taken: a new one";
  imported = rig.must(
      "preset.importJson",
      "{\"path\":" + quoted(outside.dir.getChildFile("Travels.t3kpreset").getFullPathName()) + "}");
  EXPECT_EQ(slotsOf(imported), juce::String("1:Travels Loud 2:Travels 7:Travels"));
  EXPECT_EQ(rig.libraryRoot().getChildFile("Assets").getNumberOfChildFiles(juce::File::findFiles), 2)
      << "three presets, the same two models";

  // A file whose assets nobody has: imported all the same, and said so.
  const juce::File lonely = outside.dir.getChildFile("elsewhere").getChildFile("Lonely.json");
  ASSERT_TRUE(lonely.getParentDirectory().createDirectory());
  juce::String lonelyText = withAssets.loadFileAsString();
  // (Other bytes behind the same names: hashes this library does not have.)
  const auto assets = presetjson::referencedAssets(juce::JSON::parse(lonelyText));
  for (const auto& asset : assets)
    lonelyText = lonelyText.replace(asset.hash, asset.hash.substring(1) + "0");
  writeText(lonely, lonelyText.replace("\"name\": \"Travels\"", "\"name\": \"Lonely\"")
                        .replace(exportedId.substring(5), "lonely-id"));
  imported = rig.must("preset.importJson", "{\"path\":" + quoted(lonely.getFullPathName()) + "}");
  EXPECT_EQ(imported["preset"]["missingAssets"].size(), 2);
  EXPECT_EQ(imported["preset"]["name"].toString(), juce::String("Lonely"));
  // What it pointed at is still in the file, for the day the assets come.
  const juce::String stored = rig.setlistFolder("Received").getChildFile("003 Lonely.json").loadFileAsString();
  EXPECT_EQ(presetjson::referencedAssets(juce::JSON::parse(stored)).size(), 2u);
  EXPECT_FALSE(stored.contains("pedaleira-missing-asset"));

  // Errors.
  EXPECT_COMMAND_ERROR(rig.run("preset.importJson", R"({"path":"/nowhere/at/all.json"})"),
                       CommandError::notFound);
  EXPECT_COMMAND_ERROR(
      rig.run("preset.importJson", "{\"path\":" + quoted(withAssets.getFullPathName()) + ",\"slot\":7}"),
      CommandError::failed);
  const juce::File notAPreset = outside.dir.getChildFile("notes.json");
  writeText(notAPreset, R"({"hello":"world"})");
  EXPECT_COMMAND_ERROR(rig.run("preset.importJson", "{\"path\":" + quoted(notAPreset.getFullPathName()) + "}"),
                       CommandError::failed);
  EXPECT_COMMAND_ERROR(rig.run("preset.importJson", "{\"path\":" + quoted(withAssets.getFullPathName()) +
                                                        ",\"setlist\":\"factory\"}"),
                       CommandError::failed);
  EXPECT_COMMAND_ERROR(rig.run("preset.importJson", R"({})"), CommandError::invalidArgs);

  // library.rescan: changes made behind the app's back, with and without
  // renaming the files to match.
  const juce::File folder = rig.setlistFolder("Received");
  ASSERT_TRUE(folder.getChildFile("007 Travels.json").copyFileTo(folder.getChildFile("Dropped in.json")));
  ASSERT_TRUE(rig.libraryRoot().getChildFile("Setlists").getChildFile("By Hand").createDirectory());
  writeText(folder.getChildFile("050 Junk.json"), "{ nope");
  juce::var rescanned = rig.must("library.rescan");
  EXPECT_EQ(static_cast<int>(rescanned["renamed"]), 0);
  EXPECT_EQ(rescanned["setlists"].size(), 3);
  EXPECT_EQ(rescanned["root"].toString(), rig.libraryRoot().getFullPathName());
  juce::var received = rig.must("setlist.get");
  EXPECT_EQ(slotsOf(received), juce::String("1:Travels Loud 2:Travels 3:Lonely 4:Travels 7:Travels"));
  ASSERT_EQ(received["ignored"].size(), 1);
  EXPECT_EQ(received["ignored"][0]["file"].toString(), juce::String("050 Junk.json"));
  EXPECT_EQ(slotAt(received, 4)["file"].toString(), juce::String("Dropped in.json"));
  EXPECT_EQ(rig.proc.getProgramName(3), juce::String("Travels"));
  rescanned = rig.must("library.rescan", R"({"tidy":true})");
  EXPECT_EQ(static_cast<int>(rescanned["renamed"]), 1);
  EXPECT_TRUE(folder.getChildFile("004 Travels.json").existsAsFile());
  EXPECT_FALSE(folder.getChildFile("Dropped in.json").exists());
  EXPECT_TRUE(folder.getChildFile("050 Junk.json").existsAsFile());
  EXPECT_COMMAND_ERROR(rig.run("library.rescan", R"({"tidy":"yes"})"), CommandError::invalidArgs);
  (void)setlist;
}

TEST(LibraryCommandsTest, ATempoATagAndSceneCountShowInTheSetlist) {
  LibraryRig rig;
  const juce::String delay = rig.addKind("delay.simple");
  const juce::String reverb = rig.addKind("reverb.room");
  rig.settle();
  rig.must("scene.rename", R"({"scene":"B","name":"Chorus"})");
  rig.must("scene.select", R"({"scene":"C"})");
  rig.must("block.setParam", "{\"blockId\":" + quoted(reverb) + ",\"param\":\"enabled\",\"value\":0}");
  rig.must("preset.save", R"({"name":"Ballad"})");
  const juce::File file = rig.setlistFolder("My Presets").getChildFile("001 Ballad.json");

  // A block state of its own in every scene: all eight count as used.
  EXPECT_EQ(static_cast<int>(slotAt(rig.must("setlist.get"), 1)["sceneCount"]), 8);
  EXPECT_TRUE(slotAt(rig.must("setlist.get"), 1)["tempo"].isVoid())
      << "this build's saves do not store a tempo";

  // A tempo and tags written into the file by hand (or by a later build)
  // are shown, kept by the edits that rewrite the file, and the tempo is
  // the rig's when the preset loads.
  juce::String text = file.loadFileAsString();
  ASSERT_TRUE(text.contains("  \"name\": \"Ballad\",\n"));
  text = text.replace("  \"name\": \"Ballad\",\n",
                      "  \"name\": \"Ballad\",\n  \"tempo\": 72.5,\n  \"tags\": [\"slow\", \"clean\"],\n");
  writeText(file, text);
  juce::var slot = slotAt(rig.must("setlist.get"), 1);
  EXPECT_DOUBLE_EQ(static_cast<double>(slot["tempo"]), 72.5);
  ASSERT_EQ(slot["tags"].size(), 2);
  EXPECT_EQ(slot["tags"][1].toString(), juce::String("clean"));

  rig.must("tempo.set", R"({"bpm":120})");
  rig.must("preset.loadSlot", R"({"slot":1})");
  EXPECT_DOUBLE_EQ(rig.proc.getTempo(), 72.5);
  rig.must("preset.rename", "{\"id\":" + quoted(slot["id"].toString()) + ",\"name\":\"Slow One\"}");
  rig.must("preset.moveSlot", R"({"from":1,"to":6})");
  rig.must("preset.duplicateSlot", R"({"slot":6})");
  // Saving the rig over the preset keeps them too: the rig does not know
  // them, the file does.
  rig.setParam(delay, "mix", 0.42);
  rig.must("preset.saveToSlot", R"({"slot":6})");
  rig.must("preset.save", R"({"name":"Slow One"})");
  for (const int number : {6, 7}) {
    slot = slotAt(rig.must("setlist.get"), number);
    EXPECT_DOUBLE_EQ(static_cast<double>(slot["tempo"]), 72.5) << number;
    EXPECT_EQ(slot["tags"].size(), 2) << number;
  }
  // Still in the header, before the chain, and the save is stable.
  const juce::File saved = rig.setlistFolder("My Presets").getChildFile("006 Slow One.json");
  const juce::String savedText = saved.loadFileAsString();
  EXPECT_LT(savedText.indexOf("\"tempo\": 72.5"), savedText.indexOf("\"chain\": {"));
  EXPECT_GT(savedText.indexOf("\"tempo\": 72.5"), 0);
  rig.must("preset.save", R"({"name":"Slow One"})");
  EXPECT_EQ(saved.loadFileAsString(), savedText);
}

// ---------------------------------------------------------------------------
// The render tool
// ---------------------------------------------------------------------------

namespace {

bool writeTestWav(const juce::File& file, double seconds) {
  const int frames = static_cast<int>(seconds * kRate);
  juce::AudioBuffer<float> buffer(2, frames);
  std::uint32_t lcg = 0x13572468u;
  for (int i = 0; i < frames; ++i) {
    lcg = lcg * 1664525u + 1013904223u;
    const float noise = static_cast<float>(lcg >> 8) / 8388608.0f - 1.0f;
    const float t = static_cast<float>(i) / static_cast<float>(kRate);
    const float tone = std::sin(2.0f * juce::MathConstants<float>::pi * 196.0f * t) * std::exp(-3.0f * t);
    buffer.setSample(0, i, 0.2f * tone + 0.02f * noise);
    buffer.setSample(1, i, 0.17f * tone - 0.02f * noise);
  }
  file.deleteFile();
  std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
  if (stream == nullptr)
    return false;
  juce::WavAudioFormat wav;
  auto writer = wav.createWriterFor(
      stream, juce::AudioFormatWriterOptions{}
                  .withSampleRate(kRate)
                  .withNumChannels(2)
                  .withBitsPerSample(32)
                  .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
  return writer != nullptr && writer->writeFromAudioSampleBuffer(buffer, 0, frames);
}

// Runs PedaleiraRender with `args`; its exit code, or -1 if it did not run.
int runRenderTool(const juce::StringArray& args, juce::String* output = nullptr) {
  juce::StringArray command{T3K_RENDER_TOOL_FILE};
  command.addArray(args);
  juce::ChildProcess tool;
  if (!tool.start(command))
    return -1;
  const juce::String text = tool.readAllProcessOutput();
  if (output != nullptr)
    *output = text;
  if (!tool.waitForProcessToFinish(180000))
    return -1;
  return static_cast<int>(tool.getExitCode());
}

}  // namespace

TEST(LibraryRenderToolTest, AJsonPresetRendersBitForBitLikeItsT3kPreset) {
  ASSERT_TRUE(juce::File(T3K_RENDER_TOOL_FILE).existsAsFile())
      << "PedaleiraRender was not built at " << T3K_RENDER_TOOL_FILE;
  TempDir scratch("render");
  const juce::File input = scratch.dir.getChildFile("in.wav");
  ASSERT_TRUE(writeTestWav(input, 1.0));

  // A rig with a capture, an IR, effects with tails and scenes, saved into a
  // library and exported as the .t3kpreset the JSON preset "came from".
  // The rig's models come from files dropped in, which the engine keeps in
  // its local-model stash; a scratch stash here, removed before anything is
  // rendered, so the tool can only find them where a preset says they are.
  TempDir stash("render-stash");
  const juce::File previousStash = TONE3000Processor::setLocalModelsDirForTesting(stash.dir);
  const juce::File store = scratch.dir.getChildFile("store");
  const juce::File t3k = scratch.dir.getChildFile("old").getChildFile("Render Me.t3kpreset");
  const juce::File single = scratch.dir.getChildFile("single").getChildFile("Render Me.json");
  const juce::File beside = scratch.dir.getChildFile("beside").getChildFile("Render Me.json");
  juce::File inLibrary;
  juce::String setlistId;
  {
    LibraryRig rig(store);
    rig.addTone("a2-amp-test.nam");
    rig.addTone("cab-ir-test.wav");
    const juce::String delay = rig.addKind("delay.digital");
    const juce::String reverb = rig.addKind("reverb.plate");
    rig.settle();
    dressWithScenesAndStomps(rig, delay, reverb, {});
    rig.must("block.setParam", "{\"blockId\":" + quoted(reverb) + ",\"param\":\"enabled\",\"value\":1}");
    setFaceplate(rig);
    rig.settle();
    setlistId = rig.must("setlist.create", R"({"name":"Renders","select":true})")["id"].toString();
    const juce::String id = rig.must("preset.saveToSlot", R"({"slot":12,"name":"Render Me"})")["id"].toString();
    inLibrary = rig.setlistFolder("Renders").getChildFile("012 Render Me.json");
    ASSERT_TRUE(inLibrary.existsAsFile());
    for (const auto& [file, embed] :
         {std::pair<juce::File, bool>{t3k, false}, {single, true}, {beside, false}})
      ASSERT_TRUE(rig.proc.exportPresetJson(id, file, embed).wasOk()) << file.getFullPathName();
  }
  TONE3000Processor::setLocalModelsDirForTesting(previousStash);
  ASSERT_TRUE(stash.dir.deleteRecursively());
  const juce::File libraryRoot = store.getChildFile("Library");
  const juce::MemoryBlock libraryState = fileBytes(libraryRoot.getChildFile("library.json"));

  const auto render = [&](const juce::String& name, const juce::StringArray& rigArgs) {
    const juce::File out = scratch.dir.getChildFile(name + ".wav");
    juce::StringArray args = rigArgs;
    args.addArray({"--in", input.getFullPathName(), "--out", out.getFullPathName(), "--tail", "0.5"});
    juce::String output;
    EXPECT_EQ(runRenderTool(args, &output), 0) << name << ": " << output;
    return out;
  };

  const juce::File reference = render("from-t3kpreset", {"--preset", t3k.getFullPathName()});
  ASSERT_TRUE(reference.existsAsFile());
  ASSERT_GT(reference.getSize(), 100000);
  const juce::MemoryBlock expected = fileBytes(reference);
  // Not silence: the comparison below means something.
  {
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader(
        wav.createReaderFor(reference.createInputStream().release(), true));
    ASSERT_NE(reader, nullptr);
    juce::AudioBuffer<float> audio(2, static_cast<int>(reader->lengthInSamples));
    reader->read(&audio, 0, audio.getNumSamples(), 0, true, true);
    EXPECT_GT(audio.getMagnitude(0, audio.getNumSamples()), 0.01f);
  }

  // The JSON preset, wherever its assets are: in the file, beside it, or in
  // the library it sits in.
  EXPECT_TRUE(fileBytes(render("from-json-single", {"--preset", single.getFullPathName()})) == expected);
  EXPECT_TRUE(fileBytes(render("from-json-beside", {"--preset", beside.getFullPathName()})) == expected);
  EXPECT_TRUE(fileBytes(render("from-json-in-library", {"--preset", inLibrary.getFullPathName()})) == expected);

  // --library: the preset loaded out of the library by a script.
  const juce::File script = scratch.dir.getChildFile("load.json");
  writeText(script, "[{\"command\":\"setlist.select\",\"args\":{\"id\":" + quoted(setlistId) +
                        "}},\n {\"command\":\"preset.loadSlot\",\"args\":{\"slot\":12}}]\n");
  const juce::File stateFile = scratch.dir.getChildFile("state.json");
  EXPECT_TRUE(fileBytes(render("from-library",
                               {"--library", libraryRoot.getFullPathName(), "--run",
                                script.getFullPathName(), "--state-out", stateFile.getFullPathName()})) ==
              expected);
  const juce::var state = juce::JSON::parse(stateFile.loadFileAsString());
  EXPECT_EQ(state["library"]["root"].toString(), libraryRoot.getFullPathName());
  EXPECT_EQ(static_cast<int>(state["library"]["current"]["slot"]), 12);
  EXPECT_EQ(state["chain"]["preset"]["name"].toString(), juce::String("Render Me"));

  // Without --library the tool's library is an empty scratch one: the same
  // script finds no such setlist, and a script that saves leaves nothing in
  // a library it was not pointed at.
  juce::String output;
  EXPECT_EQ(runRenderTool({"--run", script.getFullPathName(), "--in", input.getFullPathName(), "--out",
                           scratch.dir.getChildFile("never.wav").getFullPathName()},
                          &output),
            1);
  EXPECT_TRUE(output.contains("not_found")) << output;
  const juce::File saving = scratch.dir.getChildFile("save.json");
  writeText(saving, R"([{"command":"chain.addBlock","args":{"kind":"utility.gain"}},
                        {"command":"preset.save","args":{"name":"Scratch Only"}},
                        {"command":"setlist.create","args":{"name":"Scratch Setlist"}}])");
  EXPECT_EQ(runRenderTool({"--run", saving.getFullPathName(), "--in", input.getFullPathName(), "--out",
                           scratch.dir.getChildFile("scratch.wav").getFullPathName()},
                          &output),
            0)
      << output;
  EXPECT_FALSE(libraryRoot.getChildFile("Setlists").getChildFile("Scratch Setlist").exists());
  EXPECT_FALSE(libraryRoot.getChildFile("Setlists").getChildFile("My Presets").exists());
  // And with --library it is written like the app would write it.
  EXPECT_EQ(runRenderTool({"--library", libraryRoot.getFullPathName(), "--run", saving.getFullPathName(),
                           "--in", input.getFullPathName(), "--out",
                           scratch.dir.getChildFile("kept.wav").getFullPathName()},
                          &output),
            0)
      << output;
  EXPECT_TRUE(libraryRoot.getChildFile("Setlists").getChildFile("Scratch Setlist").isDirectory());
  EXPECT_TRUE(libraryRoot.getChildFile("Setlists")
                  .getChildFile("Renders")
                  .getChildFile("001 Scratch Only.json")
                  .existsAsFile());
  (void)libraryState;

  // A preset that is not one, and a JSON preset whose model is nowhere.
  const juce::File bogus = scratch.dir.getChildFile("bogus.json");
  writeText(bogus, R"({"format":"pedaleira.preset","version":1,"name":"No chain"})");
  EXPECT_EQ(runRenderTool({"--preset", bogus.getFullPathName(), "--in", input.getFullPathName(), "--out",
                           scratch.dir.getChildFile("bogus.wav").getFullPathName()},
                          &output),
            1);
  EXPECT_TRUE(output.contains("not a valid preset file")) << output;
  const juce::File orphan = scratch.dir.getChildFile("orphan").getChildFile("Render Me.json");
  ASSERT_TRUE(orphan.getParentDirectory().createDirectory());
  ASSERT_TRUE(beside.copyFileTo(orphan));
  EXPECT_EQ(runRenderTool({"--preset", orphan.getFullPathName(), "--in", input.getFullPathName(), "--out",
                           scratch.dir.getChildFile("orphan.wav").getFullPathName()},
                          &output),
            1);
  EXPECT_TRUE(output.contains("failed to load")) << output;
}

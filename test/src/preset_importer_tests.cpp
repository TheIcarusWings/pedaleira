// The preset importer (plugin/src/PresetImporter.cpp, docs/import.md): a
// foreign preset and a mapping table in, a grid preset and a report out; and
// the preset.importForeign command that puts the result in the library.
//
// What has to hold:
//   - a mapped block gets the kind the table names and its parameters
//     converted (linear, scale and offset, map), clamped and reported when
//     out of range, with the table's defaults underneath,
//   - a model the table does not know falls back to its category's kind at
//     defaults (approximated), or is left empty (unmapped) when the category
//     has none,
//   - an amp, a cab or a capture is never mapped: the slot stays empty and
//     the report says to pick a capture or an IR,
//   - rows and positions keep their layout, splits are placed where the grid
//     rules allow and flattened with a note where they do not,
//   - scenes carry block states and parameter values, and the report names
//     what could not be carried,
//   - bad input fails with a reason, the output is the canonical preset
//     JSON, the same bytes every time,
//   - the built-in table names kinds and parameters this build has,
//   - the command imports into the library, the result loads, and its paths
//     meet the same rules as every other path argument.
#include "EngineCommands.h"
#include "PresetImporter.h"
#include "PresetJson.h"
#include "Processor.h"
#include "agent_test_helpers.h"
#include "chain_test_helpers.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

namespace importer = t3k::importer;
namespace presetjson = t3k::presetjson;

juce::var parseJson(const char* text) {
  juce::var value;
  const juce::Result parsed = juce::JSON::parse(text, value);
  EXPECT_TRUE(parsed.wasOk()) << parsed.getErrorMessage() << "\n" << text;
  return value;
}

// A small table over real kinds, with every kind of transform.
const char* kTestMapping = R"({
  "source": "test-source",
  "version": 1,
  "models": {
    "Echo": {
      "kind": "delay.simple",
      "verified": true,
      "notes": "test",
      "params": {
        "TIME": { "param": "timeMs", "unit": "ms", "transform": "linear" },
        "REPEATS": { "param": "feedback", "transform": { "scale": 100, "offset": 0 } },
        "LEVEL": { "param": "mix", "transform": { "scale": 0.01 } },
        "SYNC": { "param": "sync", "transform": { "map": { "off": 0, "on": 1 } } },
        "TONE": { "param": "toneHz", "transform": { "scale": 1000, "offset": 500 } }
      },
      "defaults": { "feedback": 20 }
    },
    "Squash": {
      "kind": "dynamics.compressor",
      "params": {
        "THRESH": { "param": "thresholdDb" },
        "DETECTOR": { "param": "detector", "transform": { "map": { "peak": 0, "rms": 1 } } },
        "BOGUS": { "param": "noSuchParameter" }
      }
    },
    "Big Hall": {
      "kind": "reverb.hall",
      "params": { "SIZE": { "param": "size" }, "DECAY": { "param": "decayS" } }
    },
    "Nothing": { "kind": null },
    "Future": { "kind": "nonexistent.kind" },
    "Wah (M) ": { "kind": "filter.wah" }
  },
  "categories": {
    "Delay": "delay.digital",
    "Reverb": "reverb.hall",
    "Dirt": null,
    "Amps": { "role": "amp" },
    "Cabs": { "role": "cab" },
    "Captures": { "role": "capture" }
  }
})";

// ---- walking the preset JSON ----

juce::var childNamed(const juce::var& node, const char* name, int nth = 0) {
  if (const auto* children = node["children"].getArray())
    for (const auto& child : *children)
      if (child["node"].toString() == name && nth-- == 0)
        return child;
  return {};
}

juce::var gridOf(const juce::var& preset) { return childNamed(preset["chain"], "Grid"); }

juce::var rowNode(const juce::var& preset, int row) {
  const juce::var grid = gridOf(preset);
  if (const auto* children = grid["children"].getArray())
    for (const auto& child : *children)
      if (child["node"].toString() == "Row" && static_cast<int>(child["index"]) == row)
        return child;
  return {};
}

juce::var slotNode(const juce::var& preset, int row, int position) {
  const juce::var slots = childNamed(rowNode(preset, row), "Slots");
  return slots["children"][position];
}

bool isBlock(const juce::var& slot) { return slot["type"].toString() == "kind"; }

juce::String kindOf(const juce::var& slot) {
  return childNamed(slot, "KindState")["kind"].toString();
}

// A kind parameter's value in a slot, void when the node has none.
juce::var kindParam(const juce::var& slot, const char* id) {
  const juce::var state = childNamed(slot, "KindState");
  if (const auto* children = state["children"].getArray())
    for (const auto& child : *children)
      if (child["node"].toString() == "Param" && child["id"].toString() == id)
        return child["value"];
  return {};
}

int countIn(const juce::var& list) { return list.isArray() ? list.size() : 0; }

// The entry of a report list for a block, by row and position.
juce::var entryAt(const juce::var& list, int row, int position, const char* key = nullptr,
                  const char* value = nullptr) {
  if (const auto* items = list.getArray())
    for (const auto& item : *items)
      if (static_cast<int>(item["row"]) == row && static_cast<int>(item["position"]) == position &&
          (key == nullptr || item[key].toString() == value))
        return item;
  return {};
}

bool noteContains(const juce::var& report, const char* text) {
  if (const auto* notes = report["notes"].getArray())
    for (const auto& note : *notes)
      if (note.toString().contains(text))
        return true;
  return false;
}

struct Converted {
  importer::ImportResult result;
  juce::var preset() const { return result.preset; }
  juce::var report() const { return result.report; }
};

Converted convert(const char* foreignJson, const char* mappingJson = kTestMapping) {
  Converted out;
  out.result = importer::convertForeignPreset(parseJson(foreignJson), parseJson(mappingJson));
  EXPECT_TRUE(out.result.ok) << out.result.error;
  return out;
}

juce::String failureOf(const char* foreignJson, const char* mappingJson = kTestMapping) {
  const importer::ImportResult result =
      importer::convertForeignPreset(parseJson(foreignJson), parseJson(mappingJson));
  EXPECT_FALSE(result.ok);
  return result.error;
}

// A foreign preset with the given rows body (a JSON array) and optional
// extra top-level members (leading comma included).
juce::String foreignWith(const juce::String& rows, const juce::String& extra = {}) {
  return R"({"format":"pedaleira-foreign-preset","version":1,"source":"test-source",
             "name":"Test","rows":)" +
         rows + extra + "}";
}

Converted convertText(const juce::String& text, const char* mappingJson = kTestMapping) {
  Converted out;
  out.result = importer::convertForeignPreset(parseJson(text.toRawUTF8()), parseJson(mappingJson));
  EXPECT_TRUE(out.result.ok) << out.result.error;
  return out;
}

// ####################
// MAPPED BLOCKS
// ####################

TEST(PresetImporterTest, MappedBlocksGetTheirKindAndConvertedParameters) {
  const Converted c = convertText(foreignWith(R"([
    {"index":0,"blocks":[
      {"position":1,"model":"Echo","category":"Delay","enabled":true,"params":{
        "TIME":{"value":420,"unit":"ms"},
        "REPEATS":{"value":0.5},
        "LEVEL":{"value":40,"unit":"%"},
        "SYNC":{"value":"on"},
        "TONE":{"value":3.5},
        "UNKNOWN":{"value":1}}},
      {"position":4,"model":"Squash","category":"Compressor","enabled":false,"params":{
        "THRESH":{"value":-30,"unit":"dB"},"DETECTOR":{"value":"RMS"}}}]}])"));
  ASSERT_TRUE(c.result.ok);
  const juce::var preset = c.preset();

  const juce::var echo = slotNode(preset, 0, 1);
  ASSERT_TRUE(isBlock(echo));
  EXPECT_EQ(kindOf(echo), "delay.simple");
  EXPECT_NEAR(static_cast<double>(kindParam(echo, "timeMs")), 420.0, 1e-3);
  EXPECT_NEAR(static_cast<double>(kindParam(echo, "feedback")), 50.0, 1e-3) << "scale 100";
  EXPECT_NEAR(static_cast<double>(kindParam(echo, "toneHz")), 4000.0, 1e-3) << "scale and offset";
  EXPECT_NEAR(static_cast<double>(kindParam(echo, "sync")), 1.0, 1e-6) << "map";
  EXPECT_NEAR(static_cast<double>(echo["mix"]), 0.4, 1e-6) << "the common mix";
  EXPECT_TRUE(static_cast<bool>(echo["enabled"]));

  const juce::var squash = slotNode(preset, 0, 4);
  EXPECT_EQ(kindOf(squash), "dynamics.compressor");
  EXPECT_NEAR(static_cast<double>(kindParam(squash, "thresholdDb")), -30.0, 1e-3);
  EXPECT_NEAR(static_cast<double>(kindParam(squash, "detector")), 1.0, 1e-6) << "map is not case sensitive";
  EXPECT_FALSE(static_cast<bool>(squash["enabled"]));

  // Every other slot of the row is an empty slot, and the row is full length.
  EXPECT_EQ(slotNode(preset, 0, 0)["type"].toString(), "insert");
  EXPECT_EQ(slotNode(preset, 0, 7)["type"].toString(), "insert");
  EXPECT_EQ(childNamed(rowNode(preset, 0), "Slots")["children"].size(), 8);

  const juce::var report = c.report();
  EXPECT_EQ(countIn(report["mapped"]), 2);
  const juce::var mappedEcho = entryAt(report["mapped"], 0, 1);
  EXPECT_EQ(mappedEcho["kind"].toString(), "delay.simple");
  EXPECT_EQ(mappedEcho["model"].toString(), "Echo");
  EXPECT_TRUE(static_cast<bool>(mappedEcho["verified"]));
  EXPECT_FALSE(static_cast<bool>(entryAt(report["mapped"], 0, 4)["verified"]));
  EXPECT_EQ(countIn(report["unmapped"]), 0);

  // The parameter the table does not know is named, with its value.
  const juce::var dropped = entryAt(report["droppedParams"], 0, 1, "param", "UNKNOWN");
  ASSERT_TRUE(dropped.isObject());
  EXPECT_EQ(static_cast<int>(dropped["value"]), 1);
  EXPECT_TRUE(dropped["reason"].toString().contains("no entry"));
}

TEST(PresetImporterTest, TheTablesDefaultsLieUnderTheParameters) {
  const Converted c = convertText(foreignWith(R"([
    {"index":0,"blocks":[{"position":0,"model":"Echo","category":"Delay","params":{}}]}])"));
  const juce::var echo = slotNode(c.preset(), 0, 0);
  EXPECT_NEAR(static_cast<double>(kindParam(echo, "feedback")), 20.0, 1e-3) << "the table's default";
  // Not mentioned anywhere: the kind's own default (delay.simple: 6000 Hz).
  EXPECT_NEAR(static_cast<double>(kindParam(echo, "toneHz")), 6000.0, 1e-3);
  // The common mix starts where the kind starts it.
  EXPECT_NEAR(static_cast<double>(echo["mix"]), 0.35, 1e-6);
}

TEST(PresetImporterTest, OutOfRangeValuesAreClampedAndReported) {
  const Converted c = convertText(foreignWith(R"([
    {"index":1,"blocks":[
      {"position":2,"model":"Echo","category":"Delay","params":{
        "REPEATS":{"value":1.2},"TIME":{"value":99999},"LEVEL":{"value":250}}}]}])"));
  const juce::var echo = slotNode(c.preset(), 1, 2);
  EXPECT_NEAR(static_cast<double>(kindParam(echo, "feedback")), 95.0, 1e-3) << "120 to the top of 0..95";
  EXPECT_NEAR(static_cast<double>(kindParam(echo, "timeMs")), 2000.0, 1e-3);
  EXPECT_NEAR(static_cast<double>(echo["mix"]), 1.0, 1e-6) << "2.5 to 1";

  const juce::var approximated = c.report()["approximated"];
  ASSERT_EQ(countIn(approximated), 3);
  const juce::var feedback = entryAt(approximated, 1, 2, "param", "REPEATS");
  ASSERT_TRUE(feedback.isObject());
  EXPECT_EQ(feedback["type"].toString(), "param");
  EXPECT_EQ(feedback["ourParam"].toString(), "feedback");
  EXPECT_NEAR(static_cast<double>(feedback["from"]), 120.0, 1e-3);
  EXPECT_NEAR(static_cast<double>(feedback["to"]), 95.0, 1e-3);
  EXPECT_TRUE(feedback["reason"].toString().contains("clamped"));
  // The block itself still counts as mapped.
  EXPECT_EQ(countIn(c.report()["mapped"]), 1);
}

TEST(PresetImporterTest, ValuesThatCannotBeConvertedAreDroppedWithTheReason) {
  const Converted c = convertText(foreignWith(R"([
    {"index":0,"blocks":[
      {"position":0,"model":"Echo","category":"Delay","params":{
        "SYNC":{"value":"sometimes"},"TIME":{"value":"slow"}}},
      {"position":1,"model":"Squash","category":"Compressor","params":{"BOGUS":{"value":3}}}]}])"));
  const juce::var dropped = c.report()["droppedParams"];
  EXPECT_TRUE(entryAt(dropped, 0, 0, "param", "SYNC")["reason"].toString().contains("not in the table's map"));
  EXPECT_TRUE(entryAt(dropped, 0, 0, "param", "TIME")["reason"].toString().contains("not a number"));
  EXPECT_TRUE(entryAt(dropped, 0, 1, "param", "BOGUS")["reason"].toString().contains("noSuchParameter"));
  // The block is placed at its defaults all the same.
  EXPECT_EQ(kindOf(slotNode(c.preset(), 0, 0)), "delay.simple");
  EXPECT_NEAR(static_cast<double>(kindParam(slotNode(c.preset(), 0, 0), "timeMs")),
              350.0, 1e-3) << "the kind's default time";
}

TEST(PresetImporterTest, AUnitThatDiffersFromTheTablesIsNotConverted) {
  const Converted c = convertText(foreignWith(R"([
    {"index":0,"blocks":[{"position":0,"model":"Echo","category":"Delay","params":{
      "TIME":{"value":0.5,"unit":"s"}}}]}])"));
  const juce::var dropped = entryAt(c.report()["droppedParams"], 0, 0, "param", "TIME");
  ASSERT_TRUE(dropped.isObject());
  EXPECT_TRUE(dropped["reason"].toString().contains("expects ms"));
  EXPECT_NEAR(static_cast<double>(kindParam(slotNode(c.preset(), 0, 0), "timeMs")), 350.0, 1e-3);
}

TEST(PresetImporterTest, ModelNamesMatchWithoutCaseMarksOrMonoStereoSuffix) {
  const Converted c = convertText(foreignWith(R"([
    {"index":0,"blocks":[
      {"position":0,"model":"BIG-HALL (ST) ","category":"Reverb","params":{}},
      {"position":1,"model":"big hall","category":"Reverb","params":{}},
      {"position":2,"model":"Wah","category":"Wah","params":{}}]}])"));
  EXPECT_EQ(kindOf(slotNode(c.preset(), 0, 0)), "reverb.hall");
  EXPECT_EQ(kindOf(slotNode(c.preset(), 0, 1)), "reverb.hall");
  EXPECT_EQ(kindOf(slotNode(c.preset(), 0, 2)), "filter.wah") << "a table key may carry the suffix";
  EXPECT_EQ(countIn(c.report()["mapped"]), 3);
  EXPECT_EQ(countIn(c.report()["approximated"]), 0);
}

// ####################
// UNKNOWN AND UNMAPPABLE MODELS
// ####################

TEST(PresetImporterTest, AnUnknownModelFallsBackToItsCategoryOrIsLeftEmpty) {
  const Converted c = convertText(foreignWith(R"([
    {"index":0,"blocks":[
      {"position":0,"model":"Mystery Echo","category":"Delay","enabled":false,"params":{"TIME":{"value":900}}},
      {"position":1,"model":"Fuzz Thing","category":"Dirt","params":{}},
      {"position":2,"model":"Alien","category":"Never Heard Of It","params":{}}]}])"));
  const juce::var preset = c.preset();

  // With a fallback: that kind at its defaults, bypassed as the source had it.
  const juce::var fallback = slotNode(preset, 0, 0);
  ASSERT_TRUE(isBlock(fallback));
  EXPECT_EQ(kindOf(fallback), "delay.digital");
  EXPECT_FALSE(static_cast<bool>(fallback["enabled"]));
  EXPECT_NEAR(static_cast<double>(kindParam(fallback, "feedback")), 35.0, 1e-3) << "kind default, not the source's values";
  const juce::var approximated = entryAt(c.report()["approximated"], 0, 0);
  ASSERT_TRUE(approximated.isObject());
  EXPECT_EQ(approximated["type"].toString(), "block");
  EXPECT_EQ(approximated["kind"].toString(), "delay.digital");
  EXPECT_EQ(approximated["model"].toString(), "Mystery Echo");
  EXPECT_EQ(approximated["category"].toString(), "Delay");

  // A category with no kind, and a category the table has never heard of:
  // the slot stays empty and the report says so.
  EXPECT_EQ(slotNode(preset, 0, 1)["type"].toString(), "insert");
  EXPECT_EQ(slotNode(preset, 0, 2)["type"].toString(), "insert");
  const juce::var unmapped = c.report()["unmapped"];
  EXPECT_EQ(countIn(unmapped), 2);
  EXPECT_TRUE(entryAt(unmapped, 0, 1)["reason"].toString().contains("no fallback"));
  EXPECT_TRUE(entryAt(unmapped, 0, 2)["reason"].toString().contains("no entry"));
  EXPECT_EQ(entryAt(unmapped, 0, 1)["model"].toString(), "Fuzz Thing");
  EXPECT_EQ(countIn(c.report()["mapped"]), 0);
}

TEST(PresetImporterTest, AmpsCabsAndCapturesAreNeverMapped) {
  const Converted c = convertText(foreignWith(R"([
    {"index":0,"blocks":[
      {"position":0,"model":"Brit 2203","category":"Amps","params":{"GAIN":{"value":5}}},
      {"position":1,"model":"4x12","category":"Cabs","params":{}},
      {"position":2,"model":"My Capture","category":"Captures","params":{"file_name":{"value":"x.nam"}}},
      {"position":3,"model":"Nothing","category":"Delay","params":{}}]}])"));
  const juce::var preset = c.preset();
  for (int position = 0; position < 4; ++position)
    EXPECT_EQ(slotNode(preset, 0, position)["type"].toString(), "insert") << position;

  const juce::var unmapped = c.report()["unmapped"];
  ASSERT_EQ(countIn(unmapped), 4);
  const char* roles[] = {"amp", "cab", "capture"};
  for (int position = 0; position < 3; ++position) {
    const juce::var entry = entryAt(unmapped, 0, position);
    EXPECT_EQ(entry["role"].toString(), roles[position]);
    EXPECT_TRUE(entry["reason"].toString().contains("pick a NAM capture or an IR")) << entry["reason"].toString();
  }
  // A model the table names but maps to nothing is unmapped too, no role.
  EXPECT_FALSE(entryAt(unmapped, 0, 3).hasProperty("role"));
  // None of their parameters are reported as dropped: there is nothing to drop them from.
  EXPECT_EQ(countIn(c.report()["droppedParams"]), 0);
  EXPECT_EQ(countIn(c.report()["mapped"]), 0);
}

TEST(PresetImporterTest, AKindThisBuildLacksIsUnmapped) {
  const Converted c = convertText(foreignWith(R"([
    {"index":0,"blocks":[{"position":0,"model":"Future","category":"Delay","params":{}}]}])"));
  EXPECT_EQ(slotNode(c.preset(), 0, 0)["type"].toString(), "insert");
  EXPECT_TRUE(entryAt(c.report()["unmapped"], 0, 0)["reason"].toString().contains("nonexistent.kind"));
}

TEST(PresetImporterTest, PositionsThatDoNotFitAreReported) {
  const Converted c = convertText(foreignWith(R"([
    {"index":0,"blocks":[
      {"position":8,"model":"Echo","category":"Delay"},
      {"position":-1,"model":"Echo","category":"Delay"},
      {"position":3,"model":"Echo","category":"Delay"},
      {"position":3,"model":"Squash","category":"Compressor"}]},
    {"index":4,"blocks":[{"position":0,"model":"Echo","category":"Delay"}]}])"));
  EXPECT_EQ(countIn(c.report()["mapped"]), 1);
  EXPECT_EQ(kindOf(slotNode(c.preset(), 0, 3)), "delay.simple") << "the first at a position keeps it";
  EXPECT_TRUE(entryAt(c.report()["unmapped"], 0, 3)["reason"].toString().contains("already"));
  EXPECT_TRUE(entryAt(c.report()["unmapped"], 0, 8)["reason"].toString().contains("does not fit"));
  EXPECT_TRUE(entryAt(c.report()["unmapped"], 0, -1)["reason"].toString().contains("does not fit"));
  EXPECT_TRUE(entryAt(c.report()["unmapped"], 4, 0)["reason"].toString().contains("outside the grid"));
  EXPECT_TRUE(noteContains(c.report(), "does not fit the grid"));
}

// ####################
// LAYOUT, ROUTING AND SPLITS
// ####################

TEST(PresetImporterTest, RowsAndPositionsKeepTheirLayout) {
  const Converted c = convertText(foreignWith(R"([
    {"index":0,"blocks":[{"position":0,"model":"Squash","category":"Compressor"},
                         {"position":7,"model":"Echo","category":"Delay"}]},
    {"index":2,"blocks":[{"position":5,"model":"Big Hall","category":"Reverb"}]},
    {"index":3,"blocks":[{"position":2,"model":"Wah","category":"Wah"}]}])",
                                     R"(,"routing":{"rows":[
    {"index":0,"input":{"type":"device","channel":1,"stereo":false},"output":{"type":"device","pair":1}},
    {"index":2,"input":{"type":"device","channel":2,"stereo":false},"output":{"type":"device","pair":2}},
    {"index":3,"input":{"type":"device","channel":1,"stereo":true},"output":{"type":"none"}}]})"));
  const juce::var preset = c.preset();
  EXPECT_EQ(preset["schemaVersion"].toString(), "2");
  EXPECT_EQ(preset["chain"]["layout"].toString(), "grid");
  const juce::var grid = gridOf(preset);
  EXPECT_EQ(static_cast<int>(grid["rows"]), 4);
  EXPECT_EQ(static_cast<int>(grid["cols"]), 8);
  EXPECT_EQ(static_cast<int>(grid["version"]), 1) << "no joins: readable by the first grid format";

  EXPECT_EQ(kindOf(slotNode(preset, 0, 0)), "dynamics.compressor");
  EXPECT_EQ(kindOf(slotNode(preset, 0, 7)), "delay.simple");
  EXPECT_EQ(kindOf(slotNode(preset, 2, 5)), "reverb.hall");
  EXPECT_EQ(kindOf(slotNode(preset, 3, 2)), "filter.wah");
  EXPECT_EQ(rowNode(preset, 1).isObject(), true);
  for (int position = 0; position < 8; ++position)
    EXPECT_EQ(slotNode(preset, 1, position)["type"].toString(), "insert");

  const juce::var in0 = childNamed(rowNode(preset, 0), "Input");
  EXPECT_EQ(in0["type"].toString(), "device");
  EXPECT_EQ(static_cast<int>(in0["channel"]), 0);
  EXPECT_FALSE(static_cast<bool>(in0["stereo"]));
  const juce::var out0 = childNamed(rowNode(preset, 0), "Output");
  EXPECT_EQ(out0["type"].toString(), "device");
  EXPECT_EQ(static_cast<int>(out0["pair"]), 0);
  EXPECT_EQ(static_cast<int>(childNamed(rowNode(preset, 2), "Input")["channel"]), 1);
  EXPECT_EQ(static_cast<int>(childNamed(rowNode(preset, 2), "Output")["pair"]), 1);
  EXPECT_TRUE(static_cast<bool>(childNamed(rowNode(preset, 3), "Input")["stereo"]));
  EXPECT_EQ(childNamed(rowNode(preset, 3), "Output")["type"].toString(), "none");
  // The row nobody described has neither.
  EXPECT_EQ(childNamed(rowNode(preset, 1), "Input")["type"].toString(), "none");
  EXPECT_EQ(childNamed(rowNode(preset, 1), "Output")["type"].toString(), "none");
}

TEST(PresetImporterTest, WithoutRoutingTheFirstRowGetsInputOneAndOutputOne) {
  const Converted c = convertText(foreignWith(R"([
    {"index":0,"blocks":[{"position":0,"model":"Echo","category":"Delay"}]}])"));
  EXPECT_EQ(childNamed(rowNode(c.preset(), 0), "Input")["type"].toString(), "device");
  EXPECT_EQ(childNamed(rowNode(c.preset(), 0), "Output")["type"].toString(), "device");
  EXPECT_EQ(childNamed(rowNode(c.preset(), 1), "Input")["type"].toString(), "none");
  EXPECT_TRUE(noteContains(c.report(), "no routing"));
}

TEST(PresetImporterTest, PortsWithNoCounterpartAreNotedAndFallBack) {
  const Converted c = convertText(foreignWith(R"([
    {"index":0,"blocks":[{"position":0,"model":"Echo","category":"Delay"}]}])",
                                     R"(,"routing":{"rows":[
    {"index":0,"input":{"type":"other","name":"RETURN_1"},"output":{"type":"other","name":"MULTIPLE"}}]})"));
  EXPECT_EQ(childNamed(rowNode(c.preset(), 0), "Input")["type"].toString(), "none");
  EXPECT_EQ(childNamed(rowNode(c.preset(), 0), "Output")["type"].toString(), "device");
  EXPECT_TRUE(noteContains(c.report(), "RETURN_1"));
  EXPECT_TRUE(noteContains(c.report(), "MULTIPLE"));
}

TEST(PresetImporterTest, ASplitIsPlacedWhereTheGridAllowsIt) {
  const Converted c = convertText(foreignWith(R"([
    {"index":0,"blocks":[{"position":0,"model":"Squash","category":"Compressor"},
                         {"position":6,"model":"Big Hall","category":"Reverb"}]},
    {"index":1,"blocks":[{"position":3,"model":"Echo","category":"Delay"}]}])",
                                     R"(,"routing":{
    "rows":[
      {"index":0,"input":{"type":"device","channel":1,"stereo":false},"output":{"type":"device","pair":1}},
      {"index":1,"input":{"type":"device","channel":2,"stereo":false},"output":{"type":"device","pair":2}}],
    "splits":[{"row":0,"splitAt":2,"mixAt":6}]})"));
  const juce::var preset = c.preset();
  const juce::var grid = gridOf(preset);
  EXPECT_EQ(static_cast<int>(grid["version"]), 2);
  EXPECT_EQ(static_cast<int>(grid["minReader"]), 2);

  const juce::var split = childNamed(grid, "Split");
  ASSERT_TRUE(split.isObject());
  EXPECT_EQ(static_cast<int>(split["upperRow"]), 0);
  EXPECT_EQ(static_cast<int>(split["splitCol"]), 2);
  EXPECT_EQ(static_cast<int>(split["mixCol"]), 6);
  EXPECT_EQ(split["type"].toString(), "ab");

  // The row below is fed by the split: no input of its own, and its signal
  // joins the upper row, so no output either.
  EXPECT_EQ(childNamed(rowNode(preset, 1), "Input")["type"].toString(), "none");
  EXPECT_EQ(childNamed(rowNode(preset, 1), "Output")["type"].toString(), "none");
  EXPECT_EQ(childNamed(rowNode(preset, 0), "Output")["type"].toString(), "device");
  EXPECT_EQ(kindOf(slotNode(preset, 1, 3)), "delay.simple");
  EXPECT_TRUE(noteContains(c.report(), "boundaries between slots"));
  EXPECT_TRUE(noteContains(c.report(), "input was dropped")) << "row 1 had an input of its own";
}

TEST(PresetImporterTest, ASplitWithoutRejoinKeepsTheLowerRowsOutput) {
  const Converted c = convertText(foreignWith(R"([
    {"index":2,"blocks":[{"position":0,"model":"Squash","category":"Compressor"}]},
    {"index":3,"blocks":[{"position":4,"model":"Echo","category":"Delay"}]}])",
                                     R"(,"routing":{
    "rows":[
      {"index":2,"input":{"type":"device","channel":1,"stereo":false},"output":{"type":"device","pair":1}},
      {"index":3,"input":{"type":"none"},"output":{"type":"device","pair":2}}],
    "splits":[{"row":2,"splitAt":1,"mixAt":null}]})"));
  const juce::var split = childNamed(gridOf(c.preset()), "Split");
  ASSERT_TRUE(split.isObject());
  EXPECT_EQ(static_cast<int>(split["upperRow"]), 2);
  EXPECT_EQ(static_cast<int>(split["mixCol"]), -1);
  EXPECT_EQ(childNamed(rowNode(c.preset(), 3), "Output")["type"].toString(), "device");
  EXPECT_EQ(static_cast<int>(childNamed(rowNode(c.preset(), 3), "Output")["pair"]), 1);
}

TEST(PresetImporterTest, ASplitTheGridCannotHoldIsFlattenedWithANote) {
  const Converted c = convertText(foreignWith(R"([
    {"index":0,"blocks":[{"position":0,"model":"Squash","category":"Compressor"}]}])",
                                     R"(,"routing":{"rows":[
      {"index":0,"input":{"type":"device","channel":1,"stereo":false},"output":{"type":"device","pair":1}}],
    "splits":[
      {"row":3,"splitAt":1,"mixAt":4},
      {"row":0,"splitAt":9,"mixAt":4},
      {"row":0,"splitAt":4,"mixAt":2},
      {"row":0,"splitAt":1,"mixAt":9}]})"));
  EXPECT_FALSE(childNamed(gridOf(c.preset()), "Split").isObject());
  EXPECT_EQ(static_cast<int>(gridOf(c.preset())["version"]), 1);
  int flattened = 0;
  for (const auto& note : *c.report()["notes"].getArray())
    flattened += note.toString().contains("Flattened") ? 1 : 0;
  EXPECT_EQ(flattened, 4);
}

TEST(PresetImporterTest, OnlyOneSplitPerRowAndNestedOnesStayInside) {
  const Converted c = convertText(foreignWith(R"([
    {"index":0,"blocks":[{"position":0,"model":"Squash","category":"Compressor"}]}])",
                                     R"(,"routing":{"splits":[
      {"row":0,"splitAt":1,"mixAt":6},
      {"row":0,"splitAt":2,"mixAt":4},
      {"row":1,"splitAt":0,"mixAt":4},
      {"row":1,"splitAt":2,"mixAt":5}]})"));
  const juce::var grid = gridOf(c.preset());
  int splits = 0;
  for (const auto& child : *grid["children"].getArray())
    splits += child["node"].toString() == "Split" ? 1 : 0;
  EXPECT_EQ(splits, 2) << "the second split of row 0 and the one reaching outside row 0's are dropped";
  EXPECT_TRUE(noteContains(c.report(), "only one split"));
  EXPECT_TRUE(noteContains(c.report(), "inside the split above"));
}

TEST(PresetImporterTest, ARowFeedIsKeptWhenTheTargetCanTakeIt) {
  const Converted c = convertText(foreignWith(R"([
    {"index":0,"blocks":[{"position":0,"model":"Squash","category":"Compressor"}]},
    {"index":1,"blocks":[{"position":0,"model":"Echo","category":"Delay"}]},
    {"index":2,"blocks":[{"position":0,"model":"Echo","category":"Delay"}]}])",
                                     R"(,"routing":{"rows":[
      {"index":0,"input":{"type":"device","channel":1,"stereo":false},"output":{"type":"row"}},
      {"index":1,"input":{"type":"none"},"output":{"type":"device","pair":1}},
      {"index":2,"input":{"type":"device","channel":2,"stereo":false},"output":{"type":"row"}}]})"));
  const juce::var preset = c.preset();
  const juce::var out0 = childNamed(rowNode(preset, 0), "Output");
  EXPECT_EQ(out0["type"].toString(), "row");
  EXPECT_EQ(static_cast<int>(out0["toRow"]), 1);
  EXPECT_EQ(static_cast<int>(gridOf(preset)["version"]), 2) << "a feed needs the newer grid format";
  // Row 2 feeds row 3, which takes no input of its own: a valid target too.
  EXPECT_EQ(childNamed(rowNode(preset, 2), "Output")["type"].toString(), "row");
}

TEST(PresetImporterTest, ARowFeedIntoARowWithItsOwnInputFallsBackToTheOutput) {
  const Converted c = convertText(foreignWith(R"([
    {"index":0,"blocks":[{"position":0,"model":"Squash","category":"Compressor"}]},
    {"index":1,"blocks":[{"position":0,"model":"Echo","category":"Delay"}]}])",
                                     R"(,"routing":{"rows":[
      {"index":0,"input":{"type":"device","channel":1,"stereo":false},"output":{"type":"row"}},
      {"index":1,"input":{"type":"device","channel":2,"stereo":false},"output":{"type":"device","pair":1}}]})"));
  EXPECT_EQ(childNamed(rowNode(c.preset(), 0), "Output")["type"].toString(), "device");
  EXPECT_TRUE(noteContains(c.report(), "feeds the row below"));
}

// ####################
// SCENES
// ####################

TEST(PresetImporterTest, ScenesCarryBlockStatesValuesAndNames) {
  const Converted c = convertText(foreignWith(R"([
    {"index":0,"blocks":[
      {"position":0,"model":"Echo","category":"Delay","enabled":true,"params":{
        "TIME":{"value":300,"unit":"ms"},"REPEATS":{"value":0.3}}},
      {"position":2,"model":"Big Hall","category":"Reverb","enabled":true,"params":{"SIZE":{"value":50}}},
      {"position":3,"model":"Brit","category":"Amps","enabled":true,"params":{}}]}])",
                                     R"(,"defaultScene":1,"scenes":[
    {"name":"Scene A","enabled":{"0:2":true},"params":{}},
    {"name":"Rhythm","enabled":{"0:2":false,"0:3":false},"params":{"0:0":{"TIME":600,"REPEATS":0.6,"TONE":2}}},
    {"name":"","enabled":{"0:2":true},"params":{"0:0":{"TIME":450,"SYNC":"on"},"0:1":{"X":1}}}]})"));
  ASSERT_TRUE(c.result.ok);
  const juce::var preset = c.preset();
  const juce::var scenes = childNamed(preset["chain"], "Scenes");
  ASSERT_TRUE(scenes.isObject());
  EXPECT_EQ(static_cast<int>(scenes["active"]), 1);
  ASSERT_EQ(scenes["children"].size(), 8);
  EXPECT_TRUE(childNamed(scenes["children"][0], "Block").isObject()) << "scene A has the hall's state";
  EXPECT_EQ(scenes["children"][1]["name"].toString(), "Rhythm");
  EXPECT_FALSE(scenes["children"][0].hasProperty("name")) << "a default name is not stored";
  EXPECT_FALSE(scenes["children"][2].hasProperty("name"));

  const juce::String hallId = slotNode(preset, 0, 2)["id"].toString();
  const juce::String echoId = slotNode(preset, 0, 0)["id"].toString();
  auto blockState = [&](int scene, const juce::String& id) -> int {
    if (const auto* children = scenes["children"][scene]["children"].getArray())
      for (const auto& child : *children)
        if (child["node"].toString() == "Block" && child["id"].toString() == id)
          return static_cast<bool>(child["enabled"]) ? 1 : 0;
    return -1;
  };
  EXPECT_EQ(blockState(0, hallId), 1);
  EXPECT_EQ(blockState(1, hallId), 0);
  EXPECT_EQ(blockState(2, hallId), 1);
  EXPECT_EQ(blockState(7, hallId), 1) << "a scene the file does not give keeps the block as it is";
  EXPECT_EQ(blockState(1, echoId), -1) << "a block whose switch never moves has no scene entry";

  auto valueOf = [&](int scene, const juce::String& id, const char* param) -> double {
    if (const auto* children = scenes["children"][scene]["children"].getArray())
      for (const auto& child : *children)
        if (child["node"].toString() == "Value" && child["blockId"].toString() == id &&
            child["param"].toString() == param)
          return static_cast<double>(child["value"]);
    return -1.0e9;
  };
  // The block holds the active scene (B): 600 ms, feedback 60, tone 2.5k.
  const juce::var echo = slotNode(preset, 0, 0);
  EXPECT_NEAR(static_cast<double>(kindParam(echo, "timeMs")), 600.0, 1e-3);
  EXPECT_NEAR(static_cast<double>(kindParam(echo, "feedback")), 60.0, 1e-3);
  EXPECT_NEAR(valueOf(0, echoId, "timeMs"), 300.0, 1e-3) << "scene A is the base value";
  EXPECT_NEAR(valueOf(1, echoId, "timeMs"), 600.0, 1e-3);
  EXPECT_NEAR(valueOf(2, echoId, "timeMs"), 450.0, 1e-3);
  EXPECT_NEAR(valueOf(7, echoId, "timeMs"), 300.0, 1e-3);
  EXPECT_NEAR(valueOf(1, echoId, "feedback"), 60.0, 1e-3);
  EXPECT_NEAR(valueOf(0, echoId, "feedback"), 30.0, 1e-3);
  EXPECT_NEAR(valueOf(1, echoId, "toneHz"), 2500.0, 1e-3);
  EXPECT_NEAR(valueOf(2, echoId, "sync"), 1.0, 1e-6);

  // The amp's on/off and the entry for a block that is not there cannot be
  // carried, and the report counts them.
  EXPECT_TRUE(noteContains(c.report(), "Scenes carried"));
  EXPECT_TRUE(noteContains(c.report(), "Scenes not carried"));
}

TEST(PresetImporterTest, SceneValuesThatCannotFollowScenesAreReported) {
  const Converted c = convertText(foreignWith(R"([
    {"index":0,"blocks":[{"position":0,"model":"Echo","category":"Delay","params":{}}]}])",
                                     R"(,"scenes":[
    {"name":"Scene A","enabled":{},"params":{}},
    {"name":"Scene B","enabled":{},"params":{"0:0":{"NOPE":3,"TIME":"slow"}}}]})"));
  const juce::var dropped = c.report()["droppedParams"];
  ASSERT_EQ(countIn(dropped), 2);
  EXPECT_EQ(static_cast<int>(entryAt(dropped, 0, 0, "param", "NOPE")["scene"]), 1);
  EXPECT_TRUE(entryAt(dropped, 0, 0, "param", "TIME")["reason"].toString().contains("scene value"));
  // Nothing varies, so no scene data at all.
  EXPECT_FALSE(childNamed(c.preset()["chain"], "Scenes").isObject());
}

TEST(PresetImporterTest, ApproximatedBlocksKeepTheirOnOffStatesInScenes) {
  const Converted c = convertText(foreignWith(R"([
    {"index":0,"blocks":[{"position":0,"model":"Mystery","category":"Delay","enabled":true}]}])",
                                     R"(,"scenes":[
    {"name":"A","enabled":{"0:0":true}},{"name":"B","enabled":{"0:0":false}}]})"));
  const juce::var scenes = childNamed(c.preset()["chain"], "Scenes");
  ASSERT_TRUE(scenes.isObject());
  const juce::String id = slotNode(c.preset(), 0, 0)["id"].toString();
  bool sawOff = false;
  for (const auto& child : *scenes["children"][1]["children"].getArray())
    if (child["id"].toString() == id)
      sawOff = !static_cast<bool>(child["enabled"]);
  EXPECT_TRUE(sawOff);
}

// ####################
// THE FILE AND THE OUTPUT
// ####################

TEST(PresetImporterTest, TheOutputIsTheCanonicalPresetJson) {
  const juce::String text = foreignWith(R"([
    {"index":0,"blocks":[{"position":0,"model":"Echo","category":"Delay","params":{"TIME":{"value":420}}}]}])",
                                        R"(,"tempo":96.5)");
  const Converted first = convertText(text);
  const Converted second = convertText(text);
  EXPECT_EQ(first.result.presetJson, second.result.presetJson) << "the same bytes every time";

  const juce::var preset = first.preset();
  EXPECT_EQ(preset["format"].toString(), "pedaleira.preset");
  EXPECT_EQ(static_cast<int>(preset["version"]), 1);
  EXPECT_EQ(preset["name"].toString(), "Test");
  EXPECT_NEAR(static_cast<double>(preset["tempo"]), 96.5, 1e-9);
  EXPECT_EQ(preset["importedFrom"].toString(), "test-source");
  EXPECT_TRUE(preset["params"].isObject());

  // It reads back as a preset, and writing that tree gives the same text.
  const presetjson::ReadResult read = presetjson::fromJson(first.result.presetJson, {});
  ASSERT_TRUE(read.preset.isValid()) << read.error;
  juce::String error;
  EXPECT_EQ(presetjson::toJson(read.preset, {}, error), first.result.presetJson) << error;
  const presetjson::Meta meta = presetjson::readMeta(first.result.presetJson);
  EXPECT_TRUE(meta.valid) << meta.problem;
  EXPECT_EQ(meta.name, "Test");
  ASSERT_TRUE(meta.tempo.has_value());
}

TEST(PresetImporterTest, AnUnnamedPresetGetsAName) {
  const Converted c = convertText(R"({"format":"pedaleira-foreign-preset","version":1,
      "source":"test-source","name":"  ","rows":[]})");
  EXPECT_EQ(c.preset()["name"].toString(), "Imported preset");
  EXPECT_EQ(countIn(c.report()["mapped"]), 0);
}

TEST(PresetImporterTest, BadInputFailsWithAClearReason) {
  EXPECT_TRUE(failureOf(R"([1,2])").contains("not a JSON object"));
  EXPECT_TRUE(failureOf(R"({"rows":[]})").contains("\"format\" is missing"));
  EXPECT_TRUE(failureOf(R"({"format":"pedaleira.preset","version":1,"source":"test-source","rows":[]})")
                  .contains("expected \"pedaleira-foreign-preset\""));
  EXPECT_TRUE(failureOf(R"({"format":"pedaleira-foreign-preset","source":"test-source","rows":[]})")
                  .contains("no \"version\""));
  const juce::String newer = failureOf(
      R"({"format":"pedaleira-foreign-preset","version":2,"source":"test-source","rows":[]})");
  EXPECT_TRUE(newer.contains("version 2")) << newer;
  EXPECT_TRUE(newer.contains("up to 1")) << newer;
  EXPECT_TRUE(failureOf(R"({"format":"pedaleira-foreign-preset","version":0,"source":"test-source","rows":[]})")
                  .contains("invalid"));
  EXPECT_TRUE(failureOf(R"({"format":"pedaleira-foreign-preset","version":1,"rows":[]})")
                  .contains("no \"source\""));
  EXPECT_TRUE(failureOf(R"({"format":"pedaleira-foreign-preset","version":1,"source":"test-source"})")
                  .contains("no \"rows\""));
  const juce::String other = failureOf(
      R"({"format":"pedaleira-foreign-preset","version":1,"source":"quad-cortex","rows":[]})");
  EXPECT_TRUE(other.contains("test-source") && other.contains("quad-cortex")) << other;
  EXPECT_EQ(importer::checkForeignPreset(parseJson(R"({"format":"x"})")).isNotEmpty(), true);
}

TEST(PresetImporterTest, BadMappingTablesFailWithAClearReason) {
  const char* foreign = R"({"format":"pedaleira-foreign-preset","version":1,"source":"test-source","rows":[]})";
  EXPECT_TRUE(failureOf(foreign, R"([])").contains("not a JSON object"));
  EXPECT_TRUE(failureOf(foreign, R"({"version":1})").contains("no \"source\""));
  EXPECT_TRUE(failureOf(foreign, R"({"source":"test-source"})").contains("no \"version\""));
  EXPECT_TRUE(failureOf(foreign, R"({"source":"test-source","version":2})").contains("version 2"));
  EXPECT_TRUE(failureOf(foreign, R"({"source":"test-source","version":1,"models":[]})")
                  .contains("must be an object"));
  EXPECT_TRUE(failureOf(foreign, R"({"source":"test-source","version":1,"categories":3})")
                  .contains("must be an object"));
  // An empty table is a table: everything is unmapped.
  const Converted empty = convert(
      R"({"format":"pedaleira-foreign-preset","version":1,"source":"test-source","rows":[
        {"index":0,"blocks":[{"position":0,"model":"X","category":"Y"}]}]})",
      R"({"source":"test-source","version":1})");
  EXPECT_EQ(countIn(empty.report()["unmapped"]), 1);
}

// ####################
// THE BUILT-IN TABLE
// ####################

TEST(PresetImporterTest, TheBuiltInTableNamesKindsAndParametersThisBuildHas) {
  const juce::StringArray sources = importer::builtInMappingSources();
  EXPECT_TRUE(sources.contains("quad-cortex"));
  const juce::var table = importer::builtInMapping("quad-cortex");
  ASSERT_TRUE(table.isObject());
  EXPECT_EQ(table["source"].toString(), "quad-cortex");
  EXPECT_TRUE(importer::builtInMapping("no-such-source").isVoid());

  auto expectKind = [](const juce::String& id, const juce::String& where) -> const BlockKindInfo* {
    const BlockKindInfo* info = BlockKindRegistry::instance().find(id);
    EXPECT_NE(info, nullptr) << where << ": kind " << id << " is not registered";
    if (info != nullptr)
      EXPECT_TRUE(info->addable) << where;
    return info;
  };

  int models = 0;
  for (const auto& model : table["models"].getDynamicObject()->getProperties()) {
    ++models;
    const juce::String where = "model " + model.name.toString();
    // Nothing is verified against a real unit yet.
    EXPECT_TRUE(model.value.hasProperty("verified")) << where;
    EXPECT_FALSE(static_cast<bool>(model.value["verified"])) << where;
    const BlockKindInfo* info = expectKind(model.value["kind"].toString(), where);
    if (info == nullptr)
      continue;
    if (const auto* params = model.value["params"].getDynamicObject()) {
      for (const auto& param : params->getProperties()) {
        const juce::String id = param.value["param"].toString();
        const bool common = id == "mix" || id == "inputGain" || id == "outputGain";
        EXPECT_TRUE(common || findBlockParam(info->params, id) >= 0)
            << where << ": " << param.name.toString() << " maps to " << id << ", which " << info->id
            << " does not have";
        EXPECT_TRUE(param.value.hasProperty("transform")) << where << ": " << param.name.toString();
      }
    }
    if (const auto* defaults = model.value["defaults"].getDynamicObject())
      for (const auto& value : defaults->getProperties())
        EXPECT_GE(findBlockParam(info->params, value.name.toString()), 0) << where;
  }
  EXPECT_GE(models, 20);

  for (const auto& category : table["categories"].getDynamicObject()->getProperties()) {
    const juce::String where = "category " + category.name.toString();
    if (category.value.isString())
      expectKind(category.value.toString(), where);
    else if (category.value.isObject())
      EXPECT_TRUE(category.value["role"].toString().isNotEmpty()) << where;
    else
      EXPECT_TRUE(category.value.isVoid()) << where;
  }
  // Amps, cabs and captures are explicitly never mapped.
  for (const char* amp : {"Guitar Amplifier", "Bass Amplifier", "Neural Capture"})
    EXPECT_TRUE(table["categories"][amp].isObject()) << amp;
}

TEST(PresetImporterTest, TheBuiltInTableMapsTheCategoriesWeHave) {
  const juce::var table = importer::builtInMapping("quad-cortex");
  const juce::var foreign = parseJson(R"({"format":"pedaleira-foreign-preset","version":1,
    "source":"quad-cortex","name":"QC","rows":[{"index":0,"blocks":[
      {"position":0,"model":"VCA Comp (M) ","category":"Compressor","enabled":true,
       "params":{"THRESHOLD":{"value":-24.5,"unit":"dB"}}},
      {"position":1,"model":"Digital Delay (ST) ","category":"Delay","enabled":true,
       "params":{"FEEDBACK":{"value":45,"unit":"%"},"MIX":{"value":30,"unit":"%"},"DELAY TIME":{"value":380,"unit":"ms"}}},
      {"position":2,"model":"Plate Lush","category":"Reverb","enabled":true,
       "params":{"PRE DELAY":{"value":40,"unit":"ms"},"MIX":{"value":25,"unit":"%"}}},
      {"position":3,"model":"Phaser","category":"Modulation","enabled":true,
       "params":{"SPEED":{"value":40},"DEPTH":{"value":65,"unit":"%"}}},
      {"position":4,"model":"Petrucci Wah","category":"Wah","enabled":true,"params":{}},
      {"position":5,"model":"Brit 2203","category":"Guitar Amplifier","enabled":true,"params":{"GAIN":{"value":6}}},
      {"position":6,"model":"Some Capture","category":"Neural Capture","enabled":true,"params":{}},
      {"position":7,"model":"Mono Looper","category":"Loopers","enabled":true,"params":{}}]}]})");
  const importer::ImportResult result = importer::convertForeignPreset(foreign, table);
  ASSERT_TRUE(result.ok) << result.error;
  const juce::var preset = result.preset;
  EXPECT_EQ(kindOf(slotNode(preset, 0, 0)), "dynamics.compressor");
  EXPECT_NEAR(static_cast<double>(kindParam(slotNode(preset, 0, 0), "thresholdDb")), -25.78, 1e-3);
  EXPECT_EQ(kindOf(slotNode(preset, 0, 1)), "delay.digital");
  EXPECT_NEAR(static_cast<double>(kindParam(slotNode(preset, 0, 1), "feedback")), 45.0, 1e-3);
  EXPECT_NEAR(static_cast<double>(slotNode(preset, 0, 1)["mix"]), 0.3, 1e-6);
  EXPECT_NEAR(static_cast<double>(kindParam(slotNode(preset, 0, 1), "timeMs")), 379.4, 1e-3)
      << "the unit's delay runs 0.6 ms short of the time it shows";
  EXPECT_EQ(kindOf(slotNode(preset, 0, 2)), "reverb.plate");
  EXPECT_NEAR(static_cast<double>(kindParam(slotNode(preset, 0, 2), "preDelayMs")), 40.0, 1e-3);
  EXPECT_EQ(kindOf(slotNode(preset, 0, 3)), "modulation.phaser");
  EXPECT_NEAR(static_cast<double>(kindParam(slotNode(preset, 0, 3), "depth")), 65.0, 1e-3);
  EXPECT_EQ(kindOf(slotNode(preset, 0, 4)), "filter.wah") << "the Wah category's fallback";
  for (int position : {5, 6, 7})
    EXPECT_EQ(slotNode(preset, 0, position)["type"].toString(), "insert") << position;

  const juce::var report = result.report;
  EXPECT_EQ(countIn(report["mapped"]), 4);
  EXPECT_EQ(countIn(report["approximated"]), 1);
  EXPECT_EQ(countIn(report["unmapped"]), 3);
  EXPECT_EQ(entryAt(report["unmapped"], 0, 5)["role"].toString(), "amp");
  EXPECT_EQ(entryAt(report["unmapped"], 0, 6)["role"].toString(), "capture");
  // The phaser's SPEED has no unit, so it is not taken as our rate in Hz: it is named.
  EXPECT_TRUE(entryAt(report["droppedParams"], 0, 3, "param", "SPEED").isObject());
  EXPECT_FALSE(static_cast<bool>(entryAt(report["mapped"], 0, 0)["verified"]));
}

TEST(PresetImporterTest, TheBuiltInTableMapsTheFactoryDriveModels) {
  const juce::var table = importer::builtInMapping("quad-cortex");
  struct Expected {
    const char* model;
    const char* kind;
  };
  const Expected expected[] = {
      {"Myth Drive", "drive.transparent"},  {"Obsessive Drive", "drive.overdrive"},
      {"OD250", "drive.overdrive"},         {"Rodent Drive", "drive.distortion"},
      {"Green 808", "drive.overdrive"},     {"Exotic", "drive.boost"},
      {"Freeman BOD", "drive.metal"},      {"Brit Blues", "drive.overdrive"},
      {"Chief DS1", "drive.distortion"},    {"Brit Governor", "drive.metal"},
      {"Chief MT", "drive.metal"},          {"Chief OD1", "drive.overdrive"},
      {"Chief SD1", "drive.overdrive"},     {"Fuzz Pi", "drive.fuzz"},
      {"Chief BD2", "drive.overdrive"},     {"Rage Booster", "drive.trebleboost"},
      {"Facial Fuzz", "drive.fuzz"},        {"Exotic Z Boost", "drive.boost"},
      {"Red Drive", "drive.overdrive"},     {"Vemural Ray", "drive.transparent"},
      {"No-Bell OD1", "drive.overdrive"},   {"Thunderpaw", "drive.distortion"},
      {"MK3 Silicon Fuzz", "drive.fuzz"},   {"81 Creations Drive", "drive.overdrive"},
      {"BDDI", "drive.bass"},               {"Douglas MT 3K", "drive.bass"},
      {"Douglas Vintage MT", "drive.bass"}, {"Soviet Fuzz", "drive.fuzz"},
      {"Exotic Bass Z Boost", "drive.bass"},
  };
  for (const Expected& item : expected) {
    const juce::var model = table["models"][item.model];
    ASSERT_TRUE(model.isObject()) << item.model;
    EXPECT_EQ(model["kind"].toString(), item.kind) << item.model;
    EXPECT_FALSE(static_cast<bool>(model["verified"])) << item.model;
    EXPECT_TRUE(model["notes"].toString().isNotEmpty()) << item.model;
  }
  // A model of the whole category the table does not name still lands on a
  // drive block.
  EXPECT_EQ(table["categories"]["Guitar Overdrive"].toString(), "drive.overdrive");
  EXPECT_EQ(table["categories"]["Bass Overdrive"].toString(), "drive.bass");
}

TEST(PresetImporterTest, DriveModelsGetTheirKindsAndTheKnobsOnOurScales) {
  const juce::var table = importer::builtInMapping("quad-cortex");
  const juce::var foreign = parseJson(R"json({"format":"pedaleira-foreign-preset","version":1,
    "source":"quad-cortex","name":"Drives","rows":[{"index":0,"blocks":[
      {"position":0,"model":"Chief MT","category":"Guitar Overdrive","enabled":true,
       "params":{"DIST":{"value":7},"LOW":{"value":5},"MIDDLE":{"value":2.5},
                 "MIDFREQ":{"value":5},"HIGH":{"value":10},"LEVEL":{"value":5}}},
      {"position":1,"model":"Rodent Drive","category":"Guitar Overdrive","enabled":true,
       "params":{"DISTORTION":{"value":10},"FILTER":{"value":3},"VOLUME":{"value":5}}},
      {"position":2,"model":"Fuzz Pi (M)","category":"Guitar Overdrive","enabled":true,
       "params":{"SUSTAIN":{"value":8},"TONE":{"value":5},"VOLUME":{"value":5}}},
      {"position":3,"model":"Green 808","category":"Guitar Overdrive","enabled":true,
       "params":{"OVERDRIVE":{"value":4},"TONE":{"value":6},"LEVEL":{"value":7.5}}},
      {"position":4,"model":"Rage Booster","category":"Guitar Overdrive","enabled":true,
       "params":{"VOLUME":{"value":5}}},
      {"position":5,"model":"Facial Fuzz","category":"Guitar Overdrive","enabled":true,
       "params":{"FUZZ":{"value":6},"VOLUME":{"value":5}}},
      {"position":6,"model":"A Pedal Not In The Table","category":"Guitar Overdrive","enabled":true,"params":{}},
      {"position":7,"model":"A Bass Pedal Not In The Table","category":"Bass Overdrive","enabled":true,"params":{}}]}]})json");
  const importer::ImportResult result = importer::convertForeignPreset(foreign, table);
  ASSERT_TRUE(result.ok) << result.error;
  const juce::var preset = result.preset;
  const auto number = [&](int position, const char* id) {
    return static_cast<double>(kindParam(slotNode(preset, 0, position), id));
  };

  EXPECT_EQ(kindOf(slotNode(preset, 0, 0)), "drive.metal");
  EXPECT_NEAR(number(0, "distortionPercent"), 70.0, 1e-3);
  EXPECT_NEAR(number(0, "lowDb"), 0.0, 1e-3);
  EXPECT_NEAR(number(0, "midDb"), -6.0, 1e-3);
  EXPECT_NEAR(number(0, "midHz"), 1600.0, 1e-2);
  EXPECT_NEAR(number(0, "highDb"), 12.0, 1e-3);
  EXPECT_NEAR(number(0, "levelDb"), -21.5, 1e-3) << "5 on the knob is the level measured on the unit";

  EXPECT_EQ(kindOf(slotNode(preset, 0, 1)), "drive.distortion");
  EXPECT_NEAR(number(1, "distortionPercent"), 100.0, 1e-3);
  EXPECT_NEAR(number(1, "filterPercent"), 70.0, 1e-3) << "its filter is a low-pass that darkens upward";

  EXPECT_EQ(kindOf(slotNode(preset, 0, 2)), "drive.fuzz");
  EXPECT_NEAR(number(2, "voicing"), 2.0, 1e-6) << "the sustain voicing";
  EXPECT_NEAR(number(2, "fuzzPercent"), 80.0, 1e-3);
  EXPECT_NEAR(number(2, "tonePercent"), 50.0, 1e-3);

  EXPECT_EQ(kindOf(slotNode(preset, 0, 3)), "drive.overdrive");
  EXPECT_NEAR(number(3, "drivePercent"), 40.0, 1e-3);
  EXPECT_NEAR(number(3, "tonePercent"), 60.0, 1e-3);
  EXPECT_NEAR(number(3, "levelDb"), 9.0, 1e-3);

  EXPECT_EQ(kindOf(slotNode(preset, 0, 4)), "drive.trebleboost");
  EXPECT_NEAR(number(4, "gainDb"), 15.0, 1e-3);

  EXPECT_EQ(kindOf(slotNode(preset, 0, 5)), "drive.fuzz");
  EXPECT_NEAR(number(5, "voicing"), 1.0, 1e-6) << "the germanium voicing";
  EXPECT_NEAR(number(5, "fuzzPercent"), 60.0, 1e-3);

  // Models the table does not name land on the category's drive block at
  // its defaults, and the report says so.
  EXPECT_EQ(kindOf(slotNode(preset, 0, 6)), "drive.overdrive");
  EXPECT_EQ(kindOf(slotNode(preset, 0, 7)), "drive.bass");
  const juce::var report = result.report;
  EXPECT_EQ(countIn(report["mapped"]), 6);
  EXPECT_EQ(countIn(report["approximated"]), 2);
  EXPECT_EQ(countIn(report["unmapped"]), 0);
  EXPECT_EQ(countIn(report["droppedParams"]), 0);
  for (int position = 0; position < 6; ++position)
    EXPECT_FALSE(static_cast<bool>(entryAt(report["mapped"], 0, position)["verified"])) << position;
}

TEST(PresetImporterTest, UtilityModelsGetTheirKinds) {
  const juce::var table = importer::builtInMapping("quad-cortex");
  const juce::var foreign = parseJson(R"json({"format":"pedaleira-foreign-preset","version":1,
    "source":"quad-cortex","name":"Utilities","rows":[{"index":0,"blocks":[
      {"position":0,"model":"Gain (M)","category":"Utility","enabled":true,
       "params":{"LEVEL":{"value":-60,"unit":"dB"},"PAN":{"value":-0.5},"PHASE INVERT":{"value":true}}},
      {"position":1,"model":"Volume","category":"Utility","enabled":true,
       "params":{"LEVEL":{"value":40,"unit":"%"},"CURVE":{"value":"Linear"}}},
      {"position":2,"model":"Doubler (ST)","category":"Utility","enabled":true,
       "params":{"SPREAD":{"value":80,"unit":"ms"},"DRY LEVEL":{"value":-3,"unit":"dB"},
                 "FX LEVEL":{"value":-6,"unit":"dB"}}},
      {"position":3,"model":"Phase Doctor","category":"Utility","enabled":true,
       "params":{"Ø POLARITY":{"value":true},"SOMETHING ELSE":{"value":3}}}]}]})json");
  const importer::ImportResult result = importer::convertForeignPreset(foreign, table);
  ASSERT_TRUE(result.ok) << result.error;
  const juce::var preset = result.preset;
  const auto number = [&](int position, const char* id) {
    return static_cast<double>(kindParam(slotNode(preset, 0, position), id));
  };

  EXPECT_EQ(kindOf(slotNode(preset, 0, 0)), "utility.gain");
  EXPECT_NEAR(number(0, "gainDb"), -24.0, 1e-3) << "clamped to our range";
  EXPECT_NEAR(number(0, "pan"), -0.5, 1e-3);
  EXPECT_NEAR(number(0, "phaseInvert"), 1.0, 1e-6);

  EXPECT_EQ(kindOf(slotNode(preset, 0, 1)), "utility.volume");
  EXPECT_NEAR(number(1, "positionPercent"), 40.0, 1e-3);
  EXPECT_NEAR(number(1, "curve"), 0.0, 1e-6);

  EXPECT_EQ(kindOf(slotNode(preset, 0, 2)), "utility.doubler");
  EXPECT_NEAR(number(2, "spreadMs"), 60.0, 1e-3) << "clamped to our range";
  EXPECT_NEAR(number(2, "dryDb"), -3.0, 1e-3);
  EXPECT_NEAR(number(2, "doubledDb"), -6.0, 1e-3);

  EXPECT_EQ(kindOf(slotNode(preset, 0, 3)), "utility.phase");
  EXPECT_NEAR(number(3, "polarity"), 1.0, 1e-6);
  EXPECT_NEAR(number(3, "delayMs"), 0.0, 1e-6);

  const juce::var report = result.report;
  EXPECT_EQ(countIn(report["mapped"]), 4);
  EXPECT_EQ(countIn(report["unmapped"]), 0);
  EXPECT_EQ(countIn(report["droppedParams"]), 1) << "the Phase Doctor control with no entry";
  EXPECT_EQ(countIn(report["approximated"]), 2) << "the two clamped values";
  for (int position = 0; position < 4; ++position)
    EXPECT_FALSE(static_cast<bool>(entryAt(report["mapped"], 0, position)["verified"])) << position;
}

// ####################
// THE COMMAND
// ####################

struct ScratchDir {
  ScratchDir()
      : dir(juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("t3k-import-tests-" + juce::Uuid().toString())) {
    dir.createDirectory();
  }
  ~ScratchDir() { dir.deleteRecursively(); }
  juce::File write(const char* name, const juce::String& text) const {
    const juce::File file = dir.getChildFile(name);
    EXPECT_TRUE(file.replaceWithText(text));
    return file;
  }
  juce::File dir;
};

juce::String quote(const juce::String& text) { return juce::JSON::toString(juce::var(text)); }

struct ImportRig {
  ImportRig() {
    proc.setPresetStoreForTesting(store.dir);
    proc.setMultiCoreEnabled(false, /*persist=*/false);
    proc.setPlayConfigDetails(2, 2, 48000.0, 128);
    proc.prepareToPlay(48000.0, 128);
    // A fresh library shows the read-only factory setlist; presets go to one of ours.
    must("setlist.create", R"({"name":"Gig","select":true})");
  }
  CommandResult run(const char* name, const juce::String& argsJson) {
    juce::var args;
    EXPECT_TRUE(juce::JSON::parse(argsJson, args).wasOk()) << argsJson;
    return commands.registry().run(name, args);
  }
  juce::var must(const char* name, const juce::String& argsJson) {
    const CommandResult result = run(name, argsJson);
    EXPECT_TRUE(result.ok) << name << ": " << result.errorCode << ": " << result.errorMessage;
    return result.value;
  }
  ScratchDir store;
  ChainTestProcessor proc;
  EngineCommands commands{proc};
};

const char* kQuadCortexPreset = R"({"format":"pedaleira-foreign-preset","version":1,
  "source":"quad-cortex","sourceVersion":"CorOS test","name":"Clean With Verb","tempo":100,
  "defaultScene":0,
  "rows":[{"index":0,"blocks":[
    {"position":0,"model":"VCA Comp (M) ","category":"Compressor","enabled":true,
     "params":{"THRESHOLD":{"value":-20,"unit":"dB"}}},
    {"position":1,"model":"Brit 2203","category":"Guitar Amplifier","enabled":true,"params":{}},
    {"position":4,"model":"Digital Delay (ST) ","category":"Delay","enabled":true,
     "params":{"FEEDBACK":{"value":30,"unit":"%"},"MIX":{"value":20,"unit":"%"}}},
    {"position":6,"model":"Hall","category":"Reverb","enabled":false,
     "params":{"MIX":{"value":35,"unit":"%"}}}]}],
  "routing":{"rows":[{"index":0,"input":{"type":"device","channel":1,"stereo":false},
                      "output":{"type":"device","pair":1}}],"splits":[]},
  "scenes":[{"name":"Clean","enabled":{"0:6":false,"0:4":false},"params":{}},
            {"name":"Wet","enabled":{"0:6":true,"0:4":true},"params":{}}]})";

TEST(PresetImporterCommandTest, ImportsIntoTheLibraryAndTheResultLoads) {
  ImportRig rig;
  ScratchDir files;
  const juce::File file = files.write("clean.json", kQuadCortexPreset);

  const juce::var result = rig.must("preset.importForeign", "{\"file\":" + quote(file.getFullPathName()) + "}");
  ASSERT_TRUE(result["preset"].isObject()) << juce::JSON::toString(result);
  EXPECT_EQ(result["preset"]["name"].toString(), "Clean With Verb");
  const int slot = static_cast<int>(result["preset"]["slot"]);
  EXPECT_EQ(slot, 1);
  EXPECT_EQ(result["preset"]["missingAssets"].size(), 0);

  const juce::var report = result["report"];
  EXPECT_EQ(countIn(report["mapped"]), 3);
  EXPECT_EQ(countIn(report["unmapped"]), 1);
  EXPECT_EQ(entryAt(report["unmapped"], 0, 1)["role"].toString(), "amp");
  EXPECT_EQ(entryAt(report["mapped"], 0, 4)["kind"].toString(), "delay.digital");

  // It is in the slot with the name, and it loads as a grid.
  EXPECT_EQ(rig.must("preset.list", "{}")["presets"].size(), 1);
  rig.must("preset.loadSlot", "{\"slot\":" + juce::String(slot) + "}");
  EXPECT_TRUE(static_cast<bool>(rig.must("state.waitUntilLoaded", R"({"timeoutMs":30000})")["loaded"]));

  const juce::var grid = rig.proc.getGridState();
  EXPECT_EQ(grid["mode"].toString(), "grid");
  EXPECT_FALSE(rig.proc.gridSlotBlockId(0, 0).empty());
  const juce::var tree = rig.must("scene.get", "{}");
  EXPECT_EQ(tree["scenes"][1]["name"].toString(), "Wet");
  EXPECT_EQ(tree["scenes"][0]["name"].toString(), "Clean");

  // The blocks are what the report said: the compressor at 0, the delay at 4.
  const std::string compressorId = rig.proc.gridSlotBlockId(0, 0);
  const std::string delayId = rig.proc.gridSlotBlockId(0, 4);
  const std::string hallId = rig.proc.gridSlotBlockId(0, 6);
  const juce::var compressor = rig.must("block.listParams", "{\"blockId\":" + quote(compressorId) + "}");
  double threshold = 1.0e9;
  for (const auto& param : *compressor["params"].getArray())
    if (param["id"].toString() == "thresholdDb")
      threshold = static_cast<double>(param["value"]);
  EXPECT_NEAR(threshold, -21.28, 1e-3);
  // The kind's own parameters tell the kind: feedback for the delay, decay for the hall.
  auto hasParam = [&rig](const std::string& blockId, const char* paramId) {
    const juce::var listed = rig.must("block.listParams", "{\"blockId\":" + quote(blockId) + "}");
    for (const auto& param : *listed["params"].getArray())
      if (param["id"].toString() == paramId)
        return true;
    return false;
  };
  EXPECT_TRUE(hasParam(delayId, "feedback"));
  EXPECT_TRUE(hasParam(hallId, "decayS"));
  EXPECT_EQ(rig.proc.getTempo(), 100.0) << "the preset's tempo";
}

TEST(PresetImporterCommandTest, ImportingTwiceGivesTwoPresetsAndANamedSlotIsRefused) {
  ImportRig rig;
  ScratchDir files;
  const juce::String arg = "\"file\":" + quote(files.write("a.json", kQuadCortexPreset).getFullPathName());
  EXPECT_EQ(static_cast<int>(rig.must("preset.importForeign", "{" + arg + "}")["preset"]["slot"]), 1);
  EXPECT_EQ(static_cast<int>(rig.must("preset.importForeign", "{" + arg + "}")["preset"]["slot"]), 2);
  EXPECT_EQ(static_cast<int>(rig.must("preset.importForeign", "{" + arg + ",\"slot\":9}")["preset"]["slot"]), 9);
  EXPECT_EQ(rig.run("preset.importForeign", "{" + arg + ",\"slot\":9}").errorCode, "failed");
  EXPECT_EQ(rig.run("preset.importForeign", "{" + arg + ",\"setlist\":\"nope\"}").errorCode, "not_found");
  EXPECT_EQ(rig.must("preset.list", "{}")["presets"].size(), 3);
}

TEST(PresetImporterCommandTest, TheDryRunReportsWhatTheImportDoesAndSavesNothing) {
  ImportRig rig;
  ScratchDir files;
  const juce::String arg = "\"file\":" + quote(files.write("dry.json", kQuadCortexPreset).getFullPathName());

  const juce::var preview = rig.must("preset.previewForeign", "{" + arg + "}");
  EXPECT_EQ(preview["name"].toString(), "Clean With Verb");
  EXPECT_EQ(rig.must("preset.list", "{}")["presets"].size(), 0) << "a dry run saved a preset";

  // The same report, entry for entry, as the import that follows.
  const juce::var imported = rig.must("preset.importForeign", "{" + arg + "}");
  EXPECT_EQ(juce::JSON::toString(preview["report"]), juce::JSON::toString(imported["report"]));
  EXPECT_TRUE(entryAt(preview["report"]["mapped"], 0, 4)["category"].toString().isNotEmpty());
  EXPECT_EQ(entryAt(preview["report"]["unmapped"], 0, 1)["cause"].toString(), "role");

  // It is a read: refused input fails the same way, and it needs no setlist.
  EXPECT_EQ(rig.run("preset.previewForeign", R"({"file":"/no/such/file.json"})").errorCode, "not_found");
}

TEST(PresetImporterCommandTest, ATableOfOnesOwnReplacesTheBuiltInOne) {
  ImportRig rig;
  ScratchDir files;
  const juce::File mapping = files.write("table.json", kTestMapping);
  const juce::File foreign = files.write("f.json", foreignWith(R"([
      {"index":0,"blocks":[{"position":2,"model":"Echo","category":"Delay","params":{"TIME":{"value":500}}}]}])"));
  const juce::var result = rig.must("preset.importForeign",
      "{\"file\":" + quote(foreign.getFullPathName()) + ",\"mapping\":" + quote(mapping.getFullPathName()) + "}");
  EXPECT_EQ(countIn(result["report"]["mapped"]), 1);
  EXPECT_TRUE(entryAt(result["report"]["mapped"], 0, 2)["verified"]);
  // Without it the file's source has no built-in table.
  const CommandResult none = rig.run("preset.importForeign", "{\"file\":" + quote(foreign.getFullPathName()) + "}");
  EXPECT_EQ(none.errorCode, "failed");
  EXPECT_TRUE(none.errorMessage.contains("no built-in mapping table")) << none.errorMessage;
  EXPECT_TRUE(none.errorMessage.contains("quad-cortex")) << none.errorMessage;
}

TEST(PresetImporterCommandTest, BadInputIsRefusedWithAReasonAndImportsNothing) {
  ImportRig rig;
  ScratchDir files;
  auto expectFailure = [&](const juce::String& args, const char* code, const char* part) {
    const CommandResult result = rig.run("preset.importForeign", args);
    EXPECT_FALSE(result.ok) << args;
    EXPECT_EQ(result.errorCode, juce::String(code)) << args << ": " << result.errorMessage;
    EXPECT_TRUE(result.errorMessage.contains(part)) << result.errorMessage;
  };
  expectFailure("{}", "invalid_args", "file");
  expectFailure("{\"file\":" + quote(files.dir.getChildFile("missing.json").getFullPathName()) + "}",
                "not_found", "file not found");
  expectFailure("{\"file\":" + quote(files.write("broken.json", "{not json").getFullPathName()) + "}",
                "failed", "cannot read");
  expectFailure("{\"file\":" + quote(files.write("array.json", "[]").getFullPathName()) + "}",
                "failed", "not a JSON object");
  expectFailure("{\"file\":" + quote(files.write("preset.json",
                    R"({"format":"pedaleira.preset","version":1})").getFullPathName()) + "}",
                "failed", "expected \"pedaleira-foreign-preset\"");
  expectFailure("{\"file\":" + quote(files.write("newer.json",
                    R"({"format":"pedaleira-foreign-preset","version":7,"source":"quad-cortex","rows":[]})").getFullPathName()) + "}",
                "failed", "version 7");
  expectFailure("{\"file\":" + quote(files.write("ok.json", kQuadCortexPreset).getFullPathName()) +
                    ",\"mapping\":" + quote(files.dir.getChildFile("none.json").getFullPathName()) + "}",
                "not_found", "file not found");
  expectFailure("{\"file\":" + quote(files.write("ok2.json", kQuadCortexPreset).getFullPathName()) +
                    ",\"mapping\":" + quote(files.write("badtable.json", R"({"source":"quad-cortex"})").getFullPathName()) + "}",
                "failed", "version");
  expectFailure("{\"file\":" + quote(files.write("ok3.json", kQuadCortexPreset).getFullPathName()) +
                    ",\"mapping\":" + quote(files.write("other.json", R"({"source":"other","version":1})").getFullPathName()) + "}",
                "failed", "mapping table is for");
  EXPECT_EQ(rig.must("preset.list", "{}")["presets"].size(), 0);
}

TEST(PresetImporterCommandTest, PathsMeetTheRulesOfEveryOtherPathArgument) {
  AgentRig rig;
  const juce::File inside = rig.dirs.scratch.getChildFile("clean.json");
  ASSERT_TRUE(inside.replaceWithText(kQuadCortexPreset));
  const juce::File outside = rig.dirs.outside.getChildFile("clean.json");
  ASSERT_TRUE(outside.replaceWithText(kQuadCortexPreset));
  const juce::File mappingOutside = rig.dirs.outside.getChildFile("table.json");
  ASSERT_TRUE(mappingOutside.replaceWithText(kTestMapping));

  const auto client = rig.hub.openSession("agent");
  ASSERT_TRUE(rig.commands.registry().contains("preset.importForeign"));
  rig.mustLocal("setlist.create", R"({"name":"Gig","select":true})");

  // Outside the folders an agent may name, or relative: refused before the
  // command sees the path, for both path arguments.
  EXPECT_EQ(rig.as(client, "preset.importForeign", "{\"file\":" + agentQuoted(outside.getFullPathName()) + "}").errorCode,
            "permission_denied");
  EXPECT_EQ(rig.as(client, "preset.importForeign", R"({"file":"clean.json"})").errorCode, "permission_denied");
  EXPECT_EQ(rig.as(client, "preset.importForeign",
                   "{\"file\":" + agentQuoted(inside.getFullPathName()) + ",\"mapping\":" +
                       agentQuoted(mappingOutside.getFullPathName()) + "}").errorCode,
            "permission_denied");
  // Inside, it is an edit: it waits for the user's approval.
  const CommandResult asked =
      rig.as(client, "preset.importForeign", "{\"file\":" + agentQuoted(inside.getFullPathName()) + "}");
  EXPECT_EQ(asked.errorCode, "needs_approval") << asked.errorMessage;
  if (asked.errorCode == CommandError::needsApproval)
    rig.mustLocal("approvals.resolve", "{\"id\":" + agentQuoted(asked.errorDetails["approvalId"].toString()) +
                                           R"(,"approve":false})");
  EXPECT_EQ(rig.mustLocal("preset.list")["presets"].size(), 0);

  // The user themselves may name a file anywhere.
  const juce::var done =
      rig.mustLocal("preset.importForeign", "{\"file\":" + agentQuoted(outside.getFullPathName()) + "}");
  EXPECT_EQ(done["preset"]["name"].toString(), "Clean With Verb");
  EXPECT_EQ(rig.mustLocal("preset.list")["presets"].size(), 1);
}

TEST(PresetImporterTest, TheBuiltInTableMapsTheFactoryDelayModels) {
  const juce::var table = importer::builtInMapping("quad-cortex");
  struct Expected {
    const char* model;
    const char* kind;
  };
  const Expected expected[] = {
      {"Digital Delay", "delay.digital"},       {"Analog Delay", "delay.analog"},
      {"Tape Delay", "delay.tape"},             {"Simple Delay", "delay.simple"},
      {"Slapback Delay", "delay.slapback"},     {"Dual Delay", "delay.dual"},
      {"Reverse Delay", "delay.reverse"},       {"Dual Reverse Delay", "delay.reverse"},
      {"Circular Delay", "delay.multitap"},
  };
  for (const Expected& item : expected) {
    const juce::var model = table["models"][item.model];
    ASSERT_TRUE(model.isObject()) << item.model;
    EXPECT_EQ(model["kind"].toString(), item.kind) << item.model;
    EXPECT_FALSE(static_cast<bool>(model["verified"])) << item.model;
    EXPECT_TRUE(model["notes"].toString().isNotEmpty()) << item.model;
  }
  // The ducking of the unit's delays, and only of those.
  for (const char* model : {"Digital Delay", "Analog Delay", "Tape Delay", "Simple Delay",
                            "Slapback Delay", "Dual Delay"}) {
    const juce::var params = table["models"][model]["params"];
    EXPECT_EQ(params["DYN DEPTH"]["param"].toString(), "duckDb") << model;
    EXPECT_EQ(params["RELEASE"]["param"].toString(), "duckReleaseMs") << model;
  }
  for (const char* model : {"Reverse Delay", "Dual Reverse Delay", "Circular Delay",
                            "Simple Ping Pong Delay"})
    EXPECT_FALSE(table["models"][model]["params"]["DYN DEPTH"].isObject()) << model;
}

TEST(PresetImporterTest, DelayModelsGetTheirKindsAndValuesOnOurScales) {
  const juce::var table = importer::builtInMapping("quad-cortex");
  const juce::var foreign = parseJson(R"json({"format":"pedaleira-foreign-preset","version":1,
    "source":"quad-cortex","name":"Delays","rows":[{"index":0,"blocks":[
      {"position":0,"model":"Slapback Delay (M)","category":"Delay","enabled":true,
       "params":{"DELAY TIME":{"value":95,"unit":"ms"},"FEEDBACK":{"value":20,"unit":"%"},
                 "MIX":{"value":40,"unit":"%"},"LOW PASS":{"value":4000,"unit":"Hz"},
                 "HIGH PASS":{"value":120,"unit":"Hz"}}},
      {"position":1,"model":"Dual Delay (ST)","category":"Delay","enabled":true,
       "params":{"DELAY TIME L":{"value":300,"unit":"ms"},"DELAY TIME R":{"value":450,"unit":"ms"},
                 "FEEDBACK L":{"value":30,"unit":"%"},"FEEDBACK R":{"value":45,"unit":"%"},
                 "X-FEEDBACK":{"value":60,"unit":"%"},"HIGH PASS":{"value":200,"unit":"Hz"},
                 "LOW PASS":{"value":9000,"unit":"Hz"},"MIX":{"value":35,"unit":"%"},
                 "DYN DEPTH":{"value":40,"unit":"dB"},"RELEASE":{"value":600,"unit":"ms"}}},
      {"position":2,"model":"Reverse Delay (M)","category":"Delay","enabled":true,
       "params":{"DELAY TIME":{"value":1200,"unit":"ms"},"FEEDBACK":{"value":50,"unit":"%"},
                 "LOW PASS":{"value":6000,"unit":"Hz"},"MIX":{"value":50,"unit":"%"}}},
      {"position":3,"model":"Dual Reverse Delay (ST)","category":"Delay","enabled":true,
       "params":{"DELAY TIME L":{"value":800,"unit":"ms"},"DELAY TIME R":{"value":900,"unit":"ms"},
                 "FEEDBACK L":{"value":25,"unit":"%"}}},
      {"position":4,"model":"Circular Delay (M)","category":"Delay","enabled":true,
       "params":{"DELAY TIME":{"value":700,"unit":"ms"},"FEEDBACK":{"value":55,"unit":"%"},
                 "DIFFUSION":{"value":65,"unit":"%"},"HIGH PASS":{"value":150,"unit":"Hz"},
                 "LOW PASS":{"value":5000,"unit":"Hz"},"MIX":{"value":25,"unit":"%"}}},
      {"position":5,"model":"Digital Delay (M)","category":"Delay","enabled":true,
       "params":{"DELAY TIME":{"value":250,"unit":"ms"},"DYN DEPTH":{"value":12,"unit":"dB"},
                 "RELEASE":{"value":300,"unit":"ms"}}}]}]})json");
  const importer::ImportResult result = importer::convertForeignPreset(foreign, table);
  ASSERT_TRUE(result.ok) << result.error;
  const juce::var preset = result.preset;
  const auto number = [&](int position, const char* id) {
    return static_cast<double>(kindParam(slotNode(preset, 0, position), id));
  };

  EXPECT_EQ(kindOf(slotNode(preset, 0, 0)), "delay.slapback");
  EXPECT_NEAR(number(0, "timeMs"), 95.0, 1e-3);
  EXPECT_NEAR(number(0, "feedback"), 20.0, 1e-3);
  EXPECT_NEAR(number(0, "toneHz"), 4000.0, 1e-2);
  EXPECT_NEAR(static_cast<double>(slotNode(preset, 0, 0)["mix"]), 0.4, 1e-6);

  EXPECT_EQ(kindOf(slotNode(preset, 0, 1)), "delay.dual");
  EXPECT_NEAR(number(1, "leftTimeMs"), 300.0, 1e-3);
  EXPECT_NEAR(number(1, "rightTimeMs"), 450.0, 1e-3);
  EXPECT_NEAR(number(1, "leftFeedback"), 30.0, 1e-3);
  EXPECT_NEAR(number(1, "rightFeedback"), 45.0, 1e-3);
  EXPECT_NEAR(number(1, "crossFeedback"), 60.0, 1e-3);
  EXPECT_NEAR(number(1, "lowCutHz"), 200.0, 1e-2);
  EXPECT_NEAR(number(1, "highCutHz"), 9000.0, 1e-1);
  EXPECT_NEAR(number(1, "duckDb"), 24.0, 1e-3) << "40 dB on the unit is clamped to our 24";
  EXPECT_NEAR(number(1, "duckReleaseMs"), 600.0, 1e-3);

  EXPECT_EQ(kindOf(slotNode(preset, 0, 2)), "delay.reverse");
  EXPECT_NEAR(number(2, "timeMs"), 1200.0, 1e-3);
  EXPECT_NEAR(number(2, "feedback"), 50.0, 1e-3);
  EXPECT_NEAR(number(2, "toneHz"), 6000.0, 1e-2);

  EXPECT_EQ(kindOf(slotNode(preset, 0, 3)), "delay.reverse");
  EXPECT_NEAR(number(3, "timeMs"), 800.0, 1e-3) << "the left time only";
  EXPECT_NEAR(number(3, "feedback"), 25.0, 1e-3);

  EXPECT_EQ(kindOf(slotNode(preset, 0, 4)), "delay.multitap");
  EXPECT_NEAR(number(4, "timeMs"), 700.0, 1e-3);
  EXPECT_NEAR(number(4, "feedback"), 55.0, 1e-3);
  EXPECT_NEAR(number(4, "diffusion"), 65.0, 1e-3);
  EXPECT_NEAR(number(4, "lowCutHz"), 150.0, 1e-2);
  EXPECT_NEAR(number(4, "highCutHz"), 5000.0, 1e-2);

  EXPECT_EQ(kindOf(slotNode(preset, 0, 5)), "delay.digital");
  EXPECT_NEAR(number(5, "duckDb"), 12.0, 1e-3);
  EXPECT_NEAR(number(5, "duckReleaseMs"), 300.0, 1e-3);

  const juce::var report = result.report;
  EXPECT_EQ(countIn(report["mapped"]), 6);
  EXPECT_EQ(countIn(report["unmapped"]), 0);
  // What our kinds have no place for is named, not guessed.
  EXPECT_TRUE(entryAt(report["droppedParams"], 0, 0, "param", "HIGH PASS").isObject());
  EXPECT_TRUE(entryAt(report["droppedParams"], 0, 3, "param", "DELAY TIME R").isObject());
  // The ducking depth was out of our range and is reported as clamped.
  EXPECT_TRUE(entryAt(report["approximated"], 0, 1, "param", "DYN DEPTH").isObject());
}

}  // namespace

TEST(PresetImporterTest, EqualiserModelsGetTheirKinds) {
  const juce::var table = importer::builtInMapping("quad-cortex");
  const juce::var foreign = parseJson(R"json({"format":"pedaleira-foreign-preset","version":1,
    "source":"quad-cortex","name":"Equalisers","rows":[{"index":0,"blocks":[
      {"position":0,"model":"Parametric-3","category":"Equalizer","enabled":true,
       "params":{"2 FREQ":{"value":800,"unit":"Hz"},"2 GAIN":{"value":24,"unit":"dB"},"2 Q":{"value":2},
                 "OUTPUT":{"value":-3,"unit":"dB"}}},
      {"position":1,"model":"Parametric-8","category":"Equalizer","enabled":true,
       "params":{"8 FREQ":{"value":9000,"unit":"Hz"},"8 TYPE":{"value":1}}},
      {"position":2,"model":"Low-High Cut","category":"Equalizer","enabled":true,
       "params":{"HPF FREQ":{"value":90,"unit":"Hz"},"LPF FREQ":{"value":7000,"unit":"Hz"}}},
      {"position":3,"model":"Plugin Parametric-4","category":"Equalizer","enabled":true,
       "params":{"LO MID GAIN":{"value":-4,"unit":"dB"},"LO MID FREQ":{"value":350,"unit":"Hz"}}}]}]})json");
  const importer::ImportResult result = importer::convertForeignPreset(foreign, table);
  ASSERT_TRUE(result.ok) << result.error;
  const juce::var preset = result.preset;
  const auto number = [&](int position, const char* id) {
    return static_cast<double>(kindParam(slotNode(preset, 0, position), id));
  };

  EXPECT_EQ(kindOf(slotNode(preset, 0, 0)), "eq.parametric3");
  EXPECT_NEAR(number(0, "b2Freq"), 800.0, 1e-3);
  EXPECT_NEAR(number(0, "b2Gain"), 18.0, 1e-3) << "clamped to our range";
  EXPECT_NEAR(number(0, "b2Q"), 2.0, 1e-3);
  EXPECT_NEAR(number(0, "levelDb"), -3.0, 1e-3);

  EXPECT_EQ(kindOf(slotNode(preset, 0, 1)), "eq.parametric8");
  EXPECT_NEAR(number(1, "b8Freq"), 9000.0, 1e-3);
  EXPECT_NEAR(number(1, "b8Type"), 1.0, 1e-6);

  EXPECT_EQ(kindOf(slotNode(preset, 0, 2)), "eq.cut");
  EXPECT_NEAR(number(2, "hpFreq"), 90.0, 1e-3);
  EXPECT_NEAR(number(2, "lpFreq"), 7000.0, 1e-3);

  EXPECT_EQ(kindOf(slotNode(preset, 0, 3)), "eq.console4");
  EXPECT_NEAR(number(3, "lowMidGain"), -4.0, 1e-3);
  EXPECT_NEAR(number(3, "lowMidFreq"), 350.0, 1e-3);
  EXPECT_NEAR(number(3, "hpOn"), 1.0, 1e-6) << "the unit's filters are always in";
}

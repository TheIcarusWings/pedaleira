// The JSON preset format and the asset store (PresetJson.h, docs/library.md),
// on trees built by hand: what the format must give back exactly, what its
// text must look like for a diff to stay small, and what happens around a
// missing or damaged asset. The processor's side (real rigs through real
// presets) is library_tests.cpp.
#include "PresetFile.h"
#include "PresetJson.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

namespace {

namespace presetjson = t3k::presetjson;

struct TempDir {
  TempDir()
      : dir(juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("t3k-preset-json-tests-" + juce::Uuid().toString())) {
    dir.createDirectory();
  }
  ~TempDir() { dir.deleteRecursively(); }
  juce::File dir;
};

// The bytes a .t3kpreset body would be: two trees with the same bytes hold
// the same properties, of the same kinds, in the same order.
juce::MemoryBlock bytesOf(const juce::ValueTree& tree) {
  juce::MemoryBlock bytes;
  juce::MemoryOutputStream out(bytes, false);
  tree.writeToStream(out);
  out.flush();
  return bytes;
}

juce::ValueTree presetWith(const juce::ValueTree& chainChild) {
  juce::ValueTree preset(t3k::presetfile::kTag);
  preset.setProperty("schemaVersion", 1, nullptr);
  preset.setProperty("name", "Glass Cleans", nullptr);
  preset.setProperty("id", "0f1e2d3c4b5a69788796a5b4c3d2e1f0", nullptr);
  juce::ValueTree chain("ChainSnapshot");
  chain.setProperty("stereoEnabled", false, nullptr);
  chain.setProperty("branchSide", "left", nullptr);
  chain.setProperty("branchAfterBlockId", "", nullptr);
  if (chainChild.isValid())
    chain.appendChild(chainChild, nullptr);
  preset.appendChild(chain, nullptr);
  juce::ValueTree params("Params");
  for (const auto& [id, value] : {std::pair<const char*, double>{"inputLevel", 0.0},
                                  {"gateThreshold", -62.5},
                                  {"toneBass", 5.0}}) {
    juce::ValueTree param("Param");
    param.setProperty("id", id, nullptr);
    param.setProperty("value", value, nullptr);
    params.appendChild(param, nullptr);
  }
  preset.appendChild(params, nullptr);
  return preset;
}

// Writes with `store` (null: everything inline), reads back with it.
juce::ValueTree roundTrip(const juce::ValueTree& preset, const presetjson::AssetStore* store,
                          juce::String* textOut = nullptr) {
  presetjson::WriteOptions writeOptions;
  writeOptions.assets = store;
  juce::String error;
  const juce::String text = presetjson::toJson(preset, writeOptions, error);
  EXPECT_TRUE(text.isNotEmpty()) << error;
  if (textOut != nullptr)
    *textOut = text;
  presetjson::ReadOptions readOptions;
  if (store != nullptr)
    readOptions.assets.push_back(store);
  const presetjson::ReadResult read = presetjson::fromJson(text, readOptions);
  EXPECT_TRUE(read.preset.isValid()) << read.error;
  EXPECT_TRUE(read.missing.empty());
  return read.preset;
}

juce::MemoryBlock patternBytes(size_t size, unsigned seed) {
  juce::MemoryBlock block(size);
  auto* data = static_cast<unsigned char*>(block.getData());
  std::uint32_t lcg = seed;
  for (size_t i = 0; i < size; ++i) {
    lcg = lcg * 1664525u + 1013904223u;
    data[i] = static_cast<unsigned char>(lcg >> 24);
  }
  return block;
}

}  // namespace

TEST(PresetJsonTest, EveryKindOfValueComesBackExactly) {
  juce::ValueTree node("Values");
  node.setProperty("true", true, nullptr);
  node.setProperty("false", false, nullptr);
  node.setProperty("int", 42, nullptr);
  node.setProperty("negative", -7, nullptr);
  node.setProperty("intMax", std::numeric_limits<int>::max(), nullptr);
  // JSON::parse reads this one back as an int64 unless it is marked.
  node.setProperty("intMin", std::numeric_limits<int>::min(), nullptr);
  // And a small int64 as an int.
  node.setProperty("smallInt64", static_cast<juce::int64>(5), nullptr);
  node.setProperty("zeroInt64", static_cast<juce::int64>(0), nullptr);
  node.setProperty("bigInt64", static_cast<juce::int64>(9007199254740993LL), nullptr);
  node.setProperty("justOverInt", static_cast<juce::int64>(2147483648LL), nullptr);
  node.setProperty("justUnderInt", static_cast<juce::int64>(-2147483649LL), nullptr);
  node.setProperty("int64Min", std::numeric_limits<juce::int64>::min(), nullptr);
  node.setProperty("int64Max", std::numeric_limits<juce::int64>::max(), nullptr);
  // A double that happens to be whole stays a double.
  node.setProperty("wholeDouble", 3.0, nullptr);
  node.setProperty("zeroDouble", 0.0, nullptr);
  node.setProperty("minusZero", -0.0, nullptr);
  node.setProperty("nan", std::numeric_limits<double>::quiet_NaN(), nullptr);
  node.setProperty("inf", std::numeric_limits<double>::infinity(), nullptr);
  node.setProperty("minusInf", -std::numeric_limits<double>::infinity(), nullptr);
  node.setProperty("empty", "", nullptr);
  node.setProperty("text", juce::String::fromUTF8("Caf\xc3\xa9 \"quoted\" back\\slash\n\ttab \x01 "
                                                  "\xe6\x97\xa5\xe6\x9c\xac \xf0\x9f\x8e\xb8"),
                   nullptr);
  node.setProperty("looksLikeANumber", "12", nullptr);
  node.setProperty("looksTyped", "{\"$int64\": \"1\"}", nullptr);
  node.setProperty("void", juce::var(), nullptr);
  node.setProperty("bytes", juce::var(patternBytes(300, 1)), nullptr);
  node.setProperty("noBytes", juce::var(juce::MemoryBlock()), nullptr);
  juce::Array<juce::var> list;
  list.add(1);
  list.add(2.5);
  list.add("three");
  list.add(static_cast<juce::int64>(4));
  node.setProperty("list", list, nullptr);

  const juce::ValueTree preset = presetWith(node);
  juce::String text;
  const juce::ValueTree back = roundTrip(preset, nullptr, &text);
  EXPECT_TRUE(bytesOf(back) == bytesOf(preset)) << text;

  // The kinds, spelled out for the ones the reader would otherwise change.
  const juce::ValueTree values = back.getChildWithName("ChainSnapshot").getChildWithName("Values");
  EXPECT_TRUE(values.getProperty("intMin").isInt());
  EXPECT_TRUE(values.getProperty("smallInt64").isInt64());
  EXPECT_TRUE(values.getProperty("zeroInt64").isInt64());
  EXPECT_TRUE(values.getProperty("wholeDouble").isDouble());
  EXPECT_TRUE(std::signbit(static_cast<double>(values.getProperty("minusZero"))));
  EXPECT_TRUE(std::isnan(static_cast<double>(values.getProperty("nan"))));
  EXPECT_TRUE(values.getProperty("looksLikeANumber").isString());
  EXPECT_TRUE(values.getProperty("looksTyped").isString());
  EXPECT_TRUE(values.getProperty("void").isVoid());
}

TEST(PresetJsonTest, DoublesAndFloatsKeepEveryBit) {
  std::vector<double> values = {
      0.1,
      1.0 / 3.0,
      juce::MathConstants<double>::pi,
      std::numeric_limits<double>::min(),
      std::numeric_limits<double>::denorm_min(),
      std::numeric_limits<double>::max(),
      std::numeric_limits<double>::epsilon(),
      1.0 + std::numeric_limits<double>::epsilon(),
      1e21,
      1e-7,
      123456789012345680.0,
      -2.2250738585072011e-308,  // the classic slow-path strtod value
      5e-324,
      // What a block parameter really is: a float widened to double.
      static_cast<double>(0.1f),
      static_cast<double>(0.7f),
      static_cast<double>(-12.3f),
      static_cast<double>(std::numeric_limits<float>::min()),
      static_cast<double>(std::numeric_limits<float>::max()),
      static_cast<double>(std::nextafter(1.0f, 2.0f)),
  };
  // And a few thousand arbitrary bit patterns, doubles and widened floats.
  std::mt19937_64 random(20261010);
  for (int i = 0; i < 4000; ++i) {
    const std::uint64_t bits = random();
    double asDouble = 0.0;
    std::memcpy(&asDouble, &bits, sizeof(double));
    if (std::isfinite(asDouble))
      values.push_back(asDouble);
    const auto floatBits = static_cast<std::uint32_t>(bits >> 17);
    float asFloat = 0.0f;
    std::memcpy(&asFloat, &floatBits, sizeof(float));
    if (std::isfinite(asFloat))
      values.push_back(static_cast<double>(asFloat));
  }

  juce::ValueTree node("Doubles");
  for (size_t i = 0; i < values.size(); ++i)
    node.setProperty(juce::Identifier("v" + juce::String(static_cast<int>(i))), values[i], nullptr);
  const juce::ValueTree back =
      roundTrip(presetWith(node), nullptr).getChildWithName("ChainSnapshot").getChildWithName("Doubles");
  ASSERT_EQ(back.getNumProperties(), static_cast<int>(values.size()));
  for (size_t i = 0; i < values.size(); ++i) {
    const juce::var read = back.getProperty(juce::Identifier("v" + juce::String(static_cast<int>(i))));
    ASSERT_TRUE(read.isDouble()) << values[i];
    const double value = static_cast<double>(read);
    ASSERT_EQ(std::memcmp(&value, &values[i], sizeof(double)), 0)
        << juce::String(values[i], 20) << " came back as " << juce::String(value, 20);
  }

  // Short where it can be: nobody wants 0.10000000000000001 in a diff.
  juce::ValueTree simple("Simple");
  simple.setProperty("tenth", 0.1, nullptr);
  simple.setProperty("whole", 2.0, nullptr);
  simple.setProperty("float", static_cast<double>(0.1f), nullptr);
  juce::String text;
  roundTrip(presetWith(simple), nullptr, &text);
  EXPECT_TRUE(text.contains("\"tenth\": 0.1,")) << text;
  EXPECT_TRUE(text.contains("\"whole\": 2.0,")) << text;
  EXPECT_TRUE(text.contains("\"float\": 0.10000000149011612")) << text;
}

TEST(PresetJsonTest, TheTextIsStableAndReadable) {
  juce::ValueTree blocks("ChainBlocks");
  juce::ValueTree block("ChainBlock");
  block.setProperty("id", "block-1", nullptr);
  block.setProperty("type", "kind", nullptr);
  block.setProperty("enabled", true, nullptr);
  block.setProperty("inputGain", 0.5f, nullptr);
  juce::ValueTree kind("KindState");
  kind.setProperty("kind", "delay.digital", nullptr);
  kind.setProperty("version", 1, nullptr);
  kind.setProperty("name", "Digital Delay", nullptr);
  juce::ValueTree time("Param");
  time.setProperty("id", "time", nullptr);
  time.setProperty("value", 380.0, nullptr);
  kind.appendChild(time, nullptr);
  block.appendChild(kind, nullptr);
  blocks.appendChild(block, nullptr);
  const juce::ValueTree preset = presetWith(blocks);

  juce::String error;
  const juce::String first = presetjson::toJson(preset, {}, error);
  ASSERT_TRUE(first.isNotEmpty()) << error;
  // The same tree is the same bytes, and so is the tree read back from them.
  EXPECT_EQ(presetjson::toJson(preset, {}, error), first);
  const presetjson::ReadResult read = presetjson::fromJson(first, {});
  ASSERT_TRUE(read.preset.isValid()) << read.error;
  EXPECT_EQ(presetjson::toJson(read.preset, {}, error), first);
  EXPECT_TRUE(bytesOf(read.preset) == bytesOf(preset));

  // The top of the file, in the order the tree holds it.
  EXPECT_TRUE(first.startsWith("{\n  \"format\": \"pedaleira.preset\",\n  \"version\": 1,\n"
                               "  \"schemaVersion\": 1,\n  \"name\": \"Glass Cleans\",\n"
                               "  \"id\": \"0f1e2d3c4b5a69788796a5b4c3d2e1f0\",\n  \"chain\": {\n"))
      << first;
  // The faceplate as a map, one parameter a line.
  EXPECT_TRUE(first.contains("  \"params\": {\n    \"inputLevel\": 0.0,\n"
                             "    \"gateThreshold\": -62.5,\n    \"toneBass\": 5.0\n  }\n}\n"))
      << first;
  // A small node is one line of a diff.
  EXPECT_TRUE(first.contains("{ \"node\": \"Param\", \"id\": \"time\", \"value\": 380.0 }")) << first;
  EXPECT_TRUE(first.endsWith("}\n"));
  EXPECT_FALSE(first.contains("\r"));
  EXPECT_FALSE(first.contains("\t"));

  // One value changed is one line changed.
  juce::ValueTree edited = preset.createCopy();
  edited.getChildWithName("ChainSnapshot")
      .getChildWithName("ChainBlocks")
      .getChild(0)
      .getChildWithName("KindState")
      .getChild(0)
      .setProperty("value", 420.0, nullptr);
  const juce::StringArray before = juce::StringArray::fromLines(first);
  const juce::StringArray after = juce::StringArray::fromLines(presetjson::toJson(edited, {}, error));
  ASSERT_EQ(before.size(), after.size());
  int changed = 0;
  for (int i = 0; i < before.size(); ++i)
    changed += before[i] != after[i] ? 1 : 0;
  EXPECT_EQ(changed, 1);
}

TEST(PresetJsonTest, NamesTheFormatUsesItselfAreEscaped) {
  // Properties called like the format's own keys, at the top and in a node.
  juce::ValueTree node("Odd");
  node.setProperty("node", "not a type", nullptr);
  node.setProperty("children", 3, nullptr);
  node.setProperty("$dollar", true, nullptr);
  node.setProperty("$$two", 1.5, nullptr);
  // Free in a node: only the top level reserves these.
  node.setProperty("version", 7, nullptr);
  node.setProperty("format", "x", nullptr);
  juce::ValueTree preset = presetWith(node);
  preset.setProperty("format", "mine", nullptr);
  preset.setProperty("version", 99, nullptr);
  preset.setProperty("chain", "a property", nullptr);
  preset.setProperty("params", 1, nullptr);
  preset.setProperty("children", 2, nullptr);
  preset.setProperty("node", 3, nullptr);
  preset.setProperty("tempo", 92.0, nullptr);

  juce::String text;
  const juce::ValueTree back = roundTrip(preset, nullptr, &text);
  EXPECT_TRUE(bytesOf(back) == bytesOf(preset)) << text;
  EXPECT_TRUE(text.contains("\"$format\": \"mine\"")) << text;
  EXPECT_TRUE(text.contains("\"$$dollar\": true")) << text;
  // Still a preset to a reader of the header.
  const presetjson::Meta meta = presetjson::readMeta(text);
  EXPECT_TRUE(meta.valid) << meta.problem;
  EXPECT_EQ(meta.name, juce::String("Glass Cleans"));
  ASSERT_TRUE(meta.tempo.has_value());
  EXPECT_DOUBLE_EQ(*meta.tempo, 92.0);
}

TEST(PresetJsonTest, ShapesThisBuildDoesNotSaveStillRoundTrip) {
  // A Params node that is not a plain id/value list cannot be a map.
  juce::ValueTree preset = presetWith({});
  preset.getChildWithName("Params").setProperty("note", "hand made", nullptr);
  juce::String text;
  EXPECT_TRUE(bytesOf(roundTrip(preset, nullptr, &text)) == bytesOf(preset));
  EXPECT_FALSE(text.contains("\"params\":")) << text;
  EXPECT_TRUE(text.contains("\"children\": [")) << text;

  // A value that is an int where a double is expected.
  preset = presetWith({});
  preset.getChildWithName("Params").getChild(0).setProperty("value", 3, nullptr);
  EXPECT_TRUE(bytesOf(roundTrip(preset, nullptr)) == bytesOf(preset));

  // Children in another order, a child of another type, no children at all.
  preset = presetWith({});
  preset.moveChild(1, 0, nullptr);
  preset.appendChild(juce::ValueTree("Future"), nullptr);
  EXPECT_TRUE(bytesOf(roundTrip(preset, nullptr)) == bytesOf(preset));
  preset = juce::ValueTree(t3k::presetfile::kTag);
  preset.setProperty("name", "Bare", nullptr);
  EXPECT_TRUE(bytesOf(roundTrip(preset, nullptr)) == bytesOf(preset));

  // A file written by hand: whole numbers for parameter values are doubles
  // in the tree, as the engine saves them.
  const presetjson::ReadResult read = presetjson::fromJson(
      R"({"format":"pedaleira.preset","version":1,"name":"By hand","chain":{"stereoEnabled":false},
          "params":{"toneBass":7}})",
      {});
  ASSERT_TRUE(read.preset.isValid()) << read.error;
  EXPECT_TRUE(read.preset.getChildWithName("Params").getChild(0).getProperty("value").isDouble());
  EXPECT_TRUE(read.preset.getChildWithName("ChainSnapshot").isValid());
}

TEST(PresetJsonTest, WhatIsNotAPresetIsRefusedWithAReason) {
  const auto errorOf = [](const char* text) { return presetjson::fromJson(text, {}).error; };
  EXPECT_TRUE(errorOf("{ not json").startsWith("not valid JSON"));
  EXPECT_EQ(errorOf("[1, 2]"), juce::String("not a JSON object"));
  EXPECT_EQ(errorOf(R"({"format":"something.else","version":1})"),
            juce::String("not a pedaleira.preset file"));
  EXPECT_TRUE(errorOf(R"({"format":"pedaleira.preset","version":2})").contains("newer version"));
  EXPECT_TRUE(errorOf(R"({"format":"pedaleira.preset","version":1,"chain":[]})").contains("chain"));
  EXPECT_TRUE(errorOf(R"({"format":"pedaleira.preset","version":1,"x":{"$mystery":1}})")
                  .contains("typed values"));
  EXPECT_TRUE(errorOf(R"({"format":"pedaleira.preset","version":1,"children":[{"id":"no type"}]})")
                  .contains("\"node\""));
  EXPECT_TRUE(errorOf(R"({"format":"pedaleira.preset","version":1,"x":{"$base64":"***"}})")
                  .contains("base64"));

  juce::String error;
  EXPECT_TRUE(presetjson::toJson(juce::ValueTree("SomethingElse"), {}, error).isEmpty());
  EXPECT_TRUE(error.isNotEmpty());

  EXPECT_FALSE(presetjson::readMeta("{}").valid);
  EXPECT_FALSE(presetjson::readMeta("").valid);
}

TEST(PresetJsonTest, MetaSummarisesWithoutTheAssets) {
  juce::ValueTree scenes("Scenes");
  scenes.setProperty("version", 1, nullptr);
  scenes.setProperty("active", 2, nullptr);
  for (int i = 0; i < 8; ++i) {
    juce::ValueTree scene("Scene");
    scene.setProperty("index", i, nullptr);
    if (i == 0)
      scene.setProperty("name", "Verse", nullptr);
    if (i == 5) {
      juce::ValueTree block("Block");
      block.setProperty("id", "b", nullptr);
      block.setProperty("enabled", false, nullptr);
      scene.appendChild(block, nullptr);
    }
    // An assigned value is in every scene and says nothing about use.
    juce::ValueTree value("Value");
    value.setProperty("blockId", "b", nullptr);
    value.setProperty("param", "mix", nullptr);
    value.setProperty("value", 0.5, nullptr);
    scene.appendChild(value, nullptr);
    scenes.appendChild(scene, nullptr);
  }
  juce::ValueTree preset = presetWith(scenes);
  juce::Array<juce::var> tags;
  tags.add("clean");
  tags.add("  chorus ");
  preset.setProperty("tags", tags, nullptr);

  juce::String error;
  const presetjson::Meta meta = presetjson::readMeta(presetjson::toJson(preset, {}, error));
  ASSERT_TRUE(meta.valid) << meta.problem;
  EXPECT_EQ(meta.id, juce::String("0f1e2d3c4b5a69788796a5b4c3d2e1f0"));
  EXPECT_EQ(meta.name, juce::String("Glass Cleans"));
  // The named one, the active one and the one with a block state of its own.
  EXPECT_EQ(meta.sceneCount, 3);
  EXPECT_FALSE(meta.tempo.has_value());
  EXPECT_EQ(meta.tags, juce::StringArray({"clean", "chorus"}));

  // No scenes: one.
  EXPECT_EQ(presetjson::readMeta(presetjson::toJson(presetWith({}), {}, error)).sceneCount, 1);
}

TEST(PresetJsonTest, BinaryGoesInlineOrToTheStoreByKindAndSize) {
  TempDir temp;
  const presetjson::AssetStore store(temp.dir.getChildFile("Assets"));

  juce::ValueTree blocks("ChainBlocks");
  juce::ValueTree block("ChainBlock");
  block.setProperty("id", "amp", nullptr);
  block.setProperty("type", "nam", nullptr);
  block.setProperty("toneJson",
                    R"({"id":7,"title":"Plexi","format":"nam","models":[)"
                    R"({"id":11,"name":"Plexi bright","model_url":"https://x.invalid/a/plexi.nam?sig=1"},)"
                    R"({"id":12,"name":"Plexi dark","model_url":"file:///stash/123"}]})",
                    nullptr);
  juce::ValueTree cache("ModelCache");
  // Model data goes to the store whatever its size: a tiny one too.
  const juce::MemoryBlock tinyModel = patternBytes(64, 11);
  const juce::MemoryBlock model = patternBytes(50000, 12);
  for (const auto& [modelId, data] : {std::pair<int, const juce::MemoryBlock*>{11, &tinyModel},
                                      {12, &model}}) {
    juce::ValueTree cached("CachedModel");
    cached.setProperty("modelId", modelId, nullptr);
    cached.setProperty("data", juce::var(*data), nullptr);
    cache.appendChild(cached, nullptr);
  }
  block.appendChild(cache, nullptr);
  blocks.appendChild(block, nullptr);

  // A plugin's chunk: inline up to the limit, an asset above it.
  juce::ValueTree plugin("ChainBlock");
  plugin.setProperty("id", "fx", nullptr);
  plugin.setProperty("type", "kind", nullptr);
  juce::ValueTree kind("KindState");
  kind.setProperty("kind", "plugin", nullptr);
  kind.setProperty("name", "Space Echo", nullptr);
  const juce::MemoryBlock atLimit = patternBytes(presetjson::kInlineBinaryLimit, 3);
  const juce::MemoryBlock overLimit = patternBytes(presetjson::kInlineBinaryLimit + 1, 4);
  kind.setProperty("chunk", juce::var(atLimit), nullptr);
  kind.setProperty("bigChunk", juce::var(overLimit), nullptr);
  plugin.appendChild(kind, nullptr);
  blocks.appendChild(plugin, nullptr);
  const juce::ValueTree preset = presetWith(blocks);

  juce::String text;
  const juce::ValueTree back = roundTrip(preset, &store, &text);
  EXPECT_TRUE(bytesOf(back) == bytesOf(preset));

  // Three assets: the two models and the large chunk.
  const auto assets = presetjson::referencedAssets(juce::JSON::parse(text));
  ASSERT_EQ(assets.size(), 3u);
  EXPECT_EQ(assets[0].hash, presetjson::AssetStore::hashOf(tinyModel.getData(), tinyModel.getSize()));
  EXPECT_EQ(assets[0].ext, juce::String("nam"));
  EXPECT_EQ(assets[0].name, juce::String("Plexi bright"));
  EXPECT_EQ(assets[0].size, 64);
  EXPECT_EQ(assets[1].name, juce::String("Plexi dark"));
  EXPECT_EQ(assets[1].ext, juce::String("nam"));  // no extension in its URL: the block's type
  EXPECT_EQ(assets[1].size, 50000);
  EXPECT_EQ(assets[2].ext, juce::String("bin"));
  EXPECT_EQ(assets[2].name, juce::String("Space Echo bigChunk"));
  for (const auto& asset : assets)
    EXPECT_TRUE(store.fileFor(asset.hash, asset.ext).existsAsFile()) << asset.name;
  EXPECT_EQ(temp.dir.getChildFile("Assets").getNumberOfChildFiles(juce::File::findFiles), 3);
  EXPECT_TRUE(text.contains("\"chunk\": { \"$base64\": \"")) << text;
  // The megabytes are not in the text.
  EXPECT_LT(text.length(), 6000);

  // An IR block's model is a .wav.
  juce::ValueTree ir = block.createCopy();
  ir.setProperty("type", "ir", nullptr);
  ir.setProperty("toneJson", R"({"id":8,"title":"Cab","format":"ir"})", nullptr);
  juce::ValueTree irBlocks("ChainBlocks");
  irBlocks.appendChild(ir, nullptr);
  roundTrip(presetWith(irBlocks), &store, &text);
  const auto irAssets = presetjson::referencedAssets(juce::JSON::parse(text));
  ASSERT_EQ(irAssets.size(), 2u);
  EXPECT_EQ(irAssets[0].ext, juce::String("wav"));
  EXPECT_EQ(irAssets[0].name, juce::String("Cab"));  // no model entry: the tone's title

  // Without a store everything is in the file, and still exact.
  juce::String selfContained;
  EXPECT_TRUE(bytesOf(roundTrip(preset, nullptr, &selfContained)) == bytesOf(preset));
  EXPECT_TRUE(presetjson::referencedAssets(juce::JSON::parse(selfContained)).empty());
  EXPECT_GT(selfContained.length(), 60000);
}

TEST(AssetStoreTest, TheSameBytesAreStoredOnce) {
  TempDir temp;
  const presetjson::AssetStore store(temp.dir.getChildFile("Assets"));
  const juce::MemoryBlock bytes = patternBytes(4096, 99);

  presetjson::AssetRef first, second, other;
  ASSERT_TRUE(store.put(bytes.getData(), bytes.getSize(), "nam", first));
  const juce::File file = store.fileFor(first.hash, first.ext);
  ASSERT_TRUE(file.existsAsFile());
  EXPECT_EQ(file.getFileName(), first.hash + ".nam");
  EXPECT_EQ(first.hash.length(), 64);
  EXPECT_EQ(first.size, 4096);
  // SHA-256 of the empty input, as a known answer for the hash itself.
  EXPECT_EQ(presetjson::AssetStore::hashOf(nullptr, 0),
            juce::String("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));

  // A second put writes nothing: the file is the one from before.
  const juce::Time written = file.getLastModificationTime();
  juce::Thread::sleep(20);
  ASSERT_TRUE(store.put(bytes.getData(), bytes.getSize(), "NAM", second));
  EXPECT_EQ(second.hash, first.hash);
  EXPECT_EQ(second.ext, juce::String("nam"));
  EXPECT_EQ(file.getLastModificationTime(), written);
  EXPECT_EQ(store.directory().getNumberOfChildFiles(juce::File::findFiles), 1);

  // Other bytes are another file; an odd extension is "bin".
  const juce::MemoryBlock different = patternBytes(4096, 100);
  ASSERT_TRUE(store.put(different.getData(), different.getSize(), "../../evil", other));
  EXPECT_NE(other.hash, first.hash);
  EXPECT_EQ(other.ext, juce::String("bin"));
  EXPECT_EQ(store.directory().getNumberOfChildFiles(juce::File::findFiles), 2);

  juce::MemoryBlock read;
  EXPECT_EQ(store.get(first, read), presetjson::AssetStore::Status::ok);
  EXPECT_TRUE(read == bytes);

  // Two presets with the same model share the file.
  juce::ValueTree cache("ModelCache");
  juce::ValueTree cached("CachedModel");
  cached.setProperty("modelId", 1, nullptr);
  cached.setProperty("data", juce::var(bytes), nullptr);
  cache.appendChild(cached, nullptr);
  juce::ValueTree block("ChainBlock");
  block.setProperty("type", "nam", nullptr);
  block.appendChild(cache, nullptr);
  juce::String error;
  presetjson::WriteOptions options;
  options.assets = &store;
  ASSERT_TRUE(presetjson::toJson(presetWith(block), options, error).isNotEmpty());
  ASSERT_TRUE(presetjson::toJson(presetWith(block.createCopy()), options, error).isNotEmpty());
  EXPECT_EQ(store.directory().getNumberOfChildFiles(juce::File::findFiles), 2);

  // Only a hash becomes a path.
  presetjson::AssetRef crafted;
  crafted.hash = "../../../etc/passwd";
  crafted.ext = "nam";
  EXPECT_EQ(store.fileFor(crafted.hash, crafted.ext), juce::File());
  EXPECT_EQ(store.get(crafted, read), presetjson::AssetStore::Status::missing);
}

TEST(AssetStoreTest, AMissingAssetIsReportedAndKeptAsAReference) {
  TempDir temp;
  const presetjson::AssetStore store(temp.dir.getChildFile("Assets"));
  const juce::MemoryBlock model = patternBytes(9000, 5);

  juce::ValueTree cache("ModelCache");
  juce::ValueTree cached("CachedModel");
  cached.setProperty("modelId", 1, nullptr);
  cached.setProperty("data", juce::var(model), nullptr);
  cache.appendChild(cached, nullptr);
  juce::ValueTree block("ChainBlock");
  block.setProperty("type", "ir", nullptr);
  block.setProperty("toneJson", R"({"id":1,"title":"Room","models":[{"id":1,"name":"Room A"}]})",
                    nullptr);
  block.appendChild(cache, nullptr);
  const juce::ValueTree preset = presetWith(block);

  presetjson::WriteOptions writeOptions;
  writeOptions.assets = &store;
  juce::String error;
  const juce::String text = presetjson::toJson(preset, writeOptions, error);
  ASSERT_TRUE(text.isNotEmpty()) << error;
  const auto assets = presetjson::referencedAssets(juce::JSON::parse(text));
  ASSERT_EQ(assets.size(), 1u);
  const juce::File file = store.fileFor(assets[0].hash, assets[0].ext);

  // Somebody forgot to commit the Assets folder.
  ASSERT_TRUE(file.deleteFile());
  presetjson::ReadOptions readOptions;
  readOptions.assets.push_back(&store);
  presetjson::ReadResult read = presetjson::fromJson(text, readOptions);
  // The preset is there all the same, with everything but those bytes.
  ASSERT_TRUE(read.preset.isValid()) << read.error;
  ASSERT_EQ(read.missing.size(), 1u);
  EXPECT_EQ(read.missing[0].hash, assets[0].hash);
  EXPECT_EQ(read.missing[0].name, juce::String("Room A"));
  EXPECT_EQ(read.missing[0].size, 9000);
  const juce::ValueTree readModel =
      read.preset.getChildWithName("ChainSnapshot").getChild(0).getChildWithName("ModelCache").getChild(0);
  EXPECT_EQ(readModel.getProperty("data").getBinaryData(), nullptr);
  EXPECT_TRUE(presetjson::isMissingAsset(readModel.getProperty("data")));
  EXPECT_TRUE(presetjson::hasMissingAsset(read.preset));
  EXPECT_FALSE(presetjson::hasMissingAsset(preset));
  EXPECT_EQ(read.preset.getProperty("name").toString(), juce::String("Glass Cleans"));

  // Written again, it is the same file: the reference is not lost by moving
  // or renaming a preset whose asset is away.
  EXPECT_EQ(presetjson::toJson(read.preset, writeOptions, error), text);

  // The asset comes back (the folder is pulled) and so do the bytes.
  ASSERT_TRUE(file.replaceWithData(model.getData(), model.getSize()));
  read = presetjson::fromJson(text, readOptions);
  EXPECT_TRUE(read.missing.empty());
  EXPECT_TRUE(bytesOf(read.preset) == bytesOf(preset));

  // A second store is asked when the first does not have it.
  const presetjson::AssetStore empty(temp.dir.getChildFile("Elsewhere"));
  readOptions.assets = {&empty, &store};
  EXPECT_TRUE(presetjson::fromJson(text, readOptions).missing.empty());
  // With none at all every reference is missing.
  EXPECT_EQ(presetjson::fromJson(text, {}).missing.size(), 1u);
}

TEST(AssetStoreTest, AFileThatIsNotWhatItsNameSaysIsNotUsed) {
  TempDir temp;
  const presetjson::AssetStore store(temp.dir.getChildFile("Assets"));
  const juce::MemoryBlock bytes = patternBytes(2000, 21);
  presetjson::AssetRef ref;
  ASSERT_TRUE(store.put(bytes.getData(), bytes.getSize(), "wav", ref));
  const juce::File file = store.fileFor(ref.hash, ref.ext);
  juce::MemoryBlock read;
  ASSERT_EQ(store.get(ref, read), presetjson::AssetStore::Status::ok);

  // Same size, other content (a bad sync, a disk error): caught by the hash.
  // (A store that has checked a file trusts it until it changes; the write
  // below changes its time, and a fresh store has checked nothing.)
  juce::MemoryBlock flipped = bytes;
  static_cast<char*>(flipped.getData())[1000] ^= 0x40;
  juce::Thread::sleep(1100);  // file times may have one second resolution
  ASSERT_TRUE(file.replaceWithData(flipped.getData(), flipped.getSize()));
  EXPECT_EQ(store.get(ref, read), presetjson::AssetStore::Status::corrupt);
  EXPECT_EQ(read.getSize(), 0u) << "damaged bytes must not be handed out";
  const presetjson::AssetStore fresh(temp.dir.getChildFile("Assets"));
  EXPECT_EQ(fresh.get(ref, read), presetjson::AssetStore::Status::corrupt);

  // Another size (a truncated copy): caught before reading.
  ASSERT_TRUE(file.replaceWithData(bytes.getData(), 1000));
  EXPECT_EQ(fresh.get(ref, read), presetjson::AssetStore::Status::corrupt);

  // To a preset, a damaged asset is a missing one.
  juce::ValueTree cached("CachedModel");
  cached.setProperty("modelId", 1, nullptr);
  cached.setProperty("data", juce::var(bytes), nullptr);
  juce::ValueTree cache("ModelCache");
  cache.appendChild(cached, nullptr);
  juce::ValueTree block("ChainBlock");
  block.setProperty("type", "ir", nullptr);
  block.appendChild(cache, nullptr);
  // (put leaves a file of another size alone only when the size matches, so
  // this write repairs the truncated one.)
  presetjson::WriteOptions writeOptions;
  writeOptions.assets = &fresh;
  juce::String error;
  const juce::String text = presetjson::toJson(presetWith(block), writeOptions, error);
  ASSERT_TRUE(text.isNotEmpty()) << error;
  ASSERT_EQ(fresh.get(ref, read), presetjson::AssetStore::Status::ok) << "put repaired the file";
  juce::Thread::sleep(1100);
  ASSERT_TRUE(file.replaceWithData(flipped.getData(), flipped.getSize()));
  presetjson::ReadOptions readOptions;
  readOptions.assets.push_back(&fresh);
  const presetjson::ReadResult result = presetjson::fromJson(text, readOptions);
  ASSERT_TRUE(result.preset.isValid());
  ASSERT_EQ(result.missing.size(), 1u);
  EXPECT_EQ(result.missing[0].hash, ref.hash);
}

TEST(AssetStoreTest, AHugeAssetStaysOutOfThePresetAndComesBackWhole) {
  TempDir temp;
  const presetjson::AssetStore store(temp.dir.getChildFile("Assets"));
  // A long stereo IR at a high rate is this order of size.
  constexpr size_t kSize = 48u * 1024u * 1024u;
  const juce::MemoryBlock huge = patternBytes(kSize, 77);

  juce::ValueTree cached("CachedModel");
  cached.setProperty("modelId", 1, nullptr);
  cached.setProperty("data", juce::var(huge), nullptr);
  juce::ValueTree cache("ModelCache");
  cache.appendChild(cached, nullptr);
  juce::ValueTree block("ChainBlock");
  block.setProperty("type", "ir", nullptr);
  block.appendChild(cache, nullptr);
  const juce::ValueTree preset = presetWith(block);

  juce::String text;
  const juce::ValueTree back = roundTrip(preset, &store, &text);
  EXPECT_LT(text.length(), 2000);
  const auto assets = presetjson::referencedAssets(juce::JSON::parse(text));
  ASSERT_EQ(assets.size(), 1u);
  EXPECT_EQ(assets[0].size, static_cast<juce::int64>(kSize));
  EXPECT_EQ(store.fileFor(assets[0].hash, assets[0].ext).getSize(), static_cast<juce::int64>(kSize));
  const auto* data = back.getChildWithName("ChainSnapshot")
                         .getChild(0)
                         .getChildWithName("ModelCache")
                         .getChild(0)
                         .getProperty("data")
                         .getBinaryData();
  ASSERT_NE(data, nullptr);
  EXPECT_TRUE(*data == huge);

  // Read again by the same store: checked once, not hashed a second time
  // (a preset switch must not cost a pass over 48 MB).
  presetjson::ReadOptions options;
  options.assets.push_back(&store);
  const auto start = juce::Time::getMillisecondCounterHiRes();
  const presetjson::ReadResult again = presetjson::fromJson(text, options);
  const double readMs = juce::Time::getMillisecondCounterHiRes() - start;
  EXPECT_TRUE(again.missing.empty());
  std::cout << "[ info ] 48 MB asset read back in " << readMs << " ms\n";
}

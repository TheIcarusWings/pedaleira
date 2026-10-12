// The BlockProcessor boundary (plugin/include/BlockProcessor.h): NAM and IR
// engines sit behind one interface, and nothing a project, a user preset or
// a factory preset stores may move because of it.
//
// Saved state has no per-block version field, so the block node's shape is
// the contract: these tests pin the exact property list of each node kind
// and require save -> restore -> save to reproduce the tree. They also pin
// what each kind reports through the interface (latency, tail) and that a
// tone swap to the other kind lands the other processor without losing the
// block's settings.

#include "chain_test_helpers.h"

#include <gtest/gtest.h>
#include <juce_dsp/juce_dsp.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

// Decode a T3KB blob (getStateInformation's output) back into its state tree.
juce::ValueTree parseStateBlob(const juce::MemoryBlock& data) {
  if (data.getSize() <= 4 || std::memcmp(data.getData(), "T3KB", 4) != 0)
    return {};
  return juce::ValueTree::readFromData(static_cast<const char*>(data.getData()) + 4,
                                       data.getSize() - 4);
}

juce::ValueTree savedState(TONE3000Processor& proc) {
  juce::MemoryBlock saved;
  proc.getStateInformation(saved);
  return parseStateBlob(saved);
}

juce::ValueTree savedBlock(const juce::ValueTree& state, const juce::String& blockId) {
  const juce::ValueTree snapshot = state.getChildWithName("ChainSnapshot");
  for (const auto* laneName : {"ChainBlocks", "RightChainBlocks"}) {
    const juce::ValueTree lane = snapshot.getChildWithName(laneName);
    for (int i = 0; i < lane.getNumChildren(); ++i)
      if (lane.getChild(i).getProperty("id").toString() == blockId)
        return lane.getChild(i);
  }
  return {};
}

juce::ValueTree firstSavedInsert(const juce::ValueTree& state) {
  const juce::ValueTree lane =
      state.getChildWithName("ChainSnapshot").getChildWithName("ChainBlocks");
  for (int i = 0; i < lane.getNumChildren(); ++i)
    if (lane.getChild(i).getProperty("type").toString() == "insert")
      return lane.getChild(i);
  return {};
}

// Property names in stored order: the binary stream writes them in this
// order, so a reorder changes saved bytes just like a rename does.
std::vector<std::string> propertyNames(const juce::ValueTree& tree) {
  std::vector<std::string> names;
  for (int i = 0; i < tree.getNumProperties(); ++i)
    names.push_back(tree.getPropertyName(i).toString().toStdString());
  return names;
}

std::vector<std::string> childTypes(const juce::ValueTree& tree) {
  std::vector<std::string> types;
  for (int i = 0; i < tree.getNumChildren(); ++i)
    types.push_back(tree.getChild(i).getType().toString().toStdString());
  return types;
}

juce::var chainStateBlock(TONE3000Processor& proc, const juce::String& blockId) {
  const juce::var state = proc.getChainState(-1);
  for (const auto* laneKey : {"chain", "chainRight"})
    if (const auto* lane = state[laneKey].getArray())
      for (const auto& item : *lane)
        if (item["blockId"].toString() == blockId)
          return item;
  return {};
}

// A swap keeps the outgoing engine processing (`loaded` stays true) while
// the replacement builds, so "loaded" alone cannot tell the swap has landed:
// wait for the in-flight load to finish on the new active model.
bool waitForSwap(TONE3000Processor& proc, const juce::String& blockId, int modelId,
                 int timeoutMs = 20000) {
  const auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(timeoutMs);
  while (juce::Time::getMillisecondCounter() < deadline) {
    const juce::var block = chainStateBlock(proc, blockId);
    if (block.isObject() && static_cast<int>(block["activeModelId"]) == modelId &&
        static_cast<bool>(block["loaded"]) && !static_cast<bool>(block["modelLoading"]))
      return true;
    juce::Thread::sleep(20);
  }
  return false;
}

juce::ValueTree monoSnapshot(const std::vector<juce::ValueTree>& blocks) {
  juce::ValueTree snapshot("ChainSnapshot");
  juce::ValueTree left("ChainBlocks");
  for (const auto& block : blocks)
    left.appendChild(block, nullptr);
  snapshot.appendChild(left, nullptr);
  return snapshot;
}

// A catalog-shaped tone whose single model is served from a file on disk
// (file:// URLs read straight from disk, see fetchModelFromUrl), so a swap
// to it loads with no network.
juce::String catalogTone(int toneId, int modelId, const char* format, const juce::File& file) {
  juce::DynamicObject::Ptr model = new juce::DynamicObject();
  model->setProperty("id", modelId);
  model->setProperty("name", "model");
  model->setProperty("model_url", juce::URL(file).toString(false));
  juce::DynamicObject::Ptr tone = new juce::DynamicObject();
  tone->setProperty("id", toneId);
  tone->setProperty("title", "Swap Target");
  tone->setProperty("format", format);
  tone->setProperty("models", juce::Array<juce::var>{juce::var(model.get())});
  return juce::JSON::toString(juce::var(tone.get()));
}

// A scratch copy of a fixture: the loader stamps what it reads, and the
// fixtures should stay untouched. Removed when the test ends.
struct ScratchCopy {
  explicit ScratchCopy(const char* fixture)
      : file(juce::File::getSpecialLocation(juce::File::tempDirectory)
                 .getChildFile("t3k-blockproc-" + juce::Uuid().toString() + "-" + fixture)) {
    EXPECT_TRUE(testFile(fixture).copyFileTo(file));
  }
  ~ScratchCopy() { file.deleteFile(); }
  juce::File file;
};

// The kernel length the convolver ends up with for `fixture`, measured the
// way the loader loads it (mono, trimmed, resampled to the base rate) but on
// a convolver of the test's own.
double loadedIrSeconds(const char* fixture) {
  juce::dsp::Convolution convolver;
  convolver.loadImpulseResponse(testFile(fixture), juce::dsp::Convolution::Stereo::no,
                                juce::dsp::Convolution::Trim::yes, 0,
                                juce::dsp::Convolution::Normalise::no);
  convolver.prepare({kFs, 256, 2});
  return convolver.getCurrentIRSize() / kFs;
}

const std::vector<std::string> kToneBlockProperties = {
    "id",        "type",       "enabled", "normalize", "slimSize",     "inputGain",
    "outputGain", "mix",       "toneId",  "toneJson",  "activeModelId"};
const std::vector<std::string> kInsertBlockProperties = {
    "id", "type", "enabled", "normalize", "slimSize", "inputGain", "outputGain", "mix"};

}  // namespace

TEST(BlockProcessorTest, StateRoundTripKeepsTheSavedShape) {
  ChainTestProcessor proc;
  proc.restoreFromTree(monoSnapshot(
      {makeNamBlockTree("blk-amp", 1, 100), makeIrBlockTree("blk-cab", 2, 200)}));
  ASSERT_TRUE(waitForChainLoaded(proc));

  // Move every per-block setting off its default, the type-specific ones
  // included, so a field that fails to round-trip cannot hide behind one.
  EXPECT_TRUE(proc.setBlockParam("blk-amp", "normalize", 0.0));
  EXPECT_TRUE(proc.setBlockSlimSize("blk-amp", 1.0));
  EXPECT_TRUE(proc.setBlockParam("blk-amp", "inputGain", 0.62));
  EXPECT_TRUE(proc.setBlockParam("blk-amp", "outputGain", 0.41));
  EXPECT_TRUE(proc.setBlockParam("blk-amp", "mix", 0.8));
  EXPECT_TRUE(proc.setBlockEqBand("blk-amp", 2, eqBandVar("bell", 900.0, 4.5, 1.4)));
  EXPECT_TRUE(proc.setBlockParam("blk-cab", "enabled", 0.0));
  EXPECT_TRUE(proc.setBlockParam("blk-cab", "normalize", 0.0));
  EXPECT_TRUE(proc.setBlockParam("blk-cab", "inputGain", 0.45));
  EXPECT_TRUE(proc.setBlockParam("blk-cab", "outputGain", 0.7));
  EXPECT_TRUE(proc.setBlockParam("blk-cab", "mix", 0.35));
  EXPECT_TRUE(proc.setBlockEqPre("blk-cab", true));

  const juce::ValueTree first = savedState(proc);
  ASSERT_TRUE(first.isValid());

  // The shape of each node kind. A new, renamed, dropped or reordered
  // property here changes what every existing project and preset means.
  const juce::ValueTree amp = savedBlock(first, "blk-amp");
  const juce::ValueTree cab = savedBlock(first, "blk-cab");
  const juce::ValueTree insert = firstSavedInsert(first);
  ASSERT_TRUE(amp.isValid());
  ASSERT_TRUE(cab.isValid());
  ASSERT_TRUE(insert.isValid());
  EXPECT_EQ(propertyNames(amp), kToneBlockProperties);
  EXPECT_EQ(propertyNames(cab), kToneBlockProperties);
  EXPECT_EQ(propertyNames(insert), kInsertBlockProperties);
  EXPECT_EQ(childTypes(amp), (std::vector<std::string>{"Eq", "ModelCache"}));
  EXPECT_EQ(childTypes(cab), (std::vector<std::string>{"Eq", "ModelCache"}));
  EXPECT_TRUE(childTypes(insert).empty());

  EXPECT_EQ(amp.getProperty("type").toString(), "nam");
  EXPECT_EQ(cab.getProperty("type").toString(), "ir");
  // Stored types matter as much as names: the binary stream tags each value.
  for (const auto& node : {amp, cab, insert}) {
    EXPECT_TRUE(node.getProperty("normalize").isBool());
    EXPECT_TRUE(node.getProperty("slimSize").isDouble());
  }
  EXPECT_FALSE(static_cast<bool>(amp.getProperty("normalize")));
  EXPECT_DOUBLE_EQ(static_cast<double>(amp.getProperty("slimSize")), 1.0);
  // The IR node carries the NAM-only fields too; they are inert there but
  // have always been written, and the toggle sticks on any block.
  EXPECT_FALSE(static_cast<bool>(cab.getProperty("normalize")));
  EXPECT_DOUBLE_EQ(static_cast<double>(cab.getProperty("slimSize")), 0.0);
  EXPECT_FALSE(static_cast<bool>(cab.getProperty("enabled")));

  // Save -> restore into a fresh processor -> save: the same tree.
  juce::MemoryBlock blob;
  proc.getStateInformation(blob);
  ChainTestProcessor reopened;
  reopened.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
  ASSERT_TRUE(waitForChainLoaded(reopened));
  const juce::ValueTree second = savedState(reopened);
  ASSERT_TRUE(second.isValid());

  EXPECT_TRUE(first.getChildWithName("ChainSnapshot")
                  .isEquivalentTo(second.getChildWithName("ChainSnapshot")))
      << "the chain snapshot changed across a save/restore round trip";
  EXPECT_TRUE(first.isEquivalentTo(second))
      << "the saved state changed across a save/restore round trip";

  // And what the restored blocks run with is what was saved.
  const juce::var ampRow = chainStateBlock(reopened, "blk-amp");
  EXPECT_FALSE(static_cast<bool>(ampRow["params"]["normalize"]));
  EXPECT_DOUBLE_EQ(static_cast<double>(ampRow["params"]["slimSize"]), 1.0);
}

TEST(BlockProcessorTest, TypeSpecificSettingsSurviveOnEveryNodeKind) {
  // An IR node saved with a non-default size (a block created while the
  // machine default was "full") and normalize off. Neither field does
  // anything on an IR, but both have to come back out exactly as they went
  // in: the block may be swapped to a NAM tone later.
  juce::ValueTree cab = makeIrBlockTree("blk-cab", 2, 200);
  cab.setProperty("normalize", false, nullptr);
  cab.setProperty("slimSize", 1.0, nullptr);

  ChainTestProcessor proc;
  proc.restoreFromTree(monoSnapshot({cab}));
  ASSERT_TRUE(waitForChainLoaded(proc));

  const juce::ValueTree saved = savedBlock(savedState(proc), "blk-cab");
  ASSERT_TRUE(saved.isValid());
  EXPECT_EQ(propertyNames(saved), kToneBlockProperties);
  EXPECT_FALSE(static_cast<bool>(saved.getProperty("normalize")));
  EXPECT_DOUBLE_EQ(static_cast<double>(saved.getProperty("slimSize")), 1.0);
}

TEST(BlockProcessorTest, EachKindReportsLatencyAndTail) {
  ChainTestProcessor proc;
  proc.restoreFromTree(monoSnapshot(
      {makeNamBlockTree("blk-amp", 1, 100),
       makeIrBlockTree("blk-cab", 2, 200, "cab-ir-test.wav"),
       makeIrBlockTree("blk-verb", 3, 300, "reverb-ir-mono-test.wav")}));
  ASSERT_TRUE(waitForChainLoaded(proc));

  // No block kind adds latency today, and none of it is in the plugin's
  // reported latency (processor_tests.cpp pins that number).
  EXPECT_EQ(proc.blockLatencySamples("blk-amp"), 0);
  EXPECT_EQ(proc.blockLatencySamples("blk-cab"), 0);
  EXPECT_EQ(proc.blockLatencySamples("blk-verb"), 0);

  // A NAM capture has no tail of its own; an IR rings for its kernel.
  EXPECT_DOUBLE_EQ(proc.blockTailSeconds("blk-amp"), 0.0);
  const double cabSeconds = loadedIrSeconds("cab-ir-test.wav");
  const double verbSeconds = loadedIrSeconds("reverb-ir-mono-test.wav");
  ASSERT_GT(cabSeconds, 0.0);
  ASSERT_GT(verbSeconds, cabSeconds) << "the reverb fixture should outlast the cab";
  EXPECT_DOUBLE_EQ(proc.blockTailSeconds("blk-cab"), cabSeconds);
  EXPECT_DOUBLE_EQ(proc.blockTailSeconds("blk-verb"), verbSeconds);

  // The host-facing tail is the longest block tail, floored by the DC
  // blocker's 2 s allowance.
  EXPECT_DOUBLE_EQ(proc.getTailLengthSeconds(), std::max(verbSeconds, 2.0));

  // Insert placeholders have no engine to ask; neither do unknown ids.
  const juce::ValueTree insert = firstSavedInsert(savedState(proc));
  ASSERT_TRUE(insert.isValid());
  const std::string insertId = insert.getProperty("id").toString().toStdString();
  EXPECT_EQ(proc.blockLatencySamples(insertId), -1);
  EXPECT_DOUBLE_EQ(proc.blockTailSeconds(insertId), -1.0);
  EXPECT_EQ(proc.blockLatencySamples("no-such-block"), -1);
}

TEST(BlockProcessorTest, ToneSwapAcrossKindsKeepsTheBlockAndItsSettings) {
  ChainTestProcessor proc;
  proc.restoreFromTree(monoSnapshot({makeNamBlockTree("blk-a", 1, 100)}));
  ASSERT_TRUE(waitForChainLoaded(proc));
  EXPECT_TRUE(proc.setBlockParam("blk-a", "normalize", 0.0));
  EXPECT_TRUE(proc.setBlockSlimSize("blk-a", 1.0));
  EXPECT_EQ(proc.namEngineVoiceCount("blk-a"), 1);
  EXPECT_DOUBLE_EQ(proc.blockTailSeconds("blk-a"), 0.0);

  // NAM -> IR: the same block, now running a convolver.
  const ScratchCopy verb("reverb-ir-mono-test.wav");
  ASSERT_TRUE(proc.swapTone("blk-a", catalogTone(2, 200, "ir", verb.file)));
  ASSERT_TRUE(waitForSwap(proc, "blk-a", 200));
  EXPECT_EQ(savedBlock(savedState(proc), "blk-a").getProperty("type").toString(), "ir");
  EXPECT_EQ(proc.namEngineVoiceCount("blk-a"), 0);
  EXPECT_DOUBLE_EQ(proc.blockTailSeconds("blk-a"), loadedIrSeconds("reverb-ir-mono-test.wav"));
  EXPECT_TRUE(static_cast<bool>(chainStateBlock(proc, "blk-a")["irLong"]));

  // The NAM settings ride along on the IR block, in state and in the UI row.
  {
    const juce::ValueTree saved = savedBlock(savedState(proc), "blk-a");
    EXPECT_FALSE(static_cast<bool>(saved.getProperty("normalize")));
    EXPECT_DOUBLE_EQ(static_cast<double>(saved.getProperty("slimSize")), 1.0);
  }

  // IR -> NAM: back to an amp, with the settings it had before.
  const ScratchCopy amp("a2-amp-test.nam");
  ASSERT_TRUE(proc.swapTone("blk-a", catalogTone(3, 300, "nam", amp.file)));
  ASSERT_TRUE(waitForSwap(proc, "blk-a", 300));
  EXPECT_EQ(savedBlock(savedState(proc), "blk-a").getProperty("type").toString(), "nam");
  EXPECT_EQ(proc.namEngineVoiceCount("blk-a"), 1);
  EXPECT_DOUBLE_EQ(proc.blockTailSeconds("blk-a"), 0.0);
  const juce::var row = chainStateBlock(proc, "blk-a");
  EXPECT_FALSE(static_cast<bool>(row["irLong"]));
  EXPECT_FALSE(static_cast<bool>(row["params"]["normalize"]));
  EXPECT_DOUBLE_EQ(static_cast<double>(row["params"]["slimSize"]), 1.0);
}

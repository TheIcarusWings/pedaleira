// The block framework (plugin/include/BlockProcessor.h, BlockKinds.h): what
// every block kind beyond NAM and IR is built on. One shipped kind
// (utility.gain) and one test-only kind (test.latency, test_block_kinds.h)
// drive it end to end through the real processor:
//
//   - parameters: one described list per block, real-unit sets, clamping
//   - state: a kind's node saves and restores; a kind this build lacks
//     survives a round trip byte for byte as a bypassed placeholder
//   - creation: chain.addBlock, the audio a gain block produces, removal,
//     and add / remove / replace under running audio without a click
//   - latency: the report, the dry path of the mix, lane and branch alignment
//   - tails that follow a parameter, tempo and transport in the context,
//     the base-rate island, the failed-block path
//
// Exact comparisons are made against a second processor with an empty chain
// fed the same input: everything outside the chain (input stage, DC blocker,
// tone stack, output gain) is then identical on both sides, and linear, so a
// polarity flip, a power-of-two gain or a pure delay inside the chain comes
// out as exactly that.
#include "BlockKinds.h"
#include "EngineCommands.h"
#include "GainBlockProcessor.h"
#include "LegacyParamIds.h"
#include "Processor.h"
#include "chain_test_helpers.h"
#include "test_block_kinds.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr int kBlock = 256;
constexpr int kLatency = TestLatencyProcessor::kDefaultLatency;

// A prepared stereo-bus processor with the command set over it. The noise
// gate is off: it sits before the chain and is not what these tests measure.
struct Rig {
  explicit Rig(int oversample = 1) {
    proc.setMultiCoreEnabled(false, /*persist=*/false);
    proc.setPlayConfigDetails(2, 2, kFs, kBlock);
    proc.parameters.getParameter("gateEnabled")->setValueNotifyingHost(0.0f);
    if (oversample > 1) {
      proc.parameters.getParameter("osEnabled")->setValueNotifyingHost(1.0f);
      proc.parameters.getParameter("osFactor")
          ->setValueNotifyingHost((oversample == 2 ? 0 : oversample == 4 ? 1 : 2) / 2.0f);
    }
    prepare();
  }

  // Like a host (re)starting transport: every smoother snaps to its target
  // and every filter starts from rest, so two rigs prepared here and fed the
  // same input are comparable from the first sample.
  void prepare() { proc.prepareToPlay(kFs, kBlock); }

  CommandResult run(const char* name, const juce::String& argsJson = "{}") {
    return commands.registry().run(name, juce::JSON::parse(argsJson));
  }

  juce::var must(const char* name, const juce::String& argsJson = "{}") {
    const CommandResult result = run(name, argsJson);
    EXPECT_TRUE(result.ok) << name << " " << argsJson.toStdString() << ": "
                           << result.errorCode.toStdString() << ": "
                           << result.errorMessage.toStdString();
    return result.value;
  }

  // Adds a block of `kind` to the active lane's first free slot.
  std::string add(const char* kind) {
    const juce::var added = proc.addKindBlock(kind);
    EXPECT_TRUE(added["error"].toString().isEmpty()) << added["error"].toString().toStdString();
    return added["blockId"].toString().toStdString();
  }

  juce::var block(const std::string& blockId) {
    const juce::var state = proc.getChainState(-1);
    for (const char* key : {"chain", "chainRight"})
      if (const auto* lane = state[key].getArray())
        for (const auto& item : *lane)
          if (item["blockId"].toString() == juce::String(blockId))
            return item;
    return {};
  }

  ChainTestProcessor proc;
  EngineCommands commands{proc};
};

juce::String blockArgs(const std::string& blockId, const juce::String& rest = {}) {
  return "{\"blockId\":" + juce::JSON::toString(juce::var(juce::String(blockId))) + rest + "}";
}

std::vector<float> testSignal(int blocks = 60) {
  return makeSine(blocks * kBlock, 440.0, 0.25f);
}

// Both output channels of `rig` for the same signal on both inputs.
std::pair<std::vector<float>, std::vector<float>> render(Rig& rig, const std::vector<float>& in) {
  return processStereo(rig.proc, in, kBlock);
}

// An empty-chain reference render of `in` (see the file comment).
std::pair<std::vector<float>, std::vector<float>> renderReference(const std::vector<float>& in) {
  Rig reference;
  return render(reference, in);
}

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

// The bytes a node contributes to a saved state.
juce::MemoryBlock streamBytes(const juce::ValueTree& tree) {
  juce::MemoryOutputStream out;
  tree.writeToStream(out);
  return out.getMemoryBlock();
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
// (file:// URLs read straight from disk), so a swap to it needs no network.
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

// A scratch path for a fixture copy (the loader stamps what it reads). The
// file only exists once copyIn() ran, so a load before that fails.
struct ScratchFile {
  explicit ScratchFile(const char* fixtureName)
      : fixture(fixtureName),
        file(juce::File::getSpecialLocation(juce::File::tempDirectory)
                 .getChildFile("t3k-blockfw-" + juce::Uuid().toString() + "-" + fixtureName)) {}
  ~ScratchFile() { file.deleteFile(); }
  void copyIn() { EXPECT_TRUE(testFile(fixture).copyFileTo(file)); }
  const char* fixture;
  juce::File file;
};

// Polls a block's chain-state row until `done` says so.
template <typename Predicate>
bool waitForBlock(Rig& rig, const std::string& blockId, Predicate done, int timeoutMs = 20000) {
  const auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(timeoutMs);
  while (juce::Time::getMillisecondCounter() < deadline) {
    if (done(rig.block(blockId)))
      return true;
    juce::Thread::sleep(20);
  }
  return false;
}

// The tone swap onto `blockId` has landed on model `modelId`.
bool waitForSwap(Rig& rig, const std::string& blockId, int modelId) {
  return waitForBlock(rig, blockId, [modelId](const juce::var& row) {
    return row.isObject() && static_cast<int>(row["activeModelId"]) == modelId &&
           static_cast<bool>(row["loaded"]) && !static_cast<bool>(row["modelLoading"]);
  });
}

// Everything in flight has landed and every deferred report is delivered,
// as state.waitUntilLoaded does for a headless caller.
bool settle(Rig& rig) {
  const bool loaded = waitForChainLoaded(rig.proc);
  rig.proc.flushDeferredBlockRefresh();
  return loaded;
}

int peakIndex(const std::vector<float>& x) {
  int best = 0;
  for (int i = 0; i < static_cast<int>(x.size()); ++i)
    if (std::abs(x[static_cast<size_t>(i)]) > std::abs(x[static_cast<size_t>(best)]))
      best = i;
  return best;
}

juce::var paramNamed(const juce::var& list, const char* id) {
  if (const auto* params = list.getArray())
    for (const auto& param : *params)
      if (param["id"].toString() == id)
        return param;
  return {};
}

}  // namespace

// ---------------------------------------------------------------------------
// Kinds and parameters
// ---------------------------------------------------------------------------

TEST(BlockFrameworkTest, KindsListDescribesEveryRegisteredKind) {
  Rig rig;
  const juce::var listed = rig.must("kinds.list");

  juce::var gain, nam, ir;
  for (const auto& kind : *listed["kinds"].getArray()) {
    if (kind["id"].toString() == "utility.gain") gain = kind;
    if (kind["id"].toString() == "nam") nam = kind;
    if (kind["id"].toString() == "ir") ir = kind;
  }
  ASSERT_TRUE(gain.isObject());
  EXPECT_EQ(gain["name"].toString(), juce::String("Gain"));
  EXPECT_EQ(gain["category"].toString(), juce::String("utility"));
  EXPECT_TRUE(static_cast<bool>(gain["addable"]));
  ASSERT_EQ(gain["params"].size(), 3);

  const juce::var gainDb = paramNamed(gain["params"], "gainDb");
  EXPECT_EQ(gainDb["type"].toString(), juce::String("float"));
  EXPECT_EQ(gainDb["unit"].toString(), juce::String("dB"));
  EXPECT_DOUBLE_EQ(static_cast<double>(gainDb["min"]), -24.0);
  EXPECT_DOUBLE_EQ(static_cast<double>(gainDb["max"]), 24.0);
  EXPECT_DOUBLE_EQ(static_cast<double>(gainDb["default"]), 0.0);
  EXPECT_TRUE(static_cast<bool>(gainDb["automatable"]));
  EXPECT_FALSE(static_cast<bool>(gainDb["affectsTail"]));
  EXPECT_DOUBLE_EQ(static_cast<double>(paramNamed(gain["params"], "pan")["min"]), -1.0);
  EXPECT_EQ(paramNamed(gain["params"], "phaseInvert")["type"].toString(), juce::String("boolean"));

  EXPECT_EQ(gain["readings"].size(), 0);
  for (const auto& kind : *listed["kinds"].getArray()) {
    if (kind["id"].toString() != "dynamics.compressor")
      continue;
    ASSERT_EQ(kind["readings"].size(), 1);
    const juce::var reduction = kind["readings"][0];
    EXPECT_EQ(reduction["id"].toString(), juce::String("gainReductionDb"));
    EXPECT_EQ(reduction["unit"].toString(), juce::String("dB"));
    EXPECT_DOUBLE_EQ(static_cast<double>(reduction["min"]), 0.0);
    EXPECT_DOUBLE_EQ(static_cast<double>(reduction["max"]), 60.0);
  }

  // The model-based kinds are listed, but only a tone makes one.
  ASSERT_TRUE(nam.isObject());
  ASSERT_TRUE(ir.isObject());
  EXPECT_FALSE(static_cast<bool>(nam["addable"]));
  EXPECT_EQ(nam["category"].toString(), juce::String("amp"));
  EXPECT_EQ(ir["category"].toString(), juce::String("cab"));

  // The flags a kind sets reach the listing.
  for (const auto& kind : *listed["kinds"].getArray())
    if (kind["id"].toString() == "test.latency") {
      EXPECT_TRUE(static_cast<bool>(paramNamed(kind["params"], "tailMs")["affectsTail"]));
      EXPECT_TRUE(
          static_cast<bool>(paramNamed(kind["params"], "latencySamples")["affectsLatency"]));
    }

  EXPECT_EQ(listed["commonParams"].size(), 5);
}

TEST(BlockFrameworkTest, RegistryRefusesIdsThatCollide) {
  auto& registry = BlockKindRegistry::instance();
  const auto factory = [] { return std::make_unique<GainBlockProcessor>(); };
  // Taken id.
  EXPECT_FALSE(registry.add({"utility.gain", "Again", BlockCategory::Utility, {}, factory, true}));
  // A kind parameter that would shadow a common one in block.setParam.
  EXPECT_FALSE(registry.add({"test.shadow", "Shadow", BlockCategory::Utility,
                             {BlockParamSpec::floatParam("mix", "Mix", "", 0.0f, 1.0f, 1.0f)},
                             factory, true}));
  EXPECT_EQ(registry.find("test.shadow"), nullptr);
  EXPECT_EQ(registry.create("no.such.kind"), nullptr);
  EXPECT_EQ(registry.create("nam"), nullptr) << "a model-based kind is not built by id";
}

TEST(BlockFrameworkTest, ParameterListSetClampAndErrors) {
  Rig rig;
  const juce::var added = rig.must("chain.addBlock", R"({"kind":"utility.gain"})");
  const std::string id = added["blockId"].toString().toStdString();
  ASSERT_FALSE(id.empty());
  EXPECT_EQ(added["block"]["kindId"].toString(), juce::String("utility.gain"));
  EXPECT_EQ(added["block"]["category"].toString(), juce::String("utility"));
  EXPECT_TRUE(static_cast<bool>(added["block"]["loaded"]));

  // One list: the five common settings, then the kind's own.
  const juce::var listed = rig.must("block.listParams", blockArgs(id));
  EXPECT_EQ(listed["kindId"].toString(), juce::String("utility.gain"));
  const auto* params = listed["params"].getArray();
  ASSERT_NE(params, nullptr);
  ASSERT_EQ(params->size(), 8);
  const char* expected[] = {"enabled", "normalize",  "inputGain", "outputGain",
                            "mix",     "gainDb",     "pan",       "phaseInvert"};
  for (int i = 0; i < 8; ++i) {
    EXPECT_EQ((*params)[i]["id"].toString(), juce::String(expected[i]));
    EXPECT_EQ((*params)[i]["scope"].toString(), juce::String(i < 5 ? "common" : "kind"));
  }
  EXPECT_DOUBLE_EQ(static_cast<double>(paramNamed(listed["params"], "mix")["value"]), 1.0);
  EXPECT_DOUBLE_EQ(static_cast<double>(paramNamed(listed["params"], "inputGain")["value"]), 0.5);
  EXPECT_DOUBLE_EQ(static_cast<double>(paramNamed(listed["params"], "gainDb")["value"]), 0.0);

  // A real-unit value goes in and comes back as itself, everywhere it shows.
  const juce::var set =
      rig.must("block.setParam", blockArgs(id, R"(,"param":"gainDb","value":3.5)"));
  EXPECT_DOUBLE_EQ(static_cast<double>(set["value"]), 3.5);
  EXPECT_FALSE(static_cast<bool>(set["clamped"]));
  EXPECT_DOUBLE_EQ(static_cast<double>(set["block"]["kindParams"]["gainDb"]), 3.5);
  EXPECT_DOUBLE_EQ(static_cast<double>(paramNamed(
                       rig.must("block.listParams", blockArgs(id))["params"], "gainDb")["value"]),
                   3.5);
  const juce::var chain = rig.must("state.get")["chain"]["chain"];
  bool inState = false;
  for (const auto& item : *chain.getArray())
    if (item["blockId"].toString() == juce::String(id)) {
      inState = true;
      EXPECT_EQ(item["kindId"].toString(), juce::String("utility.gain"));
      EXPECT_EQ(item["category"].toString(), juce::String("utility"));
      EXPECT_DOUBLE_EQ(static_cast<double>(item["kindParams"]["gainDb"]), 3.5);
      EXPECT_DOUBLE_EQ(static_cast<double>(item["kindParams"]["pan"]), 0.0);
    }
  EXPECT_TRUE(inState);

  // Out of range clamps and says so; a switch is 0 or 1.
  const juce::var clamped =
      rig.must("block.setParam", blockArgs(id, R"(,"param":"gainDb","value":99)"));
  EXPECT_DOUBLE_EQ(static_cast<double>(clamped["value"]), 24.0);
  EXPECT_TRUE(static_cast<bool>(clamped["clamped"]));
  EXPECT_DOUBLE_EQ(static_cast<double>(rig.must(
                       "block.setParam", blockArgs(id, R"(,"param":"pan","value":-7)"))["value"]),
                   -1.0);
  EXPECT_DOUBLE_EQ(
      static_cast<double>(rig.must(
          "block.setParam", blockArgs(id, R"(,"param":"phaseInvert","value":0.7)"))["value"]),
      1.0);

  // The common settings still take knob positions, and still refuse others.
  const juce::var mixed = rig.must("block.setParam", blockArgs(id, R"(,"param":"mix","value":0.25)"));
  EXPECT_DOUBLE_EQ(static_cast<double>(mixed["block"]["params"]["mix"]), 0.25);
  EXPECT_EQ(rig.run("block.setParam", blockArgs(id, R"(,"param":"mix","value":1.5)")).errorCode,
            juce::String(CommandError::invalidArgs));

  // Unknown ids: one no kind has, and one another kind has.
  EXPECT_EQ(rig.run("block.setParam", blockArgs(id, R"(,"param":"volume","value":1)")).errorCode,
            juce::String(CommandError::invalidArgs));
  EXPECT_EQ(rig.run("block.setParam", blockArgs(id, R"(,"param":"tailMs","value":1)")).errorCode,
            juce::String(CommandError::invalidArgs));
  EXPECT_FALSE(rig.proc.setBlockParam(id, "tailMs", 1.0));
  EXPECT_EQ(rig.run("block.setParam", blockArgs("no-such", R"(,"param":"gainDb","value":1)"))
                .errorCode,
            juce::String(CommandError::notFound));
  EXPECT_EQ(rig.run("block.listParams", blockArgs("no-such")).errorCode,
            juce::String(CommandError::notFound));

  // chain.addBlock's own errors.
  EXPECT_EQ(rig.run("chain.addBlock", R"({"kind":"no.such.kind"})").errorCode,
            juce::String(CommandError::invalidArgs));
  EXPECT_EQ(rig.run("chain.addBlock", R"({"kind":"nam"})").errorCode,
            juce::String(CommandError::invalidArgs));
  EXPECT_EQ(rig.run("chain.addBlock", R"({"kind":"utility.gain","target":"no-such"})").errorCode,
            juce::String(CommandError::notFound));
  EXPECT_EQ(rig.run("chain.addBlock", R"({"kind":"utility.gain","lane":"right"})").errorCode,
            juce::String(CommandError::failed));
}

TEST(BlockFrameworkTest, AudioThreadReadsWhatWasSet) {
  // The processor on its own: a set is a lock-free store, and the next
  // process call runs with it.
  GainBlockProcessor gain;
  BlockPrepareSpec spec;
  spec.chainSampleRate = kFs;
  spec.baseBlockSize = spec.maxBlockSize = kBlock;

  EXPECT_EQ(gain.findParameter("gainDb"), GainBlockProcessor::kGainDb);
  EXPECT_EQ(gain.findParameter("nope"), -1);
  gain.setParameter(GainBlockProcessor::kGainDb, 1000.0f);
  EXPECT_FLOAT_EQ(gain.getParameter(GainBlockProcessor::kGainDb), 24.0f);
  gain.setParameter(GainBlockProcessor::kGainDb, 0.0f);
  gain.setParameter(99, 1.0f);  // outside the table: ignored

  gain.prepareForChain(spec);
  juce::AudioBuffer<float> buffer(2, kBlock);
  const BlockProcessContext ctx;
  const auto fill = [&buffer] {
    for (int ch = 0; ch < 2; ++ch)
      for (int i = 0; i < kBlock; ++i)
        buffer.setSample(ch, i, 0.25f);
  };

  // Defaults: untouched.
  fill();
  EXPECT_EQ(gain.processInChain(buffer, ctx), BlockProcessResult::Processed);
  EXPECT_EQ(buffer.getSample(0, kBlock - 1), 0.25f);

  // Set between two callbacks: the next one ramps to it (20 ms), and a few
  // callbacks later the value is exact.
  gain.setParameter(GainBlockProcessor::kPhaseInvert, 1.0f);
  for (int i = 0; i < 8; ++i) {
    fill();
    gain.processInChain(buffer, ctx);
  }
  EXPECT_EQ(buffer.getSample(0, kBlock - 1), -0.25f);
  EXPECT_EQ(buffer.getSample(1, kBlock - 1), -0.25f);
}

// ---------------------------------------------------------------------------
// The gain kind in a rig
// ---------------------------------------------------------------------------

TEST(BlockFrameworkTest, GainBlockAudioIsExact) {
  const auto in = testSignal();
  const auto reference = renderReference(in);

  // At its defaults the block is a bit-exact pass-through.
  {
    Rig rig;
    rig.add("utility.gain");
    rig.prepare();
    EXPECT_EQ(render(rig, in), reference);
  }

  // Polarity: every sample negated, exactly.
  {
    Rig rig;
    const std::string id = rig.add("utility.gain");
    ASSERT_TRUE(rig.proc.setBlockParam(id, "phaseInvert", 1.0));
    rig.prepare();
    const auto out = render(rig, in);
    for (size_t i = 0; i < in.size(); ++i) {
      ASSERT_EQ(out.first[i], -reference.first[i]) << "left, frame " << i;
      ASSERT_EQ(out.second[i], -reference.second[i]) << "right, frame " << i;
    }
  }

  // Pan hard right: the left channel is silent, the right untouched. Pan
  // half left: the right channel at exactly half.
  {
    Rig rig;
    const std::string id = rig.add("utility.gain");
    ASSERT_TRUE(rig.proc.setBlockParam(id, "pan", 1.0));
    rig.prepare();
    const auto out = render(rig, in);
    for (size_t i = 0; i < in.size(); ++i)
      ASSERT_EQ(out.first[i], 0.0f) << "frame " << i;
    EXPECT_EQ(out.second, reference.second);
  }
  {
    Rig rig;
    const std::string id = rig.add("utility.gain");
    ASSERT_TRUE(rig.proc.setBlockParam(id, "pan", -0.5));
    rig.prepare();
    const auto out = render(rig, in);
    EXPECT_EQ(out.first, reference.first);
    for (size_t i = 0; i < in.size(); ++i)
      ASSERT_EQ(out.second[i], 0.5f * reference.second[i]) << "frame " << i;
  }

  // +6 dB at mix 100%: the reference times 10^(6/20). Not a power of two, so
  // the two sides round differently, and the 5 Hz DC blocker after the
  // chain integrates that rounding (its pole sits at 0.9993): a few 1e-5 on
  // a 0.5 peak. A gain 0.01 dB off would miss by 6e-4.
  Rig rig;
  const std::string id = rig.add("utility.gain");
  ASSERT_TRUE(rig.proc.setBlockParam(id, "gainDb", 6.0));
  rig.prepare();
  const double sixDb = std::pow(10.0, 6.0 / 20.0);
  {
    const auto out = render(rig, in);
    double worst = 0.0;
    for (size_t i = 0; i < in.size(); ++i)
      worst = std::max(worst, std::abs(out.first[i] - sixDb * reference.first[i]));
    EXPECT_LT(worst, 1e-4) << "+6 dB block is off the expected level";
    EXPECT_GT(std::abs(out.first[1000]), 1.9 * std::abs(reference.first[1000]));
  }

  // Removal: the chain is the empty chain again, to the bit.
  letAudioGoIdle();
  ASSERT_TRUE(rig.proc.removeChainBlock(id));
  rig.prepare();
  EXPECT_EQ(render(rig, in), reference) << "a removed block left something behind";
}

TEST(BlockFrameworkTest, AddRemoveAndReplaceUnderAudioDoNotClick) {
  Rig rig;

  // A host-style render thread on a steady sine (250 Hz loops seamlessly
  // over 93 blocks of 256: 124 whole cycles), tracking the largest
  // sample-to-sample step and the loudest sample.
  std::atomic<bool> stop{false};
  std::atomic<float> maxStep{0.0f}, maxLevel{0.0f};
  std::thread pump([&] {
    const int loopFrames = 93 * kBlock;
    const auto sine = makeSine(loopFrames, 250.0, 0.1f);
    juce::AudioBuffer<float> buffer(2, kBlock);
    juce::MidiBuffer midi;
    int offset = 0;
    float previous = 0.0f, step = 0.0f, level = 0.0f;
    while (!stop.load()) {
      buffer.copyFrom(0, 0, sine.data() + offset, kBlock);
      buffer.copyFrom(1, 0, sine.data() + offset, kBlock);
      offset = (offset + kBlock) % loopFrames;
      rig.proc.processBlock(buffer, midi);
      const float* y = buffer.getReadPointer(0);
      for (int i = 0; i < kBlock; ++i) {
        step = std::max(step, std::abs(y[i] - previous));
        level = std::max(level, std::abs(y[i]));
        previous = y[i];
      }
      maxStep.store(step);
      maxLevel.store(level);
      juce::Thread::sleep(1);  // ~5x real time; keeps the mutex uncontended
    }
  });
  const auto settle = [&rig](int ms) {
    juce::Thread::sleep(ms);
    const auto deadline = juce::Time::getMillisecondCounter() + 5000;
    while (rig.proc.isChainEditFadeHeld() && juce::Time::getMillisecondCounter() < deadline)
      juce::Thread::sleep(10);
    juce::Thread::sleep(ms);
  };

  settle(100);
  // Add (fades in from bypass), then make it audible: +12 dB.
  const std::string id = rig.add("utility.gain");
  settle(100);
  ASSERT_TRUE(rig.proc.setBlockParam(id, "gainDb", 12.0));
  settle(100);
  // Remove (glides to bypass), bring it back by undo (a chain restore),
  // then replace it in place with a fresh one at unity (wet mute and swap).
  ASSERT_TRUE(rig.proc.removeChainBlock(id));
  settle(100);
  ASSERT_TRUE(rig.proc.undoChain());
  settle(150);
  ASSERT_TRUE(static_cast<bool>(rig.block(id)["loaded"]));
  const juce::var replaced = rig.proc.addKindBlock("utility.gain", id);
  EXPECT_EQ(replaced["blockId"].toString().toStdString(), id);
  settle(100);
  ASSERT_TRUE(rig.proc.removeChainBlock(id));
  settle(100);

  stop.store(true);
  pump.join();

  // The sine at +12 dB moves at most 0.4 * 2*pi*250/48000 = 0.013 per
  // sample; a 25 ms fade adds next to nothing. A splice between the two
  // levels would step by up to 0.3.
  EXPECT_GT(maxLevel.load(), 0.35f) << "the +12 dB block was never heard";
  EXPECT_LT(maxStep.load(), 0.03f) << "an edit under running audio spliced the waveform";
}

TEST(BlockFrameworkTest, ReplacingAToneBlockKeepsItsSlotAndSettings) {
  Rig rig;
  rig.proc.restoreFromTree(monoSnapshot({makeIrBlockTree("blk-cab", 2, 200)}));
  ASSERT_TRUE(waitForChainLoaded(rig.proc));
  ASSERT_TRUE(rig.proc.setBlockParam("blk-cab", "outputGain", 0.4));
  letAudioGoIdle();

  const juce::var replaced =
      rig.must("chain.addBlock", R"({"kind":"utility.gain","target":"blk-cab"})");
  EXPECT_EQ(replaced["blockId"].toString(), juce::String("blk-cab"));
  EXPECT_EQ(replaced["block"]["kindId"].toString(), juce::String("utility.gain"));
  EXPECT_NEAR(static_cast<double>(replaced["block"]["params"]["outputGain"]), 0.4, 1e-6);
  EXPECT_EQ(savedBlock(savedState(rig.proc), "blk-cab").getProperty("type").toString(),
            juce::String("kind"));

  // And back by undo: an IR block again. Like undoing a removed tone it
  // reloads from its URL (snapshots carry no model bytes), which this
  // fixture's fake URL cannot serve, so only the load's end is awaited.
  ASSERT_TRUE(rig.proc.undoChain());
  EXPECT_EQ(rig.block("blk-cab")["kindId"].toString(), juce::String("ir"));
  EXPECT_EQ(savedBlock(savedState(rig.proc), "blk-cab").getProperty("type").toString(),
            juce::String("ir"));
  const auto deadline = juce::Time::getMillisecondCounter() + 20000;
  while (static_cast<bool>(rig.block("blk-cab")["modelLoading"]) &&
         juce::Time::getMillisecondCounter() < deadline)
    juce::Thread::sleep(20);
  EXPECT_FALSE(static_cast<bool>(rig.block("blk-cab")["modelLoading"]));
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

TEST(BlockFrameworkTest, KindStateSavesAndRestores) {
  Rig rig;
  rig.proc.restoreFromTree(monoSnapshot({makeIrBlockTree("blk-cab", 2, 200)}));
  ASSERT_TRUE(waitForChainLoaded(rig.proc));
  const std::string id = rig.add("utility.gain");
  ASSERT_TRUE(rig.proc.setBlockParam(id, "gainDb", -4.5));
  ASSERT_TRUE(rig.proc.setBlockParam(id, "pan", 0.25));
  ASSERT_TRUE(rig.proc.setBlockParam(id, "phaseInvert", 1.0));
  ASSERT_TRUE(rig.proc.setBlockParam(id, "mix", 0.7));
  ASSERT_TRUE(rig.proc.setBlockParam(id, "enabled", 0.0));

  const juce::ValueTree first = savedState(rig.proc);
  const juce::ValueTree node = savedBlock(first, juce::String(id));
  ASSERT_TRUE(node.isValid());

  // The kind block's node: the properties every block carries, and ONE
  // child for everything the kind owns.
  EXPECT_EQ(propertyNames(node),
            (std::vector<std::string>{"id", "type", "enabled", "normalize", "slimSize",
                                      "inputGain", "outputGain", "mix"}));
  EXPECT_EQ(node.getProperty("type").toString(), juce::String("kind"));
  EXPECT_EQ(childTypes(node), (std::vector<std::string>{"Eq", "KindState"}));
  const juce::ValueTree kindState = node.getChildWithName("KindState");
  EXPECT_EQ(propertyNames(kindState), (std::vector<std::string>{"kind", "version", "name"}));
  EXPECT_EQ(kindState.getProperty("kind").toString(), juce::String("utility.gain"));
  EXPECT_TRUE(kindState.getProperty("version").isInt());
  EXPECT_EQ(static_cast<int>(kindState.getProperty("version")), 1);
  EXPECT_EQ(kindState.getProperty("name").toString(), juce::String("Gain"));
  ASSERT_EQ(kindState.getNumChildren(), 3);
  EXPECT_EQ(kindState.getChild(0).getProperty("id").toString(), juce::String("gainDb"));
  EXPECT_DOUBLE_EQ(static_cast<double>(kindState.getChild(0).getProperty("value")), -4.5);

  // The legacy node next to it is what it has always been.
  const juce::ValueTree cab = savedBlock(first, "blk-cab");
  EXPECT_EQ(propertyNames(cab),
            (std::vector<std::string>{"id", "type", "enabled", "normalize", "slimSize",
                                      "inputGain", "outputGain", "mix", "toneId", "toneJson",
                                      "activeModelId"}));
  EXPECT_EQ(childTypes(cab), (std::vector<std::string>{"Eq", "ModelCache"}));

  // Into a fresh processor and out again: the same tree, the same values.
  juce::MemoryBlock blob;
  rig.proc.getStateInformation(blob);
  Rig reopened;
  reopened.proc.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
  ASSERT_TRUE(waitForChainLoaded(reopened.proc));
  const juce::ValueTree second = savedState(reopened.proc);
  EXPECT_TRUE(first.getChildWithName("ChainSnapshot")
                  .isEquivalentTo(second.getChildWithName("ChainSnapshot")))
      << "the chain snapshot changed across a save/restore round trip";

  const juce::var row = reopened.block(id);
  EXPECT_EQ(row["kindId"].toString(), juce::String("utility.gain"));
  EXPECT_TRUE(static_cast<bool>(row["loaded"]));
  EXPECT_DOUBLE_EQ(static_cast<double>(row["kindParams"]["gainDb"]), -4.5);
  EXPECT_DOUBLE_EQ(static_cast<double>(row["kindParams"]["pan"]), 0.25);
  EXPECT_DOUBLE_EQ(static_cast<double>(row["kindParams"]["phaseInvert"]), 1.0);
  EXPECT_NEAR(static_cast<double>(row["params"]["mix"]), 0.7, 1e-6);
  EXPECT_FALSE(static_cast<bool>(row["params"]["enabled"]));
}

TEST(BlockFrameworkTest, LegacyKindIdsLoadUnderTheirNewIds) {
  // Freeze, Bit Crusher, Stutter and Ring Modulator were saved as filter.*
  // before the Creative category; a state that says so still loads them.
  const std::pair<const char*, const char*> moved[] = {{"filter.freeze", "creative.freeze"},
                                                       {"filter.bitcrush", "creative.bitcrush"},
                                                       {"filter.glitch", "creative.glitch"},
                                                       {"filter.ringmod", "creative.ringmod"}};
  for (const auto& [legacy, current] : moved) {
    SCOPED_TRACE(legacy);
    EXPECT_EQ(t3k::legacy_ids::currentKindId(legacy), juce::String(current));
    EXPECT_EQ(t3k::legacy_ids::currentKindId(current), juce::String(current));
    ASSERT_NE(BlockKindRegistry::instance().find(current), nullptr);
    EXPECT_EQ(BlockKindRegistry::instance().find(legacy), BlockKindRegistry::instance().find(current));

    Rig rig;
    const std::string id = rig.add(current);
    ASSERT_FALSE(id.empty());
    juce::ValueTree state = savedState(rig.proc);
    ASSERT_TRUE(state.isValid());
    // Written the way an older build wrote it.
    juce::ValueTree kindState = savedBlock(state, juce::String(id)).getChildWithName("KindState");
    ASSERT_TRUE(kindState.isValid());
    EXPECT_EQ(kindState.getProperty("kind").toString(), juce::String(current));
    kindState.setProperty("kind", legacy, nullptr);
    juce::MemoryOutputStream out;
    out.write("T3KB", 4);
    state.writeToStream(out);

    Rig reopened;
    reopened.proc.setStateInformation(out.getData(), static_cast<int>(out.getDataSize()));
    ASSERT_TRUE(waitForChainLoaded(reopened.proc));
    const juce::var row = reopened.block(id);
    EXPECT_EQ(row["kindId"].toString(), juce::String(current));
    EXPECT_TRUE(static_cast<bool>(row["loaded"]));
    // And it saves under the current id from then on.
    EXPECT_EQ(savedBlock(savedState(reopened.proc), juce::String(id))
                  .getChildWithName("KindState")
                  .getProperty("kind")
                  .toString(),
              juce::String(current));
  }
  // Ids that were never renamed pass through, a tree without kinds is left alone.
  EXPECT_EQ(t3k::legacy_ids::currentKindId("filter.wah"), juce::String("filter.wah"));
  EXPECT_EQ(t3k::legacy_ids::migrateKindIds(juce::ValueTree("ChainSnapshot")), 0);
}

TEST(BlockFrameworkTest, KindParamsUndoRedoAndDuplicate) {
  Rig rig;
  const std::string id = rig.add("utility.gain");
  ASSERT_TRUE(rig.proc.setBlockParam(id, "gainDb", 9.0));
  EXPECT_DOUBLE_EQ(static_cast<double>(rig.block(id)["kindParams"]["gainDb"]), 9.0);

  // Undo the knob move (the block stays, its value goes back), then the add.
  ASSERT_TRUE(rig.proc.undoChain());
  EXPECT_DOUBLE_EQ(static_cast<double>(rig.block(id)["kindParams"]["gainDb"]), 0.0);
  ASSERT_TRUE(rig.proc.undoChain());
  EXPECT_TRUE(rig.block(id).isVoid());
  // Redo brings the block back: its processor is built off the chain lock
  // (the block loads, like a tone block), and the second redo may well find
  // it still on its way and has to supersede that build with its own.
  ASSERT_TRUE(rig.proc.redoChain());
  ASSERT_TRUE(rig.proc.redoChain());
  ASSERT_TRUE(settle(rig));
  EXPECT_DOUBLE_EQ(static_cast<double>(rig.block(id)["kindParams"]["gainDb"]), 9.0);
  EXPECT_TRUE(static_cast<bool>(rig.block(id)["loaded"]));

  // A clone is the same kind with the same values.
  const std::string clone = rig.proc.duplicateChainBlock(id, "left", 1);
  ASSERT_FALSE(clone.empty());
  ASSERT_TRUE(settle(rig));
  const juce::var cloned = rig.block(clone);
  EXPECT_EQ(cloned["kindId"].toString(), juce::String("utility.gain"));
  EXPECT_TRUE(static_cast<bool>(cloned["loaded"]));
  EXPECT_FALSE(static_cast<bool>(cloned["modelLoading"]));
  EXPECT_DOUBLE_EQ(static_cast<double>(cloned["kindParams"]["gainDb"]), 9.0);
  EXPECT_TRUE(waitForChainLoaded(rig.proc));
}

TEST(BlockFrameworkTest, UnknownKindSurvivesARoundTripAsABypassedPlaceholder) {
  // A block as a newer build (or a build with a plugin this one lacks) would
  // have saved it: a kind nobody here has registered, a state version from
  // the future, an opaque chunk, and a property and a child this build has
  // never heard of.
  juce::ValueTree future("ChainBlock");
  future.setProperty("id", "blk-future", nullptr);
  future.setProperty("type", "kind", nullptr);
  future.setProperty("enabled", true, nullptr);
  future.setProperty("normalize", true, nullptr);
  future.setProperty("slimSize", 0.0, nullptr);
  future.setProperty("inputGain", 0.625, nullptr);
  future.setProperty("outputGain", 0.375, nullptr);
  future.setProperty("mix", 0.75, nullptr);
  future.setProperty("colour", "teal", nullptr);
  juce::ValueTree kindState("KindState");
  kindState.setProperty("kind", "future.shimmer", nullptr);
  kindState.setProperty("version", 7, nullptr);
  kindState.setProperty("name", "Shimmer", nullptr);
  const unsigned char chunkBytes[] = {0x00, 0xff, 0x10, 0x20, 0x00, 0x42, 0x99, 0x01};
  kindState.setProperty("chunk", juce::var(juce::MemoryBlock(chunkBytes, sizeof(chunkBytes))),
                        nullptr);
  juce::ValueTree decay("Param");
  decay.setProperty("id", "decay", nullptr);
  decay.setProperty("value", 3.5, nullptr);
  kindState.appendChild(decay, nullptr);
  future.appendChild(kindState, nullptr);
  juce::ValueTree extra("Modulation");
  extra.setProperty("source", "lfo1", nullptr);
  future.appendChild(extra, nullptr);

  const auto in = testSignal(20);
  const auto reference = renderReference(in);

  Rig rig;
  rig.proc.restoreFromTree(monoSnapshot({future, makeIrBlockTree("blk-cab", 2, 200)}));
  ASSERT_TRUE(waitForChainLoaded(rig.proc));

  // Not dropped: a placeholder under its saved name, where it was.
  const juce::var row = rig.block("blk-future");
  ASSERT_TRUE(row.isObject());
  EXPECT_EQ(row["kind"].toString(), juce::String("tone"));
  EXPECT_TRUE(static_cast<bool>(row["missing"]));
  EXPECT_FALSE(static_cast<bool>(row["loaded"]));
  EXPECT_EQ(row["kindId"].toString(), juce::String("future.shimmer"));
  EXPECT_EQ(row["tone"]["title"].toString(), juce::String("Shimmer"));
  const juce::var chain = rig.proc.getChainState(-1)["chain"];
  EXPECT_EQ((*chain.getArray())[0]["blockId"].toString(), juce::String("blk-future"));
  EXPECT_EQ(rig.proc.getLatencySamples(), 0);

  // It cannot be edited (the edit could not be saved), and a wait for the
  // rig to load is not held up by it.
  EXPECT_FALSE(rig.proc.setBlockParam("blk-future", "mix", 0.1));
  EXPECT_FALSE(rig.proc.setBlockEqBand("blk-future", 2, eqBandVar("bell", 900.0, 4.5, 1.4)));
  EXPECT_FALSE(rig.proc.setBlockEqEnabled("blk-future", true));
  EXPECT_FALSE(rig.proc.setBlockEqPre("blk-future", true));
  EXPECT_FALSE(rig.proc.resetBlockEq("blk-future"));
  EXPECT_EQ(rig.run("block.setEqBand", blockArgs("blk-future", R"(,"band":2,"gainDb":3)")).errorCode,
            juce::String(CommandError::failed));
  EXPECT_TRUE(static_cast<bool>(rig.must("state.waitUntilLoaded", R"({"timeoutMs":5000})")["loaded"]));

  // Saved back exactly as it came: the same tree, the same bytes.
  const juce::MemoryBlock originalBytes = streamBytes(future);
  {
    const juce::ValueTree saved = savedBlock(savedState(rig.proc), "blk-future");
    ASSERT_TRUE(saved.isValid());
    EXPECT_TRUE(saved.isEquivalentTo(future));
    EXPECT_TRUE(streamBytes(saved) == originalBytes) << "the unknown block's bytes changed";
  }

  // Still exactly that after a second generation, and after an edit
  // elsewhere in the chain and its undo (snapshots carry the node too).
  juce::MemoryBlock blob;
  rig.proc.getStateInformation(blob);
  Rig reopened;
  reopened.proc.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
  ASSERT_TRUE(waitForChainLoaded(reopened.proc));
  const std::string gain = reopened.add("utility.gain");
  EXPECT_FALSE(gain.empty());
  ASSERT_TRUE(reopened.proc.undoChain());
  ASSERT_TRUE(waitForChainLoaded(reopened.proc));
  EXPECT_TRUE(static_cast<bool>(reopened.block("blk-future")["missing"]));
  EXPECT_TRUE(streamBytes(savedBlock(savedState(reopened.proc), "blk-future")) == originalBytes);

  // Bypassed: with the cab out of the way the rig is the empty chain.
  ASSERT_TRUE(reopened.proc.removeChainBlock("blk-cab"));
  reopened.prepare();
  EXPECT_EQ(render(reopened, in), reference) << "the placeholder is not a clean bypass";

  // The user can still take it out.
  letAudioGoIdle();
  EXPECT_TRUE(reopened.proc.removeChainBlock("blk-future"));
  EXPECT_TRUE(reopened.block("blk-future").isVoid());
}

// ---------------------------------------------------------------------------
// Latency
// ---------------------------------------------------------------------------

TEST(BlockFrameworkTest, BlockLatencyIsReportedAndTheDryPathFollows) {
  const auto in = testSignal();
  const auto reference = renderReference(in);

  Rig rig;
  EXPECT_EQ(rig.proc.getLatencySamples(), 0);
  const std::string id = rig.add("test.latency");
  EXPECT_EQ(rig.proc.blockLatencySamples(id), kLatency);
  EXPECT_EQ(rig.proc.getLatencySamples(), kLatency) << "block latency missing from the report";
  EXPECT_EQ(static_cast<int>(rig.must("state.get")["engine"]["latencySamples"]), kLatency);

  // What the output must be at any mix: the dry signal delayed by the
  // block's latency, since the wet path is that same delay. Half of one
  // plus half of the other is exactly the delayed signal; an undelayed dry
  // path would comb it instead.
  const auto expectDelayed = [&](const std::vector<float>& out, double tolerance) {
    double worst = 0.0;
    for (size_t i = 0; i < out.size(); ++i) {
      const float expected = i >= static_cast<size_t>(kLatency)
                                 ? reference.first[i - static_cast<size_t>(kLatency)]
                                 : 0.0f;
      worst = std::max(worst, static_cast<double>(std::abs(out[i] - expected)));
    }
    EXPECT_LE(worst, tolerance);
  };

  ASSERT_TRUE(rig.proc.setBlockParam(id, "mix", 0.5));
  rig.prepare();
  expectDelayed(render(rig, in).first, 0.0);

  ASSERT_TRUE(rig.proc.setBlockParam(id, "mix", 0.3));
  rig.prepare();
  // 0.7 * x + 0.3 * x is not exactly x in float, and how far off depends on
  // whether the compiler fuses the multiply-add: allow a few ulps.
  expectDelayed(render(rig, in).first, 1e-6);

  // Bypassed, the block still delays: the lane's latency follows the
  // chain's structure, not the power switch.
  ASSERT_TRUE(rig.proc.setBlockParam(id, "enabled", 0.0));
  rig.prepare();
  expectDelayed(render(rig, in).first, 0.0);
  EXPECT_EQ(rig.proc.getLatencySamples(), kLatency);

  // A latency that follows a parameter is adopted, by the kind and by the
  // chain together, once the edit settles.
  ASSERT_TRUE(rig.proc.setBlockParam(id, "latencySamples", 128.0));
  EXPECT_EQ(rig.proc.blockLatencySamples(id), kLatency) << "the kind moved on its own";
  EXPECT_EQ(rig.proc.getLatencySamples(), kLatency) << "re-reported before the gesture settled";
  rig.proc.flushDeferredBlockRefresh();
  EXPECT_EQ(rig.proc.blockLatencySamples(id), 128);
  EXPECT_EQ(rig.proc.getLatencySamples(), 128);

  // Gone with the block.
  letAudioGoIdle();
  ASSERT_TRUE(rig.proc.removeChainBlock(id));
  EXPECT_EQ(rig.proc.getLatencySamples(), 0);
  ASSERT_TRUE(rig.proc.undoChain());
  ASSERT_TRUE(settle(rig));  // the restored block is built in the background
  EXPECT_EQ(rig.proc.getLatencySamples(), 128);
}

TEST(BlockFrameworkTest, LanesAndBranchStayAlignedAroundALatentBlock) {
  // An impulse on both channels at frame 100.
  std::vector<float> impulse(static_cast<size_t>(8 * kBlock), 0.0f);
  impulse[100] = 0.5f;

  // Two independent lanes, latency in the left one only: the right lane
  // waits for it, and both channels come out together.
  {
    Rig rig;
    rig.proc.setStereoMode(true);
    rig.proc.setActiveEditChain("left");
    rig.add("test.latency");
    EXPECT_EQ(rig.proc.getLatencySamples(), kLatency);
    rig.prepare();
    const auto out = render(rig, impulse);
    EXPECT_EQ(peakIndex(out.first), 100 + kLatency);
    EXPECT_EQ(peakIndex(out.second), 100 + kLatency) << "the lane without latency ran ahead";
    EXPECT_EQ(out.first, out.second);
  }

  // The same with the latency in the right lane.
  {
    Rig rig;
    rig.proc.setStereoMode(true);
    rig.proc.setActiveEditChain("right");
    rig.add("test.latency");
    EXPECT_EQ(rig.proc.getLatencySamples(), kLatency);
    rig.prepare();
    const auto out = render(rig, impulse);
    EXPECT_EQ(peakIndex(out.first), 100 + kLatency);
    EXPECT_EQ(peakIndex(out.second), 100 + kLatency);
  }

  // Branched: the right lane taps the left after a gain block; the latent
  // block sits in the trunk's remainder, so the branch waits for it.
  {
    Rig rig;
    rig.proc.setStereoMode(true);
    rig.proc.setActiveEditChain("left");
    const std::string tap = rig.add("utility.gain");
    rig.add("test.latency");
    ASSERT_TRUE(rig.proc.setChainBranch("left", tap));
    EXPECT_EQ(rig.proc.getLatencySamples(), kLatency);
    rig.prepare();
    const auto out = render(rig, impulse);
    EXPECT_EQ(peakIndex(out.first), 100 + kLatency);
    EXPECT_EQ(peakIndex(out.second), 100 + kLatency) << "the branch ran ahead of the trunk";

    // Before the tap instead, it delays both paths alike and nothing pads.
    letAudioGoIdle();
    ASSERT_TRUE(rig.proc.clearChainBranch());
    const juce::var chain = rig.proc.getChainState(-1)["chain"];
    const std::string latent = (*chain.getArray())[1]["blockId"].toString().toStdString();
    ASSERT_TRUE(rig.proc.setChainBranch("left", latent));
    EXPECT_EQ(rig.proc.getLatencySamples(), kLatency);
    rig.prepare();
    const auto prefixed = render(rig, impulse);
    EXPECT_EQ(peakIndex(prefixed.first), 100 + kLatency);
    EXPECT_EQ(peakIndex(prefixed.second), 100 + kLatency);
  }

  // Back in mono the dormant right lane counts for nothing.
  {
    Rig rig;
    rig.proc.setStereoMode(true);
    rig.proc.setActiveEditChain("right");
    rig.add("test.latency");
    EXPECT_EQ(rig.proc.getLatencySamples(), kLatency);
    rig.proc.setStereoMode(false);
    EXPECT_EQ(rig.proc.getLatencySamples(), 0);
  }
}

TEST(BlockFrameworkTest, BaseRateKindRunsAtTheBaseRateInsideAnOversampledChain) {
  Rig rig(/*oversample=*/2);
  const std::string id = rig.add("test.latency.base");
  // 64 base-rate frames are 128 chain-rate samples and 64 host samples.
  EXPECT_EQ(rig.proc.blockLatencySamples(id), 2 * kLatency);
  EXPECT_EQ(rig.proc.getLatencySamples(), kLatency);

  const auto in = testSignal(20);
  render(rig, in);
  EXPECT_DOUBLE_EQ(TestLatencyProcessor::seen().sampleRate.load(), kFs)
      << "a base-rate kind was prepared for the oversampled rate";

  // The chain-rate twin in the same chain sees the oversampled rate, and
  // its 64 chain-rate samples are 32 host samples.
  const std::string chainRate = rig.add("test.latency");
  EXPECT_EQ(rig.proc.blockLatencySamples(chainRate), kLatency);
  EXPECT_EQ(rig.proc.getLatencySamples(), kLatency + kLatency / 2);
  ASSERT_TRUE(rig.proc.removeChainBlock(id));
  rig.prepare();
  render(rig, in);
  EXPECT_DOUBLE_EQ(TestLatencyProcessor::seen().sampleRate.load(), 2.0 * kFs);
}

// ---------------------------------------------------------------------------
// Tail, tempo, failure
// ---------------------------------------------------------------------------

TEST(BlockFrameworkTest, TailFollowsAParameterAfterTheDebounce) {
  Rig rig;
  const std::string id = rig.add("test.latency");
  // The DC blocker's 2 s floor until a block outlasts it.
  EXPECT_DOUBLE_EQ(rig.proc.getTailLengthSeconds(), 2.0);

  ASSERT_TRUE(rig.proc.setBlockParam(id, "tailMs", 5000.0));
  EXPECT_DOUBLE_EQ(rig.proc.blockTailSeconds(id), 5.0);
  EXPECT_DOUBLE_EQ(rig.proc.getTailLengthSeconds(), 2.0) << "recomputed mid-gesture";

  // The refresh rides a timer on the message thread, one gesture-settle
  // interval after the last move.
  const auto deadline = juce::Time::getMillisecondCounter() + 5000;
  while (rig.proc.getTailLengthSeconds() < 5.0 && juce::Time::getMillisecondCounter() < deadline)
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
  EXPECT_DOUBLE_EQ(rig.proc.getTailLengthSeconds(), 5.0);

  // And back down, this time without waiting for the timer.
  ASSERT_TRUE(rig.proc.setBlockParam(id, "tailMs", 0.0));
  rig.proc.flushDeferredBlockRefresh();
  EXPECT_DOUBLE_EQ(rig.proc.getTailLengthSeconds(), 2.0);
}

TEST(BlockFrameworkTest, TempoReachesTheContextAndTheHostOverridesIt) {
  struct FakePlayHead : juce::AudioPlayHead {
    juce::Optional<PositionInfo> getPosition() const override {
      PositionInfo info;
      info.setBpm(140.0);
      info.setTimeSignature(TimeSignature{3, 4});
      info.setIsPlaying(true);
      info.setTimeInSamples(12345);
      return info;
    }
  };

  Rig rig;
  rig.add("test.latency");
  const auto in = testSignal(4);
  auto& seen = TestLatencyProcessor::seen();

  // No host playhead: the rig's own tempo, 120 until set, and no transport.
  render(rig, in);
  EXPECT_DOUBLE_EQ(seen.bpm.load(), 120.0);
  EXPECT_FALSE(seen.fromHost.load());
  EXPECT_FALSE(seen.playing.load());
  EXPECT_EQ(seen.timeSigNumerator.load(), 4);
  EXPECT_DOUBLE_EQ(static_cast<double>(rig.must("tempo.get")["bpm"]), 120.0);

  const juce::var set = rig.must("tempo.set", R"({"bpm":97.5})");
  EXPECT_DOUBLE_EQ(static_cast<double>(set["bpm"]), 97.5);
  const juce::int64 positionBefore = seen.samplePosition.load();
  render(rig, in);
  EXPECT_DOUBLE_EQ(seen.bpm.load(), 97.5);
  EXPECT_GT(seen.samplePosition.load(), positionBefore) << "the free-running position stood still";
  EXPECT_FALSE(static_cast<bool>(rig.must("tempo.get")["hostSync"]));

  EXPECT_EQ(rig.run("tempo.set", R"({"bpm":10})").errorCode,
            juce::String(CommandError::invalidArgs));
  EXPECT_EQ(rig.run("tempo.set", R"({"bpm":301})").errorCode,
            juce::String(CommandError::invalidArgs));

  // A host that reports a tempo wins, and brings its transport with it.
  FakePlayHead playHead;
  rig.proc.setPlayHead(&playHead);
  render(rig, in);
  EXPECT_DOUBLE_EQ(seen.bpm.load(), 140.0);
  EXPECT_TRUE(seen.fromHost.load());
  EXPECT_TRUE(seen.playing.load());
  EXPECT_EQ(seen.timeSigNumerator.load(), 3);
  EXPECT_EQ(seen.timeSigDenominator.load(), 4);
  EXPECT_EQ(seen.samplePosition.load(), 12345);
  const juce::var hosted = rig.must("tempo.get");
  EXPECT_DOUBLE_EQ(static_cast<double>(hosted["bpm"]), 97.5);
  EXPECT_DOUBLE_EQ(static_cast<double>(hosted["effectiveBpm"]), 140.0);
  EXPECT_TRUE(static_cast<bool>(hosted["hostSync"]));

  // Gone again: back to the rig's own.
  rig.proc.setPlayHead(nullptr);
  render(rig, in);
  EXPECT_DOUBLE_EQ(seen.bpm.load(), 97.5);
  EXPECT_FALSE(seen.fromHost.load());
}

TEST(BlockFrameworkTest, AFailingBlockIsDroppedSaysSoAndUndoRevivesIt) {
  Rig rig;
  const std::string id = rig.add("test.latency");
  const auto in = testSignal(8);
  render(rig, in);
  EXPECT_TRUE(static_cast<bool>(rig.block(id)["loaded"]));
  EXPECT_TRUE(rig.block(id)["processingFailed"].isVoid());

  // The kind throws on its next callback. The audio carries on (the dry
  // signal), the block leaves processing, and the chain state says why.
  ASSERT_TRUE(rig.proc.setBlockParam(id, "fail", 1.0));
  const auto out = render(rig, in);
  float peak = 0.0f;
  for (const float v : out.first) {
    ASSERT_TRUE(std::isfinite(v));
    peak = std::max(peak, std::abs(v));
  }
  EXPECT_GT(peak, 0.2f) << "the signal stopped with the block";
  const juce::var row = rig.block(id);
  EXPECT_FALSE(static_cast<bool>(row["loaded"]));
  EXPECT_TRUE(static_cast<bool>(row["processingFailed"]));
  EXPECT_TRUE(static_cast<bool>(rig.block(id)["processingFailed"])) << "the flag did not stick";

  // A wait for the rig does not sit out its timeout on a block that will
  // never come back: it answers at once, naming the block as failed.
  const auto before = juce::Time::getMillisecondCounter();
  const juce::var waited = rig.must("state.waitUntilLoaded", R"({"timeoutMs":20000})");
  EXPECT_LT(juce::Time::getMillisecondCounter() - before, 5000u);
  EXPECT_FALSE(static_cast<bool>(waited["loaded"]));
  ASSERT_EQ(waited["failedBlockIds"].size(), 1);
  EXPECT_EQ(waited["failedBlockIds"][0].toString(), juce::String(id));

  // Undo takes back the setting that made it fail, and with it the failure:
  // the same block, processing again.
  letAudioGoIdle();
  ASSERT_TRUE(rig.proc.undoChain());
  ASSERT_TRUE(settle(rig));
  const juce::var revived = rig.block(id);
  EXPECT_TRUE(static_cast<bool>(revived["loaded"]));
  EXPECT_TRUE(revived["processingFailed"].isVoid());
  EXPECT_DOUBLE_EQ(static_cast<double>(revived["kindParams"]["fail"]), 0.0);
  rig.prepare();
  render(rig, in);
  EXPECT_TRUE(static_cast<bool>(rig.block(id)["loaded"])) << "the revived block failed again";
  EXPECT_TRUE(static_cast<bool>(rig.must("state.waitUntilLoaded")["loaded"]));

  letAudioGoIdle();
  EXPECT_TRUE(rig.proc.removeChainBlock(id));
}

// ---------------------------------------------------------------------------
// A tone swapped onto a registry-kind block
// ---------------------------------------------------------------------------
// swapTone takes any non-insert block, so a kind block can have a model on
// its way in while it is still a kind block. Every path that asks "is there
// a model here" has to agree about that window.

TEST(BlockFrameworkTest, ToneSwapOntoAKindBlockLands) {
  Rig rig;
  const std::string id = rig.add("utility.gain");
  ASSERT_TRUE(rig.proc.setBlockParam(id, "outputGain", 0.4));

  ScratchFile amp("a2-amp-test.nam");
  amp.copyIn();
  ASSERT_TRUE(rig.proc.swapTone(id, catalogTone(7, 700, "nam", amp.file)));

  // Saved in that window, the block is the tone block it is becoming: one
  // coherent node, never a kind node with a tone or a tone node without one.
  {
    const juce::ValueTree node = savedBlock(savedState(rig.proc), juce::String(id));
    ASSERT_TRUE(node.isValid());
    const juce::String type = node.getProperty("type").toString();
    if (type == "kind") {
      // (The load can only have been overtaken if it has not been queued.)
      ADD_FAILURE() << "a block with a tone on its way in saved as a kind";
    } else {
      EXPECT_EQ(type, juce::String("nam"));
      EXPECT_EQ(static_cast<int>(node.getProperty("toneId")), 7);
      EXPECT_EQ(static_cast<int>(node.getProperty("activeModelId")), 700);
      EXPECT_FALSE(node.getChildWithName("KindState").isValid());
    }
  }

  ASSERT_TRUE(waitForSwap(rig, id, 700));
  const juce::var row = rig.block(id);
  EXPECT_EQ(row["kindId"].toString(), juce::String("nam"));
  EXPECT_EQ(row["tone"]["title"].toString(), juce::String("Swap Target"));
  EXPECT_EQ(row["kindParams"].getDynamicObject()->getProperties().size(), 0);
  EXPECT_NEAR(static_cast<double>(row["params"]["outputGain"]), 0.4, 1e-6);
  EXPECT_EQ(rig.proc.namEngineVoiceCount(id), 1);
  EXPECT_EQ(savedBlock(savedState(rig.proc), juce::String(id)).getProperty("type").toString(),
            juce::String("nam"));

  // And undo makes it the gain block again.
  letAudioGoIdle();
  ASSERT_TRUE(rig.proc.undoChain());
  ASSERT_TRUE(settle(rig));
  EXPECT_EQ(rig.block(id)["kindId"].toString(), juce::String("utility.gain"));
  EXPECT_TRUE(static_cast<bool>(rig.block(id)["loaded"]));
}

TEST(BlockFrameworkTest, FailedToneSwapOntoAKindBlockCanBeRetried) {
  Rig rig;
  const std::string id = rig.add("utility.gain");

  // The model file is not there yet: the download fails.
  ScratchFile amp("a2-amp-test.nam");
  ASSERT_TRUE(rig.proc.swapTone(id, catalogTone(7, 700, "nam", amp.file)));
  ASSERT_TRUE(waitForBlock(rig, id, [](const juce::var& row) {
    return static_cast<bool>(row["loadFailed"]);
  }));
  {
    const juce::var row = rig.block(id);
    EXPECT_FALSE(static_cast<bool>(row["loaded"]));
    EXPECT_FALSE(static_cast<bool>(row["modelLoading"]));
    EXPECT_EQ(row["tone"]["title"].toString(), juce::String("Swap Target"));
  }

  // Not a dead block: with the file in place the retry loads it.
  amp.copyIn();
  ASSERT_TRUE(rig.proc.retryModelLoad(id));
  ASSERT_TRUE(waitForSwap(rig, id, 700));
  EXPECT_EQ(rig.block(id)["kindId"].toString(), juce::String("nam"));
  EXPECT_FALSE(static_cast<bool>(rig.block(id)["loadFailed"]));

  // A kind block with no tone on its way has nothing to retry.
  const std::string plain = rig.add("utility.gain");
  EXPECT_FALSE(rig.proc.retryModelLoad(plain));

  // The other way out of a failed swap: undo, which gives the kind back,
  // processing.
  ScratchFile missing("a2-amp-test.nam");
  ASSERT_TRUE(rig.proc.swapTone(plain, catalogTone(8, 800, "nam", missing.file)));
  ASSERT_TRUE(waitForBlock(rig, plain, [](const juce::var& row) {
    return static_cast<bool>(row["loadFailed"]);
  }));
  letAudioGoIdle();
  ASSERT_TRUE(rig.proc.undoChain());
  ASSERT_TRUE(settle(rig));
  const juce::var restored = rig.block(plain);
  EXPECT_EQ(restored["kindId"].toString(), juce::String("utility.gain"));
  EXPECT_TRUE(static_cast<bool>(restored["loaded"]));
  EXPECT_FALSE(static_cast<bool>(restored["loadFailed"]));
  EXPECT_EQ(static_cast<int>(restored["activeModelId"]), 0);
  EXPECT_EQ(restored["tone"]["title"].toString(), juce::String("Gain"));
}

TEST(BlockFrameworkTest, UndoDuringAToneSwapOntoAKindBlockStopsTheEngineLanding) {
  // Several rounds: whether the undo catches the load before, during or
  // after its build is up to the machine, and every one of those has to end
  // the same way.
  for (int round = 0; round < 4; ++round) {
    Rig rig;
    const std::string id = rig.add("utility.gain");
    ASSERT_TRUE(rig.proc.setBlockParam(id, "gainDb", 5.0));

    ScratchFile amp("a2-amp-test.nam");
    amp.copyIn();
    ASSERT_TRUE(rig.proc.swapTone(id, catalogTone(7, 700, "nam", amp.file)));
    if (round > 0)
      juce::Thread::sleep(round * 15);
    ASSERT_TRUE(rig.proc.undoChain());

    // Long enough for the overtaken load to finish whatever it was doing.
    ASSERT_TRUE(settle(rig));
    juce::Thread::sleep(400);
    ASSERT_TRUE(settle(rig));

    const juce::var row = rig.block(id);
    EXPECT_EQ(row["kindId"].toString(), juce::String("utility.gain")) << "round " << round;
    EXPECT_TRUE(static_cast<bool>(row["loaded"])) << "round " << round;
    EXPECT_FALSE(static_cast<bool>(row["modelLoading"])) << "round " << round;
    EXPECT_FALSE(static_cast<bool>(row["loadFailed"])) << "round " << round;
    EXPECT_EQ(static_cast<int>(row["activeModelId"]), 0) << "round " << round;
    EXPECT_DOUBLE_EQ(static_cast<double>(row["kindParams"]["gainDb"]), 5.0) << "round " << round;
    EXPECT_EQ(rig.proc.namEngineVoiceCount(id), 0) << "the NAM engine landed, round " << round;
    const juce::ValueTree node = savedBlock(savedState(rig.proc), juce::String(id));
    EXPECT_EQ(node.getProperty("type").toString(), juce::String("kind"));
    EXPECT_FALSE(node.hasProperty("toneId"));
    EXPECT_TRUE(node.getChildWithName("KindState").isValid());
  }
}

TEST(BlockFrameworkTest, ToneSwapOntoALatentKindBlockGivesTheLatencyBack) {
  Rig rig;
  const std::string id = rig.add("test.latency");
  EXPECT_EQ(rig.proc.getLatencySamples(), kLatency);

  ScratchFile cab("cab-ir-test.wav");
  cab.copyIn();
  ASSERT_TRUE(rig.proc.swapTone(id, catalogTone(9, 900, "ir", cab.file)));
  // Still the latent kind until the IR lands.
  ASSERT_TRUE(waitForSwap(rig, id, 900));
  ASSERT_TRUE(settle(rig));
  EXPECT_EQ(rig.block(id)["kindId"].toString(), juce::String("ir"));
  EXPECT_EQ(rig.proc.blockLatencySamples(id), 0);
  EXPECT_EQ(rig.proc.getLatencySamples(), 0);
}

// ---------------------------------------------------------------------------
// Restores
// ---------------------------------------------------------------------------

TEST(BlockFrameworkTest, RestoringOntoALiveKindBlockStartsFromTheDefaults) {
  // The processor on its own: a node that lacks an id (one saved before the
  // parameter existed) leaves that parameter at its default, not at
  // whatever the processor happened to hold.
  {
    GainBlockProcessor gain;
    gain.setParameter(GainBlockProcessor::kPan, 0.5f);
    gain.setParameter(GainBlockProcessor::kGainDb, 3.0f);
    juce::ValueTree node("KindState");
    node.setProperty("kind", "utility.gain", nullptr);
    node.setProperty("version", 1, nullptr);
    juce::ValueTree param("Param");
    param.setProperty("id", "gainDb", nullptr);
    param.setProperty("value", 1.0, nullptr);
    node.appendChild(param, nullptr);
    gain.setKindState(node);
    EXPECT_FLOAT_EQ(gain.getParameter(GainBlockProcessor::kGainDb), 1.0f);
    EXPECT_FLOAT_EQ(gain.getParameter(GainBlockProcessor::kPan), 0.0f);
  }

  // Through a rig: the restore reuses the live block (same id, same kind),
  // and ends up where a fresh one would.
  Rig rig;
  rig.proc.restoreFromTree(
      monoSnapshot({makeKindBlockTree("blk-gain", "utility.gain", "Gain", {{"pan", 0.5}})}));
  ASSERT_TRUE(settle(rig));
  ASSERT_TRUE(rig.proc.setBlockParam("blk-gain", "phaseInvert", 1.0));
  EXPECT_DOUBLE_EQ(static_cast<double>(rig.block("blk-gain")["kindParams"]["pan"]), 0.5);

  rig.proc.restoreFromTree(
      monoSnapshot({makeKindBlockTree("blk-gain", "utility.gain", "Gain", {{"gainDb", 2.0}})}));
  ASSERT_TRUE(settle(rig));
  const juce::var row = rig.block("blk-gain");
  EXPECT_DOUBLE_EQ(static_cast<double>(row["kindParams"]["gainDb"]), 2.0);
  EXPECT_DOUBLE_EQ(static_cast<double>(row["kindParams"]["pan"]), 0.0);
  EXPECT_DOUBLE_EQ(static_cast<double>(row["kindParams"]["phaseInvert"]), 0.0);
}

TEST(BlockFrameworkTest, ASaveWhileAKindIsBeingBuiltWritesTheWholeBlock) {
  // A restored kind block's processor is built off the chain lock, so there
  // is a window in which the block exists without one. Whatever the timing,
  // a save in that window carries the kind and its values.
  const juce::ValueTree snapshot = monoSnapshot(
      {makeKindBlockTree("blk-gain", "utility.gain", "Gain",
                         {{"gainDb", -7.5}, {"pan", 0.25}, {"phaseInvert", 1.0}})});
  for (int round = 0; round < 8; ++round) {
    Rig rig;
    rig.proc.restoreFromTree(snapshot);
    const juce::ValueTree early = savedBlock(savedState(rig.proc), "blk-gain");
    ASSERT_TRUE(early.isValid());
    const juce::ValueTree kindState = early.getChildWithName("KindState");
    ASSERT_TRUE(kindState.isValid()) << "a block still being built saved without its kind";
    EXPECT_EQ(kindState.getProperty("kind").toString(), juce::String("utility.gain"));
    ASSERT_EQ(kindState.getNumChildren(), 3);
    EXPECT_DOUBLE_EQ(static_cast<double>(kindState.getChild(0).getProperty("value")), -7.5);

    ASSERT_TRUE(settle(rig));
    const juce::ValueTree late = savedBlock(savedState(rig.proc), "blk-gain");
    EXPECT_TRUE(late.isEquivalentTo(early)) << "the block changed between build and landing";
    EXPECT_TRUE(static_cast<bool>(rig.block("blk-gain")["loaded"]));
  }
}

// ---------------------------------------------------------------------------
// The gain kind on one channel
// ---------------------------------------------------------------------------

TEST(BlockFrameworkTest, GainStartsAtItsTargetOnTheChannelsItIsGiven) {
  BlockPrepareSpec spec;
  spec.chainSampleRate = kFs;
  spec.baseBlockSize = spec.maxBlockSize = kBlock;
  const BlockProcessContext ctx;

  // Pan hard right. On one channel (a stereo-mode lane) pan does nothing,
  // so the signal is at full level from the first sample after a prepare,
  // not fading in from the silence a stereo left channel would have.
  GainBlockProcessor gain;
  gain.setParameter(GainBlockProcessor::kPan, 1.0f);
  gain.prepareForChain(spec);
  juce::AudioBuffer<float> mono(1, kBlock);
  for (int i = 0; i < kBlock; ++i)
    mono.setSample(0, i, 0.25f);
  gain.processInChain(mono, ctx);
  for (int i = 0; i < kBlock; ++i)
    ASSERT_EQ(mono.getSample(0, i), 0.25f) << "frame " << i;

  // The same processor handed two channels: left silent, right untouched,
  // again from the first sample (a change of layout is not a knob move).
  juce::AudioBuffer<float> stereo(2, kBlock);
  for (int ch = 0; ch < 2; ++ch)
    for (int i = 0; i < kBlock; ++i)
      stereo.setSample(ch, i, 0.25f);
  gain.processInChain(stereo, ctx);
  for (int i = 0; i < kBlock; ++i) {
    ASSERT_EQ(stereo.getSample(0, i), 0.0f) << "frame " << i;
    ASSERT_EQ(stereo.getSample(1, i), 0.25f) << "frame " << i;
  }

  // In a rig: a stereo-mode lane is one channel wide.
  const auto in = testSignal(8);
  Rig reference;
  reference.proc.setStereoMode(true);
  reference.prepare();
  Rig rig;
  rig.proc.setStereoMode(true);
  rig.proc.setActiveEditChain("left");
  const std::string id = rig.add("utility.gain");
  ASSERT_TRUE(rig.proc.setBlockParam(id, "pan", 1.0));
  rig.prepare();
  EXPECT_EQ(render(rig, in), render(reference, in));
}

// ---------------------------------------------------------------------------
// Latency that follows a parameter
// ---------------------------------------------------------------------------

TEST(BlockFrameworkTest, LatencyChangeUnderAudioKeepsWetAndDryTogether) {
  Rig rig, reference;
  const std::string id = rig.add("test.latency");
  ASSERT_TRUE(rig.proc.setBlockParam(id, "mix", 0.5));
  rig.prepare();
  reference.prepare();

  // A render thread feeding both rigs the same steady sine and keeping
  // everything they put out. At mix 50% the output is the reference delayed
  // by the block's latency, exactly, as long as the dry half and the wet
  // half are delayed alike; any callback in which they are not is a comb.
  constexpr int kMaxBlocks = 6000;
  std::vector<float> out(static_cast<size_t>(kMaxBlocks) * kBlock, 0.0f);
  std::vector<float> ref(out.size(), 0.0f);
  std::atomic<int> framesDone{0};
  std::atomic<bool> stop{false};
  std::thread pump([&] {
    const int loopFrames = 93 * kBlock;
    const auto sine = makeSine(loopFrames, 250.0, 0.25f);
    juce::AudioBuffer<float> buffer(2, kBlock);
    juce::MidiBuffer midi;
    int offset = 0;
    for (int block = 0; block < kMaxBlocks && !stop.load(); ++block) {
      for (auto* target : {&rig, &reference}) {
        buffer.copyFrom(0, 0, sine.data() + offset, kBlock);
        buffer.copyFrom(1, 0, sine.data() + offset, kBlock);
        target->proc.processBlock(buffer, midi);
        auto& sink = target == &rig ? out : ref;
        std::copy(buffer.getReadPointer(0), buffer.getReadPointer(0) + kBlock,
                  sink.begin() + static_cast<std::ptrdiff_t>(block) * kBlock);
      }
      offset = (offset + kBlock) % loopFrames;
      framesDone.store((block + 1) * kBlock);
      juce::Thread::sleep(1);
    }
  });
  const auto waitForFrames = [&framesDone](int frames) {
    const int target = framesDone.load() + frames;
    const auto deadline = juce::Time::getMillisecondCounter() + 20000;
    while (framesDone.load() < target && juce::Time::getMillisecondCounter() < deadline)
      juce::Thread::sleep(2);
  };

  waitForFrames(24000);
  // The parameter moves. The kind keeps its latched latency: nothing
  // changes in the audio or in the report until the edit settles.
  ASSERT_TRUE(rig.proc.setBlockParam(id, "latencySamples", 128.0));
  waitForFrames(24000);
  EXPECT_EQ(rig.proc.getLatencySamples(), kLatency);
  EXPECT_EQ(rig.proc.blockLatencySamples(id), kLatency);
  const int beforeFlush = framesDone.load();

  // The settle: wet path, dry path and report move in one muted splice.
  rig.proc.flushDeferredBlockRefresh();
  EXPECT_EQ(rig.proc.getLatencySamples(), 128);
  EXPECT_EQ(rig.proc.blockLatencySamples(id), 128);
  const int afterFlush = framesDone.load();
  waitForFrames(96000);
  stop.store(true);
  pump.join();
  const int total = framesDone.load();
  ASSERT_GT(total, afterFlush + 72000) << "the render thread stalled";

  // Before the settle, across the parameter move: the old delay, to the bit.
  for (int n = 1000; n < beforeFlush; ++n)
    ASSERT_EQ(out[static_cast<size_t>(n)], ref[static_cast<size_t>(n - kLatency)])
        << "wet and dry came apart at frame " << n << " (parameter set, not yet settled)";

  // After it (once the mute has released and the DC blocker has forgotten
  // the splice): the new delay.
  double worst = 0.0;
  for (int n = afterFlush + 48000; n < total; ++n)
    worst = std::max(worst, static_cast<double>(std::abs(out[static_cast<size_t>(n)] -
                                                         ref[static_cast<size_t>(n - 128)])));
  EXPECT_LT(worst, 1e-4) << "after the change the output is not the signal delayed by 128";
}

TEST(BlockFrameworkTest, HeadlessCallersGetTheDeferredRefreshFromWaitUntilLoaded) {
  // No message loop runs here, so the settle timer never fires: the edits
  // stay deferred until something asks for them.
  Rig rig;
  const std::string id = rig.add("test.latency");
  ASSERT_TRUE(rig.proc.setBlockParam(id, "tailMs", 5000.0));
  ASSERT_TRUE(rig.proc.setBlockParam(id, "latencySamples", 200.0));
  juce::Thread::sleep(500);  // well past the settle interval
  EXPECT_DOUBLE_EQ(rig.proc.getTailLengthSeconds(), 2.0);
  EXPECT_EQ(rig.proc.getLatencySamples(), kLatency);

  // state.waitUntilLoaded is the headless caller's "make it current".
  const juce::var waited = rig.must("state.waitUntilLoaded", R"({"timeoutMs":5000})");
  EXPECT_TRUE(static_cast<bool>(waited["loaded"]));
  EXPECT_DOUBLE_EQ(rig.proc.getTailLengthSeconds(), 5.0);
  EXPECT_EQ(rig.proc.getLatencySamples(), 200);
  EXPECT_EQ(static_cast<int>(rig.must("state.get")["engine"]["latencySamples"]), 200);

  // And the audio agrees with the report.
  ASSERT_TRUE(rig.proc.setBlockParam(id, "mix", 0.5));
  rig.prepare();
  const auto in = testSignal(20);
  const auto reference = renderReference(in);
  const auto out = render(rig, in);
  for (size_t i = 200; i < in.size(); ++i)
    ASSERT_EQ(out.first[i], reference.first[i - 200]) << "frame " << i;
}

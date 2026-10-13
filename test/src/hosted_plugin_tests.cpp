// Hosted plugins (plugin/include/HostedPluginBlockProcessor.h): a chain block
// that is a VST3 or Audio Unit effect, driven end to end through the real
// processor and the command layer with PedaleiraTestFx (test/plugins), a VST3
// built with this suite whose audio is exact: out = gain * in, delayed by
// `delaySamples`, which is also the latency it reports.
//
//   - add by command, exact audio, parameters listed and set
//   - latency: reported, the dry share of the mix, lane alignment, and a
//     change while the rig runs
//   - state: save / restore, presets, undo / redo, duplicate
//   - a plugin that is not there: a placeholder that round-trips byte for
//     byte and comes back when the plugin does
//   - layouts: the three the host asks for, on mono and stereo lanes
//   - a plugin that throws; and, on macOS, one of Apple's Audio Units
//
// Exact comparisons are made against a second processor with an empty chain
// fed the same input, as in block_framework_tests.cpp: everything outside
// the chain is identical and linear, so a power-of-two gain or a pure delay
// inside it comes out as exactly that.
#include "EngineCommands.h"
#include "HostedPluginBlockProcessor.h"
#include "Processor.h"
#include "chain_test_helpers.h"
#include "hosted_plugin_test_helpers.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr int kBlock = 256;
// PedaleiraTestFx's delaySamples parameter covers 0..4096 samples.
constexpr double kMaxDelay = 4096.0;

using StereoAudio = std::pair<std::vector<float>, std::vector<float>>;

// A prepared stereo-bus processor with the command set over it, the noise
// gate off (it sits before the chain and is not what these tests measure),
// and a scratch preset store.
struct Rig {
  explicit Rig(int oversample = 1) {
    presetStore.createDirectory();
    proc.setPresetStoreForTesting(presetStore);
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
  ~Rig() { presetStore.deleteRecursively(); }

  // Like a host (re)starting transport: every smoother snaps to its target
  // and every filter (the plugin's delay line included) starts from rest.
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

  // Everything in flight has landed (a plugin is created on the message
  // loop this pumps) and every deferred report is delivered.
  bool settle() {
    const CommandResult result = run("state.waitUntilLoaded", R"({"timeoutMs":30000})");
    return result.ok && static_cast<bool>(result.value["loaded"]);
  }

  // Adds the plugin `identifier` names and waits for it. Returns the block id.
  std::string addPlugin(const juce::String& identifier, const juce::String& extraArgs = {}) {
    const juce::var added = must(
        "chain.addPlugin",
        "{\"identifier\":" + juce::JSON::toString(juce::var(identifier)) + extraArgs + "}");
    const std::string id = added["blockId"].toString().toStdString();
    EXPECT_FALSE(id.empty());
    EXPECT_TRUE(settle()) << "the plugin never loaded";
    return id;
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

  // One row of block.listParams, found by the parameter's display name.
  juce::var param(const std::string& blockId, const char* name) {
    const juce::var listed =
        must("block.listParams", "{\"blockId\":\"" + juce::String(blockId) + "\"}");
    if (const auto* params = listed["params"].getArray())
      for (const auto& entry : *params)
        if (entry["scope"].toString() == "kind" && entry["name"].toString() == name)
          return entry;
    return {};
  }

  // Sets the plugin parameter called `name` (normalised 0..1) by command.
  juce::var setParam(const std::string& blockId, const char* name, double value) {
    const juce::String id = param(blockId, name)["id"].toString();
    EXPECT_TRUE(id.isNotEmpty()) << "no parameter called " << name;
    return must("block.setParam",
                "{\"blockId\":\"" + juce::String(blockId) + "\",\"param\":" +
                    juce::JSON::toString(juce::var(id)) + ",\"value\":" + juce::String(value, 12) +
                    "}");
  }

  const juce::File presetStore = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                     .getChildFile("t3k-hosted-presets-" + juce::Uuid().toString());
  ChainTestProcessor proc;
  EngineCommands commands{proc};
};

// PedaleiraTestFx's gain runs 0..2, so a normalised 0.25 is a gain of exactly
// one half, and the default 0.5 is unity.
constexpr double kHalfGain = 0.25;
double delayValue(int samples) { return samples / kMaxDelay; }

std::vector<float> testSignal(int blocks = 60) { return makeSine(blocks * kBlock, 440.0, 0.25f); }

StereoAudio render(Rig& rig, const std::vector<float>& in) {
  return processStereo(rig.proc, in, kBlock);
}

// An empty-chain reference render of `in` (see the file comment).
StereoAudio renderReference(const std::vector<float>& in) {
  Rig reference;
  return render(reference, in);
}

void expectScaled(const std::vector<float>& out, const std::vector<float>& reference, float gain,
                  const char* what) {
  ASSERT_EQ(out.size(), reference.size());
  for (size_t i = 0; i < out.size(); ++i)
    ASSERT_EQ(out[i], gain * reference[i]) << what << ", frame " << i;
}

void expectDelayed(const std::vector<float>& out, const std::vector<float>& reference, int delay,
                   double tolerance, const char* what) {
  double worst = 0.0;
  for (size_t i = 0; i < out.size(); ++i) {
    const float expected =
        i >= static_cast<size_t>(delay) ? reference[i - static_cast<size_t>(delay)] : 0.0f;
    worst = std::max(worst, static_cast<double>(std::abs(out[i] - expected)));
  }
  EXPECT_LE(worst, tolerance) << what;
}

juce::ValueTree parseStateBlob(const juce::MemoryBlock& data) {
  if (data.getSize() <= 4 || std::memcmp(data.getData(), "T3KB", 4) != 0)
    return {};
  return juce::ValueTree::readFromData(static_cast<const char*>(data.getData()) + 4,
                                       data.getSize() - 4);
}

juce::ValueTree savedBlock(TONE3000Processor& proc, const juce::String& blockId) {
  juce::MemoryBlock saved;
  proc.getStateInformation(saved);
  const juce::ValueTree snapshot = parseStateBlob(saved).getChildWithName("ChainSnapshot");
  for (const auto* laneName : {"ChainBlocks", "RightChainBlocks"}) {
    const juce::ValueTree lane = snapshot.getChildWithName(laneName);
    for (int i = 0; i < lane.getNumChildren(); ++i)
      if (lane.getChild(i).getProperty("id").toString() == blockId)
        return lane.getChild(i);
  }
  return {};
}

juce::MemoryBlock streamBytes(const juce::ValueTree& tree) {
  juce::MemoryOutputStream out;
  tree.writeToStream(out);
  return out.getMemoryBlock();
}

int peakIndex(const std::vector<float>& x) {
  int best = 0;
  for (int i = 0; i < static_cast<int>(x.size()); ++i)
    if (std::abs(x[static_cast<size_t>(i)]) > std::abs(x[static_cast<size_t>(best)]))
      best = i;
  return best;
}

}  // namespace

// ---------------------------------------------------------------------------
// Adding, audio, parameters
// ---------------------------------------------------------------------------

TEST(HostedPluginTest, AddedByCommandItProcessesExactly) {
  ScannerSandbox sandbox;
  const juce::String identifier = scanTestPlugin();
  const auto in = testSignal();
  const auto reference = renderReference(in);

  Rig rig;
  // The command answers at once with a block that is still loading; the
  // instance is created on the message thread afterwards.
  const juce::var added = rig.must(
      "chain.addPlugin", "{\"identifier\":" + juce::JSON::toString(juce::var(identifier)) + "}");
  const std::string id = added["blockId"].toString().toStdString();
  ASSERT_FALSE(id.empty());
  EXPECT_EQ(added["block"]["kindId"].toString(), juce::String("plugin"));
  EXPECT_EQ(added["block"]["category"].toString(), juce::String("plugin"));
  EXPECT_EQ(added["block"]["tone"]["title"].toString(), juce::String("PedaleiraTestFx"));
  EXPECT_FALSE(static_cast<bool>(added["block"]["loaded"]));
  EXPECT_TRUE(static_cast<bool>(added["block"]["modelLoading"]));
  EXPECT_EQ(added["block"]["plugin"]["format"].toString(), juce::String("VST3"));

  ASSERT_TRUE(rig.settle());
  const juce::var row = rig.block(id);
  EXPECT_TRUE(static_cast<bool>(row["loaded"]));
  EXPECT_FALSE(static_cast<bool>(row["modelLoading"]));
  EXPECT_FALSE(static_cast<bool>(row["missing"]));
  EXPECT_EQ(row["tone"]["title"].toString(), juce::String("PedaleiraTestFx"));
  EXPECT_EQ(row["plugin"]["name"].toString(), juce::String("PedaleiraTestFx"));
  EXPECT_EQ(row["plugin"]["vendor"].toString(), juce::String("PedaleiraTests"));
  EXPECT_EQ(static_cast<int>(row["plugin"]["inputChannels"]), 2);
  EXPECT_EQ(static_cast<int>(row["plugin"]["outputChannels"]), 2);
  EXPECT_EQ(rig.proc.getLatencySamples(), 0);
  EXPECT_DOUBLE_EQ(rig.proc.blockTailSeconds(id), 0.0);

  // At its defaults (unity gain, no delay) the plugin is a bit-exact
  // pass-through: the lane goes in and comes out untouched.
  rig.prepare();
  EXPECT_EQ(render(rig, in), reference);

  // Half gain: every sample exactly halved, on both channels.
  rig.setParam(id, "Gain", kHalfGain);
  rig.prepare();
  {
    const auto out = render(rig, in);
    expectScaled(out.first, reference.first, 0.5f, "left");
    expectScaled(out.second, reference.second, 0.5f, "right");
  }

  // By file path as well (what a hand-written script knows), into a named
  // slot; two half gains in series make a quarter.
  const juce::var chain = rig.proc.getChainState(-1)["chain"];
  const juce::String slot = (*chain.getArray())[3]["blockId"].toString();
  const std::string second =
      rig.addPlugin(testPluginFile().getFullPathName(),
                    ",\"target\":" + juce::JSON::toString(juce::var(slot)));
  EXPECT_EQ((*rig.proc.getChainState(-1)["chain"].getArray())[3]["blockId"].toString(),
            juce::String(second))
      << "the plugin did not take the slot it was given";
  rig.setParam(second, "Gain", kHalfGain);
  rig.prepare();
  expectScaled(render(rig, in).first, reference.first, 0.25f, "two plugins");

  // Removing them gives the empty chain back, to the bit.
  letAudioGoIdle();
  EXPECT_TRUE(rig.proc.removeChainBlock(id));
  EXPECT_TRUE(rig.proc.removeChainBlock(second));
  rig.prepare();
  EXPECT_EQ(render(rig, in), reference) << "a removed plugin left something behind";

  // What cannot be added says why.
  EXPECT_EQ(rig.run("chain.addPlugin", R"({"identifier":"No Such Plugin"})").errorCode,
            juce::String(CommandError::notFound));
  EXPECT_EQ(rig.run("chain.addBlock", R"({"kind":"plugin"})").errorCode,
            juce::String(CommandError::invalidArgs));
  const std::string gain = rig.must("chain.addBlock", R"({"kind":"utility.gain"})")["blockId"]
                               .toString()
                               .toStdString();
  EXPECT_EQ(rig.run("chain.addPlugin", "{\"identifier\":\"PedaleiraTestFx\",\"target\":\"" +
                                           juce::String(gain) + "\"}")
                .errorCode,
            juce::String(CommandError::failed))
      << "a plugin replaced a block in place";
}

// The shipped plugin cannot pump its message loop from inside a command, so
// a caller on the message thread that waits for the rig must be able to run
// the plugin's creation itself. serviceHostedPlugins is that (it is what
// state.waitUntilLoaded and every command call): no message is dispatched
// here at all.
TEST(HostedPluginTest, AMessageThreadCallerCanRunTheBuildItself) {
  ScannerSandbox sandbox;
  const juce::String identifier = scanTestPlugin();

  Rig rig;
  const juce::var added = rig.proc.addPluginBlock(identifier);
  const std::string id = added["blockId"].toString().toStdString();
  ASSERT_FALSE(id.empty()) << added["error"].toString();
  EXPECT_TRUE(static_cast<bool>(rig.block(id)["modelLoading"]));

  rig.proc.serviceHostedPlugins();
  EXPECT_TRUE(static_cast<bool>(rig.block(id)["loaded"]));
  EXPECT_FALSE(static_cast<bool>(rig.block(id)["modelLoading"]));

  // A build whose block has gone by then is dropped without a trace: add,
  // undo, and only then let the build run.
  const std::string second = rig.proc.addPluginBlock(identifier)["blockId"].toString().toStdString();
  ASSERT_TRUE(rig.proc.undoChain());
  rig.proc.serviceHostedPlugins();
  EXPECT_TRUE(rig.block(second).isVoid());
  EXPECT_TRUE(static_cast<bool>(rig.block(id)["loaded"]));
  EXPECT_TRUE(rig.settle());
}

TEST(HostedPluginTest, ParametersAreListedAndSet) {
  ScannerSandbox sandbox;
  const juce::String identifier = scanTestPlugin();
  Rig rig;
  const std::string id = rig.addPlugin(identifier);

  // One list: the five common settings, then the plugin's own automatable
  // parameters as normalised 0..1 values.
  const juce::var listed = rig.must("block.listParams", "{\"blockId\":\"" + juce::String(id) + "\"}");
  EXPECT_EQ(listed["kindId"].toString(), juce::String("plugin"));
  const auto* params = listed["params"].getArray();
  ASSERT_NE(params, nullptr);
  ASSERT_EQ(params->size(), 7);
  for (int i = 5; i < 7; ++i) {
    const juce::var& entry = (*params)[i];
    EXPECT_EQ(entry["scope"].toString(), juce::String("kind"));
    EXPECT_DOUBLE_EQ(static_cast<double>(entry["min"]), 0.0);
    EXPECT_DOUBLE_EQ(static_cast<double>(entry["max"]), 1.0);
    EXPECT_TRUE(static_cast<bool>(entry["automatable"]));
    EXPECT_TRUE(entry["id"].toString().isNotEmpty());
    EXPECT_TRUE(entry["text"].toString().isNotEmpty()) << "no display text for the value";
  }

  const juce::var gain = rig.param(id, "Gain");
  const juce::var delay = rig.param(id, "Delay Samples");
  ASSERT_TRUE(gain.isObject());
  ASSERT_TRUE(delay.isObject());
  EXPECT_NE(gain["id"].toString(), delay["id"].toString());
  EXPECT_EQ(gain["type"].toString(), juce::String("float"));
  EXPECT_DOUBLE_EQ(static_cast<double>(gain["value"]), 0.5);
  EXPECT_DOUBLE_EQ(static_cast<double>(gain["default"]), 0.5);
  EXPECT_DOUBLE_EQ(static_cast<double>(delay["value"]), 0.0);

  // The ids are the plugin's own, so they are the same for the next
  // instance of it.
  const std::string other = rig.addPlugin(identifier);
  EXPECT_EQ(rig.param(other, "Gain")["id"].toString(), gain["id"].toString());

  // A set comes back in the answer, in the listing and in the chain state,
  // with the plugin's own text for it.
  const juce::var set = rig.setParam(id, "Gain", kHalfGain);
  EXPECT_DOUBLE_EQ(static_cast<double>(set["value"]), kHalfGain);
  EXPECT_FALSE(static_cast<bool>(set["clamped"]));
  const juce::var after = rig.param(id, "Gain");
  EXPECT_DOUBLE_EQ(static_cast<double>(after["value"]), kHalfGain);
  EXPECT_NE(after["text"].toString(), gain["text"].toString());
  EXPECT_DOUBLE_EQ(
      static_cast<double>(rig.block(id)["kindParams"][juce::Identifier(gain["id"].toString())]),
      kHalfGain);
  // The other instance is its own.
  EXPECT_DOUBLE_EQ(static_cast<double>(rig.param(other, "Gain")["value"]), 0.5);

  // Out of range is clamped and says so; an unknown id is refused.
  const juce::var clamped = rig.setParam(id, "Gain", 1.5);
  EXPECT_DOUBLE_EQ(static_cast<double>(clamped["value"]), 1.0);
  EXPECT_TRUE(static_cast<bool>(clamped["clamped"]));
  EXPECT_EQ(rig.run("block.setParam",
                    "{\"blockId\":\"" + juce::String(id) + "\",\"param\":\"nope\",\"value\":0.5}")
                .errorCode,
            juce::String(CommandError::invalidArgs));
  // A common setting is still a common setting.
  rig.must("block.setParam", "{\"blockId\":\"" + juce::String(id) + "\",\"param\":\"mix\",\"value\":0.5}");
  EXPECT_DOUBLE_EQ(static_cast<double>(rig.block(id)["params"]["mix"]), 0.5);
}

TEST(HostedPluginTest, TheUiFindsTheInstanceOfAPluginBlockOnly) {
  ScannerSandbox sandbox;
  const juce::String identifier = scanTestPlugin();
  Rig rig;
  const std::string id = rig.addPlugin(identifier);
  const std::string builtIn =
      rig.must("chain.addBlock", R"({"kind":"utility.gain"})")["blockId"].toString().toStdString();

  const auto instance = rig.proc.hostedPluginInstance(id);
  ASSERT_NE(instance, nullptr);
  EXPECT_EQ(instance->identity().name, juce::String("PedaleiraTestFx"));
  EXPECT_EQ(rig.proc.hostedPluginInstance(builtIn), nullptr);
  EXPECT_EQ(rig.proc.hostedPluginInstance("no-such-block"), nullptr);

  // The test plugin has no editor unless asked for one, so there is nothing
  // to open; and off the message thread nothing is ever asked of the plugin.
  EXPECT_EQ(instance->createEditor(), nullptr);
  std::unique_ptr<juce::AudioProcessorEditor> elsewhere;
  std::thread([&] { elsewhere = instance->createEditor(); }).join();
  EXPECT_EQ(elsewhere, nullptr);

  // A removed block's instance outlives it for whoever still holds it.
  rig.must("chain.removeBlock", "{\"blockId\":\"" + juce::String(id) + "\"}");
  EXPECT_EQ(rig.proc.hostedPluginInstance(id), nullptr);
  EXPECT_EQ(instance->identity().name, juce::String("PedaleiraTestFx"));
}

TEST(HostedPluginTest, ASetFromAnotherThreadReachesThePluginOnTheNextBlock) {
  ScannerSandbox sandbox;
  const juce::String identifier = scanTestPlugin();
  const auto in = testSignal();
  const auto reference = renderReference(in);

  Rig rig;
  const std::string id = rig.addPlugin(identifier);
  const juce::String gainId = rig.param(id, "Gain")["id"].toString();
  rig.prepare();

  // Not the message thread: the set is two atomic stores (the path a MIDI
  // mapping or a host's automation takes), applied by the audio thread at
  // the top of its next block.
  std::thread setter([&] { EXPECT_TRUE(rig.proc.setBlockParam(id, gainId, kHalfGain)); });
  setter.join();
  EXPECT_DOUBLE_EQ(static_cast<double>(rig.param(id, "Gain")["value"]), kHalfGain)
      << "the listing does not show a set that is waiting for the audio thread";

  const auto out = render(rig, in);
  expectScaled(out.first, reference.first, 0.5f, "after a set from another thread");
}

// ---------------------------------------------------------------------------
// Latency
// ---------------------------------------------------------------------------

TEST(HostedPluginTest, LatencyIsReportedAndTheDryPathFollows) {
  ScannerSandbox sandbox;
  const juce::String identifier = scanTestPlugin();
  const auto in = testSignal();
  const auto reference = renderReference(in);
  constexpr int kDelay = 480;

  Rig rig;
  const std::string id = rig.addPlugin(identifier);
  EXPECT_EQ(rig.proc.getLatencySamples(), 0);

  // The plugin starts delaying (and says so) the moment the parameter is
  // set; the chain adopts the new latency once the edit settles, the block
  // and its compensation together.
  rig.setParam(id, "Delay Samples", delayValue(kDelay));
  EXPECT_EQ(rig.proc.blockLatencySamples(id), 0) << "the latency moved before the latch";
  ASSERT_TRUE(rig.settle());
  EXPECT_EQ(rig.proc.blockLatencySamples(id), kDelay);
  EXPECT_EQ(rig.proc.getLatencySamples(), kDelay) << "plugin latency missing from the report";
  EXPECT_EQ(static_cast<int>(rig.must("state.get")["engine"]["latencySamples"]), kDelay);

  // Fully wet: the input, delayed by exactly the reported latency.
  rig.prepare();
  expectDelayed(render(rig, in).first, reference.first, kDelay, 0.0, "wet");

  // Mix 50%: half the delayed wet path plus half the dry path. Only if the
  // dry path is delayed by the same amount is that the delayed signal
  // itself; an undelayed dry share would comb it.
  ASSERT_TRUE(rig.proc.setBlockParam(id, "mix", 0.5));
  rig.prepare();
  expectDelayed(render(rig, in).first, reference.first, kDelay, 0.0, "mix 50%");

  // With the plugin's gain halved the same mix is 0.5 * x + 0.25 * x, both
  // delayed: three quarters of the delayed signal. (Not a power of two, so
  // the two sides round differently and the 5 Hz DC blocker after the chain
  // integrates that rounding: a few 1e-6 here. A dry path one sample out of
  // step would miss by 1e-2.)
  rig.setParam(id, "Gain", kHalfGain);
  rig.prepare();
  {
    const auto out = render(rig, in).first;
    double worst = 0.0;
    for (size_t i = kDelay; i < out.size(); ++i)
      worst = std::max(worst, static_cast<double>(
                                  std::abs(out[i] - 0.75f * reference.first[i - kDelay])));
    EXPECT_LE(worst, 1e-4) << "wet and dry are not aligned at mix 50%";
  }

  // Bypassed, the block still delays: latency follows the chain's
  // structure, not the power switch.
  ASSERT_TRUE(rig.proc.setBlockParam(id, "enabled", 0.0));
  rig.prepare();
  expectDelayed(render(rig, in).first, reference.first, kDelay, 0.0, "bypassed");
  EXPECT_EQ(rig.proc.getLatencySamples(), kDelay);

  // Gone with the block, back with an undo (the plugin is created again
  // from its saved chunk, delay included).
  letAudioGoIdle();
  ASSERT_TRUE(rig.proc.removeChainBlock(id));
  EXPECT_EQ(rig.proc.getLatencySamples(), 0);
  rig.must("chain.undo");
  ASSERT_TRUE(rig.settle());
  EXPECT_EQ(rig.proc.getLatencySamples(), kDelay);
}

// The plugin always runs at 48 kHz: in an oversampled chain it sits behind
// the framework's base-rate island and never sees the factor.
TEST(HostedPluginTest, RunsAtTheBaseRateInsideAnOversampledChain) {
  ScannerSandbox sandbox;
  const juce::String identifier = scanTestPlugin();
  constexpr int kDelay = 200;

  Rig rig(/*oversample=*/2);
  ASSERT_DOUBLE_EQ(static_cast<double>(rig.must("state.get")["engine"]["chainSampleRate"]),
                   2.0 * kFs);
  const std::string id = rig.addPlugin(identifier);
  rig.setParam(id, "Delay Samples", delayValue(kDelay));
  ASSERT_TRUE(rig.settle());

  // The plugin's 200 samples are 200 samples at 48 kHz: 400 at the chain's
  // rate, and 200 again in what the host is told.
  EXPECT_EQ(rig.proc.blockLatencySamples(id), 2 * kDelay);
  EXPECT_EQ(rig.proc.getLatencySamples(), kDelay);

  // And that is the delay it really adds: an impulse comes out kDelay host
  // samples later than through the same rig without the plugin's delay.
  std::vector<float> impulse(static_cast<size_t>(8 * kBlock), 0.0f);
  impulse[100] = 0.5f;
  rig.prepare();
  const int delayedPeak = peakIndex(render(rig, impulse).first);
  rig.setParam(id, "Delay Samples", 0.0);
  ASSERT_TRUE(rig.settle());
  rig.prepare();
  const int directPeak = peakIndex(render(rig, impulse).first);
  EXPECT_EQ(delayedPeak - directPeak, kDelay);
}

TEST(HostedPluginTest, LanesStayAlignedAroundALatentPlugin) {
  ScannerSandbox sandbox;
  const juce::String identifier = scanTestPlugin();
  constexpr int kDelay = 300;

  // An impulse on both channels at frame 100.
  std::vector<float> impulse(static_cast<size_t>(8 * kBlock), 0.0f);
  impulse[100] = 0.5f;

  for (const char* side : {"left", "right"}) {
    // Two independent lanes, the plugin (and its latency) in one of them:
    // the other lane waits for it, and both channels come out together.
    Rig rig;
    rig.proc.setStereoMode(true);
    const std::string id =
        rig.addPlugin(identifier, juce::String(",\"lane\":\"") + side + "\"");
    rig.setParam(id, "Delay Samples", delayValue(kDelay));
    ASSERT_TRUE(rig.settle());
    EXPECT_EQ(rig.proc.getLatencySamples(), kDelay);

    rig.prepare();
    const auto out = render(rig, impulse);
    EXPECT_EQ(peakIndex(out.first), 100 + kDelay) << side;
    EXPECT_EQ(peakIndex(out.second), 100 + kDelay)
        << "the lane without the plugin ran ahead (plugin on the " << side << ")";
    EXPECT_EQ(out.first, out.second);
  }
}

TEST(HostedPluginTest, ALatencyChangeWhileRunningKeepsWetAndDryTogether) {
  ScannerSandbox sandbox;
  const juce::String identifier = scanTestPlugin();
  constexpr int kFirst = 480, kSecond = 128;
  constexpr int kHalf = 240 * kBlock;

  // One continuous stretch of audio, the latency changed in the middle
  // without a prepare in between (a prepare would hide a stale dry delay by
  // clearing everything).
  const auto in = makeSine(2 * kHalf, 440.0, 0.25f);
  const std::vector<float> firstHalf(in.begin(), in.begin() + kHalf);
  const std::vector<float> secondHalf(in.begin() + kHalf, in.end());
  const auto reference = renderReference(in);

  Rig rig;
  const std::string id = rig.addPlugin(identifier);
  rig.setParam(id, "Delay Samples", delayValue(kFirst));
  ASSERT_TRUE(rig.proc.setBlockParam(id, "mix", 0.5));
  ASSERT_TRUE(rig.settle());
  ASSERT_EQ(rig.proc.getLatencySamples(), kFirst);

  rig.prepare();
  const auto before = render(rig, firstHalf).first;
  expectDelayed(before, reference.first, kFirst, 0.0, "before the change");

  // The plugin reports its new latency by itself (no parameter flag tells
  // the chain it will); the report is picked up from its notification.
  letAudioGoIdle();
  rig.setParam(id, "Delay Samples", delayValue(kSecond));
  EXPECT_EQ(rig.proc.getLatencySamples(), kFirst) << "re-reported before the edit settled";
  ASSERT_TRUE(rig.settle());
  EXPECT_EQ(rig.proc.blockLatencySamples(id), kSecond);
  EXPECT_EQ(rig.proc.getLatencySamples(), kSecond);

  // Carrying on: once the splice has passed, half wet plus half dry is the
  // input delayed by the new latency again. A dry path still at the old
  // delay would leave a comb here instead. (Not to the bit: the 5 Hz DC
  // blocker after the chain remembers the splice for a while.)
  const auto after = render(rig, secondHalf).first;
  double worst = 0.0;
  for (int i = kHalf / 2; i < kHalf; ++i)
    worst = std::max(worst, static_cast<double>(std::abs(
                                after[static_cast<size_t>(i)] -
                                reference.first[static_cast<size_t>(kHalf + i - kSecond)])));
  EXPECT_LE(worst, 1e-5) << "wet and dry came apart after the latency change";
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

TEST(HostedPluginTest, StateSavesAndRestoresThroughTheChunk) {
  ScannerSandbox sandbox;
  const juce::String identifier = scanTestPlugin();
  const auto in = testSignal();
  const auto reference = renderReference(in);
  constexpr int kDelay = 96;

  Rig rig;
  const std::string id = rig.addPlugin(identifier);
  rig.setParam(id, "Gain", kHalfGain);
  rig.setParam(id, "Delay Samples", delayValue(kDelay));
  ASSERT_TRUE(rig.proc.setBlockParam(id, "mix", 0.9));
  ASSERT_TRUE(rig.settle());

  // The saved node: the common block properties, the EQ, and ONE KindState
  // with the plugin's identity and its opaque chunk. No Param children: the
  // chunk has every value, and they would only repeat it.
  const juce::ValueTree node = savedBlock(rig.proc, juce::String(id));
  ASSERT_TRUE(node.isValid());
  EXPECT_EQ(node.getProperty("type").toString(), juce::String("kind"));
  const juce::ValueTree kindState = node.getChildWithName("KindState");
  ASSERT_TRUE(kindState.isValid());
  EXPECT_EQ(kindState.getProperty("kind").toString(), juce::String("plugin"));
  EXPECT_EQ(kindState.getProperty("name").toString(), juce::String("PedaleiraTestFx"));
  EXPECT_FALSE(kindState.getChildWithName("Param").isValid());
  const auto* chunk = kindState.getProperty("chunk").getBinaryData();
  ASSERT_NE(chunk, nullptr) << "no state chunk in the saved node";
  EXPECT_GT(chunk->getSize(), 0u);
  const juce::ValueTree plugin = kindState.getChildWithName("Plugin");
  ASSERT_TRUE(plugin.isValid());
  EXPECT_EQ(plugin.getProperty("format").toString(), juce::String("VST3"));
  EXPECT_EQ(plugin.getProperty("name").toString(), juce::String("PedaleiraTestFx"));
  EXPECT_EQ(plugin.getProperty("vendor").toString(), juce::String("PedaleiraTests"));
  EXPECT_TRUE(isTestPluginFile(plugin.getProperty("file").toString()));
  EXPECT_NE(static_cast<int>(plugin.getProperty("uid")), 0);
  EXPECT_TRUE(plugin.hasProperty("version"));

  juce::MemoryBlock blob;
  rig.proc.getStateInformation(blob);

  // A fresh processor from that state: the plugin comes back with its
  // values (they travel in the chunk, applied before it runs a block), its
  // latency is part of the report, and the audio is the same.
  Rig reopened;
  reopened.proc.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
  ASSERT_TRUE(reopened.settle());
  const juce::var row = reopened.block(id);
  ASSERT_TRUE(row.isObject());
  EXPECT_TRUE(static_cast<bool>(row["loaded"]));
  EXPECT_DOUBLE_EQ(static_cast<double>(reopened.param(id, "Gain")["value"]), kHalfGain);
  EXPECT_NEAR(static_cast<double>(reopened.param(id, "Delay Samples")["value"]),
              delayValue(kDelay), 1e-6);
  EXPECT_FLOAT_EQ(static_cast<float>(row["params"]["mix"]), 0.9f);
  EXPECT_EQ(reopened.proc.getLatencySamples(), kDelay);

  rig.prepare();
  reopened.prepare();
  const auto original = render(rig, in);
  EXPECT_EQ(render(reopened, in), original);
  // And it is the sound the settings describe, not just the same sound.
  EXPECT_GT(std::abs(original.first[2000]), 0.4f * std::abs(reference.first[2000 - kDelay]));

  // Saved again without an edit, the block is byte for byte what was loaded.
  EXPECT_TRUE(streamBytes(savedBlock(reopened.proc, juce::String(id))) == streamBytes(node))
      << "an unedited plugin block changed on a load and save";
}

TEST(HostedPluginTest, PresetsUndoRedoAndDuplicateKeepThePluginState) {
  ScannerSandbox sandbox;
  const juce::String identifier = scanTestPlugin();
  const auto in = testSignal();
  const auto reference = renderReference(in);

  Rig rig;
  const std::string id = rig.addPlugin(identifier);
  const auto gainOf = [&](const std::string& blockId) {
    return static_cast<double>(rig.param(blockId, "Gain")["value"]);
  };

  // Undo and redo of a plugin parameter: the state goes back onto the
  // running instance (no reload) and the audio follows.
  rig.setParam(id, "Gain", kHalfGain);
  rig.must("chain.undo");
  ASSERT_TRUE(rig.settle());
  EXPECT_DOUBLE_EQ(gainOf(id), 0.5) << "undo did not reach the plugin";
  rig.prepare();
  EXPECT_EQ(render(rig, in), reference);

  letAudioGoIdle();
  rig.must("chain.redo");
  ASSERT_TRUE(rig.settle());
  EXPECT_DOUBLE_EQ(gainOf(id), kHalfGain) << "redo did not reach the plugin";
  rig.prepare();
  expectScaled(render(rig, in).first, reference.first, 0.5f, "after redo");

  // A preset carries the chunk: save, change the plugin, load.
  letAudioGoIdle();
  const juce::String presetId = rig.must("preset.save", R"({"name":"Hosted"})")["id"].toString();
  ASSERT_TRUE(presetId.isNotEmpty());
  rig.setParam(id, "Gain", 1.0);
  EXPECT_DOUBLE_EQ(gainOf(id), 1.0);
  rig.must("preset.load", "{\"id\":" + juce::JSON::toString(juce::var(presetId)) + "}");
  ASSERT_TRUE(rig.settle());
  EXPECT_DOUBLE_EQ(gainOf(id), kHalfGain) << "the preset did not restore the plugin";
  rig.prepare();
  expectScaled(render(rig, in).first, reference.first, 0.5f, "after the preset");

  // A duplicate is a second instance with the same state: two half gains.
  letAudioGoIdle();
  const juce::var duplicated =
      rig.must("chain.duplicateBlock", "{\"blockId\":\"" + juce::String(id) + "\"}");
  const std::string clone = duplicated["blockId"].toString().toStdString();
  ASSERT_FALSE(clone.empty());
  ASSERT_TRUE(rig.settle());
  EXPECT_TRUE(static_cast<bool>(rig.block(clone)["loaded"]));
  EXPECT_DOUBLE_EQ(gainOf(clone), kHalfGain);
  rig.prepare();
  expectScaled(render(rig, in).first, reference.first, 0.25f, "original and duplicate");

  // The two are independent, and undoing the duplicate takes one away.
  rig.setParam(clone, "Gain", 0.5);
  EXPECT_DOUBLE_EQ(gainOf(id), kHalfGain);
  letAudioGoIdle();
  rig.must("chain.undo");  // the clone's gain
  rig.must("chain.undo");  // the duplicate
  ASSERT_TRUE(rig.settle());
  EXPECT_TRUE(rig.block(clone).isVoid());
  rig.prepare();
  expectScaled(render(rig, in).first, reference.first, 0.5f, "after undoing the duplicate");
}

// ---------------------------------------------------------------------------
// A plugin that is not there
// ---------------------------------------------------------------------------

TEST(HostedPluginTest, AMissingPluginIsAPlaceholderThatRoundTripsAndRevives) {
  ScannerSandbox sandbox;
  const auto in = testSignal(20);
  const auto reference = renderReference(in);
  const juce::File empty = sandbox.folder("empty");
  const auto pathArgs = [](const juce::File& folder) {
    return "{\"path\":" + juce::JSON::toString(juce::var(folder.getFullPathName())) + "}";
  };
  const char* scanAndWait = R"({"waitMs":120000})";

  // A rig with the plugin in it, saved while the plugin is installed (its
  // folder is one the scanner searches).
  juce::MemoryBlock blob;
  juce::ValueTree originalNode;
  std::string id;
  {
    Rig rig;
    rig.must("plugins.addFolder", pathArgs(testPluginFolder()));
    rig.must("plugins.scan", scanAndWait);
    id = rig.addPlugin("PedaleiraTestFx");
    rig.setParam(id, "Gain", kHalfGain);
    ASSERT_TRUE(rig.proc.setBlockParam(id, "mix", 0.8));
    ASSERT_TRUE(rig.settle());
    originalNode = savedBlock(rig.proc, juce::String(id));
    rig.proc.getStateInformation(blob);

    // The plugin goes away: the scanner is pointed at an empty folder.
    rig.must("plugins.removeFolder", pathArgs(testPluginFolder()));
    rig.must("plugins.addFolder", pathArgs(empty));
    rig.must("plugins.scan", scanAndWait);
    EXPECT_EQ(rig.must("plugins.list")["plugins"][0]["status"].toString(),
              juce::String("missing"));
    // A plugin that is already running carries on; only loading asks the
    // scanner.
    EXPECT_TRUE(static_cast<bool>(rig.block(id)["loaded"]));
  }
  ASSERT_TRUE(originalNode.isValid());
  const juce::MemoryBlock originalBytes = streamBytes(originalNode);

  // Opening the rig now: no error, nothing held up, the rest of the rig
  // loads. The block is a bypassed placeholder under the plugin's name.
  Rig rig;
  rig.proc.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
  const juce::var waited = rig.must("state.waitUntilLoaded", R"({"timeoutMs":20000})");
  EXPECT_TRUE(static_cast<bool>(waited["loaded"]));
  EXPECT_EQ(waited["failedBlockIds"].size(), 0);

  juce::var row = rig.block(id);
  ASSERT_TRUE(row.isObject());
  EXPECT_TRUE(static_cast<bool>(row["missing"]));
  EXPECT_FALSE(static_cast<bool>(row["loaded"]));
  EXPECT_FALSE(static_cast<bool>(row["loadFailed"]));
  EXPECT_EQ(row["kindId"].toString(), juce::String("plugin"));
  EXPECT_EQ(row["tone"]["title"].toString(), juce::String("PedaleiraTestFx"));
  EXPECT_EQ(row["plugin"]["name"].toString(), juce::String("PedaleiraTestFx"));
  EXPECT_EQ(rig.proc.getLatencySamples(), 0);
  EXPECT_FALSE(rig.proc.setBlockParam(id, "mix", 0.1)) << "a placeholder took an edit";

  // Bypassed: the rig is the empty chain.
  rig.prepare();
  EXPECT_EQ(render(rig, in), reference) << "the placeholder is not a clean bypass";

  // Saved back exactly as it came, now and a generation later, and through
  // an edit elsewhere and its undo.
  EXPECT_TRUE(streamBytes(savedBlock(rig.proc, juce::String(id))) == originalBytes)
      << "the missing plugin's block changed on a load and save";
  letAudioGoIdle();
  rig.must("chain.addBlock", R"({"kind":"utility.gain"})");
  rig.must("chain.undo");
  ASSERT_TRUE(rig.settle());
  juce::MemoryBlock secondBlob;
  rig.proc.getStateInformation(secondBlob);
  {
    Rig again;
    again.proc.setStateInformation(secondBlob.getData(), static_cast<int>(secondBlob.getSize()));
    ASSERT_TRUE(again.settle());
    EXPECT_TRUE(static_cast<bool>(again.block(id)["missing"]));
    EXPECT_TRUE(streamBytes(savedBlock(again.proc, juce::String(id))) == originalBytes);
  }

  // The plugin is installed again and a scan finds it: the placeholder
  // loads it, with the state it was saved with.
  rig.must("plugins.addFolder", pathArgs(testPluginFolder()));
  rig.must("plugins.scan", scanAndWait);
  ASSERT_TRUE(rig.settle());
  row = rig.block(id);
  EXPECT_FALSE(static_cast<bool>(row["missing"])) << "the plugin did not come back";
  EXPECT_TRUE(static_cast<bool>(row["loaded"]));
  EXPECT_DOUBLE_EQ(static_cast<double>(rig.param(id, "Gain")["value"]), kHalfGain);
  EXPECT_FLOAT_EQ(static_cast<float>(row["params"]["mix"]), 0.8f);

  // Mix 0.8 of a half gain: 0.2 * x + 0.8 * 0.5 * x (to rounding, see the
  // latency test).
  rig.prepare();
  const auto out = render(rig, in).first;
  double worst = 0.0;
  for (size_t i = 0; i < out.size(); ++i)
    worst = std::max(worst, static_cast<double>(std::abs(out[i] - 0.6f * reference.first[i])));
  EXPECT_LE(worst, 1e-4);
  EXPECT_GT(std::abs(out[1000]), 0.5f * std::abs(reference.first[1000]));
  // And it saves as the block it was.
  EXPECT_TRUE(savedBlock(rig.proc, juce::String(id)).isEquivalentTo(originalNode));
}

// ---------------------------------------------------------------------------
// Layouts
// ---------------------------------------------------------------------------

TEST(HostedPluginTest, MonoAndStereoLanesThroughEveryLayout) {
  ScannerSandbox sandbox;
  const juce::String identifier = scanTestPlugin();
  const auto left = testSignal(40);
  const auto right = makeSine(40 * kBlock, 660.0, 0.2f);

  // PEDALEIRA_TESTFX_LAYOUT narrows the layouts the test plugin accepts, so
  // each of the three the host asks for, in order, gets its turn:
  //   ""             stereo in, stereo out (the first ask)
  //   "monoToStereo" mono in, stereo out   (the second)
  //   "mono"         mono in, mono out     (the third)
  struct Case {
    const char* layout;
    int in, out;
  };
  for (const Case& layout : {Case{"", 2, 2}, Case{"monoToStereo", 1, 2}, Case{"mono", 1, 1}}) {
    SCOPED_TRACE(juce::String("layout '") + layout.layout + "'");
    ScopedEnvironment narrowed("PEDALEIRA_TESTFX_LAYOUT", layout.layout);

    // A mono chain on a stereo rig: one lane, two channels carrying the
    // same signal. Whatever the plugin's layout, both come out at half gain.
    {
      Rig rig, reference;
      const std::string id = rig.addPlugin(identifier);
      EXPECT_EQ(static_cast<int>(rig.block(id)["plugin"]["inputChannels"]), layout.in);
      EXPECT_EQ(static_cast<int>(rig.block(id)["plugin"]["outputChannels"]), layout.out);
      rig.setParam(id, "Gain", kHalfGain);
      rig.prepare();
      const auto expected = render(reference, left);
      const auto out = render(rig, left);
      expectScaled(out.first, expected.first, 0.5f, "mono chain, left");
      expectScaled(out.second, expected.second, 0.5f, "mono chain, right");
    }

    // Stereo chains: each lane is ONE channel. The plugin sits in the left
    // lane; a stereo input gets the channel on both sides and a stereo
    // output folds back to one, so the left is halved exactly and the right
    // lane is not touched.
    {
      Rig rig, reference;
      rig.proc.setStereoMode(true);
      reference.proc.setStereoMode(true);
      const std::string id = rig.addPlugin(identifier, ",\"lane\":\"left\"");
      rig.setParam(id, "Gain", kHalfGain);
      rig.prepare();
      reference.prepare();
      const auto expected = processStereoLR(reference.proc, left, right, kBlock);
      const auto out = processStereoLR(rig.proc, left, right, kBlock);
      expectScaled(out.first, expected.first, 0.5f, "stereo chains, plugin lane");
      EXPECT_EQ(out.second, expected.second) << "the plugin leaked into the other lane";
    }
  }

  // Dual mono: the lane's two channels are two different guitars. The
  // plugin runs as one ordinary stereo instance (a documented limitation),
  // which for a plugin that keeps its channels apart is still two
  // independent paths.
  {
    Rig rig, reference;
    rig.proc.setInputMode(TONE3000Processor::InputMode::DualMono);
    reference.proc.setInputMode(TONE3000Processor::InputMode::DualMono);
    const std::string id = rig.addPlugin(identifier);
    EXPECT_EQ(static_cast<int>(rig.block(id)["plugin"]["inputChannels"]), 2);
    rig.setParam(id, "Gain", kHalfGain);
    ASSERT_TRUE(rig.settle());
    rig.prepare();
    reference.prepare();
    // The mode only engages once a callback has seen a stereo source.
    processStereoLR(rig.proc, left, right, kBlock);
    processStereoLR(reference.proc, left, right, kBlock);
    ASSERT_TRUE(rig.proc.dualMonoEngaged());
    rig.prepare();
    reference.prepare();
    const auto expected = processStereoLR(reference.proc, left, right, kBlock);
    const auto out = processStereoLR(rig.proc, left, right, kBlock);
    expectScaled(out.first, expected.first, 0.5f, "dual mono, left");
    expectScaled(out.second, expected.second, 0.5f, "dual mono, right");
    EXPECT_NE(expected.first, expected.second);
  }
}

// ---------------------------------------------------------------------------
// Failure
// ---------------------------------------------------------------------------

// ####################
// THE CHAIN LOCK AND A SLOW PLUGIN
// ####################
// The audio thread waits for chainMutex when a message-thread edit holds it.
// A call into a hosted plugin (its parameter text, its state) can take as
// long as the plugin likes, so none may run under the lock. The plugin here
// sleeps in both (PEDALEIRA_TESTFX_SLOW_MS) and carries a 4 MB state; a probe
// thread tries the lock all the while and reports the longest stretch in
// which it could not get it.
namespace {

constexpr int kSlowPluginMs = 40;
// What counts as "not held": far below the plugin's own delay, with room for
// a loaded machine's scheduling.
constexpr double kMaxHoldMs = 4.0;

class LockHoldProbe {
public:
  explicit LockHoldProbe(const TONE3000Processor& processor)
      : watched(processor), worker([this] { run(); }) {}
  ~LockHoldProbe() { stop(); }

  // The longest time the lock was out of reach, in ms.
  double stop() {
    running.store(false);
    if (worker.joinable())
      worker.join();
    return longestMs;
  }

private:
  void run() {
    double blockedSince = -1.0;
    while (running.load()) {
      const double now = juce::Time::getMillisecondCounterHiRes();
      if (watched.chainLockFreeForTesting()) {
        if (blockedSince >= 0.0)
          longestMs = std::max(longestMs, now - blockedSince);
        blockedSince = -1.0;
      } else if (blockedSince < 0.0) {
        blockedSince = now;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    if (blockedSince >= 0.0)
      longestMs = std::max(longestMs, juce::Time::getMillisecondCounterHiRes() - blockedSince);
  }

  const TONE3000Processor& watched;
  std::atomic<bool> running{true};
  double longestMs{0.0};
  std::thread worker;
};

}  // namespace

TEST(HostedPluginTest, ListingParametersDoesNotHoldTheChainLockThroughThePlugin) {
  ScannerSandbox sandbox;
  const juce::String identifier = scanTestPlugin();
  ScopedEnvironment slow("PEDALEIRA_TESTFX_SLOW_MS", std::to_string(kSlowPluginMs));
  Rig rig;
  const std::string id = rig.addPlugin(identifier);

  juce::var listed;
  double held = 0.0;
  const double started = juce::Time::getMillisecondCounterHiRes();
  {
    LockHoldProbe probe(rig.proc);
    for (int i = 0; i < 3; ++i)
      listed = rig.proc.getBlockParams(id);
    held = probe.stop();
  }
  const double took = juce::Time::getMillisecondCounterHiRes() - started;

  // The plugin really was slow, and its text still made it into the list.
  EXPECT_GT(took, kSlowPluginMs * 3 * 0.8);
  bool sawText = false;
  if (const auto* params = listed.getArray())
    for (const auto& entry : *params)
      if (entry["scope"].toString() == "kind" && entry["name"].toString() == "Gain")
        sawText = entry["text"].toString().isNotEmpty();
  EXPECT_TRUE(sawText);
  EXPECT_LT(held, kMaxHoldMs) << "getBlockParams held the chain lock for " << held << " ms";
}

TEST(HostedPluginTest, TheUnsavedCheckAndTheMetersDoNotHoldTheChainLockThroughThePlugin) {
  ScannerSandbox sandbox;
  const juce::String identifier = scanTestPlugin();
  ScopedEnvironment slow("PEDALEIRA_TESTFX_SLOW_MS", std::to_string(kSlowPluginMs));
  ScopedEnvironment big("PEDALEIRA_TESTFX_STATE_KB", "4096");
  Rig rig;
  const std::string id = rig.addPlugin(identifier);

  // A saved preset gives hasUnsavedChanges something to compare with.
  ASSERT_TRUE(rig.run("preset.save", R"({"name":"Slow"})").ok);
  EXPECT_FALSE(rig.proc.hasUnsavedChanges());

  double heldUnsaved = 0.0, heldMeters = 0.0;
  {
    LockHoldProbe probe(rig.proc);
    for (int i = 0; i < 5; ++i)
      EXPECT_FALSE(rig.proc.hasUnsavedChanges());
    heldUnsaved = probe.stop();
  }
  {
    LockHoldProbe probe(rig.proc);
    for (int i = 0; i < 20; ++i)
      rig.proc.getMeterLevels();
    heldMeters = probe.stop();
  }
  EXPECT_LT(heldUnsaved, kMaxHoldMs) << "hasUnsavedChanges held the chain lock for " << heldUnsaved
                                     << " ms";
  EXPECT_LT(heldMeters, kMaxHoldMs) << "getMeterLevels held the chain lock for " << heldMeters
                                    << " ms";
  const juce::var meters = rig.proc.getMeterLevels();
  EXPECT_TRUE(meters["blocks"].getDynamicObject()->hasProperty(juce::String(id)));

  // The digest still sees an edit of the plugin: the flag follows the cache,
  // which a looking-only caller refreshes a few hundred ms after the change.
  rig.setParam(id, "Gain", kHalfGain);
  const auto deadline = juce::Time::getMillisecondCounter() + 5000;
  bool unsaved = false;
  while (!unsaved && juce::Time::getMillisecondCounter() < deadline) {
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    unsaved = rig.proc.hasUnsavedChanges();
  }
  EXPECT_TRUE(unsaved) << "an edit of the plugin was not seen as unsaved";
}

TEST(HostedPluginTest, AChattyPluginHasItsStateReadAtMostOncePerInterval) {
  ScannerSandbox sandbox;
  const juce::String identifier = scanTestPlugin();
  Rig rig;
  const std::string id = rig.addPlugin(identifier);
  const auto instance = rig.proc.hostedPluginInstance(id);
  ASSERT_NE(instance, nullptr);

  instance->serviceOnMessageThread(/*forceStateRefresh=*/true);
  EXPECT_TRUE(instance->cacheIsCurrent());

  // Flagged again right away: a throttled look leaves it for later...
  instance->setParameter(0, 0.3f);
  EXPECT_FALSE(instance->cacheIsCurrent());
  auto result = instance->serviceOnMessageThread(false, 500);
  EXPECT_TRUE(result.captureDeferred);
  EXPECT_FALSE(instance->cacheIsCurrent());
  // ...a save (or a snapshot, which does not throttle) does not wait...
  result = instance->serviceOnMessageThread(false, 0);
  EXPECT_FALSE(result.captureDeferred);
  EXPECT_TRUE(instance->cacheIsCurrent());

  // ...and a throttled look reads it once the interval has passed.
  instance->setParameter(0, 0.6f);
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  result = instance->serviceOnMessageThread(false, 500);
  EXPECT_FALSE(result.captureDeferred);
  EXPECT_TRUE(instance->cacheIsCurrent());
}

#if !JUCE_WINDOWS
// A C++ exception crossing from a plugin's module into the host's is only
// dependable where both share one runtime; on Windows each links its own.
TEST(HostedPluginTest, APluginThatThrowsIsDroppedAndTheRigCarriesOn) {
  ScannerSandbox sandbox;
  const juce::String identifier = scanTestPlugin();
  const auto in = testSignal();
  const auto reference = renderReference(in);

  ScopedEnvironment throwing("PEDALEIRA_TESTFX_THROW", "1");
  Rig rig;
  const juce::var added = rig.must(
      "chain.addPlugin", "{\"identifier\":" + juce::JSON::toString(juce::var(identifier)) + "}");
  const std::string id = added["blockId"].toString().toStdString();
  ASSERT_TRUE(waitForPluginsLoaded(rig.proc));
  EXPECT_TRUE(static_cast<bool>(rig.block(id)["loaded"]));

  // The first block throws inside the plugin. Nothing escapes: the dry
  // signal is restored and the block is taken out of processing, so the
  // output is the empty chain's from the first sample on.
  rig.prepare();
  EXPECT_EQ(render(rig, in), reference);

  const juce::var row = rig.block(id);
  EXPECT_FALSE(static_cast<bool>(row["loaded"]));
  EXPECT_TRUE(static_cast<bool>(row["processingFailed"]));
  // A wait for the rig is not held up by it, and says which block failed.
  const juce::var waited = rig.must("state.waitUntilLoaded", R"({"timeoutMs":5000})");
  EXPECT_FALSE(static_cast<bool>(waited["loaded"]));
  ASSERT_EQ(waited["failedBlockIds"].size(), 1);
  EXPECT_EQ(waited["failedBlockIds"][0].toString(), juce::String(id));
  // It still saves whole.
  EXPECT_TRUE(savedBlock(rig.proc, juce::String(id))
                  .getChildWithName("KindState")
                  .getChildWithName("Plugin")
                  .isValid());
}
#endif

// ---------------------------------------------------------------------------
// Audio Units
// ---------------------------------------------------------------------------

#if JUCE_MAC
// The AU path, with a unit every Mac has: Apple's AUDelay ('aufx' 'dely'
// 'appl'). Scanned by identifier, so nothing else on the machine is looked at.
TEST(HostedPluginTest, HostsAnAppleAudioUnit) {
  ScannerSandbox sandbox;
  PluginScanner& scanner = PluginScanner::instance();
  PluginScanner::ScanOptions options;
  options.files.add("AudioUnit:Effects/aufx,dely,appl");
  ASSERT_TRUE(scanner.startScan(options));
  ASSERT_TRUE(scanner.waitForScan(120000));
  ASSERT_EQ(scanner.status().total, 1);

  const juce::Array<juce::PluginDescription> effects = scanner.knownEffects();
  ASSERT_EQ(effects.size(), 1) << "AUDelay was not found";
  const juce::PluginDescription delay = effects[0];
  EXPECT_EQ(delay.pluginFormatName, juce::String("AudioUnit"));
  EXPECT_EQ(delay.manufacturerName, juce::String("Apple"));

  const auto in = testSignal();
  const auto reference = renderReference(in);

  Rig rig;
  const std::string id = rig.addPlugin(delay.createIdentifierString());
  const juce::var row = rig.block(id);
  ASSERT_TRUE(static_cast<bool>(row["loaded"])) << "the Audio Unit did not load";
  EXPECT_EQ(row["plugin"]["format"].toString(), juce::String("AudioUnit"));
  EXPECT_EQ(row["tone"]["title"].toString(), delay.name);

  // Its parameters are listed under the AU's own parameter ids.
  const juce::var listed = rig.must("block.listParams", "{\"blockId\":\"" + juce::String(id) + "\"}");
  ASSERT_GT(listed["params"].size(), 5) << "no Audio Unit parameters";
  const juce::var wetDry = (*listed["params"].getArray())[5];
  EXPECT_EQ(wetDry["scope"].toString(), juce::String("kind"));

  // It processes: the output is neither silent nor the untouched input (at
  // its defaults the delay mixes a delayed copy under the dry signal).
  rig.prepare();
  const auto out = render(rig, in);
  float peak = 0.0f;
  for (const float v : out.first) {
    ASSERT_TRUE(std::isfinite(v));
    peak = std::max(peak, std::abs(v));
  }
  EXPECT_GT(peak, 0.01f) << "the Audio Unit rendered silence";
  EXPECT_NE(out.first, reference.first) << "the Audio Unit left the signal untouched";
  EXPECT_FALSE(static_cast<bool>(rig.block(id)["processingFailed"]));

  // Its state survives a save and a load, like a VST3's.
  rig.must("block.setParam", "{\"blockId\":\"" + juce::String(id) + "\",\"param\":" +
                                 juce::JSON::toString(wetDry["id"]) + ",\"value\":1}");
  juce::MemoryBlock blob;
  rig.proc.getStateInformation(blob);
  Rig reopened;
  reopened.proc.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
  ASSERT_TRUE(reopened.settle());
  EXPECT_TRUE(static_cast<bool>(reopened.block(id)["loaded"]));
  const juce::var restored =
      reopened.must("block.listParams", "{\"blockId\":\"" + juce::String(id) + "\"}");
  EXPECT_NEAR(static_cast<double>((*restored["params"].getArray())[5]["value"]), 1.0, 1e-3);
}
#endif

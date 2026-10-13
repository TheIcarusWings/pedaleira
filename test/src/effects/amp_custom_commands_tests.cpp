// block.loadDescription: an amp description file given to an amp.custom block
// through the command layer (docs/commands.md), end to end through the real
// processor: the block takes the file, refuses a bad one with a message that
// names the field, keeps the description in the saved state, and undoes.
#include "EngineCommands.h"
#include "chain_test_helpers.h"
#include "effects/amp/BuiltinAmps.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace {

constexpr double kFs = 48000.0;
constexpr int kBlock = 128;

juce::var parseJson(const juce::String& json) {
  juce::var value;
  EXPECT_TRUE(juce::JSON::parse(json, value).wasOk()) << json.toStdString();
  return value;
}

juce::String quoted(const juce::String& text) { return juce::JSON::toString(juce::var(text)); }

struct Rig {
  Rig() {
    proc.setMultiCoreEnabled(false, /*persist=*/false);
    proc.setPlayConfigDetails(1, 1, kFs, kBlock);
    proc.prepareToPlay(kFs, kBlock);
  }
  CommandResult run(const char* name, const juce::String& args = "{}") {
    return commands.registry().run(name, parseJson(args));
  }
  juce::var must(const char* name, const juce::String& args = "{}") {
    const CommandResult result = run(name, args);
    EXPECT_TRUE(result.ok) << name << ": " << result.errorCode.toStdString() << ": " << result.errorMessage.toStdString();
    return result.value;
  }
  // Settle, prepare again like a host starting transport, process `input`.
  std::vector<float> render(const std::vector<float>& input) {
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

  TONE3000Processor proc;
  EngineCommands commands{proc};
};

std::vector<float> guitar() {
  std::vector<float> in(48000);
  for (size_t i = 0; i < in.size(); ++i)
    in[i] = 0.2f * std::sin(2.0f * 3.14159265f * 196.0f * static_cast<float>(i) / 48000.0f) *
            std::exp(-static_cast<float>(i) / 30000.0f);
  return in;
}

double maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
  double worst = 0.0;
  for (size_t i = 0; i < a.size(); ++i)
    worst = std::max(worst, static_cast<double>(std::abs(a[i] - b[i])));
  return worst;
}

// A description file in a temp folder: the shipped one with a new name and a
// 100 pF treble capacitor in the tone stack.
struct DescriptionFile {
  explicit DescriptionFile(const std::string& text, const char* name = "brighter.json")
      : folder(juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("pedaleira-amp-" + juce::Uuid().toString())) {
    folder.createDirectory();
    file = folder.getChildFile(name);
    file.replaceWithText(juce::String(text));
  }
  ~DescriptionFile() { folder.deleteRecursively(); }
  juce::File folder, file;
};

std::string brighter() {
  std::string text = ampfx::builtinAmpText("high-gain.json");
  auto replace = [&](const std::string& from, const std::string& to) {
    const size_t at = text.find(from);
    EXPECT_NE(at, std::string::npos) << from;
    if (at != std::string::npos)
      text.replace(at, from.size(), to);
  };
  replace("\"name\": \"High Gain\"", "\"name\": \"Brighter Cousin\"");
  replace("\"treble_cap_f\": 400e-12", "\"treble_cap_f\": 100e-12");
  return text;
}

}  // namespace

TEST(AmpCustomCommands, ALoadedDescriptionChangesTheSoundAndUndoBringsTheOldAmpBack) {
  const std::vector<float> input = guitar();
  Rig plain;
  plain.must("chain.addBlock", R"({"kind":"amp.custom"})");
  const std::vector<float> before = plain.render(input);

  const DescriptionFile description(brighter());
  Rig loaded;
  const juce::String blockId = loaded.must("chain.addBlock", R"({"kind":"amp.custom"})")["blockId"].toString();
  ASSERT_TRUE(blockId.isNotEmpty());
  const juce::var answer = loaded.must(
      "block.loadDescription", "{\"blockId\":" + quoted(blockId) + ",\"file\":" + quoted(description.file.getFullPathName()) + "}");
  EXPECT_EQ(answer["blockId"].toString(), blockId);
  const std::vector<float> after = loaded.render(input);
  EXPECT_GT(maxDiff(before, after), 0.005) << "the new amp should sound different";
  EXPECT_LT(maxDiff(before, after), 2.0);

  // The name of the amp is the block's name in the state.
  EXPECT_NE(juce::JSON::toString(loaded.must("state.get")).indexOf("Brighter Cousin"), -1);

  // One undo step gives the shipped amp back.
  loaded.must("chain.undo");
  const std::vector<float> undone = loaded.render(input);
  EXPECT_LT(maxDiff(before, undone), 1.0e-5);
}

TEST(AmpCustomCommands, TheDescriptionIsSavedWithTheStateOfTheRig) {
  const std::vector<float> input = guitar();
  const DescriptionFile description(brighter());
  Rig original;
  const juce::String blockId = original.must("chain.addBlock", R"({"kind":"amp.custom"})")["blockId"].toString();
  original.must("block.loadDescription",
                "{\"blockId\":" + quoted(blockId) + ",\"file\":" + quoted(description.file.getFullPathName()) + "}");
  const std::vector<float> expected = original.render(input);

  juce::MemoryBlock saved;
  original.proc.getStateInformation(saved);

  // The file is gone by the time the state comes back: the state holds the text.
  description.folder.deleteRecursively();
  Rig restored;
  restored.proc.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
  const std::vector<float> got = restored.render(input);
  EXPECT_LT(maxDiff(expected, got), 1.0e-5);
}

TEST(AmpCustomCommands, RefusesWhatItCannotLoadAndLeavesTheBlockAlone) {
  Rig rig;
  const juce::String amp = rig.must("chain.addBlock", R"({"kind":"amp.custom"})")["blockId"].toString();
  const juce::String gain = rig.must("chain.addBlock", R"({"kind":"utility.gain"})")["blockId"].toString();
  const std::vector<float> input = guitar();
  const std::vector<float> before = rig.render(input);

  auto args = [](const juce::String& id, const juce::File& file) {
    return "{\"blockId\":" + quoted(id) + ",\"file\":" + quoted(file.getFullPathName()) + "}";
  };

  // A file that is not there.
  const DescriptionFile good(brighter());
  CommandResult result = rig.run("block.loadDescription", args(amp, good.folder.getChildFile("none.json")));
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.errorCode, juce::String("not_found"));

  // A description with a fault: the message names the field.
  std::string broken = brighter();
  broken.replace(broken.find("\"plate_load_ohm\": 100000,"), 25, "\"plate_load_ohm\": -5,");
  const DescriptionFile bad(broken, "broken.json");
  result = rig.run("block.loadDescription", args(amp, bad.file));
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.errorCode, juce::String("invalid_args"));
  EXPECT_NE(result.errorMessage.indexOf("stages[0].plate_load_ohm"), -1) << result.errorMessage.toStdString();

  // Not JSON at all.
  const DescriptionFile garbage("this is not json", "garbage.json");
  result = rig.run("block.loadDescription", args(amp, garbage.file));
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.errorCode, juce::String("invalid_args"));

  // A block that takes no description, and a block that is not there.
  result = rig.run("block.loadDescription", args(gain, good.file));
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.errorCode, juce::String("invalid_args"));
  EXPECT_NE(result.errorMessage.indexOf("takes no description"), -1) << result.errorMessage.toStdString();
  result = rig.run("block.loadDescription", args("nonexistent", good.file));
  EXPECT_FALSE(result.ok);

  // None of that changed the sound, and none of it left an undo step.
  EXPECT_LT(maxDiff(before, rig.render(input)), 1.0e-5);
}

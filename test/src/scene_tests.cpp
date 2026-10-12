// Scenes (plugin/include/Scenes.h, plugin/src/ProcessorScenes.cpp), driven
// through the command layer the way a UI, the remote or a footswitch drives
// them:
//
//   - what a scene holds: every block's on/off state and a value for each
//     scene-assigned parameter, and what stays shared,
//   - the editing rules (assign, unassign, new and removed blocks),
//   - a switch under audio: no click, no chain mute, tails ring out through
//     spillover, and 200 switches against a running audio thread end in the
//     right state,
//   - saved state, presets, rigs from before scenes, undo and redo,
//   - hosted plugin parameters and the expression pedal flag.
#include "hosted_plugin_test_helpers.h"
#include "scene_test_helpers.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <iostream>
#include <thread>
#include <vector>

using namespace scenetest;

namespace {

constexpr const char* kSceneLetters[] = {"A", "B", "C", "D", "E", "F", "G", "H"};

// scene.get without the revision: what has to survive a save, a preset and
// an undo exactly.
juce::String scenesJson(SceneRig& rig) {
  juce::var value = rig.scenes();
  value.getDynamicObject()->removeProperty("revision");
  return juce::JSON::toString(value);
}

juce::ValueTree savedSnapshot(TONE3000Processor& proc) {
  juce::MemoryBlock data;
  proc.getStateInformation(data);
  return juce::ValueTree::readFromData(static_cast<const char*>(data.getData()) + 4,
                                       data.getSize() - 4)
      .getChildWithName("ChainSnapshot");
}

// Whole blocks of a steady sine, quiet enough that every scene of the tests
// below stays well under full scale.
std::vector<float> sineBlocks(const SceneRig& rig, int blocks, int startBlock = 0) {
  std::vector<float> x(static_cast<size_t>(blocks * rig.block));
  const double w = 2.0 * kPi * 220.0 / rig.rate;
  const auto offset = static_cast<double>(startBlock) * rig.block;
  for (size_t i = 0; i < x.size(); ++i)
    x[i] = 0.2f * static_cast<float>(std::sin(w * (offset + static_cast<double>(i))));
  return x;
}

// The largest step a 220 Hz sine of `amplitude` takes between two samples.
double sineStep(const SceneRig& rig, double amplitude) {
  return amplitude * 2.0 * kPi * 220.0 / rig.rate;
}

}  // namespace

TEST(SceneTest, ASceneHoldsBlockStatesAndAssignedValues) {
  SceneRig rig;
  const juce::String gain = rig.addKind("utility.gain");
  const juce::String delay = rig.addKind("delay.simple");
  rig.settle();

  // A fresh rig: scene A active, default names, every block on everywhere,
  // nothing assigned.
  EXPECT_EQ(rig.activeScene(), "A");
  EXPECT_EQ(rig.scene("C")["name"].toString(), "Scene C");
  for (const char* letter : kSceneLetters) {
    EXPECT_TRUE(rig.sceneBlock(letter, gain));
    EXPECT_TRUE(rig.sceneBlock(letter, delay));
  }
  EXPECT_EQ(rig.scenes()["assigned"].size(), 0);

  // A block switch belongs to the active scene.
  rig.setEnabled(gain, false);
  EXPECT_FALSE(rig.sceneBlock("A", gain));
  for (const char* letter : {"B", "C", "D", "E", "F", "G", "H"})
    EXPECT_TRUE(rig.sceneBlock(letter, gain)) << letter;

  // An assigned parameter has a value per scene: a common setting and a kind
  // parameter.
  rig.setParam(delay, "mix", 0.25);
  rig.setParam(delay, "feedback", 20.0);
  rig.assign(delay, "mix");
  rig.assign(delay, "feedback");
  rig.select("B");
  EXPECT_EQ(rig.activeScene(), "B");
  EXPECT_TRUE(rig.blockEnabled(gain)) << "scene B still has the gain block on";
  rig.setParam(delay, "mix", 0.8);
  rig.setSceneValue("C", delay, "feedback", 70.0);

  EXPECT_NEAR(rig.sceneValue("A", delay, "mix"), 0.25, 1e-6);
  EXPECT_NEAR(rig.sceneValue("B", delay, "mix"), 0.8, 1e-6);
  EXPECT_NEAR(rig.sceneValue("C", delay, "mix"), 0.25, 1e-6);
  EXPECT_NEAR(rig.sceneValue("B", delay, "feedback"), 20.0, 1e-4);
  EXPECT_NEAR(rig.sceneValue("C", delay, "feedback"), 70.0, 1e-4);
  // Setting another scene's value does not touch what is heard.
  EXPECT_NEAR(rig.blockValue(delay, "feedback"), 20.0, 1e-4);

  // Selecting a scene puts its states and values on the blocks.
  rig.select("A");
  EXPECT_FALSE(rig.blockEnabled(gain));
  EXPECT_NEAR(rig.blockValue(delay, "mix"), 0.25, 1e-6);
  rig.select("C");
  EXPECT_TRUE(rig.blockEnabled(gain));
  EXPECT_NEAR(rig.blockValue(delay, "feedback"), 70.0, 1e-4);
  rig.select("B");
  EXPECT_NEAR(rig.blockValue(delay, "mix"), 0.8, 1e-6);
  EXPECT_NEAR(rig.blockValue(delay, "feedback"), 20.0, 1e-4);

  // state.get shows it all: the sections, and the assignments on the block.
  const juce::var state = rig.must("state.get");
  EXPECT_EQ(state["scenes"]["activeId"].toString(), "B");
  EXPECT_EQ(state["chain"]["activeScene"].toString(), "B");
  EXPECT_EQ(state["stomps"].size(), 8);
  EXPECT_EQ(state["footswitches"]["switches"].size(), 11);
  const juce::var delayItem = rig.blockItem(delay);
  ASSERT_EQ(delayItem["sceneParams"].size(), 2);
  EXPECT_TRUE(delayItem["sceneParams"].getArray()->contains(juce::var("mix")));
  EXPECT_TRUE(delayItem["sceneParams"].getArray()->contains(juce::var("feedback")));
  EXPECT_EQ(rig.blockItem(gain)["sceneParams"].size(), 0);

  // Names, copy and reset.
  rig.must("scene.rename", R"({"scene":"B","name":"  Lead  "})");
  EXPECT_EQ(rig.scene("B")["name"].toString(), "Lead");
  rig.must("scene.rename", R"({"scene":"B","name":""})");
  EXPECT_EQ(rig.scene("B")["name"].toString(), "Scene B");

  rig.must("scene.copy", R"({"from":"A","to":"D"})");
  EXPECT_FALSE(rig.sceneBlock("D", gain));
  EXPECT_NEAR(rig.sceneValue("D", delay, "mix"), 0.25, 1e-6);
  // Onto the active scene: heard at once.
  rig.must("scene.copy", R"({"from":"A","to":"B"})");
  EXPECT_FALSE(rig.blockEnabled(gain));
  EXPECT_NEAR(rig.blockValue(delay, "mix"), 0.25, 1e-6);

  rig.must("scene.rename", R"({"scene":"D","name":"Solo"})");
  rig.setSceneValue("D", delay, "mix", 0.9);
  rig.must("scene.reset", R"({"scene":"D"})");
  EXPECT_EQ(rig.scene("D")["name"].toString(), "Scene D");
  EXPECT_TRUE(rig.sceneBlock("D", gain)) << "a reset scene has every block on";
  EXPECT_NEAR(rig.sceneValue("D", delay, "mix"), rig.blockValue(delay, "mix"), 1e-6)
      << "a reset scene takes the active scene's values";

  // Bad arguments never reach the engine.
  EXPECT_EQ(rig.run("scene.select", R"({"scene":"K"})").errorCode,
            juce::String(CommandError::invalidArgs));
  EXPECT_EQ(rig.run("scene.assignParam", R"({"blockId":"nope","param":"mix"})").errorCode,
            juce::String(CommandError::notFound));
}

TEST(SceneTest, EditingRules) {
  SceneRig rig;
  const juce::String gain = rig.addKind("utility.gain");
  const juce::String delay = rig.addKind("delay.simple");
  rig.settle();

  // An unassigned parameter has one value, whichever scene it is set in.
  rig.select("B");
  rig.setParam(gain, "gainDb", 4.0);
  rig.setParam(delay, "outputGain", 0.6);
  rig.select("A");
  EXPECT_NEAR(rig.blockValue(gain, "gainDb"), 4.0, 1e-5);
  EXPECT_NEAR(rig.blockValue(delay, "outputGain"), 0.6, 1e-6);

  // Assigning copies the current value to all eight scenes.
  rig.assign(gain, "gainDb");
  for (const char* letter : kSceneLetters)
    EXPECT_NEAR(rig.sceneValue(letter, gain, "gainDb"), 4.0, 1e-5) << letter;
  // Assigning again changes nothing.
  rig.setSceneValue("C", gain, "gainDb", -9.0);
  rig.assign(gain, "gainDb");
  EXPECT_NEAR(rig.sceneValue("C", gain, "gainDb"), -9.0, 1e-5);

  // From then on a set writes the active scene only, and values are clamped
  // to the parameter's range in any scene.
  rig.setParam(gain, "gainDb", 1.5);
  EXPECT_NEAR(rig.sceneValue("A", gain, "gainDb"), 1.5, 1e-5);
  EXPECT_NEAR(rig.sceneValue("B", gain, "gainDb"), 4.0, 1e-5);
  rig.setSceneValue("D", gain, "gainDb", 99.0);
  EXPECT_NEAR(rig.sceneValue("D", gain, "gainDb"), 24.0, 1e-5);
  rig.setSceneValue("A", gain, "gainDb", 2.0);  // the active scene: on the block
  EXPECT_NEAR(rig.blockValue(gain, "gainDb"), 2.0, 1e-5);

  // Unassigning keeps the active scene's value as the shared one.
  rig.select("C");
  EXPECT_NEAR(rig.blockValue(gain, "gainDb"), -9.0, 1e-5);
  rig.must("scene.unassignParam",
           "{\"blockId\":" + quoted(gain) + R"(,"param":"gainDb"})");
  EXPECT_FALSE(rig.isAssigned(gain, "gainDb"));
  rig.select("A");
  EXPECT_NEAR(rig.blockValue(gain, "gainDb"), -9.0, 1e-5);
  EXPECT_FALSE(rig.run("scene.unassignParam",
                       "{\"blockId\":" + quoted(gain) + R"(,"param":"gainDb"})")
                   .ok);
  EXPECT_FALSE(rig.run("scene.setParamValue", "{\"scene\":\"B\",\"blockId\":" + quoted(gain) +
                                                  R"(,"param":"gainDb","value":1})")
                   .ok)
      << "a value per scene needs the parameter assigned";

  // What cannot be a scene parameter: the switch (it is per scene already),
  // normalize, an unknown id, and a parameter that moves the block's
  // latency (a scene switch never mutes the chain).
  const auto refused = [&](const juce::String& blockId, const char* param) {
    const CommandResult result = rig.run(
        "scene.assignParam", "{\"blockId\":" + quoted(blockId) + ",\"param\":" + quoted(param) + "}");
    EXPECT_FALSE(result.ok) << param;
    EXPECT_EQ(result.errorCode, juce::String(CommandError::failed)) << param;
  };
  refused(gain, "enabled");
  refused(gain, "normalize");
  refused(gain, "noSuchParam");
  const juce::String octaver = rig.addKind("pitch.octaver");
  rig.waitLoaded();
  refused(octaver, "window");
  rig.must("chain.removeBlock", "{\"blockId\":" + quoted(octaver) + "}");

  // A new block is on in every scene, also when it is added while another
  // scene is active and other blocks have states of their own.
  rig.select("C");
  rig.setEnabled(gain, false);
  const juce::String reverb = rig.addKind("reverb.room");
  rig.waitLoaded();
  for (const char* letter : kSceneLetters)
    EXPECT_TRUE(rig.sceneBlock(letter, reverb)) << letter;
  // Its first toggle then writes one scene.
  rig.setEnabled(reverb, false);
  EXPECT_FALSE(rig.sceneBlock("C", reverb));
  EXPECT_TRUE(rig.sceneBlock("A", reverb));
  EXPECT_TRUE(rig.sceneBlock("H", reverb));

  // A block added bypassed (a copy of a block that is off) is off in every
  // scene until it is switched on somewhere.
  const juce::String copy =
      rig.must("chain.duplicateBlock", "{\"blockId\":" + quoted(reverb) + "}")["blockId"].toString();
  rig.waitLoaded();
  for (const char* letter : kSceneLetters)
    EXPECT_FALSE(rig.sceneBlock(letter, copy)) << letter;
  rig.setEnabled(copy, true);
  EXPECT_TRUE(rig.sceneBlock("C", copy));
  EXPECT_FALSE(rig.sceneBlock("A", copy));

  // Removing a block removes what the scenes held for it.
  rig.assign(reverb, "mix");
  rig.must("stomp.assign", "{\"switch\":\"A\",\"blockIds\":[" + quoted(reverb) + "," +
                               quoted(delay) + "]}");
  rig.must("chain.removeBlock", "{\"blockId\":" + quoted(reverb) + "}");
  const juce::var after = rig.scenes();
  EXPECT_EQ(after["assigned"].size(), 0);
  EXPECT_TRUE(after["scenes"][0]["blocks"][juce::Identifier(reverb)].isVoid());
  const juce::var stompA = rig.must("stomp.get")["stomps"][0];
  ASSERT_EQ(stompA["blockIds"].size(), 1);
  EXPECT_EQ(stompA["blockIds"][0].toString(), delay);
  const juce::ValueTree saved = savedSnapshot(rig.proc).getChildWithName("Scenes");
  ASSERT_TRUE(saved.isValid());
  for (const auto& sceneNode : saved)
    for (const auto& entry : sceneNode)
      EXPECT_NE(entry.getProperty(entry.hasType("Block") ? "id" : "blockId").toString(), reverb);
  // An undo brings the block back with its scene data.
  ASSERT_TRUE(rig.proc.undoChain());
  rig.waitLoaded();
  EXPECT_TRUE(rig.isAssigned(reverb, "mix"));
  EXPECT_FALSE(rig.sceneBlock("C", reverb));
  EXPECT_EQ(rig.must("stomp.get")["stomps"][0]["blockIds"].size(), 2);
}

TEST(SceneTest, SwitchingUnderAudioIsClickFreeAndNeverMutesTheChain) {
  SceneRig rig;
  const juce::String trim = rig.addKind("utility.gain");
  const juce::String pad = rig.addKind("utility.gain");
  const juce::String delay = rig.addKind("delay.simple");
  rig.setParam(pad, "gainDb", -12.0);
  rig.setParam(delay, "timeMs", 30.0);
  rig.setParam(delay, "feedback", 0.0);
  // Scene A: trim -6 dB, the pad in, a little delay, the delay's output down.
  // Scene B: trim +3 dB, the pad out, more delay, its output at unity.
  rig.setParam(trim, "gainDb", -6.0);
  rig.setParam(delay, "mix", 0.1);
  rig.setParam(delay, "outputGain", 0.45);
  rig.assign(trim, "gainDb");
  rig.assign(delay, "mix");
  rig.assign(delay, "outputGain");
  rig.setSceneValue("B", trim, "gainDb", 3.0);
  rig.setSceneValue("B", delay, "mix", 0.4);
  rig.setSceneValue("B", delay, "outputGain", 0.5);
  rig.select("B");
  rig.setEnabled(pad, false);
  rig.select("A");
  rig.settle();

  int blocksDone = 0;
  std::vector<float> out;
  const auto run = [&](int blocks) {
    const std::vector<float> rendered = rig.render(sineBlocks(rig, blocks, blocksDone));
    blocksDone += blocks;
    out.insert(out.end(), rendered.begin(), rendered.end());
    return rendered;
  };

  // Each scene's own level, settled.
  const std::vector<float> steadyA = run(60);
  rig.select("B");
  EXPECT_FALSE(rig.proc.isChainEditFadeHeld());
  run(40);
  const std::vector<float> steadyB = run(40);
  const double levelA = rmsOf(steadyA, steadyA.size() / 2, steadyA.size());
  const double levelB = rmsOf(steadyB, 0, steadyB.size());
  ASSERT_GT(levelA, 0.005);
  EXPECT_GT(levelB, levelA * 4.0) << "the two scenes should differ by well over 12 dB";

  // Back and forth: a full fade apart, then within a fade of each other
  // (25 ms is 1200 samples, under five blocks).
  for (const int gap : {40, 40, 3, 2, 1, 7, 1, 1, 30}) {
    rig.select(rig.activeScene() == "A" ? "B" : "A");
    EXPECT_FALSE(rig.proc.isChainEditFadeHeld()) << "a scene switch took the chain-edit fade";
    run(gap);
  }
  run(40);

  EXPECT_TRUE(allFinite(out));
  // No discontinuity beyond the fades: the largest sample-to-sample step is
  // that of the loudest sine the rig can make. A block switched without its
  // fade would step by up to the pad's 0.75 of the signal, ten times this.
  // (The bound is the trim's +3 dB on the 0.2 sine: the delay's echo arrives
  // out of phase at this frequency, so it only ever takes level away.)
  EXPECT_LT(largestStep(out), sineStep(rig, 0.2 * 1.42) * 1.3 + 0.001);
  // Printed so a drift shows long before it reaches the bound.
  std::cout << "[scene switch] levels " << levelA << " / " << levelB << ", largest step "
            << largestStep(out) << " (bound " << sineStep(rig, 0.2 * 1.42) * 1.3 + 0.001 << ")\n";
  // And never silence: no block of the run is quieter than half the quiet
  // scene, which is what a chain mute would leave behind.
  const size_t blockSize = static_cast<size_t>(rig.block);
  for (size_t start = 20 * blockSize; start + blockSize <= out.size(); start += blockSize)
    ASSERT_GT(rmsOf(out, start, start + blockSize), levelA * 0.5) << "at sample " << start;
}

TEST(SceneTest, ADelayASceneTurnsOffRingsOutWithSpilloverAndIsCutWithout) {
  // One noise burst into a 200 ms delay, then a switch to a scene that has
  // the delay off, then silence. The burst's first echo is due well after
  // the switch.
  const auto echoAfterSwitch = [](bool spillover) {
    SceneRig rig;
    const juce::String delay = rig.addKind("delay.simple");
    rig.setParam(delay, "timeMs", 200.0);
    rig.setParam(delay, "feedback", 50.0);
    rig.setParam(delay, "mix", 0.5);
    rig.setParam(delay, "spillover", spillover ? 1.0 : 0.0);
    rig.select("B");
    rig.setEnabled(delay, false);
    rig.select("A");
    rig.settle();

    const int burstBlocks = 20;  // 5120 samples, 107 ms
    rig.render(makeNoise(burstBlocks * rig.block, 7, 0.25f));
    rig.select("B");
    EXPECT_FALSE(rig.proc.isChainEditFadeHeld());
    EXPECT_FALSE(rig.blockEnabled(delay));
    const std::vector<float> after = rig.render(std::vector<float>(200 * 256, 0.0f));
    EXPECT_TRUE(allFinite(after));
    // The echo of the burst: 9600 samples after it started, 4480 after the
    // switch, 5120 long.
    return rmsOf(after, 4480, 4480 + 5120);
  };

  const double ringing = echoAfterSwitch(true);
  const double cut = echoAfterSwitch(false);
  std::cout << "[scene spillover] echo rms after the switch: " << ringing << " ringing, " << cut
            << " cut\n";
  EXPECT_GT(ringing, 0.03) << "the released delay did not ring out";
  EXPECT_LT(cut, 0.002) << "the delay kept sounding with its spillover switch off";
}

TEST(SceneTest, TwoHundredSwitchesAgainstARunningAudioThread) {
  SceneRig rig;
  const juce::String trim = rig.addKind("utility.gain");
  const juce::String pad = rig.addKind("utility.gain");
  rig.setParam(pad, "gainDb", -12.0);
  rig.assign(trim, "gainDb");
  rig.assign(pad, "mix");
  const double trimDb[] = {-6.0, 0.0, 3.0, -3.0};
  const bool padOn[] = {true, false, true, false};
  const double padMix[] = {1.0, 1.0, 0.5, 0.3};
  const char* letters[] = {"A", "B", "C", "D"};
  for (int i = 0; i < 4; ++i) {
    rig.select(letters[i]);
    rig.setParam(trim, "gainDb", trimDb[i]);
    rig.setParam(pad, "mix", padMix[i]);
    rig.setEnabled(pad, padOn[i]);
  }
  rig.select("A");
  rig.settle();

  // The audio thread: a steady sine, as fast as it will go.
  std::atomic<bool> stop{false};
  std::atomic<int> blocksRendered{0};
  std::vector<float> out;
  out.reserve(1 << 22);
  std::thread audio([&] {
    juce::AudioBuffer<float> buffer(2, rig.block);
    juce::MidiBuffer midi;
    int blockIndex = 0;
    while (!stop.load()) {
      const std::vector<float> in = sineBlocks(rig, 1, blockIndex++);
      buffer.copyFrom(0, 0, in.data(), rig.block);
      buffer.copyFrom(1, 0, in.data(), rig.block);
      rig.proc.processBlock(buffer, midi);
      if (out.size() + static_cast<size_t>(rig.block) <= out.capacity())
        out.insert(out.end(), buffer.getReadPointer(0), buffer.getReadPointer(0) + rig.block);
      blocksRendered.fetch_add(1);
      // Paced, so the 200 switches land across fades in every phase rather
      // than inside a handful of blocks (a fade is about five of them).
      juce::Thread::sleep(1);
    }
  });

  bool fadeTaken = false;
  for (int i = 0; i < 200; ++i) {
    // A fixed but irregular walk through the four scenes that ends on C.
    const int target = i == 199 ? 2 : (i * 7 + i / 3) % 4;
    rig.proc.selectScene(target);
    fadeTaken = fadeTaken || rig.proc.isChainEditFadeHeld();
    juce::Thread::sleep(1);
  }
  // Let the last switch's fades finish under audio.
  const int reached = blocksRendered.load();
  while (blocksRendered.load() < reached + 60)
    juce::Thread::sleep(1);
  stop.store(true);
  audio.join();

  EXPECT_FALSE(fadeTaken) << "a scene switch took the chain-edit fade";
  ASSERT_GT(out.size(), static_cast<size_t>(60 * rig.block));
  EXPECT_TRUE(allFinite(out));
  // Glitch-bounded: nothing steeper than the loudest scene's own sine (+3 dB
  // on a 0.2 sine) with room for the fades riding on it.
  EXPECT_LT(largestStep(out), sineStep(rig, 0.2 * 1.42) * 1.5 + 0.002);
  std::cout << "[scene rapid] " << out.size() / static_cast<size_t>(rig.block)
            << " blocks under 200 switches, largest step " << largestStep(out) << " (bound "
            << sineStep(rig, 0.2 * 1.42) * 1.5 + 0.002 << ")\n";

  // The right state: scene C on the blocks, and the scenes as they were set.
  EXPECT_EQ(rig.activeScene(), "C");
  EXPECT_NEAR(rig.blockValue(trim, "gainDb"), 3.0, 1e-5);
  EXPECT_NEAR(rig.blockValue(pad, "mix"), 0.5, 1e-6);
  EXPECT_TRUE(rig.blockEnabled(pad));
  for (int i = 0; i < 4; ++i) {
    EXPECT_NEAR(rig.sceneValue(letters[i], trim, "gainDb"), trimDb[i], 1e-5) << letters[i];
    EXPECT_NEAR(rig.sceneValue(letters[i], pad, "mix"), padMix[i], 1e-6) << letters[i];
    EXPECT_EQ(rig.sceneBlock(letters[i], pad), padOn[i]) << letters[i];
  }
  // And what is heard at the end is scene C: +3 dB into a half-mixed -12 dB
  // pad, 0.2 x 1.41 x (0.5 + 0.5 x 0.25).
  const double expected = 0.2 * std::pow(10.0, 3.0 / 20.0) * (0.5 + 0.5 * std::pow(10.0, -12.0 / 20.0));
  EXPECT_NEAR(rmsOf(out, out.size() - static_cast<size_t>(8 * rig.block), out.size()) * std::sqrt(2.0),
              expected, expected * 0.03);
}

TEST(SceneTest, SavedStateAndPresetsKeepEveryScene) {
  SceneRig rig;
  const juce::String gain = rig.addKind("utility.gain");
  const juce::String delay = rig.addKind("delay.simple");
  rig.settle();

  rig.assign(delay, "mix");
  rig.assign(gain, "gainDb");
  rig.setSceneValue("B", delay, "mix", 0.7);
  rig.setSceneValue("F", gain, "gainDb", -4.5);
  rig.must("scene.rename", R"({"scene":"B","name":"Chorus"})");
  rig.must("expression.assignParam", "{\"blockId\":" + quoted(delay) + R"(,"param":"feedback"})");
  rig.must("stomp.assign", "{\"switch\":\"C\",\"blockIds\":[" + quoted(delay) +
                               R"(],"label":"Echo","momentary":true})");
  rig.select("B");
  rig.setEnabled(gain, false);
  rig.select("C");
  rig.setParam(delay, "mix", 0.15);  // scene C is the one active at save time
  const juce::String before = scenesJson(rig);
  const juce::String stompsBefore = juce::JSON::toString(rig.must("stomp.get")["stomps"]);

  // The saved shape: one Scenes node and one Stomps node beside the lanes.
  const juce::ValueTree snapshot = savedSnapshot(rig.proc);
  const juce::ValueTree scenesNode = snapshot.getChildWithName("Scenes");
  ASSERT_TRUE(scenesNode.isValid());
  EXPECT_EQ(static_cast<int>(scenesNode.getProperty("active")), 2);
  EXPECT_EQ(static_cast<int>(scenesNode.getProperty("version")), 1);
  int sceneNodes = 0;
  for (const auto& child : scenesNode)
    sceneNodes += child.hasType("Scene") ? 1 : 0;
  EXPECT_EQ(sceneNodes, 8);
  EXPECT_TRUE(scenesNode.getChildWithName("Expression").isValid());
  EXPECT_TRUE(snapshot.getChildWithName("Stomps").isValid());
  // The block nodes hold the active scene, which is what a build without
  // scenes would open.
  for (const auto& block : snapshot.getChildWithName("ChainBlocks"))
    if (block.getProperty("id").toString() == delay)
      EXPECT_NEAR(static_cast<double>(block.getProperty("mix")), 0.15, 1e-6);

  // Through the plugin state into a fresh processor.
  juce::MemoryBlock blob;
  rig.proc.getStateInformation(blob);
  {
    SceneRig reopened;
    reopened.proc.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
    reopened.waitLoaded();
    EXPECT_EQ(scenesJson(reopened), before);
    EXPECT_EQ(reopened.activeScene(), "C") << "the scene active at save time is the default";
    EXPECT_EQ(juce::JSON::toString(reopened.must("stomp.get")["stomps"]), stompsBefore);
    EXPECT_NEAR(reopened.blockValue(delay, "mix"), 0.15, 1e-6);
    reopened.select("B");
    EXPECT_FALSE(reopened.blockEnabled(gain));
    EXPECT_NEAR(reopened.blockValue(delay, "mix"), 0.7, 1e-6);
    // And saving again writes the same tree.
    reopened.select("C");
    EXPECT_TRUE(savedSnapshot(reopened.proc).isEquivalentTo(snapshot));
  }

  // Through a preset: save, wreck the scenes, load.
  const juce::String presetId = rig.must("preset.save", R"({"name":"Scenes"})")["id"].toString();
  ASSERT_TRUE(presetId.isNotEmpty());
  rig.select("A");
  rig.must("scene.rename", R"({"scene":"B","name":"Other"})");
  rig.must("scene.unassignParam", "{\"blockId\":" + quoted(delay) + R"(,"param":"mix"})");
  rig.must("stomp.unassign", R"({"switch":"C"})");
  rig.setEnabled(gain, false);
  ASSERT_NE(scenesJson(rig), before);
  rig.must("preset.load", "{\"id\":" + quoted(presetId) + "}");
  rig.waitLoaded();
  EXPECT_EQ(scenesJson(rig), before);
  EXPECT_EQ(rig.activeScene(), "C");
  EXPECT_EQ(juce::JSON::toString(rig.must("stomp.get")["stomps"]), stompsBefore);
  EXPECT_TRUE(rig.blockEnabled(gain));
  EXPECT_NEAR(rig.blockValue(delay, "mix"), 0.15, 1e-6);
}

TEST(SceneTest, ARigSavedBeforeScenesLoadsWithEverySceneEqualToIt) {
  // A state as an older build wrote it: lanes only, one block off.
  juce::ValueTree snapshot("ChainSnapshot");
  juce::ValueTree left("ChainBlocks");
  juce::ValueTree delay = makeKindBlockTree("blk-delay", "delay.simple", "Delay", {{"feedback", 45.0}});
  delay.setProperty("mix", 0.3f, nullptr);
  juce::ValueTree gain = makeKindBlockTree("blk-gain", "utility.gain", "Gain", {{"gainDb", 2.0}});
  gain.setProperty("enabled", false, nullptr);
  left.appendChild(delay, nullptr);
  left.appendChild(gain, nullptr);
  snapshot.appendChild(left, nullptr);

  SceneRig rig;
  // Something to be replaced, so the load is seen to clear scene data too.
  const juce::String old = rig.addKind("utility.gain");
  rig.assign(old, "gainDb");
  rig.select("E");
  rig.proc.restoreFromTree(snapshot);
  rig.waitLoaded();

  const juce::var scenes = rig.scenes();
  EXPECT_EQ(scenes["activeId"].toString(), "A");
  EXPECT_EQ(scenes["assigned"].size(), 0);
  EXPECT_EQ(scenes["expression"].size(), 0);
  for (const char* letter : kSceneLetters) {
    EXPECT_EQ(rig.scene(letter)["name"].toString(), "Scene " + juce::String(letter));
    EXPECT_TRUE(rig.sceneBlock(letter, "blk-delay")) << letter;
    EXPECT_FALSE(rig.sceneBlock(letter, "blk-gain")) << letter;
    EXPECT_EQ(rig.scene(letter)["values"].size(), 0);
  }
  // Selecting any of them changes nothing.
  rig.select("D");
  EXPECT_FALSE(rig.blockEnabled("blk-gain"));
  EXPECT_NEAR(rig.blockValue("blk-delay", "mix"), 0.3, 1e-6);
  EXPECT_NEAR(rig.blockValue("blk-delay", "feedback"), 45.0, 1e-4);
  rig.select("A");

  // And saving it again adds nothing: the rig never used scenes or stomps.
  const juce::ValueTree saved = savedSnapshot(rig.proc);
  EXPECT_FALSE(saved.getChildWithName("Scenes").isValid());
  EXPECT_FALSE(saved.getChildWithName("Stomps").isValid());
  EXPECT_EQ(saved.getNumChildren(), 2);
}

TEST(SceneTest, UndoAndRedoCoverSceneEdits) {
  SceneRig rig;
  const juce::String gain = rig.addKind("utility.gain");
  const juce::String delay = rig.addKind("delay.simple");
  rig.settle();
  const double startMix = rig.blockValue(delay, "mix");

  // An assignment.
  rig.assign(delay, "mix");
  ASSERT_TRUE(rig.isAssigned(delay, "mix"));
  ASSERT_TRUE(rig.proc.undoChain());
  EXPECT_FALSE(rig.isAssigned(delay, "mix"));
  ASSERT_TRUE(rig.proc.redoChain());
  EXPECT_TRUE(rig.isAssigned(delay, "mix"));

  // A value in a scene that is not the active one.
  rig.setSceneValue("C", delay, "mix", 0.9);
  ASSERT_TRUE(rig.proc.undoChain());
  EXPECT_NEAR(rig.sceneValue("C", delay, "mix"), startMix, 1e-6);
  ASSERT_TRUE(rig.proc.redoChain());
  EXPECT_NEAR(rig.sceneValue("C", delay, "mix"), 0.9, 1e-6);

  // A name.
  rig.must("scene.rename", R"({"scene":"B","name":"Lead"})");
  ASSERT_TRUE(rig.proc.undoChain());
  EXPECT_EQ(rig.scene("B")["name"].toString(), "Scene B");

  // A block switch and an assigned value in the active scene: each undo
  // gives the scene back what it had, and leaves the others alone.
  rig.select("B");
  rig.setEnabled(gain, false);
  rig.setParam(delay, "mix", 0.2);
  ASSERT_TRUE(rig.proc.undoChain());
  EXPECT_NEAR(rig.blockValue(delay, "mix"), startMix, 1e-6);
  EXPECT_NEAR(rig.sceneValue("B", delay, "mix"), startMix, 1e-6);
  EXPECT_FALSE(rig.sceneBlock("B", gain));
  ASSERT_TRUE(rig.proc.undoChain());
  EXPECT_TRUE(rig.blockEnabled(gain));
  EXPECT_TRUE(rig.sceneBlock("B", gain));
  EXPECT_NEAR(rig.sceneValue("C", delay, "mix"), 0.9, 1e-6);
  EXPECT_EQ(rig.activeScene(), "B");
  ASSERT_TRUE(rig.proc.redoChain());
  EXPECT_FALSE(rig.blockEnabled(gain));
  EXPECT_TRUE(rig.sceneBlock("A", gain));

  // A scene switch is not an undo step. One edit in scene A, then three
  // switches: one undo takes the edit back (and, the snapshot being the rig
  // as it was when the edit was made, shows it in scene A again).
  SceneRig second;
  const juce::String block = second.addKind("utility.gain");
  second.settle();
  second.setParam(block, "gainDb", 3.0);
  second.select("B");
  second.select("C");
  second.select("D");
  ASSERT_TRUE(second.proc.undoChain());
  EXPECT_NEAR(second.blockValue(block, "gainDb"), 0.0, 1e-5);
  EXPECT_EQ(second.activeScene(), "A");
  ASSERT_TRUE(second.proc.undoChain());  // adding the block
  EXPECT_FALSE(second.proc.undoChain()) << "the scene switches left steps in the history";
  ASSERT_TRUE(second.proc.redoChain());
  ASSERT_TRUE(second.proc.redoChain());
  second.waitLoaded();
  EXPECT_NEAR(second.blockValue(block, "gainDb"), 3.0, 1e-5);
  // The redo is the rig as it was when the undo was made: in scene D.
  EXPECT_EQ(second.activeScene(), "D");
}

TEST(SceneTest, HostedPluginAndBuiltInParametersCanBeSceneAssigned) {
  ScannerSandbox sandbox;
  const juce::String identifier = scanTestPlugin();
  SceneRig rig;
  const juce::String delay = rig.addKind("delay.simple");
  const juce::String plugin =
      rig.must("chain.addPlugin", "{\"identifier\":" + quoted(identifier) + "}")["blockId"].toString();
  rig.waitLoaded();

  // The plugin's Gain parameter, by the id the plugin gives it.
  juce::String gainId;
  const auto pluginGain = [&]() -> double {
    const juce::var listed = rig.must("block.listParams", "{\"blockId\":" + quoted(plugin) + "}");
    for (const auto& entry : *listed["params"].getArray())
      if (entry["scope"].toString() == "kind" && entry["name"].toString() == "Gain") {
        gainId = entry["id"].toString();
        return static_cast<double>(entry["value"]);
      }
    return -1.0;
  };
  ASSERT_DOUBLE_EQ(pluginGain(), 0.5);
  ASSERT_TRUE(gainId.isNotEmpty());

  rig.assign(plugin, gainId.toRawUTF8());
  rig.assign(delay, "feedback");
  rig.setSceneValue("B", plugin, gainId.toRawUTF8(), 0.9);
  rig.setSceneValue("B", delay, "feedback", 80.0);
  rig.setParam(plugin, gainId.toRawUTF8(), 0.25);  // scene A
  EXPECT_NEAR(rig.sceneValue("A", plugin, gainId.toRawUTF8()), 0.25, 1e-6);
  EXPECT_NEAR(rig.sceneValue("C", plugin, gainId.toRawUTF8()), 0.5, 1e-6);

  rig.select("B");
  EXPECT_NEAR(pluginGain(), 0.9, 1e-6);
  EXPECT_NEAR(rig.blockValue(delay, "feedback"), 80.0, 1e-4);
  rig.select("C");
  EXPECT_NEAR(pluginGain(), 0.5, 1e-6);
  rig.select("A");
  EXPECT_NEAR(pluginGain(), 0.25, 1e-6);
  EXPECT_NEAR(rig.blockValue(delay, "feedback"), 35.0, 1e-4);

  // The plugin hears it: its gain is exact, 0.25 normalised against 0.9.
  rig.settle();
  const std::vector<float> in = makeSine(40 * rig.block, 440.0, 0.1f);
  rig.setEnabled(delay, false);
  rig.select("B");
  rig.setEnabled(delay, false);
  rig.select("A");
  rig.render(in);
  const std::vector<float> quiet = rig.render(in);
  rig.select("B");
  rig.render(in);
  const std::vector<float> loud = rig.render(in);
  EXPECT_GT(rmsOf(loud, 0, loud.size()), rmsOf(quiet, 0, quiet.size()) * 1.5);

  // And it survives a save, values in the plugin's own normalised 0 to 1.
  juce::MemoryBlock blob;
  rig.proc.getStateInformation(blob);
  SceneRig reopened;
  reopened.proc.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
  reopened.waitLoaded();
  EXPECT_NEAR(reopened.sceneValue("B", plugin, gainId.toRawUTF8()), 0.9, 1e-6);
  EXPECT_NEAR(reopened.sceneValue("A", plugin, gainId.toRawUTF8()), 0.25, 1e-6);
  EXPECT_EQ(reopened.activeScene(), "B");
}

TEST(SceneTest, ExpressionFlaggedParametersStayOutOfTheScenes) {
  SceneRig rig;
  const juce::String delay = rig.addKind("delay.simple");
  rig.settle();
  const juce::String mixArgs = "{\"blockId\":" + quoted(delay) + R"(,"param":"mix"})";
  const juce::String feedbackArgs = "{\"blockId\":" + quoted(delay) + R"(,"param":"feedback"})";

  // Flagged first: it cannot be assigned.
  rig.must("expression.assignParam", mixArgs);
  const CommandResult refused = rig.run("scene.assignParam", mixArgs);
  EXPECT_FALSE(refused.ok);
  EXPECT_EQ(refused.errorCode, juce::String(CommandError::failed));
  EXPECT_TRUE(refused.errorMessage.contains("expression"));
  EXPECT_FALSE(rig.isAssigned(delay, "mix"));

  // Assigned first: flagging takes it out of the scenes, and the block
  // keeps the value the active scene had.
  rig.assign(delay, "feedback");
  rig.setSceneValue("B", delay, "feedback", 80.0);
  rig.select("B");
  ASSERT_NEAR(rig.blockValue(delay, "feedback"), 80.0, 1e-4);
  rig.must("expression.assignParam", feedbackArgs);
  EXPECT_FALSE(rig.isAssigned(delay, "feedback"));
  EXPECT_NEAR(rig.blockValue(delay, "feedback"), 80.0, 1e-4);
  // Scene switches leave it where the pedal (or anyone else) put it.
  rig.setParam(delay, "feedback", 55.0);
  rig.select("A");
  EXPECT_NEAR(rig.blockValue(delay, "feedback"), 55.0, 1e-4);
  rig.select("C");
  EXPECT_NEAR(rig.blockValue(delay, "feedback"), 55.0, 1e-4);

  // Both flags are listed, in scene.get and on the block.
  EXPECT_EQ(rig.scenes()["expression"].size(), 2);
  const juce::var item = rig.blockItem(delay);
  EXPECT_EQ(item["expressionParams"].size(), 2);
  EXPECT_EQ(item["sceneParams"].size(), 0);

  // The flag is saved with the rig.
  juce::MemoryBlock blob;
  rig.proc.getStateInformation(blob);
  SceneRig reopened;
  reopened.proc.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
  reopened.waitLoaded();
  EXPECT_EQ(reopened.scenes()["expression"].size(), 2);
  EXPECT_FALSE(reopened.run("scene.assignParam", mixArgs).ok);

  // Cleared, the parameter can be scene-assigned again.
  rig.must("expression.unassignParam", mixArgs);
  rig.must("scene.assignParam", mixArgs);
  EXPECT_TRUE(rig.isAssigned(delay, "mix"));
  // The other way round as well: flagging what the last line assigned.
  rig.must("expression.assignParam", mixArgs);
  EXPECT_FALSE(rig.isAssigned(delay, "mix"));
}

// Stomp assignments, footswitch modes, tap tempo and the fixed MIDI controls
// (plugin/include/Scenes.h, plugin/src/ProcessorScenes.cpp, MidiMapper):
//
//   - a stomp flips its blocks together, latching or momentary,
//   - what switches A to H do in Preset, Scene, Stomp and Hybrid mode,
//     with bank paging over a temporary preset store,
//   - the state snapshot a pedalboard view draws from,
//   - tap tempo: the arithmetic, the command and the tempo switch,
//   - CC 43 / 44 / 45 / 47 through processBlock, and how a mapping of the
//     user's own takes a controller number over,
//   - presses from another thread,
//   - the mode persisting in a settings file that is not the user's.
#include "scene_test_helpers.h"

#include <gtest/gtest.h>
#include <juce_events/juce_events.h>

#include <cmath>
#include <thread>
#include <utility>
#include <vector>

using namespace scenetest;

namespace {

void pumpMessages(int ms = 30) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }

// One controller event through the processor's normal path: a processBlock
// carrying it.
void sendCc(SceneRig& rig, int number, int value, int channel = 1) {
  juce::AudioBuffer<float> buffer(2, rig.block);
  buffer.clear();
  juce::MidiBuffer midi;
  midi.addEvent(juce::MidiMessage::controllerEvent(channel, number, value), 0);
  rig.proc.processBlock(buffer, midi);
}

void setMode(SceneRig& rig, const char* mode) {
  rig.must("footswitch.setMode", "{\"mode\":" + quoted(mode) + "}");
}

void setRows(SceneRig& rig, const char* top, const char* bottom) {
  rig.must("footswitch.setHybridRows",
           "{\"top\":" + quoted(top) + ",\"bottom\":" + quoted(bottom) + "}");
}

// Ten presets in the rig's temp store, each a one-block rig whose gain
// names it. Returns their ids in list order.
juce::StringArray saveTenPresets(SceneRig& rig, const juce::String& gainBlock) {
  for (int i = 0; i < 10; ++i) {
    rig.setParam(gainBlock, "gainDb", static_cast<double>(i));
    rig.must("preset.save", "{\"name\":" + quoted("Preset " + juce::String(i).paddedLeft('0', 2)) + "}");
  }
  juce::StringArray ids;
  const juce::var listed = rig.must("preset.list");
  if (const auto* presets = listed["presets"].getArray())
    for (const auto& preset : *presets)
      ids.add(preset["id"].toString());
  EXPECT_EQ(ids.size(), 10);
  return ids;
}

juce::String activePresetId(SceneRig& rig) {
  return rig.proc.getChainState(-1)["preset"]["id"].toString();
}

}  // namespace

TEST(StompTest, AStompFlipsItsBlocksTogether) {
  SceneRig rig;
  const juce::String gain = rig.addKind("utility.gain");
  const juce::String delay = rig.addKind("delay.simple");
  const juce::String reverb = rig.addKind("reverb.room");
  rig.settle();
  rig.useScenes(8);
  setMode(rig, "stomp");

  // Two blocks on one switch, one of them off: the press flips each, so the
  // switch swaps them.
  rig.setEnabled(delay, false);
  const juce::var assigned = rig.must(
      "stomp.assign", "{\"switch\":\"B\",\"blockIds\":[" + quoted(gain) + "," + quoted(delay) + "]}");
  EXPECT_EQ(assigned["stomps"][1]["blockIds"].size(), 2);
  EXPECT_EQ(assigned["stomps"][1]["switch"].toString(), "B");
  EXPECT_FALSE(static_cast<bool>(assigned["stomps"][1]["momentary"]));

  rig.stomp("B");
  EXPECT_FALSE(rig.blockEnabled(gain));
  EXPECT_TRUE(rig.blockEnabled(delay));
  EXPECT_TRUE(rig.blockEnabled(reverb)) << "a block that is not on the switch moved";
  rig.stomp("B");
  EXPECT_TRUE(rig.blockEnabled(gain));
  EXPECT_FALSE(rig.blockEnabled(delay));

  // One press is one undo step for all of its blocks.
  rig.stomp("B");
  ASSERT_TRUE(rig.proc.undoChain());
  EXPECT_TRUE(rig.blockEnabled(gain));
  EXPECT_FALSE(rig.blockEnabled(delay));

  // A switch without an assignment does nothing.
  rig.stomp("C");
  EXPECT_TRUE(rig.blockEnabled(reverb));

  // The switch writes the active scene, like any block toggle.
  rig.select("B");
  rig.stomp("B");
  EXPECT_FALSE(rig.sceneBlock("B", gain));
  EXPECT_TRUE(rig.sceneBlock("A", gain));

  // Taking one block off, then clearing the switch.
  rig.must("stomp.unassign", "{\"switch\":\"B\",\"blockId\":" + quoted(gain) + "}");
  rig.stomp("B");
  EXPECT_FALSE(rig.blockEnabled(gain)) << "a block taken off the switch still followed it";
  EXPECT_FALSE(rig.run("stomp.unassign", "{\"switch\":\"B\",\"blockId\":" + quoted(gain) + "}").ok);
  rig.must("stomp.unassign", R"({"switch":"B"})");
  EXPECT_EQ(rig.must("stomp.get")["stomps"][1]["blockIds"].size(), 0);

  // Arguments.
  EXPECT_EQ(rig.run("stomp.assign", R"({"switch":"B","blockIds":["nope"]})").errorCode,
            juce::String(CommandError::notFound));
  EXPECT_EQ(rig.run("stomp.assign", R"({"switch":"B","blockIds":[]})").errorCode,
            juce::String(CommandError::invalidArgs));
  EXPECT_EQ(rig.run("stomp.assign", "{\"switch\":\"tempo\",\"blockIds\":[" + quoted(gain) + "]}")
                .errorCode,
            juce::String(CommandError::invalidArgs));
}

TEST(StompTest, AMomentaryStompIsOnWhileHeld) {
  SceneRig rig;
  const juce::String delay = rig.addKind("delay.simple");
  rig.settle();
  setMode(rig, "stomp");
  rig.setEnabled(delay, false);
  rig.must("stomp.assign", "{\"switch\":\"A\",\"blockIds\":[" + quoted(delay) + "]}");
  rig.must("stomp.setOptions", R"({"switch":"A","momentary":true,"label":"Swell"})");
  const juce::var stomp = rig.must("stomp.get")["stomps"][0];
  EXPECT_TRUE(static_cast<bool>(stomp["momentary"]));
  EXPECT_EQ(stomp["label"].toString(), "Swell");

  rig.press("A");
  EXPECT_TRUE(rig.blockEnabled(delay));
  EXPECT_TRUE(static_cast<bool>(rig.footswitch("A")["held"]));
  EXPECT_TRUE(static_cast<bool>(rig.footswitch("A")["lit"]));
  rig.release("A");
  EXPECT_FALSE(rig.blockEnabled(delay));
  EXPECT_FALSE(static_cast<bool>(rig.footswitch("A")["held"]));
  EXPECT_FALSE(static_cast<bool>(rig.footswitch("A")["lit"]));

  // The release gives back what the press took even if the mode changed
  // while the switch was down.
  rig.press("A");
  setMode(rig, "scene");
  rig.release("A");
  EXPECT_FALSE(rig.blockEnabled(delay));
  EXPECT_EQ(rig.activeScene(), "A");
  setMode(rig, "stomp");

  // Latching again: the release does nothing.
  rig.must("stomp.setOptions", R"({"switch":"A","momentary":false})");
  rig.press("A");
  rig.release("A");
  EXPECT_TRUE(rig.blockEnabled(delay));
  EXPECT_EQ(rig.must("stomp.get")["stomps"][0]["label"].toString(), "Swell");
}

TEST(FootswitchTest, SceneModeSelectsScenes) {
  SceneRig rig;
  const juce::String gain = rig.addKind("utility.gain");
  rig.settle();
  rig.useScenes(8);
  setMode(rig, "scene");
  rig.must("scene.rename", R"({"scene":"C","name":"Solo"})");

  rig.press("C");
  EXPECT_EQ(rig.activeScene(), "C");
  rig.release("C");
  EXPECT_EQ(rig.activeScene(), "C") << "a release is not a second press";
  const juce::var c = rig.footswitch("C");
  EXPECT_EQ(c["role"].toString(), "scene");
  EXPECT_EQ(c["scene"].toString(), "C");
  EXPECT_EQ(c["label"].toString(), "Solo");
  EXPECT_TRUE(static_cast<bool>(c["lit"]));
  EXPECT_FALSE(static_cast<bool>(rig.footswitch("A")["lit"]));
  rig.stomp("H");
  EXPECT_EQ(rig.activeScene(), "H");
  EXPECT_EQ(rig.footswitches()["activeScene"].toString(), "H");
}

TEST(FootswitchTest, PresetModeLoadsPresetsInBanksOfEight) {
  SceneRig rig;
  const juce::String gain = rig.addKind("utility.gain");
  rig.settle();
  const juce::StringArray ids = saveTenPresets(rig, gain);
  setMode(rig, "preset");

  juce::var state = rig.footswitches();
  EXPECT_EQ(state["mode"].toString(), "preset");
  EXPECT_EQ(static_cast<int>(state["bank"]), 0);
  EXPECT_EQ(static_cast<int>(state["bankCount"]), 2);
  for (int i = 0; i < 8; ++i) {
    const juce::var item = state["switches"][i];
    EXPECT_EQ(item["role"].toString(), "preset");
    EXPECT_EQ(item["presetId"].toString(), ids[i]);
    EXPECT_EQ(static_cast<int>(item["presetIndex"]), i);
    EXPECT_FALSE(static_cast<bool>(item["empty"]));
  }
  EXPECT_EQ(rig.footswitch("bankUp")["role"].toString(), "bankUp");
  EXPECT_EQ(rig.footswitch("bankDown")["role"].toString(), "bankDown");

  rig.stomp("C");
  rig.waitLoaded();
  EXPECT_EQ(activePresetId(rig), ids[2]);
  EXPECT_TRUE(static_cast<bool>(rig.footswitch("C")["lit"]));
  EXPECT_FALSE(static_cast<bool>(rig.footswitch("B")["lit"]));
  EXPECT_EQ(rig.footswitch("C")["label"].toString(), "Preset 02");

  // Bank Up: the next eight, of which only two exist.
  rig.stomp("bankUp");
  state = rig.footswitches();
  EXPECT_EQ(static_cast<int>(state["bank"]), 1);
  EXPECT_EQ(state["switches"][0]["presetId"].toString(), ids[8]);
  EXPECT_EQ(state["switches"][1]["presetId"].toString(), ids[9]);
  EXPECT_TRUE(static_cast<bool>(state["switches"][2]["empty"]));
  EXPECT_FALSE(static_cast<bool>(state["switches"][2]["lit"]));
  EXPECT_EQ(activePresetId(rig), ids[2]) << "paging must not load anything";

  rig.stomp("B");
  rig.waitLoaded();
  EXPECT_EQ(activePresetId(rig), ids[9]);
  // A slot past the end of the list does nothing.
  rig.stomp("D");
  EXPECT_EQ(activePresetId(rig), ids[9]);

  // The banks wrap both ways.
  rig.stomp("bankUp");
  EXPECT_EQ(static_cast<int>(rig.footswitches()["bank"]), 0);
  rig.stomp("bankDown");
  EXPECT_EQ(static_cast<int>(rig.footswitches()["bank"]), 1);
  rig.stomp("bankDown");
  EXPECT_EQ(static_cast<int>(rig.footswitches()["bank"]), 0);
  rig.stomp("A");
  rig.waitLoaded();
  EXPECT_EQ(activePresetId(rig), ids[0]);
}

TEST(FootswitchTest, HybridRowsEachDoOneThing) {
  SceneRig rig;
  const juce::String gain = rig.addKind("utility.gain");
  const juce::String delay = rig.addKind("delay.simple");
  rig.settle();
  rig.useScenes(8);
  const juce::StringArray ids = saveTenPresets(rig, gain);
  rig.must("stomp.assign", "{\"switch\":\"F\",\"blockIds\":[" + quoted(delay) + "]}");
  rig.must("stomp.assign", "{\"switch\":\"B\",\"blockIds\":[" + quoted(gain) + "]}");
  setMode(rig, "hybrid");

  // The default: scenes on top, stomps below.
  juce::var state = rig.footswitches();
  EXPECT_EQ(state["hybridRows"]["top"].toString(), "scene");
  EXPECT_EQ(state["hybridRows"]["bottom"].toString(), "stomp");
  const char* letters[] = {"A", "B", "C", "D", "E", "F", "G", "H"};
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(state["switches"][i]["role"].toString(), "scene");
    EXPECT_EQ(state["switches"][i]["scene"].toString(), letters[i]);
    EXPECT_EQ(state["switches"][i + 4]["role"].toString(), "stomp");
  }
  rig.stomp("C");
  EXPECT_EQ(rig.activeScene(), "C");
  rig.stomp("F");  // its own assignment, not switch B's
  EXPECT_FALSE(rig.blockEnabled(delay));
  EXPECT_TRUE(rig.blockEnabled(gain));

  // One scene row: Bank Up / Down flip it between A to D and E to H.
  EXPECT_EQ(rig.footswitch("bankUp")["role"].toString(), "bankUp");
  rig.stomp("bankUp");
  state = rig.footswitches();
  EXPECT_EQ(static_cast<int>(state["scenePage"]), 1);
  for (int i = 0; i < 4; ++i)
    EXPECT_EQ(state["switches"][i]["scene"].toString(), letters[i + 4]);
  rig.stomp("B");
  EXPECT_EQ(rig.activeScene(), "F");
  rig.stomp("bankDown");
  EXPECT_EQ(static_cast<int>(rig.footswitches()["scenePage"]), 0);

  // Two scene rows: all eight, nothing to page, so the bank switches step
  // through the presets.
  setRows(rig, "scene", "scene");
  state = rig.footswitches();
  for (int i = 0; i < 8; ++i)
    EXPECT_EQ(state["switches"][i]["scene"].toString(), letters[i]);
  rig.stomp("G");
  EXPECT_EQ(rig.activeScene(), "G");
  EXPECT_EQ(rig.footswitch("bankUp")["role"].toString(), "presetNext");

  // A preset row shows four presets and the bank moves by four.
  setRows(rig, "preset", "stomp");
  state = rig.footswitches();
  EXPECT_EQ(static_cast<int>(state["bankCount"]), 3);
  for (int i = 0; i < 4; ++i)
    EXPECT_EQ(state["switches"][i]["presetId"].toString(), ids[i]);
  rig.stomp("bankUp");
  rig.stomp("bankUp");
  state = rig.footswitches();
  EXPECT_EQ(static_cast<int>(state["bank"]), 2);
  EXPECT_EQ(state["switches"][1]["presetId"].toString(), ids[9]);
  EXPECT_TRUE(static_cast<bool>(state["switches"][2]["empty"]));
  rig.stomp("B");
  rig.waitLoaded();
  EXPECT_EQ(activePresetId(rig), ids[9]);

  // Two preset rows: eight, as in Preset mode.
  setRows(rig, "preset", "preset");
  state = rig.footswitches();
  EXPECT_EQ(static_cast<int>(state["bankCount"]), 2);
  EXPECT_EQ(state["switches"][7]["presetId"].toString(), ids[7]);
  rig.stomp("H");
  rig.waitLoaded();
  EXPECT_EQ(activePresetId(rig), ids[7]);

  // Stomps on top, scenes below: the bottom row is the scene row.
  setRows(rig, "stomp", "scene");
  state = rig.footswitches();
  EXPECT_EQ(state["switches"][1]["role"].toString(), "stomp");
  EXPECT_EQ(state["switches"][5]["scene"].toString(), "B");
  rig.stomp("F");
  EXPECT_EQ(rig.activeScene(), "B");
}

TEST(FootswitchTest, TheStateSnapshotDescribesEverySwitch) {
  SceneRig rig;
  const juce::String gain = rig.addKind("utility.gain");
  const juce::String delay = rig.addKind("delay.simple");
  rig.settle();
  setMode(rig, "stomp");
  rig.must("stomp.assign",
           "{\"switch\":\"A\",\"blockIds\":[" + quoted(delay) + "," + quoted(gain) + "]}");
  rig.must("stomp.assign", "{\"switch\":\"B\",\"blockIds\":[" + quoted(gain) +
                               R"(],"label":"Boost","momentary":true})");
  rig.setEnabled(gain, false);

  const juce::var state = rig.footswitches();
  EXPECT_EQ(state["mode"].toString(), "stomp");
  EXPECT_EQ(state["activeScene"].toString(), "A");
  EXPECT_DOUBLE_EQ(static_cast<double>(state["bpm"]), 120.0);
  const auto* switches = state["switches"].getArray();
  ASSERT_NE(switches, nullptr);
  ASSERT_EQ(switches->size(), 11);
  const char* ids[] = {"A", "B", "C", "D", "E", "F", "G", "H", "bankUp", "bankDown", "tempo"};
  for (int i = 0; i < 11; ++i) {
    const juce::var& item = switches->getReference(i);
    EXPECT_EQ(item["id"].toString(), ids[i]);
    EXPECT_EQ(static_cast<int>(item["index"]), i);
    for (const char* field : {"role", "label", "colour", "lit", "held", "empty"})
      EXPECT_FALSE(item[juce::Identifier(field)].isVoid()) << ids[i] << " lacks " << field;
  }

  // A stomp: named after its first block unless it has a label, coloured by
  // that block's category, lit when that block is on.
  const juce::var a = rig.footswitch("A");
  EXPECT_EQ(a["role"].toString(), "stomp");
  EXPECT_EQ(a["label"].toString(), rig.blockItem(delay)["tone"]["title"].toString());
  EXPECT_EQ(a["colour"].toString(), "delay");
  EXPECT_TRUE(static_cast<bool>(a["lit"]));
  EXPECT_FALSE(static_cast<bool>(a["empty"]));
  EXPECT_FALSE(static_cast<bool>(a["momentary"]));
  ASSERT_EQ(a["blockIds"].size(), 2);
  EXPECT_EQ(a["blockIds"][0].toString(), delay);
  const juce::var b = rig.footswitch("B");
  EXPECT_EQ(b["label"].toString(), "Boost");
  EXPECT_EQ(b["colour"].toString(), "utility");
  EXPECT_FALSE(static_cast<bool>(b["lit"]));
  EXPECT_TRUE(static_cast<bool>(b["momentary"]));
  const juce::var c = rig.footswitch("C");
  EXPECT_EQ(c["role"].toString(), "stomp");
  EXPECT_TRUE(static_cast<bool>(c["empty"]));
  EXPECT_FALSE(static_cast<bool>(c["lit"]));

  // The three others. With nothing to page, the bank switches walk the
  // preset list.
  EXPECT_EQ(rig.footswitch("bankUp")["role"].toString(), "presetNext");
  EXPECT_EQ(rig.footswitch("bankDown")["role"].toString(), "presetPrevious");
  EXPECT_EQ(rig.footswitch("tempo")["role"].toString(), "tapTempo");

  // Held follows press and release for every switch.
  rig.press("bankDown");
  EXPECT_TRUE(static_cast<bool>(rig.footswitch("bankDown")["held"]));
  rig.release("bankDown");
  EXPECT_FALSE(static_cast<bool>(rig.footswitch("bankDown")["held"]));

  // Scene mode: the same switches are scenes, lit on the active one.
  setMode(rig, "scene");
  EXPECT_EQ(rig.footswitch("A")["role"].toString(), "scene");
  EXPECT_EQ(rig.footswitch("A")["colour"].toString(), "scene");
  EXPECT_TRUE(static_cast<bool>(rig.footswitch("A")["lit"]));
  EXPECT_EQ(rig.footswitch("D")["label"].toString(), "Scene D");

  // In those two modes Bank Up / Down step through the presets.
  const juce::StringArray presets = saveTenPresets(rig, gain);
  rig.must("preset.load", "{\"id\":" + quoted(presets[3]) + "}");
  rig.waitLoaded();
  rig.stomp("bankUp");
  rig.waitLoaded();
  EXPECT_EQ(activePresetId(rig), presets[4]);
  rig.stomp("bankDown");
  rig.stomp("bankDown");
  rig.waitLoaded();
  EXPECT_EQ(activePresetId(rig), presets[2]);

  EXPECT_EQ(rig.run("footswitch.press", R"({"switch":"Z"})").errorCode,
            juce::String(CommandError::invalidArgs));
  EXPECT_EQ(rig.run("footswitch.setMode", R"({"mode":"looper"})").errorCode,
            juce::String(CommandError::invalidArgs));
}

TEST(TapTempoTest, AveragesTheLastIntervalsAndIgnoresOutliers) {
  TapTempo taps;
  double bpm = 0.0;

  // The first tap only starts the count; each one after gives a tempo.
  EXPECT_FALSE(taps.tap(10.0, bpm));
  EXPECT_TRUE(taps.tap(10.5, bpm));
  EXPECT_NEAR(bpm, 120.0, 1e-9);
  EXPECT_TRUE(taps.tap(11.0, bpm));
  EXPECT_NEAR(bpm, 120.0, 1e-9);

  // An average, not the last interval: 0.5, 0.5, 0.52, 0.48.
  EXPECT_TRUE(taps.tap(11.52, bpm));
  EXPECT_NEAR(bpm, 60.0 / ((0.5 + 0.5 + 0.52) / 3.0), 1e-9);
  EXPECT_TRUE(taps.tap(12.0, bpm));
  EXPECT_NEAR(bpm, 120.0, 1e-9);
  // Only the last four count: 0.5, 0.52, 0.48, 0.6.
  EXPECT_TRUE(taps.tap(12.6, bpm));
  EXPECT_NEAR(bpm, 60.0 / ((0.5 + 0.52 + 0.48 + 0.6) / 4.0), 1e-9);

  // A missed tap (twice the interval) is an outlier: left out, tempo kept.
  taps.reset();
  taps.tap(0.0, bpm);
  taps.tap(0.5, bpm);
  taps.tap(1.0, bpm);
  double kept = bpm;
  EXPECT_FALSE(taps.tap(2.0, bpm));
  EXPECT_DOUBLE_EQ(bpm, kept);
  // The count carries on from the late tap.
  EXPECT_TRUE(taps.tap(2.5, bpm));
  EXPECT_NEAR(bpm, 120.0, 1e-9);

  // Two outliers in a row that agree are a new tempo.
  EXPECT_FALSE(taps.tap(2.8, bpm));
  EXPECT_TRUE(taps.tap(3.1, bpm));
  EXPECT_NEAR(bpm, 200.0, 1e-6);

  // A bounce (closer than the fastest tempo allows) is dropped.
  kept = bpm;
  EXPECT_FALSE(taps.tap(3.15, bpm));
  EXPECT_TRUE(taps.tap(3.4, bpm));
  EXPECT_NEAR(bpm, 200.0, 1e-6);

  // A pause longer than the slowest tempo starts a new count.
  EXPECT_FALSE(taps.tap(20.0, bpm));
  EXPECT_TRUE(taps.tap(21.0, bpm));
  EXPECT_NEAR(bpm, 60.0, 1e-9);

  // The ends of the range: 20 and 300 BPM exactly.
  taps.reset();
  taps.tap(0.0, bpm);
  EXPECT_TRUE(taps.tap(3.0, bpm));
  EXPECT_NEAR(bpm, 20.0, 1e-9);
  taps.reset();
  taps.tap(0.0, bpm);
  EXPECT_TRUE(taps.tap(0.2, bpm));
  EXPECT_NEAR(bpm, 300.0, 1e-9);
}

TEST(TapTempoTest, TheCommandAndTheTempoSwitchSetTheRigsTempo) {
  SceneRig rig;
  EXPECT_DOUBLE_EQ(static_cast<double>(rig.must("tempo.get")["bpm"]), 120.0);

  // Explicit times: 400 ms apart is 150 BPM.
  juce::var result = rig.must("tempo.tap", R"({"timeMs":1000})");
  EXPECT_FALSE(static_cast<bool>(result["tapped"]));
  EXPECT_DOUBLE_EQ(static_cast<double>(result["bpm"]), 120.0);
  result = rig.must("tempo.tap", R"({"timeMs":1400})");
  EXPECT_TRUE(static_cast<bool>(result["tapped"]));
  EXPECT_NEAR(static_cast<double>(result["bpm"]), 150.0, 1e-6);
  rig.must("tempo.tap", R"({"timeMs":1800})");
  rig.must("tempo.tap", R"({"timeMs":2200})");
  EXPECT_NEAR(static_cast<double>(rig.must("tempo.get")["bpm"]), 150.0, 1e-6);
  EXPECT_NEAR(static_cast<double>(rig.footswitches()["bpm"]), 150.0, 1e-6);

  // The tempo switch, on the wall clock: three presses about 300 ms apart.
  // A busy machine can oversleep, so the tempo is checked against the gaps
  // the presses really had, not against the 300 ms asked for.
  SceneRig second;
  double firstPress = 0.0, lastPress = 0.0;
  for (int i = 0; i < 3; ++i) {
    if (i > 0)
      juce::Thread::sleep(300);
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (i == 0)
      firstPress = now;
    lastPress = now;
    second.press("tempo");
    second.release("tempo");
  }
  const double expected = 60000.0 / ((lastPress - firstPress) / 2.0);
  const double tapped = static_cast<double>(second.must("tempo.get")["bpm"]);
  EXPECT_NEAR(tapped, expected, expected * 0.05);
}

TEST(FootswitchMidiTest, TheFixedControlsWorkThroughProcessBlock) {
  SceneRig rig;
  const juce::String gain = rig.addKind("utility.gain");
  rig.settle();
  rig.useScenes(8);

  // midi.getMap documents them.
  const juce::var defaults = rig.must("midi.getMap")["defaults"];
  // The four above and the looper's fourteen (CC 48 to 61, looper_tests.cpp).
  ASSERT_EQ(defaults.size(), 18);
  const std::pair<const char*, int> expected[] = {
      {"sceneSelect", 43}, {"tapTempo", 44}, {"tuner", 45}, {"footswitchMode", 47}};
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(defaults[i]["action"].toString(), expected[i].first);
    EXPECT_EQ(static_cast<int>(defaults[i]["cc"]), expected[i].second);
    EXPECT_FALSE(static_cast<bool>(defaults[i]["overridden"]));
  }

  // CC 43: value 0 to 7 selects scene A to H; anything else is ignored.
  sendCc(rig, 43, 3);
  EXPECT_EQ(rig.activeScene(), "A") << "the audio thread must hand the switch over, not make it";
  pumpMessages();
  EXPECT_EQ(rig.activeScene(), "D");
  sendCc(rig, 43, 7);
  pumpMessages();
  EXPECT_EQ(rig.activeScene(), "H");
  sendCc(rig, 43, 8);
  sendCc(rig, 43, 127);
  pumpMessages();
  EXPECT_EQ(rig.activeScene(), "H");
  // Several before the message thread gets to them: the last one wins.
  sendCc(rig, 43, 1);
  sendCc(rig, 43, 2);
  pumpMessages();
  EXPECT_EQ(rig.activeScene(), "C");

  // CC 47: value 0 to 3 sets the mode.
  const char* modes[] = {"preset", "scene", "stomp", "hybrid"};
  for (const int value : {3, 0, 1, 2}) {
    sendCc(rig, 47, value);
    pumpMessages();
    EXPECT_EQ(rig.footswitches()["mode"].toString(), modes[value]);
  }
  sendCc(rig, 47, 4);
  pumpMessages();
  EXPECT_EQ(rig.footswitches()["mode"].toString(), "stomp");

  // CC 45: the tuner, on at 64 and above.
  EXPECT_FALSE(rig.proc.isTunerEnabled());
  sendCc(rig, 45, 127);
  pumpMessages();
  EXPECT_TRUE(rig.proc.isTunerEnabled());
  sendCc(rig, 45, 0);
  pumpMessages();
  EXPECT_FALSE(rig.proc.isTunerEnabled());

  // CC 44: tap tempo. A footswitch sends 127 down and 0 up; only the press
  // is a tap. Three taps about 300 ms apart, checked against the gaps they
  // really had: a busy machine can oversleep.
  double firstTap = 0.0, lastTap = 0.0;
  for (int i = 0; i < 3; ++i) {
    if (i > 0)
      juce::Thread::sleep(300);
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (i == 0)
      firstTap = now;
    lastTap = now;
    sendCc(rig, 44, 127);
    sendCc(rig, 44, 0);
  }
  pumpMessages();
  const double expectedBpm = 60000.0 / ((lastTap - firstTap) / 2.0);
  const double tapped = static_cast<double>(rig.must("tempo.get")["bpm"]);
  EXPECT_NEAR(tapped, expectedBpm, expectedBpm * 0.05);

  // The reserved number does nothing: gig view. (CC 48 to 61 are the looper's
  // and act on the looper, not on the footswitches or scenes.)
  const juce::String before = juce::JSON::toString(rig.footswitches());
  for (const int cc : {46, 48, 55, 61}) {
    sendCc(rig, cc, 127);
    sendCc(rig, cc, 0);
  }
  pumpMessages();
  EXPECT_EQ(juce::JSON::toString(rig.footswitches()), before);
  EXPECT_EQ(rig.activeScene(), "C");

  // The channel filter gates them like everything else.
  rig.must("midi.setChannelFilter", R"({"channel":5})");
  sendCc(rig, 43, 0, /*channel=*/1);
  pumpMessages();
  EXPECT_EQ(rig.activeScene(), "C");
  sendCc(rig, 43, 0, /*channel=*/5);
  pumpMessages();
  EXPECT_EQ(rig.activeScene(), "A");
  rig.must("midi.setChannelFilter", R"({"channel":0})");

  // A mapping of the user's own takes the number over: CC 43 now drives the
  // gate switch and no longer selects scenes.
  rig.must("midi.setCc", R"({"targetId":"gateEnabled","cc":43})");
  EXPECT_TRUE(static_cast<bool>(rig.must("midi.getMap")["defaults"][0]["overridden"]));
  const float gateBefore = rig.proc.parameters.getRawParameterValue("gateEnabled")->load();
  sendCc(rig, 43, 5);
  pumpMessages();
  EXPECT_EQ(rig.activeScene(), "A");
  EXPECT_NE(rig.proc.parameters.getRawParameterValue("gateEnabled")->load(), gateBefore);
  // Removing the mapping gives the default back.
  rig.must("midi.remove", R"({"targetId":"gateEnabled"})");
  sendCc(rig, 43, 5);
  pumpMessages();
  EXPECT_EQ(rig.activeScene(), "F");

  // And the defaults can be moved or switched off.
  rig.proc.midiMapper.setFixedControlCc(MidiMapper::FixedControl::sceneSelect, 20);
  sendCc(rig, 43, 1);
  pumpMessages();
  EXPECT_EQ(rig.activeScene(), "F");
  sendCc(rig, 20, 1);
  pumpMessages();
  EXPECT_EQ(rig.activeScene(), "B");
  rig.proc.midiMapper.setFixedControlCc(MidiMapper::FixedControl::sceneSelect, -1);
  sendCc(rig, 20, 2);
  pumpMessages();
  EXPECT_EQ(rig.activeScene(), "B");
}

TEST(FootswitchTest, PressesFromAnotherThreadAreDeliveredOnTheMessageThread) {
  SceneRig rig;
  const juce::String delay = rig.addKind("delay.simple");
  rig.settle();
  rig.useScenes(8);
  rig.must("stomp.assign", "{\"switch\":\"E\",\"blockIds\":[" + quoted(delay) + "]}");
  rig.must("stomp.setOptions", R"({"switch":"E","momentary":true})");
  setMode(rig, "scene");

  std::thread other([&] {
    rig.proc.footswitchPress(2);  // C
    rig.proc.footswitchRelease(2);
  });
  other.join();
  EXPECT_EQ(rig.activeScene(), "A") << "another thread's press must be handed over";
  pumpMessages();
  EXPECT_EQ(rig.activeScene(), "C");

  // Order is kept: a press and its release from elsewhere leave a momentary
  // stomp where it started, with one switch of the block in between.
  setMode(rig, "stomp");
  const int revisionBefore = static_cast<int>(rig.must("state.revision")["revision"]);
  std::thread another([&] {
    for (int i = 0; i < 5; ++i) {
      rig.proc.footswitchPress(4);  // E
      rig.proc.footswitchRelease(4);
    }
    rig.proc.footswitchPress(4);
  });
  another.join();
  pumpMessages();
  EXPECT_FALSE(rig.blockEnabled(delay)) << "held: the momentary stomp has its block flipped";
  EXPECT_TRUE(static_cast<bool>(rig.footswitch("E")["held"]));
  EXPECT_GT(static_cast<int>(rig.must("state.revision")["revision"]), revisionBefore);
  std::thread last([&] { rig.proc.footswitchRelease(4); });
  last.join();
  pumpMessages();
  EXPECT_TRUE(rig.blockEnabled(delay));

  // Out-of-range switches are ignored on any thread.
  rig.proc.footswitchPress(-1);
  rig.proc.footswitchPress(11);
  pumpMessages();
}

TEST(FootswitchTest, TheModePersistsInARedirectedSettingsFile) {
  const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                             .getChildFile("t3k-footswitch-settings-" + juce::Uuid().toString());
  ASSERT_TRUE(dir.createDirectory().wasOk());
  const juce::File file = dir.getChildFile("preferences.settings");
  const juce::File previous = TONE3000Processor::setFootswitchSettingsFileForTesting(file);
  // Whatever happens below, the suite's own scratch file is put back.
  struct Restore {
    juce::File previous, dir;
    ~Restore() {
      TONE3000Processor::setFootswitchSettingsFileForTesting(previous);
      dir.deleteRecursively();
    }
  } restore{previous, dir};
  EXPECT_NE(file, TONE3000Processor::getSettingsFile())
      << "the test would be writing the user's real settings";

  {
    // Nothing saved yet: the defaults.
    TONE3000Processor fresh;
    EXPECT_EQ(fresh.getFootswitchMode(), FootswitchMode::Stomp);
    const juce::var state = fresh.getFootswitchState();
    EXPECT_EQ(state["hybridRows"]["top"].toString(), "scene");
    EXPECT_EQ(state["hybridRows"]["bottom"].toString(), "stomp");
    EXPECT_FALSE(file.existsAsFile()) << "reading the settings must not write them";

    EngineCommands commands(fresh);
    juce::var args;
    ASSERT_TRUE(juce::JSON::parse(R"({"mode":"hybrid"})", args).wasOk());
    ASSERT_TRUE(commands.registry().run("footswitch.setMode", args).ok);
    ASSERT_TRUE(juce::JSON::parse(R"({"top":"preset","bottom":"scene"})", args).wasOk());
    ASSERT_TRUE(commands.registry().run("footswitch.setHybridRows", args).ok);
  }
  ASSERT_TRUE(file.existsAsFile());
  const juce::String written = file.loadFileAsString();
  EXPECT_TRUE(written.contains("footswitchMode"));
  EXPECT_TRUE(written.contains("hybrid"));

  {
    // The next processor starts where the last one left the board: the mode
    // is global, it is in no rig and no preset.
    TONE3000Processor next;
    EXPECT_EQ(next.getFootswitchMode(), FootswitchMode::Hybrid);
    const juce::var state = next.getFootswitchState();
    EXPECT_EQ(state["mode"].toString(), "hybrid");
    EXPECT_EQ(state["hybridRows"]["top"].toString(), "preset");
    EXPECT_EQ(state["hybridRows"]["bottom"].toString(), "scene");

    // Not part of the saved state: a rig saved in one mode does not carry
    // it into a processor that is in another.
    juce::MemoryBlock blob;
    next.getStateInformation(blob);
    next.setFootswitchMode(FootswitchMode::Scene, /*persist=*/false);
    next.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
    EXPECT_EQ(next.getFootswitchMode(), FootswitchMode::Scene);
  }
  {
    // persist = false left the file alone.
    TONE3000Processor again;
    EXPECT_EQ(again.getFootswitchMode(), FootswitchMode::Hybrid);
  }
}

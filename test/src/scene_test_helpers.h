#pragma once
// The rig scene_tests.cpp, footswitch_tests.cpp and scene_render_tests.cpp
// share: the real processor with its presets in a temp store, driven through
// the command layer, plus the few readers the scene payloads need.

#include "EngineCommands.h"
#include "Processor.h"
#include "chain_test_helpers.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace scenetest {

inline juce::String quoted(const juce::String& text) { return juce::JSON::toString(juce::var(text)); }

struct SceneRig {
  explicit SceneRig(double sampleRate = 48000.0, int blockSize = 256)
      : rate(sampleRate), block(blockSize),
        presetDir(juce::File::getSpecialLocation(juce::File::tempDirectory)
                      .getChildFile("t3k-scene-tests-" + juce::Uuid().toString())) {
    EXPECT_TRUE(presetDir.createDirectory().wasOk());
    proc.setPresetStoreForTesting(presetDir);
    proc.setMultiCoreEnabled(false, /*persist=*/false);
    // Never persisted: the mode the previous test left must not leak in.
    proc.setFootswitchMode(FootswitchMode::Stomp, /*persist=*/false);
    proc.setFootswitchHybridRows(FootswitchRowRole::Scene, FootswitchRowRole::Stomp,
                                 /*persist=*/false);
    proc.setPlayConfigDetails(2, 2, rate, block);
    proc.prepareToPlay(rate, block);
  }
  ~SceneRig() { presetDir.deleteRecursively(); }

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

  // Adds a block of a registered kind; returns its id.
  juce::String addKind(const char* kindId) {
    const juce::String id =
        must("chain.addBlock", "{\"kind\":" + quoted(kindId) + "}")["blockId"].toString();
    EXPECT_TRUE(id.isNotEmpty());
    return id;
  }

  void waitLoaded() {
    const juce::var result = must("state.waitUntilLoaded", R"({"timeoutMs":30000})");
    EXPECT_TRUE(static_cast<bool>(result["loaded"]));
  }

  // Like a host (re)starting transport with the rig in place: every fade
  // and smoother starts settled.
  void settle() {
    waitLoaded();
    proc.prepareToPlay(rate, block);
    waitLoaded();
  }

  void setParam(const juce::String& blockId, const char* param, double value) {
    must("block.setParam", "{\"blockId\":" + quoted(blockId) + ",\"param\":" + quoted(param) +
                               ",\"value\":" + juce::String(value, 9) + "}");
  }
  void setEnabled(const juce::String& blockId, bool enabled) {
    must("block.setEnabled", "{\"blockId\":" + quoted(blockId) + ",\"enabled\":" +
                                 (enabled ? "true" : "false") + "}");
  }
  void assign(const juce::String& blockId, const char* param) {
    must("scene.assignParam", "{\"blockId\":" + quoted(blockId) + ",\"param\":" + quoted(param) + "}");
  }
  // Grows the rig to `count` scenes (a rig starts with one).
  void useScenes(int count) {
    while (static_cast<int>(scenes()["count"]) < count)
      must("scene.add");
  }
  void select(const char* scene) { must("scene.select", "{\"scene\":" + quoted(scene) + "}"); }
  void setSceneValue(const char* scene, const juce::String& blockId, const char* param,
                     double value) {
    must("scene.setParamValue", "{\"scene\":" + quoted(scene) + ",\"blockId\":" + quoted(blockId) +
                                    ",\"param\":" + quoted(param) + ",\"value\":" +
                                    juce::String(value, 9) + "}");
  }

  // A block's entry in the chain state, from either lane; void when absent.
  juce::var blockItem(const juce::String& blockId) {
    const juce::var chain = proc.getChainState(-1);
    for (const char* key : {"chain", "chainRight"})
      if (const auto* lane = chain[key].getArray())
        for (const auto& item : *lane)
          if (item["blockId"].toString() == blockId)
            return item;
    return {};
  }
  bool blockEnabled(const juce::String& blockId) {
    return static_cast<bool>(blockItem(blockId)["params"]["enabled"]);
  }
  // The block's current value of a common setting or a kind parameter.
  double blockValue(const juce::String& blockId, const char* param) {
    const juce::var item = blockItem(blockId);
    const juce::var common = item["params"][juce::Identifier(param)];
    return static_cast<double>(common.isVoid() ? item["kindParams"][juce::Identifier(param)]
                                               : common);
  }

  juce::var scenes() { return must("scene.get"); }
  juce::String activeScene() { return scenes()["activeId"].toString(); }
  // One scene of scene.get, by letter.
  juce::var scene(const char* letter) {
    const juce::var all = scenes();
    if (const auto* list = all["scenes"].getArray())
      for (const auto& entry : *list)
        if (entry["id"].toString() == letter)
          return entry;
    return {};
  }
  bool sceneBlock(const char* letter, const juce::String& blockId) {
    const juce::var state = scene(letter)["blocks"][juce::Identifier(blockId)];
    EXPECT_FALSE(state.isVoid()) << "scene " << letter << " does not list block " << blockId;
    return static_cast<bool>(state);
  }
  // NaN when the scene holds no value for the parameter (not assigned).
  double sceneValue(const char* letter, const juce::String& blockId, const char* param) {
    const juce::var held = scene(letter);
    if (const auto* values = held["values"].getArray())
      for (const auto& entry : *values)
        if (entry["blockId"].toString() == blockId && entry["param"].toString() == param)
          return static_cast<double>(entry["value"]);
    return std::nan("");
  }
  bool isAssigned(const juce::String& blockId, const char* param) {
    return !std::isnan(sceneValue("A", blockId, param));
  }

  // The footswitch snapshot, and one switch of it by id ("A", "bankUp").
  juce::var footswitches() { return must("footswitch.getState"); }
  juce::var footswitch(const char* id) {
    const juce::var all = footswitches();
    if (const auto* list = all["switches"].getArray())
      for (const auto& entry : *list)
        if (entry["id"].toString() == id)
          return entry;
    return {};
  }
  void press(const char* id) { must("footswitch.press", "{\"switch\":" + quoted(id) + "}"); }
  void release(const char* id) { must("footswitch.release", "{\"switch\":" + quoted(id) + "}"); }
  void stomp(const char* id) {
    press(id);
    release(id);
  }

  // Runs `in` through the rig on both channels, in whole blocks; returns the
  // left output.
  std::vector<float> render(const std::vector<float>& in) {
    return processStereo(proc, in, block).first;
  }

  const double rate;
  const int block;
  juce::File presetDir;
  ChainTestProcessor proc;
  EngineCommands commands{proc};
};

inline double rmsOf(const std::vector<float>& x, size_t begin, size_t end) {
  end = std::min(end, x.size());
  if (begin >= end)
    return 0.0;
  double sum = 0.0;
  for (size_t i = begin; i < end; ++i)
    sum += static_cast<double>(x[i]) * static_cast<double>(x[i]);
  return std::sqrt(sum / static_cast<double>(end - begin));
}

// Largest jump between neighbouring samples: what a click is made of.
inline float largestStep(const std::vector<float>& x) {
  float largest = 0.0f;
  for (size_t i = 1; i < x.size(); ++i)
    largest = std::max(largest, std::abs(x[i] - x[i - 1]));
  return largest;
}

inline bool allFinite(const std::vector<float>& x) {
  return std::all_of(x.begin(), x.end(), [](float v) { return std::isfinite(v); });
}

}  // namespace scenetest

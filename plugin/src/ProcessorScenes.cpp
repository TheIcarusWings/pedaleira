#include "Processor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

// #############################
// SCENES, STOMPS, FOOTSWITCHES
// #############################
//
// The data model is in Scenes.h. What matters here is who does what:
//
//  - The blocks always carry the active scene. A scene switch is therefore a
//    handful of plain stores made under one short hold of chainMutex on the
//    message thread: each block's `enabled`, its gains and mix, and its
//    processor's lock-free parameter sets. The audio thread never sees a
//    scene; it sees the same values a power click and a knob move give it,
//    and glides the way it always has (wet fade, spillover release, gain and
//    mix smoothers, the kind's own smoothing).
//  - Everything else (assignments, names, stomps) is message-thread
//    bookkeeping under the same lock, recorded in the undo history through
//    the chain snapshot, which carries the Scenes and Stomps nodes.
//  - Footswitch events may start on any thread. Off the message thread they
//    are written to a wait-free ring and delivered by the processor's
//    AsyncUpdater, the way the MIDI mapper hands over its own.

namespace {

constexpr auto kFootswitchModeKey = "footswitchMode";
constexpr auto kHybridTopKey = "footswitchHybridTop";
constexpr auto kHybridBottomKey = "footswitchHybridBottom";

constexpr FootswitchMode kDefaultFootswitchMode = FootswitchMode::Stomp;
constexpr FootswitchRowRole kDefaultHybridTop = FootswitchRowRole::Scene;
constexpr FootswitchRowRole kDefaultHybridBottom = FootswitchRowRole::Stomp;

std::mutex& settingsOverrideMutex() {
  static std::mutex mutex;
  return mutex;
}

juce::File& settingsOverrideFile() {
  static juce::File file;
  return file;
}

// The shared machine-wide settings file (or the test override), opened for
// one read or one write and closed again: two PropertiesFile objects kept
// open on one file overwrite each other's keys.
std::unique_ptr<juce::PropertiesFile> openFootswitchSettings() {
  juce::File file;
  {
    const std::lock_guard<std::mutex> lock(settingsOverrideMutex());
    file = settingsOverrideFile();
  }
  if (file == juce::File())
    file = TONE3000Processor::getSettingsFile();
  return std::make_unique<juce::PropertiesFile>(file, juce::PropertiesFile::Options());
}

juce::var keyVar(const SceneParamKey& key) {
  juce::DynamicObject::Ptr obj = new juce::DynamicObject();
  obj->setProperty("blockId", juce::String(key.blockId));
  obj->setProperty("param", key.paramId);
  return juce::var(obj.get());
}

// Float values travel as doubles; rounding to the float's own precision
// keeps 0.3f from reading 0.30000001192.
double tidy(float value) { return juce::String(value, 6).getDoubleValue(); }

}  // namespace

juce::File TONE3000Processor::setFootswitchSettingsFileForTesting(const juce::File& file) {
  const std::lock_guard<std::mutex> lock(settingsOverrideMutex());
  return std::exchange(settingsOverrideFile(), file);
}

FootswitchMode TONE3000Processor::readPersistedFootswitchMode() {
  return footswitchModeFromString(openFootswitchSettings()->getValue(kFootswitchModeKey),
                                  kDefaultFootswitchMode);
}

FootswitchRowRole TONE3000Processor::readPersistedHybridRow(bool top) {
  return footswitchRowRoleFromString(
      openFootswitchSettings()->getValue(top ? kHybridTopKey : kHybridBottomKey),
      top ? kDefaultHybridTop : kDefaultHybridBottom);
}

void TONE3000Processor::persistFootswitchSettings() const {
  const auto settings = openFootswitchSettings();
  settings->setValue(kFootswitchModeKey, footswitchModeToString(getFootswitchMode()));
  settings->setValue(kHybridTopKey, footswitchRowRoleToString(
                                        static_cast<FootswitchRowRole>(hybridTopRow.load())));
  settings->setValue(kHybridBottomKey, footswitchRowRoleToString(static_cast<FootswitchRowRole>(
                                           hybridBottomRow.load())));
  if (!settings->saveIfNeeded())
    juce::Logger::writeToLog("[Processor] Couldn't write the settings file: " +
                             settings->getFile().getFullPathName());
}

// ####################
// SCENE PARAMETERS
// ####################
bool TONE3000Processor::isCommonSceneParam(const juce::String& param) {
  return param == "inputGain" || param == "outputGain" || param == "mix";
}

bool TONE3000Processor::readSceneParam(const ChainBlock& block, const juce::String& param,
                                       float& value) {
  if (param == "inputGain") {
    value = block.inputGainNormalized;
    return true;
  }
  if (param == "outputGain") {
    value = block.outputGainNormalized;
    return true;
  }
  if (param == "mix") {
    value = block.mixNormalized;
    return true;
  }
  if (block.processor == nullptr)
    return false;
  const int index = block.processor->findParameter(param);
  if (index < 0)
    return false;
  value = block.processor->getParameter(index);
  return true;
}

bool TONE3000Processor::writeSceneParam(ChainBlock& block, const juce::String& param, float value,
                                        bool* affectsTail) {
  // The audio thread reads the three common ones under chainMutex and rides
  // its smoothers to them, exactly as after setBlockParam.
  if (param == "inputGain") {
    block.inputGainNormalized = juce::jlimit(0.0f, 1.0f, value);
    return true;
  }
  if (param == "outputGain") {
    block.outputGainNormalized = juce::jlimit(0.0f, 1.0f, value);
    return true;
  }
  if (param == "mix") {
    block.mixNormalized = juce::jlimit(0.0f, 1.0f, value);
    return true;
  }
  if (block.processor == nullptr)
    return false;
  const int index = block.processor->findParameter(param);
  if (index < 0)
    return false;
  // The lock-free store a knob move makes; the kind clamps and smooths.
  block.processor->setParameter(index, value);
  if (affectsTail != nullptr &&
      block.processor->parameterTable()[static_cast<size_t>(index)].affectsTail)
    *affectsTail = true;
  return true;
}

juce::String TONE3000Processor::sceneParamRefusal(const ChainBlock& block,
                                                  const juce::String& param) {
  if (param == "enabled")
    return "a block's on/off state is part of every scene already";
  if (param == "normalize")
    return "normalize is a setting of the block, not a scene parameter";
  if (isCommonSceneParam(param))
    return {};
  if (block.isMissingKind())
    return "the block is a placeholder for a kind that is not available";
  if (block.processor == nullptr)
    return "the block is still loading";
  const int index = block.processor->findParameter(param);
  if (index < 0)
    return "the block has no parameter '" + param + "' (see block.listParams)";
  // A latency change is spliced in under the chain-edit fade
  // (flushDeferredBlockRefresh), which is exactly what a scene switch must
  // never need.
  if (block.processor->parameterTable()[static_cast<size_t>(index)].affectsLatency)
    return "'" + param + "' changes the block's latency, which mutes the chain";
  return {};
}

void TONE3000Processor::autoAssignSceneParam(const ChainBlock& block, const juce::String& param) {
  // One scene in use: values stay shared.
  if (scenes.count <= 1)
    return;
  const SceneParamKey key{block.id, param};
  if (scenes.isAssigned(key) || scenes.isExpression(key) ||
      sceneParamRefusal(block, param).isNotEmpty())
    return;
  float value = 0.0f;
  if (readSceneParam(block, param, value))
    scenes.assigned[key].fill(value);
}

void TONE3000Processor::registerSceneBlock(const ChainBlock& block) {
  if (scenes.blockStates.count(block.id) == 0)
    scenes.blockStates[block.id].fill(block.enabled);
}

void TONE3000Processor::captureActiveScene() {
  const auto slot = static_cast<size_t>(scenes.active);
  for (auto& [blockId, states] : scenes.blockStates)
    if (const ChainBlock* block = findBlockById(blockId))
      states[slot] = block->enabled;
  for (auto& [key, values] : scenes.assigned) {
    float value = 0.0f;
    // A block that cannot answer (its kind is still being built) keeps the
    // value the scene already has.
    if (const ChainBlock* block = findBlockById(key.blockId))
      if (readSceneParam(*block, key.paramId, value))
        values[slot] = value;
  }
}

SceneStore TONE3000Processor::scenesWithLiveValues() const {
  SceneStore current = scenes;
  const auto slot = static_cast<size_t>(current.active);

  std::map<std::string, const ChainBlock*> live;
  for (const auto& l : lanes)
    for (const auto& block : l)
      if (block->type != ChainBlockType::INSERT)
        live[block->id] = block.get();
  const auto gone = [&live](const std::string& blockId) { return live.count(blockId) == 0; };

  std::erase_if(current.blockStates, [&](const auto& entry) { return gone(entry.first); });
  std::erase_if(current.assigned, [&](const auto& entry) { return gone(entry.first.blockId); });
  std::erase_if(current.expression, [&](const SceneParamKey& key) { return gone(key.blockId); });

  for (auto& [blockId, states] : current.blockStates)
    states[slot] = live[blockId]->enabled;
  for (auto& [key, values] : current.assigned) {
    float value = 0.0f;
    if (readSceneParam(*live[key.blockId], key.paramId, value))
      values[slot] = value;
  }
  return current;
}

bool TONE3000Processor::applySceneToBlocks(int index) {
  const auto slot = static_cast<size_t>(index);
  bool tailMoved = false;
  for (const auto& [blockId, states] : scenes.blockStates)
    if (ChainBlock* block = findBlockById(blockId))
      block->enabled = states[slot];
  for (const auto& [key, values] : scenes.assigned)
    if (ChainBlock* block = findBlockById(key.blockId))
      writeSceneParam(*block, key.paramId, values[slot], &tailMoved);
  return tailMoved;
}

// ####################
// SCENES
// ####################
int TONE3000Processor::getActiveScene() const {
  juce::ScopedLock lock(chainMutex);
  return scenes.active;
}

bool TONE3000Processor::selectScene(int index) {
  if (index < 0 || index >= kNumScenes)
    return false;

  juce::ScopedLock lock(chainMutex);
  if (!scenes.inUse(index))
    return false;
  if (index == scenes.active)
    return true;

  // What the blocks carry is the scene being left: keep it.
  captureActiveScene();
  scenes.active = index;
  // A knob gesture that straddles the switch is two edits, one per scene.
  chainHistory.endGesture();
  // A tail that follows a parameter is re-read here rather than on the
  // gesture timer: this is one edit, not a drag, and the lock is held.
  if (applySceneToBlocks(index))
    refreshBlockTailLength();
  bumpChainRevision();
  return true;
}

int TONE3000Processor::getSceneCount() const {
  juce::ScopedLock lock(chainMutex);
  return scenes.count;
}

juce::Result TONE3000Processor::addScene(int* newIndex) {
  juce::ScopedLock lock(chainMutex);
  if (scenes.count >= kNumScenes)
    return juce::Result::fail("a preset has at most " + juce::String(kNumScenes) +
                              " scenes, and all are in use");
  pushChainHistory();
  captureActiveScene();
  const auto slot = static_cast<size_t>(scenes.count);
  const auto activeSlot = static_cast<size_t>(scenes.active);
  scenes.names[slot].clear();
  for (auto& entry : scenes.blockStates)
    entry.second[slot] = entry.second[activeSlot];
  for (auto& entry : scenes.assigned)
    entry.second[slot] = entry.second[activeSlot];
  if (newIndex != nullptr)
    *newIndex = scenes.count;
  ++scenes.count;
  bumpChainRevision();
  return juce::Result::ok();
}

juce::Result TONE3000Processor::removeScene(int index) {
  juce::ScopedLock lock(chainMutex);
  if (!scenes.inUse(index))
    return juce::Result::fail("scene " + sceneLetter(index) + " is not in use");
  if (scenes.count <= 1)
    return juce::Result::fail("the only scene cannot be removed");
  pushChainHistory();
  captureActiveScene();

  const int last = scenes.count - 1;
  const int oldActive = scenes.active;
  // Later scenes move down one place; the last place goes back to a fresh
  // scene (default name, every block on, values as the active scene has).
  for (int i = index; i < last; ++i)
    scenes.names[static_cast<size_t>(i)] = scenes.names[static_cast<size_t>(i + 1)];
  scenes.names[static_cast<size_t>(last)].clear();
  for (auto& entry : scenes.blockStates) {
    for (int i = index; i < last; ++i)
      entry.second[static_cast<size_t>(i)] = entry.second[static_cast<size_t>(i + 1)];
    entry.second[static_cast<size_t>(last)] = true;
  }
  for (auto& entry : scenes.assigned)
    for (int i = index; i < last; ++i)
      entry.second[static_cast<size_t>(i)] = entry.second[static_cast<size_t>(i + 1)];
  --scenes.count;

  // The active scene moves with its content. If it was the one removed,
  // the scene that took its place is heard (the new last one if it was last).
  if (oldActive > index)
    scenes.active = oldActive - 1;
  else if (oldActive == index)
    scenes.active = std::min(index, scenes.count - 1);
  for (auto& entry : scenes.assigned)
    entry.second[static_cast<size_t>(last)] = entry.second[static_cast<size_t>(scenes.active)];
  if (oldActive == index) {
    chainHistory.endGesture();
    if (applySceneToBlocks(scenes.active))
      refreshBlockTailLength();
  }
  bumpChainRevision();
  return juce::Result::ok();
}

juce::var TONE3000Processor::getScenesState() const {
  SceneStore current;
  std::vector<std::pair<std::string, bool>> blocks;
  {
    juce::ScopedLock lock(chainMutex);
    current = scenesWithLiveValues();
    for (const auto& l : lanes)
      for (const auto& block : l)
        if (block->type != ChainBlockType::INSERT)
          blocks.emplace_back(block->id, block->enabled);
  }

  juce::Array<juce::var> sceneList;
  for (int i = 0; i < kNumScenes; ++i) {
    const auto slot = static_cast<size_t>(i);
    juce::DynamicObject::Ptr scene = new juce::DynamicObject();
    scene->setProperty("index", i);
    scene->setProperty("id", sceneLetter(i));
    scene->setProperty("name", current.nameOf(i));
    scene->setProperty("active", i == current.active);
    scene->setProperty("inUse", current.inUse(i));

    // Every tone block, also those no scene has a state of its own for:
    // such a block is in its current state everywhere.
    juce::DynamicObject::Ptr states = new juce::DynamicObject();
    for (const auto& [blockId, enabled] : blocks) {
      const auto found = current.blockStates.find(blockId);
      states->setProperty(juce::Identifier(juce::String(blockId)),
                          found != current.blockStates.end() ? found->second[slot] : enabled);
    }
    scene->setProperty("blocks", juce::var(states.get()));

    juce::Array<juce::var> values;
    for (const auto& [key, perScene] : current.assigned) {
      juce::var entry = keyVar(key);
      entry.getDynamicObject()->setProperty("value", tidy(perScene[slot]));
      values.add(entry);
    }
    scene->setProperty("values", values);
    sceneList.add(juce::var(scene.get()));
  }

  juce::Array<juce::var> assigned, expression;
  for (const auto& entry : current.assigned)
    assigned.add(keyVar(entry.first));
  for (const auto& key : current.expression)
    expression.add(keyVar(key));

  juce::DynamicObject::Ptr root = new juce::DynamicObject();
  root->setProperty("active", current.active);
  root->setProperty("activeId", sceneLetter(current.active));
  root->setProperty("count", current.count);
  root->setProperty("scenes", sceneList);
  root->setProperty("assigned", assigned);
  root->setProperty("expression", expression);
  return juce::var(root.get());
}

bool TONE3000Processor::renameScene(int index, const juce::String& name) {
  if (index < 0 || index >= kNumScenes)
    return false;
  juce::ScopedLock lock(chainMutex);
  if (!scenes.inUse(index))
    return false;
  juce::String stored = name.trim();
  if (stored == SceneStore::defaultName(index))
    stored.clear();
  if (scenes.names[static_cast<size_t>(index)] == stored)
    return true;
  pushChainHistory();
  scenes.names[static_cast<size_t>(index)] = stored;
  bumpChainRevision();
  return true;
}

bool TONE3000Processor::copyScene(int from, int to) {
  if (from < 0 || from >= kNumScenes || to < 0 || to >= kNumScenes)
    return false;
  if (from == to)
    return true;

  juce::ScopedLock lock(chainMutex);
  if (!scenes.inUse(from) || !scenes.inUse(to))
    return false;
  pushChainHistory();
  captureActiveScene();
  // Blocks without a state of their own are the same in both scenes already.
  for (auto& entry : scenes.blockStates)
    entry.second[static_cast<size_t>(to)] = entry.second[static_cast<size_t>(from)];
  for (auto& entry : scenes.assigned)
    entry.second[static_cast<size_t>(to)] = entry.second[static_cast<size_t>(from)];
  // Onto the scene being heard: it changes now, with the per-block fades.
  if (to == scenes.active && applySceneToBlocks(to))
    refreshBlockTailLength();
  bumpChainRevision();
  return true;
}

bool TONE3000Processor::resetScene(int index) {
  if (index < 0 || index >= kNumScenes)
    return false;

  juce::ScopedLock lock(chainMutex);
  if (!scenes.inUse(index))
    return false;
  pushChainHistory();
  captureActiveScene();
  const auto slot = static_cast<size_t>(index);
  const auto activeSlot = static_cast<size_t>(scenes.active);
  scenes.names[slot].clear();
  // "Every block on" has to hold for blocks no scene has touched yet too,
  // which means giving them a state of their own first.
  for (const auto& l : lanes)
    for (const auto& block : l)
      if (block->type != ChainBlockType::INSERT && !block->isMissingKind()) {
        registerSceneBlock(*block);
        scenes.blockStates[block->id][slot] = true;
      }
  for (auto& entry : scenes.assigned)
    entry.second[slot] = entry.second[activeSlot];
  if (index == scenes.active && applySceneToBlocks(index))
    refreshBlockTailLength();
  bumpChainRevision();
  return true;
}

juce::Result TONE3000Processor::assignSceneParam(const std::string& blockId,
                                                 const juce::String& param) {
  juce::ScopedLock lock(chainMutex);
  const ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type == ChainBlockType::INSERT)
    return juce::Result::fail("no tone block with id '" + juce::String(blockId) + "'");
  if (const juce::String refusal = sceneParamRefusal(*block, param); refusal.isNotEmpty())
    return juce::Result::fail(refusal);

  const SceneParamKey key{blockId, param};
  if (scenes.isExpression(key))
    return juce::Result::fail("'" + param +
                              "' is assigned to the expression pedal, which keeps it out of "
                              "the scenes");
  if (scenes.isAssigned(key))
    return juce::Result::ok();

  float value = 0.0f;
  if (!readSceneParam(*block, param, value))
    return juce::Result::fail("could not read '" + param + "'");
  pushChainHistory();
  // All eight scenes start from what is heard now, so assigning changes
  // nothing until a scene is given a value of its own.
  scenes.assigned[key].fill(value);
  bumpChainRevision();
  return juce::Result::ok();
}

juce::Result TONE3000Processor::unassignSceneParam(const std::string& blockId,
                                                   const juce::String& param) {
  juce::ScopedLock lock(chainMutex);
  const SceneParamKey key{blockId, param};
  if (!scenes.isAssigned(key))
    return juce::Result::fail("'" + param + "' is not scene-assigned on block '" +
                              juce::String(blockId) + "'");
  pushChainHistory();
  // The block keeps the value it has, the active scene's, and that is now
  // the one value every scene shares.
  scenes.assigned.erase(key);
  bumpChainRevision();
  return juce::Result::ok();
}

juce::Result TONE3000Processor::setSceneParamValue(int scene, const std::string& blockId,
                                                   const juce::String& param, double value) {
  if (scene < 0 || scene >= kNumScenes)
    return juce::Result::fail("no such scene");
  if (!std::isfinite(value))
    return juce::Result::fail("the value is not a number");

  juce::ScopedLock lock(chainMutex);
  if (!scenes.inUse(scene))
    return juce::Result::fail("scene " + sceneLetter(scene) + " is not in use");
  const SceneParamKey key{blockId, param};
  if (!scenes.isAssigned(key))
    return juce::Result::fail("'" + param + "' is not scene-assigned on block '" +
                              juce::String(blockId) + "' (see scene.assignParam)");

  // The scene being heard is the block itself.
  if (scene == scenes.active)
    return setBlockParam(blockId, param, value)
               ? juce::Result::ok()
               : juce::Result::fail("could not set '" + param + "'");

  float clamped = static_cast<float>(value);
  if (isCommonSceneParam(param)) {
    clamped = juce::jlimit(0.0f, 1.0f, clamped);
  } else if (const ChainBlock* block = findBlockById(blockId);
             block != nullptr && block->processor != nullptr) {
    if (const int index = block->processor->findParameter(param); index >= 0)
      clamped = block->processor->parameterTable()[static_cast<size_t>(index)].clamp(clamped);
  }

  pushChainHistory("scene:" + juce::String(scene) + ":" + juce::String(blockId) + ":" + param);
  scenes.assigned[key][static_cast<size_t>(scene)] = clamped;
  deferredRevisionBump();
  return juce::Result::ok();
}

juce::Result TONE3000Processor::setParamExpressionAssigned(const std::string& blockId,
                                                           const juce::String& param,
                                                           bool assigned) {
  juce::ScopedLock lock(chainMutex);
  const ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type == ChainBlockType::INSERT)
    return juce::Result::fail("no tone block with id '" + juce::String(blockId) + "'");

  const SceneParamKey key{blockId, param};
  if (!assigned) {
    if (!scenes.isExpression(key))
      return juce::Result::ok();
    pushChainHistory();
    scenes.expression.erase(key);
    bumpChainRevision();
    return juce::Result::ok();
  }

  // What a pedal can sweep is what a scene can hold: a continuous value
  // whose change needs no chain mute.
  if (const juce::String refusal = sceneParamRefusal(*block, param); refusal.isNotEmpty())
    return juce::Result::fail(refusal);
  if (scenes.isExpression(key))
    return juce::Result::ok();
  pushChainHistory();
  scenes.expression.insert(key);
  // Out of the scenes, and kept out (assignSceneParam refuses it): the
  // block keeps the active scene's value as the shared one.
  scenes.assigned.erase(key);
  bumpChainRevision();
  return juce::Result::ok();
}

// ####################
// STOMPS
// ####################
bool TONE3000Processor::setBlocksEnabled(
    const std::vector<std::pair<std::string, bool>>& changes) {
  juce::ScopedLock lock(chainMutex);
  std::vector<std::pair<ChainBlock*, bool>> resolved;
  for (const auto& [blockId, enabled] : changes) {
    ChainBlock* block = findBlockById(blockId);
    // A block that has left the rig, or a placeholder that cannot be edited,
    // is skipped: the others of a stomp still follow the switch.
    if (block == nullptr || block->type == ChainBlockType::INSERT || block->isMissingKind())
      continue;
    resolved.emplace_back(block, enabled);
  }
  if (resolved.empty())
    return false;

  pushChainHistory();
  const auto slot = static_cast<size_t>(scenes.active);
  for (const auto& [block, enabled] : resolved) {
    registerSceneBlock(*block);
    block->enabled = enabled;
    scenes.blockStates[block->id][slot] = enabled;
  }
  bumpChainRevision();
  return true;
}

juce::Result TONE3000Processor::assignStomp(int footswitch,
                                            const std::vector<std::string>& blockIds) {
  if (footswitch < 0 || footswitch >= kNumFootswitches)
    return juce::Result::fail("no such footswitch");
  juce::ScopedLock lock(chainMutex);

  std::vector<std::string> unique;
  for (const auto& blockId : blockIds) {
    const ChainBlock* block = findBlockById(blockId);
    if (block == nullptr || block->type == ChainBlockType::INSERT)
      return juce::Result::fail("no tone block with id '" + juce::String(blockId) + "'");
    if (std::find(unique.begin(), unique.end(), blockId) == unique.end())
      unique.push_back(blockId);
  }
  if (unique.empty())
    return juce::Result::fail("a stomp needs at least one block");

  pushChainHistory();
  stomps.stomps[static_cast<size_t>(footswitch)].blockIds = std::move(unique);
  bumpChainRevision();
  return juce::Result::ok();
}

juce::Result TONE3000Processor::unassignStomp(int footswitch, const std::string& blockId) {
  if (footswitch < 0 || footswitch >= kNumFootswitches)
    return juce::Result::fail("no such footswitch");
  juce::ScopedLock lock(chainMutex);
  StompAssignment& stomp = stomps.stomps[static_cast<size_t>(footswitch)];

  if (blockId.empty()) {
    if (stomp.isEmpty() && stomp.label.isEmpty() && !stomp.momentary)
      return juce::Result::ok();
    pushChainHistory();
    stomp = StompAssignment();
    bumpChainRevision();
    return juce::Result::ok();
  }

  if (std::find(stomp.blockIds.begin(), stomp.blockIds.end(), blockId) == stomp.blockIds.end())
    return juce::Result::fail("block '" + juce::String(blockId) +
                              "' is not assigned to footswitch " +
                              footswitchIdToString(footswitch));
  pushChainHistory();
  std::erase(stomp.blockIds, blockId);
  bumpChainRevision();
  return juce::Result::ok();
}

juce::Result TONE3000Processor::setStompOptions(int footswitch, const juce::String* label,
                                                const bool* momentary) {
  if (footswitch < 0 || footswitch >= kNumFootswitches)
    return juce::Result::fail("no such footswitch");
  juce::ScopedLock lock(chainMutex);
  StompAssignment& stomp = stomps.stomps[static_cast<size_t>(footswitch)];
  const juce::String newLabel = label != nullptr ? label->trim() : stomp.label;
  const bool newMomentary = momentary != nullptr ? *momentary : stomp.momentary;
  if (newLabel == stomp.label && newMomentary == stomp.momentary)
    return juce::Result::ok();
  pushChainHistory();
  stomp.label = newLabel;
  stomp.momentary = newMomentary;
  bumpChainRevision();
  return juce::Result::ok();
}

juce::var TONE3000Processor::getStompsState() const {
  StompStore current;
  {
    juce::ScopedLock lock(chainMutex);
    current = stomps;
  }
  juce::Array<juce::var> list;
  for (int i = 0; i < kNumFootswitches; ++i) {
    const StompAssignment& stomp = current.stomps[static_cast<size_t>(i)];
    juce::Array<juce::var> blockIds;
    for (const auto& blockId : stomp.blockIds)
      blockIds.add(juce::String(blockId));
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("switch", footswitchIdToString(i));
    obj->setProperty("index", i);
    obj->setProperty("blockIds", blockIds);
    obj->setProperty("label", stomp.label);
    obj->setProperty("momentary", stomp.momentary);
    list.add(juce::var(obj.get()));
  }
  juce::DynamicObject::Ptr root = new juce::DynamicObject();
  root->setProperty("stomps", list);
  return juce::var(root.get());
}

// ####################
// FOOTSWITCHES
// ####################
void TONE3000Processor::setFootswitchMode(FootswitchMode mode, bool persist) {
  if (getFootswitchMode() == mode)
    return;
  footswitchMode.store(static_cast<int>(mode));
  // Banks and pages belong to a layout; a new one starts at its first.
  footswitchBank.store(0);
  footswitchScenePage.store(0);
  if (persist)
    persistFootswitchSettings();
  bumpChainRevision();
}

void TONE3000Processor::setFootswitchHybridRows(FootswitchRowRole top, FootswitchRowRole bottom,
                                                bool persist) {
  if (hybridTopRow.load() == static_cast<int>(top) &&
      hybridBottomRow.load() == static_cast<int>(bottom))
    return;
  hybridTopRow.store(static_cast<int>(top));
  hybridBottomRow.store(static_cast<int>(bottom));
  footswitchBank.store(0);
  footswitchScenePage.store(0);
  if (persist)
    persistFootswitchSettings();
  bumpChainRevision();
}

int TONE3000Processor::presetsPerBank() const {
  switch (getFootswitchMode()) {
    case FootswitchMode::Preset: return kNumFootswitches;
    case FootswitchMode::Hybrid: {
      const int rows = (hybridTopRow.load() == static_cast<int>(FootswitchRowRole::Preset) ? 1 : 0) +
                       (hybridBottomRow.load() == static_cast<int>(FootswitchRowRole::Preset) ? 1
                                                                                             : 0);
      return rows * (kNumFootswitches / 2);
    }
    case FootswitchMode::Scene:
    case FootswitchMode::Stomp: break;
  }
  return 0;
}

bool TONE3000Processor::hybridScenePaged() const {
  if (getFootswitchMode() != FootswitchMode::Hybrid)
    return false;
  const bool topScene = hybridTopRow.load() == static_cast<int>(FootswitchRowRole::Scene);
  const bool bottomScene = hybridBottomRow.load() == static_cast<int>(FootswitchRowRole::Scene);
  return topScene != bottomScene;
}

TONE3000Processor::FootswitchRole TONE3000Processor::footswitchRole(int footswitch) const {
  using Kind = FootswitchRole::Kind;
  if (footswitch < 0 || footswitch >= kNumFootswitches)
    return {};

  const int bank = footswitchBank.load();
  switch (getFootswitchMode()) {
    case FootswitchMode::Preset: return {Kind::Preset, bank * kNumFootswitches + footswitch};
    case FootswitchMode::Scene: return {Kind::Scene, footswitch};
    case FootswitchMode::Stomp: return {Kind::Stomp, footswitch};
    case FootswitchMode::Hybrid: break;
  }

  const int rowSize = kNumFootswitches / 2;
  const bool topRow = footswitch < rowSize;
  const int column = footswitch % rowSize;
  const auto top = static_cast<FootswitchRowRole>(hybridTopRow.load());
  const auto bottom = static_cast<FootswitchRowRole>(hybridBottomRow.load());
  switch (topRow ? top : bottom) {
    case FootswitchRowRole::Stomp: return {Kind::Stomp, footswitch};
    case FootswitchRowRole::Scene:
      // Two scene rows show all eight; one shows a page of four.
      return {Kind::Scene,
              top == bottom ? footswitch : footswitchScenePage.load() * rowSize + column};
    case FootswitchRowRole::Preset:
      return {Kind::Preset, top == bottom ? bank * kNumFootswitches + footswitch
                                          : bank * rowSize + column};
  }
  return {};
}

void TONE3000Processor::footswitchPress(int footswitch) {
  if (footswitch < 0 || footswitch >= kNumFootswitchInputs)
    return;
  if (juce::MessageManager::existsAndIsCurrentThread())
    handleFootswitch(footswitch, true, juce::Time::getMillisecondCounterHiRes());
  else
    queueFootswitchEvent(footswitch, true);
}

void TONE3000Processor::footswitchRelease(int footswitch) {
  if (footswitch < 0 || footswitch >= kNumFootswitchInputs)
    return;
  if (juce::MessageManager::existsAndIsCurrentThread())
    handleFootswitch(footswitch, false, juce::Time::getMillisecondCounterHiRes());
  else
    queueFootswitchEvent(footswitch, false);
}

// Any thread, the audio thread included: three atomic stores and the same
// wake-up the MIDI mapper uses. Bit 31 marks the slot as written, bit 8 a
// press, the low byte the switch.
void TONE3000Processor::queueFootswitchEvent(int footswitch, bool press) {
  const juce::uint32 slot =
      footswitchQueueWrite.fetch_add(1, std::memory_order_relaxed) % kFootswitchQueueSize;
  footswitchQueueTimes[slot].store(juce::Time::getMillisecondCounterHiRes(),
                                   std::memory_order_relaxed);
  footswitchQueueEvents[slot].store(0x80000000u | (press ? 0x100u : 0u) |
                                        static_cast<juce::uint32>(footswitch),
                                    std::memory_order_release);
  triggerAsyncUpdate();
}

void TONE3000Processor::drainFootswitchQueue() {
  while (footswitchQueueRead != footswitchQueueWrite.load(std::memory_order_acquire)) {
    const juce::uint32 slot = footswitchQueueRead % kFootswitchQueueSize;
    const juce::uint32 event = footswitchQueueEvents[slot].exchange(0, std::memory_order_acquire);
    if (event == 0) {
      // Claimed but not written yet: its writer is between two stores. Come
      // back for it rather than deliver later events ahead of it.
      triggerAsyncUpdate();
      return;
    }
    ++footswitchQueueRead;
    handleFootswitch(static_cast<int>(event & 0xffu), (event & 0x100u) != 0,
                     footswitchQueueTimes[slot].load(std::memory_order_relaxed));
  }
}

bool TONE3000Processor::tapTempo(double timeMs) {
  if (timeMs < 0.0)
    timeMs = juce::Time::getMillisecondCounterHiRes();
  double bpm = 0.0;
  if (!tapTempoCounter.tap(timeMs / 1000.0, bpm))
    return false;
  setTempo(bpm);
  return true;
}

// Message thread.
void TONE3000Processor::handleFootswitch(int footswitch, bool press, double timeMs) {
  footswitchHeld[static_cast<size_t>(footswitch)].store(press);
  // The snapshot shows which switches are down.
  bumpChainRevision();

  if (footswitch == kFootswitchTempo) {
    if (press)
      tapTempo(timeMs);
    return;
  }

  if (footswitch == kFootswitchBankUp || footswitch == kFootswitchBankDown) {
    if (!press)
      return;
    const int delta = footswitch == kFootswitchBankUp ? 1 : -1;
    bool paged = false;
    if (const int perBank = presetsPerBank(); perBank > 0) {
      // Up to the last slot in use: a bank of empty slots in between is
      // still a bank the player set up.
      const int count = static_cast<int>(currentPresetSlots().size());
      const int banks = juce::jmax(1, (count + perBank - 1) / perBank);
      footswitchBank.store(((footswitchBank.load() + delta) % banks + banks) % banks);
      paged = true;
    }
    if (hybridScenePaged()) {
      footswitchScenePage.store(footswitchScenePage.load() == 0 ? 1 : 0);
      paged = true;
    }
    // Nothing to page (Scene and Stomp mode, a hybrid of two scene rows or
    // of stomps): the bank switches walk the preset list instead.
    if (!paged)
      stepPreset(delta);
    return;
  }

  // A momentary stomp lets go of exactly what its press took, whatever the
  // switch has become since.
  if (!press) {
    std::vector<std::string> held = std::move(momentaryHeldBlocks[static_cast<size_t>(footswitch)]);
    momentaryHeldBlocks[static_cast<size_t>(footswitch)].clear();
    if (held.empty())
      return;
    std::vector<std::pair<std::string, bool>> changes;
    {
      juce::ScopedLock lock(chainMutex);
      for (const auto& blockId : held)
        if (const ChainBlock* block = findBlockById(blockId))
          changes.emplace_back(blockId, !block->enabled);
    }
    setBlocksEnabled(changes);
    return;
  }

  const FootswitchRole role = footswitchRole(footswitch);
  switch (role.kind) {
    case FootswitchRole::Kind::Preset:
      loadPresetAtIndex(role.target);  // a slot past the end of the list does nothing
      break;
    case FootswitchRole::Kind::Scene:
      selectScene(role.target);
      break;
    case FootswitchRole::Kind::Stomp: {
      std::vector<std::pair<std::string, bool>> changes;
      bool momentary = false;
      {
        juce::ScopedLock lock(chainMutex);
        const StompAssignment& stomp = stomps.stomps[static_cast<size_t>(role.target)];
        momentary = stomp.momentary;
        for (const auto& blockId : stomp.blockIds)
          if (const ChainBlock* block = findBlockById(blockId))
            changes.emplace_back(blockId, !block->enabled);
      }
      if (setBlocksEnabled(changes) && momentary) {
        auto& held = momentaryHeldBlocks[static_cast<size_t>(footswitch)];
        held.clear();
        for (const auto& change : changes)
          held.push_back(change.first);
      }
      break;
    }
    case FootswitchRole::Kind::None:
      break;
  }
}

juce::String TONE3000Processor::blockCategoryName(const ChainBlock& block) {
  // The refinements getChainState makes: a pedal capture is a drive, a long
  // IR a reverb.
  if (block.type == ChainBlockType::NAM &&
      block.toneSummary["gear"].toString().equalsIgnoreCase("pedal"))
    return blockCategoryToString(BlockCategory::Drive);
  if (const IrBlockProcessor* ir = block.ir(); ir != nullptr && ir->isLong())
    return blockCategoryToString(BlockCategory::Reverb);
  const juce::String kindId =
      block.type == ChainBlockType::KIND ? block.kindId : chainBlockTypeToString(block.type);
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kindId);
  return info != nullptr ? blockCategoryToString(info->category) : juce::String();
}

juce::var TONE3000Processor::getFootswitchState() const {
  // The preset list is read off the lock (a directory listing); everything
  // the rig holds is copied under it.
  const auto presets = currentPresetSlots();

  struct StompRow {
    bool assigned = false, momentary = false, lit = false;
    juce::String label, colour;
    juce::Array<juce::var> blockIds;
  };
  std::array<StompRow, kNumFootswitches> stompRows;
  std::array<juce::String, kNumScenes> sceneNames;
  int activeScene = 0;
  int sceneCount = 1;
  juce::String presetId;
  {
    juce::ScopedLock lock(chainMutex);
    activeScene = scenes.active;
    sceneCount = scenes.count;
    presetId = activePresetId;
    for (int i = 0; i < kNumScenes; ++i)
      sceneNames[static_cast<size_t>(i)] = scenes.nameOf(i);

    for (int i = 0; i < kNumFootswitches; ++i) {
      const StompAssignment& stomp = stomps.stomps[static_cast<size_t>(i)];
      StompRow& row = stompRows[static_cast<size_t>(i)];
      row.momentary = stomp.momentary;
      row.label = stomp.label;
      for (const auto& blockId : stomp.blockIds) {
        const ChainBlock* block = nullptr;
        for (const auto& l : lanes)
          for (const auto& candidate : l)
            if (candidate->id == blockId)
              block = candidate.get();
        if (block == nullptr)
          continue;
        row.blockIds.add(juce::String(blockId));
        if (row.assigned)
          continue;
        // The first block speaks for the switch: its state lights it, its
        // category colours it and, without a label, its name names it.
        row.assigned = true;
        row.lit = block->enabled;
        row.colour = blockCategoryName(*block);
        if (row.label.isEmpty())
          row.label = block->type == ChainBlockType::KIND && block->activeModelId == 0
                          ? block->kindName
                          : block->toneSummary["title"].toString();
      }
    }
  }

  int activePresetIndex = -1;
  for (size_t i = 0; i < presets.size(); ++i)
    if (presetId.isNotEmpty() && presets[i].id == presetId)
      activePresetIndex = static_cast<int>(i);

  const int perBank = presetsPerBank();
  const bool scenePaged = hybridScenePaged();

  juce::Array<juce::var> switches;
  for (int i = 0; i < kNumFootswitchInputs; ++i) {
    juce::DynamicObject::Ptr item = new juce::DynamicObject();
    item->setProperty("id", footswitchIdToString(i));
    item->setProperty("index", i);
    juce::String roleName = "none", label, colour;
    bool lit = false, empty = false;

    if (i == kFootswitchTempo) {
      roleName = "tapTempo";
      label = "Tap";
      colour = "tempo";
    } else if (i == kFootswitchBankUp || i == kFootswitchBankDown) {
      const bool up = i == kFootswitchBankUp;
      if (perBank > 0 || scenePaged) {
        roleName = up ? "bankUp" : "bankDown";
        label = up ? "Bank Up" : "Bank Down";
      } else {
        roleName = up ? "presetNext" : "presetPrevious";
        label = up ? "Next Preset" : "Previous Preset";
      }
      colour = "bank";
    } else {
      const FootswitchRole role = footswitchRole(i);
      switch (role.kind) {
        case FootswitchRole::Kind::Preset: {
          roleName = colour = "preset";
          item->setProperty("presetIndex", role.target);
          if (role.target >= 0 && role.target < static_cast<int>(presets.size()) &&
              presets[static_cast<size_t>(role.target)].id.isNotEmpty()) {
            const auto& preset = presets[static_cast<size_t>(role.target)];
            label = preset.name;
            lit = role.target == activePresetIndex;
            item->setProperty("presetId", preset.id);
          } else {
            empty = true;
          }
          break;
        }
        case FootswitchRole::Kind::Scene:
          roleName = colour = "scene";
          label = sceneNames[static_cast<size_t>(role.target)];
          lit = role.target == activeScene;
          // A scene the preset does not use: nothing behind the switch.
          empty = role.target >= sceneCount;
          item->setProperty("scene", sceneLetter(role.target));
          break;
        case FootswitchRole::Kind::Stomp: {
          const StompRow& row = stompRows[static_cast<size_t>(role.target)];
          roleName = "stomp";
          label = row.label;
          colour = row.colour;
          lit = row.lit;
          empty = !row.assigned;
          item->setProperty("blockIds", row.blockIds);
          item->setProperty("momentary", row.momentary);
          break;
        }
        case FootswitchRole::Kind::None:
          break;
      }
    }

    item->setProperty("role", roleName);
    item->setProperty("label", label);
    item->setProperty("colour", colour);
    item->setProperty("lit", lit);
    item->setProperty("held", footswitchHeld[static_cast<size_t>(i)].load());
    // Nothing behind the switch: a stomp with no block, a preset slot past
    // the end of the list.
    item->setProperty("empty", empty);
    switches.add(juce::var(item.get()));
  }

  juce::DynamicObject::Ptr rows = new juce::DynamicObject();
  rows->setProperty("top", footswitchRowRoleToString(
                               static_cast<FootswitchRowRole>(hybridTopRow.load())));
  rows->setProperty("bottom", footswitchRowRoleToString(
                                  static_cast<FootswitchRowRole>(hybridBottomRow.load())));

  juce::DynamicObject::Ptr root = new juce::DynamicObject();
  root->setProperty("mode", footswitchModeToString(getFootswitchMode()));
  root->setProperty("hybridRows", juce::var(rows.get()));
  root->setProperty("bank", footswitchBank.load());
  root->setProperty("bankCount",
                    perBank > 0 ? juce::jmax(1, (static_cast<int>(presets.size()) + perBank - 1) /
                                                    perBank)
                                : 0);
  root->setProperty("scenePage", footswitchScenePage.load());
  root->setProperty("activeScene", sceneLetter(activeScene));
  root->setProperty("sceneCount", sceneCount);
  root->setProperty("bpm", getTempo());
  root->setProperty("switches", switches);
  return juce::var(root.get());
}

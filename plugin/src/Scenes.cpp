#include "Scenes.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr const char* kCommonSceneParams[] = {"inputGain", "outputGain", "mix"};

bool isCommonSceneParam(const juce::String& paramId) {
  for (const auto* id : kCommonSceneParams)
    if (paramId == id)
      return true;
  return false;
}

}  // namespace

juce::String sceneLetter(int index) {
  if (index < 0 || index >= kNumScenes)
    return {};
  return juce::String::charToString(static_cast<juce::juce_wchar>('A' + index));
}

int sceneIndexFromLetter(const juce::String& letter) {
  if (letter.length() != 1)
    return -1;
  const int index = static_cast<int>(letter.toUpperCase()[0]) - static_cast<int>('A');
  return index >= 0 && index < kNumScenes ? index : -1;
}

juce::String footswitchIdToString(int footswitch) {
  if (footswitch >= 0 && footswitch < kNumFootswitches)
    return sceneLetter(footswitch);
  if (footswitch == kFootswitchBankUp)
    return "bankUp";
  if (footswitch == kFootswitchBankDown)
    return "bankDown";
  if (footswitch == kFootswitchTempo)
    return "tempo";
  return {};
}

int footswitchIdFromString(const juce::String& id) {
  if (id == "bankUp")
    return kFootswitchBankUp;
  if (id == "bankDown")
    return kFootswitchBankDown;
  if (id == "tempo")
    return kFootswitchTempo;
  return sceneIndexFromLetter(id);
}

// ####################
// SCENES
// ####################
juce::String SceneStore::defaultName(int index) { return "Scene " + sceneLetter(index); }

juce::String SceneStore::nameOf(int index) const {
  if (index < 0 || index >= kNumScenes)
    return {};
  const juce::String& name = names[static_cast<size_t>(index)];
  return name.isNotEmpty() ? name : defaultName(index);
}

bool SceneStore::isPristine() const {
  if (active != 0 || count != 1 || !blockStates.empty() || !assigned.empty() || !expression.empty())
    return false;
  for (const auto& name : names)
    if (name.isNotEmpty())
      return false;
  return true;
}

void SceneStore::clear() { *this = SceneStore(); }

int SceneStore::inferCount() const {
  int highest = active;
  for (int i = 1; i < kNumScenes; ++i) {
    const auto slot = static_cast<size_t>(i);
    bool used = names[slot].isNotEmpty();
    for (const auto& entry : blockStates)
      used = used || !entry.second[slot];
    for (const auto& entry : assigned)
      used = used || entry.second[slot] != entry.second[0];
    if (used)
      highest = std::max(highest, i);
  }
  return juce::jlimit(1, kNumScenes, highest + 1);
}

void SceneStore::removeBlock(const std::string& blockId) {
  blockStates.erase(blockId);
  std::erase_if(assigned, [&](const auto& entry) { return entry.first.blockId == blockId; });
  std::erase_if(expression, [&](const SceneParamKey& key) { return key.blockId == blockId; });
}

void SceneStore::removeKindParams(const std::string& blockId) {
  std::erase_if(assigned, [&](const auto& entry) {
    return entry.first.blockId == blockId && !isCommonSceneParam(entry.first.paramId);
  });
  std::erase_if(expression, [&](const SceneParamKey& key) {
    return key.blockId == blockId && !isCommonSceneParam(key.paramId);
  });
}

juce::ValueTree SceneStore::toValueTree() const {
  juce::ValueTree tree("Scenes");
  tree.setProperty("version", 1, nullptr);
  tree.setProperty("active", active, nullptr);
  tree.setProperty("count", count, nullptr);

  for (int i = 0; i < kNumScenes; ++i) {
    const auto slot = static_cast<size_t>(i);
    juce::ValueTree scene("Scene");
    scene.setProperty("index", i, nullptr);
    if (names[slot].isNotEmpty())
      scene.setProperty("name", names[slot], nullptr);
    for (const auto& [blockId, states] : blockStates) {
      juce::ValueTree block("Block");
      block.setProperty("id", juce::String(blockId), nullptr);
      block.setProperty("enabled", states[slot], nullptr);
      scene.appendChild(block, nullptr);
    }
    for (const auto& [key, values] : assigned) {
      juce::ValueTree value("Value");
      value.setProperty("blockId", juce::String(key.blockId), nullptr);
      value.setProperty("param", key.paramId, nullptr);
      value.setProperty("value", static_cast<double>(values[slot]), nullptr);
      scene.appendChild(value, nullptr);
    }
    tree.appendChild(scene, nullptr);
  }

  for (const auto& key : expression) {
    juce::ValueTree flag("Expression");
    flag.setProperty("blockId", juce::String(key.blockId), nullptr);
    flag.setProperty("param", key.paramId, nullptr);
    tree.appendChild(flag, nullptr);
  }
  return tree;
}

void SceneStore::restoreFromValueTree(const juce::ValueTree& tree) {
  clear();
  if (!tree.hasType("Scenes"))
    return;

  active = juce::jlimit(0, kNumScenes - 1, static_cast<int>(tree.getProperty("active", 0)));

  // Which scenes named each entry, so one a file left out of some scene (a
  // hand edit; this build always writes all eight) takes the value of a
  // scene that has it rather than a made-up default.
  std::map<std::string, std::array<bool, kNumScenes>> statesSeen;
  std::map<SceneParamKey, std::array<bool, kNumScenes>> valuesSeen;

  for (const auto& child : tree) {
    if (child.hasType("Scene")) {
      const int index = child.getProperty("index", -1);
      if (index < 0 || index >= kNumScenes)
        continue;
      const auto slot = static_cast<size_t>(index);
      names[slot] = child.getProperty("name").toString();
      for (const auto& entry : child) {
        if (entry.hasType("Block")) {
          const std::string blockId = entry.getProperty("id").toString().toStdString();
          if (blockId.empty())
            continue;
          blockStates[blockId][slot] = static_cast<bool>(entry.getProperty("enabled", true));
          statesSeen[blockId][slot] = true;
        } else if (entry.hasType("Value")) {
          const SceneParamKey key{entry.getProperty("blockId").toString().toStdString(),
                                  entry.getProperty("param").toString()};
          if (key.blockId.empty() || key.paramId.isEmpty())
            continue;
          const double value = entry.getProperty("value", 0.0);
          if (!std::isfinite(value))
            continue;
          assigned[key][slot] = static_cast<float>(value);
          valuesSeen[key][slot] = true;
        }
      }
    } else if (child.hasType("Expression")) {
      const SceneParamKey key{child.getProperty("blockId").toString().toStdString(),
                              child.getProperty("param").toString()};
      if (!key.blockId.empty() && key.paramId.isNotEmpty())
        expression.insert(key);
    }
  }

  const auto fillGaps = [](auto& values, const std::array<bool, kNumScenes>& seen) {
    const auto first = std::find(seen.begin(), seen.end(), true);
    if (first == seen.end())
      return;
    const auto source = values[static_cast<size_t>(first - seen.begin())];
    for (size_t i = 0; i < seen.size(); ++i)
      if (!seen[i])
        values[i] = source;
  };
  for (auto& [blockId, states] : blockStates)
    fillGaps(states, statesSeen[blockId]);
  for (auto& [key, values] : assigned)
    fillGaps(values, valuesSeen[key]);

  count = tree.hasProperty("count")
              ? juce::jlimit(1, kNumScenes, static_cast<int>(tree.getProperty("count")))
              : inferCount();
  active = std::min(active, count - 1);

  // The exclusion holds for whatever a file says.
  for (const auto& key : expression)
    assigned.erase(key);
}

// ####################
// STOMPS
// ####################
bool StompStore::isEmpty() const {
  for (const auto& stomp : stomps)
    if (!stomp.isEmpty() || stomp.label.isNotEmpty() || stomp.momentary)
      return false;
  return true;
}

void StompStore::clear() { *this = StompStore(); }

void StompStore::removeBlock(const std::string& blockId) {
  for (auto& stomp : stomps)
    std::erase(stomp.blockIds, blockId);
}

juce::ValueTree StompStore::toValueTree() const {
  juce::ValueTree tree("Stomps");
  tree.setProperty("version", 1, nullptr);
  for (int i = 0; i < kNumFootswitches; ++i) {
    const StompAssignment& stomp = stomps[static_cast<size_t>(i)];
    if (stomp.isEmpty() && stomp.label.isEmpty() && !stomp.momentary)
      continue;
    juce::ValueTree node("Stomp");
    node.setProperty("switch", i, nullptr);
    if (stomp.label.isNotEmpty())
      node.setProperty("label", stomp.label, nullptr);
    node.setProperty("momentary", stomp.momentary, nullptr);
    for (const auto& blockId : stomp.blockIds) {
      juce::ValueTree block("Block");
      block.setProperty("id", juce::String(blockId), nullptr);
      node.appendChild(block, nullptr);
    }
    tree.appendChild(node, nullptr);
  }
  return tree;
}

void StompStore::restoreFromValueTree(const juce::ValueTree& tree) {
  clear();
  if (!tree.hasType("Stomps"))
    return;
  for (const auto& node : tree) {
    if (!node.hasType("Stomp"))
      continue;
    const int index = node.getProperty("switch", -1);
    if (index < 0 || index >= kNumFootswitches)
      continue;
    StompAssignment& stomp = stomps[static_cast<size_t>(index)];
    stomp.label = node.getProperty("label").toString();
    stomp.momentary = static_cast<bool>(node.getProperty("momentary", false));
    for (const auto& block : node) {
      if (!block.hasType("Block"))
        continue;
      const std::string blockId = block.getProperty("id").toString().toStdString();
      if (!blockId.empty() &&
          std::find(stomp.blockIds.begin(), stomp.blockIds.end(), blockId) == stomp.blockIds.end())
        stomp.blockIds.push_back(blockId);
    }
  }
}

// ####################
// FOOTSWITCH MODES
// ####################
juce::String footswitchModeToString(FootswitchMode mode) {
  switch (mode) {
    case FootswitchMode::Preset: return "preset";
    case FootswitchMode::Scene: return "scene";
    case FootswitchMode::Hybrid: return "hybrid";
    case FootswitchMode::Stomp: break;
  }
  return "stomp";
}

FootswitchMode footswitchModeFromString(const juce::String& text, FootswitchMode fallback) {
  if (text == "preset") return FootswitchMode::Preset;
  if (text == "scene") return FootswitchMode::Scene;
  if (text == "stomp") return FootswitchMode::Stomp;
  if (text == "hybrid") return FootswitchMode::Hybrid;
  return fallback;
}

juce::String footswitchRowRoleToString(FootswitchRowRole role) {
  switch (role) {
    case FootswitchRowRole::Scene: return "scene";
    case FootswitchRowRole::Preset: return "preset";
    case FootswitchRowRole::Stomp: break;
  }
  return "stomp";
}

FootswitchRowRole footswitchRowRoleFromString(const juce::String& text,
                                              FootswitchRowRole fallback) {
  if (text == "scene") return FootswitchRowRole::Scene;
  if (text == "stomp") return FootswitchRowRole::Stomp;
  if (text == "preset") return FootswitchRowRole::Preset;
  return fallback;
}

// ####################
// TAP TEMPO
// ####################
void TapTempo::reset() {
  hasLast = false;
  lastTime = 0.0;
  numIntervals = 0;
  pendingOutlier = 0.0;
}

bool TapTempo::tap(double timeSeconds, double& bpm) {
  if (!std::isfinite(timeSeconds))
    return false;

  const double shortest = 60.0 / kMaxBpm;
  const double longest = 60.0 / kMinBpm;

  if (!hasLast || timeSeconds < lastTime) {
    reset();
    hasLast = true;
    lastTime = timeSeconds;
    return false;
  }

  const double interval = timeSeconds - lastTime;
  // A bounce: the switch, not the player. The count carries on from the
  // tap it follows.
  if (interval < shortest * 0.999)
    return false;
  lastTime = timeSeconds;

  // Too long ago to belong to the same count: this tap starts a new one.
  if (interval > longest * 1.001) {
    numIntervals = 0;
    pendingOutlier = 0.0;
    return false;
  }

  const auto average = [this] {
    double sum = 0.0;
    for (int i = 0; i < numIntervals; ++i)
      sum += intervals[static_cast<size_t>(i)];
    return sum / numIntervals;
  };
  const auto closeTo = [](double a, double b) {
    return std::abs(a - b) <= kOutlierTolerance * b;
  };
  const auto push = [this](double value) {
    if (numIntervals == kMaxIntervals) {
      std::copy(intervals.begin() + 1, intervals.end(), intervals.begin());
      --numIntervals;
    }
    intervals[static_cast<size_t>(numIntervals++)] = value;
  };

  if (numIntervals > 0 && !closeTo(interval, average())) {
    if (pendingOutlier > 0.0 && closeTo(interval, pendingOutlier)) {
      // Twice in a row, and the two agree: a new tempo, not a slip.
      numIntervals = 0;
      push(pendingOutlier);
      push(interval);
      pendingOutlier = 0.0;
    } else {
      // Left out; the tempo stays where the count has it.
      pendingOutlier = interval;
      return false;
    }
  } else {
    pendingOutlier = 0.0;
    push(interval);
  }

  bpm = juce::jlimit(kMinBpm, kMaxBpm, 60.0 / average());
  return true;
}

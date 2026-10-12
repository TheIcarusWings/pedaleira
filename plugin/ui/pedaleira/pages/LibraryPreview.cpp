#include "LibraryPreview.h"

#include "GridModel.h"

namespace pedaleira::ui {

namespace {

juce::String plural(int count, const juce::String& one, const juce::String& many) {
  return juce::String(count) + " " + (count == 1 ? one : many);
}

// The array of a preset file's node, whatever the node is called.
const juce::Array<juce::var>* childrenOf(const juce::var& node) { return node["children"].getArray(); }

const juce::var* findChild(const juce::var& node, const juce::String& name) {
  if (const auto* children = childrenOf(node))
    for (const auto& child : *children)
      if (child["node"].toString() == name)
        return &child;
  return nullptr;
}

// A block of a preset file: its category, or nothing for an empty slot.
// Counts what it is for the "Uses" line.
std::optional<Category> blockOf(const juce::var& block, PresetPreview& out) {
  const juce::String type = block["type"].toString();
  if (type == "insert")
    return std::nullopt;
  if (type == "nam") {
    ++out.captures;
    return Category::amp;
  }
  if (type == "ir") {
    ++out.irs;
    return Category::cab;
  }
  if (type == "plugin") {
    out.plugins.add(block["name"].toString().isNotEmpty() ? block["name"].toString() : juce::String("plugin"));
    return Category::plugin;
  }
  ++out.effects;
  if (const auto* state = findChild(block, "KindState"))
    return categoryFromId((*state)["kind"].toString().upToFirstOccurrenceOf(".", false, false));
  return Category::utility;
}

}  // namespace

juce::String PresetPreview::usesLine() const {
  juce::StringArray parts;
  if (captures > 0)
    parts.add(plural(captures, "NAM capture", "NAM captures"));
  if (irs > 0)
    parts.add(plural(irs, "IR", "IRs"));
  if (effects > 0)
    parts.add(plural(effects, "effect", "effects"));
  if (!plugins.isEmpty()) {
    juce::String text = plural(plugins.size(), "plugin", "plugins");
    juce::StringArray named;
    for (const auto& name : plugins)
      if (name != "plugin")
        named.add(name);
    if (!named.isEmpty())
      text << " (" << named.joinIntoString(", ") << ")";
    parts.add(text);
  }
  return parts.isEmpty() ? juce::String() : "Uses: " + parts.joinIntoString(", ");
}

PresetPreview PresetPreview::fromRig(EngineClient& engine) {
  PresetPreview out;
  const GridModel model = GridModel::read(engine);
  out.available = true;
  for (int r = 0; r < 4; ++r)
    for (int s = 0; s < 8; ++s) {
      const GridSlot& slot = model.rows[static_cast<size_t>(r)].slots[static_cast<size_t>(s)];
      if (slot.empty)
        continue;
      out.cells[static_cast<size_t>(r)][static_cast<size_t>(s)] = slot.category;
      if (slot.kindId == "nam")
        ++out.captures;
      else if (slot.kindId == "ir")
        ++out.irs;
      else if (slot.category == Category::plugin) {
        out.plugins.add(slot.name.isNotEmpty() ? slot.name : juce::String("plugin"));
      } else {
        ++out.effects;
      }
    }
  for (size_t i = 0; i < model.scenes.size(); ++i) {
    const GridScene& scene = model.scenes[i];
    if (i == 0 || scene.named || scene.active) {
      out.sceneKeys.add(scene.id);
      out.sceneNames.add(scene.name);
    }
  }
  out.tempo = static_cast<double>(engine.read("footswitch.getState")["bpm"]);
  return out;
}

PresetPreview PresetPreview::fromFile(const juce::String& path) {
  PresetPreview out;
  const juce::File file(path);
  if (!file.existsAsFile() || file.getSize() > 64 * 1024 * 1024)
    return out;
  const juce::var parsed = juce::JSON::parse(file);
  if (!parsed.isObject())
    return out;
  const juce::var chain = parsed["chain"];
  const auto* nodes = childrenOf(chain);
  if (nodes == nullptr)
    return out;
  out.available = true;
  out.tempo = parsed["tempo"].isVoid() ? 0.0 : static_cast<double>(parsed["tempo"]);

  const auto place = [&](size_t row, const juce::Array<juce::var>* blocks) {
    if (blocks == nullptr || row >= 4)
      return;
    for (size_t slot = 0; slot < blocks->size() && slot < 8; ++slot)
      if (const auto category = blockOf(blocks->getReference(static_cast<int>(slot)), out))
        out.cells[row][slot] = *category;
  };

  for (const auto& node : *nodes) {
    const juce::String name = node["node"].toString();
    if (name == "Grid") {
      if (const auto* rows = childrenOf(node))
        for (const auto& row : *rows) {
          if (row["node"].toString() != "Row")
            continue;
          if (const auto* slots = findChild(row, "Slots"))
            place(static_cast<size_t>(static_cast<int>(row["index"])), childrenOf(*slots));
        }
    } else if (name == "ChainBlocks") {
      place(0, childrenOf(node));
    } else if (name == "RightChainBlocks") {
      place(1, childrenOf(node));
    } else if (name == "Scenes") {
      const int active = node["active"];
      if (const auto* scenes = childrenOf(node))
        for (const auto& scene : *scenes) {
          if (scene["node"].toString() != "Scene")
            continue;
          const int index = scene["index"];
          const juce::String sceneName = scene["name"].toString();
          const bool hasBlock = findChild(scene, "Block") != nullptr;
          if (index != 0 && sceneName.isEmpty() && !hasBlock && index != active)
            continue;
          const juce::String key = juce::String::charToString(static_cast<juce::juce_wchar>('A' + index));
          out.sceneKeys.add(key);
          out.sceneNames.add(sceneName.isNotEmpty() ? sceneName : "Scene " + key);
        }
    }
  }
  return out;
}

}  // namespace pedaleira::ui

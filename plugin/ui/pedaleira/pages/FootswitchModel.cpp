#include "FootswitchModel.h"

#include "../Type.h"

namespace pedaleira::ui {

namespace {

constexpr const char* kLetters[8] = {"A", "B", "C", "D", "E", "F", "G", "H"};

juce::String slotNumber(int slot) { return juce::String(slot).paddedLeft('0', 3); }

}  // namespace

const char* FootswitchModel::letterOf(int index) { return kLetters[juce::jlimit(0, 7, index)]; }

void applyFace(Footswitch& widget, const SwitchFace& face) {
  widget.setLabel(face.name);
  widget.setSub(face.sub);
  widget.setNumber(face.number);
  widget.setLook(face.look, face.ring);
}

FootswitchModel FootswitchModel::read(EngineClient& engine) {
  FootswitchModel model;
  const juce::var state = engine.read("state.get");

  const juce::var switches = state["footswitches"];
  model.mode = switches["mode"].toString();
  model.hybridTop = switches["hybridRows"]["top"].toString();
  model.hybridBottom = switches["hybridRows"]["bottom"].toString();
  model.activeScene = switches["activeScene"].toString();
  model.bank = switches["bank"];
  model.bankCount = switches["bankCount"];
  model.scenePage = switches["scenePage"];
  model.bpm = switches["bpm"];
  if (model.bpm <= 0.0)
    model.bpm = 120.0;

  const juce::var library = state["library"];
  model.presetId = library["current"]["presetId"].toString();
  model.presetSlot = library["current"]["slot"];
  const juce::String setlistId = library["current"]["setlist"].toString();
  if (const auto* setlists = library["setlists"].getArray())
    for (const auto& setlist : *setlists)
      if (setlist["id"].toString() == setlistId)
        model.setlistName = setlist["name"].toString();

  if (const auto* list = state["presets"].getArray())
    for (const auto& preset : *list) {
      PresetEntry entry;
      entry.slot = preset["slot"];
      entry.id = preset["id"].toString();
      entry.name = preset["name"].toString();
      if (entry.id == model.presetId)
        model.presetName = entry.name;
      model.presets.push_back(entry);
    }

  for (int i = 0; i < 8; ++i)
    model.sceneNames[static_cast<size_t>(i)] = juce::String("Scene ") + kLetters[i];
  if (const auto* scenes = state["scenes"]["scenes"].getArray())
    for (int i = 0; i < scenes->size() && i < 8; ++i)
      model.sceneNames[static_cast<size_t>(i)] = scenes->getReference(i)["name"].toString();

  if (const auto* stomps = state["stomps"].getArray())
    for (int i = 0; i < stomps->size() && i < 8; ++i) {
      const juce::var& stomp = stomps->getReference(i);
      Stomp& out = model.stomps[static_cast<size_t>(i)];
      out.label = stomp["label"].toString();
      out.momentary = static_cast<bool>(stomp["momentary"]);
      if (const auto* ids = stomp["blockIds"].getArray())
        for (const auto& id : *ids)
          out.blockIds.add(id.toString());
    }

  // Every block of the rig, grid rows or legacy lanes.
  const juce::var chain = state["chain"];
  std::vector<juce::var> lists;
  if (const auto* rows = chain["grid"]["rows"].getArray()) {
    for (const auto& row : *rows)
      lists.push_back(row["blocks"]);
  } else {
    lists.push_back(chain["chain"]);
    lists.push_back(chain["chainRight"]);
  }
  for (const auto& list : lists)
    if (const auto* blocks = list.getArray())
      for (const auto& block : *blocks) {
        if (block["kind"].toString() == "insert")
          continue;
        BlockFact fact;
        fact.name = block["tone"]["title"].toString();
        fact.category = categoryFromId(block["category"].toString());
        fact.enabled = static_cast<bool>(block["params"]["enabled"]);
        model.blocks[block["blockId"].toString()] = fact;
      }
  return model;
}

juce::StringArray FootswitchModel::stompBlocks(int index) const {
  juce::StringArray found;
  for (const auto& id : stomps[static_cast<size_t>(index)].blockIds)
    if (blocks.count(id) > 0)
      found.add(id);
  return found;
}

bool FootswitchModel::stompAssigned(int index) const { return !stompBlocks(index).isEmpty(); }

int FootswitchModel::stompsAssigned() const {
  int count = 0;
  for (int i = 0; i < 8; ++i)
    count += stompAssigned(i) ? 1 : 0;
  return count;
}

SwitchFace FootswitchModel::stompFace(int index) const {
  SwitchFace face;
  face.letter = letterOf(index);
  const juce::StringArray ids = stompBlocks(index);
  if (ids.isEmpty()) {
    face.name = "Unassigned";
    face.look = Footswitch::Look::empty;
    return face;
  }
  // The label, else the first block's name; the colour and the state are
  // the first block's.
  const BlockFact& first = blocks.at(ids[0]);
  const juce::String& label = stomps[static_cast<size_t>(index)].label;
  face.name = label.isNotEmpty() ? label : first.name;
  face.ring = categoryColour(first.category);
  face.look = first.enabled ? Footswitch::Look::stomp : Footswitch::Look::off;
  face.sub = first.enabled ? "On" : "Off";
  return face;
}

const FootswitchModel::PresetEntry* FootswitchModel::presetAtSlot(int slot) const {
  for (const auto& preset : presets)
    if (preset.slot == slot)
      return &preset;
  return nullptr;
}

const FootswitchModel::PresetEntry* FootswitchModel::nextPreset() const {
  if (presetSlot <= 0)
    return nullptr;
  const PresetEntry* next = nullptr;
  for (const auto& preset : presets)
    if (preset.slot > presetSlot && (next == nullptr || preset.slot < next->slot))
      next = &preset;
  return next;
}

juce::String FootswitchModel::setlistLine() const {
  juce::String line = setlistName;
  if (presetSlot > 0)
    line << " " << type::dot() << " " << slotNumber(presetSlot);
  return line;
}

int FootswitchModel::presetsPerBank() const {
  if (mode == "preset")
    return 8;
  if (mode == "hybrid")
    return ((hybridTop == "preset" ? 1 : 0) + (hybridBottom == "preset" ? 1 : 0)) * 4;
  return 0;
}

juce::String FootswitchModel::bankLine() const {
  // The preset mode's bank: the current one, or the first when it is not in use.
  const int perBank = 8;
  const int shown = mode == "preset" ? bank : 0;
  const int first = shown * perBank + 1;
  return "Bank " + juce::String(shown + 1) + " " + type::dot() + " presets " + slotNumber(first) + "-" +
         slotNumber(first + perBank - 1);
}

std::array<SwitchFace, 8> FootswitchModel::faces(const juce::String& forMode) const {
  std::array<SwitchFace, 8> out;
  // A mode other than the current one shows its first page.
  const bool current = forMode == mode;
  const int pageBank = current ? bank : 0;
  const int pageScene = current ? scenePage : 0;

  const auto presetFace = [&](int index, int slotIndex) {
    SwitchFace face;
    face.letter = letterOf(index);
    face.number = slotNumber(slotIndex + 1);
    if (const PresetEntry* preset = presetAtSlot(slotIndex + 1)) {
      face.name = preset->name;
      face.look = preset->id == presetId ? Footswitch::Look::active : Footswitch::Look::off;
    } else {
      face.name = "empty";
      face.look = Footswitch::Look::empty;
    }
    return face;
  };
  const auto sceneFace = [&](int index, int scene) {
    SwitchFace face;
    face.letter = letterOf(index);
    face.name = sceneNames[static_cast<size_t>(scene)];
    const bool active = activeScene == letterOf(scene);
    face.look = active ? Footswitch::Look::active : Footswitch::Look::off;
    if (active)
      face.sub = "Active";
    return face;
  };

  for (int i = 0; i < 8; ++i) {
    SwitchFace& face = out[static_cast<size_t>(i)];
    if (forMode == "preset") {
      face = presetFace(i, pageBank * 8 + i);
    } else if (forMode == "scene") {
      face = sceneFace(i, i);
    } else if (forMode == "stomp") {
      face = stompFace(i);
    } else {
      // Hybrid: each row does what footswitch.setHybridRows says.
      const juce::String& row = i < 4 ? hybridTop : hybridBottom;
      const int column = i % 4;
      const bool same = hybridTop == hybridBottom;
      if (row == "stomp")
        face = stompFace(i);
      else if (row == "scene")
        face = sceneFace(i, same ? i : pageScene * 4 + column);
      else
        face = presetFace(i, same ? pageBank * 8 + i : pageBank * 4 + column);
    }
  }
  return out;
}

}  // namespace pedaleira::ui

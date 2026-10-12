#include "LibraryModel.h"

namespace pedaleira::ui {

const SetlistInfo* LibraryModel::setlist(const juce::String& id) const {
  for (const auto& s : setlists)
    if (s.id == id)
      return &s;
  return nullptr;
}

const PresetEntry* LibraryModel::find(const juce::String& id) const {
  if (id.isEmpty())
    return nullptr;
  // The current setlist first: a preset in two setlists is shown where the
  // player is.
  for (const auto& p : presets)
    if (p.id == id && p.setlistId == currentSetlist)
      return &p;
  for (const auto& p : presets)
    if (p.id == id)
      return &p;
  return nullptr;
}

const PresetEntry* LibraryModel::at(const juce::String& setlistId, int slot) const {
  for (const auto& p : presets)
    if (p.setlistId == setlistId && p.slot == slot)
      return &p;
  return nullptr;
}

int LibraryModel::userPresetCount() const {
  int total = 0;
  for (const auto& s : setlists)
    if (!s.readOnly)
      total += s.count;
  return total;
}

int LibraryModel::favouriteCount() const {
  int total = 0;
  for (const auto& p : presets)
    if (p.favourite)
      ++total;
  return total;
}

LibraryModel LibraryModel::read(EngineClient& engine) {
  LibraryModel model;
  const juce::var library = engine.read("library.get");
  model.root = library["root"].toString();
  model.currentSetlist = library["current"]["setlist"].toString();
  model.currentSlot = library["current"]["slot"];
  model.loadedId = library["current"]["presetId"].toString();
  model.unsaved = static_cast<bool>(library["unsaved"]);
  if (const auto* list = library["favourites"].getArray())
    for (const auto& id : *list)
      model.favourites.add(id.toString());
  if (const auto* list = library["recents"].getArray())
    for (const auto& recent : *list)
      model.recents.add(recent["id"].toString());

  if (const auto* list = library["setlists"].getArray())
    for (const auto& item : *list) {
      SetlistInfo info;
      info.id = item["id"].toString();
      info.name = item["name"].toString();
      info.readOnly = static_cast<bool>(item["readOnly"]);
      info.count = item["presetCount"];
      model.setlists.push_back(info);
    }

  for (auto& info : model.setlists) {
    const juce::var setlist = engine.read("setlist.get", object({{"setlist", info.id}}));
    info.folder = setlist["folder"].toString();
    if (!setlist["capacity"].isVoid())
      info.capacity = setlist["capacity"];
    const auto* slots = setlist["slots"].getArray();
    if (slots == nullptr)
      continue;
    info.count = slots->size();
    for (const auto& slot : *slots) {
      PresetEntry entry;
      entry.id = slot["id"].toString();
      entry.name = slot["name"].toString();
      entry.setlistId = info.id;
      entry.setlistName = info.name;
      entry.slot = slot["slot"];
      entry.factory = info.readOnly;
      entry.favourite = static_cast<bool>(slot["favourite"]);
      entry.sceneCount = slot["sceneCount"].isVoid() ? 0 : static_cast<int>(slot["sceneCount"]);
      entry.tempo = slot["tempo"].isVoid() ? 0.0 : static_cast<double>(slot["tempo"]);
      if (const auto* tags = slot["tags"].getArray())
        for (const auto& tag : *tags)
          entry.tags.add(tag.toString());
      entry.file = slot["file"].toString();
      if (info.folder.isNotEmpty() && entry.file.isNotEmpty())
        entry.path = juce::File(info.folder).getChildFile(entry.file).getFullPathName();
      model.presets.push_back(entry);
    }
  }
  return model;
}

}  // namespace pedaleira::ui

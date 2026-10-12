// What the library page draws, read from the engine in one pass
// (library.get and setlist.get for each setlist) and kept as plain values.
#pragma once

#include "../EngineClient.h"
#include "../Tokens.h"

#include <juce_core/juce_core.h>

#include <vector>

namespace pedaleira::ui {

struct SetlistInfo {
  juce::String id, name;
  bool readOnly = false;
  int count = 0;
  int capacity = 256;
  // The folder on disk; empty for the factory presets.
  juce::String folder;
};

struct PresetEntry {
  juce::String id, name;
  juce::String setlistId, setlistName;
  int slot = 0;
  bool factory = false;
  bool favourite = false;
  // The stored scene count and tempo; zero when the preset does not say.
  int sceneCount = 0;
  double tempo = 0.0;
  juce::StringArray tags;
  juce::String file;
  // The path of the file on disk, when it is a file of the user's.
  juce::String path;
};

struct LibraryModel {
  std::vector<SetlistInfo> setlists;
  // Every preset, by setlist in the order of setlists, then by slot.
  std::vector<PresetEntry> presets;
  // Ids of the recently loaded, most recent first.
  juce::StringArray recents;
  juce::StringArray favourites;
  juce::String root;
  juce::String currentSetlist;
  int currentSlot = 0;
  juce::String loadedId;
  bool unsaved = false;

  const SetlistInfo* setlist(const juce::String& id) const;
  const PresetEntry* find(const juce::String& id) const;
  const PresetEntry* at(const juce::String& setlistId, int slot) const;
  // The presets of the user's setlists (the factory ones left out).
  int userPresetCount() const;
  int favouriteCount() const;

  static LibraryModel read(EngineClient& engine);
};

}  // namespace pedaleira::ui

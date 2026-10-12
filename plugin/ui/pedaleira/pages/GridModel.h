// What the grid page draws, read from the engine in one pass (state.get and
// stomp.get) and kept as plain values, so the views paint from data and not
// from JSON.
#pragma once

#include "../EngineClient.h"
#include "../Tokens.h"

#include <juce_core/juce_core.h>

#include <array>
#include <map>
#include <optional>
#include <vector>

namespace pedaleira::ui {

struct GridSlot {
  juce::String blockId;
  bool empty = true;
  juce::String name;
  Category category = Category::utility;
  juce::String kindId;
  bool enabled = true;
  bool loading = false;
  bool missing = false;
  // Run: in a routed row and, for a split's lower row, inside its range.
  bool active = false;
  // The footswitch that flips it in stomp mode, "A" to "H", or empty.
  juce::String footswitch;
  juce::StringArray sceneParams;
};

struct GridRow {
  int index = 0;
  bool routed = false;
  // device, split, rows or none.
  juce::String source;
  // device, row, mix or none.
  juce::String sink;
  juce::String inputName;
  juce::String outputName;
  // The row's own input level on top of the rig's (row.setInputStage).
  double inputTrimDb = 0.0;
  double outputVolumeDb = 0.0;
  bool outputMute = false;
  bool outputSolo = false;
  std::array<GridSlot, 8> slots;
};

struct GridSplit {
  juce::String id;
  int row = 0;
  int lowerRow = 1;
  int splitAt = 0;
  // Absent: the lower path does not rejoin.
  std::optional<int> mixAt;
  juce::String type;
};

struct GridScene {
  juce::String id;
  juce::String name;
  bool active = false;
  // The scene differs from the defaults: it has a name of its own.
  bool named = false;
};

struct GridModel {
  bool gridMode = false;
  // A legacy rig with nothing in it (a fresh one): shown as an empty grid.
  bool legacyEmpty = false;
  std::array<GridRow, 4> rows;
  std::vector<GridSplit> splits;
  std::vector<GridScene> scenes;
  juce::String activeScene;
  // preset, scene, stomp or hybrid: what the footswitches do now.
  juce::String footswitchMode;

  // The rows the page shows: routed rows, and a split's lower row.
  std::vector<int> visibleRows() const;
  const GridSplit* splitOf(int row) const;
  const GridSplit* splitInto(int lowerRow) const;
  const GridSlot* find(const juce::String& blockId, int* rowOut = nullptr, int* slotOut = nullptr) const;

  static GridModel read(EngineClient& engine);
};

}  // namespace pedaleira::ui

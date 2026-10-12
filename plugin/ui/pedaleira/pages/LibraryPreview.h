// What the library page's details panel shows of a preset: the grid as a
// thumbnail (one square per slot, in its category's colour), the scene names,
// the tempo and what the preset uses. Read from the rig for the preset that
// is loaded (it may be edited), else from the preset's file.
#pragma once

#include "../EngineClient.h"
#include "../Tokens.h"

#include <juce_core/juce_core.h>

#include <array>
#include <optional>

namespace pedaleira::ui {

struct PresetPreview {
  bool available = false;
  // 4 rows of 8 slots: the block's category, or nothing for an empty slot.
  std::array<std::array<std::optional<Category>, 8>, 4> cells;
  // The scenes the preset uses, as { letter, name }.
  juce::StringArray sceneKeys, sceneNames;
  int captures = 0;
  int irs = 0;
  int effects = 0;
  juce::StringArray plugins;
  // Zero when the preset does not say.
  double tempo = 0.0;

  // "3 NAM captures, 2 IRs, 1 plugin (Seventh Heaven)", or empty.
  juce::String usesLine() const;

  // The rig as it is now.
  static PresetPreview fromRig(EngineClient& engine);
  // A preset file of the library; unavailable when it cannot be read.
  static PresetPreview fromFile(const juce::String& path);
};

}  // namespace pedaleira::ui

// What the footswitch views draw (the boards "Footswitch modes", "Stomp
// assignment" and "Gig View"), read from the engine in one state.get and
// kept as plain values: the eight switches as each mode shows them, the
// preset list, the scenes and the stomp assignments.
//
// The engine decides what a switch does (footswitch.press); this only
// mirrors its rules (ProcessorScenes.cpp, footswitchRole) so a mode can be
// previewed without being the current one.
#pragma once

#include "../EngineClient.h"
#include "../Tokens.h"
#include "../widgets/Tile.h"

#include <juce_core/juce_core.h>

#include <array>
#include <map>
#include <vector>

namespace pedaleira::ui {

// One switch as the boards draw it.
struct SwitchFace {
  juce::String letter;
  juce::String name, sub, number;
  Footswitch::Look look = Footswitch::Look::off;
  juce::Colour ring = colour::iris;
};

// Sets a switch widget to a face.
void applyFace(Footswitch& widget, const SwitchFace& face);

struct FootswitchModel {
  struct PresetEntry {
    int slot = 0;
    juce::String id, name;
  };
  struct BlockFact {
    juce::String name;
    Category category = Category::utility;
    bool enabled = true;
  };
  struct Stomp {
    juce::String label;
    bool momentary = false;
    juce::StringArray blockIds;
  };

  // preset, scene, stomp or hybrid; the rows of a hybrid are scene, stomp
  // or preset.
  juce::String mode = "preset";
  juce::String hybridTop = "scene", hybridBottom = "stomp";
  juce::String activeScene = "A";
  int bank = 0;
  int bankCount = 0;
  int scenePage = 0;
  double bpm = 120.0;

  juce::String setlistName, presetId, presetName;
  // The active preset's place in the setlist, 0 for none.
  int presetSlot = 0;
  std::vector<PresetEntry> presets;
  std::array<juce::String, 8> sceneNames;
  std::array<Stomp, 8> stomps;
  std::map<juce::String, BlockFact> blocks;

  static FootswitchModel read(EngineClient& engine);

  // The eight switches A to H as that mode shows them.
  std::array<SwitchFace, 8> faces(const juce::String& forMode) const;
  // The stomp switch alone (the assignment panel).
  SwitchFace stompFace(int index) const;
  bool stompAssigned(int index) const;
  int stompsAssigned() const;
  // The blocks of a stomp switch that exist.
  juce::StringArray stompBlocks(int index) const;

  const PresetEntry* presetAtSlot(int slot) const;
  // The preset after the active one in the setlist, if there is one.
  const PresetEntry* nextPreset() const;
  // The setlist's name and the preset's slot: "Friday Covers, middle dot, 012".
  juce::String setlistLine() const;

  // How many presets a bank of the current mode holds (0: no preset banks).
  int presetsPerBank() const;
  // The preset mode's bank: "Bank 2, middle dot, presets 009-016".
  juce::String bankLine() const;

  static const char* letterOf(int index);
};

}  // namespace pedaleira::ui

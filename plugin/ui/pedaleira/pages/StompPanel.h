// The panel of stomp assignment (the board "Stomp assignment"), under the
// grid: the eight footswitches with the block each flips, and for the
// selected one its blocks, whether it is momentary and its label. Everything
// goes through the stomp commands.
#pragma once

#include "../EngineClient.h"
#include "../widgets/Controls.h"
#include "../widgets/Tile.h"
#include "EditorParts.h"
#include "FootswitchModel.h"
#include "GridModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>
#include <vector>

namespace pedaleira::ui {

class StompPanel : public juce::Component {
public:
  explicit StompPanel(EngineClient& engine);
  ~StompPanel() override;

  // Reads the assignments again.
  void refresh(const GridModel& model);
  // Picks the switch whose blocks are shown (0 for A).
  void selectSwitch(int index);
  int selectedSwitch() const { return selected; }
  // The blocks of the selected switch, to outline on the grid.
  juce::StringArray selectedBlocks() const;

  // The stomp assignments changed: the page reads the rig again.
  std::function<void()> onChanged;
  // The selection moved: the grid outlines the switch's blocks.
  std::function<void()> onSelectionChanged;
  std::function<void()> onOpenModes;

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  class RemoveButton;
  void rebuildMembers();
  void notifyChanged();
  void addBlockMenu();
  void commitLabel();

  EngineClient& engine;
  FootswitchModel model;
  GridModel grid;
  int selected = 0;

  std::array<std::unique_ptr<Footswitch>, 8> switches;
  struct Member {
    juce::String blockId, name, category;
    Category tint = Category::utility;
    std::unique_ptr<RemoveButton> remove;
  };
  std::vector<Member> members;
  int hiddenMembers = 0;

  PushButton modes{"Footswitch modes", PushButton::Kind::ghost};
  PushButton addBlock{"Add block"};
  SwitchRow momentary{"Momentary"};
  SwitchRow midi{"Send MIDI on press"};
  juce::TextEditor label;
  juce::Rectangle<int> detailBox, labelArea, memberArea, hintArea, midiNote;
};

}  // namespace pedaleira::ui

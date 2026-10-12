// The page of a block editor (the boards "Block editor, effect", "Block
// editor, NAM capture" and "Block editor, IR"): the scenes strip, the
// compact signal row of the block's row with "Back to grid", the block's
// header (name, on in this scene, footswitch, replace, A/B compare) and the
// editor of its kind under it. It shows over the grid page and goes back to
// it; everything it changes goes through the engine's commands.
#pragma once

#include "../EngineClient.h"
#include "../widgets/Controls.h"
#include "EditorParts.h"
#include "GridModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace pedaleira::ui {

class ScenesStrip;

// Row r of the rig as chips between its input and output: the blocks of the
// row in order, the open one outlined.
class SignalRow : public juce::Component {
public:
  void setRow(const GridRow& row, const juce::String& selectedBlock);
  // A chip was clicked: its block id.
  std::function<void(const juce::String& blockId)> onSelect;

  void paint(juce::Graphics& g) override;
  void mouseUp(const juce::MouseEvent& event) override;

private:
  struct Chip {
    juce::String blockId, name;
    Category category = Category::utility;
    bool enabled = true;
    bool selected = false;
  };
  juce::Rectangle<int> chipBounds(int index) const;
  int chipWidth() const;

  int row = 0;
  juce::String inputName, outputName;
  std::vector<Chip> chips;
};

class BlockEditorPage : public juce::Component, private juce::Timer {
public:
  explicit BlockEditorPage(EngineClient& engine);
  ~BlockEditorPage() override;

  // Opens the editor of a block of the model.
  void open(const GridModel& model, const juce::String& blockId);
  // The rig changed: reads the block again.
  void refresh(const GridModel& model);
  const juce::String& blockId() const { return block.blockId; }

  std::function<void()> onBack;
  // A chip of the signal row was chosen.
  std::function<void(const juce::String& blockId)> onOpenBlock;
  // Put another block or file in this block's slot: the library opens on
  // that row and slot (BlockPicker::Source of the files to start from).
  std::function<void(int row, int slot, int source)> onReplace;
  // The rig changed from here: the page reads it again.
  std::function<void()> onChanged;

  void paint(juce::Graphics& g) override;
  void resized() override;
  bool keyPressed(const juce::KeyPress& key) override;
  void visibilityChanged() override;

  // Testbed: the open editor's body.
  BlockEditorBody* currentBody() { return body.get(); }

private:
  class Header;
  void timerCallback() override;
  void makeBody();
  void refresh(const GridModel& model, juce::String blockId);
  bool read(const GridModel& model, juce::String blockId);

  EngineClient& engine;
  BlockContext block;
  std::unique_ptr<ScenesStrip> scenes;
  SignalRow signalRow;
  PushButton back{"Back to grid"};
  std::unique_ptr<Header> header;
  std::unique_ptr<BlockEditorBody> body;
  juce::String bodyKind;
  juce::TooltipWindow tooltips{this, 600};
};

}  // namespace pedaleira::ui

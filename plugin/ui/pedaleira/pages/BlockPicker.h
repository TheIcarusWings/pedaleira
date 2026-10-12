// The block library (the board "Block picker / library"): every built-in
// effect, the captures and IRs of the library and the scanned plugins in one
// list, filtered by category on the left, by source with the chips and by a
// search. Picking one adds it to the slot the picker was opened for.
#pragma once

#include "../EngineClient.h"
#include "../Tokens.h"
#include "../widgets/Controls.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace pedaleira::ui {

class BlockPicker : public juce::Component, private juce::TextEditor::Listener {
public:
  explicit BlockPicker(EngineClient& engine);

  // Reads the kinds, assets and plugins again and shows the picker for a
  // slot.
  void openFor(int row, int slot);

  // A block was added; its id.
  std::function<void(const juce::String& blockId)> onAdded;
  std::function<void()> onClose;

  void paint(juce::Graphics& g) override;
  void paintOverChildren(juce::Graphics& g) override;
  void resized() override;
  void mouseUp(const juce::MouseEvent& event) override;
  void mouseMove(const juce::MouseEvent& event) override;
  void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
  bool keyPressed(const juce::KeyPress& key) override;

  // Testbed: filter as a player would.
  void setFilter(const juce::String& category, int source, const juce::String& search);

  // The kinds of entry the chips filter by.
  enum Source { all = 0, builtIn, captures, irs, plugins };

private:
  struct Item {
    juce::String name;
    Category category = Category::utility;
    Source source = builtIn;
    juce::String badge;    // Built-in, Capture, IR, VST3, AU
    juce::String detail;   // gain, tags, vendor
    juce::String model;    // a capture's sizes
    juce::String id;       // the kind id, the file path or the plugin identifier
  };
  struct Row {
    bool heading = false;
    juce::String title, note;
    int item = -1;
  };

  void textEditorTextChanged(juce::TextEditor&) override;
  void rebuildRows();
  void add(const Item& item);
  juce::Rectangle<int> listArea() const;
  int rowAt(juce::Point<int> position) const;
  juce::Rectangle<int> categoryBounds(int index) const;
  juce::Rectangle<int> chipBounds(int index) const;

  EngineClient& engine;
  int targetRow = 0, targetSlot = 0;
  std::vector<Item> items;
  std::vector<Row> rows;
  // Empty for all categories.
  juce::String category;
  int source = all;
  int hover = -1;
  int scroll = 0;
  juce::TextEditor search;
  PushButton close{"Close"};
  // A capture or IR from anywhere on the computer, without adding its folder.
  PushButton loadFile{juce::String::fromUTF8("Load a file\xe2\x80\xa6")};
  std::unique_ptr<juce::FileChooser> chooser;
  void chooseFile();
};

}  // namespace pedaleira::ui

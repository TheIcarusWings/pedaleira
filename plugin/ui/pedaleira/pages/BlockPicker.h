// The block library (the board "Block picker / library"): every built-in
// effect, the captures and IRs of the library and the scanned plugins in one
// list, filtered by category on the left, by source with the chips and by a
// search. Picking one adds it to the slot the picker was opened for.
//
// Captures are listed by the name the player knows them by (the file name they
// were loaded from, else the name inside the file), whether they sit in the
// library, in a folder added in Settings, in the TONE3000 app's download
// folder or were loaded once from a file; Recent lists the last ones loaded,
// and a capture Pedaleira cannot load says why instead of failing. The
// TONE3000 chip browses TONE3000 itself (Tone3000Panel).
#pragma once

#include "../EngineClient.h"
#include "../Tokens.h"
#include "../widgets/Controls.h"
#include "Tone3000Feed.h"
#include "Tone3000Panel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace pedaleira::ui {

class BlockPicker : public juce::Component, private juce::TextEditor::Listener {
public:
  explicit BlockPicker(EngineClient& engine);
  ~BlockPicker() override;

  // Where the TONE3000 chip gets its tones; without one it says browsing is
  // not available. The feed outlives the picker.
  void setTone3000Feed(Tone3000Feed* feed);
  Tone3000Panel& tone3000Panel() { return tonePanel; }

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
  // Testbed and tests: what is listed, and what a click on a row does.
  juce::StringArray shownNames() const;
  int itemCount() const { return static_cast<int>(items.size()); }
  // Clicks the first listed row with this name; false when there is none.
  bool clickName(const juce::String& name);
  // The sentence shown over the list (a refused capture, a failed add); empty
  // when there is none.
  const juce::String& noticeText() const { return notice; }
  void showNotice(const juce::String& text);

  // The kinds of entry the chips filter by, in the order of the chips.
  enum Source { all = 0, builtIn, captures, irs, plugins, recent, tone3000 };

private:
  struct Item {
    juce::String name;
    Category category = Category::utility;
    Source source = builtIn;
    juce::String badge;    // Built-in, Capture, IR, VST3, AU
    juce::String detail;   // gain, tags, vendor
    juce::String model;    // a capture's sizes, or its architecture when it is not A2
    juce::String id;       // the kind id, the file path or the plugin identifier
    juce::String keywords; // everything search matches, lower case
    // A capture Pedaleira can not load, and the sentence that says why.
    bool unsupported = false;
    juce::String reason;
    // Place in the recent list (0 is the newest), or -1.
    int recentRank = -1;
    // Only in the recent list: its file is not one of the listed assets.
    bool recentOnly = false;
  };
  struct Row {
    bool heading = false;
    juce::String title, note;
    // The right end of a heading: how the section is ordered.
    juce::String right;
    int item = -1;
  };

  void textEditorTextChanged(juce::TextEditor&) override;
  void rebuildRows();
  // A click on a row: a capture Pedaleira can not load says why, the rest is added.
  void activate(const Item& item);
  bool add(const Item& item);
  void readAssets();
  void readRecent();
  void updateSourceViews();
  juce::Rectangle<int> noticeArea() const;
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
  // assets.list had more files than it was asked for.
  bool truncated = false;
  juce::TextEditor search;
  juce::String notice;
  Tone3000Panel tonePanel;
  PushButton close{"Close"};
  // A capture or IR from anywhere on the computer, without adding its folder.
  PushButton loadFile{juce::String::fromUTF8("Load a file\xe2\x80\xa6")};
  std::unique_ptr<juce::FileChooser> chooser;
  void chooseFile();
};

}  // namespace pedaleira::ui

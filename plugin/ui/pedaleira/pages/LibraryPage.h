// The preset library (the board "Library"): the setlists and the views over
// all of them on the left, the presets of the chosen one in slot order in the
// middle (empty slots included), and the details of the chosen preset on the
// right. Everything goes through preset.*, setlist.*, favourite.set and
// library.get; the page keeps no state of its own beyond what it shows.
#pragma once

#include "../EngineClient.h"
#include "../widgets/Controls.h"
#include "../widgets/Dialog.h"
#include "LibraryModel.h"
#include "LibraryPreview.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace pedaleira::ui {

class LibraryPage : public juce::Component, private juce::TextEditor::Listener {
public:
  explicit LibraryPage(EngineClient& engine);
  ~LibraryPage() override;

  // Reads the library again and rebuilds the list; the selection stays on
  // the same preset when it is still there.
  void refresh();

  // The loaded preset or the library changed: the shell's header reads it
  // again.
  std::function<void()> onChanged;
  // A preset double-clicked and loaded: show it on the grid.
  std::function<void()> onOpenGrid;

  // What the list shows: one setlist, or a view over all of them.
  enum class View { setlist, all, favourites, recent };
  enum class Sort { slot, name, tempo, recent };

  // Testbed and tests: drive the page as a player would.
  void showSetlist(const juce::String& setlistId);
  void showView(View view);
  void setSearch(const juce::String& text);
  void setSort(Sort sort);
  // Selects the row of a slot of the shown setlist (a preset or an empty one).
  void selectSlot(int slot);
  // Selects the row at an index of the visible list.
  void selectRow(int index);
  // What the menus and the dialogs do, public so a test can run them without
  // opening a menu. The selected preset is the one acted on.
  void newSetlist();
  void renameSetlistDialog(const juce::String& setlistId);
  void deleteSetlistDialog(const juce::String& setlistId);
  void reorderSetlist(const juce::String& setlistId, int index);
  void useSetlist(const juce::String& setlistId);
  void moveSelected(int toSlot);
  void copySelectedTo(const juce::String& setlistId);
  // The sidebar's setlist ids in order, and the rows' preset ids ("" for an
  // empty slot).
  juce::StringArray setlistIds() const;
  juce::StringArray rowIds() const;
  Dialog& dialog() { return prompt; }
  // The action bar's buttons by label, for tests ("Delete" opens the dialog).
  bool pressAction(const juce::String& label);

  void paint(juce::Graphics& g) override;
  void resized() override;
  void mouseDown(const juce::MouseEvent& event) override;
  void mouseDoubleClick(const juce::MouseEvent& event) override;
  void mouseMove(const juce::MouseEvent& event) override;
  void mouseExit(const juce::MouseEvent& event) override;
  void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
  bool keyPressed(const juce::KeyPress& key) override;

private:
  struct Row {
    // Null for an empty slot.
    const PresetEntry* entry = nullptr;
    juce::String setlistId;
    int slot = 0;
    bool empty() const { return entry == nullptr; }
  };
  enum class Act { load, rename, duplicate, move, copyTo, remove, saveHere };
  struct Target {
    juce::String setlistId, id, name;
    int slot = 0;
    bool factory = false;
  };
  struct ActionButton {
    juce::String label;
    Act act;
    bool muted = false;
    juce::Rectangle<int> bounds;
  };

  void textEditorTextChanged(juce::TextEditor&) override;

  void rebuildRows();
  bool inSetlistView() const { return view == View::setlist; }
  const SetlistInfo* shownSetlist() const;
  int selectedIndex() const;
  const Row* selectedRow() const;
  Target targetOf(const Row& row) const;
  std::vector<ActionButton> actionsFor(const Row& row) const;
  void perform(Act act, const Target& target, juce::Rectangle<int> anchor);
  void moveMenu(const Target& target, juce::Rectangle<int> anchor);
  void copyMenu(const Target& target, juce::Rectangle<int> anchor);
  void setlistMenu(const juce::String& setlistId, juce::Rectangle<int> anchor);
  void movePreset(const Target& target, int to);
  void copyPreset(const Target& target, const juce::String& setlistId);
  void toggleFavourite(const Row& row);
  void fail(const juce::String& title, const EngineReply& reply);
  // Runs a command; on a failure shows why and returns false.
  bool run(const juce::String& title, const juce::String& command, const juce::var& args,
           EngineReply* reply = nullptr);
  void changed();
  void updatePreview();
  void select(const juce::String& setlistId, int slot);
  void ensureVisible(int rowIndex);
  void sortMenu();

  // Geometry.
  juce::Rectangle<int> sidebarArea() const;
  juce::Rectangle<int> mainArea() const;
  juce::Rectangle<int> detailArea() const;
  juce::Rectangle<int> libraryItemBounds(int index) const;
  juce::Rectangle<int> setlistBounds(int index) const;
  juce::Rectangle<int> setlistsViewport() const;
  juce::Rectangle<int> tableViewport() const;
  int rowHeight(int index) const;
  int rowTop(int index) const;
  int contentHeight() const;
  int rowAt(juce::Point<int> position) const;
  struct Columns {
    juce::Rectangle<int> dot, number, name, scenes, tempo, tags, star;
  };
  Columns columnsOf(juce::Rectangle<int> row) const;
  juce::Rectangle<int> starBounds(int index) const;

  void paintSidebar(juce::Graphics& g);
  void paintMain(juce::Graphics& g);
  void paintRow(juce::Graphics& g, int index);
  void paintDetails(juce::Graphics& g);

  EngineClient& engine;
  LibraryModel model;
  View view = View::setlist;
  juce::String viewSetlist;
  Sort sort = Sort::slot;
  std::vector<Row> rows;
  juce::String selectedSetlist;
  int selectedSlot = 0;
  PresetPreview preview;

  int tableScroll = 0;
  int setlistScroll = 0;
  int hoverAction = -1;
  juce::Point<int> hoverPoint{-1, -1};

  juce::TextEditor search;
  SelectBox sortBox{"Sort by"};
  PushButton newSetlistButton{"New setlist"};
  PushButton setlistActions{"Setlist actions", PushButton::Kind::ghost};
  PushButton save{"Save", PushButton::Kind::primary};
  PushButton saveAs{"Save as..."};
  PushButton revert{"Revert", PushButton::Kind::ghost};
  PushButton loadButton{"Load preset", PushButton::Kind::primary};
  Dialog prompt;
};

}  // namespace pedaleira::ui

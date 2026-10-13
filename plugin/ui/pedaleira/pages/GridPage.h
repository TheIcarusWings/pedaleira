// The grid editor (the boards "Grid editor" and "Add a split"): the scenes
// strip, the rows with their inputs, slots, cables, split and mix points
// and outputs, and below them the panel of what is selected: a block with
// its knobs and per-scene values, or a splitter or mixer with its own.
// Everything it shows comes from GridModel; everything it changes goes
// through the engine's commands.
#pragma once

#include "../widgets/Dialog.h"
#include "../EngineClient.h"
#include "../widgets/Controls.h"
#include "../widgets/Knob.h"
#include "../widgets/Tile.h"
#include "BlockEditorPage.h"
#include "BlockPicker.h"
#include "GridModel.h"
#include "ParamKnobs.h"
#include "ScenePanel.h"
#include "StompPanel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace pedaleira::ui {

// The scenes the preset uses, then "Add scene" while there is room for one
// more (a preset starts with one and can have eight).
class ScenesStrip : public juce::Component {
public:
  void setScenes(const std::vector<GridScene>& scenes);
  std::function<void(const juce::String&)> onSelect;
  std::function<void()> onAdd;
  // Right-click on a scene: the page offers to remove it.
  std::function<void(const juce::String&)> onRemove;

  void paint(juce::Graphics& g) override;
  void mouseUp(const juce::MouseEvent& event) override;
  void mouseMove(const juce::MouseEvent& event) override;
  void mouseExit(const juce::MouseEvent& event) override;

private:
  juce::Rectangle<float> buttonBounds(int index) const;
  std::vector<GridScene> scenes;
  // The button under the mouse (Add scene is the one after the scenes).
  int hovered = -1;
};

// A split or mix point: the split's upper row and which of its two points.
struct GridPoint {
  int row = 0;
  bool mix = false;
  bool operator==(const GridPoint& other) const { return row == other.row && mix == other.mix; }
};

class GridView : public juce::Component, private juce::Timer {
public:
  GridView();
  ~GridView() override;

  void setModel(const GridModel& model, const juce::String& selectedBlock, std::optional<GridPoint> selectedPoint);
  // Blocks outlined as selected besides the selected one (a stomp switch's).
  void setHighlighted(const juce::StringArray& blockIds) { highlighted = blockIds; }

  // What entered each row and what it sent, [left, right] in dB (meters.get).
  struct RowLevels {
    std::array<float, 2> in{-60.0f, -60.0f};
    std::array<float, 2> out{-60.0f, -60.0f};
  };
  void setLevels(const std::array<RowLevels, 4>& levels);

  std::function<void(const juce::String& blockId)> onSelectBlock;
  std::function<void(int row, int slot)> onEmptySlot;
  std::function<void(GridPoint)> onSelectPoint;
  // A point dragged to another boundary.
  std::function<void(GridPoint, int boundary)> onMovePoint;
  // A block dropped on a slot (swap), or on the free row below (a split).
  std::function<void(const juce::String& blockId, int row, int slot)> onMoveBlock;
  // The cable menu: split the row at a boundary, or add a block there.
  std::function<void(int row, int boundary)> onSplitHere;
  std::function<void(int row, int slot)> onInsertHere;
  std::function<void(const juce::String& blockId)> onDuplicate;
  std::function<void(const juce::String& blockId)> onRemove;
  // A block is being dragged (or the drag ended), and whether it is off the
  // grid, where a drop removes it.
  std::function<void(bool dragging, bool offGrid)> onTileDrag;
  std::function<void()> onAddRow;
  // A row's input box or output name clicked: the page offers the choices,
  // shown at `at` (in this view's coordinates).
  std::function<void(int row, juce::Rectangle<int> at)> onRowInput;
  std::function<void(int row, juce::Rectangle<int> at)> onRowOutput;
  // A tile's power switch: the block on or off (in the active scene).
  std::function<void(const juce::String& blockId, bool enabled)> onToggleBlock;
  // A row's input trim dragged on its meter, and its send changed in the
  // output box (volume dragged on the fader, mute or solo clicked): the
  // fields of row.setInputStage and row.setMix.
  std::function<void(int row, double trimDb)> onRowTrim;
  std::function<void(int row, const juce::var& mix)> onRowMix;
  // A tile double-clicked: open its editor.
  std::function<void(const juce::String& blockId)> onOpenBlock;

  void paint(juce::Graphics& g) override;
  void paintOverChildren(juce::Graphics& g) override;
  void resized() override;
  void mouseMove(const juce::MouseEvent& event) override;
  void mouseExit(const juce::MouseEvent& event) override;
  void mouseDown(const juce::MouseEvent& event) override;
  void mouseDrag(const juce::MouseEvent& event) override;
  void mouseUp(const juce::MouseEvent& event) override;
  bool keyPressed(const juce::KeyPress& key) override;

  // Layout, from the board.
  static constexpr int kRowTop = 32;
  static constexpr int kRowPitch = 104;
  static constexpr int kInputX = 24;
  static constexpr int kInputWidth = 100;
  static constexpr int kFirstSlotX = 152;
  static constexpr int kSlotPitch = 124;
  static constexpr int kSlotWidth = 96;
  static constexpr int kSlotHeight = 76;
  static constexpr int kOutputWidth = 200;

private:
  struct Cable {
    int row = -1;
    int boundary = 0;
  };
  int rowTop(int visibleIndex) const { return kRowTop + visibleIndex * kRowPitch; }
  int visibleIndexOf(int row) const;
  // The x of the boundary before slot `boundary` (0 to 8): the middle of the
  // gap, where split and mix points sit.
  static float boundaryX(int boundary) { return kFirstSlotX + boundary * kSlotPitch - (kSlotPitch - kSlotWidth) * 0.5f; }
  juce::Rectangle<int> addRowBounds() const;
  std::optional<GridPoint> pointAt(juce::Point<float> position) const;
  juce::Point<float> pointPosition(GridPoint point) const;
  // The boundaries a point may move to.
  std::vector<int> legalBoundaries(GridPoint point) const;
  Cable cableAt(juce::Point<float> position) const;
  // The slot under a position, as row and slot.
  std::optional<std::pair<int, int>> slotAt(juce::Point<int> position) const;
  void showCableMenu(Cable cable);
  // The row whose input box, or output name line, is under a position.
  std::optional<int> rowInputAt(juce::Point<int> position, juce::Rectangle<int>* box) const;
  std::optional<int> rowOutputAt(juce::Point<int> position, juce::Rectangle<int>* box) const;
  // The parts of a row's boxes, by visible index: the input meter (drag for
  // trim), the output meter, the volume fader and the mute (0) and solo (1)
  // buttons.
  juce::Rectangle<float> inputMeterArea(int visibleIndex) const;
  juce::Rectangle<float> outputMeterArea(int visibleIndex) const;
  juce::Rectangle<float> faderArea(int visibleIndex) const;
  juce::Rectangle<float> muteSoloBox(int visibleIndex, int which) const;
  // The level controls under a position: a row and what was hit.
  enum class LevelPart { none, trim, volume, mute, solo };
  std::pair<int, LevelPart> levelPartAt(juce::Point<float> position) const;
  void paintMeterPair(juce::Graphics& g, juce::Rectangle<float> area, const std::array<float, 2>& db,
                      const std::array<float, 2>& hold, bool clipped) const;
  void tileMouseDown(int row, int slot, const juce::MouseEvent& event);
  void tileMouseDrag(int row, int slot, const juce::MouseEvent& event);
  void tileMouseUp(int row, int slot, const juce::MouseEvent& event);
  void cancelDrags();

  GridModel model;
  std::vector<int> shown;
  // What the meters show, what the engine last read, and the peak holds.
  // The shown levels rise at once and fall smoothly between readings.
  std::array<RowLevels, 4> levels, targets, holds;
  void timerCallback() override;
  // The part of a row's boxes under the mouse, lit to say it can be used.
  enum class HoverPart { none, input, output, trim, volume, mute, solo, addRow };
  struct Hover {
    int row = -1;
    HoverPart part = HoverPart::none;
    bool operator!=(const Hover& other) const { return row != other.row || part != other.part; }
  };
  Hover hoverAt(juce::Point<float> position) const;
  Hover hovering;
  // Until when each row's input (0) and output (1) shows a clip.
  std::array<std::array<juce::uint32, 2>, 4> clipUntil{};
  // A trim or volume drag: the row, which, the value and x it started at.
  struct LevelDrag {
    int row = 0;
    LevelPart part = LevelPart::none;
    double startDb = 0.0;
    float startX = 0.0f;
  };
  std::optional<LevelDrag> levelDrag;
  std::optional<GridPoint> selectedPoint;
  juce::StringArray highlighted;
  std::array<std::array<std::unique_ptr<Tile>, 8>, 4> tiles;

  Cable hoverCable;
  std::optional<GridPoint> draggingPoint;
  juce::Point<float> dragPosition;
  // A block on the move: where it came from and where the mouse is.
  std::optional<std::pair<int, int>> draggingTile;
  juce::Point<int> tileDragPosition;

  struct TileMouse;
  std::vector<std::unique_ptr<TileMouse>> tileMice;
};

class BlockPanel : public juce::Component {
public:
  explicit BlockPanel(EngineClient& engine);

  // Reads the block's parameters again (after a selection or a change).
  void showBlock(const GridModel& model, const juce::String& blockId);

  std::function<void(const juce::String& blockId)> onOpenEditor;
  std::function<void(const juce::String& blockId)> onReplace;
  std::function<void()> onChanged;

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  void showSceneValues(int paramIndex);
  void openKnobMenu(int paramIndex);

  EngineClient& engine;
  juce::String blockId;
  juce::String title, subtitle, activeScene;
  Category category = Category::utility;
  bool hasBlock = false;
  std::vector<KnobParam> params;
  std::vector<std::unique_ptr<Knob>> knobs;
  int sceneParam = -1;
  std::vector<std::pair<juce::String, double>> sceneValues;

  Toggle enabled{"Block on"};
  PushButton assignFootswitch{"Assign footswitch"};
  PushButton replace{"Replace"};
  PushButton openEditor{"Open editor"};
  juce::Rectangle<int> sceneBox;
};

// The splitter or the mixer of a split (the boards "Splitter selected" and
// "Mixer selected").
class SplitPanel : public juce::Component {
public:
  explicit SplitPanel(EngineClient& engine);

  void showPoint(const GridModel& model, GridPoint point);
  std::function<void()> onChanged;

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  void addKnob(const juce::String& id, const juce::String& name, int group);
  void setParam(const juce::String& id, double value);
  juce::String blocksOf(const GridModel& model, int row, int from, int to) const;

  EngineClient& engine;
  GridPoint point;
  juce::String title, subtitle, type;
  std::vector<KnobParam> specs;
  struct Placed {
    std::unique_ptr<Knob> knob;
    KnobParam param;
    int group = 0;
  };
  std::vector<Placed> knobs;
  juce::String pathA, pathB;
  SegmentedControl typeChoice{{"Balance", "A/B", "Crossover"}};
  Toggle muteToggle{"Mute"};
  Toggle secondToggle{"Second switch"};
  juce::String secondLabel;
  PushButton remove{"Remove split"};
  std::array<juce::Rectangle<int>, 3> groups;
};

// Shown under the grid while a block is dragged: dropping it there (or
// anywhere off the grid) removes it.
class RemoveDock : public juce::Component {
public:
  RemoveDock() { setInterceptsMouseClicks(false, false); }
  void setArmed(bool shouldBeArmed);
  void paint(juce::Graphics& g) override;

private:
  bool armed = false;
};

class GridPage : public juce::Component {
public:
  explicit GridPage(EngineClient& engine);

  // The rig changed: read it again.
  void rigChanged();
  // Starts an empty preset (chain.reset), asking first when the current one
  // has unsaved changes.
  void newPreset(bool askFirst);
  // The row meters, from the shell's one meters.get reading per tick (each
  // reading takes the engine's lock, so the pages share it).
  void showMeters(const juce::var& meters);
  void selectBlock(const juce::String& blockId);
  void selectPoint(GridPoint point);
  // The block library over the page, adding to that slot.
  void openPicker(int row, int slot);
  BlockPicker& picker() { return library; }
  const GridModel& currentModel() const { return model; }
  // The editor of a block over the page (NAM capture, IR or built-in
  // effect), with "Back to grid" to return.
  void openEditor(const juce::String& blockId);
  void closeEditor();
  bool isEditorOpen() const { return editorShown; }
  BlockEditorPage& blockEditor() { return editor; }

  // What the panel under the grid shows: the selected block or split, the
  // active scene, or the stomp assignments. Clicking a block shows the first,
  // a scene in the strip the second, and the footswitch mode follows: Scene
  // mode shows the scene, Stomp mode the assignments.
  enum class Panel { selection, scene, stomp };
  void showPanel(Panel panel);
  Panel currentPanel() const { return panel; }
  StompPanel& stompView() { return stompPanel; }
  ScenePanel& sceneView() { return scenePanel; }

  // "Footswitch modes" on the scene and stomp panels.
  std::function<void()> onOpenModes;

  // A hosted plugin's block has its own window, which is another page's
  // job: the editor of such a block is asked for here first, with the
  // window's title. False (no plugin editor) opens the generated one.
  std::function<bool(const juce::String& blockId, const juce::String& title)> onOpenPluginWindow;

  // The audio device as DeviceSettings::state() gives it (inputChannels,
  // outputPairs), for the row input and output menus. Unset (the plugin, the
  // testbed): the menus offer eight inputs and four output pairs.
  std::function<juce::var()> deviceState;

  // The row menus, public for the testbed.
  void showRowInputMenu(int row, juce::Rectangle<int> at);
  void showRowOutputMenu(int row, juce::Rectangle<int> at);
  // Asks first, then removes the row's blocks and stops it running.
  void removeRow(int row);
  Dialog& dialog() { return prompt; }

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  // Converts a legacy rig to the grid before the first edit.
  void ensureGrid();
  void refreshViews();
  void setEditorShown(bool shown);
  void requestEditor(const juce::String& blockId);

  EngineClient& engine;
  GridModel model;
  juce::String selected;
  std::optional<GridPoint> point;
  ScenesStrip scenes;
  GridView grid;
  BlockPanel blockPanel;
  SplitPanel splitPanel;
  ScenePanel scenePanel;
  StompPanel stompPanel;
  Panel panel = Panel::selection;
  juce::String seenMode;
  BlockEditorPage editor;
  BlockPicker library;
  RemoveDock removeDock;
  Dialog prompt;
  bool editorShown = false;

  // Runs a routing command; a refusal is shown with the engine's reason.
  void runRouting(const char* command, const juce::var& args, const juce::String& what);

};

}  // namespace pedaleira::ui

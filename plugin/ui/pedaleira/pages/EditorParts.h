// What the three block editors share (the boards "Block editor, effect",
// "Block editor, NAM capture" and "Block editor, IR"): the section box, a
// knob bound to a block parameter with its right-click menu, a labelled
// switch, and the base of an editor's body.
#pragma once

#include "../EngineClient.h"
#include "../Tokens.h"
#include "../widgets/Controls.h"
#include "../widgets/Knob.h"
#include "ParamKnobs.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace pedaleira::ui {

// The block an editor shows: what the grid page knows of it and its entry
// in state.get (tone, params, sceneParams, expressionParams).
struct BlockContext {
  juce::String blockId;
  juce::String name;
  // nam, ir, plugin or an effect's kind id.
  juce::String kindId;
  juce::String activeScene;
  Category category = Category::utility;
  int row = 0;
  int slot = 0;
  bool enabled = true;
  juce::var block;
  juce::StringArray sceneParams;
  juce::StringArray expressionParams;
};

// A section of an editor: the ground box with its line border and the label
// in caps. Returns the area under the label for the content; the optional
// note sits after the label, muted.
juce::Rectangle<int> paintSection(juce::Graphics& g, juce::Rectangle<int> box, const juce::String& title,
                                  const juce::String& note = {});

// The footswitch menu of a block (the header's Assign footswitch).
void showFootswitchMenu(EngineClient& engine, const juce::String& blockId, juce::Component& target,
                        std::function<void()> done);

// A knob bound to one parameter of a block: turning it runs block.setParam,
// right-click opens the menu of the board (assign to scenes, to the
// expression pedal, reset to default).
class ParamKnob : public Knob {
public:
  ParamKnob(EngineClient& engine, const juce::String& blockId, const KnobParam& param);

  // Reads the parameter again without moving a knob that is being dragged.
  void update(const KnobParam& param);
  const KnobParam& spec() const { return param; }

  // The text under the knob when it is not the plain value (a synced time).
  std::function<juce::String(const KnobParam&)> valueText;
  // The parameter was assigned or unassigned: the owner reads the block again.
  std::function<void()> onAssignmentChanged;
  // The value moved by the player.
  std::function<void(const KnobParam&)> onValueChanged;

  // The size of the cell a knob of this diameter takes, tag line included.
  static juce::Point<int> cellSize(int diameter);

private:
  void openMenu();
  juce::String textOf() const;

  EngineClient& engine;
  juce::String blockId;
  KnobParam param;
};

// A switch with its label beside it, and optionally a mono value after the
// label (the tempo under "Tempo sync").
class SwitchRow : public juce::Component {
public:
  explicit SwitchRow(const juce::String& label);

  void setState(bool on);
  bool getState() const { return toggle.getToggleState(); }
  void setDetail(const juce::String& text);
  void setSwitchEnabled(bool enabled);
  std::function<void(bool)> onToggle;

  int idealWidth() const;
  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  Toggle toggle;
  juce::String label, detail;
};

// The base of an editor's body, the area under the shared header.
class BlockEditorBody : public juce::Component {
public:
  explicit BlockEditorBody(EngineClient& client) : engine(client) {}

  // Shows the block. The same block again (the rig moved on) updates the
  // controls in place, so a knob being dragged stays under the mouse.
  virtual void showBlock(const BlockContext& block) = 0;
  // Called about thirty times a second while the editor is open (meters).
  virtual void tick() {}

  // An edit that changes more than a value (an assignment, a swap): the
  // page reads the rig again.
  std::function<void()> onChanged;
  // The player asked for another file in this block's slot; the block
  // library opens on this source (BlockPicker::Source).
  std::function<void(int source)> onReplace;

protected:
  EngineClient& engine;
};

// The quiet text of an unfinished part of a board, in its place: "Comes in a
// later build".
extern const char* const kLaterBuild;

}  // namespace pedaleira::ui

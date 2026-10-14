// An amp block drawn as its faceplate (the canvas, "Amp panels: overview and
// the 15 amps"). The faceplate is the control surface: the knobs, the Bright
// and Voicing switches and the nameplate are the amp's parameters, drawn as
// its description says (block.getFaceplate: accent colour, nameplate
// position, the boxes the controls sit in and their order, the cap of the
// knobs, the strip pattern, the switch styles, a style tag and a character
// line). An amp whose description gives no style gets a plain default
// faceplate from the engine, so every amp has one. Everything the player
// turns goes through block.setParam, like the knobs of the other editors.
//
// Under the board runs the slim strip: the output Level knob and the live
// Sag the amp reports. Hovering or dragging a control shows its value in a
// bubble.
#pragma once

#include "BlockStyle.h"
#include "EditorParts.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace pedaleira::ui {

enum class FaceNameplate { left, right, top, bottom };
enum class FaceStrip { none, slash, dots, wave, cells, zigzag, chevron, bells, arcs, crosses, ticks, flames, rings, steps, diamonds, ripple };

struct FaceControl {
  juce::String id, label;
};

// One box of the faceplate: its title and its controls in order.
struct FaceGroup {
  juce::String title;
  // Columns for its knobs; 0 is one per knob.
  int columns = 0;
  std::vector<FaceControl> controls;
};

// What block.getFaceplate says, in the UI's own types.
struct FaceplateSpec {
  juce::String name, family, tag, character;
  juce::Colour accent;
  bool styled = false;
  FaceNameplate nameplate = FaceNameplate::left;
  Knob::Cap cap = Knob::Cap::round;
  FaceStrip strip = FaceStrip::none;
  juce::StringArray voicings;
  std::vector<FaceGroup> groups;

  // From the reply of block.getFaceplate; `fallbackAccent` is used when the
  // description names no colour.
  static FaceplateSpec fromVar(const juce::var& faceplate, juce::Colour fallbackAccent);
};

// A strip pattern drawn into an area (22 px tall in the design), in ink.
void paintFaceStrip(juce::Graphics& g, FaceStrip pattern, juce::Rectangle<float> area, juce::Colour ink);

// The Bright switch: a 40 x 22 toggle with its label beside it.
class FaceBrightSwitch : public juce::Button {
public:
  FaceBrightSwitch(const juce::String& label, juce::Colour accent);
  int idealWidth() const;

protected:
  void paintButton(juce::Graphics& g, bool highlighted, bool down) override;

private:
  juce::String label;
  juce::Colour accent;
};

// The Voicing switch (two or three voicings): a segmented I / II / III.
class FaceVoicingSwitch : public juce::Component, public juce::TooltipClient {
public:
  FaceVoicingSwitch(const juce::StringArray& names, juce::Colour accent);

  void setSelected(int index, juce::NotificationType notification = juce::dontSendNotification);
  int getSelected() const { return selected; }
  std::function<void(int)> onChange;

  int idealWidth() const;
  juce::String getTooltip() override;
  void paint(juce::Graphics& g) override;
  void mouseUp(const juce::MouseEvent& event) override;
  void mouseMove(const juce::MouseEvent& event) override;
  void mouseExit(const juce::MouseEvent& event) override;
  bool keyPressed(const juce::KeyPress& key) override;

private:
  juce::Rectangle<float> segment(int index) const;
  int segmentAt(juce::Point<float> point) const;
  std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

  juce::StringArray names;
  int selected = 0, hovered = -1;
};

class AmpFaceplate : public BlockEditorBody {
public:
  explicit AmpFaceplate(EngineClient& engine);
  ~AmpFaceplate() override;

  void showBlock(const BlockContext& block) override;
  // The sag the amp reports while it runs.
  void setLive(const BlockLive& live);

  // What it shows, for the tests.
  const FaceplateSpec& spec() const { return plate; }
  std::optional<double> shownValue(const juce::String& paramId) const;
  // The control for a parameter id (a knob, "bright" or "voicing"), or null.
  juce::Component* controlFor(const juce::String& paramId) const;
  // The knob under the pointer or being turned, with the text its bubble shows.
  juce::String bubbleText() const;
  int numKnobs() const { return static_cast<int>(knobs.size()); }
  juce::Rectangle<int> nameplateBounds() const { return plateBounds; }
  juce::Rectangle<int> stripBounds() const { return strip; }
  juce::Rectangle<int> groupBounds(int index) const;
  // The knob diameter the layout chose.
  int knobDiameter() const { return diameter; }
  // Whether the layout had to use its tighter gaps.
  bool isCompact() const { return compact; }

  void paint(juce::Graphics& g) override;
  void paintOverChildren(juce::Graphics& g) override;
  void resized() override;

  static constexpr int kStripHeight = 40;

private:
  // The gaps and heights the layout uses: the normal ones, and tighter ones
  // for a faceplate that has little room (a block settings panel under a
  // grid of many rows), so the knobs do not have to shrink to nothing.
  struct Metrics {
    int padY, banner, bannerGap, boxTop, title, titleGap, boxBottom, switchRow, switchGap;
  };
  struct Box {
    juce::String title;
    juce::Rectangle<int> bounds;
    std::vector<ParamKnob*> knobItems;
    std::vector<juce::Component*> switchItems;
    int columns = 0;
  };

  void rebuild(const std::vector<KnobParam>& specs);
  void updateValues(const std::vector<KnobParam>& specs);
  void setParam(const juce::String& id, double value);
  void noteValue(const juce::String& id, double value);
  const KnobParam* find(const juce::String& id) const;
  ParamKnob* makeKnob(const KnobParam& param, int diameter);
  void layoutBoard();
  int switchWidth(const juce::Component* item) const;
  void paintNameplate(juce::Graphics& g);
  void paintStrip(juce::Graphics& g);
  juce::Colour boardColour() const;

  FaceplateSpec plate;
  BlockContext context;
  juce::String signature;
  std::vector<KnobParam> lastSpecs;
  BlockLive live;
  bool liveSeen = false;
  int diameter = 52;
  Metrics metrics{10, 48, 8, 10, 16, 6, 12, 32, 6};
  bool compact = false;

  std::vector<std::unique_ptr<juce::Component>> owned;
  std::map<juce::String, ParamKnob*> knobs;
  std::unique_ptr<ParamKnob> level;
  FaceBrightSwitch* brightSwitch = nullptr;
  FaceVoicingSwitch* voicingSwitch = nullptr;
  std::vector<Box> boxes;
  juce::Rectangle<int> board, plateBounds, strip, levelLabel;
};

}  // namespace pedaleira::ui

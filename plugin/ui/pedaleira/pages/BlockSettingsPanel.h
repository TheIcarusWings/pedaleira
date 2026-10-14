// The settings of the selected block, docked under the grid and always there:
// its identity in the block's category colour (name, kind, the switch that
// turns it on in this scene), a picture of what the block does for its family
// (BlockStyle), the live readings the kind reports (gain reduction, sag), and
// all its parameters with the controls of the block editors, in sections. It
// reuses the editors' bodies (EffectEditor, NamEditor, IrEditor) inside a
// viewport, so there is one set of controls; what it changes goes through the
// same commands, and it follows the rig by being shown again after a scene
// change, an undo or a preset load. An amp is not a list of sections: its
// body is its faceplate (AmpFaceplate), whose controls are the amp's
// parameters. Collapsed it is a slim bar.
#pragma once

#include "../EngineClient.h"
#include "../widgets/Controls.h"
#include "AmpFaceplate.h"
#include "BlockStyle.h"
#include "EditorParts.h"
#include "GridModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>

namespace pedaleira::ui {

class EffectEditor;

class BlockSettingsPanel : public juce::Component {
public:
  explicit BlockSettingsPanel(EngineClient& engine);
  ~BlockSettingsPanel() override;

  // Shows the block, read again from the commands; an empty or unknown id
  // shows the hint.
  void showBlock(const GridModel& model, const juce::String& blockId);
  // One meters.get reading: the picture's live levels, the readings, the
  // body's meters.
  void showMeters(const juce::var& meters);

  void setCollapsed(bool collapsed);
  bool isCollapsed() const { return collapsed; }

  bool hasBlock() const { return shown; }
  const juce::String& blockId() const { return context.blockId; }
  // What the panel shows, for the tests: the value of a parameter of an
  // effect block, and the name in the header.
  std::optional<double> shownValue(const juce::String& paramId) const;
  juce::String shownName() const { return context.name; }
  bool hasPicture() const { return visual != nullptr; }
  // The faceplate of an amp block, or null for any other block.
  const AmpFaceplate* ampFaceplate() const { return faceplate; }
  juce::Rectangle<int> contentBounds() const;

  std::function<void(const juce::String& blockId)> onOpenEditor;
  std::function<void(const juce::String& blockId)> onReplace;
  // The rig changed from here (a switch, an assignment): the page reads it.
  std::function<void()> onChanged;
  // The chevron was pressed.
  std::function<void(bool collapsed)> onCollapsedChanged;

  static constexpr int kBarHeight = 44;
  // What the expanded panel needs to show an editor without scrolling at the
  // default window size.
  static constexpr int kExpandedHeight = 328;

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  struct Content;
  struct ReadingSpec {
    juce::String id, name, unit;
    double lo = 0.0, hi = 1.0;
  };
  void clearBlock();
  void updateButtons();
  void makeBody();
  void layoutBody();
  void readKinds();
  juce::String readingText(const ReadingSpec& spec, double value) const;
  juce::String kindLine() const;
  juce::Rectangle<int> headerArea() const;

  EngineClient& engine;
  BlockContext context;
  bool shown = false;
  bool collapsed = false;
  juce::Colour accent = colour::iris;
  juce::String bodyKind;

  std::unique_ptr<Content> content;
  juce::Viewport viewport;
  std::unique_ptr<BlockEditorBody> body;
  EffectEditor* effect = nullptr;
  // The body of an amp block, drawn straight into the content area.
  AmpFaceplate* faceplate = nullptr;
  std::unique_ptr<BlockVisual> visual;

  std::map<juce::String, std::vector<ReadingSpec>> readingSpecs;
  bool kindsRead = false;
  std::map<juce::String, double> readings;
  juce::Rectangle<int> readingsBounds;

  Toggle powered{"Block on"};
  PushButton assign{"Assign footswitch"};
  PushButton replace{"Replace"};
  PushButton openEditor{"Full editor"};
  PushButton chevron{""};
};

}  // namespace pedaleira::ui

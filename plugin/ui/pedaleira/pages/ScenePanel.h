// The panel of the active scene (the board "Scenes and parameters"), under
// the grid: its name, copy, paste and reset, the parameters that have a
// value per scene with the value in scenes A to D and E to H, and the on or
// off state of every block in this scene. Everything goes through the scene
// commands.
#pragma once

#include "../EngineClient.h"
#include "../widgets/Controls.h"
#include "GridModel.h"
#include "ParamKnobs.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>
#include <vector>

namespace pedaleira::ui {

class ScenePanel : public juce::Component {
public:
  explicit ScenePanel(EngineClient& engine);
  ~ScenePanel() override;

  // Reads the scenes again (a scene was selected, a value or a block changed).
  void refresh(const GridModel& model);

  // A scene was picked from the table's header.
  std::function<void(const juce::String& scene)> onSelectScene;
  // The scene or a block's state changed: the page reads the rig again.
  std::function<void()> onChanged;
  // "Footswitch modes": the overview page.
  std::function<void()> onOpenModes;

  void paint(juce::Graphics& g) override;
  void resized() override;
  void mouseUp(const juce::MouseEvent& event) override;

private:
  struct ParamRow {
    juce::String blockName, paramName;
    Category category = Category::utility;
    KnobParam spec;
    std::array<double, 8> values{};
  };
  struct BlockRow {
    juce::String blockId, name;
    Category category = Category::utility;
    bool enabled = true;
  };
  class MiniSwitch;

  void layoutBlocks();
  juce::String cellText(const ParamRow& row, int column) const;
  void commitName();
  juce::Rectangle<int> headerCell(int column) const;

  EngineClient& engine;
  juce::String activeId, activeName;
  // The scenes the preset uses: the table has a column for each.
  int sceneCount = 1;
  std::vector<ParamRow> rows;
  std::vector<BlockRow> blocks;
  juce::String copiedScene;

  juce::TextEditor name;
  PushButton copy{"Copy scene"};
  PushButton paste{"Paste"};
  PushButton reset{"Reset scene"};
  PushButton modes{"Footswitch modes", PushButton::Kind::ghost};
  std::vector<std::unique_ptr<MiniSwitch>> switches;

  juce::Rectangle<int> tableBox, blocksBox, noteArea;
  int hiddenRows = 0;
  int hiddenBlocks = 0;
};

}  // namespace pedaleira::ui

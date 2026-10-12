// Settings, "Expression pedals" (the board "Settings, expression"). What the
// engine has behind it today is the assignment: a block parameter set aside
// for the pedal (expression.assignParam and unassignParam), which keeps it
// out of the scenes. The pedal's own input, the heel and toe calibration, the
// response curve and the source are not in the engine yet; their cards say so.
#pragma once

#include "SettingsParts.h"

#include <memory>
#include <vector>

namespace pedaleira::ui {

class SettingsExpression : public SettingsSection {
public:
  explicit SettingsExpression(EngineClient& engine);

  void refresh() override;
  int heightFor(int width) const override;

  // Testbed and tests: the same as the page's buttons.
  int assignmentCount() const { return static_cast<int>(assignments.size()); }
  void assign(const juce::String& blockId, const juce::String& paramId);
  void unassign(int index);

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  struct Assignment {
    juce::String blockId, blockName, paramId, paramName, value;
    Category category = Category::utility;
  };
  struct Layout {
    juce::Rectangle<int> calibration, response, assignments, calibrationBox, responseBox, footer, empty;
    std::vector<juce::Rectangle<int>> rows;
    int height = 0;
  };

  Layout layoutFor(int width) const;
  void rebuildButtons();
  void openAssignMenu();

  std::vector<Assignment> assignments;
  std::vector<std::unique_ptr<IconButton>> removeButtons;
  Layout lay;
  PushButton assignButton{"Assign parameter"};
};

}  // namespace pedaleira::ui

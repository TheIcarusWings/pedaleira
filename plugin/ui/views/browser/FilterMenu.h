// The dropdown under a filter chip (the Select tone mockups' menus): a
// panel of 14px rows, the current value in white, the rest muted. Rows may
// lead with an icon. The panel fits its widest row and grows with its rows
// up to a cap, then scrolls. Any pick reports and closes.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "core/Icons.h"
#include "widgets/DragScroller.h"
#include "widgets/Popover.h"

namespace t3k::ui {

class FilterMenu : public Popover {
public:
  struct Option {
    juce::String id;
    juce::String label;
    // Set: the row leads with this icon (the Favorites bookmark).
    std::optional<Icon> icon;
  };
  static constexpr int kGap = 8;  // anchor → panel

  FilterMenu();
  ~FilterMenu() override;

  // Replace the rows; the `picked` id reads as current.
  void setOptions(std::vector<Option> options, const juce::String& picked);
  // Open below `anchor`.
  void openBelow(juce::Component& anchor);

  std::function<void(const juce::String& id)> onPick;

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  class Row;
  static constexpr int kMinWidth = 200, kMaxWidth = 320;
  static constexpr int kPad = 8;
  static constexpr int kRowHeight = 36;
  static constexpr int kMaxVisibleRows = 6;

  void layoutRows();

  DragScroller viewport_{DragScroller::Axis::vertical};
  juce::Component list_;
  std::vector<std::unique_ptr<Row>> rows_;
  int width_ = kMinWidth;
};

}  // namespace t3k::ui

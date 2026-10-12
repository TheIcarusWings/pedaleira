#include "FilterMenu.h"

#include <algorithm>
#include <cmath>

#include "core/Fonts.h"
#include "core/Icons.h"
#include "core/Paint.h"
#include "core/Theme.h"
#include "widgets/Clickable.h"

namespace t3k::ui {

// One option: an optional icon, the label; a rounded hover fill. The
// picked row reads white, the rest muted.
class FilterMenu::Row : public Clickable {
public:
  static constexpr int kPadX = 12;
  static constexpr int kIcon = 16;
  static constexpr int kColumnGap = 8;
  static constexpr float kPx = 14;

  Row(const Option& option, bool picked)
      : Clickable(option.label), label_(option.label), icon_(option.icon), picked_(picked) {
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
  }

  // Everything but the label, so the menu can fit its widest row.
  int chromeWidth() const { return 2 * kPadX + (icon_ ? kIcon + kColumnGap : 0); }
  const juce::String& label() const { return label_; }

  void paintButton(juce::Graphics& g, bool highlighted, bool) override {
    auto box = getLocalBounds();
    if (highlighted) paint::fill(g, box.toFloat(), 8.0f, juce::Colours::white.withAlpha(0.08f));
    const auto fg = picked_ ? theme::kWhite : theme::kMuted;
    auto content = box.reduced(kPadX, 0);
    if (icon_) {
      Icons::draw(g, *icon_, content.removeFromLeft(kIcon).withSizeKeepingCentre(kIcon, kIcon).toFloat(), fg);
      content.removeFromLeft(kColumnGap);
    }
    paint::text(g, label_, content, Fonts::sans(kPx), fg);
  }

private:
  juce::String label_;
  std::optional<Icon> icon_;
  bool picked_;
};

FilterMenu::FilterMenu() {
  viewport_.setViewedComponent(&list_, false);
  addAndMakeVisible(viewport_);
  layoutRows();
}

FilterMenu::~FilterMenu() = default;

void FilterMenu::setOptions(std::vector<Option> options, const juce::String& picked) {
  rows_.clear();
  for (auto& option : options) {
    auto row = std::make_unique<Row>(option, option.id == picked);
    row->onClick = [this, id = option.id] {
      close();
      if (onPick) onPick(id);
    };
    list_.addAndMakeVisible(*row);
    rows_.push_back(std::move(row));
  }
  layoutRows();
}

void FilterMenu::openBelow(juce::Component& anchor) { open(anchor, Align::left, kGap); }

// Fit the widest row, then grow with the rows up to kMaxVisibleRows.
void FilterMenu::layoutRows() {
  int content = 0;
  for (const auto& row : rows_) {
    const auto labelW = static_cast<int>(std::ceil(Fonts::width(Fonts::sans(Row::kPx), row->label())));
    content = std::max(content, row->chromeWidth() + labelW + 1);  // +1: no ellipsis on the widest
  }
  width_ = std::clamp(content + 2 * (kBorder + kPad), kMinWidth, kMaxWidth);

  const int rowCount = std::max(1, static_cast<int>(rows_.size()));
  list_.setSize(width_ - 2 * (kBorder + kPad), kRowHeight * rowCount);
  int y = 0;
  for (auto& row : rows_) {
    row->setBounds(0, y, list_.getWidth(), kRowHeight);
    y += kRowHeight;
  }
  setSize(width_, 2 * (kBorder + kPad) + kRowHeight * std::min(rowCount, kMaxVisibleRows));
  reposition();
  repaint();
}

void FilterMenu::paint(juce::Graphics& g) {
  const auto box = getLocalBounds().toFloat();
  paint::fill(g, box, theme::kPanelCorner, theme::kPanelBg);
  paint::border(g, box, theme::kPanelCorner, theme::kBorder);
}

void FilterMenu::resized() { viewport_.setBounds(contentBounds().reduced(kPad)); }

}  // namespace t3k::ui

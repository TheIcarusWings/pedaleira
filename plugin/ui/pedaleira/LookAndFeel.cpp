#include "LookAndFeel.h"

#include "Icons.h"
#include "Paint.h"
#include "Tokens.h"
#include "Type.h"

namespace pedaleira::ui {

namespace {
constexpr int kItemHeight = 32;
constexpr int kSeparatorHeight = 9;
constexpr int kHeaderHeight = 28;
constexpr int kBorder = 6;
// Room on the left of an item for its tick, and on the right for a submenu's chevron.
constexpr int kTickColumn = 28;
constexpr int kArrowColumn = 24;
}  // namespace

PedaleiraLookAndFeel::PedaleiraLookAndFeel() {
  setColour(juce::PopupMenu::backgroundColourId, colour::raised);
  setColour(juce::PopupMenu::textColourId, colour::text);
  setColour(juce::PopupMenu::headerTextColourId, colour::muted);
  setColour(juce::PopupMenu::highlightedBackgroundColourId, colour::control);
  setColour(juce::PopupMenu::highlightedTextColourId, colour::text);
}

juce::Font PedaleiraLookAndFeel::getPopupMenuFont() { return type::compact(); }

void PedaleiraLookAndFeel::preparePopupMenuWindow(juce::Component& window) {
  // The rounded corners show what is under the menu.
  window.setOpaque(false);
}

void PedaleiraLookAndFeel::drawPopupMenuBackgroundWithOptions(juce::Graphics& g, int width, int height,
                                                              const juce::PopupMenu::Options&) {
  const auto area = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
  g.setColour(colour::drop);
  g.fillRoundedRectangle(area, size::panelRadius);
  const auto body = area.withTrimmedBottom(size::dropDepth);
  g.setColour(colour::raised);
  g.fillRoundedRectangle(body, size::panelRadius);
  g.setColour(colour::line);
  g.drawRoundedRectangle(body.reduced(0.5f), size::panelRadius, 1.0f);
}

int PedaleiraLookAndFeel::getPopupMenuBorderSizeWithOptions(const juce::PopupMenu::Options&) { return kBorder; }

void PedaleiraLookAndFeel::getIdealPopupMenuItemSizeWithOptions(const juce::String& text, bool isSeparator, int,
                                                                int& idealWidth, int& idealHeight,
                                                                const juce::PopupMenu::Options&) {
  if (isSeparator) {
    idealWidth = 40;
    idealHeight = kSeparatorHeight;
    return;
  }
  idealHeight = kItemHeight;
  idealWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(getPopupMenuFont(), text)) + kTickColumn +
               kArrowColumn + 12;
}

void PedaleiraLookAndFeel::drawPopupMenuSectionHeaderWithOptions(juce::Graphics& g, const juce::Rectangle<int>& area,
                                                                 const juce::String& sectionName,
                                                                 const juce::PopupMenu::Options&) {
  paint::label(g, sectionName, area.withTrimmedLeft(12).withTrimmedTop(6), juce::Justification::centredLeft,
               colour::muted);
}

void PedaleiraLookAndFeel::drawPopupMenuItemWithOptions(juce::Graphics& g, const juce::Rectangle<int>& area,
                                                        bool isHighlighted, const juce::PopupMenu::Item& item,
                                                        const juce::PopupMenu::Options&) {
  if (item.isSeparator) {
    g.setColour(colour::line);
    g.fillRect(area.reduced(8, 0).withSizeKeepingCentre(area.getWidth() - 16, 1));
    return;
  }
  const bool live = item.isEnabled;
  auto row = area.toFloat().reduced(2.0f, 1.0f);
  if (isHighlighted && live) {
    g.setColour(colour::control);
    g.fillRoundedRectangle(row, size::buttonRadius - 2.0f);
  }
  auto inner = area.withTrimmedLeft(4);
  const auto tick = inner.removeFromLeft(kTickColumn).toFloat();
  if (item.isTicked)
    icons::draw(g, icons::path(icons::Icon::check), tick.withSizeKeepingCentre(14.0f, 14.0f), colour::iris, 2.0f);
  const auto arrow = inner.removeFromRight(kArrowColumn).toFloat();
  if (item.subMenu != nullptr)
    icons::draw(g, icons::path(icons::Icon::chevronRight), arrow.withSizeKeepingCentre(14.0f, 14.0f),
                live ? colour::muted : colour::faint);
  g.setFont(item.isTicked ? type::sans(13.0f, type::Weight::semibold) : type::compact());
  g.setColour(!live ? colour::faint : item.isTicked ? colour::text : isHighlighted ? colour::text : colour::muted);
  g.drawFittedText(item.text, inner, juce::Justification::centredLeft, 1);
}

void PedaleiraLookAndFeel::drawPopupMenuUpDownArrowWithOptions(juce::Graphics& g, int width, int height,
                                                               bool isScrollUpArrow, const juce::PopupMenu::Options&) {
  const auto area = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
  icons::draw(g, icons::path(isScrollUpArrow ? icons::Icon::chevronUp : icons::Icon::chevronDown),
              area.withSizeKeepingCentre(14.0f, 14.0f), colour::muted);
}

}  // namespace pedaleira::ui

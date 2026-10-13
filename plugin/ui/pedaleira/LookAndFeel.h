// What JUCE draws itself in the Pedaleira UI, in the design system's terms:
// popup menus (the row menus, the cable menu, the knob menus and the rest)
// on the raised surface with the app's type, radius and accent.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace pedaleira::ui {

class PedaleiraLookAndFeel : public juce::LookAndFeel_V4 {
public:
  PedaleiraLookAndFeel();

  juce::Font getPopupMenuFont() override;
  void drawPopupMenuBackgroundWithOptions(juce::Graphics& g, int width, int height,
                                          const juce::PopupMenu::Options& options) override;
  void drawPopupMenuItemWithOptions(juce::Graphics& g, const juce::Rectangle<int>& area, bool isHighlighted,
                                    const juce::PopupMenu::Item& item,
                                    const juce::PopupMenu::Options& options) override;
  void drawPopupMenuSectionHeaderWithOptions(juce::Graphics& g, const juce::Rectangle<int>& area,
                                             const juce::String& sectionName,
                                             const juce::PopupMenu::Options& options) override;
  void getIdealPopupMenuItemSizeWithOptions(const juce::String& text, bool isSeparator, int standardMenuItemHeight,
                                            int& idealWidth, int& idealHeight,
                                            const juce::PopupMenu::Options& options) override;
  int getPopupMenuBorderSizeWithOptions(const juce::PopupMenu::Options& options) override;
  void drawPopupMenuUpDownArrowWithOptions(juce::Graphics& g, int width, int height, bool isScrollUpArrow,
                                           const juce::PopupMenu::Options& options) override;
  void preparePopupMenuWindow(juce::Component& window) override;
};

}  // namespace pedaleira::ui

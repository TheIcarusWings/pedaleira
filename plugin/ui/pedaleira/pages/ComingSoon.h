// Stands in for a page that is not built yet:
// its name and a line on what will be there.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace pedaleira::ui {

class ComingSoon : public juce::Component {
public:
  ComingSoon(const juce::String& title, const juce::String& note);
  void paint(juce::Graphics& g) override;

private:
  juce::String title, note;
};

}  // namespace pedaleira::ui

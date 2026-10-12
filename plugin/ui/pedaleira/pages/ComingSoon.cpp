#include "ComingSoon.h"

#include "../Tokens.h"
#include "../Type.h"

namespace pedaleira::ui {

ComingSoon::ComingSoon(const juce::String& pageTitle, const juce::String& pageNote)
    : title(pageTitle), note(pageNote) {
  setTitle(pageTitle);
}

void ComingSoon::paint(juce::Graphics& g) {
  g.fillAll(colour::ground);
  auto area = getLocalBounds().reduced(40, 32);
  g.setFont(type::title());
  g.setColour(colour::text);
  g.drawText(title, area.removeFromTop(34), juce::Justification::centredLeft);
  area.removeFromTop(6);
  g.setFont(type::body());
  g.setColour(colour::muted);
  g.drawText(note, area.removeFromTop(20), juce::Justification::centredLeft);
}

}  // namespace pedaleira::ui

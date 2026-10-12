// What the Live page's tabs share (the boards "Live tuner" and "Live tempo"):
// the card, the small round or square plus and minus buttons,
// and the line saying a part comes in a later build.
#pragma once

#include "../Tokens.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace pedaleira::ui {

// A line-icon button with no text: round at 64 px beside the tempo, square at
// 36 px beside the reference pitch. Its name is what a screen reader says.
class StepButton : public juce::Button {
public:
  enum class Shape { square, round };

  StepButton(const juce::String& accessibleName, const juce::Path* iconPath, Shape shape);

protected:
  void paintButton(juce::Graphics& g, bool highlighted, bool down) override;

private:
  const juce::Path* icon;
  Shape shape;
};

namespace live {

// The note a setting carries when the engine has nothing behind it yet.
inline const char* const kLater = "Comes in a later build";

// A card of the boards: surface, line border, 10 px radius.
void paintCard(juce::Graphics& g, juce::Rectangle<int> box);
// A muted line of 12 px type (the later-build notes, hints).
void paintHint(juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area,
               juce::Justification justification = juce::Justification::centredLeft);

}  // namespace live

}  // namespace pedaleira::ui

// The testbed's page of widgets in every state, laid out like the board
// "Design system" so the two can be compared side by side.
#pragma once

#include "widgets/Controls.h"
#include "widgets/Knob.h"
#include "widgets/Tile.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace pedaleira::ui::testbed {

class Gallery : public juce::Component {
public:
  Gallery();

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  std::vector<std::unique_ptr<Tile>> categoryTiles;
  std::vector<std::unique_ptr<Tile>> stateTiles;
  std::vector<std::unique_ptr<Knob>> knobs;
  std::vector<std::unique_ptr<Footswitch>> switches;
  PushButton primary{"Save preset", PushButton::Kind::primary};
  PushButton secondary{"Cancel"};
  PushButton ghost{"Skip", PushButton::Kind::ghost};
  SegmentedControl segmented{{"Preset", "Scene", "Stomp", "Hybrid"}};
  Toggle toggleOn{"On"};
  Toggle toggleOff{"Off"};
  LevelMeter meter;
};


}  // namespace pedaleira::ui::testbed

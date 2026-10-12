// The four footswitch modes side by side (the board "Footswitch modes"):
// what the eight switches do in Preset, Scene, Stomp and Hybrid, the
// current one outlined in iris, a click on another one switching to it. The
// hybrid card holds the two row selectors. The switches here show; they are
// pressed on the Gig View or on the pedalboard.
#pragma once

#include "../EngineClient.h"
#include "../widgets/Controls.h"
#include "../widgets/Tile.h"
#include "FootswitchModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>

namespace pedaleira::ui {

class FootswitchModesPage : public juce::Component {
public:
  explicit FootswitchModesPage(EngineClient& engine);
  ~FootswitchModesPage() override;

  // Reads the switches, the mode and the MIDI defaults again.
  void refresh();

  std::function<void()> onBack;

  void paint(juce::Graphics& g) override;
  void resized() override;
  void mouseUp(const juce::MouseEvent& event) override;
  bool keyPressed(const juce::KeyPress& key) override;

private:
  struct Card {
    juce::String mode, title, subtitle;
    std::array<std::unique_ptr<Footswitch>, 8> switches;
    std::array<std::unique_ptr<Footswitch>, 3> util;
    juce::Rectangle<int> box, pill;
    // The pill under the switches: an optional number in mono, then text.
    juce::String pillNumber, pillText;
  };

  void setMode(const juce::String& mode);
  void chooseRow(bool top);
  // "CC 47" for a fixed MIDI control, or "off".
  juce::String ccText(const char* action) const;

  EngineClient& engine;
  FootswitchModel model;
  std::array<std::unique_ptr<Card>, 4> cards;
  PushButton back{"Back to grid"};
  PushButton topRow{"Top row: Scene"};
  PushButton bottomRow{"Bottom row: Stomp"};
  juce::var midiDefaults;
  juce::Rectangle<int> barArea, titleArea;
};

}  // namespace pedaleira::ui

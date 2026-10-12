// A grid slot (the board "Design system", "Block tiles and icons", 104 x 76):
// the block's name on top, its category under it, the category icon, and
// the footswitch letter in the top right when a stomp switch drives it.
#pragma once

#include "../Tokens.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace pedaleira::ui {

class Tile : public juce::Component {
public:
  enum class State {
    active,
    bypassed,  // grey and dashed
    missing,   // a plugin that is not installed, or a kind this build lacks
    empty,     // an empty slot: a plus in a dashed outline
    loading,   // a model still loading; the old block keeps playing
  };

  Tile();

  void setBlock(const juce::String& name, Category category, State state);
  void setEmpty();
  void setSelected(bool selected);
  // "A" to "H", or empty for none.
  void setFootswitch(const juce::String& letter);

  bool isSelected() const { return selected; }
  State getState() const { return state; }

  std::function<void()> onClick;

  void paint(juce::Graphics& g) override;
  void mouseUp(const juce::MouseEvent& event) override;
  bool keyPressed(const juce::KeyPress& key) override;

private:
  juce::String name;
  Category category = Category::utility;
  State state = State::empty;
  bool selected = false;
  juce::String footswitch;
};

// A footswitch (the board "Design system", "Footswitches"): round, 72 px on
// the Mac, 56 in the mode overview and 96 in Gig View. A stomp switch lights
// a ring in its block's colour; a scene or preset switch fills iris when it
// is the active one; the tap switch blinks on the beat; an unassigned one is
// a dashed outline. Under the circle sit its name and a short state line.
class Footswitch : public juce::Component {
public:
  enum class Look { off, stomp, active, tap, empty };
  // mini and utility are the bank and tap switches (44 and 72 px).
  enum class Size { regular, compact, gig, mini, utility };

  explicit Footswitch(const juce::String& letter);

  void setSize(Size newSize);
  // The name under the circle, and the line under that ("On", "Active").
  void setLabel(const juce::String& label);
  void setSub(const juce::String& sub);
  // A preset's number, set in mono before the name.
  void setNumber(const juce::String& number);
  // Text in the circle instead of the letter (the tempo on the tap switch).
  void setText(const juce::String& text);
  // A line icon (24 px box) in the circle instead of the letter; the path
  // must outlive the switch.
  void setIcon(const juce::Path* iconPath);
  void setLook(Look look, juce::Colour ring = colour::iris);
  void setBlinkOn(bool on);
  void setSelected(bool selected);

  // The height a switch of this size needs with its name and state lines.
  static int heightFor(Size size);

  // The press, and the release when the mouse lets go (a momentary stomp).
  std::function<void()> onPress;
  std::function<void()> onRelease;

  void paint(juce::Graphics& g) override;
  void mouseDown(const juce::MouseEvent& event) override;
  void mouseUp(const juce::MouseEvent& event) override;
  bool keyPressed(const juce::KeyPress& key) override;

private:
  juce::String letter, text;
  juce::String label, sub, number;
  const juce::Path* icon = nullptr;
  Size sizeClass = Size::regular;
  Look look = Look::off;
  juce::Colour ring = colour::iris;
  bool blink = false;
  bool selected = false;
};

}  // namespace pedaleira::ui

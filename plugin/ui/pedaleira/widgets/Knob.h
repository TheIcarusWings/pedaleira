// The knob of the board "Design system", "Knobs": a flat cap, a 270 degree
// value arc and a pointer line. The arc is white by default, iris when the
// value differs per scene and green when an expression pedal drives it; a
// bypassed block's knobs are dimmed. Drag up or down to turn it (shift for
// fine steps), double-click to return to the default, arrow keys to step.
#pragma once

#include "../Tokens.h"
#include "Motion.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace pedaleira::ui {

class Knob : public juce::Component {
public:
  enum class Assignment { none, scene, expression };

  explicit Knob(const juce::String& name);

  // 0 to 1 along the arc.
  void setPosition(double newPosition, juce::NotificationType notification = juce::dontSendNotification);
  double getPosition() const { return position; }
  void setDefaultPosition(double position) { defaultPosition = position; }
  // The value as the player reads it ("7.0", "30%", "-12.4 dB").
  void setValueText(const juce::String& text);
  void setAssignment(Assignment newAssignment);
  void setBypassed(bool bypassed);
  // A line under the value ("Scene-assigned"); empty for none.
  void setTag(const juce::String& text, juce::Colour tint = colour::iris);
  // The cap's diameter; the arc sits around it. 64 by default.
  void setDiameter(int diameter);
  // The arc's colour when nothing is assigned (a block's category colour in
  // its settings); white by default.
  void setAccent(juce::Colour colour);

  std::function<void(double)> onChange;
  // A right click (or control click): the knob's menu, such as assigning
  // it to scenes.
  std::function<void()> onContextMenu;

  void paint(juce::Graphics& g) override;
  void mouseEnter(const juce::MouseEvent& event) override;
  void mouseExit(const juce::MouseEvent& event) override;
  void mouseDown(const juce::MouseEvent& event) override;
  void mouseDrag(const juce::MouseEvent& event) override;
  void mouseUp(const juce::MouseEvent& event) override;
  void mouseDoubleClick(const juce::MouseEvent& event) override;
  void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
  bool keyPressed(const juce::KeyPress& key) override;

private:
  std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

  double position = 0.0;
  double defaultPosition = 0.0;
  double dragStart = 0.0;
  juce::String valueText, tagText;
  juce::Colour tagColour;
  Assignment assignment = Assignment::none;
  bool bypassed = false;
  juce::Colour accent = colour::text;
  int diameter = 64;
  // The mouse over the knob (a halo and a brighter track), and a turn in
  // progress (the value lights up and the cap grows a little).
  Tween hover{*this};
  Tween turning{*this, 0.0f, motion::pressMs};
};

// A horizontal level meter, -60 to 0 dB, green into amber into red, with an
// optional peak hold.
class LevelMeter : public juce::Component {
public:
  void setLevel(float db, float peakHoldDb = -100.0f);
  void paint(juce::Graphics& g) override;
  // The same bar drawn into any area, for views that meter without a
  // component per meter.
  static void paintBar(juce::Graphics& g, juce::Rectangle<float> area, float db, float peakHoldDb);

private:
  float level = -100.0f;
  float hold = -100.0f;
};

}  // namespace pedaleira::ui

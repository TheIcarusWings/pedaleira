// The small controls of the board "Design system", "Buttons and controls":
// buttons, the segmented control and the on/off switch.
#pragma once

#include "../Tokens.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace pedaleira::ui {

// A text button. Primary is the iris fill, one per screen; secondary the
// raised control colour with a line border; ghost text only; mono shows its
// text as a value (a tempo, a level).
class PushButton : public juce::Button {
public:
  enum class Kind { primary, secondary, ghost };

  explicit PushButton(const juce::String& text, Kind kind = Kind::secondary);

  void setKind(Kind newKind);
  void setMono(bool mono);
  // A line icon (24 px box) before the text; the path must outlive the button.
  void setIcon(const juce::Path* iconPath);
  // The width the text needs at this height, padding included.
  int idealWidth() const;

protected:
  void paintButton(juce::Graphics& g, bool highlighted, bool down) override;

private:
  Kind kind;
  bool monoText = false;
  const juce::Path* icon = nullptr;
};

// A choice shown as a field with a chevron (the boards' selects: Sort by,
// Setlist). It only shows the current choice; the owner opens its menu from
// onClick.
class SelectBox : public juce::Button {
public:
  explicit SelectBox(const juce::String& accessibleName);

  void setChoice(const juce::String& newText);
  const juce::String& choice() const { return text; }

protected:
  void paintButton(juce::Graphics& g, bool highlighted, bool down) override;

private:
  juce::String text;
};

// A row of mutually exclusive choices (Preset / Scene / Stomp / Hybrid).
class SegmentedControl : public juce::Component, public juce::SettableTooltipClient {
public:
  explicit SegmentedControl(juce::StringArray options);

  void setSelected(int index, juce::NotificationType notification = juce::dontSendNotification);
  int getSelected() const { return selected; }
  std::function<void(int)> onChange;

  int idealWidth() const;
  void paint(juce::Graphics& g) override;
  void mouseUp(const juce::MouseEvent& event) override;
  bool keyPressed(const juce::KeyPress& key) override;

private:
  juce::Rectangle<float> segmentBounds(int index) const;
  int segmentAt(juce::Point<float> point) const;
  std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

  juce::StringArray options;
  int selected = 0;
};

// The round switch: iris track with a dark knob when on, line track with a
// muted knob when off.
class Toggle : public juce::Button {
public:
  explicit Toggle(const juce::String& accessibleName);

protected:
  void paintButton(juce::Graphics& g, bool highlighted, bool down) override;
};

}  // namespace pedaleira::ui

#include "Controls.h"

#include "../Icons.h"
#include "../Paint.h"
#include "../Type.h"

namespace pedaleira::ui {

// ####################
// PUSH BUTTON
// ####################
PushButton::PushButton(const juce::String& text, Kind buttonKind) : juce::Button(text), kind(buttonKind) {
  setMouseCursor(juce::MouseCursor::PointingHandCursor);
  setWantsKeyboardFocus(true);
}

void PushButton::setKind(Kind newKind) {
  kind = newKind;
  repaint();
}

void PushButton::setMono(bool mono) {
  monoText = mono;
  repaint();
}

void PushButton::setIcon(const juce::Path* iconPath) {
  icon = iconPath;
  repaint();
}

int PushButton::idealWidth() const {
  const juce::Font font = monoText ? type::value() : type::button();
  return juce::roundToInt(juce::GlyphArrangement::getStringWidth(font, getButtonText())) + 28 + (icon != nullptr ? 24 : 0);
}

void PushButton::paintButton(juce::Graphics& g, bool highlighted, bool down) {
  const auto area = getLocalBounds().toFloat();
  const float alpha = isEnabled() ? 1.0f : 0.45f;
  juce::Colour textColour = colour::text;
  switch (kind) {
    case Kind::primary: {
      const juce::Colour fill = highlighted && !down ? colour::irisHover : colour::iris;
      g.setColour(fill.withMultipliedAlpha(alpha));
      g.fillRoundedRectangle(area, size::buttonRadius);
      textColour = colour::ground;
      break;
    }
    case Kind::secondary: {
      const juce::Colour fill = down ? colour::control : highlighted ? colour::control : colour::raised;
      g.setColour(fill.withMultipliedAlpha(alpha));
      g.fillRoundedRectangle(area, size::buttonRadius);
      g.setColour(colour::line.withMultipliedAlpha(alpha));
      g.drawRoundedRectangle(area.reduced(0.5f), size::buttonRadius, 1.0f);
      break;
    }
    case Kind::ghost:
      if (highlighted) {
        g.setColour(colour::raised);
        g.fillRoundedRectangle(area, size::buttonRadius);
      }
      textColour = highlighted ? colour::text : colour::muted;
      break;
  }
  if (hasKeyboardFocus(false)) {
    g.setColour(colour::iris);
    g.drawRoundedRectangle(area.reduced(1.0f), size::buttonRadius, 2.0f);
  }
  g.setFont(monoText ? type::value() : type::button());
  g.setColour(textColour.withMultipliedAlpha(alpha));
  auto textArea = getLocalBounds().reduced(8, 0);
  if (icon != nullptr) {
    // The icon and the text together, centred.
    const int textWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), getButtonText()));
    const int total = 18 + 6 + textWidth;
    const int left = textArea.getCentreX() - total / 2;
    icons::draw(g, *icon, juce::Rectangle<float>(static_cast<float>(left), getHeight() * 0.5f - 9.0f, 18.0f, 18.0f),
                textColour.withMultipliedAlpha(alpha));
    textArea = textArea.withTrimmedLeft(left + 24 - textArea.getX());
    g.drawText(getButtonText(), textArea, juce::Justification::centredLeft, true);
    return;
  }
  g.drawText(getButtonText(), textArea, juce::Justification::centred, true);
}

// ####################
// SELECT BOX
// ####################
SelectBox::SelectBox(const juce::String& accessibleName) : juce::Button(accessibleName) {
  setTitle(accessibleName);
  setMouseCursor(juce::MouseCursor::PointingHandCursor);
  setWantsKeyboardFocus(true);
}

void SelectBox::setChoice(const juce::String& newText) {
  text = newText;
  repaint();
}

void SelectBox::paintButton(juce::Graphics& g, bool highlighted, bool) {
  const auto area = getLocalBounds().toFloat();
  const float alpha = isEnabled() ? 1.0f : 0.45f;
  g.setColour((highlighted ? colour::control : colour::raised).withMultipliedAlpha(alpha));
  g.fillRoundedRectangle(area, size::buttonRadius);
  g.setColour(colour::line.withMultipliedAlpha(alpha));
  g.drawRoundedRectangle(area.reduced(0.5f), size::buttonRadius, 1.0f);
  if (hasKeyboardFocus(false)) {
    g.setColour(colour::iris);
    g.drawRoundedRectangle(area.reduced(1.0f), size::buttonRadius, 2.0f);
  }
  auto inner = getLocalBounds().reduced(10, 0);
  icons::draw(g, icons::path(icons::Icon::chevronDown), inner.removeFromRight(16).toFloat().withSizeKeepingCentre(14.0f, 14.0f),
              colour::muted.withMultipliedAlpha(alpha), 2.25f);
  g.setFont(type::body());
  g.setColour(colour::text.withMultipliedAlpha(alpha));
  g.drawText(text, inner, juce::Justification::centredLeft, true);
}

// ####################
// SEGMENTED CONTROL
// ####################
namespace {
constexpr float kTrackPadding = 3.0f;
constexpr float kSegmentPadding = 12.0f;
}  // namespace

SegmentedControl::SegmentedControl(juce::StringArray choices) : options(std::move(choices)) {
  setWantsKeyboardFocus(true);
  setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void SegmentedControl::setSelected(int index, juce::NotificationType notification) {
  index = juce::jlimit(0, juce::jmax(0, options.size() - 1), index);
  if (index == selected)
    return;
  selected = index;
  repaint();
  if (notification != juce::dontSendNotification && onChange)
    onChange(selected);
}

int SegmentedControl::idealWidth() const {
  float total = 2.0f * kTrackPadding;
  for (const auto& option : options)
    total += juce::GlyphArrangement::getStringWidth(type::button(), option) + 2.0f * kSegmentPadding;
  return juce::roundToInt(total);
}

juce::Rectangle<float> SegmentedControl::segmentBounds(int index) const {
  // Segments are as wide as their text, the leftover spread evenly.
  const auto inner = getLocalBounds().toFloat().reduced(kTrackPadding);
  std::vector<float> widths;
  float total = 0.0f;
  for (const auto& option : options) {
    widths.push_back(juce::GlyphArrangement::getStringWidth(type::button(), option) + 2.0f * kSegmentPadding);
    total += widths.back();
  }
  const float extra = options.isEmpty() ? 0.0f : (inner.getWidth() - total) / static_cast<float>(options.size());
  float x = inner.getX();
  for (int i = 0; i < index; ++i)
    x += widths[static_cast<size_t>(i)] + extra;
  return {x, inner.getY(), widths[static_cast<size_t>(index)] + extra, inner.getHeight()};
}

int SegmentedControl::segmentAt(juce::Point<float> point) const {
  for (int i = 0; i < options.size(); ++i)
    if (segmentBounds(i).contains(point))
      return i;
  return -1;
}

void SegmentedControl::paint(juce::Graphics& g) {
  if (!isEnabled())
    g.beginTransparencyLayer(0.45f);
  paint::panel(g, getLocalBounds().toFloat(), colour::surface, 10.0f);
  for (int i = 0; i < options.size(); ++i) {
    const auto segment = segmentBounds(i);
    const bool on = i == selected;
    if (on) {
      g.setColour(colour::drop);
      g.fillRoundedRectangle(segment.translated(0.0f, 1.0f), 7.0f);
      g.setColour(colour::control);
      g.fillRoundedRectangle(segment, 7.0f);
    }
    g.setFont(type::button());
    g.setColour(on ? colour::text : colour::muted);
    g.drawText(options[i], segment, juce::Justification::centred, true);
  }
  if (hasKeyboardFocus(false)) {
    g.setColour(colour::iris);
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 10.0f, 2.0f);
  }
  if (!isEnabled())
    g.endTransparencyLayer();
}

void SegmentedControl::mouseUp(const juce::MouseEvent& event) {
  if (!isEnabled())
    return;
  if (const int index = segmentAt(event.position); index >= 0)
    setSelected(index, juce::sendNotification);
}

bool SegmentedControl::keyPressed(const juce::KeyPress& key) {
  if (!isEnabled())
    return false;
  if (key == juce::KeyPress::leftKey) {
    setSelected(selected - 1, juce::sendNotification);
    return true;
  }
  if (key == juce::KeyPress::rightKey) {
    setSelected(selected + 1, juce::sendNotification);
    return true;
  }
  return false;
}

std::unique_ptr<juce::AccessibilityHandler> SegmentedControl::createAccessibilityHandler() {
  // Read as the selected choice; arrows change it.
  struct Value final : juce::AccessibilityTextValueInterface {
    explicit Value(SegmentedControl& owner) : control(owner) {}
    bool isReadOnly() const override { return true; }
    juce::String getCurrentValueAsString() const override { return control.options[control.selected]; }
    void setValueAsString(const juce::String&) override {}
    SegmentedControl& control;
  };
  return std::make_unique<juce::AccessibilityHandler>(
      *this, juce::AccessibilityRole::group, juce::AccessibilityActions(),
      juce::AccessibilityHandler::Interfaces{std::make_unique<Value>(*this)});
}

// ####################
// TOGGLE
// ####################
Toggle::Toggle(const juce::String& accessibleName) : juce::Button(accessibleName) {
  setClickingTogglesState(true);
  setTitle(accessibleName);
  setWantsKeyboardFocus(true);
  setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void Toggle::paintButton(juce::Graphics& g, bool, bool) {
  const auto track = getLocalBounds().toFloat().withSizeKeepingCentre(40.0f, 22.0f);
  const bool on = getToggleState();
  g.setColour(on ? colour::iris : colour::line);
  g.fillRoundedRectangle(track, 11.0f);
  const float knob = 18.0f;
  const float x = on ? track.getRight() - 2.0f - knob : track.getX() + 2.0f;
  g.setColour(on ? colour::ground : colour::muted);
  g.fillEllipse(x, track.getCentreY() - knob * 0.5f, knob, knob);
  if (hasKeyboardFocus(false)) {
    g.setColour(colour::irisHover);
    g.drawRoundedRectangle(track.expanded(2.0f), 13.0f, 1.5f);
  }
}

}  // namespace pedaleira::ui

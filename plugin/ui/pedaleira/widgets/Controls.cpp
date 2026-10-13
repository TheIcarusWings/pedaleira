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

void PushButton::buttonStateChanged() {
  hover.setTarget(isOver() && isEnabled() ? 1.0f : 0.0f);
  press.setTarget(isDown() && isEnabled() ? 1.0f : 0.0f);
}

void PushButton::paintButton(juce::Graphics& g, bool, bool) {
  const float lit = hover.value(), sink = press.value();
  // Pressed, it sinks a pixel and darkens.
  g.addTransform(juce::AffineTransform::translation(0.0f, sink));
  const auto area = getLocalBounds().toFloat().withTrimmedBottom(1.0f);
  const float alpha = isEnabled() ? 1.0f : 0.45f;
  juce::Colour textColour = colour::text;
  switch (kind) {
    case Kind::primary: {
      const juce::Colour fill = colour::iris.interpolatedWith(colour::irisHover, lit).darker(0.25f * sink);
      g.setColour(fill.withMultipliedAlpha(alpha));
      g.fillRoundedRectangle(area, size::buttonRadius);
      textColour = colour::ground;
      break;
    }
    case Kind::secondary: {
      const juce::Colour fill = colour::raised.interpolatedWith(colour::control, juce::jmax(lit, sink)).darker(0.2f * sink);
      g.setColour(fill.withMultipliedAlpha(alpha));
      g.fillRoundedRectangle(area, size::buttonRadius);
      g.setColour(colour::line.interpolatedWith(colour::dim, 0.5f * lit).withMultipliedAlpha(alpha));
      g.drawRoundedRectangle(area.reduced(0.5f), size::buttonRadius, 1.0f);
      break;
    }
    case Kind::ghost:
      if (lit > 0.0f) {
        g.setColour(colour::raised.withAlpha(lit));
        g.fillRoundedRectangle(area, size::buttonRadius);
      }
      textColour = colour::muted.interpolatedWith(colour::text, lit);
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

void SelectBox::buttonStateChanged() { hover.setTarget(isOver() && isEnabled() ? 1.0f : 0.0f); }

void SelectBox::paintButton(juce::Graphics& g, bool, bool) {
  const auto area = getLocalBounds().toFloat();
  const float alpha = isEnabled() ? 1.0f : 0.45f;
  const float lit = hover.value();
  g.setColour(colour::raised.interpolatedWith(colour::control, lit).withMultipliedAlpha(alpha));
  g.fillRoundedRectangle(area, size::buttonRadius);
  g.setColour(colour::line.interpolatedWith(colour::dim, 0.5f * lit).withMultipliedAlpha(alpha));
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
  const auto segment = segmentBounds(selected);
  pillX.setTarget(segment.getX());
  pillWidth.setTarget(segment.getWidth());
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
  if (!options.isEmpty()) {
    // The pill, where the selection is or on its way there.
    const auto at = segmentBounds(selected);
    const auto pill = juce::Rectangle<float>(pillX.value(), at.getY(), pillWidth.value(), at.getHeight());
    g.setColour(colour::drop);
    g.fillRoundedRectangle(pill.translated(0.0f, 1.0f), 7.0f);
    g.setColour(colour::control);
    g.fillRoundedRectangle(pill, 7.0f);
  }
  for (int i = 0; i < options.size(); ++i) {
    const auto segment = segmentBounds(i);
    const bool on = i == selected;
    if (!on && i == hovered) {
      g.setColour(colour::raised);
      g.fillRoundedRectangle(segment.reduced(1.0f), 7.0f);
    }
    g.setFont(type::button());
    g.setColour(on || i == hovered ? colour::text : colour::muted);
    g.drawText(options[i], segment, juce::Justification::centred, true);
  }
  if (hasKeyboardFocus(false)) {
    g.setColour(colour::iris);
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 10.0f, 2.0f);
  }
  if (!isEnabled())
    g.endTransparencyLayer();
}

void SegmentedControl::resized() {
  if (options.isEmpty())
    return;
  const auto segment = segmentBounds(selected);
  pillX.snapTo(segment.getX());
  pillWidth.snapTo(segment.getWidth());
}

void SegmentedControl::mouseMove(const juce::MouseEvent& event) {
  const int now = isEnabled() ? segmentAt(event.position) : -1;
  if (now != hovered) {
    hovered = now;
    repaint();
  }
}

void SegmentedControl::mouseExit(const juce::MouseEvent&) {
  if (hovered >= 0) {
    hovered = -1;
    repaint();
  }
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

void Toggle::buttonStateChanged() { hover.setTarget(isOver() && isEnabled() ? 1.0f : 0.0f); }

void Toggle::clicked() {
  // A click slides the knob; a state set from outside (a scene, an undo)
  // shows at once, below.
  slide.setTarget(getToggleState() ? 1.0f : 0.0f);
}

void Toggle::paintButton(juce::Graphics& g, bool, bool) {
  const float target = getToggleState() ? 1.0f : 0.0f;
  if (!seen || (!slide.moving() && slide.value() != target)) {
    seen = true;
    slide.snapTo(target);
  }
  const float t = slide.value(), lit = hover.value();
  const auto track = getLocalBounds().toFloat().withSizeKeepingCentre(40.0f, 22.0f);
  g.setColour(colour::line.interpolatedWith(colour::dim, 0.35f * lit)
                  .interpolatedWith(colour::iris.interpolatedWith(colour::irisHover, lit), t));
  g.fillRoundedRectangle(track, 11.0f);
  const float knob = 18.0f;
  const float x = track.getX() + 2.0f + (track.getWidth() - 4.0f - knob) * t;
  g.setColour(colour::muted.interpolatedWith(colour::text, 0.4f * lit).interpolatedWith(colour::ground, t));
  g.fillEllipse(x, track.getCentreY() - knob * 0.5f, knob, knob);
  if (hasKeyboardFocus(false)) {
    g.setColour(colour::irisHover);
    g.drawRoundedRectangle(track.expanded(2.0f), 13.0f, 1.5f);
  }
}

}  // namespace pedaleira::ui

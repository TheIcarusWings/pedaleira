#include "LiveParts.h"

#include "../Icons.h"
#include "../Paint.h"
#include "../Type.h"

namespace pedaleira::ui {

StepButton::StepButton(const juce::String& accessibleName, const juce::Path* iconPath, Shape buttonShape)
    : juce::Button(accessibleName), icon(iconPath), shape(buttonShape) {
  setTitle(accessibleName);
  setWantsKeyboardFocus(true);
  setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void StepButton::paintButton(juce::Graphics& g, bool highlighted, bool down) {
  const float alpha = isEnabled() ? 1.0f : 0.45f;
  const bool round = shape == Shape::round;
  // The round one stands on its 2 px drop; the square one lies flat.
  const auto area = getLocalBounds().toFloat().withTrimmedBottom(round ? size::dropDepth : 0.0f);
  const juce::Colour fill = (highlighted || down ? colour::control : colour::raised).withMultipliedAlpha(alpha);
  if (round) {
    g.setColour(colour::drop.withMultipliedAlpha(alpha));
    g.fillEllipse(area.translated(0.0f, down ? 0.0f : size::dropDepth));
    g.setColour(fill);
    g.fillEllipse(area);
    g.setColour(colour::line.withMultipliedAlpha(alpha));
    g.drawEllipse(area.reduced(0.5f), 1.0f);
  } else {
    g.setColour(fill);
    g.fillRoundedRectangle(area, size::buttonRadius);
    g.setColour(colour::line.withMultipliedAlpha(alpha));
    g.drawRoundedRectangle(area.reduced(0.5f), size::buttonRadius, 1.0f);
  }
  if (hasKeyboardFocus(false)) {
    g.setColour(colour::iris);
    if (round)
      g.drawEllipse(area.reduced(1.0f), 2.0f);
    else
      g.drawRoundedRectangle(area.reduced(1.0f), size::buttonRadius, 2.0f);
  }
  const float glyph = round ? 26.0f : 18.0f;
  icons::draw(g, *icon, area.withSizeKeepingCentre(glyph, glyph), colour::text.withMultipliedAlpha(alpha));
}

namespace live {

void paintCard(juce::Graphics& g, juce::Rectangle<int> box) {
  paint::panel(g, box.toFloat(), colour::surface, size::panelRadius);
}

void paintHint(juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area, juce::Justification justification) {
  g.setFont(type::sans(12.0f));
  g.setColour(colour::muted);
  g.drawText(text, area, justification, true);
}

}  // namespace live

}  // namespace pedaleira::ui

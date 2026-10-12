#include "Paint.h"

#include "Type.h"

namespace pedaleira::ui::paint {

void raised(juce::Graphics& g, juce::Rectangle<float> area, float radius, juce::Colour fill) {
  g.setColour(colour::drop);
  g.fillRoundedRectangle(area.translated(0.0f, size::dropDepth), radius);
  g.setColour(fill);
  g.fillRoundedRectangle(area, radius);
  // The highlight: one pixel of a lighter edge along the top.
  juce::Graphics::ScopedSaveState state(g);
  g.reduceClipRegion(area.withHeight(radius).toNearestInt());
  g.setColour(fill.brighter(0.12f));
  g.drawRoundedRectangle(area.reduced(0.5f), radius, 1.0f);
}

void panel(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour fill, float radius) {
  g.setColour(fill);
  g.fillRoundedRectangle(area, radius);
  g.setColour(colour::line);
  g.drawRoundedRectangle(area.reduced(0.5f), radius, 1.0f);
}

void dashedOutline(juce::Graphics& g, juce::Rectangle<float> area, float radius, juce::Colour colour,
                   float thickness) {
  juce::Path outline;
  outline.addRoundedRectangle(area.reduced(thickness * 0.5f), radius);
  juce::Path dashed;
  const float dashes[] = {4.0f, 3.0f};
  juce::PathStrokeType(thickness).createDashedStroke(dashed, outline, dashes, 2);
  g.setColour(colour);
  g.fillPath(dashed);
}

void chip(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area, ChipStyle style) {
  const float radius = area.getHeight() * 0.5f;
  juce::Colour fill = juce::Colours::transparentBlack, textColour = colour::muted;
  switch (style) {
    case ChipStyle::ok: fill = colour::ok; textColour = colour::ground; break;
    case ChipStyle::warn: fill = colour::warn; textColour = colour::ground; break;
    case ChipStyle::iris: fill = colour::iris; textColour = colour::ground; break;
    case ChipStyle::fail: fill = colour::clip; textColour = colour::ground; break;
    case ChipStyle::outline: fill = colour::raised; break;
    case ChipStyle::dashed:
    case ChipStyle::plain: break;
  }
  g.setColour(fill);
  g.fillRoundedRectangle(area, radius);
  if (style == ChipStyle::outline) {
    g.setColour(colour::line);
    g.drawRoundedRectangle(area.reduced(0.5f), radius, 1.0f);
  } else if (style == ChipStyle::dashed) {
    dashedOutline(g, area, radius, colour::faint);
  }
  g.setFont(type::sans(12.0f, type::Weight::semibold));
  g.setColour(textColour);
  g.drawText(text, area, juce::Justification::centred, false);
}

int chipWidth(const juce::String& text) {
  return juce::roundToInt(juce::GlyphArrangement::getStringWidth(type::sans(12.0f, type::Weight::semibold), text)) + 22;
}

void label(juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area,
           juce::Justification justification, juce::Colour colour) {
  g.setFont(type::label());
  g.setColour(colour);
  g.drawText(text.toUpperCase(), area, justification, true);
}

}  // namespace pedaleira::ui::paint

// Drawing rules shared by every widget: depth is a 2 px hard drop under a
// raised part and a 1 px highlight on its top edge; no gradients, no
// textures (the board "Design system", "Shape and depth").
#pragma once

#include "Tokens.h"

#include <juce_graphics/juce_graphics.h>

namespace pedaleira::ui::paint {

// A raised part: the drop, the body and the top highlight.
void raised(juce::Graphics& g, juce::Rectangle<float> area, float radius, juce::Colour fill);

// A flat panel: surface fill with a 1 px line border.
void panel(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour fill = colour::surface,
           float radius = size::panelRadius);

// A dashed rounded outline (bypassed tiles, empty slots).
void dashedOutline(juce::Graphics& g, juce::Rectangle<float> area, float radius, juce::Colour colour,
                   float thickness = 1.0f);

// A status chip (the importer's OK / Check / Placeholder, Exact / Unverified):
// a filled pill, an outlined one, or a dashed one for what is not there; fail
// is the red one, for a scan that crashed.
enum class ChipStyle { ok, warn, iris, outline, dashed, plain, fail };
void chip(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area, ChipStyle style);
// The width a chip needs for its text, padding included.
int chipWidth(const juce::String& text);

// Text in the label style: upper case, tracked, muted unless told otherwise.
void label(juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area,
           juce::Justification justification = juce::Justification::centredLeft,
           juce::Colour colour = colour::muted);

}  // namespace pedaleira::ui::paint

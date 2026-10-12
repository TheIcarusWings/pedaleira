// The canvas's own line icons: 24 px drawings, 1.75 stroke, round caps and
// joins. Each is its SVG path data, parsed once.
#pragma once

#include "Tokens.h"

#include <juce_graphics/juce_graphics.h>

namespace pedaleira::ui::icons {

enum class Icon {
  // The navigation rail.
  grid, library, capture, import, live, settings,
  // Small actions.
  check, plus, minus, close, chevronUp, chevronDown, chevronRight, arrowLeft,
  // The library and the importer.
  star, search, warning, info, attention, ear, lock, device, helix, document, pedaleiraPresets,
  // The capture wizard.
  capturePedal, captureAmpHead, captureAmpCab, captureModeller, play,
  // A block's on and off switch on its tile; the dock a block is dropped on
  // to remove it.
  power, trash,
};

const juce::Path& path(Icon icon);
const juce::Path& path(Category category);

// Strokes the icon into `area`, scaled from its 24 px box.
void draw(juce::Graphics& g, const juce::Path& icon, juce::Rectangle<float> area, juce::Colour colour,
          float stroke = size::iconStroke);

}  // namespace pedaleira::ui::icons

// Fonts and the type scale: Hanken Grotesk for all text, JetBrains Mono for
// every number the player reads or types. Both are
// embedded, so a page looks the same on every machine.
#pragma once

#include <juce_graphics/juce_graphics.h>

namespace pedaleira::ui::type {

enum class Weight { regular = 400, medium = 500, semibold = 600, bold = 700 };

// The embedded typefaces, made on first use. Not a process-lifetime static:
// a typeface holds platform font objects that must be released while JUCE
// is still up (on Windows inside DllMain would be too late), so the cache is
// a shared resource the UI's root holds for as long as it lives (the same
// rule as upstream's Fonts::Cache). Message thread only.
struct FontCache {
  juce::Typeface::Ptr faces[7];
};
using FontHold = juce::SharedResourcePointer<FontCache>;

juce::Font sans(float px, Weight weight = Weight::regular, bool italic = false);
juce::Font mono(float px, Weight weight = Weight::medium);

// The scale.
inline juce::Font display() { return sans(40.0f, Weight::bold); }
inline juce::Font title() { return sans(28.0f, Weight::bold); }
inline juce::Font heading() { return sans(20.0f, Weight::semibold); }
inline juce::Font body() { return sans(14.0f); }
inline juce::Font bodyStrong() { return sans(14.0f, Weight::semibold); }
inline juce::Font compact() { return sans(13.0f); }
inline juce::Font button() { return sans(13.0f, Weight::semibold); }
// Label 12 / 600 caps, 0.08 em tracking: draw the text upper-cased.
juce::Font label();
inline juce::Font value() { return mono(13.0f); }

// The middle dot the design uses as a separator ("Friday Covers Â· 012").
// Text with characters beyond ASCII goes through fromUTF8: a plain literal
// is read as Latin-1.
inline juce::String dot() { return juce::String::fromUTF8("\xc2\xb7"); }

}  // namespace pedaleira::ui::type

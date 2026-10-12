#include "Type.h"

#include "PedaleiraUiData.h"

#include <iterator>
#include <type_traits>

namespace pedaleira::ui::type {

namespace {

struct Face {
  const char* data;
  int size;
};

// One typeface per file, made on first use and kept in the shared cache
// (type::FontCache): typefaces are immutable and every font of the UI is one
// of these seven.
juce::Typeface::Ptr typeface(int index) {
  static const Face faces[] = {
      {PedaleiraUiData::HankenGrotesk400_ttf, PedaleiraUiData::HankenGrotesk400_ttfSize},
      {PedaleiraUiData::HankenGrotesk500_ttf, PedaleiraUiData::HankenGrotesk500_ttfSize},
      {PedaleiraUiData::HankenGrotesk600_ttf, PedaleiraUiData::HankenGrotesk600_ttfSize},
      {PedaleiraUiData::HankenGrotesk700_ttf, PedaleiraUiData::HankenGrotesk700_ttfSize},
      {PedaleiraUiData::HankenGrotesk500Italic_ttf, PedaleiraUiData::HankenGrotesk500Italic_ttfSize},
      {PedaleiraUiData::JetBrainsMono400_ttf, PedaleiraUiData::JetBrainsMono400_ttfSize},
      {PedaleiraUiData::JetBrainsMono500_ttf, PedaleiraUiData::JetBrainsMono500_ttfSize},
  };
  static_assert(std::size(faces) == std::extent_v<decltype(FontCache::faces)>);
  // A short hold of its own: the faces stay as long as anyone holds the
  // cache (the shell does, for the UI's lifetime).
  const FontHold cache;
  auto& slot = cache->faces[index];
  if (slot == nullptr)
    slot = juce::Typeface::createSystemTypefaceFor(faces[index].data,
                                                   static_cast<size_t>(faces[index].size));
  return slot;
}

}  // namespace

juce::Font sans(float px, Weight weight, bool italic) {
  // The design has one italic: the unsaved preset name, at 500.
  int index = 0;
  if (italic)
    index = 4;
  else if (weight == Weight::medium)
    index = 1;
  else if (weight == Weight::semibold)
    index = 2;
  else if (weight == Weight::bold)
    index = 3;
  return juce::Font(juce::FontOptions(typeface(index)).withPointHeight(px));
}

juce::Font mono(float px, Weight weight) {
  const int index = weight == Weight::regular ? 5 : 6;
  return juce::Font(juce::FontOptions(typeface(index)).withPointHeight(px));
}

juce::Font label() {
  const juce::Font font = sans(12.0f, Weight::semibold);
  // Kerning is relative to the JUCE height, the design's 0.08 em to the
  // point size.
  return font.withExtraKerningFactor(0.08f * font.getHeightToPointsFactor());
}

}  // namespace pedaleira::ui::type

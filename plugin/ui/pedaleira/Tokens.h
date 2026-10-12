// The Pedaleira design system as constants: the canvas's board "Design
// system". Every colour and size in the Pedaleira
// UI comes from here.
#pragma once

#include <juce_graphics/juce_graphics.h>

namespace pedaleira::ui {

namespace colour {
// Surfaces, darkest first.
inline const juce::Colour ground{0xff101214};
inline const juce::Colour surface{0xff181b1f};
inline const juce::Colour raised{0xff21252a};
inline const juce::Colour control{0xff2a2f36};
inline const juce::Colour line{0xff323841};
// The 2 px hard drop under raised parts.
inline const juce::Colour drop{0xff08090a};

inline const juce::Colour text{0xffeef0f2};
inline const juce::Colour muted{0xffa3acb6};
inline const juce::Colour faint{0xff5a626c};
inline const juce::Colour dim{0xff7c858f};

// The one accent: selection and the single primary action of a screen.
inline const juce::Colour iris{0xff8b7cff};
inline const juce::Colour irisHover{0xffa99dff};

inline const juce::Colour ok{0xff62d08f};
inline const juce::Colour warn{0xfff2c14e};
// Reserved for record and clipping.
inline const juce::Colour clip{0xffff5a5a};
}  // namespace colour

// Block categories: each owns one hue, used at 16 % as a tile's fill and at
// 100 % for its icon, and always paired with the icon.
enum class Category { amp, drive, cab, dynamics, eq, delay, reverb, modulation, pitch, filter, utility, plugin };

juce::Colour categoryColour(Category category);
// The tile fill: the hue at 16 % over the raised surface.
juce::Colour categoryTint(Category category);
juce::String categoryName(Category category);
// From the engine's category ids (kinds.list); unknown ones are utility.
Category categoryFromId(const juce::String& id);

namespace size {
inline constexpr int windowWidth = 1440;
inline constexpr int windowHeight = 900;
inline constexpr int railWidth = 72;
inline constexpr int headerHeight = 64;
inline constexpr float panelRadius = 10.0f;
inline constexpr float buttonRadius = 8.0f;
inline constexpr int buttonHeight = 32;
inline constexpr int tileWidth = 104;
inline constexpr int tileHeight = 76;
inline constexpr int footswitch = 72;
inline constexpr int footswitchGig = 96;
// Depth: a hard drop under raised parts and a highlight on their top edge.
inline constexpr float dropDepth = 2.0f;
inline constexpr float iconStroke = 1.75f;
}  // namespace size

}  // namespace pedaleira::ui

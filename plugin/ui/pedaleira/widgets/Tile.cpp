#include "Tile.h"

#include "../Icons.h"
#include "../Paint.h"
#include "../Type.h"

namespace pedaleira::ui {

// ####################
// TILE
// ####################
Tile::Tile() {
  setWantsKeyboardFocus(true);
  setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void Tile::setBlock(const juce::String& blockName, Category blockCategory, State blockState) {
  name = blockName;
  category = blockCategory;
  state = blockState;
  setTitle(blockName + ", " + categoryName(blockCategory));
  repaint();
}

void Tile::setEmpty() {
  name.clear();
  state = State::empty;
  footswitch.clear();
  setTitle("Empty slot");
  repaint();
}

void Tile::setSelected(bool isSelected) {
  selected = isSelected;
  repaint();
}

void Tile::setFootswitch(const juce::String& letter) {
  footswitch = letter;
  repaint();
}

void Tile::paint(juce::Graphics& g) {
  const auto area = getLocalBounds().toFloat().reduced(2.0f);
  const float radius = size::panelRadius;

  if (state == State::empty) {
    g.setColour(colour::ground);
    g.fillRoundedRectangle(area, radius);
    paint::dashedOutline(g, area, radius, colour::line);
    icons::draw(g, icons::path(icons::Icon::plus), area.withSizeKeepingCentre(20.0f, 20.0f), colour::faint);
  } else {
    const bool grey = state == State::bypassed || state == State::missing;
    const juce::Colour hue = grey ? colour::dim : categoryColour(category);
    if (grey) {
      g.setColour(colour::raised);
      g.fillRoundedRectangle(area, radius);
      paint::dashedOutline(g, area, radius, state == State::missing ? colour::warn : colour::faint);
    } else {
      paint::raised(g, area, radius, categoryTint(category));
    }
    if (state == State::loading)
      g.setOpacity(0.55f);

    // The grid's tiles are 96 wide and use the board's smaller text (12 and
    // 11); the design system's 104 wide ones use 14 and 13.
    const bool narrow = getWidth() < 100;
    const auto content = area.reduced(narrow ? 8.0f : 10.0f, 8.0f);
    icons::draw(g, icons::path(category), content.withSize(22.0f, 22.0f), hue);
    g.setFont(narrow ? type::sans(12.0f, type::Weight::semibold) : type::bodyStrong());
    g.setColour(grey ? colour::muted : colour::text);
    g.drawText(name, content.withTrimmedTop(narrow ? 30.0f : 26.0f).withHeight(16.0f),
               juce::Justification::centredLeft, true);
    g.setFont(narrow ? type::sans(11.0f) : type::compact());
    g.setColour(colour::muted);
    const juce::String sub = state == State::missing   ? juce::String("Missing")
                             : state == State::loading ? juce::String("Loading")
                                                       : categoryName(category);
    g.drawText(sub, content.withTrimmedTop(narrow ? 46.0f : 44.0f).withHeight(14.0f),
               juce::Justification::centredLeft, true);

    if (footswitch.isNotEmpty()) {
      const auto badge = juce::Rectangle<float>(18.0f, 18.0f).withPosition(area.getRight() - 24.0f, area.getY() + 6.0f);
      g.setColour(colour::ground.withAlpha(0.6f));
      g.fillRoundedRectangle(badge, 5.0f);
      g.setFont(type::mono(11.0f));
      g.setColour(colour::text);
      g.drawText(footswitch, badge, juce::Justification::centred, false);
    }
  }

  if (selected || hasKeyboardFocus(false)) {
    g.setColour(colour::iris);
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), radius + 1.0f, 2.0f);
  }
}

void Tile::mouseUp(const juce::MouseEvent& event) {
  if (event.mouseWasClicked() && onClick)
    onClick();
}

bool Tile::keyPressed(const juce::KeyPress& key) {
  if ((key == juce::KeyPress::returnKey || key == juce::KeyPress::spaceKey) && onClick) {
    onClick();
    return true;
  }
  return false;
}

// ####################
// FOOTSWITCH
// ####################
namespace {

// The measurements of each size, from the boards: the circle, its border,
// the room around it for the glow, the letter and the two text lines.
struct FootswitchMetrics {
  float diameter, border, drop, glow, letter, name, sub;
  int nameLine, subLine, gap;
  // The bank and tap switches name themselves in muted text.
  bool mutedName;
};

FootswitchMetrics metricsOf(Footswitch::Size size) {
  switch (size) {
    case Footswitch::Size::compact: return {56.0f, 3.0f, 3.0f, 4.0f, 18.0f, 12.0f, 11.0f, 15, 14, 6, false};
    case Footswitch::Size::gig: return {96.0f, 4.0f, 4.0f, 6.0f, 36.0f, 20.0f, 14.0f, 26, 18, 10, false};
    case Footswitch::Size::mini: return {44.0f, 3.0f, 3.0f, 0.0f, 12.0f, 11.0f, 11.0f, 14, 0, 4, true};
    case Footswitch::Size::utility: return {72.0f, 4.0f, 3.0f, 0.0f, 20.0f, 16.0f, 14.0f, 20, 0, 6, true};
    case Footswitch::Size::regular: break;
  }
  return {72.0f, 3.0f, 3.0f, 4.0f, 22.0f, 12.0f, 11.0f, 15, 14, 8, false};
}

}  // namespace

Footswitch::Footswitch(const juce::String& switchLetter) : letter(switchLetter) {
  setTitle("Footswitch " + switchLetter);
  setMouseCursor(juce::MouseCursor::PointingHandCursor);
  setWantsKeyboardFocus(true);
}

int Footswitch::heightFor(Size size) {
  const FootswitchMetrics m = metricsOf(size);
  return juce::roundToInt(m.glow + m.diameter + m.drop) + m.gap + m.nameLine + m.subLine;
}

void Footswitch::setSize(Size newSize) {
  sizeClass = newSize;
  repaint();
}

void Footswitch::setLabel(const juce::String& newLabel) {
  label = newLabel;
  setTitle("Footswitch " + letter + (label.isNotEmpty() ? ", " + label : juce::String()));
  repaint();
}

void Footswitch::setSub(const juce::String& newSub) {
  sub = newSub;
  repaint();
}

void Footswitch::setNumber(const juce::String& newNumber) {
  number = newNumber;
  repaint();
}

void Footswitch::setText(const juce::String& newText) {
  text = newText;
  repaint();
}

void Footswitch::setIcon(const juce::Path* iconPath) {
  icon = iconPath;
  repaint();
}

void Footswitch::setLook(Look newLook, juce::Colour ringColour) {
  look = newLook;
  ring = ringColour;
  repaint();
}

void Footswitch::setBlinkOn(bool on) {
  blink = on;
  repaint();
}

void Footswitch::setSelected(bool isSelected) {
  selected = isSelected;
  repaint();
}

void Footswitch::paint(juce::Graphics& g) {
  const FootswitchMetrics m = metricsOf(sizeClass);
  const auto circle = juce::Rectangle<float>(m.diameter, m.diameter)
                          .withCentre({getWidth() * 0.5f, m.glow + m.diameter * 0.5f});
  const bool filled = look == Look::active || (look == Look::tap && blink);
  const bool dashed = look == Look::empty || look == Look::tap;
  const juce::Colour borderColour = look == Look::stomp    ? ring
                                    : look == Look::active ? colour::iris
                                    : look == Look::empty  ? colour::faint
                                    : look == Look::tap    ? colour::muted
                                                           : colour::line;

  // A lit stomp switch glows: its colour at 18 % in a ring around it.
  if (look == Look::stomp) {
    g.setColour(ring.withAlpha(0.18f));
    g.drawEllipse(circle.expanded(m.glow * 0.5f), m.glow);
  }
  if (look != Look::empty) {
    g.setColour(colour::drop);
    g.fillEllipse(circle.translated(0.0f, m.drop));
  }
  g.setColour(filled ? colour::iris : look == Look::empty ? colour::surface : colour::control);
  g.fillEllipse(circle);
  if (look != Look::empty && !filled) {
    // The 1 px highlight along the top edge.
    juce::Graphics::ScopedSaveState state(g);
    g.reduceClipRegion(circle.withHeight(m.border + 4.0f).toNearestInt());
    g.setColour(juce::Colours::white.withAlpha(0.08f));
    g.drawEllipse(circle.reduced(m.border), 1.0f);
  }
  if (dashed) {
    juce::Path outline;
    outline.addEllipse(circle.reduced(m.border * 0.5f));
    juce::Path stroke;
    const float dashes[] = {m.border * 2.0f, m.border * 1.6f};
    juce::PathStrokeType(m.border).createDashedStroke(stroke, outline, dashes, 2);
    g.setColour(borderColour);
    g.fillPath(stroke);
  } else {
    g.setColour(borderColour);
    g.drawEllipse(circle.reduced(m.border * 0.5f), m.border);
  }
  if (selected || hasKeyboardFocus(false)) {
    g.setColour(colour::iris);
    g.drawEllipse(circle.expanded(m.border * 0.5f + 3.0f), 3.0f);
  }

  // Icons and the tap switch read in full text; a plain switch's letter is muted.
  const bool plain = (look == Look::off || look == Look::empty) && icon == nullptr;
  const juce::Colour ink = filled ? colour::ground : plain ? colour::muted : colour::text;
  if (icon != nullptr) {
    icons::draw(g, *icon, circle.withSizeKeepingCentre(m.diameter * 0.42f, m.diameter * 0.42f), ink);
  } else {
    const juce::String shown = text.isNotEmpty() ? text : letter;
    const bool bigTap = sizeClass == Size::mini || sizeClass == Size::utility;
    g.setFont(look == Look::tap ? type::mono(bigTap ? m.letter : m.letter * 0.7f)
                                : type::sans(m.letter, type::Weight::bold));
    g.setColour(ink);
    g.drawText(shown, circle, juce::Justification::centred, false);
  }

  // The name, then the state line.
  auto lines = getLocalBounds().withTrimmedTop(juce::roundToInt(m.glow + m.diameter + m.drop) + m.gap);
  const juce::Colour nameColour = look == Look::empty || m.mutedName ? colour::muted : colour::text;
  const juce::Font nameFont =
      type::sans(m.name, m.mutedName && sizeClass == Size::mini ? type::Weight::regular : type::Weight::semibold);
  const juce::Font numberFont = type::mono(m.name);
  const auto textArea = lines.withHeight(m.nameLine + (sub.isEmpty() ? m.subLine : 0));
  auto nameLine = lines.removeFromTop(m.nameLine);
  if (label.isNotEmpty() || number.isNotEmpty()) {
    // The measurements carry a few pixels over: a line cut to the exact width
    // would be curtailed with dots.
    const int numberWidth =
        number.isNotEmpty() ? juce::roundToInt(juce::GlyphArrangement::getStringWidth(numberFont, number)) + 6 : 0;
    const int labelWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(nameFont, label)) + 6;
    if (sizeClass == Size::compact && sub.isEmpty() && numberWidth + labelWidth > nameLine.getWidth()) {
      // A long name takes the state line's room and wraps.
      juce::Graphics::ScopedSaveState state(g);
      g.reduceClipRegion(textArea);
      juce::AttributedString wrapped;
      wrapped.setJustification(juce::Justification::centredTop);
      wrapped.setWordWrap(juce::AttributedString::byWord);
      if (number.isNotEmpty())
        wrapped.append(number + " ", numberFont, colour::muted);
      wrapped.append(label, nameFont, nameColour);
      wrapped.draw(g, textArea.toFloat());
    } else if (number.isNotEmpty()) {
      const int total = juce::jmin(nameLine.getWidth(), numberWidth + labelWidth);
      auto row = nameLine.withSizeKeepingCentre(total, nameLine.getHeight());
      g.setFont(numberFont);
      g.setColour(colour::muted);
      g.drawText(number, row.removeFromLeft(numberWidth), juce::Justification::centredLeft, false);
      g.setFont(nameFont);
      g.setColour(nameColour);
      g.drawText(label, row, juce::Justification::centredLeft, true);
    } else {
      g.setFont(nameFont);
      g.setColour(nameColour);
      g.drawText(label, nameLine, juce::Justification::centred, true);
    }
  }
  if (sub.isNotEmpty()) {
    auto subLine = lines.removeFromTop(m.subLine);
    if (sizeClass == Size::gig) {
      paint::label(g, sub, subLine, juce::Justification::centred);
    } else {
      g.setFont(type::sans(m.sub));
      g.setColour(colour::muted);
      g.drawText(sub, subLine, juce::Justification::centred, true);
    }
  }
}

void Footswitch::mouseDown(const juce::MouseEvent&) {
  if (onPress)
    onPress();
}

void Footswitch::mouseUp(const juce::MouseEvent&) {
  if (onRelease)
    onRelease();
}

bool Footswitch::keyPressed(const juce::KeyPress& key) {
  if (key == juce::KeyPress::returnKey || key == juce::KeyPress::spaceKey) {
    if (onPress)
      onPress();
    if (onRelease)
      onRelease();
    return true;
  }
  return false;
}

}  // namespace pedaleira::ui

#include "Knob.h"

#include "../Paint.h"
#include "../Type.h"

namespace pedaleira::ui {

namespace {
// The arc runs from 225 degrees clockwise through 270 degrees (from the
// lower left, over the top, to the lower right).
constexpr float kStartAngle = juce::MathConstants<float>::pi * 1.25f;
constexpr float kSweep = juce::MathConstants<float>::pi * 1.5f;
// Pixels of vertical drag for the whole arc.
constexpr double kDragPixels = 200.0;
constexpr int kNameHeight = 18;
constexpr int kValueHeight = 18;
constexpr int kTagHeight = 14;
}  // namespace

// ####################
// KNOB
// ####################
Knob::Knob(const juce::String& name) {
  setName(name);
  setTitle(name);
  setWantsKeyboardFocus(true);
  setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
}

void Knob::setPosition(double newPosition, juce::NotificationType notification) {
  newPosition = juce::jlimit(0.0, 1.0, newPosition);
  if (newPosition == position)
    return;
  position = newPosition;
  repaint();
  if (notification != juce::dontSendNotification && onChange)
    onChange(position);
}

void Knob::setValueText(const juce::String& text) {
  valueText = text;
  repaint();
}

void Knob::setAssignment(Assignment newAssignment) {
  assignment = newAssignment;
  repaint();
}

void Knob::setTag(const juce::String& text, juce::Colour tint) {
  tagText = text;
  tagColour = tint;
  repaint();
}

void Knob::setBypassed(bool isBypassed) {
  bypassed = isBypassed;
  repaint();
}

void Knob::setDiameter(int newDiameter) {
  diameter = newDiameter;
  repaint();
}

void Knob::paint(juce::Graphics& g) {
  const float alpha = bypassed ? 0.45f : 1.0f;
  // The knob's box, as the canvas draws it in a 64 px view box: the arc at
  // radius 26, the cap at 18.
  const float box = static_cast<float>(diameter);
  // The knob on top, its name and value under it (the grid editor's board).
  const auto knobArea = juce::Rectangle<float>(box, box).withCentre({getWidth() * 0.5f, box * 0.5f});
  const float radius = box * 26.0f / 64.0f;
  const juce::Point<float> centre = knobArea.getCentre();

  juce::Colour arc = colour::text;
  if (assignment == Assignment::scene)
    arc = colour::iris;
  else if (assignment == Assignment::expression)
    arc = colour::ok;

  const juce::PathStrokeType stroke(box * 4.0f / 64.0f, juce::PathStrokeType::curved,
                                    juce::PathStrokeType::rounded);
  juce::Path track;
  track.addCentredArc(centre.x, centre.y, radius, radius, 0.0f, kStartAngle - juce::MathConstants<float>::twoPi,
                      kStartAngle - juce::MathConstants<float>::twoPi + kSweep, true);
  g.setColour(colour::line.withMultipliedAlpha(alpha));
  g.strokePath(track, stroke);
  const float valueAngle = kStartAngle - juce::MathConstants<float>::twoPi + kSweep * static_cast<float>(position);
  if (position > 0.0) {
    juce::Path value;
    value.addCentredArc(centre.x, centre.y, radius, radius, 0.0f,
                        kStartAngle - juce::MathConstants<float>::twoPi, valueAngle, true);
    g.setColour(arc.withMultipliedAlpha(alpha));
    g.strokePath(value, stroke);
  }

  // The cap, with its drop, and the pointer.
  const float cap = box * 18.0f / 64.0f;
  g.setColour(colour::drop);
  g.fillEllipse(juce::Rectangle<float>(cap * 2.0f, cap * 2.0f).withCentre(centre.translated(0.0f, box * 1.5f / 64.0f)));
  g.setColour(colour::control.withMultipliedAlpha(bypassed ? 0.7f : 1.0f));
  g.fillEllipse(juce::Rectangle<float>(cap * 2.0f, cap * 2.0f).withCentre(centre));
  const juce::Point<float> inner = centre.getPointOnCircumference(cap * 7.0f / 18.0f, valueAngle);
  const juce::Point<float> outer = centre.getPointOnCircumference(cap * 15.0f / 18.0f, valueAngle);
  g.setColour(arc.withMultipliedAlpha(alpha));
  g.drawLine({inner, outer}, box * 2.5f / 64.0f);

  if (hasKeyboardFocus(false)) {
    g.setColour(colour::iris);
    g.drawEllipse(knobArea.reduced(1.0f), 1.5f);
  }

  auto text = getLocalBounds().withTrimmedTop(juce::roundToInt(box) + 4);
  g.setFont(type::sans(12.0f, type::Weight::semibold));
  g.setColour((bypassed ? colour::muted : colour::text).withMultipliedAlpha(alpha));
  g.drawText(getName(), text.removeFromTop(kNameHeight), juce::Justification::centred, true);
  g.setFont(type::mono(12.0f));
  g.setColour(colour::muted.withMultipliedAlpha(alpha));
  g.drawText(valueText, text.removeFromTop(kValueHeight), juce::Justification::centred, true);
  if (tagText.isNotEmpty()) {
    g.setFont(type::sans(11.0f));
    g.setColour(tagColour.withMultipliedAlpha(alpha));
    g.drawText(tagText, text.removeFromTop(kTagHeight), juce::Justification::centred, true);
  }
}

void Knob::mouseDown(const juce::MouseEvent& event) {
  if (event.mods.isPopupMenu()) {
    if (onContextMenu)
      onContextMenu();
    return;
  }
  dragStart = position;
}

void Knob::mouseDrag(const juce::MouseEvent& event) {
  if (event.mods.isPopupMenu())
    return;
  const double pixels = event.mods.isShiftDown() ? kDragPixels * 5.0 : kDragPixels;
  setPosition(dragStart - event.getDistanceFromDragStartY() / pixels, juce::sendNotification);
}

void Knob::mouseDoubleClick(const juce::MouseEvent&) { setPosition(defaultPosition, juce::sendNotification); }

bool Knob::keyPressed(const juce::KeyPress& key) {
  const double step = key.getModifiers().isShiftDown() ? 0.002 : 0.02;
  if (key == juce::KeyPress::upKey || key == juce::KeyPress::rightKey) {
    setPosition(position + step, juce::sendNotification);
    return true;
  }
  if (key == juce::KeyPress::downKey || key == juce::KeyPress::leftKey) {
    setPosition(position - step, juce::sendNotification);
    return true;
  }
  return false;
}

std::unique_ptr<juce::AccessibilityHandler> Knob::createAccessibilityHandler() {
  struct Value final : juce::AccessibilityRangedNumericValueInterface {
    explicit Value(Knob& owner) : knob(owner) {}
    bool isReadOnly() const override { return false; }
    double getCurrentValue() const override { return knob.position; }
    void setValue(double newValue) override { knob.setPosition(newValue, juce::sendNotification); }
    AccessibleValueRange getRange() const override { return {{0.0, 1.0}, 0.02}; }
    Knob& knob;
  };
  return std::make_unique<juce::AccessibilityHandler>(
      *this, juce::AccessibilityRole::slider, juce::AccessibilityActions(),
      juce::AccessibilityHandler::Interfaces{std::make_unique<Value>(*this)});
}

// ####################
// LEVEL METER
// ####################
void LevelMeter::setLevel(float db, float peakHoldDb) {
  if (db == level && peakHoldDb == hold)
    return;
  level = db;
  hold = peakHoldDb;
  repaint();
}

void LevelMeter::paint(juce::Graphics& g) { paintBar(g, getLocalBounds().toFloat(), level, hold); }

void LevelMeter::paintBar(juce::Graphics& g, juce::Rectangle<float> area, float level, float hold) {
  const float radius = juce::jmin(4.0f, area.getHeight() * 0.5f);
  g.setColour(colour::raised);
  g.fillRoundedRectangle(area, radius);
  const auto at = [&area](float db) {
    return area.getX() + area.getWidth() * juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);
  };
  {
    // Green up to -12 dB, amber to -3 dB, red above.
    juce::Graphics::ScopedSaveState state(g);
    g.reduceClipRegion(area.withRight(at(level)).toNearestInt());
    juce::Path body;
    body.addRoundedRectangle(area, radius);
    g.reduceClipRegion(body);
    g.setColour(colour::ok);
    g.fillRect(area.withRight(at(-12.0f)));
    g.setColour(colour::warn);
    g.fillRect(area.withLeft(at(-12.0f)).withRight(at(-3.0f)));
    g.setColour(colour::clip);
    g.fillRect(area.withLeft(at(-3.0f)));
  }
  if (hold > -60.0f) {
    g.setColour(colour::text);
    g.fillRect(juce::Rectangle<float>(at(hold) - 1.0f, area.getY(), 2.0f, area.getHeight()));
  }
}

}  // namespace pedaleira::ui

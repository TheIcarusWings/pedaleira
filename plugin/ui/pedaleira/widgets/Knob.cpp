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

void Knob::setCap(Cap newCap) {
  cap = newCap;
  repaint();
}

void Knob::setTextShown(bool shown) {
  textShown = shown;
  repaint();
}

void Knob::setTagShown(bool shown) {
  tagShown = shown;
  repaint();
}

// A fader is drawn in a 44 x 112 box scaled by diameter; the knobs in a square one.
int Knob::visualHeight(Cap cap, int diameter) {
  return cap == Cap::fader ? juce::roundToInt(static_cast<float>(diameter) * 1.7f) : diameter;
}

int Knob::cellHeight(Cap cap, int diameter, bool withTag) {
  return visualHeight(cap, diameter) + 4 + kNameHeight + kValueHeight + (withTag ? kTagHeight : 0);
}

void Knob::setDiameter(int newDiameter) {
  diameter = newDiameter;
  repaint();
}

void Knob::setAccent(juce::Colour newAccent) {
  if (accent == newAccent)
    return;
  accent = newAccent;
  repaint();
}

void Knob::paint(juce::Graphics& g) {
  const float alpha = bypassed ? 0.45f : 1.0f;
  // The knob's box, as the canvas draws it in a 64 px view box: the arc at
  // radius 26, the cap at 18.
  const float box = static_cast<float>(diameter);
  const float visual = static_cast<float>(visualHeight(cap, diameter));
  // The knob on top, its name and value under it (the grid editor's board).
  const auto knobArea = juce::Rectangle<float>(box, box).withCentre({getWidth() * 0.5f, box * 0.5f});
  const float radius = box * 26.0f / 64.0f;
  const juce::Point<float> centre = knobArea.getCentre();

  juce::Colour arc = accent;
  if (assignment == Assignment::scene)
    arc = colour::iris;
  else if (assignment == Assignment::expression)
    arc = colour::ok;

  const float lit = bypassed ? 0.0f : hover.value(), active = bypassed ? 0.0f : turning.value();
  if (cap == Cap::fader) {
    paintFader(g, arc, alpha, lit, active);
  } else {
    // A soft halo behind the knob while the mouse is on it.
    if (lit > 0.0f) {
      g.setColour(arc.withAlpha(0.07f * lit + 0.05f * active));
      g.fillEllipse(knobArea.reduced(box * 2.0f / 64.0f));
    }
    const juce::PathStrokeType stroke(box * (4.0f + 0.75f * active) / 64.0f, juce::PathStrokeType::curved,
                                      juce::PathStrokeType::rounded);
    juce::Path track;
    track.addCentredArc(centre.x, centre.y, radius, radius, 0.0f, kStartAngle - juce::MathConstants<float>::twoPi,
                        kStartAngle - juce::MathConstants<float>::twoPi + kSweep, true);
    g.setColour(colour::line.interpolatedWith(colour::dim, 0.5f * lit).withMultipliedAlpha(alpha));
    g.strokePath(track, stroke);
    const float valueAngle = kStartAngle - juce::MathConstants<float>::twoPi + kSweep * static_cast<float>(position);
    if (position > 0.0) {
      juce::Path value;
      value.addCentredArc(centre.x, centre.y, radius, radius, 0.0f,
                          kStartAngle - juce::MathConstants<float>::twoPi, valueAngle, true);
      g.setColour(arc.withMultipliedAlpha(alpha));
      g.strokePath(value, stroke);
    }
    paintCap(g, centre, box, valueAngle, arc, alpha, lit, active);
  }

  if (hasKeyboardFocus(false)) {
    g.setColour(colour::iris);
    if (cap == Cap::fader)
      g.drawRoundedRectangle(getLocalBounds().toFloat().withHeight(visual).reduced(1.0f), 6.0f, 1.5f);
    else
      g.drawEllipse(knobArea.reduced(1.0f), 1.5f);
  }

  if (!textShown)
    return;
  auto text = getLocalBounds().withTrimmedTop(juce::roundToInt(visual) + 4);
  g.setFont(type::sans(12.0f, type::Weight::semibold));
  g.setColour((bypassed ? colour::muted : colour::text).withMultipliedAlpha(alpha));
  g.drawText(getName(), text.removeFromTop(kNameHeight), juce::Justification::centred, true);
  // The value brightens while the mouse is on the knob, and takes the arc's
  // colour while it turns.
  g.setFont(type::mono(12.0f));
  g.setColour(colour::muted.interpolatedWith(colour::text, lit).interpolatedWith(arc, active).withMultipliedAlpha(alpha));
  g.drawText(valueText, text.removeFromTop(kValueHeight), juce::Justification::centred, true);
  if (tagShown && tagText.isNotEmpty()) {
    g.setFont(type::sans(11.0f));
    g.setColour(tagColour.withMultipliedAlpha(alpha));
    g.drawText(tagText, text.removeFromTop(kTagHeight), juce::Justification::centred, true);
  }
}

// The cap, with its drop, and the pointer, in the shape the knob has.
void Knob::paintCap(juce::Graphics& g, juce::Point<float> centre, float box, float angle, juce::Colour arc, float alpha,
                    float lit, float active) const {
  const float unit = box / 64.0f;
  const float grow = 1.0f * active;
  const juce::Colour capColour =
      colour::control.interpolatedWith(colour::line, 0.6f * lit).withMultipliedAlpha(bypassed ? 0.7f : 1.0f);
  const auto rotate = juce::AffineTransform::rotation(angle, centre.x, centre.y);
  const auto polygon = [&](std::initializer_list<juce::Point<float>> points) {
    juce::Path path;
    bool first = true;
    for (const auto& p : points) {
      const auto at = juce::Point<float>(centre.x + p.x * unit, centre.y + p.y * unit);
      if (first)
        path.startNewSubPath(at);
      else
        path.lineTo(at);
      first = false;
    }
    path.closeSubPath();
    return path;
  };
  switch (cap) {
    case Cap::chickenHead: {
      // The whole cap turns: a teardrop with its point out, and a pointer line.
      const float r = (15.0f + grow) * unit;
      auto tip = polygon({{0.0f, -24.0f}, {10.0f, 0.0f}, {-10.0f, 0.0f}});
      tip.applyTransform(rotate);
      g.setColour(colour::drop);
      g.fillPath(tip, juce::AffineTransform::translation(0.0f, 1.5f * unit));
      g.fillEllipse(juce::Rectangle<float>(r * 2.0f, r * 2.0f).withCentre(centre.translated(0.0f, 1.5f * unit)));
      g.setColour(capColour);
      g.fillPath(tip);
      g.fillEllipse(juce::Rectangle<float>(r * 2.0f, r * 2.0f).withCentre(centre));
      g.setColour(arc.withMultipliedAlpha(alpha));
      g.drawLine({centre.getPointOnCircumference(10.0f * unit, angle), centre.getPointOnCircumference(19.0f * unit, angle)},
                 2.5f * unit);
      break;
    }
    case Cap::pointer:
    case Cap::round:
    default: {
      const float r = (18.0f + grow) * unit;
      g.setColour(colour::drop);
      g.fillEllipse(juce::Rectangle<float>(r * 2.0f, r * 2.0f).withCentre(centre.translated(0.0f, 1.5f * unit)));
      g.setColour(capColour);
      g.fillEllipse(juce::Rectangle<float>(r * 2.0f, r * 2.0f).withCentre(centre));
      g.setColour(arc.withMultipliedAlpha(alpha));
      if (cap == Cap::pointer) {
        // A wedge from the middle of the cap out to its rim.
        auto wedge = polygon({{0.0f, -20.0f}, {6.0f, -6.0f}, {-6.0f, -6.0f}});
        wedge.applyTransform(rotate);
        g.fillPath(wedge);
      } else {
        const float scale = r / (18.0f * unit);
        g.drawLine({centre.getPointOnCircumference(7.0f * unit * scale, angle),
                    centre.getPointOnCircumference(15.0f * unit * scale, angle)},
                   box * 2.5f / 64.0f);
      }
      break;
    }
  }
}

// A vertical slider: a groove, the part below the thumb in the accent, the thumb.
void Knob::paintFader(juce::Graphics& g, juce::Colour arc, float alpha, float lit, float active) const {
  const float height = static_cast<float>(visualHeight(Cap::fader, diameter));
  const float unit = height / 112.0f;
  const float centreX = getWidth() * 0.5f;
  const float left = centreX - 22.0f * unit;
  const float top = 8.0f * unit, travel = 96.0f * unit;
  if (lit > 0.0f) {
    g.setColour(arc.withAlpha(0.07f * lit + 0.05f * active));
    g.fillRoundedRectangle(juce::Rectangle<float>(left + 2.0f * unit, 0.0f, 40.0f * unit, height), 10.0f * unit);
  }
  g.setColour(colour::line.interpolatedWith(colour::dim, 0.5f * lit).withMultipliedAlpha(alpha));
  g.fillRoundedRectangle(juce::Rectangle<float>(left + 19.0f * unit, top, 6.0f * unit, travel), 3.0f * unit);
  const float thumbCentre = top + travel * (1.0f - static_cast<float>(position));
  g.setColour(arc.withMultipliedAlpha(alpha));
  g.fillRoundedRectangle(juce::Rectangle<float>(left + 19.0f * unit, thumbCentre, 6.0f * unit, top + travel - thumbCentre), 3.0f * unit);
  const float thumbHeight = (16.0f + active) * unit;
  const auto thumb = juce::Rectangle<float>(32.0f * unit, thumbHeight).withCentre({centreX, thumbCentre});
  g.setColour(colour::drop);
  g.fillRoundedRectangle(thumb.translated(0.0f, 1.5f * unit), 4.0f * unit);
  g.setColour(colour::control.interpolatedWith(colour::line, 0.6f * lit).withMultipliedAlpha(bypassed ? 0.7f : 1.0f));
  g.fillRoundedRectangle(thumb, 4.0f * unit);
  g.setColour(arc.withMultipliedAlpha(alpha));
  g.drawLine({centreX - 10.0f * unit, thumbCentre, centreX + 10.0f * unit, thumbCentre}, 2.5f * unit);
}

void Knob::mouseEnter(const juce::MouseEvent&) {
  hover.setTarget(1.0f);
  if (onHotChanged)
    onHotChanged();
}

void Knob::mouseExit(const juce::MouseEvent&) {
  if (!isMouseButtonDown())
    hover.setTarget(0.0f);
  if (onHotChanged)
    onHotChanged();
}

void Knob::mouseDown(const juce::MouseEvent& event) {
  if (event.mods.isPopupMenu()) {
    if (onContextMenu)
      onContextMenu();
    return;
  }
  dragStart = position;
  turning.setTarget(1.0f);
  if (onHotChanged)
    onHotChanged();
}

void Knob::mouseUp(const juce::MouseEvent&) {
  turning.setTarget(0.0f);
  if (!isMouseOver())
    hover.setTarget(0.0f);
  if (onHotChanged)
    onHotChanged();
}

void Knob::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) {
  // A wheel or trackpad turns the knob too: shift for fine steps.
  const float delta = std::abs(wheel.deltaY) > std::abs(wheel.deltaX) ? wheel.deltaY : -wheel.deltaX;
  if (delta == 0.0f)
    return;
  const double scale = event.mods.isShiftDown() ? 0.05 : 0.25;
  setPosition(position + (wheel.isReversed ? -delta : delta) * scale, juce::sendNotification);
}

void Knob::mouseDrag(const juce::MouseEvent& event) {
  if (event.mods.isPopupMenu())
    return;
  // A fader's whole travel is the height of its groove.
  const double travel = cap == Cap::fader ? 96.0 / 112.0 * visualHeight(Cap::fader, diameter) : kDragPixels;
  const double pixels = event.mods.isShiftDown() ? travel * 5.0 : travel;
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

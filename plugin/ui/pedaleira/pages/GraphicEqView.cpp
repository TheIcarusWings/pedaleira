#include "GraphicEqView.h"

#include "../Paint.h"
#include "../Type.h"

#include <cmath>
#include <complex>

namespace pedaleira::ui {

namespace {
// As the block runs them (effects/eq/GraphicEq10.cpp).
constexpr double kCentreHz[GraphicEqView::kBands] = {31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0};
constexpr double kBandQ = 1.6;
constexpr double kRangeDb = 12.0;
constexpr double kSampleRate = 48000.0;
constexpr float kLabelHeight = 22.0f;
constexpr float kScaleWidth = 34.0f;
}  // namespace

GraphicEqView::GraphicEqView() {
  setTitle("Graphic equaliser");
  setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
}

juce::String GraphicEqView::bandLabel(int band) {
  const double hz = kCentreHz[band];
  if (hz >= 1000.0)
    return juce::String(juce::roundToInt(hz / 1000.0)) + "k";
  return hz < 40.0 ? juce::String(hz, 1) : juce::String(juce::roundToInt(hz));
}

void GraphicEqView::setGain(int band, double db) {
  if (band < 0 || band >= kBands || dragging == band)
    return;
  if (gains[static_cast<size_t>(band)] != db) {
    gains[static_cast<size_t>(band)] = db;
    repaint();
  }
}

void GraphicEqView::setBypassed(bool isBypassed) {
  bypassed = isBypassed;
  repaint();
}

juce::Rectangle<float> GraphicEqView::plot() const {
  return getLocalBounds().toFloat().withTrimmedLeft(kScaleWidth).withTrimmedBottom(kLabelHeight).reduced(8.0f, 10.0f);
}

float GraphicEqView::bandX(int band) const {
  const auto area = plot();
  return area.getX() + area.getWidth() * (static_cast<float>(band) + 0.5f) / static_cast<float>(kBands);
}

float GraphicEqView::yOf(double db) const {
  const auto area = plot();
  return area.getCentreY() - static_cast<float>(db / kRangeDb) * area.getHeight() * 0.5f;
}

double GraphicEqView::dbAt(float y) const {
  const auto area = plot();
  return juce::jlimit(-kRangeDb, kRangeDb, (area.getCentreY() - y) / (area.getHeight() * 0.5) * kRangeDb);
}

int GraphicEqView::bandAt(juce::Point<float> position) const {
  const auto area = plot();
  if (position.x < area.getX() || position.x > area.getRight())
    return -1;
  return juce::jlimit(0, kBands - 1,
                      static_cast<int>((position.x - area.getX()) / area.getWidth() * static_cast<float>(kBands)));
}

double GraphicEqView::responseDb(double hz) const {
  // RBJ peaking filters, the band's gain at its centre.
  double total = 0.0;
  const std::complex<double> z = std::polar(1.0, -juce::MathConstants<double>::twoPi * hz / kSampleRate);
  for (int b = 0; b < kBands; ++b) {
    const double gain = gains[static_cast<size_t>(b)];
    if (std::abs(gain) < 1.0e-3)
      continue;
    const double a = std::pow(10.0, gain / 40.0);
    const double w0 = juce::MathConstants<double>::twoPi * kCentreHz[b] / kSampleRate;
    const double alpha = std::sin(w0) / (2.0 * kBandQ);
    const double c = std::cos(w0);
    const std::complex<double> num = (1.0 + alpha * a) + (-2.0 * c) * z + (1.0 - alpha * a) * z * z;
    const std::complex<double> den = (1.0 + alpha / a) + (-2.0 * c) * z + (1.0 - alpha / a) * z * z;
    total += 20.0 * std::log10(std::abs(num / den));
  }
  return total;
}

void GraphicEqView::paint(juce::Graphics& g) {
  const auto area = plot();
  const float alpha = bypassed ? 0.45f : 1.0f;

  // The scale: 0 dB, +-6 and +-12.
  g.setFont(type::mono(10.0f));
  for (const double db : {12.0, 6.0, 0.0, -6.0, -12.0}) {
    const float y = yOf(db);
    g.setColour((db == 0.0 ? colour::line.interpolatedWith(colour::dim, 0.4f) : colour::line).withMultipliedAlpha(alpha));
    g.fillRect(juce::Rectangle<float>(area.getX(), y - 0.5f, area.getWidth(), 1.0f));
    g.setColour(colour::faint.withMultipliedAlpha(alpha));
    g.drawText((db > 0 ? "+" : "") + juce::String(juce::roundToInt(db)), juce::Rectangle<float>(0.0f, y - 7.0f, kScaleWidth - 4.0f, 14.0f),
               juce::Justification::centredRight, false);
  }

  // The response, filled toward 0 dB, across the bands' span (an octave
  // either side of the outer bands).
  juce::Path curve, fill;
  const double lowHz = kCentreHz[0] / std::sqrt(2.0), highHz = juce::jmin(20000.0, kCentreHz[kBands - 1] * std::sqrt(2.0));
  const int points = 180;
  for (int i = 0; i <= points; ++i) {
    const double t = static_cast<double>(i) / points;
    const double hz = lowHz * std::pow(highHz / lowHz, t);
    // The bands sit an octave apart, evenly across the plot.
    const float x = area.getX() + area.getWidth() *
                                      static_cast<float>((std::log2(hz / kCentreHz[0]) + 0.5) / static_cast<double>(kBands));
    const float y = yOf(juce::jlimit(-kRangeDb * 1.25, kRangeDb * 1.25, responseDb(hz)));
    if (i == 0) {
      curve.startNewSubPath(x, y);
      fill.startNewSubPath(x, yOf(0.0));
      fill.lineTo(x, y);
    } else {
      curve.lineTo(x, y);
      fill.lineTo(x, y);
    }
    if (i == points)
      fill.lineTo(x, yOf(0.0));
  }
  fill.closeSubPath();
  {
    juce::Graphics::ScopedSaveState state(g);
    g.reduceClipRegion(area.toNearestInt());
    g.setColour(colour::iris.withAlpha(0.14f * alpha));
    g.fillPath(fill);
    g.setColour(colour::iris.withMultipliedAlpha(alpha));
    g.strokePath(curve, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
  }

  // The bands: a track, a handle at the band's gain, its label under the plot.
  for (int b = 0; b < kBands; ++b) {
    const float x = bandX(b);
    const bool active = b == dragging || (dragging < 0 && b == hovered);
    const float glow = active ? (dragging == b ? 1.0f : lit.value()) : 0.0f;
    if (glow > 0.0f) {
      g.setColour(colour::text.withAlpha(0.04f * glow));
      g.fillRoundedRectangle(juce::Rectangle<float>(area.getWidth() / kBands - 6.0f, area.getHeight()).withCentre({x, area.getCentreY()}), 6.0f);
    }
    g.setColour(colour::control.interpolatedWith(colour::line, glow).withMultipliedAlpha(alpha));
    g.fillRoundedRectangle(juce::Rectangle<float>(4.0f, area.getHeight()).withCentre({x, area.getCentreY()}), 2.0f);
    const double gain = gains[static_cast<size_t>(b)];
    const float y = yOf(gain);
    // The stretch from 0 dB to the gain, in iris.
    g.setColour(colour::iris.withAlpha(0.6f * alpha));
    g.fillRoundedRectangle(juce::Rectangle<float>(4.0f, std::abs(y - yOf(0.0))).withPosition(x - 2.0f, juce::jmin(y, yOf(0.0))), 2.0f);
    const auto handle = juce::Rectangle<float>(26.0f + 4.0f * glow, 12.0f + 2.0f * glow).withCentre({x, y});
    g.setColour(colour::drop);
    g.fillRoundedRectangle(handle.translated(0.0f, 2.0f), 4.0f);
    g.setColour(colour::text.interpolatedWith(colour::irisHover, 0.5f * glow).withMultipliedAlpha(alpha));
    g.fillRoundedRectangle(handle, 4.0f);
    g.setColour(colour::ground.withMultipliedAlpha(alpha));
    g.fillRect(juce::Rectangle<float>(10.0f, 1.5f).withCentre(handle.getCentre()));
    // The value above the handle while it is in use.
    if (glow > 0.0f) {
      g.setFont(type::mono(11.0f));
      g.setColour(colour::text.withAlpha(glow * alpha));
      const juce::String value = std::abs(gain) < 0.05 ? juce::String("0.0") : (gain > 0 ? "+" : "") + juce::String(gain, 1);
      g.drawText(value, juce::Rectangle<float>(48.0f, 14.0f).withCentre({x, handle.getY() - 10.0f}),
                 juce::Justification::centred, false);
    }
    g.setFont(type::mono(11.0f));
    g.setColour((active ? colour::text : colour::muted).withMultipliedAlpha(alpha));
    g.drawText(bandLabel(b), juce::Rectangle<float>(48.0f, kLabelHeight).withCentre({x, getHeight() - kLabelHeight * 0.5f}),
               juce::Justification::centred, false);
  }
}

void GraphicEqView::set(int band, double db) {
  db = std::round(juce::jlimit(-kRangeDb, kRangeDb, db) * 10.0) / 10.0;
  if (gains[static_cast<size_t>(band)] == db)
    return;
  gains[static_cast<size_t>(band)] = db;
  repaint();
  if (onChange)
    onChange(band, db);
}

void GraphicEqView::mouseMove(const juce::MouseEvent& event) {
  const int now = bandAt(event.position);
  if (now != hovered) {
    hovered = now;
    lit.snapTo(0.0f);
    lit.setTarget(now >= 0 ? 1.0f : 0.0f);
    repaint();
  }
}

void GraphicEqView::mouseExit(const juce::MouseEvent&) {
  if (dragging < 0 && hovered >= 0) {
    hovered = -1;
    repaint();
  }
}

void GraphicEqView::mouseDown(const juce::MouseEvent& event) {
  dragging = bandAt(event.position);
  if (dragging < 0)
    return;
  dragStartDb = gains[static_cast<size_t>(dragging)];
  // A press away from the handle moves the band there; on it, the drag
  // carries on from where the band was.
  if (std::abs(event.position.y - yOf(dragStartDb)) > 10.0f) {
    set(dragging, dbAt(event.position.y));
    dragStartDb = gains[static_cast<size_t>(dragging)];
  }
}

void GraphicEqView::mouseDrag(const juce::MouseEvent& event) {
  if (dragging < 0)
    return;
  const double perPixel = kRangeDb * 2.0 / plot().getHeight() * (event.mods.isShiftDown() ? 0.2 : 1.0);
  set(dragging, dragStartDb - event.getDistanceFromDragStartY() * perPixel);
}

void GraphicEqView::mouseUp(const juce::MouseEvent& event) {
  dragging = -1;
  hovered = bandAt(event.position);
  repaint();
}

void GraphicEqView::mouseDoubleClick(const juce::MouseEvent& event) {
  if (const int band = bandAt(event.position); band >= 0)
    set(band, 0.0);
}

void GraphicEqView::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) {
  const int band = bandAt(event.position);
  if (band < 0 || wheel.deltaY == 0.0f)
    return;
  const double step = (event.mods.isShiftDown() ? 0.1 : 0.5) * (wheel.deltaY > 0 ? 1.0 : -1.0) * (wheel.isReversed ? -1.0 : 1.0);
  set(band, gains[static_cast<size_t>(band)] + step);
}

}  // namespace pedaleira::ui

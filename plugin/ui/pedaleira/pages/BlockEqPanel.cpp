#include "BlockEqPanel.h"

#include "../Paint.h"
#include "../Type.h"
#include "ParamKnobs.h"

#include <cmath>

namespace pedaleira::ui {

namespace {

// The curve is drawn in the board's 520 x 200 view box: the plot runs from
// x 34 to 512, the 0 dB line is at y 93 and 15 dB is 85 px.
constexpr float kViewWidth = 520.0f;
constexpr float kViewHeight = 200.0f;
constexpr float kPlotLeft = 34.0f;
constexpr float kPlotRight = 512.0f;
constexpr float kZeroY = 93.0f;
constexpr float kPixelsPerDb = 85.0f / 15.0f;
constexpr double kSampleRate = 48000.0;
constexpr float kHandleGrab = 14.0f;

float freqToX(double freqHz) {
  return kPlotLeft + (kPlotRight - kPlotLeft) * static_cast<float>((std::log10(freqHz) - std::log10(20.0)) / 3.0);
}

double xToFreq(float x) {
  return 20.0 * std::pow(10.0, 3.0 * (x - kPlotLeft) / (kPlotRight - kPlotLeft));
}

float dbToY(double db) { return kZeroY - static_cast<float>(db) * kPixelsPerDb; }
double yToDb(float y) { return (kZeroY - y) / kPixelsPerDb; }

juce::String freqText(double freqHz) {
  if (freqHz >= 1000.0)
    return juce::String(freqHz / 1000.0, 1) + " kHz";
  return juce::String(juce::roundToInt(freqHz)) + " Hz";
}

juce::String typeLabel(const juce::String& type) {
  if (type == "lowcut")
    return "High-pass";
  if (type == "highcut")
    return "Low-pass";
  if (type == "lowshelf")
    return "Low shelf";
  if (type == "highshelf")
    return "High shelf";
  return "Bell";
}

bool hasGain(const juce::String& type) { return type != "lowcut" && type != "highcut"; }

// The band's response at a frequency, in dB: the RBJ biquad of BlockEq.cpp.
double bandResponseDb(const BlockEqPanel::Band& band, double freqHz) {
  if (hasGain(band.type) && std::abs(band.gainDb) < 0.05)
    return 0.0;
  const double freq = juce::jlimit(20.0, juce::jmin(20000.0, kSampleRate * 0.49), band.freqHz);
  const double A = std::pow(10.0, band.gainDb / 40.0);
  const double omega = 2.0 * juce::MathConstants<double>::pi * freq / kSampleRate;
  const double sn = std::sin(omega), cs = std::cos(omega);
  const double alpha = sn / (2.0 * juce::jmax(0.1, band.q));
  const double sqrtA = std::sqrt(A);
  double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;
  if (band.type == "lowcut") {
    b0 = (1.0 + cs) * 0.5;
    b1 = -(1.0 + cs);
    b2 = (1.0 + cs) * 0.5;
    a0 = 1.0 + alpha;
    a1 = -2.0 * cs;
    a2 = 1.0 - alpha;
  } else if (band.type == "highcut") {
    b0 = (1.0 - cs) * 0.5;
    b1 = 1.0 - cs;
    b2 = (1.0 - cs) * 0.5;
    a0 = 1.0 + alpha;
    a1 = -2.0 * cs;
    a2 = 1.0 - alpha;
  } else if (band.type == "lowshelf") {
    b0 = A * ((A + 1.0) - (A - 1.0) * cs + 2.0 * sqrtA * alpha);
    b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cs);
    b2 = A * ((A + 1.0) - (A - 1.0) * cs - 2.0 * sqrtA * alpha);
    a0 = (A + 1.0) + (A - 1.0) * cs + 2.0 * sqrtA * alpha;
    a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cs);
    a2 = (A + 1.0) + (A - 1.0) * cs - 2.0 * sqrtA * alpha;
  } else if (band.type == "highshelf") {
    b0 = A * ((A + 1.0) + (A - 1.0) * cs + 2.0 * sqrtA * alpha);
    b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cs);
    b2 = A * ((A + 1.0) + (A - 1.0) * cs - 2.0 * sqrtA * alpha);
    a0 = (A + 1.0) - (A - 1.0) * cs + 2.0 * sqrtA * alpha;
    a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cs);
    a2 = (A + 1.0) - (A - 1.0) * cs - 2.0 * sqrtA * alpha;
  } else {
    b0 = 1.0 + alpha * A;
    b1 = -2.0 * cs;
    b2 = 1.0 - alpha * A;
    a0 = 1.0 + alpha / A;
    a1 = -2.0 * cs;
    a2 = 1.0 - alpha / A;
  }
  // |H| at the frequency: the numerator and denominator on the unit circle.
  const double w = 2.0 * juce::MathConstants<double>::pi * juce::jlimit(1.0, kSampleRate * 0.499, freqHz) / kSampleRate;
  const double c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2.0 * w), s2 = std::sin(2.0 * w);
  const double nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2);
  const double dr = a0 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
  return 10.0 * std::log10((nr * nr + ni * ni) / (dr * dr + di * di));
}

}  // namespace

// ####################
// FIELD
// ####################
// One value of the selected band: dragged up or down to change it, or
// clicked when it opens a list.
class BlockEqPanel::Field : public juce::Component {
public:
  explicit Field(const juce::String& fieldName) : name(fieldName) {
    setTitle(fieldName);
    setWantsKeyboardFocus(true);
  }

  void setValue(const juce::String& text) {
    if (text == value)
      return;
    value = text;
    repaint();
  }

  std::function<void(double)> onAdjust;
  std::function<void()> onClick;

  void paint(juce::Graphics& g) override {
    const auto area = getLocalBounds().toFloat();
    g.setColour(colour::raised);
    g.fillRoundedRectangle(area, size::buttonRadius);
    g.setColour(colour::line);
    g.drawRoundedRectangle(area.reduced(0.5f), size::buttonRadius, 1.0f);
    auto inner = getLocalBounds().reduced(10, 0);
    g.setFont(type::sans(12.0f));
    g.setColour(colour::muted);
    g.drawText(name, inner, juce::Justification::centredLeft, false);
    g.setFont(type::mono(12.0f, type::Weight::regular));
    g.setColour(colour::text);
    g.drawText(value, inner, juce::Justification::centredRight, false);
    if (hasKeyboardFocus(false)) {
      g.setColour(colour::iris);
      g.drawRoundedRectangle(area.reduced(1.0f), size::buttonRadius, 2.0f);
    }
  }

  void mouseDown(const juce::MouseEvent&) override {
    lastY = 0;
    moved = false;
  }

  void mouseDrag(const juce::MouseEvent& event) override {
    if (!onAdjust)
      return;
    const int dy = event.getDistanceFromDragStartY();
    if (std::abs(dy) > 2)
      moved = true;
    onAdjust(-(dy - lastY) / 3.0);
    lastY = dy;
  }

  void mouseUp(const juce::MouseEvent&) override {
    if (!moved && onClick)
      onClick();
  }

  void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override {
    if (onAdjust)
      onAdjust(wheel.deltaY * 10.0);
  }

  bool keyPressed(const juce::KeyPress& key) override {
    if (key == juce::KeyPress::upKey || key == juce::KeyPress::rightKey) {
      if (onAdjust)
        onAdjust(1.0);
      return onAdjust != nullptr;
    }
    if (key == juce::KeyPress::downKey || key == juce::KeyPress::leftKey) {
      if (onAdjust)
        onAdjust(-1.0);
      return onAdjust != nullptr;
    }
    if (key == juce::KeyPress::returnKey && onClick) {
      onClick();
      return true;
    }
    return false;
  }

private:
  juce::String name, value;
  int lastY = 0;
  bool moved = false;
};

// ####################
// BLOCK EQ PANEL
// ####################
BlockEqPanel::BlockEqPanel(EngineClient& client) : engine(client) {
  setTitle("Block EQ");
  power.onClick = [this] {
    enabled = power.getToggleState();
    engine.run("block.setEqEnabled", object({{"blockId", blockId}, {"enabled", enabled}}));
    repaint();
  };
  position.setTitle("EQ position");
  position.onChange = [this](int index) {
    pre = index == 0;
    engine.run("block.setEqPre", object({{"blockId", blockId}, {"pre", pre}}));
    repaint();
  };
  reset.setTitle("Reset EQ");
  reset.onClick = [this] {
    engine.run("block.resetEq", object({{"blockId", blockId}}));
    // The flat defaults are the engine's to name: the owner reads them back.
    if (onChanged)
      onChanged();
  };
  addAndMakeVisible(power);
  addAndMakeVisible(position);
  addAndMakeVisible(reset);
  const char* names[] = {"Type", "Freq", "Gain", "Q"};
  for (size_t i = 0; i < fields.size(); ++i) {
    fields[i] = std::make_unique<Field>(names[i]);
    addAndMakeVisible(*fields[i]);
  }
  // Type: the list of what the band can be.
  fields[0]->onClick = [this] {
    juce::PopupMenu menu;
    std::vector<juce::String> choices = {"bell"};
    if (selected == 0)
      choices = {"lowcut", "lowshelf"};
    else if (selected == 5)
      choices = {"highshelf", "highcut"};
    if (choices.size() < 2)
      return;
    for (const auto& choice : choices)
      menu.addItem(typeLabel(choice), true, bands[static_cast<size_t>(selected)].type == choice, [this, choice] {
        Band band = bands[static_cast<size_t>(selected)];
        band.type = choice;
        // Cuts and shelves start at the gentle slope, as the engine's own
        // defaults do.
        if (choice != "bell")
          band.q = 0.71;
        setBand(selected, band);
        pushBand(selected);
      });
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(fields[0].get()));
  };
  fields[1]->onAdjust = [this](double steps) {
    Band band = bands[static_cast<size_t>(selected)];
    band.freqHz = juce::jlimit(20.0, 20000.0, band.freqHz * std::pow(2.0, steps / 24.0));
    setBand(selected, band);
    pushBand(selected);
  };
  fields[2]->onAdjust = [this](double steps) {
    Band band = bands[static_cast<size_t>(selected)];
    if (!hasGain(band.type))
      return;
    band.gainDb = juce::jlimit(-15.0, 15.0, band.gainDb + steps * 0.1);
    setBand(selected, band);
    pushBand(selected);
  };
  fields[3]->onAdjust = [this](double steps) {
    Band band = bands[static_cast<size_t>(selected)];
    band.q = juce::jlimit(0.1, 10.0, band.q * std::pow(2.0, steps / 24.0));
    setBand(selected, band);
    pushBand(selected);
  };
}

BlockEqPanel::~BlockEqPanel() = default;

double BlockEqPanel::responseDb(const std::array<Band, 6>& bands, double freqHz) {
  double total = 0.0;
  for (const auto& band : bands)
    total += bandResponseDb(band, freqHz);
  return total;
}

void BlockEqPanel::showBlock(const juce::String& id, const juce::var& eq, const juce::String& modelName) {
  const bool other = id != blockId;
  blockId = id;
  model = modelName;
  if (other)
    selected = 3;
  // A band being dragged keeps the value under the mouse.
  if (dragging < 0 && eq.isObject()) {
    enabled = static_cast<bool>(eq["enabled"]);
    pre = static_cast<bool>(eq["pre"]);
    if (const auto* list = eq["bands"].getArray())
      for (int i = 0; i < list->size() && i < 6; ++i) {
        Band& band = bands[static_cast<size_t>(i)];
        band.type = (*list)[i]["type"].toString();
        band.freqHz = (*list)[i]["freqHz"];
        band.gainDb = (*list)[i]["gainDb"];
        band.q = (*list)[i]["q"];
      }
  }
  power.setToggleState(enabled, juce::dontSendNotification);
  position.setSelected(pre ? 0 : 1);
  updateFields();
  repaint();
}

void BlockEqPanel::setBand(int index, const Band& band) {
  bands[static_cast<size_t>(index)] = band;
  updateFields();
  repaint();
}

void BlockEqPanel::pushBand(int index) {
  const Band& band = bands[static_cast<size_t>(index)];
  engine.run("block.setEqBand", object({{"blockId", blockId},
                                        {"band", index},
                                        {"type", band.type},
                                        {"freqHz", band.freqHz},
                                        {"gainDb", band.gainDb},
                                        {"q", band.q}}));
}

void BlockEqPanel::selectBand(int index) {
  selected = juce::jlimit(0, 5, index);
  updateFields();
  repaint();
}

void BlockEqPanel::updateFields() {
  const Band& band = bands[static_cast<size_t>(selected)];
  fields[0]->setValue(typeLabel(band.type));
  fields[1]->setValue(freqText(band.freqHz));
  fields[2]->setValue(hasGain(band.type) ? signedDb(band.gainDb) : juce::String("12 dB/oct"));
  fields[3]->setValue(juce::String(band.q, 2));
  fields[2]->setEnabled(hasGain(band.type));
}

// ####################
// LAYOUT
// ####################
juce::Rectangle<float> BlockEqPanel::graphArea() const { return graph.toFloat(); }

juce::Rectangle<int> BlockEqPanel::chipBounds(int index) const {
  const int gap = 8;
  const int width = (chips.getWidth() - 5 * gap) / 6;
  return {chips.getX() + index * (width + gap), chips.getY(), width, chips.getHeight()};
}

void BlockEqPanel::resized() {
  auto area = getLocalBounds().reduced(16);
  auto head = area.removeFromTop(28);
  // Right to left: "On", the switch, the position, reset.
  head.removeFromRight(24);
  head.removeFromRight(8);
  power.setBounds(head.removeFromRight(40).withSizeKeepingCentre(40, 28));
  head.removeFromRight(16);
  position.setBounds(head.removeFromRight(position.idealWidth()).withSizeKeepingCentre(position.idealWidth(), 36));
  head.removeFromRight(8);
  reset.setBounds(head.removeFromRight(reset.idealWidth()).withSizeKeepingCentre(reset.idealWidth(), 32));
  area.removeFromTop(12);
  const int graphHeight = juce::roundToInt(area.getWidth() * kViewHeight / kViewWidth);
  graph = area.removeFromTop(graphHeight);
  area.removeFromTop(12);
  chips = area.removeFromTop(58);
  area.removeFromTop(12);
  fieldsRow = area.removeFromTop(32);
  auto row = fieldsRow;
  row.removeFromLeft(96 + 12);
  const int gap = 8;
  const int width = (row.getWidth() - 3 * gap) / 4;
  for (size_t i = 0; i < fields.size(); ++i)
    fields[i]->setBounds(row.getX() + static_cast<int>(i) * (width + gap), row.getY(), width, row.getHeight());
}

juce::Point<float> BlockEqPanel::toGraph(juce::Point<float> p) const {
  const float scale = graph.getWidth() / kViewWidth;
  return {(p.x - static_cast<float>(graph.getX())) / scale, (p.y - static_cast<float>(graph.getY())) / scale};
}

juce::Point<float> BlockEqPanel::handlePosition(int index) const {
  const Band& band = bands[static_cast<size_t>(index)];
  const double db = hasGain(band.type) ? band.gainDb : juce::jlimit(-15.0, 15.0, responseDb(bands, band.freqHz));
  return {freqToX(band.freqHz), dbToY(db)};
}

int BlockEqPanel::handleAt(juce::Point<float> position) const {
  const juce::Point<float> p = toGraph(position);
  int best = -1;
  float bestDistance = kHandleGrab;
  // The selected band wins a tie, so overlapping handles can be told apart.
  for (int i = 0; i < 6; ++i) {
    const float distance = handlePosition(i).getDistanceFrom(p) - (i == selected ? 1.0f : 0.0f);
    if (distance < bestDistance) {
      bestDistance = distance;
      best = i;
    }
  }
  return best;
}

// ####################
// PAINT
// ####################
void BlockEqPanel::paint(juce::Graphics& g) {
  const auto box = getLocalBounds();
  g.setColour(colour::ground);
  g.fillRoundedRectangle(box.toFloat(), size::panelRadius);
  g.setColour(colour::line);
  g.drawRoundedRectangle(box.toFloat().reduced(0.5f), size::panelRadius, 1.0f);

  auto head = box.reduced(16).removeFromTop(28);
  paint::label(g, "Block EQ", head.withWidth(120));
  g.setFont(type::sans(12.0f));
  g.setColour(colour::muted);
  const int labelWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(type::label(), "BLOCK EQ"));
  g.drawText("6 bands " + type::dot() + " " + (pre ? "before the " : "after the ") + model,
             head.withTrimmedLeft(labelWidth + 12), juce::Justification::centredLeft, true);
  g.setFont(type::sans(13.0f));
  g.setColour(colour::text);
  g.drawText(enabled ? "On" : "Off", power.getRight() + 8, power.getY(), 24, power.getHeight(),
             juce::Justification::centredLeft, false);

  // The curve, in the board's view box.
  {
    juce::Graphics::ScopedSaveState state(g);
    const float scale = graph.getWidth() / kViewWidth;
    g.addTransform(juce::AffineTransform::scale(scale).translated(static_cast<float>(graph.getX()),
                                                                    static_cast<float>(graph.getY())));
    g.setColour(colour::surface);
    g.fillRoundedRectangle(0.0f, 0.0f, kViewWidth, kViewHeight, 8.0f);

    // Octave-ish verticals, the decades stronger; 6 dB horizontals, the
    // zero line dashed.
    const std::pair<double, bool> verticals[] = {{50, false},  {100, true},   {200, false},  {500, false},
                                                 {1000, true}, {2000, false}, {5000, false}, {10000, true}};
    for (const auto& [f, strong] : verticals) {
      g.setColour(colour::line.withMultipliedAlpha(strong ? 1.0f : 0.5f));
      g.drawLine(freqToX(f), 8.0f, freqToX(f), 178.0f, 1.0f);
    }
    for (const double db : {12.0, 6.0, -6.0, -12.0}) {
      g.setColour(colour::line.withMultipliedAlpha(0.5f));
      g.drawLine(kPlotLeft, dbToY(db), kPlotRight, dbToY(db), 1.0f);
    }
    {
      juce::Path zero;
      zero.startNewSubPath(kPlotLeft, kZeroY);
      zero.lineTo(kPlotRight, kZeroY);
      juce::Path dashed;
      const float dashes[] = {4.0f, 4.0f};
      juce::PathStrokeType(1.0f).createDashedStroke(dashed, zero, dashes, 2);
      g.setColour(colour::faint);
      g.fillPath(dashed);
    }
    g.setFont(type::mono(10.0f, type::Weight::regular));
    g.setColour(colour::muted);
    for (const auto& [text, db] : {std::pair<const char*, double>{"+12", 12.0}, {"0", 0.0}, {"-12", -12.0}})
      g.drawText(text, juce::Rectangle<float>(0.0f, dbToY(db) - 6.0f, 28.0f, 12.0f), juce::Justification::centredRight,
                 false);
    for (const auto& [text, f] : {std::pair<const char*, double>{"100", 100.0}, {"1k", 1000.0}, {"10k", 10000.0}})
      g.drawText(text, juce::Rectangle<float>(freqToX(f) - 20.0f, 184.0f, 40.0f, 12.0f), juce::Justification::centred,
                 false);

    const float alpha = enabled ? 1.0f : 0.4f;
    juce::Path curve;
    for (int i = 0; i <= 120; ++i) {
      const double f = 20.0 * std::pow(1000.0, i / 120.0);
      const float y = dbToY(juce::jlimit(-14.5, 14.5, responseDb(bands, f)));
      if (i == 0)
        curve.startNewSubPath(freqToX(f), y);
      else
        curve.lineTo(freqToX(f), y);
    }
    g.setColour(colour::iris.withMultipliedAlpha(alpha));
    g.strokePath(curve, juce::PathStrokeType(2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    for (int i = 0; i < 6; ++i) {
      const auto at = handlePosition(i);
      const bool on = i == selected;
      const float radius = on ? 7.0f : 6.0f;
      g.setColour(on ? colour::iris.withMultipliedAlpha(alpha) : colour::raised);
      g.fillEllipse(at.x - radius, at.y - radius, 2.0f * radius, 2.0f * radius);
      g.setColour(on ? colour::text : colour::iris.withMultipliedAlpha(alpha));
      g.drawEllipse(at.x - radius, at.y - radius, 2.0f * radius, 2.0f * radius, 2.0f);
    }
  }

  // The six bands as cards.
  for (int i = 0; i < 6; ++i) {
    const Band& band = bands[static_cast<size_t>(i)];
    const auto card = chipBounds(i).toFloat();
    const bool on = i == selected;
    g.setColour(on ? colour::control : colour::raised);
    g.fillRoundedRectangle(card, size::buttonRadius);
    g.setColour(on ? colour::iris : colour::line);
    g.drawRoundedRectangle(card.reduced(0.5f), size::buttonRadius, 1.0f);
    auto inner = chipBounds(i).reduced(10, 8);
    g.setFont(type::sans(11.0f, type::Weight::semibold));
    g.setColour(colour::muted);
    g.drawText(typeLabel(band.type), inner.removeFromTop(14), juce::Justification::centredLeft, true);
    g.setFont(type::mono(12.0f, type::Weight::regular));
    g.setColour(colour::text);
    const juce::String prefix = band.type == "lowcut" ? "HP " : band.type == "highcut" ? "LP " : "";
    g.drawText(prefix + freqText(band.freqHz), inner.removeFromTop(16), juce::Justification::centredLeft, true);
    g.setColour(colour::muted);
    g.drawText(hasGain(band.type) ? signedDb(band.gainDb) : juce::String("12 dB/oct"), inner.removeFromTop(16),
               juce::Justification::centredLeft, true);
  }

  paint::label(g, "Band " + juce::String(selected + 1), fieldsRow.withWidth(96));
}

// ####################
// MOUSE
// ####################
void BlockEqPanel::mouseDown(const juce::MouseEvent& event) {
  for (int i = 0; i < 6; ++i)
    if (chipBounds(i).contains(event.getPosition())) {
      selectBand(i);
      return;
    }
  if (!graph.contains(event.getPosition()))
    return;
  const int hit = handleAt(event.position);
  if (hit < 0)
    return;
  selectBand(hit);
  dragging = hit;
}

void BlockEqPanel::mouseDrag(const juce::MouseEvent& event) {
  if (dragging < 0)
    return;
  const juce::Point<float> p = toGraph(event.position);
  Band band = bands[static_cast<size_t>(dragging)];
  band.freqHz = juce::jlimit(20.0, 20000.0, xToFreq(juce::jlimit(kPlotLeft, kPlotRight, p.x)));
  if (hasGain(band.type))
    band.gainDb = juce::jlimit(-15.0, 15.0, yToDb(p.y));
  setBand(dragging, band);
  pushBand(dragging);
}

void BlockEqPanel::mouseUp(const juce::MouseEvent&) { dragging = -1; }

void BlockEqPanel::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) {
  if (!graph.contains(event.getPosition()))
    return;
  // The wheel over the curve widens or narrows the selected band.
  Band band = bands[static_cast<size_t>(selected)];
  band.q = juce::jlimit(0.1, 10.0, band.q * std::pow(2.0, wheel.deltaY * 2.0));
  setBand(selected, band);
  pushBand(selected);
}

}  // namespace pedaleira::ui

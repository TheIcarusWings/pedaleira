#include "LiveTuner.h"

#include "../Icons.h"
#include "../Paint.h"
#include "GridModel.h"

#include <cmath>

namespace pedaleira::ui {

namespace {

constexpr int kPadding = 24;
constexpr int kGap = 20;
constexpr int kPanelWidth = 380;
constexpr int kCardPadX = 28;
constexpr int kCardPadY = 20;
constexpr int kBigHeight = 168 + 4 + 40 + 4 + 32;
constexpr int kMeterWidth = 760;
constexpr int kMeterHeight = 108;
constexpr int kLegendGap = 14;
constexpr int kLegendHeight = 20;
constexpr int kStringSize = 52;
constexpr int kStringGap = 14;
constexpr int kStringsHeight = kStringSize + 6 + 14;
// The scale: 7.2 px a cent, 20 px in from each end.
constexpr float kPixelsPerCent = 7.2f;
constexpr float kScaleInset = 20.0f;
constexpr int kScaleBase = 64;

// Standard tuning, the sixth string first.
constexpr double kStringHz[6] = {82.41, 110.0, 146.83, 196.0, 246.94, 329.63};
const char* const kStringNote[6] = {"E", "A", "D", "G", "B", "E"};

float scaleX(double cents) { return kScaleInset + static_cast<float>(cents + 50.0) * kPixelsPerCent; }

juce::Font trackedLabel(float px) {
  const juce::Font font = type::sans(px, type::Weight::semibold);
  return font.withExtraKerningFactor(0.08f * font.getHeightToPointsFactor());
}

}  // namespace

TunerView::TunerView(EngineClient& client)
    : engine(client),
      lower("Lower reference pitch", &icons::path(icons::Icon::minus), StepButton::Shape::square),
      raise("Raise reference pitch", &icons::path(icons::Icon::plus), StepButton::Shape::square) {
  setTitle("Tuner");
  // The reference pitch is fixed at A = 440 until the detector takes one.
  lower.setEnabled(false);
  raise.setEnabled(false);
  muteToggle.setState(false);
  muteToggle.setSwitchEnabled(false);
  stripToggle.setState(true);
  stripToggle.setSwitchEnabled(false);
  displayBox.setChoice("Needle");
  displayBox.onClick = [this] { chooseDisplay(); };
  sourceBox.setChoice(sourceText);
  sourceBox.setEnabled(false);
  for (juce::Component* c : std::initializer_list<juce::Component*>{&lower, &raise, &muteToggle, &stripToggle, &displayBox, &sourceBox})
    addAndMakeVisible(c);
}

TunerView::~TunerView() {
  stopTimer();
  if (active)
    engine.run("tuner.setEnabled", object({{"enabled", false}}));
}

void TunerView::setActive(bool shouldListen) {
  if (shouldListen == active)
    return;
  active = shouldListen;
  if (active) {
    // The tuner listens only while the tab is up.
    engine.run("tuner.setEnabled", object({{"enabled", true}}));
    refresh();
    readTuner();
    startTimerHz(30);
  } else {
    stopTimer();
    engine.run("tuner.setEnabled", object({{"enabled", false}}));
    reading = {};
    marked = -1;
    repaint();
  }
}

void TunerView::resume() {
  if (active)
    engine.run("tuner.setEnabled", object({{"enabled", true}}));
}

void TunerView::refresh() {
  // The tuner listens to the first row's input.
  const GridModel grid = GridModel::read(engine);
  const GridRow& first = grid.rows[static_cast<size_t>(grid.visibleRows().front())];
  sourceText = "Row " + juce::String(first.index + 1) + " " + type::dot() + " " + first.inputName;
  listeningText = first.inputName.startsWith("Input") ? "Listening on " + first.inputName : first.inputName;
  sourceBox.setChoice(sourceText);

  const juce::var map = engine.read("midi.getMap");
  if (const auto* defaults = map["defaults"].getArray())
    for (const auto& entry : *defaults)
      if (entry["action"].toString() == "tuner")
        toggleCc = entry["cc"];
  repaint();
}

void TunerView::readTuner() {
  const juce::var tuner = engine.read("tuner.get");
  const GigView::Reading next = GigView::analyse(tuner["frequency"], tuner["confidence"]);
  // A little smoothing keeps the needle from shaking; a new note starts over.
  if (next.valid && reading.valid && next.note == reading.note && next.sharp == reading.sharp && next.octave == reading.octave)
    shownCents += 0.5 * (next.cents - shownCents);
  else
    shownCents = next.cents;
  reading = next;

  marked = -1;
  if (reading.valid) {
    double best = 200.0;
    for (int i = 0; i < 6; ++i) {
      const double distance = std::abs(1200.0 * std::log2(reading.hz / kStringHz[i]));
      if (distance < best) {
        best = distance;
        marked = i;
      }
    }
  }
  repaint(card);
}

void TunerView::timerCallback() { readTuner(); }

void TunerView::chooseDisplay() {
  juce::PopupMenu menu;
  juce::Component::SafePointer<TunerView> self(this);
  menu.addItem("Needle", true, true, [self] {
    if (self != nullptr)
      self->repaint();
  });
  menu.addItem("Strobe, " + juce::String(live::kLater).toLowerCase(), false, false, [] {});
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&displayBox).withMinimumWidth(displayBox.getWidth()));
}

// ####################
// LAYOUT
// ####################
void TunerView::resized() {
  auto area = getLocalBounds().reduced(kPadding);
  panel = area.removeFromRight(kPanelWidth);
  area.removeFromRight(kGap);
  card = area;

  // The card: the title row on top, then the note, the meter and the strings
  // spread over the height.
  auto inner = card.reduced(kCardPadX, kCardPadY);
  head = inner.removeFromTop(28);
  const int meterGroup = kMeterHeight + kLegendGap + kLegendHeight;
  const int gap = juce::jmax(8, (inner.getHeight() - kBigHeight - meterGroup - kStringsHeight) / 3);
  chip = head.removeFromRight(juce::roundToInt(juce::GlyphArrangement::getStringWidth(type::sans(12.0f), listeningText)) + 22);
  big ={inner.getX(), head.getBottom() + gap, inner.getWidth(), kBigHeight};
  meter = juce::Rectangle<int>(kMeterWidth, kMeterHeight).withCentre({card.getCentreX(), 0}).withY(big.getBottom() + gap);
  legend = {inner.getX(), meter.getBottom() + kLegendGap, inner.getWidth(), kLegendHeight};
  strings = juce::Rectangle<int>(6 * kStringSize + 5 * kStringGap, kStringsHeight)
                .withCentre({card.getCentreX(), 0})
                .withY(legend.getBottom() + gap);

  // The settings.
  auto p = panel.reduced(20);
  panelTitle = p.removeFromTop(26);
  p.removeFromTop(18);
  referenceLabel = p.removeFromTop(16);
  p.removeFromTop(8);
  referenceRow = p.removeFromTop(36);
  lower.setBounds(referenceRow.withWidth(36));
  raise.setBounds(referenceRow.withLeft(referenceRow.getRight() - 36));
  referenceValue = referenceRow.reduced(36 + 12, 0);
  p.removeFromTop(6);
  referenceHint = p.removeFromTop(16);
  p.removeFromTop(18);
  dividers[0] = p.getY();
  p.removeFromTop(1 + 18);
  muteToggle.setBounds(p.removeFromTop(40));
  p.removeFromTop(12);
  stripToggle.setBounds(p.removeFromTop(40));
  p.removeFromTop(18);
  dividers[1] = p.getY();
  p.removeFromTop(1 + 18);
  displayLabel = p.removeFromTop(16);
  p.removeFromTop(8);
  displayBox.setBounds(p.removeFromTop(36));
  p.removeFromTop(18);
  sourceLabel = p.removeFromTop(16);
  p.removeFromTop(8);
  sourceBox.setBounds(p.removeFromTop(36));
  p.removeFromTop(6);
  sourceHint = p.removeFromTop(16);
  p.removeFromTop(18);
  dividers[2] = p.getY();
  p.removeFromTop(1 + 18);
  midiRow = p.removeFromTop(20);
}

// ####################
// PAINT
// ####################
void TunerView::paint(juce::Graphics& g) {
  g.fillAll(colour::ground);
  live::paintCard(g, card);
  live::paintCard(g, panel);

  paint::label(g, "Chromatic tuner", head);
  const auto chipBox = chip.toFloat();
  g.setColour(colour::raised);
  g.fillRoundedRectangle(chipBox, size::buttonRadius);
  g.setColour(colour::line);
  g.drawRoundedRectangle(chipBox.reduced(0.5f), size::buttonRadius, 1.0f);
  live::paintHint(g, listeningText, chip, juce::Justification::centred);

  paintReadout(g);
  paintMeter(g);
  paintStrings(g);
  paintSettings(g);
}

void TunerView::paintReadout(juce::Graphics& g) {
  const bool heard = reading.valid;
  const bool good = heard && GigView::inTune(shownCents);
  const juce::Colour signal = good ? colour::ok : colour::warn;

  // The note and its octave: the letter centred with the octave beside it.
  auto letterLine = big.withHeight(168);
  const juce::Font letterFont = type::mono(160.0f, type::Weight::bold);
  if (heard) {
    const juce::Font octaveFont = type::mono(40.0f);
    const juce::Font sharpFont = type::mono(56.0f, type::Weight::bold);
    const juce::String octave(reading.octave);
    const int letterWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(letterFont, reading.note)) + 2;
    const int octaveWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(octaveFont, octave)) + 2;
    const int sharpWidth = reading.sharp ? juce::roundToInt(juce::GlyphArrangement::getStringWidth(sharpFont, "#")) + 2 : 0;
    const int sideWidth = juce::jmax(octaveWidth, sharpWidth);
    const int total = letterWidth + 6 + sideWidth;
    auto row = letterLine.withSizeKeepingCentre(total, letterLine.getHeight());
    g.setFont(letterFont);
    g.setColour(colour::text);
    g.drawText(reading.note, row.removeFromLeft(letterWidth), juce::Justification::centred, false);
    row.removeFromLeft(6);
    if (reading.sharp) {
      g.setFont(sharpFont);
      g.drawText("#", row.withHeight(64).translated(0, 8), juce::Justification::centredLeft, false);
    }
    g.setFont(octaveFont);
    g.setColour(colour::muted);
    g.drawText(octave, row.withTrimmedTop(112).withHeight(48), juce::Justification::centredLeft, false);
  } else {
    g.setFont(letterFont);
    g.setColour(colour::faint);
    g.drawText("-", letterLine, juce::Justification::centred, false);
  }

  // How far off, and which way.
  auto centsLine = juce::Rectangle<int>(big.getX(), big.getY() + 172, big.getWidth(), 40);
  if (heard) {
    const int rounded = juce::roundToInt(shownCents);
    const juce::String cents = (rounded > 0 ? "+" : "") + juce::String(rounded) + " ct";
    const juce::String verdict = good ? "In tune" : rounded < 0 ? "Flat" : "Sharp";
    const juce::Font centsFont = type::mono(32.0f);
    const juce::Font verdictFont = trackedLabel(16.0f);
    const int centsWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(centsFont, cents)) + 2;
    const int verdictWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(verdictFont, verdict.toUpperCase())) + 2;
    auto row = centsLine.withSizeKeepingCentre(centsWidth + 16 + verdictWidth, centsLine.getHeight());
    g.setFont(centsFont);
    g.setColour(signal);
    g.drawText(cents, row.removeFromLeft(centsWidth), juce::Justification::centredLeft, false);
    row.removeFromLeft(16);
    g.setFont(verdictFont);
    g.drawText(verdict.toUpperCase(), row.withTrimmedTop(4), juce::Justification::centredLeft, false);
  } else {
    g.setFont(type::sans(20.0f));
    g.setColour(colour::muted);
    g.drawText("Play a note", centsLine, juce::Justification::centred, false);
  }

  g.setFont(type::mono(24.0f, type::Weight::regular));
  g.setColour(heard ? colour::muted : colour::faint);
  g.drawText(heard ? juce::String(reading.hz, 1) + " Hz" : juce::String("-- Hz"),
             juce::Rectangle<int>(big.getX(), big.getY() + 216, big.getWidth(), 32), juce::Justification::centred, false);
}

void TunerView::paintMeter(juce::Graphics& g) {
  const bool heard = reading.valid;
  const bool good = heard && GigView::inTune(shownCents);
  const juce::Colour signal = good ? colour::ok : colour::warn;
  const float left = static_cast<float>(meter.getX());
  const float top = static_cast<float>(meter.getY());

  // The in-tune band, ticks every 5 cents, the centre line.
  g.setColour(colour::ok.withAlpha(0.16f));
  g.fillRoundedRectangle(left + scaleX(-5.0), top, scaleX(5.0) - scaleX(-5.0), static_cast<float>(kScaleBase), 6.0f);
  for (int c = -50; c <= 50; c += 5) {
    const bool major = c % 10 == 0;
    const float height = (c == 0 || std::abs(c) == 50) ? 36.0f : major ? 26.0f : 16.0f;
    g.setColour(c == 0 ? colour::text : colour::faint);
    g.fillRect(left + scaleX(c) - 1.0f, top + kScaleBase - height, 2.0f, height);
  }
  g.setColour(colour::text);
  g.fillRect(left + scaleX(0.0) - 1.0f, top, 2.0f, static_cast<float>(kScaleBase));

  if (heard) {
    const float centre = left + scaleX(juce::jlimit(-50.0, 50.0, shownCents));
    g.setColour(signal);
    g.fillRoundedRectangle(centre - 2.5f, top + 8.0f, 5.0f, 56.0f, 2.0f);
    g.fillEllipse(centre - 8.0f, top, 16.0f, 16.0f);
  }

  g.setFont(type::mono(12.0f, type::Weight::regular));
  g.setColour(colour::muted);
  const char* const names[] = {"-50", "-25", "0", "+25", "+50"};
  const int marks[] = {-50, -25, 0, 25, 50};
  for (int i = 0; i < 5; ++i)
    g.drawText(names[i], juce::Rectangle<float>(left + scaleX(marks[i]) - 24.0f, top + 74.0f, 48.0f, 16.0f), juce::Justification::centred, false);
  paint::label(g, "Flat", {meter.getX(), meter.getY() + 92, 80, 16});
  paint::label(g, "Sharp", {meter.getRight() - 80, meter.getY() + 92, 80, 16}, juce::Justification::centredRight);

  // The key to the two colours.
  g.setFont(type::sans(13.0f));
  const juce::String outText = "Out of tune";
  const juce::String inText = "In tune, within ";
  const juce::String bandText = juce::String::fromUTF8("\xc2\xb1") + "5 ct";
  const auto width = [&](const juce::Font& font, const juce::String& text) {
    return juce::roundToInt(juce::GlyphArrangement::getStringWidth(font, text)) + 2;
  };
  const int outWidth = 12 + 8 + width(type::sans(13.0f), outText);
  const int inWidth = 12 + 8 + width(type::sans(13.0f), inText) + width(type::mono(13.0f), bandText);
  auto row = legend.withSizeKeepingCentre(outWidth + 24 + inWidth, legend.getHeight());
  const auto swatch = [&](juce::Rectangle<int>& at, juce::Colour fill) {
    g.setColour(fill);
    g.fillRoundedRectangle(at.removeFromLeft(12).withSizeKeepingCentre(12, 12).toFloat(), 3.0f);
    at.removeFromLeft(8);
  };
  swatch(row, colour::warn);
  g.setFont(type::sans(13.0f));
  g.setColour(colour::muted);
  g.drawText(outText, row.removeFromLeft(width(type::sans(13.0f), outText)), juce::Justification::centredLeft, false);
  row.removeFromLeft(24);
  swatch(row, colour::ok);
  g.setFont(type::sans(13.0f));
  g.setColour(colour::muted);
  g.drawText(inText, row.removeFromLeft(width(type::sans(13.0f), inText)), juce::Justification::centredLeft, false);
  g.setFont(type::mono(13.0f, type::Weight::regular));
  g.setColour(colour::text);
  g.drawText(bandText, row, juce::Justification::centredLeft, false);
}

void TunerView::paintStrings(juce::Graphics& g) {
  for (int i = 0; i < 6; ++i) {
    const bool on = i == marked;
    const int x = strings.getX() + i * (kStringSize + kStringGap);
    const auto circle = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(strings.getY()), kStringSize, kStringSize);
    g.setColour(on ? colour::iris : colour::control);
    g.fillEllipse(circle);
    g.setColour(on ? colour::iris : colour::line);
    g.drawEllipse(circle.reduced(1.0f), 2.0f);
    g.setFont(type::mono(20.0f, type::Weight::bold));
    g.setColour(on ? colour::ground : colour::text);
    g.drawText(kStringNote[i], circle, juce::Justification::centred, false);
    g.setFont(type::mono(11.0f, type::Weight::regular));
    g.setColour(colour::muted);
    g.drawText("String " + juce::String(6 - i), juce::Rectangle<int>(x - 7, strings.getY() + kStringSize + 6, kStringSize + 14, 14),
               juce::Justification::centred, false);
  }
}

void TunerView::paintSettings(juce::Graphics& g) {
  g.setFont(type::heading());
  g.setColour(colour::text);
  g.drawText("Tuner settings", panelTitle, juce::Justification::centredLeft, true);

  paint::label(g, "Reference pitch", referenceLabel);
  g.setColour(colour::ground);
  g.fillRoundedRectangle(referenceValue.toFloat(), size::buttonRadius);
  g.setColour(colour::line);
  g.drawRoundedRectangle(referenceValue.toFloat().reduced(0.5f), size::buttonRadius, 1.0f);
  g.setFont(type::mono(20.0f));
  g.setColour(colour::muted);
  g.drawText("440 Hz", referenceValue, juce::Justification::centred, false);
  live::paintHint(g, live::kLater, referenceHint);

  g.setColour(colour::line);
  for (int y : dividers)
    g.fillRect(panel.getX() + 20, y, panel.getWidth() - 40, 1);

  paint::label(g, "Display", displayLabel);
  paint::label(g, "Input source", sourceLabel);
  live::paintHint(g, live::kLater, sourceHint);

  paint::label(g, "MIDI", midiRow.withTrimmedRight(160));
  const juce::String toggle = "Toggle: ";
  const juce::String cc = "CC " + juce::String(toggleCc);
  const int ccWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(type::mono(13.0f, type::Weight::regular), cc)) + 2;
  auto right = midiRow.removeFromRight(ccWidth);
  g.setFont(type::mono(13.0f, type::Weight::regular));
  g.setColour(colour::text);
  g.drawText(cc, right, juce::Justification::centredRight, false);
  g.setFont(type::sans(13.0f));
  g.setColour(colour::muted);
  g.drawText(toggle, midiRow, juce::Justification::centredRight, false);
}

}  // namespace pedaleira::ui

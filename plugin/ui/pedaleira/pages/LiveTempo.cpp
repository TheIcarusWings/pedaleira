#include "LiveTempo.h"

#include "../Icons.h"
#include "../Paint.h"
#include "../Type.h"
#include "GridModel.h"
#include "ParamKnobs.h"
#include "TempoNotes.h"

#include <array>
#include <cmath>

namespace pedaleira::ui {

namespace {

constexpr int kPadding = 24;
constexpr int kGap = 20;
constexpr int kMetronomeWidth = 460;
constexpr int kStepSize = 64;
constexpr int kNumberHeight = 150;
constexpr int kBottomHeight = 128;
constexpr int kTapSize = 96;
constexpr int kTapColumn = 110;
constexpr int kTapsWidth = 170;
constexpr int kRowHeight = 45;
constexpr int kMaxSynced = 4;
constexpr double kMinBpm = 20.0;
constexpr double kMaxBpm = 300.0;

// The note under a figure: mono when it is a value.
juce::AttributedString sentence(std::initializer_list<std::pair<juce::String, bool>> parts) {
  juce::AttributedString out;
  out.setLineSpacing(2.0f);
  for (const auto& [text, value] : parts)
    out.append(text, value ? type::mono(13.0f, type::Weight::regular) : type::sans(13.0f), value ? colour::text : colour::muted);
  return out;
}

}  // namespace

// The tap pad: a round, dashed button that fills on the beat and counts a tap
// the moment the mouse goes down.
class TempoView::TapButton final : public juce::Button {
public:
  TapButton() : juce::Button("Tap tempo") {
    setTitle("Tap tempo");
    setTriggeredOnMouseDown(true);
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
  }

  void setLit(bool shouldLight) {
    if (shouldLight == lit)
      return;
    lit = shouldLight;
    repaint();
  }

  void setFigure(const juce::String& shown) {
    if (shown == figure)
      return;
    figure = shown;
    repaint();
  }

  void paintButton(juce::Graphics& g, bool highlighted, bool down) override {
    const float alpha = isEnabled() ? 1.0f : 0.45f;
    const auto circle = getLocalBounds().toFloat().withHeight(static_cast<float>(kTapSize));
    const bool filled = lit && isEnabled();
    if (!down) {
      g.setColour(colour::drop.withMultipliedAlpha(alpha));
      g.fillEllipse(circle.translated(0.0f, 3.0f));
    }
    g.setColour((filled ? colour::iris : highlighted || down ? colour::line : colour::control).withMultipliedAlpha(alpha));
    g.fillEllipse(circle);
    juce::Path outline;
    outline.addEllipse(circle.reduced(1.5f));
    juce::Path stroke;
    const float dashes[] = {6.0f, 4.8f};
    juce::PathStrokeType(3.0f).createDashedStroke(stroke, outline, dashes, 2);
    g.setColour(colour::muted.withMultipliedAlpha(alpha));
    g.fillPath(stroke);
    if (hasKeyboardFocus(false)) {
      g.setColour(colour::iris);
      g.drawEllipse(circle.expanded(4.0f), 3.0f);
    }
    g.setFont(type::mono(22.0f));
    g.setColour((filled ? colour::ground : colour::text).withMultipliedAlpha(alpha));
    g.drawText(figure, circle, juce::Justification::centred, false);
  }

private:
  bool lit = true;
  juce::String figure = "120";
};

TempoView::TempoView(EngineClient& client)
    : engine(client),
      down("Decrease tempo", &icons::path(icons::Icon::minus), StepButton::Shape::round),
      up("Increase tempo", &icons::path(icons::Icon::plus), StepButton::Shape::round),
      tapButton(std::make_unique<TapButton>()) {
  setTitle("Tempo");

  // One BPM a click, a tenth with Shift held.
  const auto step = [] { return juce::ModifierKeys::currentModifiers.isShiftDown() ? 0.1 : 1.0; };
  down.onClick = [this, step] { setBpm(bpm - step()); };
  up.onClick = [this, step] { setBpm(bpm + step()); };
  tapButton->onClick = [this] { tap(); };

  number.setFont(type::mono(140.0f));
  number.setBorderSize(juce::BorderSize<int>(0));
  number.setJustificationType(juce::Justification::centred);
  number.setTitle("Tempo in beats per minute, click to type");
  number.setEditable(true, false);
  number.setColour(juce::Label::textColourId, colour::text);
  number.setColour(juce::Label::textWhenEditingColourId, colour::text);
  number.setColour(juce::Label::backgroundWhenEditingColourId, colour::ground);
  number.setColour(juce::Label::outlineWhenEditingColourId, colour::iris);
  number.onTextChange = [this] { commitEdited(); };

  source.setTitle("Tempo source");
  source.setEnabled(false);
  // The metronome and the clock have nothing behind them yet.
  for (Toggle* t : {&metronomeSwitch, &sendClock, &receiveClock}) {
    t->setEnabled(false);
    t->setAlpha(0.5f);
  }
  for (juce::Component* c : std::initializer_list<juce::Component*>{&down, &up, &number, tapButton.get(), &source, &metronomeSwitch,
                                                                   &sendClock, &receiveClock})
    addAndMakeVisible(c);
}

TempoView::~TempoView() { stopTimer(); }

void TempoView::setActive(bool shouldFollow) {
  if (shouldFollow == active)
    return;
  active = shouldFollow;
  if (active) {
    refresh();
    startTimerHz(30);
  } else {
    stopTimer();
  }
}

void TempoView::refresh() {
  seenRevision = engine.revision();
  const juce::var map = engine.read("midi.getMap");
  if (const auto* defaults = map["defaults"].getArray())
    for (const auto& entry : *defaults)
      if (entry["action"].toString() == "tapTempo")
        tapCc = entry["cc"];
  readTempo();
  readSynced();
}

// ####################
// TEMPO
// ####################
juce::String TempoView::bpmText() const { return juce::String(hostSync ? effectiveBpm : bpm, 1); }

void TempoView::readTempo() {
  const juce::var tempo = engine.read("tempo.get");
  bpm = tempo["bpm"];
  effectiveBpm = tempo["effectiveBpm"];
  if (bpm <= 0.0)
    bpm = 120.0;
  if (effectiveBpm <= 0.0)
    effectiveBpm = bpm;
  hostSync = static_cast<bool>(tempo["hostSync"]);

  // A host's tempo wins, so nothing here can change it then.
  for (juce::Component* c : std::initializer_list<juce::Component*>{&down, &up, &number, tapButton.get()})
    c->setEnabled(controllable());
  tapButton->setFigure(juce::String(juce::roundToInt(hostSync ? effectiveBpm : bpm)));
  if (!number.isBeingEdited() && number.getText() != bpmText()) {
    number.setText(bpmText(), juce::dontSendNotification);
    resized();
  }
  repaint();
}

void TempoView::setBpm(double newBpm) {
  if (!controllable())
    return;
  const EngineReply reply = engine.run("tempo.set", object({{"bpm", juce::jlimit(kMinBpm, kMaxBpm, newBpm)}}));
  if (reply.ok)
    bpm = reply.value["bpm"];
  readTempo();
  readSynced();
  if (onTempoChanged)
    onTempoChanged();
}

void TempoView::commitEdited() {
  const double typed = number.getText().retainCharacters("0123456789.").getDoubleValue();
  if (typed > 0.0)
    setBpm(typed);
  number.setText(bpmText(), juce::dontSendNotification);
  resized();
}

void TempoView::tap(double timeMs) {
  if (!controllable())
    return;
  const double now = timeMs >= 0.0 ? timeMs : juce::Time::getMillisecondCounterHiRes();
  // The bars show the gaps the engine counts: a pause of more than 3 seconds
  // starts again, a tap within 0.2 seconds of the last is a bounce.
  if (lastTapMs >= 0.0) {
    const double gap = now - lastTapMs;
    if (gap > 3000.0)
      intervals.clear();
    else if (gap >= 200.0)
      intervals.push_back(gap);
    if (intervals.size() > 4)
      intervals.erase(intervals.begin());
  }
  if (lastTapMs < 0.0 || now - lastTapMs >= 200.0 || now < lastTapMs)
    lastTapMs = now;
  const EngineReply reply = timeMs >= 0.0 ? engine.run("tempo.tap", object({{"timeMs", timeMs}})) : engine.run("tempo.tap");
  if (reply.ok)
    bpm = reply.value["bpm"];
  readTempo();
  readSynced();
  if (onTempoChanged)
    onTempoChanged();
}

void TempoView::timerCallback() {
  // The beat light follows the tempo on the wall clock.
  const double beats = juce::Time::getMillisecondCounterHiRes() * effectiveBpm / 60000.0;
  const bool lit = std::fmod(beats, 1.0) < 0.3;
  if (lit != beatLit) {
    beatLit = lit;
    tapButton->setLit(lit);
  }
  // A tap on a footswitch or a MIDI controller moves the tempo too.
  if (++ticks % 8 == 0 && !number.isBeingEdited()) {
    if (const juce::uint32 revision = engine.revision(); revision != seenRevision) {
      seenRevision = revision;
      readSynced();
    }
    readTempo();
  }
}

void TempoView::readSynced() {
  synced.clear();
  syncedTotal = 0;
  const GridModel grid = GridModel::read(engine);
  for (const auto& row : grid.rows)
    for (const auto& slot : row.slots) {
      if (slot.empty || !slot.active)
        continue;
      const juce::var listed = engine.read("block.listParams", object({{"blockId", slot.blockId}}));
      std::vector<KnobParam> specs;
      if (const auto* list = listed["params"].getArray())
        for (const auto& item : *list)
          specs.push_back(KnobParam::fromVar(item));
      const auto find = [&specs](const juce::String& id) -> const KnobParam* {
        for (const auto& spec : specs)
          if (spec.id == id)
            return &spec;
        return nullptr;
      };
      for (const auto& spec : specs) {
        juce::String syncId, divisionId;
        if (spec.common || spec.type != "float" || !spec.tempoSync || !tempoNeighbours(spec.id, syncId, divisionId))
          continue;
        const KnobParam* sync = find(syncId);
        const KnobParam* division = find(divisionId);
        if (sync == nullptr || division == nullptr || sync->value < 0.5)
          continue;
        // One line to a block, for the first time or rate that follows the tempo.
        ++syncedTotal;
        if (static_cast<int>(synced.size()) < kMaxSynced) {
          const int note = juce::roundToInt(division->value);
          synced.push_back({slot.name, slot.category, slot.enabled, noteName(note), noteMs(note, effectiveBpm)});
        }
        break;
      }
    }
  resized();
  repaint();
}

// ####################
// LAYOUT
// ####################
void TempoView::resized() {
  auto area = getLocalBounds().reduced(kPadding);
  const int rows = juce::jmax(1, static_cast<int>(synced.size()));
  syncedCard = area.removeFromBottom(16 + 26 + 10 + 16 + rows * kRowHeight + 10 + 32 + 16);
  area.removeFromBottom(kGap);
  metronomeCard = area.removeFromRight(kMetronomeWidth);
  area.removeFromRight(kGap);
  tempoCard = area;

  // The tempo card: its label, the figure with minus and plus, and below it
  // Tap, the last taps and the source.
  auto inner = tempoCard.reduced(24, 20);
  tempoLabel = inner.removeFromTop(16);
  auto bottom = inner.removeFromBottom(kBottomHeight);
  auto middle = inner.withSizeKeepingCentre(inner.getWidth(), kNumberHeight);

  const int numberWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(type::mono(140.0f), number.getText().isEmpty() ? bpmText() : number.getText())) + 8;
  const int unitWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(type::mono(32.0f), "BPM")) + 4;
  const int total = kStepSize + 28 + numberWidth + 14 + unitWidth + 28 + kStepSize;
  auto row = middle.withSizeKeepingCentre(total, kNumberHeight);
  down.setBounds(row.removeFromLeft(kStepSize).withSizeKeepingCentre(kStepSize, kStepSize + 2));
  row.removeFromLeft(28);
  number.setBounds(row.removeFromLeft(numberWidth));
  row.removeFromLeft(14);
  bpmUnit = row.removeFromLeft(unitWidth).withTrimmedTop(84).withHeight(40);
  row.removeFromLeft(28);
  up.setBounds(row.removeFromLeft(kStepSize).withSizeKeepingCentre(kStepSize, kStepSize + 2));

  auto tapColumn = bottom.removeFromLeft(kTapColumn);
  tapButton->setBounds(tapColumn.withSizeKeepingCentre(kTapSize, kTapSize + 3).withY(tapColumn.getY()));
  tapCaption = tapColumn.withTrimmedTop(kTapSize + 8).withHeight(16);
  bottom.removeFromLeft(36);
  tapsArea = bottom.removeFromLeft(kTapsWidth);
  bottom.removeFromLeft(36);
  sourceArea = bottom;
  auto sourceFlow = sourceArea;
  sourceFlow.removeFromTop(16 + 8);
  source.setBounds(sourceFlow.removeFromTop(36).withWidth(source.idealWidth()));
  sourceFlow.removeFromTop(8);
  sourceText = sourceFlow.removeFromTop(38);
  sourceHint = sourceFlow.removeFromTop(16);

  // The metronome.
  auto metro = metronomeCard.reduced(24, 20);
  metronomeHead = metro.removeFromTop(26);
  metronomeSwitch.setBounds(metronomeHead.removeFromRight(40).withSizeKeepingCentre(40, 28));
  metro.removeFromTop(14);
  metronomeBody = metro;

  // The blocks that follow the tempo, and the clock under them.
  auto sync = syncedCard.reduced(24, 16);
  syncedHead = sync.removeFromTop(26);
  sync.removeFromTop(10);
  syncedColumns = sync.removeFromTop(16);
  syncedRows.clear();
  for (int i = 0; i < rows; ++i)
    syncedRows.push_back(sync.removeFromTop(kRowHeight));
  sync.removeFromTop(10);
  clockRow = sync.removeFromTop(32);
  auto clock = clockRow.withTrimmedLeft(130);
  sendClock.setBounds(clock.removeFromLeft(40).withSizeKeepingCentre(40, 28));
  clock.removeFromLeft(10 + 90 + 32);
  receiveClock.setBounds(clock.removeFromLeft(40).withSizeKeepingCentre(40, 28));
}

// ####################
// PAINT
// ####################
void TempoView::paint(juce::Graphics& g) {
  g.fillAll(colour::ground);
  live::paintCard(g, tempoCard);
  live::paintCard(g, metronomeCard);
  live::paintCard(g, syncedCard);
  paintTempoCard(g);
  paintMetronome(g);
  paintSynced(g);
}

void TempoView::paintTempoCard(juce::Graphics& g) {
  paint::label(g, "Tempo", tempoLabel);
  g.setFont(type::mono(32.0f, type::Weight::regular));
  g.setColour(colour::muted);
  g.drawText("BPM", bpmUnit, juce::Justification::centredLeft, false);

  g.setFont(type::mono(13.0f));
  g.setColour(colour::text);
  g.drawText("Tap " + type::dot() + " CC " + juce::String(tapCc), tapCaption, juce::Justification::centred, false);

  // The last taps: the gap each one counted, as a bar and in milliseconds.
  paint::label(g, "Last taps", tapsArea.withHeight(16));
  const auto bars = tapsArea.withTrimmedTop(16 + 8).withHeight(64);
  if (intervals.empty()) {
    live::paintHint(g, "Tap to set the tempo", bars, juce::Justification::centredLeft);
  } else {
    double mean = 0.0;
    for (double ms : intervals)
      mean += ms / static_cast<double>(intervals.size());
    const int count = static_cast<int>(intervals.size());
    for (int i = 0; i < count; ++i) {
      const double ms = intervals[static_cast<size_t>(i)];
      const int height = juce::roundToInt(juce::jlimit(8.0, 64.0, 36.0 + (ms - mean) * 1.6));
      const float opacity = juce::jmax(0.45f, 1.0f - 0.18f * static_cast<float>(count - 1 - i));
      const int x = bars.getX() + i * 32;
      g.setColour(colour::text.withAlpha(opacity));
      g.fillRoundedRectangle(static_cast<float>(x + 7), static_cast<float>(bars.getBottom() - height), 18.0f, static_cast<float>(height), 4.0f);
      g.setFont(type::mono(11.0f, type::Weight::regular));
      g.setColour(colour::muted);
      g.drawText(juce::String(juce::roundToInt(ms)), juce::Rectangle<int>(x, bars.getBottom() + 4, 32, 14), juce::Justification::centred, false);
    }
  }

  paint::label(g, "Tempo source", sourceArea.withHeight(16));
  const juce::String own = juce::String(bpm, 1) + " BPM";
  const auto words = hostSync ? sentence({{"The host sets the tempo, ", false},
                                          {juce::String(effectiveBpm, 1) + " BPM", true},
                                          {". This preset's ", false},
                                          {own, true},
                                          {" applies when it reports none.", false}})
                              : sentence({{"This preset stores ", false}, {own, true}, {". Time-based blocks follow it.", false}});
  words.draw(g, sourceText.toFloat());
  live::paintHint(g, live::kLater, sourceHint);
}

void TempoView::paintMetronome(juce::Graphics& g) {
  g.setFont(type::heading());
  g.setColour(colour::text);
  g.drawText("Metronome", metronomeHead, juce::Justification::centredLeft, true);
  paint::dashedOutline(g, metronomeBody.toFloat(), size::panelRadius, colour::faint);
  auto inner = metronomeBody.withSizeKeepingCentre(metronomeBody.getWidth() - 80, 70);
  g.setFont(type::heading());
  g.setColour(colour::text);
  g.drawText(live::kLater, inner.removeFromTop(30), juce::Justification::centredLeft, false);
  settings::paintNote(g, inner.withTrimmedTop(6), "Time signature, subdivision, volume, accent and the output it plays on.", 13.0f);
}

void TempoView::paintSynced(juce::Graphics& g) {
  g.setFont(type::heading());
  g.setColour(colour::text);
  g.drawText("Synced blocks in this preset", syncedHead, juce::Justification::centredLeft, true);
  if (syncedTotal > static_cast<int>(synced.size()))
    live::paintHint(g, "and " + juce::String(syncedTotal - static_cast<int>(synced.size())) + " more", syncedHead, juce::Justification::centredRight);

  // The columns of the table: 1.6 : 1 : 1.
  const auto columns = [](juce::Rectangle<int> row) {
    const int unit = row.getWidth() / 4;
    return std::array<juce::Rectangle<int>, 3>{row.withWidth(unit * 2).withTrimmedLeft(4), row.withTrimmedLeft(unit * 2 + 12).withWidth(unit), row.withTrimmedLeft(unit * 3 + 12)};
  };
  const auto head = columns(syncedColumns);
  paint::label(g, "Block", head[0]);
  paint::label(g, "Note value", head[1]);
  paint::label(g, "Time at " + juce::String(effectiveBpm, 1) + " BPM", head[2]);

  if (synced.empty()) {
    g.setColour(colour::line);
    g.fillRect(syncedRows[0].getX(), syncedRows[0].getY(), syncedRows[0].getWidth(), 1);
    live::paintHint(g, "No block of this preset follows the tempo.", syncedRows[0].withTrimmedLeft(4));
  }
  for (size_t i = 0; i < synced.size(); ++i) {
    const SyncedBlock& block = synced[i];
    const auto rowBox = syncedRows[i];
    g.setColour(colour::line);
    g.fillRect(rowBox.getX(), rowBox.getY(), rowBox.getWidth(), 1);
    const auto cells = columns(rowBox.withTrimmedTop(1));
    auto first = cells[0];
    const auto tile = first.removeFromLeft(32).withSizeKeepingCentre(32, 32).toFloat();
    g.setColour(categoryTint(block.category));
    g.fillRoundedRectangle(tile, size::buttonRadius);
    icons::draw(g, icons::path(block.category), tile.withSizeKeepingCentre(18.0f, 18.0f), categoryColour(block.category));
    first.removeFromLeft(12);
    g.setFont(type::bodyStrong());
    g.setColour(block.enabled ? colour::text : colour::muted);
    g.drawText(block.name, first.removeFromTop(first.getCentreY() - first.getY() + 2), juce::Justification::bottomLeft, true);
    g.setFont(type::sans(12.0f));
    g.setColour(colour::muted);
    g.drawText(categoryName(block.category), first, juce::Justification::topLeft, true);
    g.setFont(type::mono(14.0f, type::Weight::regular));
    g.setColour(block.enabled ? colour::text : colour::muted);
    g.drawText(block.note, cells[1], juce::Justification::centredLeft, false);
    g.drawText(juce::String(juce::roundToInt(block.ms)) + " ms", cells[2], juce::Justification::centredLeft, false);
  }

  // The clock: both directions come in a later build.
  paint::label(g, "MIDI clock", clockRow.withWidth(120));
  g.setFont(type::bodyStrong());
  g.setColour(colour::text);
  g.drawText("Send clock", sendClock.getBounds().translated(50, 0).withWidth(90), juce::Justification::centredLeft, false);
  g.drawText("Receive clock", receiveClock.getBounds().translated(50, 0).withWidth(110), juce::Justification::centredLeft, false);
  live::paintHint(g, live::kLater, clockRow, juce::Justification::centredRight);
}

}  // namespace pedaleira::ui

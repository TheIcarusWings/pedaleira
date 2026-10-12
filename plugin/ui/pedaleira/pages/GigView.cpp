#include "GigView.h"

#include "../Icons.h"
#include "../Paint.h"
#include "../Type.h"

#include <cmath>

namespace pedaleira::ui {

namespace {

constexpr int kStripHeight = 72;
constexpr int kPadding = 32;
constexpr int kScaleWidth = 620;
constexpr int kScaleHeight = 54;
// Pixels along the scale per cent: the band is 62 px wide, 5 cents either way.
constexpr float kPixelsPerCent = 6.2f;
constexpr int kTempoWidth = 400;
constexpr int kUtilWidth = 120;

const char* const kNotes[12] = {"C", "C", "D", "D", "E", "F", "F", "G", "G", "A", "A", "B"};
constexpr bool kSharp[12] = {false, true, false, true, false, false, true, false, true, false, true, false};

}  // namespace

GigView::Reading GigView::analyse(double frequency, double confidence) {
  Reading out;
  if (frequency < 20.0 || confidence < 0.5)
    return out;
  const double exact = 69.0 + 12.0 * std::log2(frequency / 440.0);
  const int midi = juce::roundToInt(exact);
  const int index = ((midi % 12) + 12) % 12;
  out.valid = true;
  out.note = kNotes[index];
  out.sharp = kSharp[index];
  out.cents = (exact - midi) * 100.0;
  out.hz = frequency;
  return out;
}

GigView::GigView(EngineClient& client) : engine(client) {
  setTitle("Gig View");
  setWantsKeyboardFocus(true);
  for (int i = 0; i < 8; ++i) {
    auto fs = std::make_unique<Footswitch>(FootswitchModel::letterOf(i));
    fs->setSize(Footswitch::Size::gig);
    const char* name = FootswitchModel::letterOf(i);
    fs->onPress = [this, name] { press(name); };
    fs->onRelease = [this, name] { release(name); };
    addAndMakeVisible(*fs);
    switches[static_cast<size_t>(i)] = std::move(fs);
  }
  struct Util {
    Footswitch& widget;
    const char* name;
    const char* command;
  };
  for (const Util& u : {Util{bankUp, "Bank Up", "bankUp"}, Util{bankDown, "Bank Down", "bankDown"}, Util{tap, "Tap", "tempo"}}) {
    u.widget.setSize(Footswitch::Size::utility);
    u.widget.setLabel(u.name);
    const char* command = u.command;
    u.widget.onPress = [this, command] { press(command); };
    u.widget.onRelease = [this, command] { release(command); };
    addAndMakeVisible(u.widget);
  }
  bankUp.setIcon(&icons::path(icons::Icon::chevronUp));
  bankDown.setIcon(&icons::path(icons::Icon::chevronDown));
  tap.setLook(Footswitch::Look::tap);

  leave.onClick = [this] {
    if (onExit)
      onExit();
  };
  addAndMakeVisible(leave);
}

GigView::~GigView() { stopTimer(); }

void GigView::press(const char* name) {
  held.addIfNotAlreadyThere(name);
  engine.run("footswitch.press", object({{"switch", name}}));
  refresh();
}

void GigView::release(const char* name) {
  if (!held.contains(name))
    return;
  held.removeString(name);
  engine.run("footswitch.release", object({{"switch", name}}));
  refresh();
}

void GigView::visibilityChanged() {
  if (isVisible()) {
    // The tuner listens only while the view is up.
    engine.run("tuner.setEnabled", object({{"enabled", true}}));
    refresh();
    readTuner();
    startTimerHz(30);
    grabKeyboardFocus();
  } else {
    for (const auto& name : juce::StringArray(held))
      release(name.toRawUTF8());
    stopTimer();
    engine.run("tuner.setEnabled", object({{"enabled", false}}));
    reading = {};
  }
}

juce::String GigView::modeNote() const {
  if (model.mode == "preset")
    return "Switches load presets, banks of 8";
  if (model.mode == "scene")
    return "Switches load scenes A-H";
  if (model.mode == "stomp")
    return "Each switch toggles its blocks";
  const auto plural = [](const juce::String& role) { return role + "s"; };
  return "Top row " + plural(model.hybridTop) + " " + type::dot() + " bottom row " + plural(model.hybridBottom);
}

void GigView::refresh() {
  model = FootswitchModel::read(engine);
  const double bpm = engine.read("tempo.get")["bpm"];
  if (bpm > 0.0)
    model.bpm = bpm;
  seenRevision = engine.revision();
  const auto faces = model.faces(model.mode);
  for (size_t i = 0; i < 8; ++i)
    applyFace(*switches[i], faces[i]);
  tap.setText(juce::String(juce::roundToInt(model.bpm)));
  cpu = engine.read("meters.get")["cpu"];
  repaint();
}

void GigView::readTuner() {
  const juce::var tuner = engine.read("tuner.get");
  const Reading next = analyse(tuner["frequency"], tuner["confidence"]);
  // A little smoothing keeps the needle from shaking; a new note starts over.
  if (next.valid && reading.valid && next.note == reading.note && next.sharp == reading.sharp)
    shownCents += 0.5 * (next.cents - shownCents);
  else
    shownCents = next.cents;
  reading = next;
  repaint(stripArea);
}

void GigView::timerCallback() {
  readTuner();
  if (engine.revision() != seenRevision)
    refresh();
  else if (++ticks % 6 == 0)
    cpu = engine.read("meters.get")["cpu"];

  // The beat light follows the tempo on the wall clock: four beats to a bar.
  const double beats = juce::Time::getMillisecondCounterHiRes() * model.bpm / 60000.0;
  const int nextBeat = static_cast<int>(std::fmod(std::floor(beats), 4.0)) + 1;
  const bool nextLit = std::fmod(beats, 1.0) < 0.3;
  if (nextBeat != beat || nextLit != beatLit) {
    beat = nextBeat;
    beatLit = nextLit;
    repaint(tempoBox);
  }
}

bool GigView::keyPressed(const juce::KeyPress& key) {
  if (key == juce::KeyPress::escapeKey && onExit) {
    onExit();
    return true;
  }
  return false;
}

void GigView::resized() {
  const int width = getWidth();
  stripArea = {0, 0, width, kStripHeight};
  noteArea = {kPadding, (kStripHeight - 56) / 2, 56, 56};
  scaleArea = {noteArea.getRight() + 28, (kStripHeight - kScaleHeight) / 2, kScaleWidth, kScaleHeight};
  leave.setBounds(width - kPadding - leave.idealWidth(), (kStripHeight - 36) / 2, leave.idealWidth(), 36);
  readoutArea = {scaleArea.getRight() + 28, 0, leave.getX() - 28 - (scaleArea.getRight() + 28), kStripHeight};

  const int top = kStripHeight + 28;
  const int leftWidth = width - 2 * kPadding - 40 - kTempoWidth;
  setlistArea = {kPadding, top, leftWidth, 22};
  nameArea = {kPadding, setlistArea.getBottom(), leftWidth, 100};
  sceneArea = {kPadding, nameArea.getBottom() + 4, leftWidth, 48};
  nextArea = {kPadding, sceneArea.getBottom() + 14, leftWidth, 30};
  tempoBox = {width - kPadding - kTempoWidth, top, kTempoWidth, 144};
  cpuArea = {tempoBox.getX() + 4, tempoBox.getBottom() + 14, kTempoWidth - 8, 20};

  panelBox = {kPadding, nextArea.getBottom() + 28, width - 2 * kPadding, getHeight() - kPadding - (nextArea.getBottom() + 28)};
  panelHead = {panelBox.getX() + 32, panelBox.getY() + 20, panelBox.getWidth() - 64, 26};
  const auto body = juce::Rectangle<int>(panelBox.getX() + 32, panelHead.getBottom() + 16, panelBox.getWidth() - 64,
                                         panelBox.getBottom() - 20 - (panelHead.getBottom() + 16));
  const int gridWidth = body.getWidth() - kUtilWidth - 40;
  const int columnWidth = (gridWidth - 3 * 16) / 4;
  const int cellHeight = Footswitch::heightFor(Footswitch::Size::gig);
  const int gridHeight = 2 * cellHeight + 24;
  const int gridTop = body.getY() + (body.getHeight() - gridHeight) / 2;
  for (int i = 0; i < 8; ++i)
    switches[static_cast<size_t>(i)]->setBounds(body.getX() + (i % 4) * (columnWidth + 16), gridTop + (i / 4) * (cellHeight + 24),
                                                columnWidth, cellHeight);
  const int utilHeight = Footswitch::heightFor(Footswitch::Size::utility);
  const int utilTotal = 3 * utilHeight + 2 * 14;
  int y = body.getY() + (body.getHeight() - utilTotal) / 2;
  for (Footswitch* u : {&bankUp, &bankDown, &tap}) {
    u->setBounds(body.getRight() - kUtilWidth, y, kUtilWidth, utilHeight);
    y += utilHeight + 14;
  }
}

void GigView::paintStrip(juce::Graphics& g) {
  g.setColour(colour::ground);
  g.fillRect(stripArea);
  g.setColour(colour::line);
  g.fillRect(0, stripArea.getBottom() - 1, stripArea.getWidth(), 1);

  const bool live = reading.valid;
  const bool good = live && inTune(shownCents);
  const juce::Colour signal = good ? colour::ok : colour::warn;

  // The nearest note.
  g.setColour(live ? colour::text : colour::faint);
  g.setFont(type::mono(48.0f, type::Weight::bold));
  if (live) {
    const int letterWidth = 30;
    auto box = noteArea.withSizeKeepingCentre(letterWidth + (reading.sharp ? 16 : 0), noteArea.getHeight());
    g.drawText(reading.note, box.removeFromLeft(letterWidth), juce::Justification::centred, false);
    if (reading.sharp) {
      g.setFont(type::mono(22.0f, type::Weight::bold));
      g.drawText("#", box.withTrimmedBottom(26), juce::Justification::centredLeft, false);
    }
  } else {
    g.drawText("-", noteArea, juce::Justification::centred, false);
  }

  // The cents scale: the in-tune band, ticks every 5 cents, the centre
  // line and the needle.
  const auto scale = scaleArea.toFloat();
  g.setColour(colour::ok.withAlpha(0.22f));
  g.fillRoundedRectangle(scale.getX() + 279.0f, scale.getY(), 62.0f, 30.0f, 4.0f);
  for (int c = -50; c <= 50; c += 5) {
    const bool major = c % 10 == 0;
    const float height = (c == 0 || std::abs(c) == 50) ? 22.0f : major ? 16.0f : 10.0f;
    const float x = (c + 50) * kPixelsPerCent + (c == 50 ? -2.0f : 0.0f);
    g.setColour(c == 0 ? colour::text : colour::faint);
    g.fillRect(scale.getX() + x, scale.getY() + 30.0f - height, 2.0f, height);
  }
  g.setColour(colour::text);
  g.fillRect(scale.getX() + 309.0f, scale.getY(), 2.0f, 30.0f);
  if (live) {
    const float centre = static_cast<float>(juce::jlimit(-50.0, 50.0, shownCents)) * kPixelsPerCent + 310.0f;
    g.setColour(signal);
    g.fillRoundedRectangle(scale.getX() + centre - 3.0f, scale.getY(), 6.0f, 30.0f, 3.0f);
  }
  g.setFont(type::mono(11.0f, type::Weight::regular));
  g.setColour(colour::muted);
  g.drawText("-50", juce::Rectangle<float>(scale.getX() - 2.0f, scale.getY() + 36.0f, 34.0f, 14.0f), juce::Justification::centredLeft, false);
  g.drawText("0", juce::Rectangle<float>(scale.getX() + 303.0f, scale.getY() + 36.0f, 14.0f, 14.0f), juce::Justification::centred, false);
  g.drawText("+50", juce::Rectangle<float>(scale.getX() + 580.0f, scale.getY() + 36.0f, 40.0f, 14.0f), juce::Justification::centredRight, false);

  // The readout.
  g.setFont(type::mono(22.0f));
  if (live) {
    const int rounded = juce::roundToInt(shownCents);
    g.setColour(signal);
    g.drawText((rounded > 0 ? "+" : "") + juce::String(rounded) + " ct " + type::dot() + " " +
                   juce::String(reading.hz, 1) + " Hz",
               readoutArea, juce::Justification::centredLeft, false);
  } else {
    g.setColour(colour::muted);
    g.drawText("Play a note", readoutArea, juce::Justification::centredLeft, false);
  }
}

void GigView::paint(juce::Graphics& g) {
  g.fillAll(colour::drop);
  paintStrip(g);

  // The preset.
  paint::label(g, model.setlistLine(), setlistArea, juce::Justification::centredLeft);
  {
    // 88 px unless the name is longer than the room, then smaller.
    const juce::String name = model.presetName.isNotEmpty() ? model.presetName : juce::String("New preset");
    juce::Font font = type::sans(88.0f, type::Weight::bold).withExtraKerningFactor(-0.02f);
    const float wanted = juce::GlyphArrangement::getStringWidth(font, name);
    if (wanted > nameArea.getWidth())
      font = font.withHeight(juce::jmax(40.0f, font.getHeight() * nameArea.getWidth() / wanted));
    g.setFont(font);
    g.setColour(colour::text);
    g.drawText(name, nameArea, juce::Justification::centredLeft, true);
  }
  {
    juce::String sceneName = model.sceneNames[static_cast<size_t>(juce::jmax(0, juce::String("ABCDEFGH").indexOf(model.activeScene)))];
    const juce::String line = model.activeScene + " " + type::dot() + " " + sceneName;
    g.setFont(type::sans(40.0f, type::Weight::bold));
    g.setColour(colour::iris);
    const int lineWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), line)) + 2;
    g.drawText(line, sceneArea.withWidth(juce::jmin(lineWidth, sceneArea.getWidth() - 80)), juce::Justification::centredLeft, true);
    g.setFont(type::sans(16.0f));
    g.setColour(colour::muted);
    g.drawText("Scene", sceneArea.withTrimmedLeft(juce::jmin(lineWidth, sceneArea.getWidth() - 80) + 16).withTrimmedTop(14),
               juce::Justification::centredLeft, false);
  }
  {
    g.setFont(type::sans(22.0f));
    g.setColour(colour::muted);
    const auto* next = model.nextPreset();
    if (next != nullptr) {
      const int labelWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), "Next: ")) + 2;
      auto row = nextArea;
      g.drawText("Next: ", row.removeFromLeft(labelWidth), juce::Justification::centredLeft, false);
      g.setFont(type::sans(22.0f, type::Weight::semibold));
      g.setColour(colour::text);
      g.drawText(juce::String(next->slot).paddedLeft('0', 3) + " " + next->name, row, juce::Justification::centredLeft, true);
    } else {
      g.drawText("Next: end of the setlist", nextArea, juce::Justification::centredLeft, true);
    }
  }

  // The tempo and its beat light.
  paint::panel(g, tempoBox.toFloat(), colour::ground);
  {
    auto inner = tempoBox.reduced(20, 16);
    auto head = inner.removeFromTop(20);
    paint::label(g, "Tempo", head);
    const auto light = juce::Rectangle<float>(20.0f, 20.0f).withCentre({head.getRight() - 10.0f, head.getCentreY() * 1.0f});
    g.setFont(type::sans(12.0f));
    g.setColour(colour::muted);
    g.drawText("Beat " + juce::String(beat), head.withTrimmedRight(34), juce::Justification::centredRight, false);
    if (beatLit) {
      g.setColour(colour::iris.withAlpha(0.22f));
      g.drawEllipse(light.expanded(2.0f), 4.0f);
      g.setColour(colour::iris);
      g.fillEllipse(light);
    } else {
      g.setColour(colour::raised);
      g.fillEllipse(light);
    }
    inner.removeFromTop(4);
    g.setFont(type::mono(76.0f));
    g.setColour(colour::text);
    const juce::String bpm = juce::String(model.bpm, 1);
    const int bpmWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), bpm)) + 2;
    g.drawText(bpm, inner.withWidth(bpmWidth).withHeight(88), juce::Justification::centredLeft, false);
    g.setFont(type::mono(24.0f));
    g.setColour(colour::muted);
    g.drawText("BPM", inner.withTrimmedLeft(bpmWidth + 12).withHeight(88).withTrimmedTop(44), juce::Justification::centredLeft, false);
  }

  // The CPU.
  {
    auto row = cpuArea;
    g.setFont(type::mono(12.0f));
    g.setColour(colour::muted);
    g.drawText("CPU", row.removeFromLeft(28), juce::Justification::centredLeft, false);
    g.setColour(colour::text);
    g.drawText(juce::String(juce::roundToInt(cpu * 100.0)) + "%", row.removeFromRight(36), juce::Justification::centredRight, false);
    row.reduce(12, 0);
    const auto bar = row.withSizeKeepingCentre(row.getWidth(), 6).toFloat();
    g.setColour(colour::raised);
    g.fillRoundedRectangle(bar, 3.0f);
    g.setColour(cpu > 0.85 ? colour::clip : cpu > 0.6 ? colour::warn : colour::ok);
    g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * static_cast<float>(juce::jlimit(0.0, 1.0, cpu))), 3.0f);
  }

  // The footswitches' panel.
  paint::panel(g, panelBox.toFloat(), colour::ground);
  {
    auto head = panelHead;
    paint::label(g, "Mode", head.removeFromLeft(48));
    head.removeFromLeft(14);
    const juce::String modeName = model.mode.substring(0, 1).toUpperCase() + model.mode.substring(1);
    g.setFont(type::heading());
    g.setColour(colour::text);
    const int modeWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), modeName)) + 2;
    g.drawText(modeName, head.removeFromLeft(modeWidth), juce::Justification::centredLeft, false);
    head.removeFromLeft(14);
    g.setFont(type::sans(13.0f));
    g.setColour(colour::muted);
    g.drawText(modeNote(), head, juce::Justification::centredLeft, true);
  }
}

}  // namespace pedaleira::ui

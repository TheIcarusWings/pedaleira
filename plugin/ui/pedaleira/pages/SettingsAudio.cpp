#include "SettingsAudio.h"

#include "../Paint.h"
#include "../Type.h"
#include "EditorParts.h"

#include <algorithm>

namespace pedaleira::ui {

namespace {

constexpr int kRowHeight = 34;
constexpr int kMaxListed = 8;
constexpr int kListHead = 22;

using settings::kCardBottom;
using settings::kCardGap;
using settings::kCardPadding;
using settings::kCardTop;
using settings::kFieldHeight;

juce::StringArray stringsOf(const juce::var& list) {
  juce::StringArray out;
  if (const auto* array = list.getArray())
    for (const auto& item : *array)
      out.add(item.toString());
  return out;
}

juce::String rateText(double rate) {
  juce::String text(rate / 1000.0, 1);
  if (text.endsWith(".0"))
    text = text.dropLastCharacters(2);
  return text + " kHz";
}

juce::Rectangle<int> labelAbove(juce::Rectangle<int> field) { return field.withY(field.getY() - 22).withHeight(16); }

juce::String rowsText(const juce::StringArray& rows) {
  if (rows.isEmpty())
    return "None";
  return (rows.size() == 1 ? "Row " : "Rows ") + rows.joinIntoString(", ");
}

}  // namespace

// ####################
// SETUP
// ####################
SettingsAudio::SettingsAudio(EngineClient& client, DeviceSettings* deviceSettings)
    : SettingsSection(client, "Audio and calibration",
                      "Pick your interface and tell Pedaleira how loud it is, so NAM captures play back at the level "
                      "they were captured at."),
      device(deviceSettings) {
  for (SelectBox* box : {&driverBox, &outputBox, &inputBox, &linkedBox, &rateBox, &bufferBox, &factorBox}) {
    box->onClick = [this, box] { menuFor(*box, box->getTitle()); };
    addChildComponent(box);
  }
  hearToggle.onToggle = [this](bool on) {
    if (device != nullptr)
      applied(device->setHearYourself(on));
  };
  testTone.onClick = [this] {
    if (device != nullptr)
      applied(device->playTestTone());
  };
  controlPanel.onClick = [this] {
    if (device != nullptr)
      applied(device->openControlPanel());
  };
  micFix.onClick = [this] {
    if (device != nullptr)
      applied(device->openMicSettings());
  };
  for (juce::Component* c : std::initializer_list<juce::Component*>{&hearToggle, &testTone, &controlPanel, &micFix})
    addChildComponent(c);

  gateSwitch.onToggle = [this](bool on) { setParam("gateEnabled", on ? 1.0 : 0.0); };
  threshold.setRange(-100.0, 0.0);
  threshold.onCommit = [this](double v) { setParam("gateThreshold", v); };
  release.setRange(5.0, 500.0);
  release.onCommit = [this](double v) { setParam("gateRelease", v); };
  hold.setRange(0.0, 200.0);
  hold.onCommit = [this](double v) { setParam("gateHold", v); };
  range.setRange(20.0, 80.0);
  range.onCommit = [this](double v) { setParam("gateRange", v); };

  maxLevel.setRange(-60.0, 60.0);
  maxLevel.onCommit = [this](double v) { setParam("inputCalibrationLevel", v); };
  applyCalibration.onToggle = [this](bool on) { setParam("calibrateInput", on ? 1.0 : 0.0); };
  loudness.setRange(-60.0, 0.0);
  loudness.onCommit = [this](double v) { setParam("targetLoudness", v); };
  // Measuring the interface's level with a tone is the capture wizard's
  // level step (UI step 8); the button waits for it.
  measure.setEnabled(false);

  oversampling.onToggle = [this](bool on) {
    setParam("osEnabled", on ? 1.0 : 0.0);
    refresh();
  };

  for (juce::Component* c : std::initializer_list<juce::Component*>{
           &gateSwitch, &threshold, &release, &hold, &range, &maxLevel, &applyCalibration, &loudness, &measure,
           &oversampling})
    addAndMakeVisible(c);
  factorBox.setVisible(true);
}

SettingsAudio::~SettingsAudio() = default;

// ####################
// DATA
// ####################
double SettingsAudio::param(const juce::String& id, double fallback) const {
  const juce::var entry = params[juce::Identifier(id)];
  return entry.isVoid() ? fallback : static_cast<double>(entry["value"]);
}

void SettingsAudio::setParam(const juce::String& id, double value) {
  run("Could not change the setting", "param.set", object({{"id", id}, {"value", value}}));
  readParams();
  repaint();
}

void SettingsAudio::readParams() {
  params = engine.read("param.list");
  gateSwitch.setState(param("gateEnabled", 1.0) > 0.5);
  threshold.setValue(param("gateThreshold", -80.0));
  release.setValue(param("gateRelease", 50.0));
  hold.setValue(param("gateHold", 20.0));
  range.setValue(param("gateRange", 80.0));
  maxLevel.setValue(param("inputCalibrationLevel", 12.0));
  applyCalibration.setState(param("calibrateInput") > 0.5);
  loudness.setValue(param("targetLoudness", -18.0));
  const bool osOn = param("osEnabled") > 0.5;
  oversampling.setState(osOn);
  const int factor = juce::jlimit(0, 2, juce::roundToInt(param("osFactor")));
  factorBox.setChoice(juce::String(1 << (factor + 1)) + "x");
  factorBox.setEnabled(osOn);
}

void SettingsAudio::refresh() {
  grid = GridModel::read(engine);
  readParams();
  refreshDevice();
}

void SettingsAudio::deviceChanged() { refreshDevice(); }

void SettingsAudio::refreshDevice() {
  dev = device != nullptr ? device->state() : juce::var();
  if (device != nullptr) {
    latency = device->latencyMs();
    dropouts = device->dropouts();
    driverBox.setChoice(dev["currentType"].toString());
    outputBox.setChoice(dev["outputDevice"].toString());
    inputBox.setChoice(dev["inputDevice"].toString());
    linkedBox.setChoice(dev["outputDevice"].toString());
    const double rate = dev["sampleRate"];
    rateBox.setChoice(rate > 0.0 ? rateText(rate) : juce::String("Not open"));
    const int buffer = dev["bufferSize"];
    bufferBox.setChoice(buffer > 0 ? juce::String(buffer) + " samples" : juce::String("Not open"));
    hearToggle.setState(static_cast<bool>(dev["hearYourself"]));
  }
  rebuildRows();
  relayout();
  heightChanged();
  repaint();
}

int SettingsAudio::inputRowCount() const {
  const auto* channels = dev["inputChannels"].getArray();
  return channels == nullptr ? 0 : juce::jmin(kMaxListed, channels->size());
}

int SettingsAudio::outputRowCount() const {
  if (device == nullptr || !static_cast<bool>(dev["deviceOpen"]))
    return 0;
  const auto* pairs = dev["outputPairs"].getArray();
  // A device with two outputs has no pairs to pick: one fixed row.
  return pairs == nullptr || pairs->isEmpty() ? 1 : juce::jmin(kMaxListed, pairs->size());
}

bool SettingsAudio::micDenied() const { return dev["micPermission"].toString() == "denied"; }

void SettingsAudio::rebuildRows() {
  const auto fill = [this](std::vector<std::unique_ptr<Toggle>>& toggles, int count, const juce::String& what,
                           const std::function<void(int, bool)>& changed) {
    if (static_cast<int>(toggles.size()) != count) {
      toggles.clear();
      for (int i = 0; i < count; ++i) {
        auto toggle = std::make_unique<Toggle>(what + " " + juce::String(i + 1));
        Toggle* raw = toggle.get();
        raw->onClick = [raw, i, changed] { changed(i, raw->getToggleState()); };
        addAndMakeVisible(*toggle);
        toggles.push_back(std::move(toggle));
      }
    }
  };
  fill(inputToggles, inputRowCount(), "Input", [this](int i, bool on) { toggleInput(i, on); });
  const bool fixedPair = outputRowCount() == 1 && stringsOf(dev["outputPairs"]).isEmpty();
  fill(outputToggles, fixedPair ? 0 : outputRowCount(), "Output pair", [this](int i, bool on) { toggleOutput(i, on); });

  if (const auto* channels = dev["inputChannels"].getArray())
    for (int i = 0; i < static_cast<int>(inputToggles.size()) && i < channels->size(); ++i)
      inputToggles[static_cast<size_t>(i)]->setToggleState(static_cast<bool>(channels->getReference(i)["active"]),
                                                           juce::dontSendNotification);
  const int activePair = dev["activeOutputPair"];
  for (int i = 0; i < static_cast<int>(outputToggles.size()); ++i)
    outputToggles[static_cast<size_t>(i)]->setToggleState(i == activePair, juce::dontSendNotification);
}

void SettingsAudio::applied(const juce::var& result) {
  lastError = static_cast<bool>(result["ok"]) ? juce::String() : result["error"].toString();
  refreshDevice();
}

void SettingsAudio::chooseBuffer(int samples) {
  if (device != nullptr)
    applied(device->setBufferSize(samples));
}

void SettingsAudio::toggleInput(int channel, bool on) {
  if (device == nullptr)
    return;
  // One channel is mono and two are stereo; the oldest choice gives way.
  juce::Array<juce::var> active;
  if (const auto* channels = dev["inputChannels"].getArray())
    for (int i = 0; i < channels->size(); ++i)
      if (static_cast<bool>(channels->getReference(i)["active"]))
        active.add(i);
  if (on) {
    active.addIfNotAlreadyThere(channel);
    while (active.size() > 2)
      active.remove(0);
  } else {
    active.removeFirstMatchingValue(channel);
  }
  // An input has to stay on: put the switch back.
  if (active.isEmpty()) {
    refreshDevice();
    return;
  }
  applied(device->setInputChannels(active));
}

void SettingsAudio::toggleOutput(int pair, bool on) {
  if (device == nullptr)
    return;
  if (!on) {
    refreshDevice();
    return;
  }
  applied(device->setOutputPair(pair));
}

juce::String SettingsAudio::usedByInput(int channel) const {
  // The grid names an input "Input 3" or, for a stereo row, "Input 3/4".
  juce::StringArray rows;
  for (const auto& row : grid.rows) {
    if (!row.routed || row.source != "device" || !row.inputName.startsWith("Input "))
      continue;
    const juce::String numbers = row.inputName.fromFirstOccurrenceOf("Input ", false, false);
    const int first = numbers.getIntValue();
    const int last = numbers.contains("/") ? numbers.fromFirstOccurrenceOf("/", false, false).getIntValue() : first;
    if (channel >= first && channel <= last)
      rows.add(juce::String(row.index + 1));
  }
  return rowsText(rows);
}

juce::String SettingsAudio::usedByOutput(int pair) const {
  const juce::String wanted = "Out " + juce::String(pair * 2 - 1) + "/" + juce::String(pair * 2);
  juce::StringArray rows;
  for (const auto& row : grid.rows)
    if (row.routed && row.sink == "device" && row.outputName == wanted)
      rows.add(juce::String(row.index + 1));
  return rowsText(rows);
}

void SettingsAudio::menuFor(SelectBox& box, const juce::String& key) {
  if (device == nullptr)
    return;
  juce::StringArray items;
  int selected = -1;
  std::function<void(int)> chosen;
  if (&box == &driverBox) {
    items = stringsOf(dev["deviceTypes"]);
    selected = items.indexOf(dev["currentType"].toString());
    chosen = [this, items](int i) { applied(device->setDeviceType(items[i])); };
  } else if (&box == &outputBox || &box == &linkedBox) {
    items = stringsOf(dev["outputDevices"]);
    selected = items.indexOf(dev["outputDevice"].toString());
    const juce::String kind = &box == &linkedBox ? "linked" : "output";
    chosen = [this, items, kind](int i) { applied(device->setDevice(kind, items[i])); };
  } else if (&box == &inputBox) {
    items = stringsOf(dev["inputDevices"]);
    selected = items.indexOf(dev["inputDevice"].toString());
    chosen = [this, items](int i) { applied(device->setDevice("input", items[i])); };
  } else if (&box == &rateBox) {
    juce::Array<double> rates;
    for (const auto& rate : stringsOf(dev["sampleRates"])) {
      rates.add(rate.getDoubleValue());
      items.add(rateText(rate.getDoubleValue()));
    }
    selected = rates.indexOf(static_cast<double>(dev["sampleRate"]));
    chosen = [this, rates](int i) { applied(device->setSampleRate(rates[i])); };
  } else if (&box == &bufferBox) {
    juce::Array<int> sizes;
    for (const auto& size : stringsOf(dev["bufferSizes"])) {
      sizes.add(size.getIntValue());
      items.add(size + " samples");
    }
    selected = sizes.indexOf(static_cast<int>(dev["bufferSize"]));
    chosen = [this, sizes](int i) { chooseBuffer(sizes[i]); };
  }
  juce::ignoreUnused(key);
  if (!items.isEmpty())
    settings::chooseFromMenu(box, items, selected, chosen);
}

void SettingsAudio::tick() {
  // The input level under the calibration, and the device's own counters.
  const juce::var input = engine.read("meters.get")["input"];
  const double level = juce::jmax(static_cast<double>(input[0]), static_cast<double>(input[1]));
  bool changed = std::abs(level - inputDb) > 0.05;
  inputDb = level;
  if (device != nullptr) {
    const double now = device->latencyMs();
    const int missed = device->dropouts();
    if (std::abs(now - latency) > 0.01 || missed != dropouts) {
      latency = now;
      dropouts = missed;
      repaint(lay.strip);
    }
  }
  if (changed)
    repaint(lay.levelRow);
}

// ####################
// LAYOUT
// ####################
int SettingsAudio::heightFor(int width) const { return layoutFor(width).height; }

SettingsAudio::Layout SettingsAudio::layoutFor(int width) const {
  Layout l;
  const int leftWidth = (width - kCardGap) * 11 / 21;
  const int rightWidth = width - kCardGap - leftWidth;
  const int rightX = leftWidth + kCardGap;
  const int pad = kCardPadding;

  // ---- Left: device, inputs, noise gate ----
  int y = 0;
  {
    const int top = y;
    const int cx = pad, cw = leftWidth - 2 * pad;
    int cy = top + kCardTop;
    if (device == nullptr) {
      const int h = settings::noteHeight(cw, "The audio device is chosen in the standalone application's own window.");
      l.noDevice = {cx, cy, cw, h};
      cy += h;
    } else {
      const int half = (cw - 16) / 2;
      if (stringsOf(dev["deviceTypes"]).size() > 1) {
        l.driver = {cx, cy + 22, cw, kFieldHeight};
        cy += settings::kLabelledField + 14;
      }
      if (static_cast<bool>(dev["separateIO"]) || dev["separateIO"].isVoid()) {
        l.output = {cx, cy + 22, half, kFieldHeight};
        l.input = {cx + half + 16, cy + 22, cw - half - 16, kFieldHeight};
      } else {
        l.linked = {cx, cy + 22, cw, kFieldHeight};
      }
      cy += settings::kLabelledField + 14;
      l.rate = {cx, cy + 22, half, kFieldHeight};
      l.buffer = {cx + half + 16, cy + 22, cw - half - 16, kFieldHeight};
      cy += settings::kLabelledField + 14;
      l.strip = {cx, cy, cw, 44};
      cy += 44;
      if (lastError.isNotEmpty()) {
        cy += 10;
        const int h = settings::noteHeight(cw, lastError);
        l.error = {cx, cy, cw, h};
        cy += h;
      }
      if (micDenied()) {
        cy += 12;
        l.mic = {cx, cy, cw, 36};
        cy += 36;
      }
      cy += 12;
      l.hear = {cx, cy, cw, 44};
      cy += 44 + 6;
      l.buttons = {cx, cy, cw, 36};
      cy += 36;
    }
    const int bottom = cy + kCardBottom;
    l.device = {0, top, leftWidth, bottom - top};
    y = bottom + kCardGap;
  }
  {
    const int top = y;
    const int cx = pad, cw = leftWidth - 2 * pad;
    int cy = top + kCardTop;
    l.inputHead = {cx, cy, cw, kListHead};
    cy += kListHead;
    const int rows = inputRowCount();
    for (int i = 0; i < rows; ++i) {
      l.inputRows.push_back({cx, cy, cw, kRowHeight});
      cy += kRowHeight;
    }
    if (rows == 0) {
      l.inputMore = {cx, cy, cw, kRowHeight};
      cy += kRowHeight;
    } else if (const auto* channels = dev["inputChannels"].getArray(); channels != nullptr && channels->size() > rows) {
      l.inputMore = {cx, cy, cw, kRowHeight};
      cy += kRowHeight;
    }
    const int bottom = cy + kCardBottom;
    l.inputs = {0, top, leftWidth, bottom - top};
    y = bottom + kCardGap;
  }
  {
    const int top = y;
    const int cx = pad, cw = leftWidth - 2 * pad;
    const int half = (cw - 16) / 2;
    int cy = top + kCardTop;
    l.gateSwitch = {cx, cy, cw, 36};
    cy += 36 + 14;
    l.threshold = {cx, cy + 22, half, kFieldHeight};
    l.release = {cx + half + 16, cy + 22, cw - half - 16, kFieldHeight};
    cy += settings::kLabelledField + 14;
    l.hold = {cx, cy + 22, half, kFieldHeight};
    l.range = {cx + half + 16, cy + 22, cw - half - 16, kFieldHeight};
    cy += settings::kLabelledField;
    const int bottom = cy + kCardBottom;
    l.gate = {0, top, leftWidth, bottom - top};
    y = bottom;
  }
  const int leftBottom = y;

  // ---- Right: outputs, calibration, performance ----
  y = 0;
  {
    const int top = y;
    const int cx = rightX + pad, cw = rightWidth - 2 * pad;
    int cy = top + kCardTop;
    l.outputHead = {cx, cy, cw, kListHead};
    cy += kListHead;
    const int rows = outputRowCount();
    for (int i = 0; i < rows; ++i) {
      l.outputRows.push_back({cx, cy, cw, kRowHeight});
      cy += kRowHeight;
    }
    if (rows == 0)
      cy += kRowHeight;
    const int bottom = cy + kCardBottom;
    l.outputs = {rightX, top, rightWidth, bottom - top};
    y = bottom + kCardGap;
  }
  {
    const int top = y;
    const int cx = rightX + pad, cw = rightWidth - 2 * pad;
    int cy = top + kCardTop;
    l.maxLabel = {cx, cy, cw, 16};
    cy += 22;
    l.maxField = {cx, cy, 140, kFieldHeight};
    const juce::String note =
        "Find this in your interface's manual. NAM captures with calibration data then play at the level they "
        "were captured at.";
    const int noteH = settings::noteHeight(cw - 156, note);
    l.maxNote = {cx + 156, cy, cw - 156, noteH};
    cy += juce::jmax(kFieldHeight, noteH) + 10;
    l.applyRow = {cx, cy, cw, 36};
    cy += 36 + 4;
    l.loudRow = {cx, cy, cw, 36};
    cy += 36 + 14;
    l.measure = {cx, cy, 190, 36};
    l.measureNote = {cx + 206, cy, cw - 206, 36};
    cy += 36 + 12;
    l.levelRow = {cx, cy, cw, 22};
    cy += 22;
    const int bottom = cy + kCardBottom;
    l.calibration = {rightX, top, rightWidth, bottom - top};
    y = bottom + kCardGap;
  }
  {
    const int top = y;
    const int cx = rightX + pad, cw = rightWidth - 2 * pad;
    int cy = top + kCardTop;
    l.osRow = {cx, cy, cw, 40};
    cy += 40;
    l.factorRow = {cx, cy, cw, 40};
    cy += 40;
    l.multicoreRow = {cx, cy, cw, 40};
    cy += 40;
    l.cpuRow = {cx, cy, cw, 40};
    cy += 40;
    const int bottom = cy + kCardBottom;
    l.performance = {rightX, top, rightWidth, bottom - top};
    y = bottom;
  }
  l.height = juce::jmax(leftBottom, y);
  return l;
}

void SettingsAudio::relayout() {
  lay = layoutFor(getWidth());
  applyLayout();
}

void SettingsAudio::resized() { relayout(); }

void SettingsAudio::applyLayout() {
  const bool has = device != nullptr;
  const bool separate = has && !lay.output.isEmpty();
  driverBox.setVisible(!lay.driver.isEmpty());
  driverBox.setBounds(lay.driver);
  outputBox.setVisible(separate);
  outputBox.setBounds(lay.output);
  inputBox.setVisible(separate);
  inputBox.setBounds(lay.input);
  linkedBox.setVisible(has && !separate);
  linkedBox.setBounds(lay.linked);
  rateBox.setVisible(has);
  rateBox.setBounds(lay.rate);
  bufferBox.setVisible(has);
  bufferBox.setBounds(lay.buffer);
  hearToggle.setVisible(has);
  hearToggle.setBounds(lay.hear);
  testTone.setVisible(has);
  const int toneWidth = testTone.idealWidth();
  testTone.setBounds(lay.buttons.withWidth(toneWidth));
  controlPanel.setVisible(has && static_cast<bool>(dev["hasControlPanel"]));
  controlPanel.setBounds(lay.buttons.withTrimmedLeft(toneWidth + 10).withWidth(controlPanel.idealWidth()));
  micFix.setVisible(!lay.mic.isEmpty());
  micFix.setBounds(lay.mic.withLeft(lay.mic.getRight() - micFix.idealWidth()));

  for (size_t i = 0; i < inputToggles.size() && i < lay.inputRows.size(); ++i)
    inputToggles[i]->setBounds(lay.inputRows[i].withLeft(lay.inputRows[i].getRight() - 44).withSizeKeepingCentre(44, 28));
  for (size_t i = 0; i < outputToggles.size() && i < lay.outputRows.size(); ++i)
    outputToggles[i]->setBounds(lay.outputRows[i].withLeft(lay.outputRows[i].getRight() - 44).withSizeKeepingCentre(44, 28));

  gateSwitch.setBounds(lay.gateSwitch);
  threshold.setBounds(lay.threshold);
  release.setBounds(lay.release);
  hold.setBounds(lay.hold);
  range.setBounds(lay.range);
  maxLevel.setBounds(lay.maxField);
  applyCalibration.setBounds(lay.applyRow);
  loudness.setBounds(lay.loudRow.withLeft(lay.loudRow.getRight() - 130));
  measure.setBounds(lay.measure);
  oversampling.setBounds(lay.osRow);
  factorBox.setBounds(lay.factorRow.withLeft(lay.factorRow.getRight() - 96).withSizeKeepingCentre(96, 30));
}

// ####################
// PAINT
// ####################
void SettingsAudio::paint(juce::Graphics& g) {
  using namespace settings;

  // ---- Device ----
  paintCard(g, lay.device, "Device");
  if (device == nullptr) {
    paintNote(g, lay.noDevice, "The audio device is chosen in the standalone application's own window.");
  } else {
    const struct {
      const char* text;
      juce::Rectangle<int> field;
    } labels[] = {{"Driver", lay.driver},     {"Output device", lay.output}, {"Input device", lay.input},
                  {"Device", lay.linked},     {"Sample rate", lay.rate},     {"Buffer size", lay.buffer}};
    for (const auto& label : labels)
      if (!label.field.isEmpty())
        paint::label(g, label.text, labelAbove(label.field));

    // Latency and dropouts.
    g.setColour(colour::raised);
    g.fillRoundedRectangle(lay.strip.toFloat(), size::panelRadius);
    auto strip = lay.strip.reduced(14, 0);
    const bool open = static_cast<bool>(dev["deviceOpen"]);
    if (!open) {
      g.setFont(type::body());
      g.setColour(colour::warn);
      g.drawText("No audio device is open", strip, juce::Justification::centredLeft, false);
    } else {
      juce::String valueText = latency > 0.0 ? juce::String(latency, 1) + " ms" : juce::String("not reported");
      g.setFont(type::mono(13.0f, type::Weight::regular));
      g.setColour(colour::muted);
      g.drawText("Round-trip latency", strip, juce::Justification::centredLeft, false);
      const int labelWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(
                                 type::mono(13.0f, type::Weight::regular), "Round-trip latency")) + 8;
      g.setFont(latency > 0.0 ? type::mono(16.0f) : type::mono(13.0f, type::Weight::regular));
      g.setColour(latency > 0.0 ? colour::text : colour::muted);
      g.drawText(valueText, strip.withTrimmedLeft(labelWidth), juce::Justification::centredLeft, false);
      if (dropouts >= 0) {
        const juce::String text = dropouts == 0 ? juce::String("No dropouts since start")
                                                : juce::String(dropouts) + (dropouts == 1 ? " dropout" : " dropouts") +
                                                      " since start";
        const int w = paint::chipWidth(text);
        paint::chip(g, text, strip.removeFromRight(w).withSizeKeepingCentre(w, 24).toFloat(),
                    dropouts == 0 ? paint::ChipStyle::ok : paint::ChipStyle::warn);
      }
    }
    if (!lay.error.isEmpty()) {
      juce::AttributedString text;
      text.append(lastError, type::sans(13.0f), colour::warn);
      text.draw(g, lay.error.toFloat());
    }
    if (!lay.mic.isEmpty()) {
      g.setFont(type::body());
      g.setColour(colour::warn);
      g.drawText("Microphone access is off, so the input is silent.",
                 lay.mic.withTrimmedRight(micFix.getWidth() + 12), juce::Justification::centredLeft, true);
    }
  }

  // ---- Inputs ----
  paintCard(g, lay.inputs, "Inputs");
  if (!lay.inputRows.empty()) {
    paint::label(g, "Input", lay.inputHead.withWidth(96));
    paint::label(g, "Channel", lay.inputHead.withTrimmedLeft(108));
    paint::label(g, "Used by", lay.inputHead.withTrimmedRight(56).withLeft(lay.inputHead.getRight() - 56 - 110));
    paint::label(g, "On", lay.inputHead.withLeft(lay.inputHead.getRight() - 44), juce::Justification::centred);
  }
  const auto* channels = dev["inputChannels"].getArray();
  for (size_t i = 0; i < lay.inputRows.size(); ++i) {
    auto row = lay.inputRows[i];
    paintRowLine(g, row);
    const bool on = i < inputToggles.size() && inputToggles[i]->getToggleState();
    const juce::Colour fg = on ? colour::text : colour::muted;
    g.setFont(type::sans(13.0f, type::Weight::semibold));
    g.setColour(fg);
    g.drawText("Input " + juce::String(i + 1), row.removeFromLeft(96), juce::Justification::centredLeft, true);
    row.removeFromLeft(12);
    row.removeFromRight(56);
    g.setFont(type::sans(13.0f));
    g.setColour(colour::muted);
    const juce::String used = usedByInput(static_cast<int>(i) + 1);
    g.setColour(used == "None" ? colour::muted : colour::text);
    g.drawText(used, row.removeFromRight(110), juce::Justification::centredLeft, true);
    g.setColour(colour::muted);
    if (channels != nullptr && static_cast<int>(i) < channels->size())
      g.drawText(channels->getReference(static_cast<int>(i))["name"].toString(), row, juce::Justification::centredLeft, true);
  }
  if (!lay.inputMore.isEmpty()) {
    g.setFont(type::sans(13.0f));
    g.setColour(colour::muted);
    const int total = channels != nullptr ? channels->size() : 0;
    g.drawText(total == 0 ? juce::String(device == nullptr ? "Set in the standalone application's window"
                                                            : "No inputs on this device")
                          : juce::String(total - inputRowCount()) + " more channels on this device",
               lay.inputMore, juce::Justification::centredLeft, true);
  }

  // ---- Noise gate ----
  paintCard(g, lay.gate, "Noise gate");
  paint::label(g, "Threshold", labelAbove(lay.threshold));
  paint::label(g, "Release", labelAbove(lay.release));
  paint::label(g, "Hold", labelAbove(lay.hold));
  paint::label(g, "Range", labelAbove(lay.range));

  // ---- Outputs ----
  paintCard(g, lay.outputs, "Outputs");
  if (!lay.outputRows.empty()) {
    paint::label(g, "Output", lay.outputHead.withWidth(96));
    paint::label(g, "Channels", lay.outputHead.withTrimmedLeft(108));
    const int usedRight = lay.outputHead.getRight() - (outputToggles.empty() ? 0 : 56);
    paint::label(g, "Used by", lay.outputHead.withRight(usedRight).withLeft(usedRight - 110));
    if (!outputToggles.empty())
      paint::label(g, "On", lay.outputHead.withLeft(lay.outputHead.getRight() - 44), juce::Justification::centred);
  }
  const auto* pairs = dev["outputPairs"].getArray();
  for (size_t i = 0; i < lay.outputRows.size(); ++i) {
    auto row = lay.outputRows[i];
    paintRowLine(g, row);
    const bool fixed = outputToggles.empty();
    const bool on = fixed || (i < outputToggles.size() && outputToggles[i]->getToggleState());
    g.setFont(type::sans(13.0f, type::Weight::semibold));
    g.setColour(on ? colour::text : colour::muted);
    g.drawText("Out " + juce::String(i * 2 + 1) + "/" + juce::String(i * 2 + 2), row.removeFromLeft(96),
               juce::Justification::centredLeft, true);
    row.removeFromLeft(12);
    if (!fixed)
      row.removeFromRight(56);
    const juce::String used = usedByOutput(static_cast<int>(i) + 1);
    g.setFont(type::sans(13.0f));
    g.setColour(used == "None" ? colour::muted : colour::text);
    g.drawText(used, row.removeFromRight(110), juce::Justification::centredLeft, true);
    g.setColour(colour::muted);
    if (pairs != nullptr && static_cast<int>(i) < pairs->size())
      g.drawText(pairs->getReference(static_cast<int>(i)).toString(), row, juce::Justification::centredLeft, true);
    else if (fixed)
      g.drawText("Output 1 + 2", row, juce::Justification::centredLeft, true);
  }
  if (lay.outputRows.empty()) {
    g.setFont(type::sans(13.0f));
    g.setColour(colour::muted);
    g.drawText(device == nullptr ? "Set in the standalone application's window" : "No outputs on this device",
               juce::Rectangle<int>(lay.outputHead.getX(), lay.outputHead.getBottom(), lay.outputHead.getWidth(), kRowHeight),
               juce::Justification::centredLeft, true);
  }

  // ---- Calibration ----
  paintCard(g, lay.calibration, "Calibration");
  paint::label(g, "Interface max input level", lay.maxLabel);
  paintNote(g, lay.maxNote,
            "Find this in your interface's manual. NAM captures with calibration data then play at the level they "
            "were captured at.");
  g.setFont(type::body());
  g.setColour(colour::text);
  g.drawText("Loudness target", lay.loudRow.withTrimmedRight(140), juce::Justification::centredLeft, true);
  g.setFont(type::sans(12.0f));
  g.setColour(colour::faint);
  g.drawText(kLaterBuild, lay.measureNote, juce::Justification::centredLeft, true);
  {
    auto row = lay.levelRow;
    g.setFont(type::sans(12.0f));
    g.setColour(colour::muted);
    g.drawText("Input level", row.removeFromLeft(76), juce::Justification::centredLeft, false);
    const auto value = row.removeFromRight(76);
    g.setFont(type::mono(12.0f, type::Weight::regular));
    g.setColour(colour::text);
    g.drawText(inputDb <= -59.9 ? juce::String("-inf dBFS") : juce::String(inputDb, 1) + " dBFS", value,
               juce::Justification::centredRight, false);
    paintBar(g, row.withSizeKeepingCentre(row.getWidth() - 8, 8), (inputDb + 60.0) / 60.0,
             inputDb > -1.0 ? colour::clip : inputDb > -6.0 ? colour::warn : colour::ok);
  }

  // ---- Performance ----
  paintCard(g, lay.performance, "Performance");
  paintRowLine(g, lay.osRow);
  paintRowLine(g, lay.factorRow);
  paintRowLine(g, lay.multicoreRow);
  g.setFont(type::body());
  g.setColour(oversampling.getState() ? colour::text : colour::muted);
  g.drawText("Oversampling factor", lay.factorRow.withTrimmedRight(110), juce::Justification::centredLeft, true);
  for (const auto& [row, text] : {std::pair<juce::Rectangle<int>, const char*>{lay.multicoreRow, "Use multicore worker pool"},
                                  {lay.cpuRow, "CPU warning threshold"}}) {
    g.setFont(type::body());
    g.setColour(colour::muted);
    g.drawText(text, row, juce::Justification::centredLeft, true);
    g.setFont(type::sans(12.0f));
    g.setColour(colour::faint);
    g.drawText(kLaterBuild, row, juce::Justification::centredRight, true);
  }
}

}  // namespace pedaleira::ui

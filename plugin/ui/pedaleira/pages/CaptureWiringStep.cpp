#include "../Icons.h"
#include "../Paint.h"
#include "../Type.h"
#include "CaptureViews.h"

#include <vector>

namespace pedaleira::ui {

using capture::Wiring;

namespace {

constexpr int kDiagramWidth = 1288;
constexpr int kDiagramHeight = 300;
const juce::Colour kOrange{0xfff08a4b};

// ####################
// THE DIAGRAM
// ####################
// The boards' wiring diagrams, in their own 1288 x 300 space: boxes, 3 px
// cables in the colours of the cables (iris send, orange speaker, green
// return, white guitar), a dot at each jack and the cable labels.
struct Cable {
  juce::Rectangle<int> area;
  juce::Colour colour;
  bool dashed = false;
};
struct Dot {
  int x = 0, y = 0;
  juce::Colour colour;
};
struct Text {
  juce::String text;
  juce::Rectangle<int> area;
  float size = 11.0f;
  type::Weight weight = type::Weight::regular;
  bool mono = false;
  juce::Colour colour = pedaleira::ui::colour::muted;
  juce::Justification justification = juce::Justification::centredLeft;
};
struct Box {
  juce::Rectangle<int> area;
  std::vector<Text> texts;
};
struct Label {
  juce::String text;
  int x = 0, y = 0, width = 0;
  juce::Justification justification = juce::Justification::centred;
  juce::Colour colour = pedaleira::ui::colour::text;
};
struct Diagram {
  std::vector<Cable> cables;
  std::vector<Box> boxes;
  std::vector<Dot> dots;
  std::vector<Label> labels;
};

// A device box of the chain: its name, the jacks either side and a line under.
Box deviceBox(juce::Rectangle<int> area, const juce::String& title, const juce::String& leftPort, const juce::String& rightPort,
              const juce::String& note, juce::Colour noteColour, int noteTop) {
  Box box;
  box.area = area;
  const int width = area.getWidth();
  box.texts.push_back({title, {12, 8, width - 24, 18}, 14.0f, type::Weight::semibold, false, colour::text});
  if (leftPort.isNotEmpty())
    box.texts.push_back({leftPort, {12, 37, width / 2, 16}, 11.0f});
  if (rightPort.isNotEmpty())
    box.texts.push_back({rightPort, {width / 2, 37, width / 2 - 12, 16}, 11.0f, type::Weight::regular, false, colour::muted,
                         juce::Justification::centredRight});
  if (note.isNotEmpty())
    box.texts.push_back({note, {12, noteTop, width - 24, 28}, 11.0f, type::Weight::regular, false, noteColour, juce::Justification::centred});
  return box;
}

void addDot(Diagram& d, int x, int y, juce::Colour c) { d.dots.push_back({x - 5, y - 4, c}); }

struct Names {
  juce::String send, ret, dry, wet;
};

Diagram buildDiagram(Wiring wiring, const juce::String& source, const Names& names) {
  Diagram d;
  const bool amp = source == "ampHead";
  const bool cab = source == "ampCab";
  const bool hasAux = amp || cab;
  const juce::String deviceName = source == "pedal" ? "Pedal" : cab ? "Amp + cab" : source == "modeller" ? "Modeller" : "Amp head";
  const juce::String deviceRight = source == "pedal" ? "Output" : source == "modeller" ? "Line out" : cab ? juce::String() : "Speaker out";
  const juce::Colour warnColour = colour::warn;

  const auto auxBox = [&](juce::Rectangle<int> area, bool withLineOut) {
    if (amp)
      return deviceBox(area, "Load box", "Speaker in", withLineOut ? "Line out" : "", "Never run a tube amp without a load",
                       warnColour, area.getHeight() > 90 ? 58 : 58);
    return deviceBox(area, "Microphone", "Sound", withLineOut ? "XLR out" : "", "On the speaker, in a quiet room", colour::muted, 58);
  };

  if (wiring == Wiring::dryWet) {
    // The interface on the right takes both inputs.
    Box interfaceBox;
    interfaceBox.area = {1060, 10, 228, 280};
    interfaceBox.texts.push_back({"Audio interface", {16, 14, 196, 20}, 15.0f, type::Weight::semibold, false, colour::text});
    interfaceBox.texts.push_back({"48 kHz", {16, 34, 196, 16}, 11.0f, type::Weight::regular, true});
    interfaceBox.texts.push_back({names.dry + " " + type::dot() + " Hi-Z (dry)", {16, 83, 196, 16}, 12.0f, type::Weight::semibold, false,
                                  colour::text});
    interfaceBox.texts.push_back({names.wet + " " + type::dot() + " Line (wet)", {16, 222, 196, 16}, 12.0f, type::Weight::semibold, false,
                                  colour::text});
    interfaceBox.texts.push_back({"Both inputs record at once", {16, 256, 196, 16}, 12.0f});
    d.boxes.push_back(std::move(interfaceBox));

    Box guitar = deviceBox({0, 55, 150, 90}, "Guitar", "Output jack", "", "Plays into the splitter", colour::muted, 62);
    d.boxes.push_back(std::move(guitar));
    Box splitter;
    splitter.area = {220, 50, 260, 100};
    splitter.texts.push_back({"Splitter", {12, 8, 236, 18}, 14.0f, type::Weight::semibold, false, colour::text});
    splitter.texts.push_back({"(DI box or a pedal with two outputs)", {12, 28, 236, 16}, 11.0f});
    splitter.texts.push_back({"In", {12, 56, 60, 16}, 11.0f});
    splitter.texts.push_back({"Out A " + type::dot() + " dry", {148, 56, 100, 16}, 11.0f, type::Weight::regular, false, colour::muted,
                              juce::Justification::centredRight});
    splitter.texts.push_back({"Out B " + type::dot() + " to the device", {12, 80, 236, 14}, 11.0f, type::Weight::regular, false, colour::muted,
                              juce::Justification::centred});
    d.boxes.push_back(std::move(splitter));
    d.boxes.push_back(deviceBox({500, 195, 200, 90}, deviceName, "Input", deviceRight, "Device being captured", colour::muted, 62));

    d.cables.push_back({{150, 99, 70, 3}, colour::text});
    d.cables.push_back({{480, 99, 580, 3}, colour::iris});
    d.cables.push_back({{349, 150, 3, 92}, kOrange});
    d.cables.push_back({{349, 239, 151, 3}, kOrange});
    addDot(d, 150, 99, colour::text);
    addDot(d, 220, 99, colour::text);
    addDot(d, 480, 99, colour::iris);
    addDot(d, 1060, 99, colour::iris);
    addDot(d, 500, 239, kOrange);
    d.dots.push_back({345, 145, kOrange});
    d.labels.push_back({"Guitar cable", 148, 78, 74});
    d.labels.push_back({"Dry: instrument cable to " + names.dry, 480, 76, 580});
    d.labels.push_back({"Instrument cable to the device input", 100, 196, 240, juce::Justification::centredRight});
    if (hasAux) {
      d.boxes.push_back(auxBox({820, 195, 180, 90}, true));
      d.cables.push_back({{700, 239, 120, 3}, amp ? kOrange : colour::faint});
      d.cables.push_back({{1000, 239, 60, 3}, colour::ok});
      addDot(d, 700, 239, amp ? kOrange : colour::faint);
      addDot(d, 820, 239, amp ? kOrange : colour::faint);
      addDot(d, 1000, 239, colour::ok);
      addDot(d, 1060, 239, colour::ok);
      d.labels.push_back({amp ? "Speaker cable" : "Sound", 700, 216, 120});
      d.labels.push_back({"Line out", 1000, 216, 60});
    } else {
      d.cables.push_back({{700, 239, 360, 3}, colour::ok});
      addDot(d, 700, 239, colour::ok);
      addDot(d, 1060, 239, colour::ok);
      d.labels.push_back({"Line out to " + names.wet, 700, 216, 360});
    }
    return d;
  }

  // The re-amp: the interface on the left, the chain along y = 130, the return underneath.
  Box interfaceBox;
  interfaceBox.area = {0, 70, 300, 200};
  interfaceBox.texts.push_back({"Audio interface", {16, 14, 200, 20}, 15.0f, type::Weight::semibold, false, colour::text});
  interfaceBox.texts.push_back({"48 kHz", {16, 34, 200, 16}, 11.0f, type::Weight::regular, true});
  interfaceBox.texts.push_back({names.send, {100, 53, 184, 16}, 12.0f, type::Weight::semibold, false, colour::text,
                                juce::Justification::centredRight});
  interfaceBox.texts.push_back({names.ret + " " + type::dot() + " Line", {100, 138, 184, 16}, 12.0f, type::Weight::semibold, false,
                                colour::text, juce::Justification::centredRight});
  interfaceBox.texts.push_back({"Keep a Hi-Z input free for your guitar", {16, 170, 268, 16}, 12.0f});
  d.boxes.push_back(std::move(interfaceBox));

  int label = 1;
  const auto number = [&label] { return juce::String(label++) + " " + type::dot() + " "; };
  const bool box = wiring == Wiring::reampBox;
  if (box) {
    d.boxes.push_back(deviceBox({420, 86, 180, 90}, "Re-amp box", "In", "Out", "Line level in, instrument level out", colour::muted, 58));
    d.cables.push_back({{300, 130, 120, 3}, colour::iris});
    d.cables.push_back({{600, 130, 120, 3}, colour::iris});
    addDot(d, 300, 130, colour::iris);
    addDot(d, 420, 130, colour::iris);
    addDot(d, 600, 130, colour::iris);
    addDot(d, 720, 130, colour::iris);
    d.labels.push_back({number() + "Send (TRS)", 300, 108, 120});
    d.labels.push_back({number() + "Instrument cable", 600, 108, 120});
  } else {
    d.cables.push_back({{300, 130, 420, 3}, colour::iris});
    addDot(d, 300, 130, colour::iris);
    addDot(d, 720, 130, colour::iris);
    d.labels.push_back({number() + "Send: one mono instrument cable, as short as you can", 300, 108, 420});
    d.labels.push_back({"Send starts at -30 dB", 300, 140, 420, juce::Justification::centred, colour::warn});
  }
  d.boxes.push_back(deviceBox({720, 86, 220, 90}, deviceName, "Input", deviceRight, "Device being captured", colour::muted, 62));

  int tapX = 829;
  if (hasAux) {
    d.boxes.push_back(auxBox({1060, 86, 200, 90}, false));
    d.cables.push_back({{940, 130, 120, 3}, amp ? kOrange : colour::faint});
    addDot(d, 940, 130, amp ? kOrange : colour::faint);
    addDot(d, 1060, 130, amp ? kOrange : colour::faint);
    d.labels.push_back({number() + (amp ? "Speaker cable" : "Sound"), 940, 108, 120});
    tapX = 1159;
  }
  d.cables.push_back({{tapX, 176, 3, 41}, colour::ok});
  d.cables.push_back({{300, 214, tapX + 3 - 300, 3}, colour::ok});
  addDot(d, 300, 214, colour::ok);
  d.dots.push_back({tapX - 4, 171, colour::ok});
  const int middle = (300 + tapX + 3) / 2;
  d.labels.push_back({number() + (cab ? "Return (XLR)" : "Return (TRS/XLR)"), middle - 120, 192, 240});
  if (amp)
    d.labels.push_back({"DI / line out", tapX + 13, 184, 100, juce::Justification::centredLeft, colour::muted});
  return d;
}

void paintDiagram(juce::Graphics& g, const Diagram& d) {
  for (const auto& cable : d.cables) {
    g.setColour(cable.colour);
    g.fillRoundedRectangle(cable.area.toFloat(), 1.0f);
  }
  for (const auto& box : d.boxes) {
    paint::raised(g, box.area.toFloat(), 12.0f, colour::surface);
    g.setColour(colour::line);
    g.drawRoundedRectangle(box.area.toFloat().reduced(0.5f), 12.0f, 1.0f);
    for (const auto& text : box.texts) {
      g.setFont(text.mono ? type::mono(text.size, type::Weight::regular) : type::sans(text.size, text.weight));
      g.setColour(text.colour);
      const auto area = text.area.translated(box.area.getX(), box.area.getY());
      if (text.area.getHeight() > 20) {
        // A note under the jacks: wraps to two lines.
        juce::AttributedString string;
        string.append(text.text, type::sans(text.size, text.weight), text.colour);
        string.setJustification(juce::Justification::centredTop);
        string.draw(g, area.toFloat());
      } else {
        g.drawText(text.text, area, text.justification, true);
      }
    }
  }
  for (const auto& dot : d.dots) {
    const auto area = juce::Rectangle<float>(static_cast<float>(dot.x), static_cast<float>(dot.y), 11.0f, 11.0f);
    g.setColour(colour::ground);
    g.fillEllipse(area);
    g.setColour(dot.colour);
    g.drawEllipse(area.reduced(1.0f), 2.0f);
  }
  g.setFont(type::sans(11.0f, type::Weight::semibold));
  for (const auto& label : d.labels) {
    g.setColour(label.colour);
    g.drawText(label.text, label.x, label.y, label.width, 14, label.justification, false);
  }
}

}  // namespace

// ####################
// THE STEP
// ####################
struct CaptureWiringStep::Layout {
  juce::Rectangle<int> variantBox, diagram;
  std::array<juce::Rectangle<int>, 3> cards;
};

CaptureWiringStep::CaptureWiringStep(CaptureHost& hostPage) : CaptureStep(hostPage) {
  setTitle("Wiring");
  variant.setTitle("Re-amp box");
  variant.onChange = [this](int index) { chooseVariant(index); };
  addAndMakeVisible(variant);
  addAndMakeVisible(checklist);
  checklist.onChange = [this] { repaint(); };
  addAndMakeVisible(firstChannel);
  addAndMakeVisible(secondChannel);
  parts::styleEditor(maxLevel);
  maxLevel.setTitle("Interface maximum input level in dBu");
  maxLevel.setJustification(juce::Justification::centredRight);
  maxLevel.setInputRestrictions(7, "-0123456789.");
  maxLevel.onReturnKey = [this] { sendChannels(object({{"calibrationDbu", maxLevel.getText().getDoubleValue()}})); };
  maxLevel.onFocusLost = [this] {
    if (std::abs(maxLevel.getText().getDoubleValue() - static_cast<double>(session["calibration"]["returnDbu"])) > 0.001)
      sendChannels(object({{"calibrationDbu", maxLevel.getText().getDoubleValue()}}));
  };
  addAndMakeVisible(maxLevel);
  testClick.setIcon(&icons::path(icons::Icon::play));
  testClick.setEnabled(false);
  measureGuitar.setEnabled(false);
  addAndMakeVisible(testClick);
  addAndMakeVisible(measureGuitar);

  firstChannel.onClick = [this] {
    const bool dryWet = current == Wiring::dryWet;
    juce::StringArray names;
    for (int i = 0; i < (dryWet ? deviceInputs : deviceOutputs); ++i)
      names.add(channelName(!dryWet, i));
    const int selected = static_cast<int>(session["channels"][dryWet ? "dry" : "send"]);
    parts::chooseFrom(firstChannel, names, selected, [this, dryWet](int i) {
      sendChannels(object({{dryWet ? "dry" : "send", i}}));
    });
  };
  secondChannel.onClick = [this] {
    const bool dryWet = current == Wiring::dryWet;
    juce::StringArray names;
    for (int i = 0; i < deviceInputs; ++i)
      names.add(channelName(false, i));
    const int selected = static_cast<int>(session["channels"][dryWet ? "wet" : "returnIn"]);
    parts::chooseFrom(secondChannel, names, selected, [this, dryWet](int i) {
      sendChannels(object({{dryWet ? "wet" : "returnIn", i}}));
    });
  };
}

juce::String CaptureWiringStep::channelName(bool output, int index) const {
  return juce::String(output ? "Output " : "Input ") + juce::String(index + 1);
}

juce::String CaptureWiringStep::subtitle() const {
  const juce::String source = session["source"].toString();
  if (current == Wiring::dryWet)
    return "No re-amp box needed. You play, Pedaleira records your dry guitar and the device at the same time.";
  juce::String what = "Amp head through a load box.";
  if (source == "pedal")
    what = "The pedal at line level.";
  else if (source == "ampCab")
    what = "Amp and cab, with a microphone on the speaker.";
  else if (source == "modeller")
    what = "The modeller's line output back into the interface.";
  if (current == Wiring::noBox)
    return what + " Straight from the interface. We guide you to a safe send level.";
  return what + " Follow the cable colours. A re-amp box is recommended, not required.";
}

void CaptureWiringStep::rebuildChecklist() {
  const juce::String source = session["source"].toString();
  const juce::String send = channelName(true, static_cast<int>(session["channels"]["send"]));
  const juce::String back = channelName(false, static_cast<int>(session["channels"][current == Wiring::dryWet ? "dry" : "returnIn"]));
  const juce::String wet = channelName(false, static_cast<int>(session["channels"]["wet"]));
  const bool amp = source == "ampHead";
  const bool cab = source == "ampCab";
  juce::StringArray items;
  if (current == Wiring::dryWet) {
    items.add("Split the guitar into two identical signals.");
    items.add("Dry side to " + back + " (Hi-Z).");
    items.add(amp ? "Amp through the load box, load box line out to " + wet + "."
                  : cab ? "Amp to the cab, microphone to " + wet + "." : "The device's output to " + wet + ".");
    items.add("Set both inputs to peak around -8 dB.");
    items.add("Play for 2 to 3 minutes.");
  } else {
    const juce::String into = amp ? "the amp input" : cab ? "the amp input" : source == "pedal" ? "the pedal input" : "the modeller input";
    if (current == Wiring::noBox) {
      items.add((amp || cab ? juce::String("Turn the amp volume down.") : juce::String("Keep the device's output low.")) +
                " Pedaleira has set the send to -30 dB.");
      items.add(send + " to " + into + " with one mono instrument cable.");
    } else {
      items.add(send + " to the re-amp box (TRS).");
      items.add("Re-amp box to " + into + " (instrument cable).");
    }
    if (amp)
      items.add("Amp speaker out to the load box (speaker cable).");
    if (cab)
      items.add("Microphone on the speaker, in a quiet room.");
    items.add(amp ? "Load box line out to " + back + " (TRS or XLR)."
                  : cab ? "Microphone to " + back + " (XLR)." : "The device's output to " + back + " (TRS or instrument cable).");
    if (current == Wiring::noBox)
      items.add("Set the send level in the next step.");
  }
  checklist.setItems(items);
}

void CaptureWiringStep::show(const juce::var& newSession) {
  session = newSession;
  if (id().isEmpty())
    return;
  current = session["method"].toString() == "dryWet" ? Wiring::dryWet : hasBox ? Wiring::reampBox : Wiring::noBox;
  variant.setSelected(current == Wiring::reampBox ? 0 : current == Wiring::noBox ? 1 : 2);
  // The count of device channels comes with every answer of capture.setChannels.
  if (session["deviceInputs"].isInt()) {
    channelsKnown = true;
    deviceInputs = static_cast<int>(session["deviceInputs"]);
    deviceOutputs = static_cast<int>(session["deviceOutputs"]);
    warnings.clear();
    if (const auto* list = session["warnings"].getArray())
      for (const auto& warning : *list)
        warnings.add(warning.toString());
  } else if (!channelsKnown) {
    const EngineReply reply = host.run("capture.setChannels", object({{"id", id()}}), {});
    if (reply.ok && reply.value["deviceInputs"].isInt()) {
      channelsKnown = true;
      deviceInputs = static_cast<int>(reply.value["deviceInputs"]);
      deviceOutputs = static_cast<int>(reply.value["deviceOutputs"]);
      warnings.clear();
      if (const auto* list = reply.value["warnings"].getArray())
        for (const auto& warning : *list)
          warnings.add(warning.toString());
    }
  }
  const bool dryWet = current == Wiring::dryWet;
  firstChannel.setTitle(dryWet ? "Dry input" : "Send output");
  secondChannel.setTitle(dryWet ? "Wet input" : "Return input");
  firstChannel.setChoice(channelName(!dryWet, static_cast<int>(session["channels"][dryWet ? "dry" : "send"])));
  secondChannel.setChoice(channelName(false, static_cast<int>(session["channels"][dryWet ? "wet" : "returnIn"])));
  if (!maxLevel.hasKeyboardFocus(true))
    maxLevel.setText(juce::String(static_cast<double>(session["calibration"]["returnDbu"]), 1), false);
  rebuildChecklist();
  testClick.setVisible(!dryWet);
  measureGuitar.setVisible(current == Wiring::noBox);
  maxLevel.setVisible(current == Wiring::reampBox);
  resized();
  repaint();
}

void CaptureWiringStep::chooseVariant(int index) {
  if (id().isEmpty())
    return;
  const bool wasDryWet = current == Wiring::dryWet;
  const Wiring next = index == 0 ? Wiring::reampBox : index == 1 ? Wiring::noBox : Wiring::dryWet;
  if (next == current)
    return;
  if ((next == Wiring::dryWet) != wasDryWet) {
    const EngineReply reply = host.run("capture.setSource", object({{"id", id()}, {"method", next == Wiring::dryWet ? "dryWet" : "reamp"}}),
                                       "Could not change the method");
    if (!reply.ok) {
      variant.setSelected(wasDryWet ? 2 : hasBox ? 0 : 1);
      return;
    }
    session = reply.value;
  }
  if (next != Wiring::dryWet) {
    hasBox = next == Wiring::reampBox;
    // Without a box the amp hears the interface's full output: start low.
    const double sendDb = static_cast<double>(session["levels"]["sendDb"]);
    if (next == Wiring::noBox && sendDb > -30.0)
      host.run("capture.setLevels", object({{"id", id()}, {"sendDb", -30.0}}), {});
    else if (next == Wiring::reampBox && sendDb <= -30.0)
      host.run("capture.setLevels", object({{"id", id()}, {"sendDb", -24.0}}), {});
  }
  host.reload();
}

void CaptureWiringStep::sendChannels(const juce::var& args) {
  juce::DynamicObject::Ptr full = new juce::DynamicObject();
  full->setProperty("id", id());
  if (const auto* properties = args.getDynamicObject())
    for (const auto& property : properties->getProperties())
      full->setProperty(property.name, property.value);
  const EngineReply reply = host.run("capture.setChannels", juce::var(full.get()), "Could not set the channels");
  if (reply.ok)
    host.sessionChanged(reply.value);
  else
    host.reload();
}

CaptureStep::Footer CaptureWiringStep::footer() const {
  Footer footer;
  footer.primary = "Continue to levels";
  if (current == Wiring::dryWet && !id().isEmpty())
    footer.primaryEnabled = static_cast<int>(session["channels"]["dry"]) != static_cast<int>(session["channels"]["wet"]);
  return footer;
}

void CaptureWiringStep::primary() { host.moveTo(capture::Step::levels); }

CaptureWiringStep::Layout CaptureWiringStep::layout() const {
  Layout l;
  const int top = parts::kHeadingHeight + parts::kBlockGap;
  l.diagram = {juce::jmax(0, (getWidth() - kDiagramWidth) / 2), top, kDiagramWidth, kDiagramHeight};
  const int cardsTop = top + kDiagramHeight + parts::kBlockGap;
  const int width = (getWidth() - 2 * parts::kBlockGap) / 3;
  for (int i = 0; i < 3; ++i)
    l.cards[static_cast<size_t>(i)] = {i * (width + parts::kBlockGap), cardsTop, width, getHeight() - cardsTop};
  l.variantBox = {getWidth() - variant.idealWidth() - 20, parts::kHeadingHeight - 38, variant.idealWidth() + 20, 38};
  return l;
}

void CaptureWiringStep::resized() {
  const Layout l = layout();
  variant.setBounds(l.variantBox);

  const auto a = l.cards[0].reduced(20, 0);
  checklist.setBounds(a.getX(), l.cards[0].getY() + 16 + 16 + 12, a.getWidth(), checklist.heightFor(a.getWidth()));

  const auto b = l.cards[1];
  // Interface settings: the fifth row is the maximum level.
  maxLevel.setBounds(b.getRight() - 20 - 84, b.getY() + 16 + 16 + 12 + 4 * 28 + 2, 84, 30);
  measureGuitar.setBounds(b.getX() + 20, b.getY() + 16 + 16 + 12 + 60, measureGuitar.idealWidth() + 4, 36);

  const auto c = l.cards[2].reduced(20, 0);
  const int half = (c.getWidth() - 16) / 2;
  const int selectTop = l.cards[2].getY() + 16 + 16 + 12 + 16 + 6;
  firstChannel.setBounds(c.getX(), selectTop, half, 36);
  secondChannel.setBounds(c.getX() + half + 16, selectTop, half, 36);
  testClick.setBounds(c.getX(), selectTop + 36 + 14, testClick.idealWidth() + 4, 36);
}

void CaptureWiringStep::paint(juce::Graphics& g) {
  parts::paintHeading(g, getLocalBounds().withTrimmedRight(variant.idealWidth() + 40), "Wire it up", subtitle());
  if (id().isEmpty())
    return;
  const Layout l = layout();
  {
    juce::Graphics::ScopedSaveState state(g);
    g.addTransform(juce::AffineTransform::translation(static_cast<float>(l.diagram.getX()), static_cast<float>(l.diagram.getY())));
    Names names;
    names.send = channelName(true, static_cast<int>(session["channels"]["send"]));
    names.ret = channelName(false, static_cast<int>(session["channels"]["returnIn"]));
    names.dry = channelName(false, static_cast<int>(session["channels"]["dry"]));
    names.wet = channelName(false, static_cast<int>(session["channels"]["wet"]));
    paintDiagram(g, buildDiagram(current, session["source"].toString(), names));
  }

  // Card 1: the checklist.
  {
    const juce::String label = current == Wiring::reampBox ? "Checklist"
                               : current == Wiring::noBox  ? "Without a re-amp box"
                                                           : "Play it in, " + juce::String(checklist.count()) + " steps";
    parts::paintCard(g, l.cards[0]);
    parts::paintCardHeader(g, l.cards[0], label, juce::String(checklist.doneCount()) + " of " + juce::String(checklist.count()));
  }

  // Card 2.
  const auto b = l.cards[1];
  parts::paintCard(g, b);
  if (current == Wiring::reampBox) {
    parts::paintCardHeader(g, b, "Interface settings");
    const char* const rows[4][2] = {{"Return input", "Line, not Hi-Z"}, {"48V phantom power", "Off"}, {"Pad", "Off"}, {"Direct monitor", "Off"}};
    auto area = juce::Rectangle<int>(b.getX() + 20, b.getY() + 16 + 16 + 12, b.getWidth() - 40, 28);
    for (const auto& row : rows) {
      auto line = area;
      g.setFont(type::compact());
      g.setColour(colour::muted);
      g.drawText(row[0], line.removeFromLeft(line.getWidth() / 2), juce::Justification::centredLeft, true);
      g.setFont(type::mono(13.0f));
      g.setColour(colour::text);
      g.drawText(row[1], line, juce::Justification::centredRight, true);
      area.translate(0, 28);
    }
    g.setFont(type::compact());
    g.setColour(colour::muted);
    g.drawText("Interface max input (dBu)", area.withTrimmedRight(100), juce::Justification::centredLeft, true);
  } else if (current == Wiring::noBox) {
    parts::paintCardHeader(g, b, "Match your guitar's level");
    auto area = juce::Rectangle<int>(b.getX() + 20, b.getY() + 16 + 16 + 12, b.getWidth() - 40, 56);
    const juce::String text =
        "Plug your guitar into a Hi-Z input and play hard for 5 seconds. Pedaleira measures its level and sets the send so the "
        "amp gets the level it would from the guitar.";
    parts::drawWrapped(g, text, type::compact(), colour::muted, area);
    const auto later = juce::Rectangle<int>(measureGuitar.getRight() + 12, measureGuitar.getY() + 7, paint::chipWidth(parts::kLater), 22);
    parts::paintLater(g, later);
    const juce::String hum = "Hum? Keep the send cable away from power supplies. Fuzz pedals and some vintage inputs still "
                             "respond differently without a box.";
    parts::drawWrapped(g, hum, type::sans(12.0f), colour::muted,
                       juce::Rectangle<int>(b.getX() + 20, measureGuitar.getBottom() + 10, b.getWidth() - 40, 48));
  } else {
    parts::paintCardHeader(g, b, "What to play");
    const char* const lines[] = {"Single notes and chords", "Bends and vibrato", "Soft and hard picking", "All over the neck",
                                 "Palm mutes and open strings"};
    auto area = juce::Rectangle<int>(b.getX() + 20, b.getY() + 16 + 16 + 12, b.getWidth() - 40, 24);
    for (const char* line : lines) {
      auto row = area;
      g.setColour(colour::iris);
      g.fillEllipse(row.removeFromLeft(6).toFloat().withSizeKeepingCentre(6.0f, 6.0f));
      row.removeFromLeft(10);
      g.setFont(type::compact());
      g.setColour(colour::text);
      g.drawText(line, row, juce::Justification::centredLeft, true);
      area.translate(0, 24);
    }
    g.setFont(type::mono(12.0f, type::Weight::regular));
    g.setColour(colour::muted);
    g.drawText("Recording target: " + capture::clockText(static_cast<double>(session["recording"]["plannedSeconds"])), area,
               juce::Justification::centredLeft, true);
  }

  // Card 3: routing.
  const auto c = l.cards[2];
  parts::paintCard(g, c);
  parts::paintCardHeader(g, c, "Routing");
  const bool dryWet = current == Wiring::dryWet;
  const int half = (c.getWidth() - 40 - 16) / 2;
  const int labelTop = c.getY() + 16 + 16 + 12;
  paint::label(g, dryWet ? "Dry input" : "Send output", juce::Rectangle<int>(c.getX() + 20, labelTop, half, 16));
  paint::label(g, dryWet ? "Wet input" : "Return input", juce::Rectangle<int>(c.getX() + 20 + half + 16, labelTop, half, 16));
  if (dryWet) {
    const bool same = static_cast<int>(session["channels"]["dry"]) == static_cast<int>(session["channels"]["wet"]);
    juce::String text = "A re-amped sweep is still the most accurate method. Use this when you have no re-amp box.";
    juce::Colour textColour = colour::muted;
    if (same) {
      text = "The dry and wet inputs must be different channels.";
      textColour = colour::warn;
    } else if (warnings.size() > 0) {
      text = warnings.joinIntoString(" ");
      textColour = colour::warn;
    }
    parts::drawWrapped(g, text, type::sans(12.0f), textColour,
                       juce::Rectangle<int>(c.getX() + 20, secondChannel.getBottom() + 14, c.getWidth() - 40, 60));
  } else {
    const auto later = juce::Rectangle<int>(testClick.getRight() + 12, testClick.getY() + 7, paint::chipWidth(parts::kLater), 22);
    parts::paintLater(g, later);
    juce::String text = "Plays one click on " + channelName(true, static_cast<int>(session["channels"]["send"])) + " and listens on " +
                        channelName(false, static_cast<int>(session["channels"]["returnIn"])) + ".";
    juce::Colour textColour = colour::muted;
    if (warnings.size() > 0) {
      text = warnings.joinIntoString(" ");
      textColour = colour::warn;
    }
    parts::drawWrapped(g, text, type::sans(12.0f), textColour,
                       juce::Rectangle<int>(c.getX() + 20, testClick.getBottom() + 8, c.getWidth() - 40, 40));
  }
}

}  // namespace pedaleira::ui

#include "SettingsInfo.h"

#include "../Paint.h"
#include "../Type.h"
#include "EditorParts.h"

namespace pedaleira::ui {

namespace {

constexpr int kRowHeight = 40;
constexpr int kHeadHeight = 22;

// "gateThreshold" as "Gate threshold".
juce::String spaced(const juce::String& id) {
  juce::String out;
  for (int i = 0; i < id.length(); ++i) {
    const juce::juce_wchar c = id[i];
    if (i > 0 && juce::CharacterFunctions::isUpperCase(c))
      out << ' ' << juce::String::charToString(juce::CharacterFunctions::toLowerCase(c));
    else
      out << juce::String::charToString(i == 0 ? juce::CharacterFunctions::toUpperCase(c) : c);
  }
  return out;
}

// What a mapping target is called on the page.
juce::String targetLabel(const juce::String& id) {
  if (id == "stereoEnabled")
    return "Stereo mode";
  if (id == "presetNext")
    return "Next preset";
  if (id == "presetPrevious")
    return "Previous preset";
  if (id.startsWith("rightBlock") && id.endsWith("Power"))
    return "Right lane, block " + id.fromFirstOccurrenceOf("rightBlock", false, false).upToFirstOccurrenceOf("Power", false, false) + " on or off";
  if (id.startsWith("block") && id.endsWith("Power"))
    return "Block " + id.fromFirstOccurrenceOf("block", false, false).upToFirstOccurrenceOf("Power", false, false) + " on or off";
  return spaced(id);
}

}  // namespace

// ####################
// MIDI
// ####################
SettingsMidi::SettingsMidi(EngineClient& client, DeviceSettings* deviceSettings)
    : SettingsSection(client, "MIDI mapping",
                      "Defaults match the CC numbers your foot controller already uses with the QC. Editing the map "
                      "comes in a later build."),
      device(deviceSettings) {}

void SettingsMidi::refresh() {
  const juce::var map = engine.read("midi.getMap");
  table.clear();
  struct Fixed {
    const char* action;
    const char* function;
    const char* values;
  };
  const Fixed fixed[] = {{"sceneSelect", "Scenes A-H", "0-7"},
                         {"tapTempo", "Tap tempo", "any"},
                         {"tuner", "Tuner", "toggle"},
                         {"footswitchMode", "Footswitch mode", "0 Preset, 1 Scene, 2 Stomp, 3 Hybrid"}};
  if (const auto* defaults = map["defaults"].getArray())
    for (const auto& entry : *defaults)
      for (const auto& f : fixed)
        if (entry["action"].toString() == f.action) {
          Row row;
          row.function = f.function;
          row.message = "CC " + juce::String(static_cast<int>(entry["cc"]));
          row.values = f.values;
          row.replaced = static_cast<bool>(entry["overridden"]);
          table.push_back(row);
        }
  if (const auto* mappings = map["mappings"].getArray())
    for (const auto& entry : *mappings) {
      Row row;
      row.function = targetLabel(entry["targetId"].toString());
      row.message = (entry["source"].toString() == "note" ? "Note " : "CC ") + juce::String(static_cast<int>(entry["number"]));
      row.values = entry["source"].toString() == "note" ? "press" : "0-127";
      table.push_back(row);
    }
  const int channel = map["channel"];
  channelText = channel == 0 ? juce::String("All channels") : "Channel " + juce::String(channel);

  inputs.clear();
  inputToggles.clear();
  // Kept in a local: the list must outlive the loop over it.
  const juce::var deviceState = device != nullptr ? device->state() : juce::var();
  if (const auto* list = deviceState["midiInputs"].getArray())
    for (const auto& input : *list) {
      const juce::String id = input["id"].toString();
      auto toggle = std::make_unique<SettingToggle>(input["name"].toString());
      toggle->setState(static_cast<bool>(input["enabled"]));
      toggle->onToggle = [this, id](bool on) { device->setMidiInputEnabled(id, on); };
      addAndMakeVisible(*toggle);
      inputToggles.push_back(std::move(toggle));
      inputs.emplace_back(id, input["name"].toString());
    }
  resized();
  heightChanged();
  repaint();
}

juce::StringArray SettingsMidi::tableRows() const {
  juce::StringArray out;
  for (const auto& row : table)
    out.add(row.function + " | " + row.message + " | " + row.values);
  return out;
}

int SettingsMidi::heightFor(int width) const { return layoutFor(width).height; }

SettingsMidi::Layout SettingsMidi::layoutFor(int width) const {
  using namespace settings;
  Layout l;
  // The board's columns: 640 and the rest.
  const int leftWidth = juce::jmin(640, (width - kCardGap) * 12 / 21);
  const int rightWidth = width - kCardGap - leftWidth;
  const int rightX = leftWidth + kCardGap;
  const int pad = kCardPadding;

  {
    int cy = kCardTop;
    l.head = {pad, cy, leftWidth - 2 * pad, kHeadHeight};
    cy += kHeadHeight;
    for (size_t i = 0; i < table.size(); ++i) {
      l.rows.push_back({pad, cy, leftWidth - 2 * pad, kRowHeight});
      cy += kRowHeight;
    }
    cy += 12;
    const juce::String text =
        "Learn, Reset to defaults, the clock and outgoing messages come in a later build. The mapping rows above are what "
        "the engine holds now.";
    l.note = {pad, cy, leftWidth - 2 * pad, noteHeight(leftWidth - 2 * pad, text, 12.0f)};
    cy += l.note.getHeight();
    l.mapping = {0, 0, leftWidth, cy + kCardBottom};
  }
  int y = 0;
  {
    const int cw = rightWidth - 2 * pad;
    int cy = kCardTop;
    l.channel = {rightX + pad, cy, cw, 24};
    cy += 24 + 10;
    if (!inputs.empty()) {
      l.inputsLabel = {rightX + pad, cy, cw, 16};
      cy += 22;
    }
    for (size_t i = 0; i < inputs.size(); ++i) {
      l.inputRows.push_back({rightX + pad, cy, cw, 40});
      cy += 40;
    }
    const juce::String text = device == nullptr ? "The MIDI inputs are chosen in the standalone application's window."
                                                : inputs.empty() ? "No MIDI input is connected." : "";
    if (text.isNotEmpty()) {
      l.inputsNote = {rightX + pad, cy, cw, noteHeight(cw, text)};
      cy += l.inputsNote.getHeight();
    }
    l.devices = {rightX, y, rightWidth, cy + kCardBottom};
    y = l.devices.getBottom() + kCardGap;
  }
  {
    l.outgoing = {rightX, y, rightWidth, kCardTop + 72 + kCardBottom};
    l.outgoingBox = cardContent(l.outgoing).withHeight(72);
    y = l.outgoing.getBottom();
  }
  l.height = juce::jmax(l.mapping.getBottom(), y);
  return l;
}

void SettingsMidi::resized() {
  lay = layoutFor(getWidth());
  for (size_t i = 0; i < inputToggles.size() && i < lay.inputRows.size(); ++i)
    inputToggles[i]->setBounds(lay.inputRows[i]);
}

void SettingsMidi::paint(juce::Graphics& g) {
  using namespace settings;
  paintCard(g, lay.mapping, "Global mapping");
  {
    auto head = lay.head;
    paint::label(g, "Function", head.removeFromLeft(150));
    paint::label(g, "Message", head.removeFromLeft(112));
    paint::label(g, "Values", head);
  }
  for (size_t i = 0; i < table.size() && i < lay.rows.size(); ++i) {
    auto row = lay.rows[i];
    paintRowLine(g, row);
    g.setFont(type::sans(14.0f, type::Weight::medium));
    g.setColour(colour::text);
    g.drawText(table[i].function, row.removeFromLeft(150), juce::Justification::centredLeft, true);
    g.setFont(type::mono(13.0f));
    g.drawText(table[i].message, row.removeFromLeft(112), juce::Justification::centredLeft, true);
    g.setFont(type::mono(12.0f, type::Weight::regular));
    g.setColour(table[i].replaced ? colour::warn : colour::muted);
    g.drawText(table[i].replaced ? juce::String("Replaced by a mapping below") : table[i].values, row,
               juce::Justification::centredLeft, true);
  }
  paintNote(g, lay.note,
            "Learn, Reset to defaults, the clock and outgoing messages come in a later build. The mapping rows above are "
            "what the engine holds now.",
            12.0f);

  paintCard(g, lay.devices, "Devices");
  paint::label(g, "Channel", lay.channel.withWidth(80));
  g.setFont(type::mono(13.0f));
  g.setColour(colour::text);
  g.drawText(channelText, lay.channel.withTrimmedLeft(92), juce::Justification::centredLeft, false);
  if (device == nullptr || inputs.empty())
    paintNote(g, lay.inputsNote, device == nullptr ? "The MIDI inputs are chosen in the standalone application's window." : "No MIDI input is connected.");
  else
    paint::label(g, "MIDI inputs", lay.inputsLabel);

  paintCard(g, lay.outgoing, "Outgoing MIDI");
  paintLater(g, lay.outgoingBox, "Up to 12 messages per footswitch and per preset load come in a later build.");
}

// ####################
// REMOTE
// ####################
SettingsRemote::SettingsRemote(EngineClient& client)
    : SettingsSection(client, "Remote",
                      "Your phone as a Gig View and a bank of footswitches.") {}

int SettingsRemote::heightFor(int) const { return 168; }

void SettingsRemote::paint(juce::Graphics& g) {
  using namespace settings;
  const auto box = juce::Rectangle<int>(0, 0, getWidth(), heightFor(getWidth()));
  paintCard(g, box, "Phone remote");
  paintLater(g, cardContent(box).withHeight(72), "Comes in a later build");
}

// ####################
// ABOUT
// ####################
SettingsAbout::SettingsAbout(EngineClient& client)
    : SettingsSection(client, "About", "What Pedaleira is, what it is made of and under which terms.") {}

void SettingsAbout::refresh() {
  versionText = engine.read("state.get")["engine"]["version"].toString();
  repaint();
}

std::vector<SettingsAbout::Block> SettingsAbout::blocks() const {
  return {
      {"Pedaleira",
       "A guitar rig for the stage and the studio: a grid of signal paths, scenes and stomps, NAM captures and impulse "
       "responses, and the plugins you already own, driven from the app, a footswitch or an agent."},
      {"Links",
       "Source code: github.com/TheIcarusWings/pedaleira\n"
       "TONE3000 (captures and IRs): www.tone3000.com\n"
       "Neural Amp Modeler: www.neuralampmodeler.com"},
      {"Licences",
       "Pedaleira's code is under the MIT License. The copyright line is the upstream project's (TONE3000) and stays with "
       "every copy.\n\n"
       "It is built with JUCE, used under the GNU Affero General Public License v3 (AGPL-3.0). The terms of a binary "
       "release are still to be settled.\n\n"
       "The typefaces, Hanken Grotesk and JetBrains Mono, are under the SIL Open Font License 1.1."},
      {"Credits",
       "Pedaleira is a fork of TONE3000's open source plugin (tone-3000/tone3000-plugin), which gave it the TONE3000 "
       "integration and most of the audio engine.\n\n"
       "Amp models run on NeuralAmpModelerCore, by Steven Atkinson and the Neural Amp Modeler project (MIT). Captures and "
       "IRs come from the TONE3000 community."},
  };
}

int SettingsAbout::heightFor(int width) const { return layoutFor(width).height; }

SettingsAbout::Layout SettingsAbout::layoutFor(int width) const {
  using namespace settings;
  Layout l;
  const int columnWidth = (width - kCardGap) / 2;
  const auto all = blocks();
  int leftY = 0, rightY = 0;
  for (size_t i = 0; i < all.size(); ++i) {
    const bool left = i < 2;
    int& y = left ? leftY : rightY;
    const int x = left ? 0 : columnWidth + kCardGap;
    const int cw = columnWidth - 2 * kCardPadding;
    // The first card carries the name and version above its text.
    const int lead = i == 0 ? 34 : 0;
    const int text = noteHeight(cw, all[i].text);
    l.cards.push_back({x, y, columnWidth, kCardTop + lead + text + kCardBottom});
    l.texts.push_back({x + kCardPadding, y + kCardTop + lead, cw, text});
    y += l.cards.back().getHeight() + kCardGap;
  }
  l.height = juce::jmax(leftY, rightY) - kCardGap;
  return l;
}

void SettingsAbout::paint(juce::Graphics& g) {
  using namespace settings;
  const Layout l = layoutFor(getWidth());
  const auto all = blocks();
  for (size_t i = 0; i < all.size(); ++i) {
    paintCard(g, l.cards[i], all[i].heading);
    if (i == 0) {
      g.setFont(type::mono(13.0f));
      g.setColour(colour::muted);
      g.drawText(versionText.isEmpty() ? juce::String("Version unknown") : "Version " + versionText,
                 l.texts[i].translated(0, -34).withHeight(22), juce::Justification::centredLeft, false);
    }
    paintNote(g, l.texts[i], all[i].text);
  }
}

}  // namespace pedaleira::ui

#include "SettingsAgents.h"

#include "../Paint.h"
#include "../Type.h"
#include "EditorParts.h"

namespace pedaleira::ui {

namespace {

constexpr int kRowHeight = 44;
constexpr int kAgentRow = 44;
constexpr int kGuitarRow = 60;
constexpr int kHeadHeight = 22;

struct PermissionRow {
  const char* key;
  const char* label;
  const char* note;
};

// The six switches of permissions.get, in the board's order and words.
const PermissionRow kPermissions[] = {
    {"read", "Read library and presets", ""},
    {"edit", "Create and edit presets", ""},
    {"render", "Render audio clips to measure a tone", ""},
    {"live", "Change the preset you are playing live", "Off by default so nothing changes under your fingers"},
    {"delete", "Delete presets and guitar profiles", ""},
    {"settings", "Change audio and device settings", ""},
};

juce::String pickupLabel(const juce::String& type) {
  if (type == "humbucker")
    return "Humbuckers";
  if (type == "single-coil")
    return "Single coils";
  if (type == "p90")
    return "P-90s";
  if (type == "active")
    return "Active pickups";
  if (type == "piezo")
    return "Piezo";
  return "Pickups";
}

juce::String plural(int n, const char* one, const char* many) { return juce::String(n) + " " + (n == 1 ? one : many); }

}  // namespace

// ####################
// SETUP
// ####################
SettingsAgents::SettingsAgents(EngineClient& client)
    : SettingsSection(client, "AI and MCP",
                      "Let an AI agent control Pedaleira through MCP: browse blocks, build presets and listen to the "
                      "result.") {
  enableServer.onToggle = [this](bool on) { setServerEnabled(on); };
  addAndMakeVisible(enableServer);

  settings::styleEditor(endpoint, true);
  endpoint.setTitle("Connection");
  endpoint.setReadOnly(true);
  endpoint.setTextToShowWhenEmpty("The server is off", colour::faint);
  addAndMakeVisible(endpoint);
  copyEndpoint.onClick = [this] { copyToClipboard(endpointText(), copyEndpoint); };
  copySetup.onClick = [this] { copyToClipboard(setupText(), copySetup); };
  addAndMakeVisible(copyEndpoint);
  addAndMakeVisible(copySetup);

  for (const auto& row : kPermissions) {
    permissionKeys.add(row.key);
    auto toggle = std::make_unique<SettingToggle>(row.label, row.note);
    const juce::String key = row.key;
    toggle->onToggle = [this, key](bool on) { setPermission(key, on); };
    addAndMakeVisible(*toggle);
    permissionToggles.push_back(std::move(toggle));
  }

  // The guitar wizard is UI step 9.
  addGuitar.setEnabled(false);
  addAndMakeVisible(addGuitar);
}

// ####################
// DATA
// ####################
void SettingsAgents::readServer() {
  const juce::var listed = engine.read("agents.list");
  server = listed["server"];
  clients.clear();
  if (const auto* list = listed["clients"].getArray())
    for (const auto& item : *list) {
      Client c;
      c.id = item["id"];
      c.name = item["name"].toString();
      c.connected = item["connected"].toString();
      c.lastCommand = item["lastCommand"].toString();
      c.lastTime = item["lastCommandTime"].toString();
      clients.push_back(c);
    }
  enableServer.setState(static_cast<bool>(server["enabled"]));
  endpoint.setText(endpointText(), false);
  copyEndpoint.setEnabled(running());
  copySetup.setEnabled(server["connectionFile"].toString().isNotEmpty());
}

void SettingsAgents::refresh() {
  readServer();

  const juce::var permissions = engine.read("permissions.get");
  for (size_t i = 0; i < permissionToggles.size(); ++i)
    permissionToggles[i]->setState(static_cast<bool>(permissions[juce::Identifier(permissionKeys[static_cast<int>(i)])]));

  // Walking the library takes a moment; the counts are read when the page opens.
  tags = engine.read("assets.tagSummary");

  guitars.clear();
  const juce::var listed = engine.read("guitar.list");
  if (const auto* list = listed["guitars"].getArray())
    for (const auto& item : *list) {
      Guitar g;
      g.name = item["name"].toString();
      const int positions = item["positions"].size();
      const int takes = item["takeCount"];
      g.line = pickupLabel(item["pickupType"].toString()) + " " + type::dot() + " " + plural(positions, "position", "positions") +
               ", " + plural(takes, "take", "takes");
      if (static_cast<bool>(item["isDefault"]))
        g.line << " " << type::dot() << " Default";
      g.ready = takes > 0 && takes >= positions;
      const juce::var summary = item["summary"];
      if (takes > 0 && !summary["peakDb"].isVoid())
        g.summary = "peak " + juce::String(static_cast<double>(summary["peakDb"]), 1) + " dBFS " + type::dot() + " " +
                    summary["brightness"].toString();
      else
        g.summary = "No takes yet";
      guitars.push_back(g);
    }
  rebuildClients();
  resized();
  heightChanged();
  repaint();
}

void SettingsAgents::rebuildClients() {
  disconnectButtons.clear();
  for (const auto& c : clients) {
    auto button = std::make_unique<PushButton>("Disconnect");
    button->setTitle("Disconnect " + c.name);
    const int id = c.id;
    // The list is rebuilt, button included: after the click returns.
    button->onClick = [this, id] { settings::afterClick(*this, [this, id] { disconnect(id); }); };
    addAndMakeVisible(*button);
    disconnectButtons.push_back(std::move(button));
  }
}

void SettingsAgents::tick() {
  if (copiedButton != nullptr && juce::Time::getMillisecondCounter() - copiedAt > 1500) {
    copiedButton->setButtonText(copiedButton == &copyEndpoint ? "Copy" : "Copy setup for my agent");
    copiedButton = nullptr;
    resized();
  }
  // Agents come and go: look once a second, only while the server is up.
  if (++ticks % 4 != 0 || !running())
    return;
  const size_t before = clients.size();
  const juce::String names = [this] {
    juce::String all;
    for (const auto& c : clients)
      all << c.id << c.lastCommand << c.lastTime << "|";
    return all;
  }();
  readServer();
  juce::String now;
  for (const auto& c : clients)
    now << c.id << c.lastCommand << c.lastTime << "|";
  if (clients.size() != before)
    rebuildClients();
  if (clients.size() != before || now != names) {
    resized();
    heightChanged();
    repaint();
  }
}

void SettingsAgents::setServerEnabled(bool enabled) {
  run("Could not change the server", "server.setEnabled", object({{"enabled", enabled}}));
  refresh();
}

void SettingsAgents::setPermission(const juce::String& key, bool allowed) {
  run("Could not change the permission", "permissions.set", object({{key.toRawUTF8(), allowed}}));
  refresh();
}

void SettingsAgents::disconnect(int clientId) {
  run("Could not disconnect", "agents.disconnect", object({{"id", clientId}}));
  readServer();
  rebuildClients();
  resized();
  heightChanged();
  repaint();
}

juce::String SettingsAgents::endpointText() const {
  return running() ? "127.0.0.1:" + juce::String(static_cast<int>(server["port"])) : juce::String();
}

juce::String SettingsAgents::setupText() const {
  // The shape docs/agents.md and mcp/README.md give for an MCP client: the
  // sidecar reads the port and the token from the connection file itself, so
  // neither is written here.
  juce::DynamicObject::Ptr env = new juce::DynamicObject();
  env->setProperty("PEDALEIRA_CLIENT_NAME", "My agent");
  env->setProperty("PEDALEIRA_CONNECTION_FILE", server["connectionFile"].toString());
  juce::DynamicObject::Ptr entry = new juce::DynamicObject();
  entry->setProperty("command", "node");
  juce::Array<juce::var> args;
  args.add("/absolute/path/to/pedaleira/mcp/dist/src/index.js");
  entry->setProperty("args", args);
  entry->setProperty("env", juce::var(env.get()));
  juce::DynamicObject::Ptr servers = new juce::DynamicObject();
  servers->setProperty("pedaleira", juce::var(entry.get()));
  juce::DynamicObject::Ptr root = new juce::DynamicObject();
  root->setProperty("mcpServers", juce::var(servers.get()));
  return juce::JSON::toString(juce::var(root.get()), false);
}

void SettingsAgents::copyToClipboard(const juce::String& text, PushButton& button) {
  juce::SystemClipboard::copyTextToClipboard(text);
  button.setButtonText("Copied");
  copiedButton = &button;
  copiedAt = juce::Time::getMillisecondCounter();
  resized();
}

juce::String SettingsAgents::tagListText() const {
  juce::String text;
  int shown = 0;
  if (const auto* list = tags["tags"].getArray())
    for (const auto& item : *list) {
      if (shown++ == 10)
        break;
      text << (text.isEmpty() ? "" : ", ") << item["tag"].toString() << " (" << static_cast<int>(item["count"]) << ")";
    }
  return text;
}

// ####################
// LAYOUT
// ####################
int SettingsAgents::heightFor(int width) const { return layoutFor(width).height; }

SettingsAgents::Layout SettingsAgents::layoutFor(int width) const {
  using namespace settings;
  Layout l;
  const int columnWidth = (width - kCardGap) / 2;
  const int rightX = columnWidth + kCardGap;
  const int pad = kCardPadding;

  // ---- Left ----
  int y = 0;
  {
    const int cw = columnWidth - 2 * pad;
    int cy = kCardTop;
    l.serverSwitch = {pad, cy, cw, 36};
    cy += 36 + 4;
    l.status = {pad, cy, cw, 24};
    cy += 24 + 12;
    l.endpointLabel = {pad, cy, cw, 16};
    cy += 22;
    const int copyWidth = copyEndpoint.idealWidth();
    l.endpoint = {pad, cy, cw - copyWidth - 10, kFieldHeight};
    l.copy = {pad + cw - copyWidth, cy, copyWidth, kFieldHeight};
    cy += kFieldHeight + 14;
    l.setup = {pad, cy, copySetup.idealWidth(), 36};
    cy += 36 + 12;
    const juce::String note = "Local connections only. Nothing is reachable from the internet.";
    l.serverNote = {pad, cy, cw, noteHeight(cw, note, 12.0f)};
    cy += l.serverNote.getHeight();
    l.server = {0, y, columnWidth, cy + kCardBottom};
    y = l.server.getBottom() + kCardGap;
  }
  {
    const int top = y;
    int cy = top + kCardTop;
    l.agentHead = {pad, cy, columnWidth - 2 * pad, kHeadHeight};
    cy += kHeadHeight;
    if (clients.empty()) {
      l.agentEmpty = {pad, cy, columnWidth - 2 * pad, kAgentRow};
      cy += kAgentRow;
    }
    for (size_t i = 0; i < clients.size(); ++i) {
      l.agentRows.push_back({pad, cy, columnWidth - 2 * pad, kAgentRow});
      cy += kAgentRow;
    }
    l.agents = {0, top, columnWidth, cy + kCardBottom - top};
    y = l.agents.getBottom() + kCardGap;
  }
  {
    const int top = y;
    const int cw = columnWidth - 2 * pad;
    int cy = top + kCardTop;
    const juce::String note = "Agents choose captures and IRs by their tags. Better tags give better tones.";
    l.taggingNote = {pad, cy, cw, noteHeight(cw, note)};
    cy += l.taggingNote.getHeight() + 14;
    l.captures = {pad, cy, cw, 26};
    cy += 26;
    l.irs = {pad, cy, cw, 26};
    cy += 26;
    const juce::String list = tagListText();
    if (list.isNotEmpty()) {
      cy += 12;
      l.tagList = {pad, cy, cw, noteHeight(cw, "In use: " + list, 12.0f)};
      cy += l.tagList.getHeight();
    }
    l.tagging = {0, top, columnWidth, cy + kCardBottom - top};
    y = l.tagging.getBottom();
  }
  const int leftBottom = y;

  // ---- Right ----
  y = 0;
  {
    int cy = kCardTop;
    for (size_t i = 0; i < permissionToggles.size(); ++i) {
      const int h = juce::String(kPermissions[i].note).isEmpty() ? 44 : 52;
      l.permissionRows.push_back({rightX + pad, cy, columnWidth - 2 * pad, h});
      cy += h;
    }
    l.permissions = {rightX, y, columnWidth, cy + kCardBottom};
    y = l.permissions.getBottom() + kCardGap;
  }
  {
    const int top = y;
    const int cw = columnWidth - 2 * pad;
    int cy = top + kCardTop;
    const juce::String note =
        "Every guitar drives an amp differently. Record each guitar once, playing soft and hard on each pickup position, "
        "and tones are built and measured with those recordings.";
    l.guitarNote = {rightX + pad, cy, cw, noteHeight(cw, note)};
    cy += l.guitarNote.getHeight() + 12;
    if (guitars.empty()) {
      l.guitarEmpty = {rightX + pad, cy, cw, 44};
      cy += 44;
    }
    for (size_t i = 0; i < guitars.size(); ++i) {
      l.guitarRows.push_back({rightX + pad, cy, cw, kGuitarRow});
      cy += kGuitarRow;
    }
    cy += 14;
    l.addGuitar = {rightX + pad, cy, addGuitar.idealWidth(), 36};
    cy += 36;
    l.guitars = {rightX, top, columnWidth, cy + kCardBottom - top};
    y = l.guitars.getBottom();
  }
  l.height = juce::jmax(leftBottom, y);
  return l;
}

void SettingsAgents::resized() {
  lay = layoutFor(getWidth());
  enableServer.setBounds(lay.serverSwitch);
  endpoint.setBounds(lay.endpoint);
  copyEndpoint.setBounds(lay.copy);
  copySetup.setBounds(lay.setup.withWidth(copySetup.idealWidth()));
  for (size_t i = 0; i < disconnectButtons.size() && i < lay.agentRows.size(); ++i) {
    const auto row = lay.agentRows[i];
    disconnectButtons[i]->setBounds(row.withLeft(row.getRight() - disconnectButtons[i]->idealWidth()).withSizeKeepingCentre(disconnectButtons[i]->idealWidth(), 32));
  }
  for (size_t i = 0; i < permissionToggles.size() && i < lay.permissionRows.size(); ++i)
    permissionToggles[i]->setBounds(lay.permissionRows[i]);
  addGuitar.setBounds(lay.addGuitar);
}

// ####################
// PAINT
// ####################
void SettingsAgents::paint(juce::Graphics& g) {
  using namespace settings;

  // ---- MCP server ----
  paintCard(g, lay.server, "MCP server");
  {
    const bool on = running();
    const bool wanted = static_cast<bool>(server["enabled"]);
    g.setColour(on ? colour::ok : colour::faint);
    g.fillEllipse(juce::Rectangle<float>(8.0f, 8.0f).withCentre({static_cast<float>(lay.status.getX()) + 4.0f,
                                                                   static_cast<float>(lay.status.getCentreY())}));
    g.setFont(type::body());
    g.setColour(on ? colour::text : colour::muted);
    juce::String text = on ? "Running " + type::dot() + " local only" : wanted ? juce::String("Not running") : juce::String("Off");
    if (on) {
      const int connections = server["connections"];
      text << " " << type::dot() << " " << plural(connections, "client", "clients");
    }
    g.drawText(text, lay.status.withTrimmedLeft(20), juce::Justification::centredLeft, true);
    paint::label(g, "Connection", lay.endpointLabel);
    paintNote(g, lay.serverNote, "Local connections only. Nothing is reachable from the internet.", 12.0f);
  }

  // ---- Connected agents ----
  paintCard(g, lay.agents, "Connected agents");
  {
    auto head = lay.agentHead;
    paint::label(g, "Agent", head.removeFromLeft(130));
    paint::label(g, "Since", head.removeFromLeft(110));
    paint::label(g, "Last action", head);
  }
  if (clients.empty()) {
    g.setFont(type::body());
    g.setColour(colour::muted);
    g.drawText(running() ? "No agent is connected." : "Turn the server on to let an agent connect.", lay.agentEmpty,
               juce::Justification::centredLeft, true);
  }
  for (size_t i = 0; i < clients.size() && i < lay.agentRows.size(); ++i) {
    auto row = lay.agentRows[i];
    paintRowLine(g, row);
    row.removeFromRight(disconnectButtons[i]->getWidth() + 12);
    g.setFont(type::sans(14.0f, type::Weight::semibold));
    g.setColour(colour::text);
    g.drawText(clients[i].name, row.removeFromLeft(130), juce::Justification::centredLeft, true);
    g.setFont(type::sans(13.0f));
    g.setColour(colour::muted);
    g.drawText(agoText(clients[i].connected), row.removeFromLeft(110), juce::Justification::centredLeft, true);
    g.setColour(colour::text);
    g.setFont(type::mono(12.0f, type::Weight::regular));
    g.drawText(clients[i].lastCommand.isEmpty() ? juce::String("No action yet") : clients[i].lastCommand, row,
               juce::Justification::centredLeft, true);
  }

  // ---- Tone tagging ----
  paintCard(g, lay.tagging, "Tone tagging");
  paintNote(g, lay.taggingNote, "Agents choose captures and IRs by their tags. Better tags give better tones.");
  const struct {
    const char* label;
    juce::Rectangle<int> area;
    juce::var counts;
  } bars[] = {{"Captures", lay.captures, tags["captures"]}, {"IRs", lay.irs, tags["irs"]}};
  for (const auto& bar : bars) {
    auto row = bar.area;
    const int total = bar.counts["total"], tagged = bar.counts["tagged"];
    g.setFont(type::sans(13.0f));
    g.setColour(colour::text);
    g.drawText(bar.label, row.removeFromLeft(78), juce::Justification::centredLeft, false);
    g.setFont(type::mono(12.0f, type::Weight::regular));
    g.setColour(colour::muted);
    const auto count = row.removeFromRight(110);
    g.drawText(total == 0 ? juce::String("none yet") : juce::String(tagged) + " of " + juce::String(total) + " tagged", count,
               juce::Justification::centredRight, false);
    paintBar(g, row.withSizeKeepingCentre(row.getWidth() - 12, 8), total > 0 ? static_cast<double>(tagged) / total : 0.0,
             tagged == total && total > 0 ? colour::ok : colour::iris);
  }
  if (!lay.tagList.isEmpty())
    paintNote(g, lay.tagList, "In use: " + tagListText(), 12.0f);

  // ---- What agents may do ----
  paintCard(g, lay.permissions, "What agents may do");
  for (size_t i = 0; i + 1 < lay.permissionRows.size(); ++i)
    paintRowLine(g, lay.permissionRows[i]);

  // ---- Guitar profiles ----
  paintCard(g, lay.guitars, "Guitar profiles");
  paintNote(g, lay.guitarNote,
            "Every guitar drives an amp differently. Record each guitar once, playing soft and hard on each pickup "
            "position, and tones are built and measured with those recordings.");
  if (guitars.empty()) {
    g.setFont(type::body());
    g.setColour(colour::muted);
    g.drawText("No guitar recorded yet.", lay.guitarEmpty, juce::Justification::centredLeft, true);
  }
  for (size_t i = 0; i < guitars.size() && i < lay.guitarRows.size(); ++i) {
    auto row = lay.guitarRows[i];
    paintRowLine(g, row);
    const juce::String chipText = guitars[i].ready ? "Ready" : "Incomplete";
    const int chipW = paint::chipWidth(chipText);
    paint::chip(g, chipText, row.removeFromRight(chipW).withSizeKeepingCentre(chipW, 24).toFloat(),
                guitars[i].ready ? paint::ChipStyle::ok : paint::ChipStyle::warn);
    row.removeFromRight(12);
    // The name and what was measured on one line, what the guitar is under them.
    auto top = row.removeFromTop(row.getHeight() / 2 + 2).withTrimmedTop(6);
    g.setFont(type::mono(12.0f, type::Weight::regular));
    g.setColour(colour::muted);
    g.drawText(guitars[i].summary, top.removeFromRight(juce::jmin(210, top.getWidth() / 2)), juce::Justification::centredRight, true);
    g.setFont(type::sans(14.0f, type::Weight::semibold));
    g.setColour(colour::text);
    g.drawText(guitars[i].name, top, juce::Justification::centredLeft, true);
    g.setFont(type::sans(12.0f));
    g.setColour(colour::muted);
    g.drawText(guitars[i].line, row.withTrimmedBottom(4), juce::Justification::centredLeft, true);
  }
  g.setFont(type::sans(12.0f));
  g.setColour(colour::faint);
  g.drawText(kLaterBuild, lay.addGuitar.withX(lay.addGuitar.getRight() + 14).withWidth(200), juce::Justification::centredLeft, true);
}

}  // namespace pedaleira::ui

#include "SettingsPlugins.h"

#include "../Icons.h"
#include "../Paint.h"
#include "../Type.h"

#include <algorithm>

namespace pedaleira::ui {

namespace {

constexpr int kRowHeight = 44;
constexpr int kHeadHeight = 24;
constexpr int kFolderRow = 30;
constexpr int kVendorWidth = 190;
constexpr int kFormatWidth = 96;
constexpr int kChannelsWidth = 104;
constexpr int kStatusWidth = 330;
constexpr int kColumnGap = 12;

juce::String formatLabel(const juce::String& name) { return name == "AudioUnit" ? "Audio Unit" : name; }

juce::String channelsText(int in, int out) {
  const auto word = [](int n) {
    return n == 1 ? juce::String("mono") : n == 2 ? juce::String("stereo") : juce::String(n) + " ch";
  };
  if (in == out)
    return word(in);
  return juce::String(in) + " in, " + juce::String(out) + " out";
}

paint::ChipStyle styleOf(const juce::String& status) {
  if (status == "ok")
    return paint::ChipStyle::ok;
  if (status == "failed scan")
    return paint::ChipStyle::fail;
  if (status == "missing")
    return paint::ChipStyle::warn;
  return paint::ChipStyle::outline;
}

juce::String chipTextOf(const juce::String& status) {
  if (status == "ok")
    return "OK";
  if (status == "failed scan")
    return "Failed scan";
  if (status == "missing")
    return "Missing";
  return "Not an effect";
}

}  // namespace

// ####################
// SETUP
// ####################
SettingsPlugins::SettingsPlugins(EngineClient& client)
    : SettingsSection(client, "Plugins",
                      "Pedaleira hosts your VST3 and Audio Unit plugins as blocks. Scanning runs in a separate "
                      "process, so a broken plugin cannot crash the app.") {
  formatNames.add("VST3");
#if JUCE_MAC
  formatNames.add("AudioUnit");
#endif
  enabledFormats = formatNames;

  settings::styleEditor(search);
  search.setTitle("Search plugins");
  search.setTextToShowWhenEmpty("Name or vendor", colour::faint);
  search.addListener(this);
  addAndMakeVisible(search);

  scanButton.onClick = [this] {
    if (scanning())
      stopScan();
    else
      startScan(false);
  };
  rescanButton.onClick = [this] { startScan(true); };
  addFolderButton.setIcon(&icons::path(icons::Icon::plus));
  addFolderButton.onClick = [this] { chooseFolder(); };
  for (juce::Component* c : std::initializer_list<juce::Component*>{&scanButton, &rescanButton, &addFolderButton})
    addAndMakeVisible(c);

  for (const auto& name : formatNames) {
    auto toggle = std::make_unique<SettingToggle>(formatLabel(name));
    toggle->setState(true);
    SettingToggle* raw = toggle.get();
    toggle->onToggle = [this, name, raw](bool on) {
      if (on)
        enabledFormats.addIfNotAlreadyThere(name);
      else
        enabledFormats.removeString(name);
      juce::ignoreUnused(raw);
      resized();
      repaint();
    };
    addAndMakeVisible(*toggle);
    formatToggles.push_back(std::move(toggle));
  }
}

SettingsPlugins::~SettingsPlugins() { search.removeListener(this); }

// ####################
// DATA
// ####################
void SettingsPlugins::refresh() {
  const juce::var listed = engine.read("plugins.list");
  found.clear();
  if (const auto* plugins = listed["plugins"].getArray())
    for (const auto& item : *plugins) {
      Entry e;
      e.name = item["name"].toString();
      e.vendor = item["vendor"].toString();
      e.format = formatLabel(item["format"].toString());
      e.status = item["status"].toString();
      e.channels = e.status == "failed scan" ? juce::String() : channelsText(item["inputChannels"], item["outputChannels"]);
      e.reason = item["reason"].toString();
      found.push_back(e);
    }
  folders.clear();
  if (const auto* list = listed["folders"].getArray())
    for (const auto& folder : *list)
      folders.add(folder.toString());
  scan = listed["scan"];
  rebuildControls();
  applyFilter();
}

void SettingsPlugins::applyFilter() {
  const juce::String query = search.getText().trim().toLowerCase();
  shown.clear();
  for (size_t i = 0; i < found.size(); ++i)
    if (query.isEmpty() || found[i].name.toLowerCase().contains(query) || found[i].vendor.toLowerCase().contains(query))
      shown.push_back(static_cast<int>(i));
  resized();
  heightChanged();
  repaint();
}

void SettingsPlugins::setSearch(const juce::String& text) {
  search.setText(text, true);
  applyFilter();
}

void SettingsPlugins::rebuildControls() {
  folderButtons.clear();
  for (const auto& path : folders) {
    auto button = std::make_unique<IconButton>("Remove folder " + path, &icons::path(icons::Icon::close));
    // The list is rebuilt, button included: after the click returns.
    button->onClick = [this, path] { settings::afterClick(*this, [this, path] { removeFolderPath(path); }); };
    addAndMakeVisible(*button);
    folderButtons.push_back(std::move(button));
  }
}

void SettingsPlugins::showScan(const juce::var& status) {
  scan = status;
  resized();
  repaint();
}

void SettingsPlugins::startScan(bool rescanAll) {
  juce::DynamicObject::Ptr args = new juce::DynamicObject();
  args->setProperty("rescanAll", rescanAll);
  // Every format is the default; only a subset is named.
  if (enabledFormats.size() != formatNames.size()) {
    juce::Array<juce::var> names;
    for (const auto& name : enabledFormats)
      names.add(name);
    args->setProperty("formats", names);
  }
  const EngineReply reply = engine.run("plugins.scan", juce::var(args.get()));
  if (!reply.ok) {
    notice("Could not scan", reply.errorMessage);
    return;
  }
  scan = reply.value;
  // A short scan can be over before the answer.
  if (!scanning())
    refresh();
  resized();
  repaint();
}

void SettingsPlugins::stopScan() {
  const EngineReply reply = engine.run("plugins.cancelScan");
  if (reply.ok)
    scan = reply.value;
  refresh();
}

void SettingsPlugins::addFolderPath(const juce::String& path) {
  if (run("Could not add the folder", "plugins.addFolder", object({{"path", path}})))
    refresh();
}

void SettingsPlugins::removeFolderPath(const juce::String& path) {
  if (run("Could not remove the folder", "plugins.removeFolder", object({{"path", path}})))
    refresh();
}

void SettingsPlugins::chooseFolder() {
  chooser = std::make_unique<juce::FileChooser>("Add a plugin folder",
                                                juce::File::getSpecialLocation(juce::File::userHomeDirectory));
  juce::Component::SafePointer<SettingsPlugins> self(this);
  chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                       [self](const juce::FileChooser& picked) {
                         if (self == nullptr || picked.getResults().isEmpty())
                           return;
                         self->addFolderPath(picked.getResult().getFullPathName());
                       });
}

void SettingsPlugins::tick() {
  if (!scanning())
    return;
  scan = engine.read("plugins.scanStatus");
  // The scan ended: the list has what it found.
  if (!scanning()) {
    refresh();
    return;
  }
  repaint(lay.statusColumn);
}

// ####################
// LAYOUT
// ####################
int SettingsPlugins::heightFor(int width) const { return layoutFor(width).height; }

SettingsPlugins::Layout SettingsPlugins::layoutFor(int width) const {
  using namespace settings;
  Layout l;
  const int cw = width - 2 * kCardPadding;
  const int gap = 24;
  // The board's columns: 400, 160 and the rest.
  const int statusWidth = juce::jmin(400, cw * 40 / 100);
  const int formatsWidth = juce::jmin(160, cw * 17 / 100);
  const int foldersWidth = cw - statusWidth - formatsWidth - 2 * gap;

  // ---- Scan ----
  const int top = kCardTop;
  const int statusHeight = 20 + 8 + 8 + 8 + 16 + 12 + 36;
  const int formatsHeight = 22 + static_cast<int>(formatNames.size()) * 38;
  const int foldersHeight = 40 + juce::jmax(1, folders.size()) * kFolderRow + 8 + 18;
  const int content = juce::jmax(statusHeight, formatsHeight, foldersHeight);
  l.scan = {0, 0, width, top + content + kCardBottom};
  l.statusColumn = {kCardPadding, top, statusWidth, content};
  l.formatsColumn = {kCardPadding + statusWidth + gap, top, formatsWidth, content};
  l.foldersColumn = {l.formatsColumn.getRight() + gap, top, foldersWidth, content};
  l.bar = {l.statusColumn.getX(), top + 28, statusWidth, 8};
  l.buttonRow = {l.statusColumn.getX(), top + statusHeight - 36, statusWidth, 36};
  for (size_t i = 0; i < formatNames.size(); ++i)
    l.formatRows.push_back({l.formatsColumn.getX(), top + 22 + static_cast<int>(i) * 38, formatsWidth, 36});
  l.addFolder = {l.foldersColumn.getRight() - addFolderButton.idealWidth(), top - 2, addFolderButton.idealWidth(), 32};
  for (int i = 0; i < folders.size(); ++i)
    l.folderRows.push_back({l.foldersColumn.getX(), top + 40 + i * kFolderRow, foldersWidth, kFolderRow});
  l.folderNote = {l.foldersColumn.getX(), top + 40 + juce::jmax(1, folders.size()) * kFolderRow + 8, foldersWidth, 18};

  // ---- Plugins found ----
  const int pluginsTop = l.scan.getBottom() + kCardGap;
  const int rows = juce::jmax(1, static_cast<int>(shown.size()));
  l.tableTop = pluginsTop + kCardTop + kHeadHeight;
  l.tableHead = {kCardPadding, pluginsTop + kCardTop, cw, kHeadHeight};
  const int footerTop = l.tableTop + rows * kRowHeight + 14;
  l.footer = {kCardPadding, footerTop, cw, 18};
  l.plugins = {0, pluginsTop, width, footerTop + 18 + kCardBottom - pluginsTop};
  l.searchBox = {width - kCardPadding - 280, pluginsTop + 14, 280, kFieldHeight};
  l.height = l.plugins.getBottom();
  return l;
}

void SettingsPlugins::resized() {
  lay = layoutFor(getWidth());
  const bool busy = scanning();
  const bool usable = available() && !enabledFormats.isEmpty();
  scanButton.setButtonText(busy ? "Stop scan" : "Scan for plugins");
  scanButton.setKind(busy ? PushButton::Kind::secondary : PushButton::Kind::primary);
  scanButton.setEnabled(busy || usable);
  rescanButton.setEnabled(usable);
  const int scanWidth = juce::jmax(scanButton.idealWidth(), 150);
  scanButton.setBounds(lay.buttonRow.withWidth(scanWidth));
  rescanButton.setBounds(lay.buttonRow.withTrimmedLeft(scanWidth + 10).withWidth(rescanButton.idealWidth()));
  for (size_t i = 0; i < formatToggles.size() && i < lay.formatRows.size(); ++i)
    formatToggles[i]->setBounds(lay.formatRows[i]);
  addFolderButton.setBounds(lay.addFolder);
  for (size_t i = 0; i < folderButtons.size() && i < lay.folderRows.size(); ++i)
    folderButtons[i]->setBounds(lay.folderRows[i].withLeft(lay.folderRows[i].getRight() - 28).withSizeKeepingCentre(28, 28));
  search.setBounds(lay.searchBox);
}

// ####################
// PAINT
// ####################
void SettingsPlugins::paintRow(juce::Graphics& g, const Entry& entry, juce::Rectangle<int> row) {
  settings::paintRowLine(g, row);
  const bool dim = entry.status == "failed scan" || entry.status == "not an effect";
  auto area = row;
  const auto status = area.removeFromRight(kStatusWidth);
  area.removeFromRight(kColumnGap);
  const auto channels = area.removeFromRight(kChannelsWidth);
  area.removeFromRight(kColumnGap);
  const auto format = area.removeFromRight(kFormatWidth);
  area.removeFromRight(kColumnGap);
  const auto vendor = area.removeFromRight(kVendorWidth);
  area.removeFromRight(kColumnGap);

  g.setFont(type::sans(14.0f, type::Weight::medium));
  g.setColour(dim ? colour::muted : colour::text);
  g.drawText(entry.name, area, juce::Justification::centredLeft, true);
  g.setFont(type::sans(13.0f));
  g.setColour(colour::muted);
  g.drawText(entry.vendor, vendor, juce::Justification::centredLeft, true);
  g.drawText(entry.format, format, juce::Justification::centredLeft, true);
  g.setFont(type::mono(12.0f, type::Weight::regular));
  g.drawText(entry.channels, channels, juce::Justification::centredLeft, true);

  auto cell = status;
  const juce::String text = chipTextOf(entry.status);
  const int chipW = paint::chipWidth(text);
  paint::chip(g, text, cell.removeFromLeft(chipW).withSizeKeepingCentre(chipW, 24).toFloat(), styleOf(entry.status));
  // The engine's reason, as a sentence.
  juce::String note = entry.reason == "instrument" ? juce::String("Instrument, not an effect") : entry.reason;
  if (note.isNotEmpty())
    note = note.substring(0, 1).toUpperCase() + note.substring(1);
  if (note.isNotEmpty()) {
    g.setFont(type::sans(12.0f));
    g.setColour(colour::muted);
    g.drawText(note, cell.withTrimmedLeft(10), juce::Justification::centredLeft, true);
  }
}

void SettingsPlugins::paint(juce::Graphics& g) {
  using namespace settings;

  // ---- Scan ----
  paintCard(g, lay.scan, "Scan");
  {
    auto column = lay.statusColumn;
    const juce::String state = scan["state"].toString();
    auto line = column.removeFromTop(20);
    g.setFont(type::body());
    if (state == "scanning") {
      const int done = scan["done"], total = scan["total"];
      g.setColour(colour::text);
      g.drawText("Scanning" + juce::String::fromUTF8("\xe2\x80\xa6") + " " + juce::String(done) + " of " +
                     juce::String(total),
                 line, juce::Justification::centredLeft, true);
      g.setFont(type::mono(13.0f));
      g.setColour(colour::muted);
      g.drawText(juce::String(total > 0 ? done * 100 / total : 0) + "%", line, juce::Justification::centredRight, false);
      paintBar(g, lay.bar, total > 0 ? static_cast<double>(done) / total : 0.0, colour::iris);
      g.setFont(type::mono(12.0f, type::Weight::regular));
      g.setColour(colour::muted);
      g.drawText(scan["currentFile"].toString(), juce::Rectangle<int>(lay.bar.getX(), lay.bar.getBottom() + 8, lay.bar.getWidth(), 16),
                 juce::Justification::centredLeft, true);
    } else {
      juce::String head = "Not scanning", detail;
      juce::Colour headColour = colour::text;
      if (state == "unavailable") {
        head = "Scanning is not available";
        detail = "The scan helper is not installed next to the app.";
        headColour = colour::warn;
      } else if (state == "finished") {
        head = "Scan finished";
        detail = juce::String(static_cast<int>(scan["scanned"])) + " scanned, " + juce::String(static_cast<int>(scan["failed"])) + " failed";
      } else if (state == "cancelled") {
        head = "Scan stopped";
        detail = "What was scanned is kept.";
      } else {
        detail = juce::String(static_cast<int>(found.size())) + (found.size() == 1 ? " plugin known" : " plugins known");
      }
      g.setColour(headColour);
      g.drawText(head, line, juce::Justification::centredLeft, true);
      g.setFont(type::sans(13.0f));
      g.setColour(colour::muted);
      g.drawText(detail, juce::Rectangle<int>(lay.bar.getX(), lay.bar.getY() - 2, lay.bar.getWidth(), 20),
                 juce::Justification::centredLeft, true);
    }
  }
  paint::label(g, "Formats", lay.formatsColumn.withHeight(16));
  paint::label(g, "Folders", lay.foldersColumn.withHeight(16));
  for (size_t i = 0; i < lay.folderRows.size(); ++i) {
    auto row = lay.folderRows[i];
    paintRowLine(g, row);
    g.setFont(type::mono(12.0f, type::Weight::regular));
    g.setColour(colour::text);
    g.drawText(folders[static_cast<int>(i)], row.withTrimmedRight(36), juce::Justification::centredLeft, true);
  }
  if (folders.isEmpty()) {
    g.setFont(type::sans(13.0f));
    g.setColour(colour::muted);
    g.drawText("No folders added", juce::Rectangle<int>(lay.folderNote.getX(), lay.folderNote.getY() - 8 - kFolderRow, lay.folderNote.getWidth(), kFolderRow),
               juce::Justification::centredLeft, true);
  }
  g.setFont(type::sans(12.0f));
  g.setColour(colour::faint);
  g.drawText("The default locations of each format are always searched.", lay.folderNote, juce::Justification::centredLeft, true);

  // ---- Plugins found ----
  paintCard(g, lay.plugins, "Plugins found");
  {
    const int headingWidth =
        juce::roundToInt(juce::GlyphArrangement::getStringWidth(type::sans(16.0f, type::Weight::semibold), "Plugins found"));
    g.setFont(type::sans(13.0f));
    g.setColour(colour::muted);
    g.drawText(juce::String(static_cast<int>(shown.size())) + " of " + juce::String(static_cast<int>(found.size())) + " shown",
               juce::Rectangle<int>(kCardPadding + headingWidth + 14, lay.plugins.getY() + kCardPadding, 200, 22),
               juce::Justification::centredLeft, false);
  }
  {
    auto head = lay.tableHead;
    paint::label(g, "Status", head.removeFromRight(kStatusWidth));
    head.removeFromRight(kColumnGap);
    paint::label(g, "Channels", head.removeFromRight(kChannelsWidth));
    head.removeFromRight(kColumnGap);
    paint::label(g, "Format", head.removeFromRight(kFormatWidth));
    head.removeFromRight(kColumnGap);
    paint::label(g, "Vendor", head.removeFromRight(kVendorWidth));
    head.removeFromRight(kColumnGap);
    paint::label(g, "Name", head);
  }
  if (shown.empty()) {
    g.setFont(type::body());
    g.setColour(colour::muted);
    g.drawText(found.empty() ? "No plugins yet. Scan to look for the plugins on this computer."
                             : "No plugin matches the search.",
               juce::Rectangle<int>(kCardPadding, lay.tableTop, lay.tableHead.getWidth(), kRowHeight),
               juce::Justification::centredLeft, true);
  }
  const auto clip = g.getClipBounds();
  for (size_t i = 0; i < shown.size(); ++i) {
    const juce::Rectangle<int> row(kCardPadding, lay.tableTop + static_cast<int>(i) * kRowHeight, lay.tableHead.getWidth(), kRowHeight);
    if (row.intersects(clip))
      paintRow(g, found[static_cast<size_t>(shown[i])], row);
  }
  g.setFont(type::sans(12.0f));
  g.setColour(colour::faint);
  g.drawText("Plugin state is saved inside each preset.", lay.footer, juce::Justification::centredLeft, true);
}

}  // namespace pedaleira::ui

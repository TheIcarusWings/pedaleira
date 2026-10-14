#include "SettingsCaptures.h"

#include "../Icons.h"
#include "../Paint.h"
#include "../Type.h"

namespace pedaleira::ui {

namespace {

constexpr int kFolderRow = 36;
constexpr int kNoteGap = 10;

const char* const kMineNote =
    "Subfolders are searched too, and the folders' names become tags. Programs connected over MCP can read "
    "files in these folders as well (AI and MCP).";
const char* const kTone3000Note =
    "Where the TONE3000 app keeps the models it downloaded on this computer. A folder that is there is searched "
    "with the others. If yours is somewhere else, add it above.";

}  // namespace

SettingsCaptures::SettingsCaptures(EngineClient& client)
    : SettingsSection(client, "Capture folders",
                      "Pedaleira finds your NAM captures and IRs by file name and by the name inside the file, in the "
                      "library, in the folders below and in the TONE3000 app's download folder. Captures it can not "
                      "load, such as older A1 models, are listed with the reason.") {
  addFolderButton.setIcon(&icons::path(icons::Icon::plus));
  addFolderButton.onClick = [this] { chooseFolder(); };
  addAndMakeVisible(addFolderButton);
}

int SettingsCaptures::tone3000FoundCount() const {
  int found = 0;
  for (const auto& w : where)
    found += w.exists ? 1 : 0;
  return found;
}

// ####################
// DATA
// ####################
void SettingsCaptures::refresh() {
  folders.clear();
  const juce::var roots = engine.read("roots.list");
  if (const auto* added = roots["added"].getArray())
    for (const auto& folder : *added)
      folders.add(folder.toString());
  where.clear();
  const juce::var searched = engine.read("nam.tone3000Folders");
  if (const auto* list = searched["folders"].getArray())
    for (const auto& item : *list)
      where.push_back({item["path"].toString(), static_cast<bool>(item["exists"])});
  rebuildControls();
  resized();
  heightChanged();
  repaint();
}

void SettingsCaptures::rebuildControls() {
  folderButtons.clear();
  for (const auto& path : folders) {
    auto button = std::make_unique<IconButton>("Remove folder " + path, &icons::path(icons::Icon::close));
    // The list is rebuilt, button included: after the click returns.
    button->onClick = [this, path] { settings::afterClick(*this, [this, path] { removeFolderPath(path); }); };
    addAndMakeVisible(*button);
    folderButtons.push_back(std::move(button));
  }
}

void SettingsCaptures::addFolderPath(const juce::String& path) {
  if (run("Could not add the folder", "roots.add", object({{"path", path}})))
    refresh();
}

void SettingsCaptures::removeFolderPath(const juce::String& path) {
  if (run("Could not remove the folder", "roots.remove", object({{"path", path}})))
    refresh();
}

void SettingsCaptures::chooseFolder() {
  chooser = std::make_unique<juce::FileChooser>("Add a folder of captures", juce::File::getSpecialLocation(juce::File::userHomeDirectory));
  juce::Component::SafePointer<SettingsCaptures> self(this);
  chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                       [self](const juce::FileChooser& picked) {
                         if (self == nullptr || picked.getResults().isEmpty())
                           return;
                         self->addFolderPath(picked.getResult().getFullPathName());
                       });
}

// ####################
// LAYOUT
// ####################
int SettingsCaptures::heightFor(int width) const { return layoutFor(width).height; }

SettingsCaptures::Layout SettingsCaptures::layoutFor(int width) const {
  using namespace settings;
  Layout l;
  const int cw = width - 2 * kCardPadding;

  // ---- Your folders ----
  const int rowsHeight = juce::jmax(1, folders.size()) * kFolderRow;
  const int mineNote = noteHeight(cw, kMineNote, 12.0f);
  l.mine = {0, 0, width, kCardTop + rowsHeight + kNoteGap + mineNote + kCardBottom};
  l.addFolder = {width - kCardPadding - addFolderButton.idealWidth(), kCardPadding - 4, addFolderButton.idealWidth(), 32};
  for (int i = 0; i < folders.size(); ++i)
    l.folderRows.push_back({kCardPadding, kCardTop + i * kFolderRow, cw, kFolderRow});
  l.note = {kCardPadding, kCardTop + rowsHeight + kNoteGap, cw, mineNote};

  // ---- The TONE3000 app ----
  const int top = l.mine.getBottom() + kCardGap;
  const int whereHeight = juce::jmax(1, static_cast<int>(where.size())) * kFolderRow;
  const int appNote = noteHeight(cw, kTone3000Note, 12.0f);
  l.tone3000 = {0, top, width, kCardTop + whereHeight + kNoteGap + appNote + kCardBottom};
  for (size_t i = 0; i < where.size(); ++i)
    l.whereRows.push_back({kCardPadding, top + kCardTop + static_cast<int>(i) * kFolderRow, cw, kFolderRow});
  l.tone3000Note = {kCardPadding, top + kCardTop + whereHeight + kNoteGap, cw, appNote};
  l.height = l.tone3000.getBottom();
  return l;
}

void SettingsCaptures::resized() {
  lay = layoutFor(getWidth());
  addFolderButton.setBounds(lay.addFolder);
  for (size_t i = 0; i < folderButtons.size() && i < lay.folderRows.size(); ++i)
    folderButtons[i]->setBounds(lay.folderRows[i].withLeft(lay.folderRows[i].getRight() - 28).withSizeKeepingCentre(28, 28));
}

// ####################
// PAINT
// ####################
void SettingsCaptures::paint(juce::Graphics& g) {
  using namespace settings;

  paintCard(g, lay.mine, "Your folders");
  for (size_t i = 0; i < lay.folderRows.size(); ++i) {
    const auto row = lay.folderRows[i];
    paintRowLine(g, row);
    g.setFont(type::mono(12.0f, type::Weight::regular));
    g.setColour(colour::text);
    g.drawText(folders[static_cast<int>(i)], row.withTrimmedRight(36), juce::Justification::centredLeft, true);
  }
  if (folders.isEmpty()) {
    g.setFont(type::sans(13.0f));
    g.setColour(colour::muted);
    g.drawText("No folders added. Captures in the library and the ones you load are found without one.",
               juce::Rectangle<int>(kCardPadding, kCardTop, getWidth() - 2 * kCardPadding, kFolderRow), juce::Justification::centredLeft, true);
  }
  paintNote(g, lay.note, kMineNote, 12.0f);

  paintCard(g, lay.tone3000, "TONE3000 app downloads");
  for (size_t i = 0; i < lay.whereRows.size(); ++i) {
    const auto row = lay.whereRows[i];
    paintRowLine(g, row);
    auto area = row;
    const juce::String text = where[i].exists ? "Found" : "Not found";
    const int chipW = paint::chipWidth(text);
    paint::chip(g, text, area.removeFromRight(chipW).withSizeKeepingCentre(chipW, 24).toFloat(),
                where[i].exists ? paint::ChipStyle::ok : paint::ChipStyle::outline);
    area.removeFromRight(12);
    g.setFont(type::mono(12.0f, type::Weight::regular));
    g.setColour(where[i].exists ? colour::text : colour::muted);
    g.drawText(where[i].path, area, juce::Justification::centredLeft, true);
  }
  if (where.empty()) {
    g.setFont(type::sans(13.0f));
    g.setColour(colour::muted);
    g.drawText("Nothing to look in.", lay.tone3000Note.withY(lay.tone3000Note.getY() - kNoteGap - kFolderRow).withHeight(kFolderRow),
               juce::Justification::centredLeft, true);
  }
  paintNote(g, lay.tone3000Note, kTone3000Note, 12.0f);
}

}  // namespace pedaleira::ui

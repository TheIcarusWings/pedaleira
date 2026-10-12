#include "ImportPage.h"

#include "../Icons.h"
#include "../Paint.h"
#include "../Type.h"
#include "PresetImporter.h"

#include <algorithm>

namespace pedaleira::ui {

namespace {

constexpr int kStepperHeight = 36;
constexpr int kLeftWidth = 340;
constexpr int kColumnGap = 24;
constexpr int kFooterHeight = 51;
constexpr int kListRowHeight = 40;
constexpr int kMapRowHeight = 46;

const char* const kStepNames[] = {"Source", "Choose presets", "Map blocks", "Import", "Report"};

int textWidth(const juce::Font& font, const juce::String& text) {
  return juce::roundToInt(juce::GlyphArrangement::getStringWidth(font, text));
}

// A framed section, as the boards draw them.
void panel(juce::Graphics& g, juce::Rectangle<int> area) { paint::panel(g, area.toFloat()); }

void drawCheckbox(juce::Graphics& g, juce::Rectangle<float> box, bool on, bool enabled) {
  if (on) {
    g.setColour(colour::iris);
    g.fillRoundedRectangle(box, 4.0f);
    icons::draw(g, icons::path(icons::Icon::check), box.reduced(2.0f), colour::ground, 2.5f);
  } else if (enabled) {
    g.setColour(colour::faint);
    g.drawRoundedRectangle(box.reduced(0.5f), 4.0f, 1.5f);
  } else {
    paint::dashedOutline(g, box, 4.0f, colour::line);
  }
}

// Wraps `text` in `area` and draws it.
void drawWrapped(juce::Graphics& g, const juce::String& text, const juce::Font& font, juce::Colour colour,
                 juce::Rectangle<int> area) {
  juce::AttributedString string;
  string.append(text, font, colour);
  string.setLineSpacing(2.0f);
  string.draw(g, area.toFloat());
}

paint::ChipStyle chipStyleOf(ImportOutcome::Status status) {
  switch (status) {
    case ImportOutcome::Status::ok: return paint::ChipStyle::ok;
    case ImportOutcome::Status::check: return paint::ChipStyle::warn;
    case ImportOutcome::Status::placeholder: return paint::ChipStyle::dashed;
    case ImportOutcome::Status::failed: break;
  }
  return paint::ChipStyle::outline;
}

juce::String slotText(int slot) { return juce::String(slot).paddedLeft('0', 3); }

}  // namespace

// ####################
// SETUP
// ####################
ImportPage::ImportPage(EngineClient& client) : engine(client) {
  setTitle("Import");
  setWantsKeyboardFocus(true);

  cancel.onClick = [this] {
    if (step == Step::run) {
      // Stops after the preset being imported; what is in stays in.
      cursor = queue.size();
      return;
    }
    reset();
  };
  backButton.onClick = [this] { back(); };
  nextButton.onClick = [this] { next(); };
  changeSource.onClick = [this] { goTo(Step::source); };
  chooseFileButton.onClick = [this] { chooseFiles(false); };
  chooseFolderButton.onClick = [this] { chooseFiles(true); };
  selectAll.onClick = [this] {
    for (auto& item : items)
      item.selected = item.readable();
    rebuildMapping();
    layoutButtons();
    repaint();
  };
  selectNone.onClick = [this] {
    for (auto& item : items)
      item.selected = false;
    rebuildMapping();
    layoutButtons();
    repaint();
  };
  saveTable.onClick = [this] { saveMappingTable(); };
  useTable.onClick = [this] { useMappingTable(); };
  exportButton.onClick = [this] { exportReport(); };
  openButton.onClick = [this] { openOutcome(nullptr); };
  setlistBox.onClick = [this] { chooseSetlist(); };

  for (juce::Component* c : std::initializer_list<juce::Component*>{
           &cancel, &backButton, &nextButton, &changeSource, &chooseFileButton, &chooseFolderButton, &selectAll,
           &selectNone, &saveTable, &useTable, &exportButton, &openButton, &setlistBox})
    addChildComponent(c);
  addChildComponent(prompt);
  kinds = readKinds(engine);
  refresh();
  layoutButtons();
}

ImportPage::~ImportPage() { stopTimer(); }

void ImportPage::refresh() {
  kinds = readKinds(engine);
  setlists.clear();
  const juce::var library = engine.read("library.get");
  juce::String current = library["current"]["setlist"].toString();
  if (const auto* list = library["setlists"].getArray())
    for (const auto& item : *list) {
      if (static_cast<bool>(item["readOnly"]))
        continue;
      SetlistChoice choice;
      choice.id = item["id"].toString();
      choice.name = item["name"].toString();
      choice.free = 256 - static_cast<int>(item["presetCount"]);
      setlists.push_back(choice);
    }
  const auto known = [&](const juce::String& id) {
    return std::any_of(setlists.begin(), setlists.end(), [&](const SetlistChoice& s) { return s.id == id; });
  };
  if (!known(targetSetlist)) {
    targetSetlist = known(current) ? current : (setlists.empty() ? juce::String() : setlists.front().id);
  }
  layoutButtons();
  repaint();
}

// ####################
// FLOW
// ####################
void ImportPage::chooseKind(ImportKind newKind) {
  if (kind != newKind) {
    kind = newKind;
    items.clear();
    mapping.clear();
  }
  layoutButtons();
  repaint();
}

bool ImportPage::kindAvailable(int card) const { return card == 0 || card == 3; }

int ImportPage::selectedCount() const {
  int n = 0;
  for (const auto& item : items)
    if (item.selected && item.readable())
      ++n;
  return n;
}

int ImportPage::readableCount() const {
  int n = 0;
  for (const auto& item : items)
    if (item.readable())
      ++n;
  return n;
}

void ImportPage::setFiles(const juce::Array<juce::File>& files) {
  items = readImportItems(kind, expandChoice(files));
  mapping.clear();
  scroll.fill(0);
  rebuildMapping();
  layoutButtons();
  repaint();
}

void ImportPage::setSelected(int index, bool selected) {
  if (index < 0 || index >= static_cast<int>(items.size()) || !items[static_cast<size_t>(index)].readable())
    return;
  items[static_cast<size_t>(index)].selected = selected;
  rebuildMapping();
  layoutButtons();
  repaint();
}

juce::var ImportPage::currentMapping() const {
  return customMapping.isVoid() ? t3k::importer::builtInMapping("quad-cortex") : customMapping;
}

void ImportPage::rebuildMapping() {
  if (kind != ImportKind::quadCortex) {
    mapping.clear();
    return;
  }
  mapping = previewMapping(engine, customMappingFile, items, kinds);
}

void ImportPage::goTo(Step newStep) {
  step = newStep;
  if (step == Step::map)
    rebuildMapping();
  if (step == Step::report) {
    rebuildReport();
    buildFilter();
  }
  scroll.fill(0);
  layoutButtons();
  repaint();
}

void ImportPage::next() {
  switch (step) {
    case Step::source:
      if (readableCount() > 0)
        goTo(Step::choose);
      break;
    case Step::choose:
      if (selectedCount() == 0)
        return;
      if (kind == ImportKind::quadCortex)
        goTo(Step::map);
      else
        startImport();
      break;
    case Step::map: startImport(); break;
    case Step::run:
    case Step::report: break;
  }
}

void ImportPage::back() {
  switch (step) {
    case Step::choose: goTo(Step::source); break;
    case Step::map: goTo(Step::choose); break;
    default: break;
  }
}

void ImportPage::reset() {
  stopTimer();
  items.clear();
  mapping.clear();
  outcomes.clear();
  queue.clear();
  cursor = 0;
  shownOutcome = -1;
  checked.clear();
  customMapping = juce::var();
  customMappingFile = juce::File();
  goTo(Step::source);
}

void ImportPage::notice(const juce::String& title, const juce::String& message) {
  Dialog::Options options;
  options.title = title;
  options.message = message;
  options.cancelable = false;
  prompt.show(options, [](bool, const juce::String&) {});
}

void ImportPage::chooseFiles(bool folder) {
  const juce::String title = folder ? "Choose a folder of presets"
                                    : (kind == ImportKind::quadCortex ? "Choose Quad Cortex dump files" : "Choose preset files");
  chooser = std::make_unique<juce::FileChooser>(title, juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
                                                "*.json");
  juce::Component::SafePointer<ImportPage> self(this);
  const int flags = juce::FileBrowserComponent::openMode |
                    (folder ? juce::FileBrowserComponent::canSelectDirectories
                            : juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems);
  chooser->launchAsync(flags, [self](const juce::FileChooser& picked) {
    if (self == nullptr || picked.getResults().isEmpty())
      return;
    self->setFiles(picked.getResults());
    if (self->items.empty())
      self->notice("No preset files found", "The choice holds no .json files.");
  });
}

void ImportPage::chooseSetlist() {
  juce::PopupMenu menu;
  juce::Component::SafePointer<ImportPage> self(this);
  for (const auto& setlist : setlists) {
    const juce::String id = setlist.id;
    menu.addItem(setlist.name + "  (" + juce::String(setlist.free) + " free)", setlist.free > 0, id == targetSetlist,
                 [self, id] {
                   if (self == nullptr)
                     return;
                   self->targetSetlist = id;
                   self->layoutButtons();
                   self->repaint();
                 });
  }
  if (!setlists.empty())
    menu.addSeparator();
  menu.addItem("New setlist...", true, false, [self] {
    if (self == nullptr)
      return;
    Dialog::Options options;
    options.title = "New setlist";
    options.message = "The presets go into a new, empty setlist.";
    options.confirm = "Create";
    options.withText = true;
    options.text = "Imported Presets";
    self->prompt.show(options, [self](bool confirmed, const juce::String& name) {
      if (!confirmed || self == nullptr)
        return;
      const EngineReply reply = self->engine.run("setlist.create", object({{"name", name}}));
      if (!reply.ok) {
        self->notice("Could not create the setlist", reply.errorMessage);
        return;
      }
      self->targetSetlist = reply.value["id"].toString();
      self->refresh();
      if (self->onChanged)
        self->onChanged();
    });
  });
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&setlistBox));
}

void ImportPage::useMappingTable() {
  if (!customMapping.isVoid()) {
    customMapping = juce::var();
    customMappingFile = juce::File();
    rebuildMapping();
    layoutButtons();
    repaint();
    return;
  }
  chooser = std::make_unique<juce::FileChooser>("Choose a mapping table", juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
                                                "*.json");
  juce::Component::SafePointer<ImportPage> self(this);
  chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                       [self](const juce::FileChooser& picked) {
                         if (self == nullptr || !picked.getResult().existsAsFile())
                           return;
                         juce::String problem;
                         const juce::var table = t3k::importer::readJsonFile(picked.getResult(), problem);
                         if (problem.isEmpty() && table["source"].toString() != "quad-cortex")
                           problem = "The table is for the source \"" + table["source"].toString() + "\", not quad-cortex.";
                         if (problem.isEmpty() && table["models"].getDynamicObject() == nullptr)
                           problem = "The table has no models.";
                         if (problem.isNotEmpty()) {
                           self->notice("Not a usable mapping table", problem);
                           return;
                         }
                         self->customMapping = table;
                         self->customMappingFile = picked.getResult();
                         self->rebuildMapping();
                         self->layoutButtons();
                         self->repaint();
                       });
}

void ImportPage::saveMappingTable() {
  chooser = std::make_unique<juce::FileChooser>(
      "Save the mapping table", juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("quad-cortex-mapping.json"),
      "*.json");
  juce::Component::SafePointer<ImportPage> self(this);
  chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                           juce::FileBrowserComponent::warnAboutOverwriting,
                       [self](const juce::FileChooser& picked) {
                         if (self == nullptr || picked.getResult() == juce::File())
                           return;
                         if (!picked.getResult().replaceWithText(juce::JSON::toString(self->currentMapping(), false)))
                           self->notice("Could not save", "The file " + picked.getResult().getFullPathName() + " could not be written.");
                       });
}

void ImportPage::exportReport() {
  chooser = std::make_unique<juce::FileChooser>(
      "Export the report", juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Import report.md"), "*.md");
  juce::Component::SafePointer<ImportPage> self(this);
  chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                           juce::FileBrowserComponent::warnAboutOverwriting,
                       [self](const juce::FileChooser& picked) {
                         if (self == nullptr || picked.getResult() == juce::File())
                           return;
                         if (!picked.getResult().replaceWithText(self->markdown()))
                           self->notice("Could not export", "The file " + picked.getResult().getFullPathName() + " could not be written.");
                       });
}

juce::String ImportPage::markdown() const { return reportMarkdown(kind, outcomes, kinds, targetName); }

void ImportPage::openOutcome(const ReportRow* pick) {
  const ImportOutcome* outcome = selectedOutcome();
  if (outcome == nullptr || outcome->slot <= 0)
    return;
  const EngineReply reply =
      engine.run("preset.loadSlot", object({{"slot", outcome->slot}, {"setlist", targetSetlist}}));
  if (!reply.ok) {
    notice("Could not open the preset", reply.errorMessage);
    return;
  }
  if (onChanged)
    onChanged();
  if (pick != nullptr && pick->row >= 0 && pick->position >= 0 && onPickCapture)
    onPickCapture(pick->row, pick->position);
  else if (onOpenGrid)
    onOpenGrid();
}

// ####################
// THE RUN
// ####################
void ImportPage::startImport() {
  outcomes.clear();
  queue.clear();
  for (size_t i = 0; i < items.size(); ++i)
    if (items[i].selected && items[i].readable())
      queue.push_back(static_cast<int>(i));
  cursor = 0;
  if (queue.empty())
    return;

  if (targetSetlist.isEmpty()) {
    const EngineReply made = engine.run("setlist.create", object({{"name", "Imported Presets"}}));
    if (!made.ok) {
      notice("Could not create the setlist", made.errorMessage);
      return;
    }
    targetSetlist = made.value["id"].toString();
  }
  targetName = targetSetlist;
  for (const auto& setlist : setlists)
    if (setlist.id == targetSetlist)
      targetName = setlist.name;
  const juce::var state = engine.read("setlist.get", object({{"setlist", targetSetlist}}));
  if (state["name"].toString().isNotEmpty())
    targetName = state["name"].toString();

  goTo(Step::run);
  startTimer(30);
}

void ImportPage::runNow(int limit) {
  if (step != Step::run)
    startImport();
  int done = 0;
  while (step == Step::run && cursor < queue.size() && (limit < 0 || done < limit)) {
    importOne();
    ++done;
  }
  if (limit < 0 && step == Step::run)
    finishImport();
  repaint();
}

void ImportPage::timerCallback() {
  if (cursor >= queue.size()) {
    finishImport();
    return;
  }
  importOne();
  repaint();
}

void ImportPage::importOne() {
  const ImportItem& item = items[static_cast<size_t>(queue[cursor++])];
  ImportOutcome outcome;
  outcome.name = item.name;
  outcome.file = item.file;
  outcome.blocks = static_cast<int>(item.blocks.size());
  outcome.scenes = item.scenes;
  outcome.tempo = item.tempo;
  std::set<int> rowsUsed;
  for (const auto& block : item.blocks)
    rowsUsed.insert(block.row);
  outcome.rows = static_cast<int>(rowsUsed.size());

  EngineReply reply;
  if (kind == ImportKind::quadCortex) {
    juce::DynamicObject::Ptr args = new juce::DynamicObject();
    args->setProperty("file", item.file.getFullPathName());
    args->setProperty("setlist", targetSetlist);
    if (customMappingFile != juce::File())
      args->setProperty("mapping", customMappingFile.getFullPathName());
    reply = engine.run("preset.importForeign", juce::var(args.get()));
  } else {
    reply = engine.run("preset.importJson", object({{"path", item.file.getFullPathName()}, {"setlist", targetSetlist}}));
  }
  if (!reply.ok) {
    outcome.error = reply.errorMessage.isNotEmpty() ? reply.errorMessage : reply.errorCode;
  } else {
    outcome.presetId = reply.value["preset"]["id"].toString();
    outcome.slot = reply.value["preset"]["slot"];
    outcome.setlistName = reply.value["name"].toString();
    outcome.report = reply.value["report"];
    if (const auto* missing = reply.value["preset"]["missingAssets"].getArray())
      for (const auto& asset : *missing)
        outcome.missingAssets.add(asset.toString());
  }
  outcome.status = statusOf(outcome);
  outcomes.push_back(std::move(outcome));
}

void ImportPage::finishImport() {
  stopTimer();
  shownOutcome = 0;
  for (size_t i = 0; i < outcomes.size(); ++i)
    if (outcomes[i].status != ImportOutcome::Status::ok) {
      shownOutcome = static_cast<int>(i);
      break;
    }
  filter = 0;
  goTo(Step::report);
  refresh();
  if (onChanged)
    onChanged();
}

// ####################
// THE REPORT
// ####################
const ImportOutcome* ImportPage::selectedOutcome() const {
  if (shownOutcome < 0 || shownOutcome >= static_cast<int>(outcomes.size()))
    return nullptr;
  return &outcomes[static_cast<size_t>(shownOutcome)];
}

std::vector<int> ImportPage::visibleOutcomes() const {
  std::vector<int> shown;
  for (size_t i = 0; i < outcomes.size(); ++i) {
    const auto status = outcomes[i].status;
    if (filter == 1 && status != ImportOutcome::Status::check)
      continue;
    if (filter == 2 && status != ImportOutcome::Status::placeholder)
      continue;
    shown.push_back(static_cast<int>(i));
  }
  return shown;
}

void ImportPage::selectOutcome(int index) {
  if (index < 0 || index >= static_cast<int>(outcomes.size()))
    return;
  shownOutcome = index;
  rebuildReport();
  scroll[static_cast<size_t>(Area::sectionList)] = 0;
  layoutButtons();
  repaint();
}

void ImportPage::rebuildReport() {
  sections.clear();
  if (const ImportOutcome* outcome = selectedOutcome()) {
    if (outcome->status == ImportOutcome::Status::failed) {
      ReportSection failed;
      failed.kind = ReportSection::Kind::needs;
      failed.title = "Could not be imported";
      ReportRow row;
      row.detail = outcome->error;
      row.key = "failed|" + outcome->name;
      failed.rows.push_back(row);
      sections.push_back(failed);
    } else {
      for (auto& section : reportSections(kind, *outcome, kinds))
        if (!section.rows.empty())
          sections.push_back(std::move(section));
    }
  }
}

void ImportPage::buildFilter() {
  int counts[4] = {0, 0, 0, 0};
  for (const auto& outcome : outcomes)
    ++counts[static_cast<int>(outcome.status)];
  const int imported = counts[0] + counts[1] + counts[2];
  filterControl = std::make_unique<SegmentedControl>(
      juce::StringArray{"All " + juce::String(imported + counts[3]), "Check " + juce::String(counts[1]),
                        "Placeholder " + juce::String(counts[2])});
  filterControl->setTitle("Filter the imported presets");
  filterControl->setSelected(filter);
  filterControl->onChange = [this](int index) {
    filter = index;
    scroll[static_cast<size_t>(Area::presetList)] = 0;
    repaint();
  };
  addAndMakeVisible(*filterControl);
}

// ####################
// GEOMETRY
// ####################
juce::Rectangle<int> ImportPage::stepperArea() const {
  return getLocalBounds().reduced(24, 20).removeFromTop(kStepperHeight);
}

juce::Rectangle<int> ImportPage::bodyArea() const {
  auto area = getLocalBounds().reduced(24, 0).withTrimmedTop(20).withTrimmedBottom(24);
  area.removeFromTop(kStepperHeight + 16);
  return area;
}

juce::Rectangle<int> ImportPage::footerArea() const {
  auto body = bodyArea();
  return body.removeFromBottom(kFooterHeight);
}

juce::Rectangle<int> ImportPage::sourceCardBounds(int index) const {
  const auto body = bodyArea();
  const int width = (body.getWidth() - 3 * kColumnGap) / 4;
  return {body.getX() + index * (width + kColumnGap), body.getY() + 76, width, 280};
}

juce::Rectangle<int> ImportPage::viewportOf(Area area) const {
  const auto body = bodyArea();
  switch (area) {
    case Area::chooseList: {
      auto panelArea = body.withTrimmedLeft(kLeftWidth + kColumnGap).reduced(20);
      panelArea.removeFromBottom(kFooterHeight + 12);
      panelArea.removeFromTop(28 + 20 + 12 + 28);
      return panelArea;
    }
    case Area::mapList: {
      auto panelArea = body.withTrimmedLeft(kLeftWidth + kColumnGap).reduced(20);
      panelArea.removeFromBottom(kFooterHeight + 12);
      panelArea.removeFromTop(28 + 20 + 10 + 44 + 12 + 28);
      return panelArea;
    }
    case Area::runList: {
      auto panelArea = body.withTrimmedBottom(kFooterHeight + 16).reduced(24);
      panelArea.removeFromTop(28 + 16 + 8 + 16 + 24 + 12);
      return panelArea;
    }
    case Area::presetList: {
      auto panelArea = body;
      panelArea.removeFromTop(68 + 16);
      auto left = panelArea.removeFromLeft(380).reduced(16);
      left.removeFromTop(36 + 12);
      left.removeFromBottom(18 + 12);
      return left;
    }
    case Area::sectionList: {
      auto panelArea = body;
      panelArea.removeFromTop(68 + 16);
      panelArea.removeFromLeft(380 + kColumnGap);
      auto inner = panelArea.reduced(20, 18);
      inner.removeFromTop(46 + 14);
      inner.removeFromBottom(kFooterHeight + 14);
      return inner;
    }
    case Area::count: break;
  }
  return {};
}

int ImportPage::contentHeightOf(Area area) const {
  switch (area) {
    case Area::chooseList: return static_cast<int>(items.size()) * (kListRowHeight + 3);
    case Area::mapList: return static_cast<int>(mapping.size()) * kMapRowHeight;
    case Area::runList: return static_cast<int>(outcomes.size()) * (kListRowHeight + 3);
    case Area::presetList: return static_cast<int>(visibleOutcomes().size()) * (kListRowHeight + 3);
    case Area::sectionList: {
      const auto lines = sectionLines();
      return lines.empty() ? 0 : lines.back().top + lines.back().height;
    }
    case Area::count: break;
  }
  return 0;
}

void ImportPage::scrollBy(Area area, int delta) {
  const int maxScroll = juce::jmax(0, contentHeightOf(area) - viewportOf(area).getHeight());
  auto& value = scroll[static_cast<size_t>(area)];
  value = juce::jlimit(0, maxScroll, value + delta);
  repaint();
}

void ImportPage::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) {
  const int delta = -juce::roundToInt(wheel.deltaY * 200.0f);
  const auto at = event.getPosition();
  for (const Area area : {Area::chooseList, Area::mapList, Area::runList, Area::presetList, Area::sectionList}) {
    const bool onStep = (area == Area::chooseList && step == Step::choose) || (area == Area::mapList && step == Step::map) ||
                        (area == Area::runList && step == Step::run) ||
                        ((area == Area::presetList || area == Area::sectionList) && step == Step::report);
    if (onStep && viewportOf(area).expanded(0, 20).contains(at)) {
      scrollBy(area, delta);
      return;
    }
  }
}

// The source card: the board's "Source" section with the file's origin.
void ImportPage::layoutButtons() {
  const bool onSource = step == Step::source;
  const bool onChoose = step == Step::choose;
  const bool onMap = step == Step::map;
  const bool onRun = step == Step::run;
  const bool onReport = step == Step::report;
  const auto body = bodyArea();

  cancel.setVisible(onSource || onRun);
  backButton.setVisible(onChoose || onMap);
  nextButton.setVisible(onSource || onChoose || onMap);
  changeSource.setVisible(onChoose || onMap);
  chooseFileButton.setVisible(onSource);
  chooseFolderButton.setVisible(onSource);
  selectAll.setVisible(onChoose);
  selectNone.setVisible(onChoose);
  saveTable.setVisible(onMap);
  useTable.setVisible(onMap);
  exportButton.setVisible(onReport);
  openButton.setVisible(onReport);
  setlistBox.setVisible(onChoose);
  if (filterControl != nullptr)
    filterControl->setVisible(onReport);

  cancel.setButtonText(onRun ? "Stop after this one" : "Cancel");
  {
    juce::String choice = "New setlist: Imported Presets";
    for (const auto& setlist : setlists)
      if (setlist.id == targetSetlist)
        choice = setlist.name;
    setlistBox.setChoice(choice);
  }
  chooseFileButton.setButtonText(kind == ImportKind::quadCortex ? "Choose dump files..." : "Choose preset files...");

  // Step 0: the footer, and the file buttons in the section.
  {
    auto footer = footerArea();
    footer.removeFromTop(15);
    auto row = footer.removeFromTop(36);
    if (onSource) {
      nextButton.setButtonText("Choose presets");
      nextButton.setEnabled(readableCount() > 0);
      nextButton.setBounds(row.removeFromRight(nextButton.idealWidth() + 4));
      row.removeFromRight(10);
      cancel.setBounds(row.removeFromRight(cancel.idealWidth() + 4));
    }
    if (onRun) {
      cancel.setBounds(row.removeFromRight(cancel.idealWidth() + 4));
    }
  }
  if (onSource) {
    const auto sectionTop = sourceCardBounds(0).getBottom() + 16;
    const auto section = juce::Rectangle<int>(body.getX(), sectionTop, body.getWidth(), footerArea().getY() - 16 - sectionTop);
    auto left = section.reduced(20).removeFromLeft(320);
    chooseFolderButton.setBounds(left.removeFromBottom(36).removeFromLeft(chooseFolderButton.idealWidth() + 4));
    left.removeFromBottom(8);
    chooseFileButton.setBounds(left.removeFromBottom(36).removeFromLeft(chooseFileButton.idealWidth() + 4));
  }

  // Steps 1 and 2: the right panel's header and footer.
  if (onChoose || onMap) {
    const auto right = body.withTrimmedLeft(kLeftWidth + kColumnGap).reduced(20);
    auto footer = right.withTop(right.getBottom() - kFooterHeight);
    footer.removeFromTop(15);
    auto row = footer.removeFromTop(36);
    const int selected = selectedCount();
    nextButton.setButtonText(onChoose && kind == ImportKind::pedaleira
                                 ? "Import " + juce::String(selected) + (selected == 1 ? " preset" : " presets")
                                 : onChoose ? juce::String("Map blocks")
                                            : "Import " + juce::String(selected) + (selected == 1 ? " preset" : " presets"));
    nextButton.setEnabled(selected > 0);
    nextButton.setBounds(row.removeFromRight(nextButton.idealWidth() + 4));
    row.removeFromRight(10);
    if (onMap) {
      useTable.setButtonText(customMapping.isVoid() ? "Use a mapping table..." : "Use the built-in table");
      useTable.setBounds(row.removeFromRight(useTable.idealWidth() + 4));
      row.removeFromRight(10);
      saveTable.setBounds(row.removeFromRight(saveTable.idealWidth() + 4));
      row.removeFromRight(10);
    }
    backButton.setBounds(row.removeFromRight(backButton.idealWidth() + 4));
    if (onChoose) {
      auto header = right.withHeight(28);
      selectNone.setBounds(header.removeFromRight(selectNone.idealWidth()).withHeight(28));
      header.removeFromRight(6);
      selectAll.setBounds(header.removeFromRight(selectAll.idealWidth()).withHeight(28));
    }
    // The left column: the source card's header and the setlist select.
    const auto left = body.withWidth(kLeftWidth);
    changeSource.setBounds(left.getX() + kLeftWidth - 20 - (changeSource.idealWidth() - 8), left.getY() + 20,
                           changeSource.idealWidth() - 8, 26);
    if (onChoose)
      setlistBox.setBounds(left.getX() + 20, left.getY() + 130, kLeftWidth - 40, 36);
  }

  // Step 4: the panel footer and the filter.
  if (onReport) {
    auto panelArea = body;
    panelArea.removeFromTop(68 + 16);
    const auto left = panelArea.removeFromLeft(380).reduced(16);
    if (filterControl != nullptr)
      filterControl->setBounds(left.getX(), left.getY(), left.getWidth(), 36);
    panelArea.removeFromLeft(kColumnGap);
    const auto inner = panelArea.reduced(20, 18);
    auto footer = inner.withTop(inner.getBottom() - kFooterHeight);
    footer.removeFromTop(15);
    auto row = footer.removeFromTop(36);
    const ImportOutcome* outcome = selectedOutcome();
    openButton.setEnabled(outcome != nullptr && outcome->slot > 0);
    openButton.setBounds(row.removeFromRight(openButton.idealWidth() + 4));
    row.removeFromRight(10);
    exportButton.setBounds(row.removeFromRight(exportButton.idealWidth() + 4));
  }
  prompt.setBounds(getLocalBounds());
}

void ImportPage::resized() { layoutButtons(); }

// ####################
// INPUT
// ####################
void ImportPage::mouseDown(const juce::MouseEvent& event) {
  grabKeyboardFocus();
  const auto at = event.getPosition();

  if (step == Step::source) {
    for (int i = 0; i < 4; ++i)
      if (sourceCardBounds(i).contains(at) && kindAvailable(i)) {
        chooseKind(i == 0 ? ImportKind::quadCortex : ImportKind::pedaleira);
        return;
      }
    return;
  }

  if (step == Step::choose) {
    const auto viewport = viewportOf(Area::chooseList);
    if (!viewport.contains(at))
      return;
    const int index = (at.y - viewport.getY() + scrollOf(Area::chooseList)) / (kListRowHeight + 3);
    if (index >= 0 && index < static_cast<int>(items.size()) && items[static_cast<size_t>(index)].readable()) {
      auto& item = items[static_cast<size_t>(index)];
      item.selected = !item.selected;
      layoutButtons();
      repaint();
    }
    return;
  }

  if (step == Step::report) {
    const auto presets = viewportOf(Area::presetList);
    if (presets.contains(at)) {
      const auto visible = visibleOutcomes();
      const int index = (at.y - presets.getY() + scrollOf(Area::presetList)) / (kListRowHeight + 3);
      if (index >= 0 && index < static_cast<int>(visible.size()))
        selectOutcome(visible[static_cast<size_t>(index)]);
      return;
    }
    const auto detail = viewportOf(Area::sectionList);
    if (detail.contains(at)) {
      const ImportOutcome* outcome = selectedOutcome();
      for (const auto& line : sectionLines()) {
        if (line.row == nullptr)
          continue;
        const juce::String key = (outcome != nullptr ? outcome->presetId : juce::String()) + "|" + line.row->key;
        if (line.kind != ReportSection::Kind::notes && checkBounds(line).contains(at)) {
          if (checked.count(key) != 0)
            checked.erase(key);
          else
            checked.insert(key);
          repaint();
          return;
        }
        if (!line.row->role.isEmpty() && pickBounds(line).contains(at)) {
          openOutcome(line.row);
          return;
        }
      }
    }
  }
}

bool ImportPage::keyPressed(const juce::KeyPress& key) {
  if (key == juce::KeyPress::escapeKey && (step == Step::choose || step == Step::map)) {
    back();
    return true;
  }
  return false;
}

// ####################
// PAINT
// ####################
juce::String ImportPage::sourceName() const { return kind == ImportKind::quadCortex ? "Quad Cortex" : "Pedaleira presets"; }

void ImportPage::paint(juce::Graphics& g) {
  g.fillAll(colour::ground);
  paintStepper(g);
  // The page's own footer, on the steps whose actions are not inside a panel.
  if (step == Step::source || step == Step::run) {
    const auto footer = footerArea();
    g.setColour(colour::line);
    g.fillRect(footer.getX(), footer.getY(), footer.getWidth(), 1);
  }
  switch (step) {
    case Step::source: paintSource(g); break;
    case Step::choose: paintChoose(g); break;
    case Step::map: paintMap(g); break;
    case Step::run: paintRun(g); break;
    case Step::report: paintReport(g); break;
  }
}

void ImportPage::paintStepper(juce::Graphics& g) {
  auto area = stepperArea();
  juce::String note;
  if (step == Step::report) {
    int failed = 0;
    for (const auto& outcome : outcomes)
      if (outcome.status == ImportOutcome::Status::failed)
        ++failed;
    note = "Source: " + sourceName() + " " + type::dot() + " " + juce::String(static_cast<int>(outcomes.size())) +
           (outcomes.size() == 1 ? " preset" : " presets");
    if (failed > 0)
      note << " " << type::dot() << " " << failed << " could not be imported";
    g.setFont(type::sans(12.0f));
    g.setColour(colour::muted);
    const int width = textWidth(type::sans(12.0f), note);
    g.drawText(note, area.removeFromRight(width + 4), juce::Justification::centredRight, false);
    area.removeFromRight(24);
  }

  // The importer skips Map blocks for Pedaleira presets, which have no mapping.
  const auto doneAt = [&](int n) { return n < static_cast<int>(step) + 1; };
  const juce::Font nameFont = type::bodyStrong();
  const int lastWidth = 28 + 12 + textWidth(nameFont, kStepNames[4]) + 2;
  const int others = (area.getWidth() - lastWidth - 4 * 12) / 4;
  int x = area.getX();
  const int middle = area.getCentreY();
  for (int i = 0; i < 5; ++i) {
    const int n = i + 1;
    const int current = static_cast<int>(step) + 1;
    const bool done = doneAt(n);
    const bool isCurrent = n == current;
    const int width = i == 4 ? lastWidth : others;
    const juce::Colour fill = done ? colour::ok : isCurrent ? colour::iris : juce::Colours::transparentBlack;
    const juce::Colour edge = done ? colour::ok : isCurrent ? colour::iris : colour::faint;
    const auto circle = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(middle - 14), 28.0f, 28.0f);
    g.setColour(fill);
    g.fillEllipse(circle);
    g.setColour(edge);
    g.drawEllipse(circle.reduced(1.0f), 2.0f);
    if (done) {
      icons::draw(g, icons::path(icons::Icon::check), circle.reduced(7.0f), colour::ground, 2.5f);
    } else {
      g.setFont(type::mono(12.0f));
      g.setColour(isCurrent ? colour::ground : colour::muted);
      g.drawText(juce::String(n), circle.toNearestInt(), juce::Justification::centred, false);
    }
    g.setFont(nameFont);
    g.setColour(done || isCurrent ? colour::text : colour::muted);
    const int labelWidth = textWidth(nameFont, kStepNames[i]) + 2;
    g.drawText(kStepNames[i], x + 28 + 12, middle - 12, labelWidth, 24, juce::Justification::centredLeft, false);
    if (i < 4) {
      const int lineX = x + 28 + 12 + labelWidth + 12;
      g.setColour(done ? colour::ok : colour::line);
      g.fillRoundedRectangle(static_cast<float>(lineX), static_cast<float>(middle - 1), static_cast<float>(x + width - lineX), 2.0f,
                             1.0f);
    }
    x += width + 12;
  }
}

// ----- Source -----
void ImportPage::paintSource(juce::Graphics& g) {
  const auto body = bodyArea();
  g.setFont(type::title());
  g.setColour(colour::text);
  g.drawText("Import presets", body.getX(), body.getY(), body.getWidth(), 36, juce::Justification::centredLeft, false);
  g.setFont(type::body());
  g.setColour(colour::muted);
  g.drawText("Bring presets over from another modeller. Layout, scenes, tempo and the blocks we have a match for come with "
             "them; amps, cabs and captures are left for you to fill.",
             body.getX(), body.getY() + 38, body.getWidth(), 20, juce::Justification::centredLeft, true);

  struct Card {
    const char* name;
    const char* connection;
    const char* chip;
    paint::ChipStyle style;
    const char* description;
    icons::Icon icon;
    bool selected;
  };
  const Card cards[4] = {
      {"Quad Cortex", "From a dump file", "Available", paint::ChipStyle::ok,
       "Reads the files that tools/import/quadcortex_dump.py writes from your unit. Read-only: nothing is written to it.",
       icons::Icon::device, kind == ImportKind::quadCortex},
      {"Helix", "Preset files", "Planned", paint::ChipStyle::outline, "Import from exported preset files. Not built yet.",
       icons::Icon::helix, false},
      {"Other modellers", "Preset files", "Not supported yet", paint::ChipStyle::dashed,
       "Only possible where the preset format can be read openly.", icons::Icon::document, false},
      {"Pedaleira presets", "JSON files or a folder", "Available", paint::ChipStyle::ok,
       "Import presets or a whole setlist from another Pedaleira install.", icons::Icon::pedaleiraPresets,
       kind == ImportKind::pedaleira},
  };
  for (int i = 0; i < 4; ++i) {
    const auto r = sourceCardBounds(i);
    const Card& c = cards[i];
    const bool available = kindAvailable(i);
    g.setColour(colour::surface);
    g.fillRoundedRectangle(r.toFloat(), 12.0f);
    g.setColour(colour::line);
    g.drawRoundedRectangle(r.toFloat().reduced(0.5f), 12.0f, 1.0f);
    if (c.selected) {
      g.setColour(colour::iris);
      g.drawRoundedRectangle(r.toFloat().reduced(1.0f), 12.0f, 2.0f);
    }
    auto inner = r.reduced(20);
    const float alpha = available ? 1.0f : 0.8f;
    icons::draw(g, icons::path(c.icon), inner.removeFromTop(40).removeFromLeft(40).toFloat(),
                (c.selected ? colour::iris : colour::muted).withMultipliedAlpha(alpha));
    inner.removeFromTop(12);
    g.setFont(type::heading());
    g.setColour(colour::text.withMultipliedAlpha(alpha));
    g.drawText(c.name, inner.removeFromTop(26), juce::Justification::centredLeft, true);
    inner.removeFromTop(2);
    g.setFont(type::sans(13.0f));
    g.setColour(colour::muted);
    g.drawText(c.connection, inner.removeFromTop(18), juce::Justification::centredLeft, true);
    inner.removeFromTop(12);
    const auto chip = inner.removeFromTop(22).removeFromLeft(paint::chipWidth(c.chip));
    paint::chip(g, c.chip, chip.toFloat(), c.style);
    inner.removeFromTop(12);
    drawWrapped(g, c.description, type::sans(13.0f), colour::muted, inner);
  }

  // The section for the chosen source.
  const auto top = sourceCardBounds(0).getBottom() + 16;
  const juce::Rectangle<int> section(body.getX(), top, body.getWidth(), footerArea().getY() - 16 - top);
  panel(g, section);
  auto inner = section.reduced(20);
  auto left = inner.removeFromLeft(320);
  inner.removeFromLeft(40);

  g.setFont(type::heading());
  g.setColour(colour::text);
  g.drawText(sourceName(), left.removeFromTop(26), juce::Justification::centredLeft, true);
  left.removeFromTop(12);
  {
    auto status = left.removeFromTop(20);
    const bool have = readableCount() > 0;
    g.setColour(have ? colour::ok : colour::faint);
    g.fillEllipse(status.removeFromLeft(10).toFloat().withSizeKeepingCentre(10.0f, 10.0f));
    status.removeFromLeft(10);
    g.setFont(type::bodyStrong());
    g.setColour(colour::text);
    juce::String text = "No file chosen yet";
    if (!items.empty()) {
      const int ok = readableCount();
      text = juce::String(ok) + (ok == 1 ? " preset file" : " preset files") + " chosen";
      if (ok < static_cast<int>(items.size()))
        text << ", " << (static_cast<int>(items.size()) - ok) << " not usable";
    }
    g.drawText(text, status, juce::Justification::centredLeft, true);
  }
  left.removeFromTop(10);
  if (kind == ImportKind::quadCortex) {
    paint::label(g, "Format", left.removeFromTop(16));
    g.setFont(type::mono(13.0f));
    g.setColour(colour::text);
    g.drawText("pedaleira-foreign-preset, version 1", left.removeFromTop(20), juce::Justification::centredLeft, true);
  }

  // Right: how to get the files, then the cautions.
  auto drawNote = [&](icons::Icon icon, const juce::String& text, bool warning) {
    const int height = detailHeight(text, inner.getWidth() - 24 - 28, false) + 24;
    auto box = inner.removeFromTop(height);
    if (warning) {
      g.setColour(colour::raised);
      g.fillRoundedRectangle(box.toFloat(), size::buttonRadius);
      g.setColour(colour::warn);
      g.drawRoundedRectangle(box.toFloat().reduced(0.5f), size::buttonRadius, 1.0f);
    }
    auto row = box.reduced(12, warning ? 12 : 0);
    icons::draw(g, icons::path(icon), row.removeFromLeft(18).removeFromTop(18).toFloat(), warning ? colour::warn : colour::muted);
    row.removeFromLeft(10);
    drawWrapped(g, text, type::sans(13.0f), warning ? colour::warn : colour::muted, row);
    inner.removeFromTop(10);
  };

  if (kind == ImportKind::quadCortex) {
    paint::label(g, "Make the dump", inner.removeFromTop(16));
    inner.removeFromTop(6);
    drawWrapped(g, "On the computer the Quad Cortex is connected to, run this once. It writes one .json file per preset and only "
                   "reads from the unit; then choose the files here.",
                type::sans(13.0f), colour::text, inner.removeFromTop(38));
    inner.removeFromTop(8);
    {
      auto box = inner.removeFromTop(36);
      g.setColour(colour::ground);
      g.fillRoundedRectangle(box.toFloat(), size::buttonRadius);
      g.setColour(colour::line);
      g.drawRoundedRectangle(box.toFloat().reduced(0.5f), size::buttonRadius, 1.0f);
      g.setFont(type::mono(13.0f, type::Weight::regular));
      g.setColour(colour::text);
      g.drawText("python3 tools/import/quadcortex_dump.py --all --yes --out dumps/", box.reduced(12, 0),
                 juce::Justification::centredLeft, true);
    }
    inner.removeFromTop(6);
    g.setFont(type::sans(12.0f));
    g.setColour(colour::muted);
    g.drawText("--all recalls each preset on the unit. For only the preset on the grid, use --current instead of --all --yes.",
               inner.removeFromTop(18), juce::Justification::centredLeft, true);
    inner.removeFromTop(10);
    drawNote(icons::Icon::warning, "Quit the QC desktop editor first; it holds the USB connection.", true);
    drawNote(icons::Icon::info, "Neural Captures cannot be imported. They are recaptured with the capture wizard instead.", false);
  } else {
    paint::label(g, "What to choose", inner.removeFromTop(16));
    inner.removeFromTop(6);
    drawWrapped(g, "Preset .json files, or a folder of them. A setlist folder from another install works too. A model file in an "
                   "Assets folder beside a preset comes with it.",
                type::sans(13.0f), colour::text, inner.removeFromTop(38));
    inner.removeFromTop(10);
    drawNote(icons::Icon::info, "Presets go into empty slots of a setlist you choose next. A model file that cannot be found is "
                                "listed in the report and its block comes up missing.",
             false);
  }
}

int ImportPage::detailHeight(const juce::String& text, int width, bool mono) const {
  juce::AttributedString string;
  string.append(text, mono ? type::mono(12.0f) : type::sans(13.0f), colour::muted);
  string.setLineSpacing(2.0f);
  juce::TextLayout layout;
  layout.createLayout(string, static_cast<float>(juce::jmax(40, width)));
  return juce::roundToInt(std::ceil(layout.getHeight())) + 2;
}

// ----- Left column used by steps 1 and 2 -----
void ImportPage::paintSourceCard(juce::Graphics& g, juce::Rectangle<int> area, bool withSetlist) {
  panel(g, area);
  auto inner = area.reduced(20);
  auto header = inner.removeFromTop(26);
  paint::label(g, "Source", header);
  inner.removeFromTop(14);
  auto row = inner.removeFromTop(36);
  const auto tile = row.removeFromLeft(36).toFloat();
  g.setColour(colour::ok.withAlpha(0.16f));
  g.fillRoundedRectangle(tile, size::buttonRadius);
  icons::draw(g, icons::path(kind == ImportKind::quadCortex ? icons::Icon::device : icons::Icon::pedaleiraPresets),
              tile.withSizeKeepingCentre(20.0f, 20.0f), colour::ok);
  row.removeFromLeft(12);
  g.setFont(type::bodyStrong());
  g.setColour(colour::text);
  g.drawText("Source: " + sourceName() + (kind == ImportKind::quadCortex ? " " + type::dot() + " dump files" : juce::String()),
             row.removeFromTop(20), juce::Justification::centredLeft, true);
  juce::String version;
  for (const auto& item : items)
    if (item.sourceVersion.isNotEmpty()) {
      version = item.sourceVersion;
      break;
    }
  g.setFont(type::mono(12.0f, type::Weight::regular));
  g.setColour(colour::muted);
  g.drawText(version, row, juce::Justification::centredLeft, true);
  inner.removeFromTop(14);

  if (withSetlist) {
    paint::label(g, "Import into", inner.removeFromTop(16));
    inner.removeFromTop(4 + 36 + 14);
    const int selected = selectedCount();
    const int total = readableCount();
    g.setFont(type::bodyStrong());
    g.setColour(colour::text);
    g.drawText(juce::String(selected) + (selected == 1 ? " preset selected of " : " presets selected of ") + juce::String(total),
               inner.removeFromTop(20), juce::Justification::centredLeft, true);
    inner.removeFromTop(6);
    const auto bar = inner.removeFromTop(6).toFloat();
    g.setColour(colour::line);
    g.fillRoundedRectangle(bar, 3.0f);
    g.setColour(colour::iris);
    g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * (total > 0 ? static_cast<float>(selected) / static_cast<float>(total) : 0.0f)),
                           3.0f);
    inner.removeFromTop(10);
    int free = 0;
    for (const auto& setlist : setlists)
      if (setlist.id == targetSetlist)
        free = setlist.free;
    const bool newSetlist = targetSetlist.isEmpty();
    if (!newSetlist && selected > free) {
      g.setFont(type::sans(12.0f));
      g.setColour(colour::warn);
      g.drawText("Only " + juce::String(free) + " free slots here; the rest will not fit.", inner.removeFromTop(18),
                 juce::Justification::centredLeft, true);
    }
  }
  inner.removeFromTop(8);
  auto lock = inner.removeFromBottom(20);
  icons::draw(g, icons::path(icons::Icon::lock), lock.removeFromLeft(16).toFloat().withSizeKeepingCentre(16.0f, 16.0f), colour::muted);
  lock.removeFromLeft(8);
  g.setFont(type::sans(13.0f));
  g.setColour(colour::muted);
  g.drawText(kind == ImportKind::quadCortex ? "Read-only: the dump files are only read." : "Read-only: the files are only read.",
             lock, juce::Justification::centredLeft, true);
}

// ----- Choose presets -----
void ImportPage::paintChoose(juce::Graphics& g) {
  const auto body = bodyArea();
  auto left = body.withWidth(kLeftWidth);
  paintSourceCard(g, left.withHeight(290), true);

  const auto right = body.withTrimmedLeft(kLeftWidth + kColumnGap);
  panel(g, right);
  auto inner = right.reduced(20);
  g.setFont(type::heading());
  g.setColour(colour::text);
  g.drawText("Choose presets", inner.removeFromTop(28), juce::Justification::centredLeft, false);
  g.setFont(type::sans(13.0f));
  g.setColour(colour::muted);
  g.drawText(kind == ImportKind::quadCortex ? "Each file is one preset from the unit. Untick the ones to leave out."
                                            : "Untick the presets to leave out. Each goes into the next free slot.",
             inner.removeFromTop(20), juce::Justification::centredLeft, true);
  inner.removeFromTop(12);

  const auto viewport = viewportOf(Area::chooseList);
  const auto head = juce::Rectangle<int>(viewport.getX(), viewport.getY() - 28, viewport.getWidth(), 28).reduced(10, 0);
  auto h = head;
  h.removeFromLeft(18 + 12);
  const bool qc = kind == ImportKind::quadCortex;
  auto cols = h;
  const auto tempoCol = qc ? cols.removeFromRight(84) : juce::Rectangle<int>();
  if (qc)
    cols.removeFromRight(12);
  const auto scenesCol = qc ? cols.removeFromRight(56) : juce::Rectangle<int>();
  if (qc)
    cols.removeFromRight(12);
  const auto blocksCol = qc ? cols.removeFromRight(56) : juce::Rectangle<int>();
  if (qc)
    cols.removeFromRight(12);
  paint::label(g, "Name", cols);
  if (qc) {
    paint::label(g, "Blocks", blocksCol);
    paint::label(g, "Scenes", scenesCol);
    paint::label(g, "Tempo", tempoCol);
  }

  juce::Graphics::ScopedSaveState state(g);
  g.reduceClipRegion(viewport);
  for (size_t i = 0; i < items.size(); ++i) {
    const ImportItem& item = items[i];
    const int y = viewport.getY() + static_cast<int>(i) * (kListRowHeight + 3) - scrollOf(Area::chooseList);
    if (y + kListRowHeight < viewport.getY() || y > viewport.getBottom())
      continue;
    const juce::Rectangle<int> r(viewport.getX(), y, viewport.getWidth(), kListRowHeight);
    g.setColour(item.selected ? colour::raised : colour::surface);
    g.fillRoundedRectangle(r.toFloat(), size::buttonRadius);
    g.setColour(colour::line);
    g.drawRoundedRectangle(r.toFloat().reduced(0.5f), size::buttonRadius, 1.0f);
    auto row = r.reduced(10, 0);
    drawCheckbox(g, row.removeFromLeft(18).toFloat().withSizeKeepingCentre(18.0f, 18.0f), item.selected, item.readable());
    row.removeFromLeft(12);
    if (!item.readable()) {
      g.setFont(type::body());
      g.setColour(colour::muted);
      const int nameWidth = juce::jmin(220, textWidth(type::body(), item.file.getFileName()) + 4);
      g.drawText(item.file.getFileName(), row.removeFromLeft(nameWidth), juce::Justification::centredLeft, true);
      row.removeFromLeft(12);
      g.setFont(type::sans(12.0f));
      g.setColour(colour::warn);
      g.drawText(item.problem, row, juce::Justification::centredLeft, true);
      continue;
    }
    if (qc) {
      auto c = row;
      const auto t = c.removeFromRight(84);
      c.removeFromRight(12);
      const auto s = c.removeFromRight(56);
      c.removeFromRight(12);
      const auto b = c.removeFromRight(56);
      c.removeFromRight(12);
      g.setFont(type::bodyStrong());
      g.setColour(colour::text);
      g.drawText(item.name, c, juce::Justification::centredLeft, true);
      g.setFont(type::value());
      g.drawText(juce::String(static_cast<int>(item.blocks.size())), b, juce::Justification::centredLeft, false);
      g.drawText(item.scenes > 0 ? juce::String(item.scenes) : juce::String(), s, juce::Justification::centredLeft, false);
      g.drawText(item.tempo > 0.0 ? juce::String(item.tempo, 1) + " BPM" : juce::String(), t, juce::Justification::centredLeft,
                 false);
    } else {
      g.setFont(type::bodyStrong());
      g.setColour(colour::text);
      const auto file = row.removeFromRight(220);
      g.drawText(item.name, row, juce::Justification::centredLeft, true);
      g.setFont(type::mono(12.0f, type::Weight::regular));
      g.setColour(colour::muted);
      g.drawText(item.file.getFileName(), file, juce::Justification::centredRight, true);
    }
  }
  if (items.empty()) {
    g.setFont(type::body());
    g.setColour(colour::muted);
    g.drawText("No files chosen. Go back and choose some.", viewport.withHeight(24), juce::Justification::centredLeft, false);
  }
}

// ----- Map blocks -----
void ImportPage::paintCarriedOver(juce::Graphics& g, juce::Rectangle<int> area) {
  panel(g, area);
  auto inner = area.reduced(20);
  paint::label(g, "Carried over", inner.removeFromTop(16));
  inner.removeFromTop(12);
  const char* carried[] = {"Grid layout and splits", "Scene names", "Scene on or off states and values",
                           "Tempo"};
  for (const char* text : carried) {
    auto row = inner.removeFromTop(20);
    icons::draw(g, icons::path(icons::Icon::check), row.removeFromLeft(16).toFloat().withSizeKeepingCentre(16.0f, 16.0f), colour::ok,
                2.25f);
    row.removeFromLeft(10);
    g.setFont(type::body());
    g.setColour(colour::text);
    g.drawText(text, row, juce::Justification::centredLeft, true);
    inner.removeFromTop(8);
  }
  inner.removeFromTop(8);
  paint::label(g, "Left behind", inner.removeFromTop(16));
  inner.removeFromTop(12);
  const char* behind[] = {"Footswitch assignments", "Expression pedals and MIDI", "Mixer and splitter settings"};
  for (const char* text : behind) {
    auto row = inner.removeFromTop(20);
    icons::draw(g, icons::path(icons::Icon::close), row.removeFromLeft(16).toFloat().withSizeKeepingCentre(14.0f, 14.0f), colour::muted,
                2.0f);
    row.removeFromLeft(10);
    g.setFont(type::body());
    g.setColour(colour::muted);
    g.drawText(text, row, juce::Justification::centredLeft, true);
    inner.removeFromTop(8);
  }
}

void ImportPage::paintMap(juce::Graphics& g) {
  const auto body = bodyArea();
  const auto left = body.withWidth(kLeftWidth);
  paintSourceCard(g, left.withHeight(160), false);
  paintCarriedOver(g, juce::Rectangle<int>(left.getX(), left.getY() + 160 + 16, kLeftWidth, 282));

  const auto right = body.withTrimmedLeft(kLeftWidth + kColumnGap);
  panel(g, right);
  auto inner = right.reduced(20);
  g.setFont(type::heading());
  g.setColour(colour::text);
  g.drawText("Map blocks", inner.removeFromTop(28), juce::Justification::centredLeft, false);
  g.setFont(type::sans(13.0f));
  g.setColour(colour::muted);
  g.drawText("What the mapping table does with each block of the " +
                 juce::String(selectedCount()) + " chosen presets. Amps, cabs and captures are never mapped.",
             inner.removeFromTop(20), juce::Justification::centredLeft, true);
  inner.removeFromTop(10);
  {
    auto box = inner.removeFromTop(44);
    g.setColour(colour::raised);
    g.fillRoundedRectangle(box.toFloat(), size::buttonRadius);
    g.setColour(colour::warn);
    g.drawRoundedRectangle(box.toFloat().reduced(0.5f), size::buttonRadius, 1.0f);
    auto row = box.reduced(12, 0);
    icons::draw(g, icons::path(icons::Icon::warning), row.removeFromLeft(18).toFloat().withSizeKeepingCentre(18.0f, 18.0f), colour::warn);
    row.removeFromLeft(10);
    g.setFont(type::sans(13.0f));
    g.setColour(colour::warn);
    g.drawText(customMapping.isVoid() ? "Every mapping here is unverified: none has been checked against a real Quad Cortex yet. "
                                        "Listen to each preset."
                                      : "Using " + customMappingFile.getFileName() + ". Check its entries against a real unit.",
               row, juce::Justification::centredLeft, true);
  }

  const auto viewport = viewportOf(Area::mapList);
  const auto head = juce::Rectangle<int>(viewport.getX(), viewport.getY() - 28, viewport.getWidth(), 28).reduced(10, 0);
  auto cols = head;
  const auto sourceCol = cols.removeFromLeft(220);
  cols.removeFromLeft(12);
  const auto usedCol = cols.removeFromLeft(84);
  cols.removeFromLeft(12);
  const auto matchCol = cols.removeFromRight(112);
  cols.removeFromRight(12);
  paint::label(g, "Source block", sourceCol);
  paint::label(g, "Used in", usedCol);
  paint::label(g, "Maps to", cols);
  paint::label(g, "Match", matchCol);

  {
  juce::Graphics::ScopedSaveState state(g);
  g.reduceClipRegion(viewport);
  for (size_t i = 0; i < mapping.size(); ++i) {
    const MappingRow& m = mapping[i];
    const int y = viewport.getY() + static_cast<int>(i) * kMapRowHeight - scrollOf(Area::mapList);
    if (y + kMapRowHeight < viewport.getY() || y > viewport.getBottom())
      continue;
    const juce::Rectangle<int> r(viewport.getX(), y, viewport.getWidth(), kMapRowHeight);
    g.setColour(colour::line);
    g.fillRect(r.getX(), r.getBottom() - 1, r.getWidth(), 1);
    auto c = r.reduced(10, 0);
    g.setFont(type::bodyStrong());
    g.setColour(colour::text);
    g.drawText(m.sourceBlock, c.removeFromLeft(220), juce::Justification::centredLeft, true);
    c.removeFromLeft(12);
    g.setFont(type::value());
    g.setColour(colour::muted);
    g.drawText(juce::String(m.usedIn), c.removeFromLeft(84), juce::Justification::centredLeft, false);
    c.removeFromLeft(12);
    auto chipArea = c.removeFromRight(112);
    c.removeFromRight(12);

    const bool mapped = m.kind == MappingRow::Kind::mapped || m.kind == MappingRow::Kind::fallback;
    const auto icon = c.removeFromLeft(28).toFloat().withSizeKeepingCentre(28.0f, 28.0f);
    c.removeFromLeft(10);
    if (mapped) {
      g.setColour(categoryColour(m.category).withAlpha(0.16f));
      g.fillRoundedRectangle(icon, 6.0f);
      icons::draw(g, icons::path(m.category), icon.reduced(6.0f), categoryColour(m.category));
      g.setFont(type::body());
      g.setColour(colour::text);
      const juce::String text = m.kind == MappingRow::Kind::fallback ? m.kindName + "  (a plain block at defaults)" : m.kindName;
      g.drawText(text, c, juce::Justification::centredLeft, true);
    } else {
      paint::dashedOutline(g, icon, 6.0f, colour::faint);
      icons::draw(g, icons::path(icons::Icon::warning), icon.reduced(6.0f), colour::muted);
      g.setFont(type::body());
      g.setColour(colour::muted);
      g.drawText(m.note, c, juce::Justification::centredLeft, true);
    }
    const juce::String chipText = m.kind == MappingRow::Kind::mapped ? (m.verified ? "Exact" : "Unverified")
                                  : m.kind == MappingRow::Kind::fallback ? "Fallback"
                                  : m.kind == MappingRow::Kind::empty    ? "Empty slot"
                                                                         : "Not mapped";
    const auto style = m.kind == MappingRow::Kind::mapped && m.verified ? paint::ChipStyle::ok
                       : mapped                                         ? paint::ChipStyle::warn
                                                                        : paint::ChipStyle::dashed;
    paint::chip(g, chipText, chipArea.withSizeKeepingCentre(juce::jmax(88, paint::chipWidth(chipText)), 22).withX(chipArea.getX()).toFloat(),
                style);
  }
  if (mapping.empty()) {
    g.setFont(type::body());
    g.setColour(colour::muted);
    g.drawText("The chosen presets hold no blocks.", viewport.withHeight(24), juce::Justification::centredLeft, false);
  }
  }

  // The footer's summary, left of the buttons.
  int mappedCount = 0, emptyCount = 0, unmappedCount = 0;
  for (const auto& m : mapping) {
    if (m.kind == MappingRow::Kind::empty)
      ++emptyCount;
    else if (m.kind == MappingRow::Kind::unmapped)
      ++unmappedCount;
    else
      ++mappedCount;
  }
  auto footer = right.reduced(20);
  footer = footer.removeFromBottom(kFooterHeight);
  g.setColour(colour::line);
  g.fillRect(footer.getX(), footer.getY(), footer.getWidth(), 1);
  g.setFont(type::bodyStrong());
  g.setColour(colour::text);
  g.drawText(juce::String(mappedCount) + " mapped, unverified " + type::dot() + " " + juce::String(emptyCount) + " left empty " +
                 type::dot() + " " + juce::String(unmappedCount) + " not mapped",
             footer.withTrimmedTop(15).withHeight(36).withWidth(juce::jmax(100, saveTable.getX() - footer.getX() - 12)),
             juce::Justification::centredLeft, true);
}

// ----- Import -----
void ImportPage::paintRun(juce::Graphics& g) {
  const auto body = bodyArea().withTrimmedBottom(kFooterHeight + 16);
  panel(g, body);
  auto inner = body.reduced(24);
  const int total = static_cast<int>(queue.size());
  const int done = static_cast<int>(cursor);
  auto head = inner.removeFromTop(28);
  g.setFont(type::heading());
  g.setColour(colour::text);
  g.drawText("Importing presets", head, juce::Justification::centredLeft, false);
  g.setFont(type::mono(13.0f));
  g.setColour(colour::muted);
  g.drawText(juce::String(done) + " of " + juce::String(total), head, juce::Justification::centredRight, false);
  inner.removeFromTop(16);
  const auto bar = inner.removeFromTop(8).toFloat();
  g.setColour(colour::line);
  g.fillRoundedRectangle(bar, 4.0f);
  g.setColour(colour::iris);
  g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * (total > 0 ? static_cast<float>(done) / static_cast<float>(total) : 0.0f)), 4.0f);
  inner.removeFromTop(16);
  g.setFont(type::sans(13.0f));
  g.setColour(colour::muted);
  g.drawText(done < total ? "Into " + targetName + ". Each preset goes into the next free slot." : "Done.", inner.removeFromTop(24),
             juce::Justification::centredLeft, true);

  const auto viewport = viewportOf(Area::runList);
  juce::Graphics::ScopedSaveState state(g);
  g.reduceClipRegion(viewport);
  for (size_t i = 0; i < outcomes.size(); ++i) {
    const int y = viewport.getY() + static_cast<int>(i) * (kListRowHeight + 3) - scrollOf(Area::runList);
    if (y + kListRowHeight < viewport.getY() || y > viewport.getBottom())
      continue;
    const juce::Rectangle<int> r(viewport.getX(), y, viewport.getWidth(), kListRowHeight);
    g.setColour(colour::raised);
    g.fillRoundedRectangle(r.toFloat(), size::buttonRadius);
    auto row = r.reduced(10, 0);
    const auto status = outcomes[i].status;
    const juce::String text = statusName(status);
    paint::chip(g, text, row.removeFromLeft(88).withSizeKeepingCentre(88, 22).toFloat(), chipStyleOf(status));
    row.removeFromLeft(10);
    g.setFont(type::mono(12.0f, type::Weight::regular));
    g.setColour(colour::muted);
    g.drawText(outcomes[i].slot > 0 ? slotText(outcomes[i].slot) : juce::String(), row.removeFromLeft(36),
               juce::Justification::centredLeft, false);
    g.setFont(type::bodyStrong());
    g.setColour(colour::text);
    g.drawText(outcomes[i].name, row.removeFromLeft(juce::jmin(row.getWidth() / 2, 320)), juce::Justification::centredLeft, true);
    if (status == ImportOutcome::Status::failed) {
      g.setFont(type::sans(12.0f));
      g.setColour(colour::warn);
      g.drawText(outcomes[i].error, row, juce::Justification::centredLeft, true);
    }
  }
}

// ----- Report -----
std::vector<ImportPage::SectionLine> ImportPage::sectionLines() const {
  std::vector<SectionLine> lines;
  const auto viewport = viewportOf(Area::sectionList);
  int y = 0;
  for (const auto& section : sections) {
    SectionLine heading;
    heading.top = y;
    heading.height = 26;
    heading.heading = true;
    heading.kind = section.kind;
    heading.title = section.title;
    lines.push_back(heading);
    y += 26 + 6;
    for (const auto& row : section.rows) {
      SectionLine line;
      line.top = y;
      line.row = &row;
      line.kind = section.kind;
      const bool labelled = row.label.isNotEmpty();
      line.labelWidth = labelled ? 200 : 0;
      const int textWidth = viewport.getWidth() - 24 - (labelled ? 200 + 12 : 0) - (section.kind == ReportSection::Kind::notes ? 0 : 16 + 90);
      int height = juce::jmax(32, detailHeight(row.detail, textWidth, row.mono) + 12);
      if (row.role.isNotEmpty())
        height += 38;
      line.height = height;
      lines.push_back(line);
      y += height + 6;
    }
    y += 8;
  }
  return lines;
}

juce::Rectangle<int> ImportPage::checkBounds(const SectionLine& line) const {
  const auto viewport = viewportOf(Area::sectionList);
  const int top = viewport.getY() + line.top - scrollOf(Area::sectionList);
  return {viewport.getRight() - 12 - 84, top, 84, juce::jmin(line.height, 32)};
}

juce::Rectangle<int> ImportPage::pickBounds(const SectionLine& line) const {
  const auto viewport = viewportOf(Area::sectionList);
  const int top = viewport.getY() + line.top - scrollOf(Area::sectionList);
  const int width = textWidth(type::button(), "Pick a capture") + 24;
  return {viewport.getX() + 12 + (line.labelWidth > 0 ? line.labelWidth + 12 : 0), top + line.height - 36, width, 30};
}

void ImportPage::paintReport(juce::Graphics& g) {
  const auto body = bodyArea();
  // The four counts.
  int counts[4] = {0, 0, 0, 0};
  for (const auto& outcome : outcomes)
    ++counts[static_cast<int>(outcome.status)];
  struct Stat {
    int number;
    const char* label;
    juce::Colour colour;
    icons::Icon icon;
    bool dashed;
  };
  const Stat stats[4] = {{counts[0] + counts[1] + counts[2], "presets imported", colour::iris, icons::Icon::import, false},
                         {counts[0], "need nothing", colour::ok, icons::Icon::check, false},
                         {counts[1], "to check by ear", colour::warn, icons::Icon::ear, false},
                         {counts[2], "have placeholders", colour::muted, icons::Icon::warning, true}};
  const int tileWidth = (body.getWidth() - 3 * 16) / 4;
  for (int i = 0; i < 4; ++i) {
    const juce::Rectangle<int> r(body.getX() + i * (tileWidth + 16), body.getY(), tileWidth, 68);
    g.setColour(colour::surface);
    g.fillRoundedRectangle(r.toFloat(), size::panelRadius);
    if (stats[i].dashed)
      paint::dashedOutline(g, r.toFloat(), size::panelRadius, colour::faint);
    else {
      g.setColour(colour::line);
      g.drawRoundedRectangle(r.toFloat().reduced(0.5f), size::panelRadius, 1.0f);
    }
    auto inner = r.reduced(16, 0);
    const auto tile = inner.removeFromLeft(36).toFloat().withSizeKeepingCentre(36.0f, 36.0f);
    g.setColour(stats[i].colour.withAlpha(0.16f));
    g.fillRoundedRectangle(tile, size::buttonRadius);
    icons::draw(g, icons::path(stats[i].icon), tile.withSizeKeepingCentre(20.0f, 20.0f), stats[i].colour);
    inner.removeFromLeft(14);
    g.setFont(type::mono(28.0f));
    g.setColour(colour::text);
    const juce::String number(stats[i].number);
    const int numberWidth = textWidth(type::mono(28.0f), number) + 8;
    g.drawText(number, inner.removeFromLeft(numberWidth), juce::Justification::centredLeft, false);
    g.setFont(type::body());
    g.setColour(colour::muted);
    g.drawText(stats[i].label, inner, juce::Justification::centredLeft, true);
  }

  // The list of imported presets.
  auto lower = body;
  lower.removeFromTop(68 + 16);
  auto listPanel = lower.removeFromLeft(380);
  panel(g, listPanel);
  const auto visible = visibleOutcomes();
  {
    const auto viewport = viewportOf(Area::presetList);
    juce::Graphics::ScopedSaveState state(g);
    g.reduceClipRegion(viewport);
    for (size_t i = 0; i < visible.size(); ++i) {
      const int y = viewport.getY() + static_cast<int>(i) * (kListRowHeight + 3) - scrollOf(Area::presetList);
      if (y + kListRowHeight < viewport.getY() || y > viewport.getBottom())
        continue;
      const ImportOutcome& outcome = outcomes[static_cast<size_t>(visible[i])];
      const juce::Rectangle<int> r(viewport.getX(), y, viewport.getWidth(), kListRowHeight);
      const bool selected = visible[i] == shownOutcome;
      if (selected) {
        g.setColour(colour::control);
        g.fillRoundedRectangle(r.toFloat(), size::buttonRadius);
        g.setColour(colour::iris);
        g.drawRoundedRectangle(r.toFloat().reduced(0.5f), size::buttonRadius, 1.0f);
      }
      auto row = r.reduced(10, 0);
      paint::chip(g, statusName(outcome.status), row.removeFromLeft(88).withSizeKeepingCentre(88, 22).toFloat(),
                  chipStyleOf(outcome.status));
      row.removeFromLeft(10);
      g.setFont(type::mono(12.0f, type::Weight::regular));
      g.setColour(colour::muted);
      g.drawText(outcome.slot > 0 ? slotText(outcome.slot) : juce::String(), row.removeFromLeft(34), juce::Justification::centredLeft,
                 false);
      g.setFont(type::sans(14.0f, selected ? type::Weight::semibold : type::Weight::medium));
      g.setColour(colour::text);
      g.drawText(outcome.name, row, juce::Justification::centredLeft, true);
    }
  }
  g.setFont(type::sans(12.0f));
  g.setColour(colour::muted);
  g.drawText("Showing " + juce::String(static_cast<int>(visible.size())) + " of " + juce::String(static_cast<int>(outcomes.size())) +
                 " imported presets",
             listPanel.reduced(16).removeFromBottom(18), juce::Justification::centredLeft, true);

  paintSections(g);
}

void ImportPage::paintSections(juce::Graphics& g) {
  auto lower = bodyArea();
  lower.removeFromTop(68 + 16);
  lower.removeFromLeft(380 + kColumnGap);
  panel(g, lower);
  auto inner = lower.reduced(20, 18);
  const ImportOutcome* outcome = selectedOutcome();
  if (outcome == nullptr) {
    g.setFont(type::body());
    g.setColour(colour::muted);
    g.drawText("Nothing was imported.", inner.removeFromTop(24), juce::Justification::centredLeft, false);
    return;
  }
  {
    auto head = inner.removeFromTop(46);
    paint::chip(g, statusName(outcome->status), head.removeFromRight(88).withSizeKeepingCentre(88, 22).toFloat(),
                chipStyleOf(outcome->status));
    g.setFont(type::heading());
    g.setColour(colour::text);
    g.drawText(outcome->name, head.removeFromTop(28), juce::Justification::centredLeft, true);
    g.setFont(type::sans(12.0f));
    g.setColour(colour::muted);
    g.drawText(outcome->slot > 0 ? "Imported into " + targetName + ", slot " + slotText(outcome->slot)
                                 : juce::String("Not imported"),
               head, juce::Justification::centredLeft, true);
  }

  const auto viewport = viewportOf(Area::sectionList);
  {
    juce::Graphics::ScopedSaveState state(g);
    g.reduceClipRegion(viewport);
    for (const auto& line : sectionLines()) {
      const int top = viewport.getY() + line.top - scrollOf(Area::sectionList);
      if (top + line.height < viewport.getY() || top > viewport.getBottom())
        continue;
      if (line.heading) {
        icons::Icon icon = icons::Icon::check;
        juce::Colour accent = colour::ok;
        switch (line.kind) {
          case ReportSection::Kind::exact: break;
          case ReportSection::Kind::approximated: icon = icons::Icon::warning; accent = colour::warn; break;
          case ReportSection::Kind::needs: icon = icons::Icon::attention; accent = colour::iris; break;
          case ReportSection::Kind::notes: icon = icons::Icon::info; accent = colour::muted; break;
        }
        auto row = juce::Rectangle<int>(viewport.getX(), top, viewport.getWidth(), 26);
        icons::draw(g, icons::path(icon), row.removeFromLeft(18).toFloat().withSizeKeepingCentre(18.0f, 18.0f), accent);
        row.removeFromLeft(8);
        g.setFont(type::sans(15.0f, type::Weight::semibold));
        g.setColour(colour::text);
        g.drawText(line.title, row, juce::Justification::centredLeft, true);
        continue;
      }
      const ReportRow& row = *line.row;
      const juce::Rectangle<int> r(viewport.getX(), top, viewport.getWidth(), line.height);
      g.setColour(colour::raised);
      g.fillRoundedRectangle(r.toFloat(), size::buttonRadius);
      auto content = r.reduced(12, 6);
      // The checkbox column on the right.
      const juce::String key = outcome->presetId + "|" + row.key;
      const bool isChecked = checked.count(key) != 0;
      if (line.kind != ReportSection::Kind::notes) {
        auto right = content.removeFromRight(84);
        auto box = right.removeFromTop(20);
        drawCheckbox(g, box.removeFromLeft(16).toFloat().withSizeKeepingCentre(16.0f, 16.0f), isChecked, true);
        box.removeFromLeft(8);
        g.setFont(type::sans(13.0f));
        g.setColour(colour::muted);
        g.drawText("Checked", box, juce::Justification::centredLeft, false);
        content.removeFromRight(16);
      }
      if (line.labelWidth > 0) {
        g.setFont(type::bodyStrong());
        g.setColour(colour::text);
        g.drawText(row.label, content.removeFromLeft(line.labelWidth).withHeight(20), juce::Justification::centredLeft, true);
        content.removeFromLeft(12);
      }
      drawWrapped(g, row.detail, row.mono ? type::mono(13.0f, type::Weight::regular) : type::sans(13.0f),
                  line.labelWidth > 0 ? colour::muted : colour::text, content.withHeight(line.height - 12));
      if (row.role.isNotEmpty()) {
        const auto pick = pickBounds(line);
        g.setColour(colour::control);
        g.fillRoundedRectangle(pick.toFloat(), size::buttonRadius);
        g.setColour(colour::line);
        g.drawRoundedRectangle(pick.toFloat().reduced(0.5f), size::buttonRadius, 1.0f);
        g.setFont(type::button());
        g.setColour(colour::text);
        g.drawText("Pick a capture", pick, juce::Justification::centred, false);
      }
    }
  }

  // The footer note.
  auto footer = lower.reduced(20, 18);
  footer = footer.removeFromBottom(kFooterHeight);
  g.setColour(colour::line);
  g.fillRect(footer.getX(), footer.getY(), footer.getWidth(), 1);
  footer.removeFromTop(8);
  const int buttons = exportButton.getWidth() + openButton.getWidth() + 10 + 16;
  g.setFont(type::sans(12.0f));
  g.setColour(colour::muted);
  drawWrapped(g,
              kind == ImportKind::quadCortex
                  ? "Quad Cortex note: every mapping is unverified until checked against a real unit, so nothing counts as exact "
                    "yet."
                  : "Model files that were not found are listed above; the blocks that use them come up missing.",
              type::sans(12.0f), colour::muted, footer.withTrimmedRight(buttons));
}

}  // namespace pedaleira::ui

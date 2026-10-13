#include "LibraryPage.h"

#include "../Icons.h"
#include "../Paint.h"
#include "../Type.h"

#include <algorithm>

namespace pedaleira::ui {

namespace {

constexpr int kSidebarWidth = 260;
constexpr int kDetailWidth = 340;
constexpr int kMargin = 24;
constexpr int kRowHeight = 38;
constexpr int kRowGap = 3;
constexpr int kBarHeight = 46;
constexpr int kColumnGap = 12;
constexpr int kSetlistCardHeight = 54;
constexpr int kFactoryCardHeight = 42;

juce::String slotText(int slot) { return juce::String(slot).paddedLeft('0', 3); }

juce::String tempoText(double bpm) { return bpm > 0.0 ? juce::String(bpm, 1) + " BPM" : juce::String(); }

// A filled star for a favourite, an outline for the rest.
void drawStar(juce::Graphics& g, juce::Rectangle<float> area, bool filled) {
  const juce::Path& star = icons::path(icons::Icon::star);
  if (filled) {
    const float scale = juce::jmin(area.getWidth(), area.getHeight()) / 24.0f;
    g.setColour(colour::warn);
    g.fillPath(star, juce::AffineTransform::scale(scale).translated(area.getCentreX() - 12.0f * scale,
                                                                    area.getCentreY() - 12.0f * scale));
  }
  icons::draw(g, star, area, filled ? colour::warn : colour::muted);
}

int textWidth(const juce::Font& font, const juce::String& text) {
  return juce::roundToInt(juce::GlyphArrangement::getStringWidth(font, text));
}

}  // namespace

// ####################
// SETUP
// ####################
LibraryPage::LibraryPage(EngineClient& client) : engine(client) {
  setTitle("Library");
  setWantsKeyboardFocus(true);

  search.setTitle("Search");
  search.setTextToShowWhenEmpty("Name or tag", colour::faint);
  search.setFont(type::body());
  search.setColour(juce::TextEditor::backgroundColourId, colour::raised);
  search.setColour(juce::TextEditor::outlineColourId, colour::line);
  search.setColour(juce::TextEditor::focusedOutlineColourId, colour::iris);
  search.setColour(juce::TextEditor::textColourId, colour::text);
  search.setIndents(12, 9);
  search.addListener(this);
  addAndMakeVisible(search);

  sortBox.setChoice("Slot number");
  sortBox.onClick = [this] { sortMenu(); };
  addAndMakeVisible(sortBox);

  newSetlistButton.setIcon(&icons::path(icons::Icon::plus));
  newSetlistButton.onClick = [this] { newSetlist(); };
  addAndMakeVisible(newSetlistButton);

  setlistActions.setIcon(&icons::path(icons::Icon::chevronDown));
  setlistActions.onClick = [this] {
    if (const SetlistInfo* info = shownSetlist())
      setlistMenu(info->id, setlistActions.getBounds());
  };
  addAndMakeVisible(setlistActions);

  save.onClick = [this] {
    // Saving under the preset's own name replaces it in its slot.
    if (const Row* row = selectedRow(); row != nullptr && !row->empty() && row->entry->id == model.loadedId &&
                                        run("Could not save", "preset.save", object({{"name", row->entry->name}})))
      changed();
  };
  saveAs.onClick = [this] {
    Dialog::Options options;
    options.title = "Save as";
    options.message = "The rig as it is now is saved as a new preset in the current setlist.";
    options.confirm = "Save";
    options.withText = true;
    options.placeholder = "Preset name";
    if (const Row* row = selectedRow(); row != nullptr && !row->empty())
      options.text = row->entry->name;
    prompt.show(options, [this](bool confirmed, const juce::String& name) {
      if (confirmed && run("Could not save", "preset.save", object({{"name", name}})))
        changed();
    });
  };
  revert.onClick = [this] {
    if (const Row* row = selectedRow(); row != nullptr && !row->empty() &&
                                        run("Could not revert", "preset.loadSlot",
                                            object({{"slot", row->slot}, {"setlist", row->setlistId}})))
      changed();
  };
  loadButton.onClick = [this] {
    if (const Row* row = selectedRow(); row != nullptr && !row->empty())
      perform(Act::load, targetOf(*row), {});
  };
  for (juce::Component* c : std::initializer_list<juce::Component*>{&save, &saveAs, &revert, &loadButton})
    addChildComponent(c);

  addChildComponent(prompt);
  refresh();
}

LibraryPage::~LibraryPage() { search.removeListener(this); }

// ####################
// DATA
// ####################
void LibraryPage::refresh() {
  model = LibraryModel::read(engine);
  if (viewSetlist.isEmpty() || model.setlist(viewSetlist) == nullptr)
    viewSetlist = model.currentSetlist;
  if (model.setlist(viewSetlist) == nullptr && !model.setlists.empty())
    viewSetlist = model.setlists.front().id;
  rebuildRows();
}

const SetlistInfo* LibraryPage::shownSetlist() const {
  return inSetlistView() ? model.setlist(viewSetlist) : nullptr;
}

void LibraryPage::rebuildRows() {
  rows.clear();
  const juce::String query = search.getText().trim().toLowerCase();
  const auto matches = [&](const PresetEntry& p) {
    if (query.isEmpty())
      return true;
    return p.name.toLowerCase().contains(query) || p.tags.joinIntoString(" ").toLowerCase().contains(query);
  };

  std::vector<const PresetEntry*> shown;
  switch (view) {
    case View::setlist:
      for (const auto& p : model.presets)
        if (p.setlistId == viewSetlist && matches(p))
          shown.push_back(&p);
      break;
    case View::all:
      for (const auto& p : model.presets)
        if (!p.factory && matches(p))
          shown.push_back(&p);
      break;
    case View::favourites:
      for (const auto& p : model.presets)
        if (p.favourite && matches(p))
          shown.push_back(&p);
      break;
    case View::recent:
      for (const auto& id : model.recents)
        if (const PresetEntry* p = model.find(id); p != nullptr && matches(*p))
          shown.push_back(p);
      break;
  }

  // The recent view is in recency order whatever the sort says.
  Sort effective = view == View::recent ? Sort::recent : sort;
  const auto recency = [&](const PresetEntry* p) {
    const int at = model.recents.indexOf(p->id);
    return at < 0 ? 100000 : at;
  };
  switch (effective) {
    case Sort::slot: break;
    case Sort::name:
      std::stable_sort(shown.begin(), shown.end(), [](const PresetEntry* a, const PresetEntry* b) {
        return a->name.compareNatural(b->name) < 0;
      });
      break;
    case Sort::tempo:
      std::stable_sort(shown.begin(), shown.end(), [](const PresetEntry* a, const PresetEntry* b) {
        if ((a->tempo > 0.0) != (b->tempo > 0.0))
          return a->tempo > 0.0;
        return a->tempo < b->tempo;
      });
      break;
    case Sort::recent:
      std::stable_sort(shown.begin(), shown.end(),
                       [&](const PresetEntry* a, const PresetEntry* b) { return recency(a) < recency(b); });
      break;
  }

  // A setlist in slot order shows its empty slots too, up to the last
  // one in use and one more, so the next free slot is there to save into.
  const SetlistInfo* info = shownSetlist();
  const bool withEmpties = info != nullptr && !info->readOnly && query.isEmpty() && effective == Sort::slot;
  if (withEmpties) {
    int last = 0;
    for (const PresetEntry* p : shown)
      last = juce::jmax(last, p->slot);
    const int upTo = juce::jmin(info->capacity, last + 1);
    size_t next = 0;
    for (int slot = 1; slot <= upTo; ++slot) {
      Row row;
      row.setlistId = info->id;
      row.slot = slot;
      if (next < shown.size() && shown[next]->slot == slot)
        row.entry = shown[next++];
      rows.push_back(row);
    }
  } else {
    for (const PresetEntry* p : shown) {
      Row row;
      row.entry = p;
      row.setlistId = p->setlistId;
      row.slot = p->slot;
      rows.push_back(row);
    }
  }

  // The selection stays when its row is still there.
  if (selectedIndex() < 0) {
    selectedSetlist.clear();
    selectedSlot = 0;
  }
  const int contentMax = juce::jmax(0, contentHeight() - tableViewport().getHeight());
  tableScroll = juce::jlimit(0, contentMax, tableScroll);
  updatePreview();
  resized();
  repaint();
}

int LibraryPage::selectedIndex() const {
  if (selectedSlot <= 0)
    return -1;
  for (size_t i = 0; i < rows.size(); ++i)
    if (rows[i].setlistId == selectedSetlist && rows[i].slot == selectedSlot)
      return static_cast<int>(i);
  return -1;
}

const LibraryPage::Row* LibraryPage::selectedRow() const {
  const int index = selectedIndex();
  return index < 0 ? nullptr : &rows[static_cast<size_t>(index)];
}

LibraryPage::Target LibraryPage::targetOf(const Row& row) const {
  Target t;
  t.setlistId = row.setlistId;
  t.slot = row.slot;
  if (row.entry != nullptr) {
    t.id = row.entry->id;
    t.name = row.entry->name;
    t.factory = row.entry->factory;
  }
  return t;
}

void LibraryPage::select(const juce::String& setlistId, int slot) {
  selectedSetlist = setlistId;
  selectedSlot = slot;
  updatePreview();
  const int index = selectedIndex();
  if (index >= 0)
    ensureVisible(index);
  resized();
  repaint();
}

void LibraryPage::updatePreview() {
  preview = {};
  const Row* row = selectedRow();
  if (row == nullptr || row->empty())
    return;
  if (row->entry->id == model.loadedId)
    preview = PresetPreview::fromRig(engine);
  else if (row->entry->path.isNotEmpty())
    preview = PresetPreview::fromFile(row->entry->path);
}

void LibraryPage::changed() {
  refresh();
  if (onChanged)
    onChanged();
}

// ####################
// TESTBED HOOKS
// ####################
void LibraryPage::showSetlist(const juce::String& setlistId) {
  view = View::setlist;
  viewSetlist = setlistId;
  tableScroll = 0;
  selectedSetlist.clear();
  selectedSlot = 0;
  rebuildRows();
}

void LibraryPage::showView(View newView) {
  view = newView;
  tableScroll = 0;
  selectedSetlist.clear();
  selectedSlot = 0;
  rebuildRows();
}

void LibraryPage::setSearch(const juce::String& text) { search.setText(text, true); }

void LibraryPage::setSort(Sort newSort) {
  sort = newSort;
  const char* names[] = {"Slot number", "Name", "Tempo", "Recently used"};
  sortBox.setChoice(names[static_cast<int>(newSort)]);
  tableScroll = 0;
  rebuildRows();
}

void LibraryPage::selectSlot(int slot) { select(viewSetlist, slot); }

void LibraryPage::selectRow(int index) {
  if (index >= 0 && index < static_cast<int>(rows.size()))
    select(rows[static_cast<size_t>(index)].setlistId, rows[static_cast<size_t>(index)].slot);
}

juce::StringArray LibraryPage::setlistIds() const {
  juce::StringArray ids;
  for (const auto& s : model.setlists)
    ids.add(s.id);
  return ids;
}

juce::StringArray LibraryPage::rowIds() const {
  juce::StringArray ids;
  for (const auto& row : rows)
    ids.add(row.empty() ? juce::String() : row.entry->id);
  return ids;
}

bool LibraryPage::pressAction(const juce::String& label) {
  const Row* row = selectedRow();
  if (row == nullptr)
    return false;
  for (const auto& button : actionsFor(*row))
    if (button.label == label) {
      perform(button.act, targetOf(*row), button.bounds);
      return true;
    }
  return false;
}

void LibraryPage::textEditorTextChanged(juce::TextEditor&) {
  tableScroll = 0;
  rebuildRows();
}

// ####################
// ACTIONS
// ####################
std::vector<LibraryPage::ActionButton> LibraryPage::actionsFor(const Row& row) const {
  std::vector<ActionButton> buttons;
  if (row.empty()) {
    const SetlistInfo* info = model.setlist(row.setlistId);
    if (info != nullptr && !info->readOnly)
      buttons.push_back({"Save current rig here", Act::saveHere, false, {}});
  } else {
    const bool factory = row.entry->factory;
    buttons.push_back({"Load", Act::load, false, {}});
    if (!factory)
      buttons.push_back({"Rename", Act::rename, false, {}});
    if (!factory)
      buttons.push_back({"Duplicate", Act::duplicate, false, {}});
    if (!factory && inSetlistView())
      buttons.push_back({"Move", Act::move, false, {}});
    buttons.push_back({"Copy to setlist", Act::copyTo, false, {}});
    if (!factory)
      buttons.push_back({"Delete", Act::remove, true, {}});
  }
  // Placed in the selected row's bar.
  const int index = selectedIndex();
  if (index >= 0) {
    const int y = tableViewport().getY() + rowTop(index) - tableScroll + kRowHeight + 8;
    int x = tableViewport().getX() + 12;
    for (auto& button : buttons) {
      const int width = textWidth(type::button(), button.label) + 24;
      button.bounds = {x, y, width, 30};
      x += width + 8;
    }
  }
  return buttons;
}

void LibraryPage::fail(const juce::String& title, const EngineReply& reply) {
  Dialog::Options options;
  options.title = title;
  options.message = reply.errorMessage.isNotEmpty() ? reply.errorMessage : reply.errorCode;
  options.cancelable = false;
  prompt.show(options, [](bool, const juce::String&) {});
}

bool LibraryPage::run(const juce::String& title, const juce::String& command, const juce::var& args,
                      EngineReply* reply) {
  EngineReply result = engine.run(command, args);
  if (!result.ok) {
    fail(title, result);
    return false;
  }
  if (reply != nullptr)
    *reply = result;
  return true;
}

void LibraryPage::perform(Act act, const Target& target, juce::Rectangle<int> anchor) {
  switch (act) {
    case Act::load:
      if (run("Could not load " + target.name, "preset.loadSlot",
              object({{"slot", target.slot}, {"setlist", target.setlistId}})))
        changed();
      break;
    case Act::rename: {
      Dialog::Options options;
      options.title = "Rename preset";
      options.confirm = "Rename";
      options.withText = true;
      options.text = target.name;
      options.placeholder = "Preset name";
      prompt.show(options, [this, target](bool confirmed, const juce::String& name) {
        if (confirmed && run("Could not rename", "preset.rename", object({{"id", target.id}, {"name", name}})))
          changed();
      });
      break;
    }
    case Act::duplicate: {
      EngineReply reply;
      if (run("Could not duplicate", "preset.duplicateSlot",
              object({{"slot", target.slot}, {"setlist", target.setlistId}}), &reply)) {
        const int made = reply.value["preset"]["slot"];
        refresh();
        if (made > 0)
          select(target.setlistId, made);
        if (onChanged)
          onChanged();
      }
      break;
    }
    case Act::move: moveMenu(target, anchor); break;
    case Act::copyTo: copyMenu(target, anchor); break;
    case Act::remove: {
      Dialog::Options options;
      options.title = "Delete " + target.name + "?";
      options.message = "The preset's file goes to the system trash and its slot is left empty. "
                        "What is loaded now stays as it is.";
      options.confirm = "Delete";
      prompt.show(options, [this, target](bool confirmed, const juce::String&) {
        if (confirmed && run("Could not delete", "preset.delete", object({{"id", target.id}})))
          changed();
      });
      break;
    }
    case Act::saveHere: {
      Dialog::Options options;
      options.title = "Save to slot " + slotText(target.slot);
      options.message = "The rig as it is now is saved into this empty slot.";
      options.confirm = "Save";
      options.withText = true;
      options.placeholder = "Preset name";
      prompt.show(options, [this, target](bool confirmed, const juce::String& name) {
        if (!confirmed)
          return;
        if (run("Could not save", "preset.saveToSlot",
                object({{"slot", target.slot}, {"setlist", target.setlistId}, {"name", name}}))) {
          refresh();
          select(target.setlistId, target.slot);
          if (onChanged)
            onChanged();
        }
      });
      break;
    }
  }
}

void LibraryPage::movePreset(const Target& target, int to) {
  if (run("Could not move", "preset.moveSlot", object({{"from", target.slot}, {"to", to}, {"setlist", target.setlistId}}))) {
    refresh();
    select(target.setlistId, to);
    if (onChanged)
      onChanged();
  }
}

void LibraryPage::copyPreset(const Target& target, const juce::String& to) {
  if (run("Could not copy", "preset.copyToSetlist", object({{"slot", target.slot}, {"setlist", target.setlistId}, {"to", to}})))
    changed();
}

void LibraryPage::moveSelected(int to) {
  if (const Row* row = selectedRow(); row != nullptr && !row->empty())
    movePreset(targetOf(*row), to);
}

void LibraryPage::copySelectedTo(const juce::String& setlistId) {
  if (const Row* row = selectedRow(); row != nullptr && !row->empty())
    copyPreset(targetOf(*row), setlistId);
}

void LibraryPage::moveMenu(const Target& target, juce::Rectangle<int> anchor) {
  juce::PopupMenu menu;
  juce::Component::SafePointer<LibraryPage> self(this);
  const auto moveTo = [self, target](int to) {
    if (self != nullptr)
      self->movePreset(target, to);
  };
  menu.addItem("Up one slot", target.slot > 1, false, [moveTo, target] { moveTo(target.slot - 1); });
  menu.addItem("Down one slot", target.slot < 256, false, [moveTo, target] { moveTo(target.slot + 1); });
  menu.addItem("To a slot number...", true, false, [self, target, moveTo] {
    if (self == nullptr)
      return;
    Dialog::Options options;
    options.title = "Move " + target.name;
    options.message = "The slot to move it to, 1 to 256. The presets in between shift by one.";
    options.confirm = "Move";
    options.withText = true;
    options.placeholder = "Slot number";
    self->prompt.show(options, [moveTo](bool confirmed, const juce::String& text) {
      const int slot = text.getIntValue();
      if (confirmed && slot >= 1 && slot <= 256)
        moveTo(slot);
    });
  });
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(localAreaToGlobal(anchor)));
}

void LibraryPage::copyMenu(const Target& target, juce::Rectangle<int> anchor) {
  juce::PopupMenu menu;
  juce::Component::SafePointer<LibraryPage> self(this);
  int others = 0;
  for (const auto& setlist : model.setlists) {
    if (setlist.readOnly || setlist.id == target.setlistId)
      continue;
    ++others;
    const juce::String to = setlist.id;
    menu.addItem(setlist.name, true, false, [self, target, to] {
      if (self != nullptr)
        self->copyPreset(target, to);
    });
  }
  if (others == 0)
    menu.addItem("No other setlist", false, false, nullptr);
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(localAreaToGlobal(anchor)));
}

void LibraryPage::useSetlist(const juce::String& setlistId) {
  if (run("Could not select the setlist", "setlist.select", object({{"id", setlistId}})))
    changed();
}

void LibraryPage::reorderSetlist(const juce::String& setlistId, int index) {
  if (run("Could not move the setlist", "setlist.reorder", object({{"id", setlistId}, {"index", index}})))
    changed();
}

void LibraryPage::renameSetlistDialog(const juce::String& setlistId) {
  const SetlistInfo* info = model.setlist(setlistId);
  if (info == nullptr || info->readOnly)
    return;
  Dialog::Options options;
  options.title = "Rename setlist";
  options.confirm = "Rename";
  options.withText = true;
  options.text = info->name;
  options.placeholder = "Setlist name";
  prompt.show(options, [this, setlistId](bool confirmed, const juce::String& text) {
    if (confirmed && run("Could not rename", "setlist.rename", object({{"id", setlistId}, {"name", text}})))
      changed();
  });
}

void LibraryPage::deleteSetlistDialog(const juce::String& setlistId) {
  const SetlistInfo* info = model.setlist(setlistId);
  if (info == nullptr || info->readOnly)
    return;
  Dialog::Options options;
  options.title = "Delete " + info->name + "?";
  options.message = "The setlist's folder, with all its presets, goes to the system trash. Model files in the library stay.";
  options.confirm = "Delete";
  prompt.show(options, [this, setlistId](bool confirmed, const juce::String&) {
    if (confirmed && run("Could not delete", "setlist.delete", object({{"id", setlistId}}))) {
      viewSetlist.clear();
      selectedSlot = 0;
      changed();
    }
  });
}

void LibraryPage::setlistMenu(const juce::String& setlistId, juce::Rectangle<int> anchor) {
  const SetlistInfo* info = model.setlist(setlistId);
  if (info == nullptr)
    return;
  int index = 0, userCount = 0;
  for (size_t i = 0; i < model.setlists.size(); ++i) {
    if (model.setlists[i].id == setlistId)
      index = static_cast<int>(i);
    if (!model.setlists[i].readOnly)
      ++userCount;
  }
  const bool editable = !info->readOnly;
  juce::PopupMenu menu;
  juce::Component::SafePointer<LibraryPage> self(this);
  menu.addItem("Use for the footswitches", setlistId != model.currentSetlist, false, [self, setlistId] {
    if (self != nullptr)
      self->useSetlist(setlistId);
  });
  menu.addSeparator();
  menu.addItem("Rename...", editable, false, [self, setlistId] {
    if (self != nullptr)
      self->renameSetlistDialog(setlistId);
  });
  menu.addItem("Move up", editable && index > 0, false, [self, setlistId, index] {
    if (self != nullptr)
      self->reorderSetlist(setlistId, index - 1);
  });
  menu.addItem("Move down", editable && index < userCount - 1, false, [self, setlistId, index] {
    if (self != nullptr)
      self->reorderSetlist(setlistId, index + 1);
  });
  menu.addSeparator();
  menu.addItem("Delete...", editable, false, [self, setlistId] {
    if (self != nullptr)
      self->deleteSetlistDialog(setlistId);
  });
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(localAreaToGlobal(anchor)));
}

void LibraryPage::newSetlist() {
  Dialog::Options options;
  options.title = "New setlist";
  options.message = "An empty setlist of up to 256 presets.";
  options.confirm = "Create";
  options.withText = true;
  options.placeholder = "Setlist name";
  prompt.show(options, [this](bool confirmed, const juce::String& name) {
    EngineReply reply;
    if (!confirmed || !run("Could not create the setlist", "setlist.create", object({{"name", name}}), &reply))
      return;
    const juce::String id = reply.value["id"].toString();
    refresh();
    if (id.isNotEmpty())
      showSetlist(id);
    if (onChanged)
      onChanged();
  });
}

void LibraryPage::toggleFavourite(const Row& row) {
  if (row.empty())
    return;
  if (run("Could not change the favourite", "favourite.set",
          object({{"id", row.entry->id}, {"favourite", !row.entry->favourite}}))) {
    refresh();
    if (onChanged)
      onChanged();
  }
}

void LibraryPage::sortMenu() {
  juce::PopupMenu menu;
  juce::Component::SafePointer<LibraryPage> self(this);
  const char* names[] = {"Slot number", "Name", "Tempo", "Recently used"};
  for (int i = 0; i < 4; ++i)
    menu.addItem(names[i], true, static_cast<int>(sort) == i, [self, i] {
      if (self != nullptr)
        self->setSort(static_cast<Sort>(i));
    });
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&sortBox));
}

// ####################
// GEOMETRY
// ####################
juce::Rectangle<int> LibraryPage::sidebarArea() const { return {0, 0, kSidebarWidth, getHeight()}; }
juce::Rectangle<int> LibraryPage::detailArea() const {
  return {getWidth() - kDetailWidth, 0, kDetailWidth, getHeight()};
}
juce::Rectangle<int> LibraryPage::mainArea() const {
  return juce::Rectangle<int>(kSidebarWidth, 0, getWidth() - kSidebarWidth - kDetailWidth, getHeight()).reduced(kMargin);
}

juce::Rectangle<int> LibraryPage::libraryItemBounds(int index) const {
  return {16, 46 + index * 42, kSidebarWidth - 32, 36};
}

juce::Rectangle<int> LibraryPage::setlistsViewport() const {
  return {16, 214, kSidebarWidth - 32, getHeight() - 214};
}

juce::Rectangle<int> LibraryPage::setlistBounds(int index) const {
  int y = setlistsViewport().getY() + 4 - setlistScroll;
  for (int i = 0; i < index; ++i)
    y += (model.setlists[static_cast<size_t>(i)].readOnly ? kFactoryCardHeight : kSetlistCardHeight) + 8;
  const int height = model.setlists[static_cast<size_t>(index)].readOnly ? kFactoryCardHeight : kSetlistCardHeight;
  return {setlistsViewport().getX(), y, setlistsViewport().getWidth(), height};
}

juce::Rectangle<int> LibraryPage::tableViewport() const {
  const auto main = mainArea();
  return {main.getX(), main.getY() + 56 + 16 + 28 + kRowGap, main.getWidth(),
          main.getBottom() - (main.getY() + 56 + 16 + 28 + kRowGap)};
}

int LibraryPage::rowHeight(int index) const {
  return kRowHeight + (index == selectedIndex() ? kBarHeight : 0);
}

int LibraryPage::rowTop(int index) const {
  int y = 0;
  for (int i = 0; i < index; ++i)
    y += rowHeight(i) + kRowGap;
  return y;
}

int LibraryPage::contentHeight() const { return rowTop(static_cast<int>(rows.size())); }

int LibraryPage::rowAt(juce::Point<int> position) const {
  const auto viewport = tableViewport();
  if (!viewport.contains(position))
    return -1;
  const int y = position.y - viewport.getY() + tableScroll;
  for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
    const int top = rowTop(i);
    if (y >= top && y < top + rowHeight(i))
      return i;
  }
  return -1;
}

LibraryPage::Columns LibraryPage::columnsOf(juce::Rectangle<int> row) const {
  Columns c;
  auto r = row.reduced(12, 0);
  c.dot = r.removeFromLeft(20);
  r.removeFromLeft(kColumnGap);
  c.number = r.removeFromLeft(48);
  r.removeFromLeft(kColumnGap);
  c.star = r.removeFromRight(32);
  r.removeFromRight(kColumnGap);
  c.tags = r.removeFromRight(170);
  r.removeFromRight(kColumnGap);
  c.tempo = r.removeFromRight(84);
  r.removeFromRight(kColumnGap);
  c.scenes = r.removeFromRight(56);
  r.removeFromRight(kColumnGap);
  c.name = r;
  return c;
}

juce::Rectangle<int> LibraryPage::starBounds(int index) const {
  const auto viewport = tableViewport();
  const juce::Rectangle<int> row(viewport.getX(), viewport.getY() + rowTop(index) - tableScroll, viewport.getWidth(),
                                 kRowHeight);
  return columnsOf(row).star.withSizeKeepingCentre(28, 28);
}

void LibraryPage::ensureVisible(int index) {
  const auto viewport = tableViewport();
  const int top = rowTop(index);
  const int bottom = top + rowHeight(index);
  if (top < tableScroll)
    tableScroll = top;
  else if (bottom > tableScroll + viewport.getHeight())
    tableScroll = bottom - viewport.getHeight();
  tableScroll = juce::jmax(0, tableScroll);
}

void LibraryPage::resized() {
  const auto main = mainArea();
  // Sort by and Search at the title row's right.
  sortBox.setBounds(main.getRight() - 148, main.getY() + 20, 148, 36);
  search.setBounds(sortBox.getX() - kColumnGap - 200, main.getY() + 20, 200, 36);

  // Setlist actions follow the title.
  const SetlistInfo* info = shownSetlist();
  setlistActions.setVisible(info != nullptr);
  if (info != nullptr) {
    const int titleWidth = textWidth(type::heading(), info->name);
    setlistActions.setBounds(main.getX() + titleWidth + 10, main.getY() - 1, setlistActions.idealWidth() - 4, 30);
  }

  // New setlist sits under the last card.
  int bottom = setlistsViewport().getY() + 4 - setlistScroll;
  if (!model.setlists.empty())
    bottom = setlistBounds(static_cast<int>(model.setlists.size()) - 1).getBottom() + 8;
  newSetlistButton.setBounds(setlistsViewport().getX(), bottom + 4, setlistsViewport().getWidth(), 36);
  newSetlistButton.setVisible(newSetlistButton.getBottom() <= getHeight() - 8);

  // The details' buttons at the panel's foot.
  const auto detail = detailArea().reduced(24, 20);
  const int y = detail.getBottom() - 36;
  const Row* row = selectedRow();
  const bool loaded = row != nullptr && !row->empty() && row->entry->id == model.loadedId;
  const bool user = row != nullptr && !row->empty() && !row->entry->factory;
  save.setVisible(loaded && user);
  saveAs.setVisible(loaded);
  revert.setVisible(loaded && user && model.unsaved);
  loadButton.setVisible(row != nullptr && !row->empty() && !loaded);
  int x = detail.getX();
  for (PushButton* b : {&save, &saveAs, &revert}) {
    b->setBounds(x, y, b->idealWidth() + 4, 36);
    x = b->getRight() + 8;
  }
  loadButton.setBounds(detail.getX(), y, loadButton.idealWidth() + 8, 36);
  save.setEnabled(model.unsaved);
  prompt.setBounds(getLocalBounds());
}

// ####################
// INPUT
// ####################
void LibraryPage::mouseDown(const juce::MouseEvent& event) {
  grabKeyboardFocus();
  const auto at = event.getPosition();
  // The sidebar: library views, then setlists.
  for (int i = 0; i < 3; ++i)
    if (libraryItemBounds(i).contains(at)) {
      showView(i == 0 ? View::all : i == 1 ? View::favourites : View::recent);
      return;
    }
  if (setlistsViewport().contains(at))
    for (int i = 0; i < static_cast<int>(model.setlists.size()); ++i)
      if (setlistBounds(i).contains(at)) {
        const juce::String id = model.setlists[static_cast<size_t>(i)].id;
        if (event.mods.isPopupMenu()) {
          setlistMenu(id, setlistBounds(i));
          return;
        }
        showSetlist(id);
        return;
      }

  // The table: the action bar, the star, then the row.
  const int index = rowAt(at);
  if (index < 0)
    return;
  const Row& row = rows[static_cast<size_t>(index)];
  if (index == selectedIndex()) {
    for (const auto& button : actionsFor(row))
      if (button.bounds.contains(at)) {
        perform(button.act, targetOf(row), button.bounds);
        return;
      }
  }
  if (!row.empty() && starBounds(index).contains(at)) {
    toggleFavourite(row);
    return;
  }
  select(row.setlistId, row.slot);
  if (event.mods.isPopupMenu() && selectedRow() != nullptr) {
    juce::PopupMenu menu;
    juce::Component::SafePointer<LibraryPage> self(this);
    const Row current = *selectedRow();
    const Target target = targetOf(current);
    const auto anchor = juce::Rectangle<int>(at.x, at.y, 1, 1);
    for (const auto& button : actionsFor(current)) {
      const Act act = button.act;
      menu.addItem(button.label, true, false, [self, act, target, anchor] {
        if (self != nullptr)
          self->perform(act, target, anchor);
      });
    }
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(localAreaToGlobal(anchor)));
  }
}

void LibraryPage::mouseDoubleClick(const juce::MouseEvent& event) {
  const int index = rowAt(event.getPosition());
  if (index < 0 || rows[static_cast<size_t>(index)].empty() || starBounds(index).contains(event.getPosition()))
    return;
  const Target target = targetOf(rows[static_cast<size_t>(index)]);
  if (run("Could not load " + target.name, "preset.loadSlot",
          object({{"slot", target.slot}, {"setlist", target.setlistId}}))) {
    changed();
    if (onOpenGrid)
      onOpenGrid();
  }
}

void LibraryPage::mouseMove(const juce::MouseEvent& event) {
  const int before = hoverAction;
  hoverAction = -1;
  if (const Row* row = selectedRow()) {
    const auto buttons = actionsFor(*row);
    for (size_t i = 0; i < buttons.size(); ++i)
      if (buttons[i].bounds.contains(event.getPosition()))
        hoverAction = static_cast<int>(i);
  }
  if (hoverAction != before)
    repaint(tableViewport());
}

void LibraryPage::mouseExit(const juce::MouseEvent&) {
  if (hoverAction >= 0) {
    hoverAction = -1;
    repaint(tableViewport());
  }
}

void LibraryPage::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) {
  const int delta = juce::roundToInt(wheel.deltaY * 200.0f);
  if (event.getPosition().x < kSidebarWidth) {
    int content = 4;
    for (const auto& s : model.setlists)
      content += (s.readOnly ? kFactoryCardHeight : kSetlistCardHeight) + 8;
    content += 48;
    setlistScroll = juce::jlimit(0, juce::jmax(0, content - setlistsViewport().getHeight()), setlistScroll - delta);
    resized();
    repaint(sidebarArea());
    return;
  }
  const int maxScroll = juce::jmax(0, contentHeight() - tableViewport().getHeight());
  tableScroll = juce::jlimit(0, maxScroll, tableScroll - delta);
  repaint(tableViewport());
}

bool LibraryPage::keyPressed(const juce::KeyPress& key) {
  if (rows.empty())
    return false;
  const int index = selectedIndex();
  if (key == juce::KeyPress::downKey || key == juce::KeyPress::upKey) {
    const int next = juce::jlimit(0, static_cast<int>(rows.size()) - 1,
                                  index < 0 ? 0 : index + (key == juce::KeyPress::downKey ? 1 : -1));
    selectRow(next);
    return true;
  }
  if (index < 0)
    return false;
  const Row& row = rows[static_cast<size_t>(index)];
  if (key == juce::KeyPress::returnKey) {
    perform(row.empty() ? Act::saveHere : Act::load, targetOf(row), {});
    return true;
  }
  if ((key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) && !row.empty() && !row.entry->factory) {
    perform(Act::remove, targetOf(row), {});
    return true;
  }
  return false;
}

// ####################
// PAINT
// ####################
void LibraryPage::paint(juce::Graphics& g) {
  g.fillAll(colour::ground);
  paintSidebar(g);
  paintMain(g);
  paintDetails(g);
}

void LibraryPage::paintSidebar(juce::Graphics& g) {
  g.setColour(colour::surface);
  g.fillRect(sidebarArea());
  g.setColour(colour::line);
  g.fillRect(kSidebarWidth - 1, 0, 1, getHeight());

  paint::label(g, "Library", {26, 20, 200, 16});
  struct Item {
    const char* name;
    icons::Icon icon;
    int count;
    View target;
  };
  const Item items[] = {{"All presets", icons::Icon::library, model.userPresetCount(), View::all},
                        {"Favourites", icons::Icon::star, model.favouriteCount(), View::favourites},
                        {"Recent", icons::Icon::live, model.recents.size(), View::recent}};
  for (int i = 0; i < 3; ++i) {
    const auto r = libraryItemBounds(i);
    if (view == items[i].target) {
      g.setColour(colour::control);
      g.fillRoundedRectangle(r.toFloat(), size::buttonRadius);
    }
    auto inner = r.reduced(10, 0);
    icons::draw(g, icons::path(items[i].icon), inner.removeFromLeft(18).toFloat().withSizeKeepingCentre(18.0f, 18.0f),
                colour::muted);
    inner.removeFromLeft(10);
    g.setFont(type::mono(12.0f, type::Weight::regular));
    g.setColour(colour::muted);
    g.drawText(juce::String(items[i].count), inner, juce::Justification::centredRight, false);
    g.setFont(type::bodyStrong());
    g.setColour(colour::text);
    g.drawText(items[i].name, inner, juce::Justification::centredLeft, true);
  }

  paint::label(g, "Setlists", {26, 190, 200, 16});
  juce::Graphics::ScopedSaveState state(g);
  g.reduceClipRegion(setlistsViewport());
  for (int i = 0; i < static_cast<int>(model.setlists.size()); ++i) {
    const SetlistInfo& setlist = model.setlists[static_cast<size_t>(i)];
    const auto r = setlistBounds(i).toFloat();
    const bool shown = inSetlistView() && setlist.id == viewSetlist;
    g.setColour(shown ? colour::control : colour::raised);
    g.fillRoundedRectangle(r, size::panelRadius);
    g.setColour(shown ? colour::iris : colour::line);
    g.drawRoundedRectangle(r.reduced(0.5f), size::panelRadius, 1.0f);
    auto inner = r.toNearestInt().reduced(12, 10);
    auto top = inner.removeFromTop(20);
    const juce::String count = setlist.readOnly ? juce::String(setlist.count)
                                                : juce::String(setlist.count) + " / " + juce::String(setlist.capacity);
    g.setFont(type::mono(12.0f));
    g.setColour(colour::muted);
    const int countWidth = textWidth(type::mono(12.0f), count);
    g.drawText(count, top.removeFromRight(countWidth + 2), juce::Justification::centredRight, false);
    if (setlist.id == model.currentSetlist) {
      // The setlist the footswitches follow.
      g.setColour(colour::iris);
      g.fillEllipse(top.removeFromRight(14).toFloat().withSizeKeepingCentre(6.0f, 6.0f));
    }
    g.setFont(type::bodyStrong());
    g.setColour(colour::text);
    g.drawText(setlist.name, top, juce::Justification::centredLeft, true);
    if (!setlist.readOnly) {
      inner.removeFromTop(8);
      const auto bar = inner.removeFromTop(4).toFloat();
      g.setColour(colour::line);
      g.fillRoundedRectangle(bar, 2.0f);
      g.setColour(shown ? colour::iris : colour::muted);
      g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * juce::jlimit(0.0f, 1.0f, static_cast<float>(setlist.count) /
                                                                                         static_cast<float>(setlist.capacity))),
                             2.0f);
    }
  }
}

void LibraryPage::paintMain(juce::Graphics& g) {
  const auto main = mainArea();
  // Title and how many.
  juce::String title, subtitle;
  if (const SetlistInfo* info = shownSetlist()) {
    title = info->name;
    subtitle = info->readOnly ? juce::String(info->count) + " factory presets, read only"
                              : juce::String(info->count) + " of " + juce::String(info->capacity) + " presets";
  } else {
    title = view == View::all ? "All presets" : view == View::favourites ? "Favourites" : "Recent";
    int presets = 0;
    for (const auto& row : rows)
      if (!row.empty())
        ++presets;
    subtitle = juce::String(presets) + (presets == 1 ? " preset" : " presets");
  }
  g.setFont(type::heading());
  g.setColour(colour::text);
  g.drawText(title, main.getX(), main.getY(), search.getX() - main.getX() - 12, 28, juce::Justification::centredLeft, true);
  g.setFont(type::body());
  g.setColour(colour::muted);
  g.drawText(subtitle, main.getX(), main.getY() + 28, search.getX() - main.getX() - 12, 20,
             juce::Justification::centredLeft, true);
  paint::label(g, "Search", {search.getX(), main.getY(), 200, 16});
  paint::label(g, "Sort by", {sortBox.getX(), main.getY(), 148, 16});

  // Column titles.
  const auto viewport = tableViewport();
  const juce::Rectangle<int> head(viewport.getX(), viewport.getY() - kRowGap - 28, viewport.getWidth(), 28);
  const Columns c = columnsOf(head);
  paint::label(g, "#", c.number);
  paint::label(g, "Name", c.name);
  paint::label(g, "Scenes", c.scenes);
  paint::label(g, "Tempo", c.tempo);
  paint::label(g, inSetlistView() ? "Tags" : "Setlist", c.tags);

  {
    juce::Graphics::ScopedSaveState state(g);
    g.reduceClipRegion(viewport);
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
      const int top = viewport.getY() + rowTop(i) - tableScroll;
      if (top + rowHeight(i) < viewport.getY() || top > viewport.getBottom())
        continue;
      paintRow(g, i);
    }
  }
  if (rows.empty()) {
    g.setFont(type::body());
    g.setColour(colour::muted);
    juce::String note = "Nothing here yet.";
    if (search.getText().isNotEmpty())
      note = "Nothing matches the search.";
    else if (view == View::favourites)
      note = "Mark a preset with the star to keep it here.";
    else if (view == View::recent)
      note = "Presets you load show up here.";
    else if (view == View::all)
      note = "No preset is saved yet. Save the rig from the grid, or import presets.";
    else if (const SetlistInfo* info = shownSetlist(); info != nullptr && info->readOnly)
      note = "No factory presets are installed.";
    else if (model.userPresetCount() == 0 && std::none_of(model.setlists.begin(), model.setlists.end(),
                                                         [](const SetlistInfo& s) { return !s.readOnly; }))
      note = "No setlist yet. Make one with New setlist, then save the rig into it or import presets.";
    else if (info != nullptr)
      note = "This setlist is empty. Save the rig into it from the grid.";
    g.drawText(note, viewport.withHeight(24).translated(0, 16), juce::Justification::centredLeft, false);
  }
}

void LibraryPage::paintRow(juce::Graphics& g, int index) {
  const Row& row = rows[static_cast<size_t>(index)];
  const auto viewport = tableViewport();
  const int top = viewport.getY() + rowTop(index) - tableScroll;
  const juce::Rectangle<int> full(viewport.getX(), top, viewport.getWidth(), rowHeight(index));
  const juce::Rectangle<int> line(viewport.getX(), top, viewport.getWidth(), kRowHeight);
  const bool selected = index == selectedIndex();
  const bool loaded = !row.empty() && row.entry->id == model.loadedId;
  const bool unsaved = loaded && model.unsaved;
  const Columns c = columnsOf(line);

  const auto box = full.toFloat();
  if (row.empty()) {
    if (selected) {
      g.setColour(colour::raised);
      g.fillRoundedRectangle(box, size::buttonRadius);
    }
    paint::dashedOutline(g, box, size::buttonRadius, selected ? colour::muted : colour::faint);
  } else {
    g.setColour(loaded || selected ? colour::raised : colour::surface);
    g.fillRoundedRectangle(box, size::buttonRadius);
    g.setColour(loaded ? colour::iris : selected ? colour::muted : colour::line);
    g.drawRoundedRectangle(box.reduced(0.5f), size::buttonRadius, 1.0f);
  }

  if (loaded) {
    g.setColour(colour::iris);
    g.fillEllipse(c.dot.toFloat().withSizeKeepingCentre(10.0f, 10.0f));
  }
  g.setFont(type::value());
  g.setColour(colour::muted);
  g.drawText(slotText(row.slot), c.number, juce::Justification::centredLeft, false);

  if (row.empty()) {
    g.setFont(type::body());
    g.drawText("Empty", c.name, juce::Justification::centredLeft, false);
  } else {
    const PresetEntry& p = *row.entry;
    const juce::Font nameFont = type::sans(14.0f, unsaved ? type::Weight::medium : type::Weight::regular, unsaved);
    auto name = c.name;
    if (unsaved) {
      const auto pill = name.removeFromRight(62).toFloat().withSizeKeepingCentre(58.0f, 20.0f);
      g.setColour(colour::warn);
      g.drawRoundedRectangle(pill.reduced(0.5f), 10.0f, 1.0f);
      g.setFont(type::sans(11.0f, type::Weight::semibold));
      g.drawText("Unsaved", pill, juce::Justification::centred, false);
    }
    g.setFont(nameFont);
    g.setColour(colour::text);
    g.drawText(p.name, name, juce::Justification::centredLeft, true);

    g.setFont(type::value());
    g.setColour(colour::text);
    g.drawText(p.sceneCount > 0 ? juce::String(p.sceneCount) : juce::String(), c.scenes, juce::Justification::centredLeft,
               false);
    g.drawText(tempoText(p.tempo), c.tempo, juce::Justification::centredLeft, false);

    if (inSetlistView()) {
      int x = c.tags.getX();
      for (const auto& tag : p.tags) {
        const juce::Font tagFont = type::sans(12.0f, type::Weight::medium);
        const int width = textWidth(tagFont, tag) + 16;
        if (x + width > c.tags.getRight())
          break;
        const juce::Rectangle<float> chip(static_cast<float>(x), static_cast<float>(c.tags.getCentreY() - 11),
                                          static_cast<float>(width), 22.0f);
        g.setColour(colour::control);
        g.fillRoundedRectangle(chip, 6.0f);
        g.setFont(tagFont);
        g.setColour(colour::muted);
        g.drawText(tag, chip, juce::Justification::centred, false);
        x += width + 6;
      }
    } else {
      g.setFont(type::sans(12.0f));
      g.setColour(colour::muted);
      g.drawText(p.setlistName, c.tags, juce::Justification::centredLeft, true);
    }
    drawStar(g, c.star.toFloat().withSizeKeepingCentre(18.0f, 18.0f), p.favourite);
  }

  if (selected) {
    g.setColour(colour::line);
    g.fillRect(full.getX() + 1, full.getY() + kRowHeight, full.getWidth() - 2, 1);
    const auto buttons = actionsFor(row);
    for (size_t i = 0; i < buttons.size(); ++i) {
      const auto& button = buttons[i];
      if (static_cast<int>(i) == hoverAction) {
        g.setColour(colour::control);
        g.fillRoundedRectangle(button.bounds.toFloat(), size::buttonRadius);
      }
      g.setFont(type::button());
      g.setColour(button.muted ? colour::muted : colour::text);
      g.drawText(button.label, button.bounds, juce::Justification::centred, false);
    }
  }
}

void LibraryPage::paintDetails(juce::Graphics& g) {
  const auto area = detailArea();
  g.setColour(colour::surface);
  g.fillRect(area);
  g.setColour(colour::line);
  g.fillRect(area.getX(), 0, 1, getHeight());

  auto inner = area.reduced(24, 24).withTrimmedBottom(-4);
  const Row* row = selectedRow();
  if (row == nullptr) {
    g.setFont(type::body());
    g.setColour(colour::muted);
    g.drawText("Choose a preset to see what is in it.", inner.removeFromTop(24), juce::Justification::centredLeft, false);
    return;
  }
  const SetlistInfo* info = model.setlist(row->setlistId);
  const juce::String where = (info != nullptr ? info->name : juce::String()) + " " + type::dot() + " " + slotText(row->slot);
  paint::label(g, where, inner.removeFromTop(16));
  inner.removeFromTop(4);

  if (row->empty()) {
    g.setFont(type::sans(24.0f, type::Weight::regular));
    g.setColour(colour::muted);
    g.drawText("Empty slot", inner.removeFromTop(32), juce::Justification::centredLeft, false);
    inner.removeFromTop(10);
    g.setFont(type::body());
    g.setColour(colour::muted);
    juce::AttributedString text;
    text.append(info != nullptr && info->readOnly
                    ? "Factory presets are read only."
                    : "Save the rig as it is now into this slot.",
                type::body(), colour::muted);
    text.draw(g, inner.removeFromTop(60).toFloat());
    return;
  }

  const PresetEntry& p = *row->entry;
  const bool loaded = p.id == model.loadedId;
  const bool unsaved = loaded && model.unsaved;
  {
    auto head = inner.removeFromTop(32);
    const juce::Font nameFont = type::sans(24.0f, unsaved ? type::Weight::medium : type::Weight::regular, unsaved);
    const int nameWidth = juce::jmin(head.getWidth() - (unsaved ? 74 : 0), textWidth(nameFont, p.name) + 2);
    g.setFont(nameFont);
    g.setColour(colour::text);
    g.drawText(p.name, head.removeFromLeft(nameWidth), juce::Justification::centredLeft, true);
    if (unsaved) {
      head.removeFromLeft(10);
      const auto pill = head.removeFromLeft(58).toFloat().withSizeKeepingCentre(58.0f, 20.0f);
      g.setColour(colour::warn);
      g.drawRoundedRectangle(pill.reduced(0.5f), 10.0f, 1.0f);
      g.setFont(type::sans(11.0f, type::Weight::semibold));
      g.drawText("Unsaved", pill, juce::Justification::centred, false);
    }
  }
  inner.removeFromTop(18);

  // The grid as thumbnails.
  const auto thumb = inner.removeFromTop(156);
  paint::panel(g, thumb.toFloat(), colour::ground);
  if (preview.available) {
    const auto cells = thumb.reduced(12);
    const float cell = (static_cast<float>(cells.getWidth()) - 7.0f * 4.0f) / 8.0f;
    for (int r = 0; r < 4; ++r)
      for (int s = 0; s < 8; ++s) {
        const juce::Rectangle<float> box(cells.getX() + s * (cell + 4.0f), cells.getY() + r * (cell + 4.0f), cell, cell);
        if (const auto& category = preview.cells[static_cast<size_t>(r)][static_cast<size_t>(s)]) {
          g.setColour(categoryColour(*category));
          g.fillRoundedRectangle(box, 4.0f);
        } else {
          paint::dashedOutline(g, box, 4.0f, colour::line);
        }
      }
  } else {
    g.setFont(type::compact());
    g.setColour(colour::muted);
    g.drawText(p.factory ? "Load the preset to see its grid." : "The grid could not be read.", thumb.reduced(16),
               juce::Justification::centred, true);
  }
  inner.removeFromTop(18);

  // Scenes.
  paint::label(g, "Scenes", inner.removeFromTop(16));
  inner.removeFromTop(8);
  if (preview.available && !preview.sceneKeys.isEmpty()) {
    auto grid = inner.removeFromTop(36 * 2 + 8);
    const int cellWidth = (grid.getWidth() - 8) / 2;
    for (int i = 0; i < juce::jmin(4, preview.sceneKeys.size()); ++i) {
      const juce::Rectangle<float> box(static_cast<float>(grid.getX() + (i % 2) * (cellWidth + 8)),
                                       static_cast<float>(grid.getY() + (i / 2) * 44), static_cast<float>(cellWidth), 36.0f);
      g.setColour(colour::raised);
      g.fillRoundedRectangle(box, size::buttonRadius);
      g.setColour(colour::line);
      g.drawRoundedRectangle(box.reduced(0.5f), size::buttonRadius, 1.0f);
      auto text = box.toNearestInt().reduced(10, 0);
      g.setFont(type::mono(13.0f));
      g.setColour(colour::text);
      g.drawText(preview.sceneKeys[i], text.removeFromLeft(14), juce::Justification::centredLeft, false);
      text.removeFromLeft(8);
      g.setFont(type::sans(13.0f, type::Weight::semibold));
      g.drawText(preview.sceneNames[i], text, juce::Justification::centredLeft, true);
    }
    if (preview.sceneKeys.size() > 4) {
      g.setFont(type::sans(12.0f));
      g.setColour(colour::muted);
      g.drawText("and " + juce::String(preview.sceneKeys.size() - 4) + " more", inner.removeFromTop(18),
                 juce::Justification::centredLeft, false);
    }
  } else {
    g.setFont(type::compact());
    g.setColour(colour::muted);
    const juce::String text = p.sceneCount > 0 ? juce::String(p.sceneCount) + (p.sceneCount == 1 ? " scene" : " scenes")
                                               : juce::String("Not stored for factory presets.");
    g.drawText(text, inner.removeFromTop(20), juce::Justification::centredLeft, false);
  }
  inner.removeFromTop(18);

  // Tempo.
  {
    auto line = inner.removeFromTop(20);
    paint::label(g, "Tempo", line);
    g.setFont(type::value());
    g.setColour(colour::text);
    const double bpm = preview.available && preview.tempo > 0.0 ? preview.tempo : p.tempo;
    g.drawText(bpm > 0.0 ? tempoText(bpm) : juce::String("Not stored"), line, juce::Justification::centredRight, false);
  }
  inner.removeFromTop(18);

  if (preview.available && preview.usesLine().isNotEmpty()) {
    g.setFont(type::body());
    g.setColour(colour::text);
    juce::AttributedString text;
    text.append(preview.usesLine(), type::body(), colour::text);
    text.draw(g, inner.removeFromTop(40).toFloat());
    inner.removeFromTop(8);
  }

  // The file on disk.
  if (p.path.isNotEmpty()) {
    paint::label(g, "File", inner.removeFromTop(16));
    inner.removeFromTop(6);
    const juce::File file(p.path);
    juce::String shown = file.getFullPathName();
    const juce::String home = juce::File::getSpecialLocation(juce::File::userHomeDirectory).getFullPathName();
    if (shown.startsWith(home))
      shown = "~" + shown.substring(home.length());
    juce::AttributedString text;
    text.append(shown, type::mono(12.0f, type::Weight::regular), colour::muted);
    text.setLineSpacing(2.0f);
    juce::TextLayout layout;
    layout.createLayout(text, static_cast<float>(inner.getWidth() - 24));
    const auto box = inner.removeFromTop(juce::roundToInt(std::ceil(layout.getHeight())) + 18);
    g.setColour(colour::ground);
    g.fillRoundedRectangle(box.toFloat(), size::buttonRadius);
    g.setColour(colour::line);
    g.drawRoundedRectangle(box.toFloat().reduced(0.5f), size::buttonRadius, 1.0f);
    // The measured layout itself: a second layout pass can wrap a long path
    // differently and spill out of the box.
    layout.draw(g, box.reduced(12, 8).toFloat());
    inner.removeFromTop(6);
    g.setFont(type::sans(12.0f));
    g.setColour(colour::muted);
    juce::AttributedString note;
    note.append("Presets are plain JSON files; back them up or version them with git.", type::sans(12.0f), colour::muted);
    note.draw(g, inner.removeFromTop(36).toFloat());
  }
}

}  // namespace pedaleira::ui

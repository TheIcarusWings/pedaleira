#include "BlockPicker.h"

#include "../Icons.h"
#include "../Paint.h"
#include "../Type.h"

namespace pedaleira::ui {

namespace {

constexpr int kMargin = 24;
constexpr int kSidebarWidth = 220;
constexpr int kRowHeight = 38;
constexpr int kHeadingHeight = 30;
constexpr int kNoticeHeight = 46;
// The most files assets.list is asked for; a folder with more is listed in part.
constexpr int kMaxAssets = 1000;
constexpr int kMaxRecent = 30;

const std::vector<std::pair<juce::String, Category>>& categoryList() {
  static const std::vector<std::pair<juce::String, Category>> list = {
      {"amp", Category::amp},           {"drive", Category::drive},     {"cab", Category::cab},
      {"dynamics", Category::dynamics}, {"eq", Category::eq},           {"delay", Category::delay},
      {"reverb", Category::reverb},     {"modulation", Category::modulation}, {"pitch", Category::pitch},
      {"filter", Category::filter},     {"creative", Category::creative}, {"utility", Category::utility}, {"plugin", Category::plugin},
  };
  return list;
}

const juce::StringArray& chipNames() {
  static const juce::StringArray names{"All sources", "Built-in", "NAM captures", "IRs", "My plugins (VST3/AU)", "Recent", "TONE3000"};
  return names;
}

}  // namespace

BlockPicker::BlockPicker(EngineClient& client) : engine(client) {
  setTitle("Block library");
  setWantsKeyboardFocus(true);
  search.setTitle("Search");
  search.setTextToShowWhenEmpty("Search blocks, captures by name, IRs, plugins", colour::faint);
  search.setFont(type::sans(14.0f, type::Weight::medium));
  search.setColour(juce::TextEditor::backgroundColourId, colour::ground);
  search.setColour(juce::TextEditor::outlineColourId, colour::line);
  search.setColour(juce::TextEditor::focusedOutlineColourId, colour::iris);
  search.setColour(juce::TextEditor::textColourId, colour::text);
  search.setIndents(38, 11);
  search.addListener(this);
  addAndMakeVisible(search);
  close.onClick = [this] {
    if (onClose)
      onClose();
  };
  addAndMakeVisible(close);
  loadFile.onClick = [this] { chooseFile(); };
  addAndMakeVisible(loadFile);
  addChildComponent(tonePanel);
  // A capture downloaded from TONE3000 goes to the slot like any file.
  tonePanel.onAddFile = [this](const juce::File& file, const juce::String& name,
                               std::function<void(const juce::String&)> finished) {
    Item item;
    item.id = file.getFullPathName();
    item.name = name;
    item.source = file.hasFileExtension("nam") ? captures : irs;
    const bool added = add(item);
    // The panel shows the reason itself.
    const juce::String why = added ? juce::String() : notice;
    notice = {};
    finished(why);
  };
}

BlockPicker::~BlockPicker() { search.removeListener(this); }

void BlockPicker::setTone3000Feed(Tone3000Feed* feed) { tonePanel.setFeed(feed); }

void BlockPicker::showNotice(const juce::String& text) {
  notice = text;
  resized();
  repaint();
}

void BlockPicker::chooseFile() {
  chooser = std::make_unique<juce::FileChooser>(
      "Choose a NAM capture or an impulse response",
      juce::File::getSpecialLocation(juce::File::userHomeDirectory), "*.nam;*.wav");
  juce::Component::SafePointer<BlockPicker> self(this);
  chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                       [self](const juce::FileChooser& picked) {
                         const juce::File file = picked.getResult();
                         if (self == nullptr || !file.existsAsFile())
                           return;
                         Item item;
                         item.id = file.getFullPathName();
                         item.name = file.getFileNameWithoutExtension();
                         item.source = file.hasFileExtension("nam") ? captures : irs;
                         self->add(item);
                       });
}

void BlockPicker::openFor(int row, int slot) {
  targetRow = row;
  targetSlot = slot;
  items.clear();
  notice = {};

  // Built-in effects: every kind a block can be made of.
  const juce::var kinds = engine.read("kinds.list");
  if (const auto* list = kinds["kinds"].getArray())
    for (const auto& kind : *list) {
      if (!static_cast<bool>(kind["addable"]) || kind["id"].toString() == "plugin")
        continue;
      Item item;
      item.name = kind["name"].toString();
      item.category = categoryFromId(kind["category"].toString());
      item.source = builtIn;
      item.badge = "Built-in";
      // An amp is filed under its family (High gain, Clean, Crunch): shown in
      // the detail column and found by the search.
      item.detail = kind["family"].toString();
      item.id = kind["id"].toString();
      item.keywords = (item.name + " " + item.detail + " " + item.badge).toLowerCase();
      items.push_back(item);
    }

  readAssets();

  // Scanned plugins that can be added.
  const juce::var plugins = engine.read("plugins.list");
  if (const auto* list = plugins["plugins"].getArray())
    for (const auto& plugin : *list) {
      if (plugin["status"].toString() != "ok")
        continue;
      Item item;
      item.name = plugin["name"].toString();
      item.category = Category::plugin;
      item.source = BlockPicker::plugins;
      item.badge = plugin["format"].toString().containsIgnoreCase("audio") ? juce::String("AU") : juce::String("VST3");
      item.detail = plugin["vendor"].toString();
      item.id = plugin["identifier"].toString();
      item.keywords = (item.name + " " + item.detail + " " + item.badge).toLowerCase();
      items.push_back(item);
    }

  std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
    return a.name.compareNatural(b.name) < 0;
  });
  // The recent list refers to items by their file, so it is read last.
  readRecent();
  search.clear();
  scroll = 0;
  tonePanel.setTarget(row, slot);
  updateSourceViews();
  rebuildRows();
  grabKeyboardFocus();
}

// The library's captures and IRs, the folders added in Settings, the TONE3000
// app's download folder and the copies of the files loaded once. The stash and
// the asset store keep the same bytes under the same hash name, so a loaded
// file is listed once, by the name it was loaded under.
void BlockPicker::readAssets() {
  const juce::var assets = engine.read("assets.list", object({{"limit", kMaxAssets}}));
  truncated = static_cast<bool>(assets["truncated"]);
  juce::StringArray seen, sizes;
  // The files that are where they were come first: a loaded copy of one of them
  // (same name, same size) is the same capture and is not listed again.
  std::vector<juce::var> ordered;
  if (const auto* list = assets["assets"].getArray()) {
    for (const auto& asset : *list)
      if (asset["origin"].toString() != "loaded")
        ordered.push_back(asset);
    for (const auto& asset : *list)
      if (asset["origin"].toString() == "loaded")
        ordered.push_back(asset);
  }
  {
    for (const auto& asset : ordered) {
      const juce::String origin = asset["origin"].toString();
      const juce::String path = asset["path"].toString();
      const juce::String key = origin == "loaded" ? juce::File(path).getFileName() : path;
      const juce::String signature = asset["name"].toString() + "|" + asset["sizeBytes"].toString();
      if (seen.contains(key) || (origin == "loaded" && sizes.contains(signature)))
        continue;
      seen.add(key);
      if (origin != "loaded")
        sizes.add(signature);

      Item item;
      item.name = asset["name"].toString();
      const bool capture = asset["kind"].toString() == "capture";
      item.source = capture ? captures : irs;
      item.category = capture ? Category::amp : Category::cab;
      const juce::String gearType = asset["nam_gear_type"].toString();
      if (capture && gearType.containsIgnoreCase("pedal") && !gearType.containsIgnoreCase("amp"))
        item.category = Category::drive;
      if (!capture)
        item.badge = "IR";
      else if (origin == "tone3000")
        item.badge = "TONE3000";
      else if (origin == "folder")
        item.badge = "Folder";
      else if (origin == "loaded")
        item.badge = "Loaded";
      else
        item.badge = "Capture";
      // The folders and the player's tags, else what the file says of its gear.
      juce::StringArray tags;
      if (const auto* tagList = asset["tags"].getArray())
        for (const auto& tag : *tagList)
          tags.add(tag.toString());
      item.detail = tags.joinIntoString(", ");
      const juce::String gear = (asset["nam_gear_make"].toString() + " " + asset["nam_gear_model"].toString()).trim();
      if (item.detail.isEmpty())
        item.detail = gear;
      item.id = path;
      if (capture) {
        item.unsupported = asset.hasProperty("supported") && !static_cast<bool>(asset["supported"]);
        item.reason = asset["unsupportedReason"].toString();
        item.model = item.unsupported ? asset["architectureLabel"].toString() : juce::String("Full, Lite");
      }
      item.keywords = (item.name + " " + item.detail + " " + item.badge + " " + gear + " " + asset["nam_name"].toString() + " " +
                       juce::File(asset["loadedFrom"].toString()).getFileName() + " " + asset["architectureLabel"].toString() + " " +
                       juce::File(path).getFileNameWithoutExtension())
                          .toLowerCase();
      items.push_back(item);
    }
  }
}

// The last captures loaded from a file, newest first. Each is a listed item
// when its file is one; a file outside every listed folder is an item of its
// own that only the Recent chip shows.
void BlockPicker::readRecent() {
  const juce::var recentList = engine.read("nam.recent", object({{"kind", "all"}, {"limit", kMaxRecent}}));
  const auto* list = recentList["recent"].getArray();
  if (list == nullptr)
    return;
  int rank = 0;
  for (const auto& entry : *list) {
    const juce::String path = entry["path"].toString();
    bool found = false;
    for (auto& item : items)
      if (item.id == path && item.source != builtIn && item.source != plugins) {
        item.recentRank = rank;
        found = true;
        break;
      }
    // Loaded from the library or a folder, the capture is listed under its own path.
    if (!found)
      for (auto& item : items)
        if (item.source == captures && item.recentRank < 0 && !item.recentOnly && item.name == entry["name"].toString() &&
            entry["kind"].toString() == "capture") {
          item.recentRank = rank;
          found = true;
          break;
        }
    if (!found) {
      Item item;
      item.name = entry["name"].toString();
      const bool capture = entry["kind"].toString() == "capture";
      item.source = capture ? captures : irs;
      item.category = capture ? Category::amp : Category::cab;
      item.badge = capture ? (static_cast<bool>(entry["fromStash"]) ? "Loaded" : "File") : "IR";
      item.id = path;
      if (capture) {
        item.unsupported = entry.hasProperty("supported") && !static_cast<bool>(entry["supported"]);
        item.reason = entry["unsupportedReason"].toString();
        item.model = item.unsupported ? entry["architectureLabel"].toString() : juce::String("Full, Lite");
      }
      item.keywords = (item.name + " " + item.badge).toLowerCase();
      item.recentRank = rank;
      item.recentOnly = true;
      items.push_back(item);
    }
    ++rank;
  }
}

void BlockPicker::setFilter(const juce::String& newCategory, int newSource, const juce::String& text) {
  category = newCategory;
  source = newSource;
  search.setText(text, false);
  notice = {};
  updateSourceViews();
  rebuildRows();
}

// The TONE3000 chip replaces the list with the TONE3000 page.
void BlockPicker::updateSourceViews() {
  const bool online = source == tone3000;
  tonePanel.setVisible(online);
  if (online)
    tonePanel.show();
  resized();
}

juce::StringArray BlockPicker::shownEntries() const {
  juce::StringArray entries;
  for (const Row& row : rows)
    if (!row.heading && row.item >= 0)
      entries.add(items[static_cast<size_t>(row.item)].name + " | " + items[static_cast<size_t>(row.item)].detail);
  return entries;
}

void BlockPicker::textEditorTextChanged(juce::TextEditor&) {
  scroll = 0;
  rebuildRows();
}

void BlockPicker::rebuildRows() {
  rows.clear();
  const juce::String query = search.getText().trim().toLowerCase();
  const auto inCategory = [&](const Item& item) {
    return category.isEmpty() || item.category == categoryFromId(category);
  };
  const auto matches = [&](const Item& item) {
    return inCategory(item) && (query.isEmpty() || item.keywords.contains(query));
  };

  if (source == recent) {
    // One section, the newest first.
    std::vector<int> matched;
    int total = 0;
    for (size_t i = 0; i < items.size(); ++i)
      if (items[i].recentRank >= 0 && inCategory(items[i])) {
        ++total;
        if (matches(items[i]))
          matched.push_back(static_cast<int>(i));
      }
    std::sort(matched.begin(), matched.end(), [this](int a, int b) {
      return items[static_cast<size_t>(a)].recentRank < items[static_cast<size_t>(b)].recentRank;
    });
    if (total > 0) {
      Row heading;
      heading.heading = true;
      heading.title = "Recent";
      heading.note = juce::String(matched.size()) + " of " + juce::String(total);
      heading.right = "Newest first";
      rows.push_back(heading);
      for (const int index : matched) {
        Row row;
        row.item = index;
        rows.push_back(row);
      }
    }
    repaint();
    return;
  }

  // One section per source, in the board's order.
  const std::pair<Source, const char*> sections[] = {
      {captures, "Amp captures"}, {irs, "Impulse responses"}, {builtIn, "Built-in blocks"}, {plugins, "My plugins"}};
  for (const auto& [sectionSource, title] : sections) {
    if (source != all && source != sectionSource)
      continue;
    std::vector<int> matched;
    int total = 0;
    for (size_t i = 0; i < items.size(); ++i) {
      const Item& item = items[i];
      if (item.source != sectionSource || item.recentOnly || !inCategory(item))
        continue;
      ++total;
      if (matches(item))
        matched.push_back(static_cast<int>(i));
    }
    if (total == 0)
      continue;
    Row heading;
    heading.heading = true;
    heading.title = title;
    heading.note = juce::String(matched.size()) + " of " + juce::String(total);
    heading.right = truncated && (sectionSource == captures || sectionSource == irs)
                        ? "First " + juce::String(kMaxAssets) + " files"
                        : juce::String("Sorted by name");
    rows.push_back(heading);
    for (const int index : matched) {
      Row row;
      row.item = index;
      rows.push_back(row);
    }
  }
  repaint();
}

juce::StringArray BlockPicker::shownNames() const {
  juce::StringArray names;
  for (const auto& row : rows)
    if (!row.heading)
      names.add(items[static_cast<size_t>(row.item)].name);
  return names;
}

bool BlockPicker::clickName(const juce::String& name) {
  for (const auto& row : rows)
    if (!row.heading && items[static_cast<size_t>(row.item)].name == name) {
      activate(items[static_cast<size_t>(row.item)]);
      return true;
    }
  return false;
}

juce::Rectangle<int> BlockPicker::categoryBounds(int index) const {
  // "All" first, then the categories.
  return {kMargin + 12, kMargin + 46 + index * 42, kSidebarWidth - 24, 40};
}

juce::Rectangle<int> BlockPicker::chipBounds(int index) const {
  int x = kMargin + kSidebarWidth + 20;
  for (int i = 0; i < index; ++i)
    x += juce::roundToInt(juce::GlyphArrangement::getStringWidth(type::sans(12.0f, type::Weight::semibold), chipNames()[i])) + 24 + 8;
  const int width = juce::roundToInt(juce::GlyphArrangement::getStringWidth(type::sans(12.0f, type::Weight::semibold), chipNames()[index])) + 24;
  return {x, kMargin + 140, width, 30};
}

juce::Rectangle<int> BlockPicker::noticeArea() const {
  if (notice.isEmpty() || source == tone3000)
    return {};
  return {kMargin + kSidebarWidth + 20, kMargin + 180, getWidth() - 2 * kMargin - kSidebarWidth - 40, kNoticeHeight};
}

juce::Rectangle<int> BlockPicker::listArea() const {
  const int top = kMargin + 184 + (notice.isEmpty() || source == tone3000 ? 0 : kNoticeHeight + 8);
  return {kMargin + kSidebarWidth + 20, top, getWidth() - 2 * kMargin - kSidebarWidth - 40,
          getHeight() - kMargin - top - 16};
}

int BlockPicker::rowAt(juce::Point<int> position) const {
  const auto area = listArea();
  if (!area.contains(position))
    return -1;
  int y = area.getY() + 20 - scroll;
  for (size_t i = 0; i < rows.size(); ++i) {
    const int height = rows[i].heading ? kHeadingHeight : kRowHeight;
    if (position.y >= y && position.y < y + height)
      return rows[i].heading ? -1 : static_cast<int>(i);
    y += height + 2;
  }
  return -1;
}

void BlockPicker::paint(juce::Graphics& g) {
  g.fillAll(colour::ground);
  const auto frame = getLocalBounds().reduced(kMargin);
  paint::panel(g, frame.toFloat());
  g.setColour(colour::line);
  g.fillRect(frame.getX() + kSidebarWidth, frame.getY(), 1, frame.getHeight());

  // Categories, with how many of each there are.
  paint::label(g, "Categories", {frame.getX() + 20, frame.getY() + 16, 180, 14});
  for (int i = 0; i <= static_cast<int>(categoryList().size()); ++i) {
    const auto area = categoryBounds(i);
    const bool isAll = i == 0;
    const juce::String id = isAll ? juce::String() : categoryList()[static_cast<size_t>(i - 1)].first;
    const Category cat = isAll ? Category::utility : categoryList()[static_cast<size_t>(i - 1)].second;
    int count = 0;
    for (const auto& item : items)
      if (!item.recentOnly && (isAll || item.category == cat))
        ++count;
    if (id == category) {
      g.setColour(colour::control);
      g.fillRoundedRectangle(area.toFloat(), size::buttonRadius);
    }
    auto inner = area.reduced(8, 0);
    const auto iconArea = inner.removeFromLeft(24).toFloat().withSizeKeepingCentre(20.0f, 20.0f);
    if (isAll)
      icons::draw(g, icons::path(icons::Icon::grid), iconArea, colour::muted);
    else
      icons::draw(g, icons::path(cat), iconArea, categoryColour(cat));
    inner.removeFromLeft(10);
    g.setFont(type::mono(12.0f, type::Weight::regular));
    g.setColour(colour::muted);
    g.drawText(juce::String(count), inner, juce::Justification::centredRight, false);
    g.setFont(type::button());
    g.setColour(colour::text);
    g.drawText(isAll ? juce::String("All") : (cat == Category::plugin ? juce::String("Plugins") : categoryName(cat)), inner,
               juce::Justification::centredLeft, false);
  }

  // Title, the slot it adds to, the search label and the chips.
  const int x = frame.getX() + kSidebarWidth + 20;
  g.setFont(type::heading());
  g.setColour(colour::text);
  g.drawText("Block library", x, frame.getY() + 20, 200, 26, juce::Justification::centredLeft, false);
  g.setFont(type::sans(12.0f));
  g.setColour(colour::muted);
  g.drawText("Adding to Row " + juce::String(targetRow + 1) + ", slot " + juce::String(targetSlot + 1), x + 172,
             frame.getY() + 24, 300, 20, juce::Justification::centredLeft, false);
  paint::label(g, "Search", {x, frame.getY() + 58, 200, 14});
  for (int i = 0; i < chipNames().size(); ++i) {
    const auto chip = chipBounds(i).toFloat();
    const bool on = i == source;
    g.setColour(on ? colour::control : colour::raised);
    g.fillRoundedRectangle(chip, 15.0f);
    g.setColour(on ? colour::iris : colour::line);
    g.drawRoundedRectangle(chip.reduced(0.5f), 15.0f, 1.0f);
    g.setFont(type::sans(12.0f, type::Weight::semibold));
    g.setColour(on ? colour::text : colour::muted);
    g.drawText(chipNames()[i], chip, juce::Justification::centred, false);
  }

  // TONE3000 replaces the list with its own page.
  if (source == tone3000)
    return;

  // A capture that can not be loaded, or an add that failed.
  if (const auto strip = noticeArea(); !strip.isEmpty()) {
    g.setColour(colour::raised);
    g.fillRoundedRectangle(strip.toFloat(), size::buttonRadius);
    g.setColour(colour::warn);
    g.drawRoundedRectangle(strip.toFloat().reduced(0.5f), size::buttonRadius, 1.0f);
    icons::draw(g, icons::path(icons::Icon::warning), strip.withTrimmedLeft(12).removeFromLeft(24).toFloat().withSizeKeepingCentre(20.0f, 20.0f),
                colour::warn);
    g.setFont(type::sans(12.0f));
    g.setColour(colour::text);
    g.drawFittedText(notice, strip.withTrimmedLeft(46).withTrimmedRight(14).reduced(0, 4), juce::Justification::centredLeft, 2, 1.0f);
  }

  // The list: column titles, then sections.
  const auto area = listArea();
  const int nameWidth = area.getWidth() - 100 - 120 - 76 - 36 - 4 * 10 - 16;
  const auto columns = [&](juce::Rectangle<int> r) {
    std::array<juce::Rectangle<int>, 5> c;
    r = r.reduced(10, 0);
    c[0] = r.removeFromLeft(nameWidth);
    r.removeFromLeft(10);
    c[1] = r.removeFromLeft(100);
    r.removeFromLeft(10);
    c[2] = r.removeFromLeft(120);
    r.removeFromLeft(10);
    c[3] = r.removeFromLeft(76);
    return c;
  };
  {
    const auto c = columns(area.withHeight(20));
    const char* titles[] = {"Name", "Source", "Detail", "Model"};
    for (int i = 0; i < 4; ++i)
      paint::label(g, titles[i], c[static_cast<size_t>(i)]);
  }
  juce::Graphics::ScopedSaveState state(g);
  g.reduceClipRegion(area.withTrimmedTop(20));
  int y = area.getY() + 20 - scroll;
  for (size_t i = 0; i < rows.size(); ++i) {
    const Row& row = rows[i];
    if (row.heading) {
      const auto r = juce::Rectangle<int>(area.getX(), y, area.getWidth(), kHeadingHeight).reduced(10, 0);
      g.setFont(type::bodyStrong());
      g.setColour(colour::text);
      const int titleWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(type::bodyStrong(), row.title)) + 10;
      g.drawText(row.title, r.withWidth(titleWidth), juce::Justification::centredLeft, false);
      g.setFont(type::mono(12.0f, type::Weight::regular));
      g.setColour(colour::muted);
      g.drawText(row.note, r.withTrimmedLeft(titleWidth), juce::Justification::centredLeft, false);
      g.setFont(type::sans(12.0f));
      g.drawText(row.right, r, juce::Justification::centredRight, false);
      y += kHeadingHeight + 2;
      continue;
    }
    const Item& item = items[static_cast<size_t>(row.item)];
    const auto r = juce::Rectangle<int>(area.getX(), y, area.getWidth(), kRowHeight);
    if (static_cast<int>(i) == hover) {
      g.setColour(colour::raised);
      g.fillRoundedRectangle(r.toFloat(), size::buttonRadius);
    }
    const auto c = columns(r);
    auto name = c[0];
    const auto icon = name.removeFromLeft(24).toFloat().withSizeKeepingCentre(24.0f, 24.0f);
    g.setColour(categoryTint(item.category));
    g.fillRoundedRectangle(icon, 6.0f);
    icons::draw(g, icons::path(item.category), icon.reduced(5.0f), categoryColour(item.category));
    name.removeFromLeft(10);
    g.setFont(type::sans(13.0f, type::Weight::semibold));
    g.setColour(item.unsupported ? colour::dim : colour::text);
    g.drawText(item.name, name, juce::Justification::centredLeft, true);
    const juce::Font badgeFont = type::sans(11.0f, type::Weight::semibold);
    const auto badge = c[1].withWidth(juce::roundToInt(juce::GlyphArrangement::getStringWidth(badgeFont, item.badge)) + 16)
                           .withSizeKeepingCentre(juce::roundToInt(juce::GlyphArrangement::getStringWidth(badgeFont, item.badge)) + 16, 20)
                           .withX(c[1].getX());
    g.setColour(colour::control);
    g.fillRoundedRectangle(badge.toFloat(), 6.0f);
    g.setFont(badgeFont);
    g.setColour(item.source == plugins ? categoryColour(Category::plugin) : colour::text);
    g.drawText(item.badge, badge, juce::Justification::centred, false);
    g.setFont(type::sans(12.0f));
    g.setColour(colour::text);
    g.drawText(item.detail, c[2], juce::Justification::centredLeft, true);
    g.setColour(item.unsupported ? colour::warn : colour::muted);
    g.drawText(item.model, c[3], juce::Justification::centredLeft, true);
    if (item.unsupported)
      icons::draw(g, icons::path(icons::Icon::warning), juce::Rectangle<float>(static_cast<float>(c[3].getRight() + 8), static_cast<float>(c[3].getCentreY() - 8), 16.0f, 16.0f),
                  colour::warn);
    y += kRowHeight + 2;
  }
  if (rows.empty()) {
    g.setFont(type::body());
    g.setColour(colour::muted);
    g.drawText(source == recent ? juce::String("Nothing loaded from a file yet. Captures you load show up here.")
                                : juce::String("Nothing matches. Try another category or source."),
               area.withTrimmedTop(40).withHeight(24), juce::Justification::centredLeft, false);
  }
}

void BlockPicker::paintOverChildren(juce::Graphics& g) {
  // The magnifier sits inside the search field, so it is drawn over it.
  static const juce::Path magnifier = juce::Drawable::parseSVGPath("M11 4a7 7 0 1 0 0 14a7 7 0 1 0 0-14M21 21l-5-5");
  icons::draw(g, magnifier, search.getBounds().toFloat().removeFromLeft(38.0f).withSizeKeepingCentre(16.0f, 16.0f),
              colour::muted);
}

void BlockPicker::resized() {
  const auto frame = getLocalBounds().reduced(kMargin);
  const int x = frame.getX() + kSidebarWidth + 20;
  const int loadWidth = loadFile.idealWidth() + 8;
  search.setBounds(x, frame.getY() + 76, frame.getRight() - 20 - x - loadWidth - 12, 40);
  loadFile.setBounds(frame.getRight() - 20 - loadWidth, frame.getY() + 76, loadWidth, 40);
  close.setBounds(frame.getRight() - 20 - close.idealWidth(), frame.getY() + 18, close.idealWidth(), 32);
  tonePanel.setBounds(x, kMargin + 180, frame.getRight() - 20 - x, frame.getBottom() - 16 - (kMargin + 180));
}

void BlockPicker::mouseMove(const juce::MouseEvent& event) {
  const int now = rowAt(event.getPosition());
  if (now != hover) {
    hover = now;
    repaint(listArea());
  }
}

void BlockPicker::mouseUp(const juce::MouseEvent& event) {
  const auto at = event.getPosition();
  if (noticeArea().contains(at)) {
    showNotice({});
    return;
  }
  for (int i = 0; i <= static_cast<int>(categoryList().size()); ++i)
    if (categoryBounds(i).contains(at)) {
      category = i == 0 ? juce::String() : categoryList()[static_cast<size_t>(i - 1)].first;
      scroll = 0;
      notice = {};
      rebuildRows();
      return;
    }
  for (int i = 0; i < chipNames().size(); ++i)
    if (chipBounds(i).contains(at)) {
      source = i;
      scroll = 0;
      notice = {};
      updateSourceViews();
      rebuildRows();
      return;
    }
  if (const int row = rowAt(at); row >= 0)
    activate(items[static_cast<size_t>(rows[static_cast<size_t>(row)].item)]);
}

void BlockPicker::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) {
  int content = 0;
  for (const auto& row : rows)
    content += (row.heading ? kHeadingHeight : kRowHeight) + 2;
  const int maxScroll = juce::jmax(0, content + 20 - listArea().getHeight());
  scroll = juce::jlimit(0, maxScroll, scroll - juce::roundToInt(wheel.deltaY * 200.0f));
  repaint(listArea());
}

bool BlockPicker::keyPressed(const juce::KeyPress& key) {
  if (key == juce::KeyPress::escapeKey) {
    if (onClose)
      onClose();
    return true;
  }
  return false;
}

void BlockPicker::activate(const Item& item) {
  // Said before anything is tried: the file is not one Pedaleira loads.
  if (item.unsupported && item.source != builtIn && item.source != plugins) {
    showNotice(item.name + ": " + (item.reason.isNotEmpty() ? item.reason : juce::String("Pedaleira can not load this capture.")));
    return;
  }
  add(item);
}

bool BlockPicker::add(const Item& item) {
  EngineReply reply;
  if (item.source == builtIn)
    reply = engine.run("chain.addBlock", object({{"kind", item.id}, {"row", targetRow}, {"slot", targetSlot}}));
  else if (item.source == plugins)
    reply = engine.run("chain.addPlugin", object({{"identifier", item.id}, {"row", targetRow}, {"slot", targetSlot}}));
  else
    reply = engine.run("chain.addToneFromFiles",
                       object({{"paths", juce::Array<juce::var>{item.id}}, {"row", targetRow}, {"slot", targetSlot}}));
  if (!reply.ok) {
    // The engine's sentence, shown where the player is looking.
    showNotice("Could not add " + item.name + ". " + reply.errorMessage);
    return false;
  }
  if (onAdded)
    onAdded(reply.value["blockId"].toString());
  return true;
}

}  // namespace pedaleira::ui

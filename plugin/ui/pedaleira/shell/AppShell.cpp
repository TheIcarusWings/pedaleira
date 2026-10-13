#include "AppShell.h"

#include "../Paint.h"
#include "../Type.h"

namespace pedaleira::ui {

namespace {

constexpr std::array<std::pair<Page, const char*>, 6> kPages = {{
    {Page::grid, "Grid"},
    {Page::library, "Library"},
    {Page::capture, "Capture"},
    {Page::import, "Import"},
    {Page::live, "Live"},
    {Page::settings, "Settings"},
}};

icons::Icon iconOf(Page page) {
  switch (page) {
    case Page::grid: return icons::Icon::grid;
    case Page::library: return icons::Icon::library;
    case Page::capture: return icons::Icon::capture;
    case Page::import: return icons::Icon::import;
    case Page::live: return icons::Icon::live;
    case Page::settings: break;
  }
  return icons::Icon::settings;
}

// The modes in the order the header shows them, as footswitch.setMode names
// them.
const juce::StringArray& modeIds() {
  static const juce::StringArray ids{"preset", "scene", "stomp", "hybrid"};
  return ids;
}

// The board mark: a rail with three pedals on a board (the logo stays this
// until a refinement is chosen).
void paintMark(juce::Graphics& g, juce::Rectangle<float> area) {
  const float s = area.getWidth() / 48.0f;
  const auto at = [&](float x, float y, float w, float h) {
    return juce::Rectangle<float>(area.getX() + x * s, area.getY() + y * s, w * s, h * s);
  };
  g.setColour(colour::muted);
  g.drawLine(area.getX() + 2.0f * s, area.getY() + 20.0f * s, area.getX() + 46.0f * s, area.getY() + 20.0f * s, 3.0f * s);
  g.setColour(colour::iris);
  g.fillRoundedRectangle(at(4, 9, 8, 22), 2.5f * s);
  g.setColour(colour::text);
  g.fillRoundedRectangle(at(15.5f, 9, 16, 22), 2.5f * s);
  g.fillRoundedRectangle(at(35, 9, 9, 22), 2.5f * s);
  g.fillRoundedRectangle(at(2, 34, 44, 5), 2.5f * s);
}

}  // namespace

// ####################
// NAVIGATION RAIL
// ####################
struct NavRail::Item final : juce::Button {
  Item(Page itemPage, const juce::String& name) : juce::Button(name), page(itemPage) {
    setTitle(name);
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
  }

  void paintButton(juce::Graphics& g, bool highlighted, bool) override {
    const auto area = getLocalBounds().toFloat();
    if (current || highlighted) {
      g.setColour(current ? colour::control : colour::raised);
      g.fillRoundedRectangle(area, 10.0f);
    }
    const juce::Colour fg = current ? colour::text : colour::muted;
    icons::draw(g, icons::path(iconOf(page)), area.withHeight(34.0f).withTrimmedTop(10.0f), fg);
    g.setFont(type::sans(11.0f, type::Weight::semibold));
    g.setColour(fg);
    g.drawText(getButtonText(), area.withTrimmedTop(34.0f).withHeight(16.0f), juce::Justification::centred, false);
    if (hasKeyboardFocus(false)) {
      g.setColour(colour::iris);
      g.drawRoundedRectangle(area.reduced(1.0f), 10.0f, 2.0f);
    }
  }

  Page page;
  bool current = false;
};

NavRail::NavRail() {
  setTitle("Main");
  for (size_t i = 0; i < kPages.size(); ++i) {
    items[i] = std::make_unique<Item>(kPages[i].first, kPages[i].second);
    items[i]->onClick = [this, i] {
      if (onSelect)
        onSelect(kPages[i].first);
    };
    addAndMakeVisible(*items[i]);
  }
  setPage(Page::grid);
}

void NavRail::setPage(Page newPage) {
  page = newPage;
  for (auto& item : items) {
    item->current = item->page == page;
    item->repaint();
  }
}

void NavRail::setMasterLevels(const std::array<float, 2>& input, const std::array<float, 2>& output) {
  const juce::uint32 time = juce::Time::getMillisecondCounter();
  const std::array<std::array<float, 2>, 2> now{input, output};
  for (size_t m = 0; m < 2; ++m)
    for (size_t c = 0; c < 2; ++c) {
      // The peak holds fall about 12 dB a second at ten readings a second.
      masterHold[m][c] = juce::jmax(now[m][c], masterHold[m][c] - 1.2f);
      if (now[m][c] >= -0.5f)
        masterClipUntil[m] = time + 2000;
    }
  if (now == master)
    return;
  master = now;
  repaint(meterArea().expanded(4));
}

juce::Rectangle<int> NavRail::meterArea() const {
  // Under the last page button, down to the foot of the rail.
  return {(getWidth() - 48) / 2, juce::jmax(70 + 6 * 60 + 16, getHeight() - 236), 48, 220};
}

void NavRail::paint(juce::Graphics& g) {
  g.fillAll(colour::surface);
  g.setColour(colour::line);
  g.fillRect(getWidth() - 1, 0, 1, getHeight());
  paintMark(g, juce::Rectangle<float>(36.0f, 36.0f).withCentre({getWidth() * 0.5f, 32.0f}));

  // The master meters: IN (the device input) and OUT (the main output), a
  // bar per channel from -60 dB at the foot to 0 dB at the top, green into
  // amber at -12 dB and red at -3 dB, a peak tick, and a red cap that stays
  // lit for two seconds after a level reaches full scale.
  const auto area = meterArea().toFloat();
  const juce::uint32 time = juce::Time::getMillisecondCounter();
  const auto bars = area.withTrimmedTop(10.0f).withTrimmedBottom(18.0f);
  const auto yOf = [&bars](float db) {
    return bars.getBottom() - bars.getHeight() * juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);
  };
  for (size_t m = 0; m < 2; ++m) {
    const float x = area.getX() + 4.0f + m * 26.0f;
    for (size_t c = 0; c < 2; ++c) {
      const auto bar = juce::Rectangle<float>(x + c * 7.0f, bars.getY(), 5.0f, bars.getHeight());
      g.setColour(colour::raised);
      g.fillRoundedRectangle(bar, 2.0f);
      {
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(bar.withTop(yOf(master[m][c])).toNearestInt());
        g.setColour(colour::ok);
        g.fillRect(bar.withTop(yOf(-12.0f)));
        g.setColour(colour::warn);
        g.fillRect(bar.withTop(yOf(-3.0f)).withBottom(yOf(-12.0f)));
        g.setColour(colour::clip);
        g.fillRect(bar.withBottom(yOf(-3.0f)));
      }
      if (masterHold[m][c] > -60.0f) {
        g.setColour(colour::text);
        g.fillRect(bar.withHeight(2.0f).withY(yOf(masterHold[m][c]) - 1.0f));
      }
    }
    const auto cap = juce::Rectangle<float>(x, area.getY(), 12.0f, 5.0f);
    g.setColour(time < masterClipUntil[m] ? colour::clip : colour::raised);
    g.fillRoundedRectangle(cap, 2.0f);
    g.setFont(type::mono(10.0f));
    g.setColour(colour::muted);
    g.drawText(m == 0 ? "IN" : "OUT", juce::Rectangle<float>(x - 6.0f, bars.getBottom() + 4.0f, 24.0f, 14.0f),
               juce::Justification::centred, false);
  }
}

void NavRail::resized() {
  int y = 70;
  for (auto& item : items) {
    item->setBounds((getWidth() - 60) / 2, y, 60, 56);
    y += 60;
  }
}

// ####################
// HEADER
// ####################
Header::Header(EngineClient& client) : engine(client) {
  setTitle("Header");
  save.setTitle("Save preset");
  save.onClick = [this] {
    // Saving under the preset's own name replaces it in its slot.
    engine.run("preset.save", object({{"name", presetName}}));
    refresh();
  };
  mode.setTitle("Footswitch mode");
  mode.onChange = [this](int index) {
    engine.run("footswitch.setMode", object({{"mode", modeIds()[index]}}));
    // Read back: the control shows what the engine now does.
    refresh();
  };
  tempo.setMono(true);
  tempo.setTitle("Tempo, tap to set");
  tempo.onClick = [this] {
    engine.run("tempo.tap");
    refresh();
  };
  tuner.onClick = [this] { if (onTuner) onTuner(); };
  gigView.onClick = [this] { if (onGigView) onGigView(); };
  newPreset.setTitle("New preset");
  newPreset.onClick = [this] {
    if (onNewPreset)
      onNewPreset(unsaved);
  };
  for (juce::Component* c : std::initializer_list<juce::Component*>{&save, &newPreset, &mode, &tempo, &tuner, &gigView})
    addAndMakeVisible(c);
}

void Header::setSaveIsPrimary(bool primary) {
  saveIsPrimary = primary;
  save.setKind(primary ? PushButton::Kind::primary : PushButton::Kind::secondary);
}

void Header::refresh() {
  const juce::var library = engine.read("library.get");
  const juce::String setlistId = library["current"]["setlist"].toString();
  const int slot = library["current"]["slot"];
  const juce::String presetId = library["current"]["presetId"].toString();
  unsaved = static_cast<bool>(library["unsaved"]);

  juce::String setlistName;
  if (const auto* setlists = library["setlists"].getArray())
    for (const auto& setlist : *setlists)
      if (setlist["id"].toString() == setlistId)
        setlistName = setlist["name"].toString();
  setlistLine = setlistName;
  if (slot > 0)
    setlistLine << " " << type::dot() << " " << juce::String(slot).paddedLeft('0', 3);

  presetName.clear();
  if (presetId.isNotEmpty()) {
    // Kept in a local: the list must outlive the loop over it.
    const juce::var presets = engine.read("preset.list")["presets"];
    if (const auto* list = presets.getArray())
      for (const auto& preset : *list)
        if (preset["id"].toString() == presetId)
          presetName = preset["name"].toString();
  }
  if (presetName.isEmpty())
    presetName = "New preset";

  const juce::var footswitches = engine.read("footswitch.getState");
  mode.setSelected(juce::jmax(0, modeIds().indexOf(footswitches["mode"].toString())));
  const double bpm = footswitches["bpm"];
  tempo.setButtonText(juce::String(bpm > 0.0 ? bpm : 120.0, 1) + " BPM " + type::dot() + " Tap");

  save.setEnabled(unsaved);
  refreshMeters();
  resized();
  repaint();
}

void Header::refreshMeters() {
  const juce::var meters = engine.read("meters.get");
  if (onMeters)
    onMeters(meters);
  const double load = meters["cpu"];
  if (std::abs(load - cpu) < 0.005)
    return;
  cpu = load;
  repaint(cpuArea);
}

void Header::paint(juce::Graphics& g) {
  g.fillAll(colour::ground);
  g.setColour(colour::line);
  g.fillRect(0, getHeight() - 1, getWidth(), 1);

  paint::label(g, setlistLine, titleArea.withHeight(16).translated(0, 10));
  g.setFont(type::sans(20.0f, type::Weight::medium, unsaved));
  g.setColour(colour::text);
  g.drawText(presetName, titleArea.withTrimmedTop(26).withHeight(26), juce::Justification::centredLeft, true);
  if (unsaved) {
    g.setFont(type::sans(12.0f));
    g.setColour(colour::warn);
    g.drawText("Unsaved changes", unsavedArea, juce::Justification::centredLeft, false);
  }

  // CPU: the figure over a short bar.
  g.setFont(type::mono(11.0f));
  g.setColour(colour::muted);
  g.drawText("CPU", cpuArea.withHeight(14), juce::Justification::centredLeft, false);
  g.setColour(colour::text);
  g.drawText(juce::String(juce::roundToInt(cpu * 100.0)) + "%", cpuArea.withHeight(14),
             juce::Justification::centredRight, false);
  const auto bar = cpuArea.withTrimmedTop(18).withHeight(6).toFloat();
  g.setColour(colour::raised);
  g.fillRoundedRectangle(bar, 3.0f);
  g.setColour(cpu > 0.85 ? colour::clip : cpu > 0.6 ? colour::warn : colour::ok);
  g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * static_cast<float>(juce::jlimit(0.0, 1.0, cpu))), 3.0f);
}

void Header::resized() {
  auto area = getLocalBounds().reduced(24, 0);
  const int middle = getHeight() / 2;

  // Right: tempo, tuner, Gig View, CPU.
  cpuArea = juce::Rectangle<int>(area.getRight() - 96, middle - 12, 96, 24);
  int right = cpuArea.getX() - 12;
  for (PushButton* button : {&gigView, &tuner, &tempo}) {
    const int width = button->idealWidth();
    button->setBounds(right - width, middle - 16, width, 32);
    right = button->getX() - 12;
  }

  // Centre: the mode switch.
  const int modeWidth = mode.idealWidth();
  mode.setBounds(getWidth() / 2 - modeWidth / 2, middle - 18, modeWidth, 36);

  // Left: setlist and preset, the unsaved note, Save.
  const juce::Font nameFont = type::sans(20.0f, type::Weight::medium, unsaved);
  const int nameWidth = juce::jmin(320, juce::roundToInt(juce::GlyphArrangement::getStringWidth(nameFont, presetName)) + 4);
  const int setlistWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(type::label(), setlistLine.toUpperCase())) + 4;
  titleArea = area.withWidth(juce::jmax(nameWidth, setlistWidth));
  int x = titleArea.getRight() + 16;
  if (unsaved) {
    unsavedArea = juce::Rectangle<int>(x, middle - 9, 104, 18);
    x = unsavedArea.getRight() + 8;
  } else {
    unsavedArea = {};
  }
  save.setBounds(x, middle - 16, save.idealWidth(), 32);
  newPreset.setBounds(save.getRight() + 8, middle - 16, newPreset.idealWidth(), 32);
}

// ####################
// SHELL
// ####################
AppShell::AppShell(EngineClient& client) : engine(client), top(client) {
  setTitle("Pedaleira");
  addAndMakeVisible(nav);
  addAndMakeVisible(top);
  nav.onSelect = [this](Page selected) { showPage(selected); };
  top.onMeters = [this](const juce::var& meters) {
    const auto pairOf = [](const juce::var& pair) {
      return std::array<float, 2>{static_cast<float>(pair[0]), static_cast<float>(pair[1])};
    };
    nav.setMasterLevels(pairOf(meters["input"]), pairOf(meters["output"]));
    if (onMeters)
      onMeters(meters);
  };
  top.onGigView = [this] { showGigView(true); };
  top.onTuner = [this] {
    showPage(Page::live);
    if (onTunerRequested)
      onTunerRequested();
  };
  top.refresh();
  seenRevision = engine.revision();
  startTimerHz(10);
}

AppShell::~AppShell() { stopTimer(); }

void AppShell::setPageComponent(Page which, juce::Component* component) {
  auto& slot = pages[static_cast<size_t>(which)];
  if (slot != nullptr)
    removeChildComponent(slot);
  slot = component;
  if (component != nullptr) {
    addChildComponent(component);
    component->setVisible(which == page);
  }
  resized();
}

void AppShell::setGigView(juce::Component* component) {
  if (gigView != nullptr)
    removeChildComponent(gigView);
  gigView = component;
  if (component != nullptr) {
    addChildComponent(component);
    component->setBounds(getLocalBounds());
  }
}

void AppShell::showGigView(bool shown) {
  if (gigView == nullptr || gigView->isVisible() == shown)
    return;
  // The rail and header go with it, so nothing behind it takes focus.
  nav.setVisible(!shown);
  top.setVisible(!shown);
  if (shown)
    gigView->toFront(true);
  gigView->setVisible(shown);
  if (!shown)
    repaint();
}

bool AppShell::isGigViewShown() const { return gigView != nullptr && gigView->isVisible(); }

void AppShell::setModesView(juce::Component* component) {
  if (modesView != nullptr)
    removeChildComponent(modesView);
  modesView = component;
  if (component != nullptr) {
    addChildComponent(component);
    resized();
  }
}

void AppShell::showModesView(bool shown) {
  if (modesView == nullptr || modesView->isVisible() == shown)
    return;
  if (shown)
    modesView->toFront(true);
  modesView->setVisible(shown);
}

bool AppShell::isModesViewShown() const { return modesView != nullptr && modesView->isVisible(); }

void AppShell::showPage(Page which) {
  showModesView(false);
  page = which;
  nav.setPage(which);
  for (size_t i = 0; i < pages.size(); ++i)
    if (pages[i] != nullptr)
      pages[i]->setVisible(static_cast<Page>(i) == which);
  // The capture wizard's own steps carry its primary action.
  top.setSaveIsPrimary(which != Page::capture);
  repaint();
  if (onPageShown)
    onPageShown(which);
}

void AppShell::paint(juce::Graphics& g) { g.fillAll(colour::ground); }

void AppShell::resized() {
  auto area = getLocalBounds();
  nav.setBounds(area.removeFromLeft(size::railWidth));
  top.setBounds(area.removeFromTop(size::headerHeight));
  for (auto* component : pages)
    if (component != nullptr)
      component->setBounds(area);
  if (modesView != nullptr)
    modesView->setBounds(area);
  if (gigView != nullptr)
    gigView->setBounds(getLocalBounds());
}

void AppShell::timerCallback() {
  // The header follows the rig: a new revision means something changed
  // (an edit, a preset, a mode); the meters are read every tick.
  if (const juce::uint32 revision = engine.revision(); revision != seenRevision) {
    seenRevision = revision;
    top.refresh();
    if (onRigChanged)
      onRigChanged();
  } else {
    top.refreshMeters();
  }
}

}  // namespace pedaleira::ui

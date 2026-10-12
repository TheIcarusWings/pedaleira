#include "SettingsPage.h"

#include "../Paint.h"
#include "../Type.h"

namespace pedaleira::ui {

namespace {

constexpr int kSubNavWidth = 220;
constexpr int kMarginX = 40;
constexpr int kMarginY = 32;

const char* const kPartNames[] = {"Audio and calibration", "MIDI mapping", "Expression pedals", "Plugins",
                                  "AI and MCP",            "Remote",       "About"};

}  // namespace

// One entry of the list on the left: 36 high, the chosen one raised.
class SettingsPage::NavButton final : public juce::Button {
public:
  explicit NavButton(const juce::String& name) : juce::Button(name) {
    setTitle(name);
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
  }

  bool current = false;

  void paintButton(juce::Graphics& g, bool highlighted, bool) override {
    const auto area = getLocalBounds().toFloat();
    if (current || highlighted) {
      g.setColour(current ? colour::control : colour::raised);
      g.fillRoundedRectangle(area, size::buttonRadius);
    }
    if (hasKeyboardFocus(false)) {
      g.setColour(colour::iris);
      g.drawRoundedRectangle(area.reduced(1.0f), size::buttonRadius, 2.0f);
    }
    g.setFont(type::bodyStrong());
    g.setColour(current ? colour::text : colour::muted);
    g.drawText(getButtonText(), getLocalBounds().reduced(12, 0), juce::Justification::centredLeft, true);
  }
};

SettingsPage::SettingsPage(EngineClient& client, DeviceSettings* deviceSettings)
    : engine(client), device(deviceSettings) {
  setTitle("Settings");
  sections[static_cast<size_t>(Part::audio)] = std::make_unique<SettingsAudio>(engine, device);
  sections[static_cast<size_t>(Part::midi)] = std::make_unique<SettingsMidi>(engine, device);
  sections[static_cast<size_t>(Part::expression)] = std::make_unique<SettingsExpression>(engine);
  sections[static_cast<size_t>(Part::plugins)] = std::make_unique<SettingsPlugins>(engine);
  sections[static_cast<size_t>(Part::agents)] = std::make_unique<SettingsAgents>(engine);
  sections[static_cast<size_t>(Part::remote)] = std::make_unique<SettingsRemote>(engine);
  sections[static_cast<size_t>(Part::about)] = std::make_unique<SettingsAbout>(engine);

  for (size_t i = 0; i < sections.size(); ++i) {
    SettingsSection* section = sections[i].get();
    section->onNotice = [this](const juce::String& title, const juce::String& message) {
      Dialog::Options options;
      options.title = title;
      options.message = message;
      options.cancelable = false;
      prompt.show(options, [](bool, const juce::String&) {});
    };
    section->onHeightChanged = [this, section] {
      if (sections[static_cast<size_t>(part)].get() == section)
        fitSection();
    };
    navButtons[i] = std::make_unique<NavButton>(kPartNames[i]);
    navButtons[i]->onClick = [this, i] { showPart(static_cast<Part>(i)); };
    addAndMakeVisible(*navButtons[i]);
  }

  viewport.setScrollBarsShown(true, false);
  viewport.setScrollBarThickness(8);
  viewport.getVerticalScrollBar().setColour(juce::ScrollBar::thumbColourId, colour::line);
  addAndMakeVisible(viewport);
  addChildComponent(prompt);

  if (device != nullptr) {
    juce::Component::SafePointer<SettingsPage> self(this);
    device->onChanged = [self] {
      if (self != nullptr)
        self->deviceChanged();
    };
  }
  navButtons[0]->current = true;
  viewport.setViewedComponent(sections[0].get(), false);
}

SettingsPage::~SettingsPage() {
  stopTimer();
  // The device outlives the page and must not call back into it.
  if (device != nullptr)
    device->onChanged = nullptr;
  viewport.setViewedComponent(nullptr, false);
}

void SettingsPage::refresh() { sections[static_cast<size_t>(part)]->refresh(); }

void SettingsPage::rigChanged() {
  // Used-by columns and the pedal's assignments follow the rig; the rest
  // does not, and the tag counts walk the library.
  if (part == Part::audio || part == Part::expression)
    refresh();
}

void SettingsPage::deviceChanged() {
  if (part == Part::audio)
    static_cast<SettingsAudio&>(*sections[static_cast<size_t>(Part::audio)]).deviceChanged();
  else if (part == Part::midi)
    static_cast<SettingsMidi&>(*sections[static_cast<size_t>(Part::midi)]).deviceChanged();
}

void SettingsPage::showPart(Part which) {
  part = which;
  for (size_t i = 0; i < navButtons.size(); ++i) {
    navButtons[i]->current = static_cast<Part>(i) == which;
    navButtons[i]->repaint();
  }
  SettingsSection& chosen = *sections[static_cast<size_t>(which)];
  viewport.setViewedComponent(&chosen, false);
  chosen.refresh();
  resized();
  viewport.setViewPosition(0, 0);
  repaint();
}

void SettingsPage::visibilityChanged() {
  // The timer only runs while Settings is on screen.
  if (isVisible())
    startTimerHz(4);
  else
    stopTimer();
}

void SettingsPage::timerCallback() { sections[static_cast<size_t>(part)]->tick(); }

// ####################
// LAYOUT
// ####################
juce::Rectangle<int> SettingsPage::mainArea() const {
  return getLocalBounds().withTrimmedLeft(kSubNavWidth).reduced(kMarginX, kMarginY);
}

int SettingsPage::blurbHeight() const {
  return juce::jmax(20, settings::noteHeight(mainArea().getWidth(), sections[static_cast<size_t>(part)]->blurb(), 14.0f));
}

void SettingsPage::resized() {
  auto subnav = getLocalBounds().removeFromLeft(kSubNavWidth).reduced(12, 20);
  subnav.removeFromTop(16 + 8);
  for (auto& button : navButtons) {
    button->setBounds(subnav.removeFromTop(36));
    subnav.removeFromTop(4);
  }
  const auto main = mainArea();
  const int top = main.getY() + 34 + 4 + blurbHeight() + 24;
  viewport.setBounds(main.getX(), top, getWidth() - main.getX(), getHeight() - top);
  fitSection();
  prompt.setBounds(getLocalBounds());
}

void SettingsPage::fitSection() {
  SettingsSection& chosen = *sections[static_cast<size_t>(part)];
  const int width = mainArea().getWidth();
  if (width <= 0)
    return;
  const int height = chosen.heightFor(width) + kMarginY;
  if (chosen.getWidth() != width || chosen.getHeight() != height)
    chosen.setSize(width, height);
}

void SettingsPage::paint(juce::Graphics& g) {
  g.fillAll(colour::ground);
  // The list of sections.
  const auto subnav = getLocalBounds().removeFromLeft(kSubNavWidth);
  g.setColour(colour::surface);
  g.fillRect(subnav);
  g.setColour(colour::line);
  g.fillRect(subnav.getRight() - 1, 0, 1, getHeight());
  paint::label(g, "Settings", subnav.reduced(12, 20).withHeight(16).withTrimmedLeft(12));

  // The title and the line under it.
  const SettingsSection& chosen = *sections[static_cast<size_t>(part)];
  auto main = mainArea();
  g.setFont(type::title());
  g.setColour(colour::text);
  g.drawText(chosen.heading(), main.removeFromTop(34), juce::Justification::centredLeft, true);
  main.removeFromTop(4);
  settings::paintNote(g, main.removeFromTop(blurbHeight()), chosen.blurb(), 14.0f);
}

}  // namespace pedaleira::ui

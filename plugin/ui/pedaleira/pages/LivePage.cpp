#include "LivePage.h"

#include "../Paint.h"
#include "../Type.h"

namespace pedaleira::ui {

namespace {

constexpr int kBarHeight = 48;
constexpr int kBarPadding = 24;
constexpr int kTabGap = 8;
constexpr int kTabPadding = 16;
constexpr int kLooperPadding = 24;

const char* const kTabNames[] = {"Tuner", "Tempo", "Looper"};

}  // namespace

// One tab of the bar: 48 high, the chosen one underlined in iris.
class LivePage::TabButton final : public juce::Button {
public:
  explicit TabButton(const juce::String& name) : juce::Button(name) {
    setTitle(name);
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
  }

  int idealWidth() const {
    return juce::roundToInt(juce::GlyphArrangement::getStringWidth(type::bodyStrong(), getButtonText())) + 2 * kTabPadding + 2;
  }

  bool current = false;

  void paintButton(juce::Graphics& g, bool highlighted, bool) override {
    const auto area = getLocalBounds();
    g.setFont(type::bodyStrong());
    g.setColour(current || highlighted ? colour::text : colour::muted);
    g.drawText(getButtonText(), area.withTrimmedBottom(2), juce::Justification::centred, false);
    if (current) {
      g.setColour(colour::iris);
      g.fillRect(area.withTop(area.getBottom() - 2));
    }
    if (hasKeyboardFocus(false)) {
      g.setColour(colour::iris);
      g.drawRoundedRectangle(area.toFloat().reduced(3.0f, 5.0f), size::buttonRadius, 2.0f);
    }
  }
};

LivePage::LivePage(EngineClient& client) : engine(client), tuner(client), tempo(client) {
  setTitle("Live");
  for (size_t i = 0; i < tabButtons.size(); ++i) {
    tabButtons[i] = std::make_unique<TabButton>(kTabNames[i]);
    tabButtons[i]->onClick = [this, i] { showTab(static_cast<Tab>(i)); };
    addAndMakeVisible(*tabButtons[i]);
  }
  tabButtons[0]->current = true;
  tempo.onTempoChanged = [this] {
    if (onTempoChanged)
      onTempoChanged();
  };
  addAndMakeVisible(tuner);
  addChildComponent(tempo);
}

LivePage::~LivePage() {
  // The views stop their timers and let the tuner go as they are destroyed.
  tempo.onTempoChanged = nullptr;
}

void LivePage::showTab(Tab which) {
  tab = which;
  for (size_t i = 0; i < tabButtons.size(); ++i) {
    tabButtons[i]->current = static_cast<Tab>(i) == which;
    tabButtons[i]->repaint();
  }
  tuner.setVisible(which == Tab::tuner);
  tempo.setVisible(which == Tab::tempo);
  updateActive();
  repaint();
}

void LivePage::updateActive() {
  const bool showing = isVisible();
  tuner.setActive(showing && tab == Tab::tuner);
  tempo.setActive(showing && tab == Tab::tempo);
}

void LivePage::rigChanged() {
  if (!isVisible())
    return;
  if (tab == Tab::tuner)
    tuner.refresh();
  else if (tab == Tab::tempo)
    tempo.refresh();
}

void LivePage::visibilityChanged() { updateActive(); }

void LivePage::resized() {
  auto area = getLocalBounds();
  barArea = area.removeFromTop(kBarHeight);
  int x = barArea.getX() + kBarPadding;
  for (auto& button : tabButtons) {
    button->setBounds(x, barArea.getY(), button->idealWidth(), kBarHeight);
    x = button->getRight() + kTabGap;
  }
  tuner.setBounds(area);
  tempo.setBounds(area);
  looperArea = area.reduced(kLooperPadding);
}

void LivePage::paint(juce::Graphics& g) {
  g.fillAll(colour::ground);
  g.setColour(colour::line);
  g.fillRect(barArea.getX(), barArea.getBottom() - 1, barArea.getWidth(), 1);

  if (tab != Tab::looper)
    return;
  // The looper comes in a later build: its tab keeps the board's card.
  paint::panel(g, looperArea.toFloat(), colour::surface, size::panelRadius);
  auto inner = looperArea.reduced(28, 20);
  paint::label(g, "Looper", inner.removeFromTop(16));
  auto middle = looperArea.withSizeKeepingCentre(looperArea.getWidth() - 120, 90);
  g.setFont(type::heading());
  g.setColour(colour::text);
  g.drawText(live::kLater, middle.removeFromTop(30), juce::Justification::centred, false);
  middle.removeFromTop(8);
  g.setFont(type::body());
  g.setColour(colour::muted);
  g.drawFittedText("Record, overdub and play back loops from the footswitches. The controller numbers CC 48 to 61 stay "
                   "reserved for the looper.",
                   middle, juce::Justification::centredTop, 2);
}

}  // namespace pedaleira::ui

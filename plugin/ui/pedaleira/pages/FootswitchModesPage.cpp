#include "FootswitchModesPage.h"

#include "../Icons.h"
#include "../Paint.h"
#include "../Type.h"

namespace pedaleira::ui {

namespace {

constexpr int kPadding = 24;
constexpr int kGap = 16;
constexpr int kTopRow = 44;
constexpr int kBarHeight = 36;
constexpr int kColumnPitch = 112;
constexpr int kRowPitch = 108;
constexpr int kSwitchWidth = 100;

juce::String rowName(const juce::String& role) {
  return role == "preset" ? "Preset" : role == "stomp" ? "Stomp" : "Scene";
}

}  // namespace

FootswitchModesPage::FootswitchModesPage(EngineClient& client) : engine(client) {
  setTitle("Footswitch modes");
  setWantsKeyboardFocus(true);

  const char* modes[4] = {"preset", "scene", "stomp", "hybrid"};
  const char* titles[4] = {"Preset", "Scene", "Stomp", "Hybrid"};
  const char* subtitles[4] = {"Switches load presets, banks of 8", "Switches load scenes A-H of the current preset",
                              "Each switch toggles one or more blocks", "Top row and bottom row use different modes"};
  for (size_t c = 0; c < 4; ++c) {
    auto card = std::make_unique<Card>();
    card->mode = modes[c];
    card->title = titles[c];
    card->subtitle = subtitles[c];
    for (int i = 0; i < 8; ++i) {
      auto fs = std::make_unique<Footswitch>(FootswitchModel::letterOf(i));
      fs->setSize(Footswitch::Size::compact);
      // These show; a click on one is a click on its card.
      fs->setInterceptsMouseClicks(false, false);
      fs->setWantsKeyboardFocus(false);
      addAndMakeVisible(*fs);
      card->switches[static_cast<size_t>(i)] = std::move(fs);
    }
    const char* names[3] = {"Bank Up", "Bank Down", "Tap"};
    for (int i = 0; i < 3; ++i) {
      auto fs = std::make_unique<Footswitch>(names[i]);
      fs->setSize(Footswitch::Size::mini);
      fs->setLabel(names[i]);
      fs->setInterceptsMouseClicks(false, false);
      fs->setWantsKeyboardFocus(false);
      if (i == 0)
        fs->setIcon(&icons::path(icons::Icon::chevronUp));
      else if (i == 1)
        fs->setIcon(&icons::path(icons::Icon::chevronDown));
      else
        fs->setLook(Footswitch::Look::tap);
      addAndMakeVisible(*fs);
      card->util[static_cast<size_t>(i)] = std::move(fs);
    }
    cards[c] = std::move(card);
  }

  back.setIcon(&icons::path(icons::Icon::arrowLeft));
  back.onClick = [this] {
    if (onBack)
      onBack();
  };
  topRow.setTitle("Top row mode");
  bottomRow.setTitle("Bottom row mode");
  topRow.onClick = [this] { chooseRow(true); };
  bottomRow.onClick = [this] { chooseRow(false); };
  for (juce::Component* c : std::initializer_list<juce::Component*>{&back, &topRow, &bottomRow})
    addAndMakeVisible(c);
  refresh();
}

FootswitchModesPage::~FootswitchModesPage() = default;

void FootswitchModesPage::setMode(const juce::String& mode) {
  if (mode == model.mode)
    return;
  engine.run("footswitch.setMode", object({{"mode", mode}}));
  refresh();
}

void FootswitchModesPage::chooseRow(bool top) {
  juce::PopupMenu menu;
  juce::Component::SafePointer<FootswitchModesPage> self(this);
  const juce::String current = top ? model.hybridTop : model.hybridBottom;
  for (const char* role : {"scene", "preset", "stomp"})
    menu.addItem(rowName(role), true, current == role, [self, top, role] {
      if (self == nullptr)
        return;
      const juce::String other = top ? self->model.hybridBottom : self->model.hybridTop;
      self->engine.run("footswitch.setHybridRows", object({{"top", top ? juce::String(role) : other},
                                                           {"bottom", top ? other : juce::String(role)}}));
      self->refresh();
    });
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(top ? &topRow : &bottomRow));
}

juce::String FootswitchModesPage::ccText(const char* action) const {
  if (const auto* list = midiDefaults.getArray())
    for (const auto& entry : *list)
      if (entry["action"].toString() == action) {
        const int cc = entry["cc"];
        return cc >= 0 ? "CC " + juce::String(cc) : juce::String("off");
      }
  return {};
}

void FootswitchModesPage::refresh() {
  model = FootswitchModel::read(engine);
  midiDefaults = engine.read("midi.getMap")["defaults"];

  for (auto& card : cards) {
    const auto faces = model.faces(card->mode);
    for (size_t i = 0; i < 8; ++i)
      applyFace(*card->switches[i], faces[i]);
    card->util[2]->setText(juce::String(juce::roundToInt(model.bpm)));

    if (card->mode == "preset") {
      card->pillNumber.clear();
      card->pillText = model.bankLine();
    } else if (card->mode == "scene") {
      card->pillNumber = model.presetSlot > 0 ? juce::String(model.presetSlot).paddedLeft('0', 3) : juce::String();
      card->pillText = (model.presetName.isNotEmpty() ? model.presetName : juce::String("New preset")) + " " +
                       type::dot() + " scenes A-H";
    } else if (card->mode == "stomp") {
      card->pillNumber = juce::String(model.stompsAssigned()) + " / 8";
      card->pillText = "switches assigned";
    }
  }
  topRow.setButtonText("Top row: " + rowName(model.hybridTop));
  bottomRow.setButtonText("Bottom row: " + rowName(model.hybridBottom));
  resized();
  repaint();
}

void FootswitchModesPage::resized() {
  auto area = getLocalBounds().reduced(kPadding);
  titleArea = area.removeFromTop(32);
  back.setBounds(titleArea.getRight() - back.idealWidth(), titleArea.getY(), back.idealWidth(), 32);
  area.removeFromTop(12);
  barArea = area.removeFromBottom(kBarHeight);
  area.removeFromBottom(kGap);

  const int cardWidth = (area.getWidth() - kGap) / 2;
  const int cardHeight = (area.getHeight() - kGap) / 2;
  for (size_t c = 0; c < 4; ++c) {
    Card& card = *cards[c];
    card.box = {area.getX() + static_cast<int>(c % 2) * (cardWidth + kGap),
                area.getY() + static_cast<int>(c / 2) * (cardHeight + kGap), cardWidth, cardHeight};
    const int left = card.box.getX() + 20;
    const int top = card.box.getY() + 16 + 44 + 12;
    const int height = Footswitch::heightFor(Footswitch::Size::compact);
    for (int i = 0; i < 8; ++i)
      card.switches[static_cast<size_t>(i)]->setBounds(left + (i % 4) * kColumnPitch, top + (i / 4) * kRowPitch,
                                                       kSwitchWidth, height);
    const int utilLeft = left + 4 * kColumnPitch - 12 + 28;
    const int utilHeight = Footswitch::heightFor(Footswitch::Size::mini);
    for (int i = 0; i < 3; ++i)
      card.util[static_cast<size_t>(i)]->setBounds(utilLeft, top + i * (utilHeight + 6), 64, utilHeight);
    card.pill = {left, top + 2 * kRowPitch + 2, 0, 28};
    // The pill is as wide as its text.
    int width = 20;
    if (card.pillNumber.isNotEmpty())
      width += juce::roundToInt(juce::GlyphArrangement::getStringWidth(type::mono(12.0f), card.pillNumber)) + 8;
    const juce::Font textFont = card.mode == "preset" ? type::mono(12.0f) : type::sans(12.0f);
    width += juce::roundToInt(juce::GlyphArrangement::getStringWidth(textFont, card.pillText)) + 2;
    card.pill.setWidth(width);
  }

  // The hybrid card's two selectors take the pill's place.
  const Card& hybrid = *cards[3];
  const int selectorTop = hybrid.pill.getY();
  topRow.setBounds(hybrid.pill.getX(), selectorTop, topRow.idealWidth() + 22, 28);
  bottomRow.setBounds(topRow.getRight() + 10, selectorTop, bottomRow.idealWidth() + 22, 28);
}

void FootswitchModesPage::mouseUp(const juce::MouseEvent& event) {
  if (!event.mouseWasClicked())
    return;
  for (const auto& card : cards)
    if (card->box.contains(event.getPosition()))
      setMode(card->mode);
}

bool FootswitchModesPage::keyPressed(const juce::KeyPress& key) {
  if (key == juce::KeyPress::escapeKey && onBack) {
    onBack();
    return true;
  }
  return false;
}

void FootswitchModesPage::paint(juce::Graphics& g) {
  g.fillAll(colour::ground);
  g.setFont(type::heading());
  g.setColour(colour::text);
  g.drawText("Footswitch modes", titleArea.withTrimmedRight(200), juce::Justification::centredLeft, true);

  for (const auto& card : cards) {
    const bool current = card->mode == model.mode;
    const auto box = card->box.toFloat();
    g.setColour(colour::surface);
    g.fillRoundedRectangle(box, size::panelRadius);
    g.setColour(current ? colour::iris : colour::line);
    g.drawRoundedRectangle(box.reduced(0.5f), size::panelRadius, 1.0f);
    if (current)
      g.drawRoundedRectangle(box.reduced(1.5f), size::panelRadius - 1.0f, 1.0f);

    g.setFont(type::heading());
    g.setColour(colour::text);
    g.drawText(card->title, card->box.getX() + 20, card->box.getY() + 16, 300, 26, juce::Justification::centredLeft, true);
    g.setFont(type::compact());
    g.setColour(colour::muted);
    g.drawText(card->subtitle, card->box.getX() + 20, card->box.getY() + 42, card->box.getWidth() - 40, 18,
               juce::Justification::centredLeft, true);
    if (current)
      paint::label(g, "Current mode", {card->box.getRight() - 20 - 200, card->box.getY() + 20, 200, 14},
                   juce::Justification::centredRight, colour::iris);

    if (card->mode == "hybrid")
      continue;
    const auto pill = card->pill.toFloat();
    g.setColour(colour::raised);
    g.fillRoundedRectangle(pill, size::buttonRadius);
    g.setColour(colour::line);
    g.drawRoundedRectangle(pill.reduced(0.5f), size::buttonRadius, 1.0f);
    auto inner = card->pill.reduced(10, 0);
    if (card->pillNumber.isNotEmpty()) {
      const int numberWidth =
          juce::roundToInt(juce::GlyphArrangement::getStringWidth(type::mono(12.0f), card->pillNumber)) + 8;
      g.setFont(type::mono(12.0f));
      g.setColour(colour::text);
      g.drawText(card->pillNumber, inner.removeFromLeft(numberWidth), juce::Justification::centredLeft, false);
    }
    g.setFont(card->mode == "preset" ? type::mono(12.0f) : type::sans(12.0f));
    g.setColour(colour::text);
    g.drawText(card->pillText, inner, juce::Justification::centredLeft, true);
  }

  // The MIDI conventions, from the engine's defaults.
  paint::panel(g, barArea.toFloat(), colour::surface, size::panelRadius);
  auto bar = barArea.reduced(16, 0);
  g.setFont(type::compact());
  g.setColour(colour::muted);
  g.drawText("Mode switch: " + ccText("footswitchMode") + " (values 0 to 3 pick Preset, Scene, Stomp, Hybrid)",
             bar.removeFromLeft(bar.getWidth() / 2), juce::Justification::centredLeft, true);
  g.setFont(type::sans(12.0f));
  g.drawText("Scenes " + ccText("sceneSelect") + " " + type::dot() + " Tap " + ccText("tapTempo") + " " + type::dot() +
                 " Tuner " + ccText("tuner"),
             bar, juce::Justification::centredRight, true);
}

}  // namespace pedaleira::ui

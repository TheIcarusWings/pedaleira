#include "BlockSettingsPanel.h"

#include "../Icons.h"
#include "../Paint.h"
#include "../Type.h"
#include "../widgets/Motion.h"
#include "EffectEditor.h"
#include "IrEditor.h"
#include "NamEditor.h"

#include <cmath>

namespace pedaleira::ui {

namespace {

constexpr int kRule = 3;
constexpr int kHeaderHeight = 60;
constexpr int kPadX = 24;
constexpr int kPictureWidth = 272;
constexpr int kBodyGap = 16;
// The capture and IR editors lay out for a page; in the panel they scroll.
constexpr int kFullEditorWidth = 1000;
constexpr int kFullEditorHeight = 520;
constexpr int kScrollBar = 8;

juce::String whatIs(const juce::String& kindId) {
  if (kindId == "nam")
    return "NAM capture";
  if (kindId == "ir")
    return "Impulse response";
  if (kindId == "plugin")
    return "Hosted plugin";
  if (kindId.startsWith("amp."))
    return "Built-in model";
  return "Built-in effect";
}

juce::String midiNoteName(int note) {
  static const char* const names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  return juce::String(names[((note % 12) + 12) % 12]) + juce::String(note / 12 - 1);
}

}  // namespace

// The area under the header: the picture and the viewport.
struct BlockSettingsPanel::Content : public juce::Component {
  std::function<void()> onResized;
  void resized() override {
    if (onResized)
      onResized();
  }
};

BlockSettingsPanel::BlockSettingsPanel(EngineClient& client) : engine(client), content(std::make_unique<Content>()) {
  setTitle("Block settings");
  content->onResized = [this] { layoutBody(); };
  content->addAndMakeVisible(viewport);
  viewport.setScrollBarsShown(true, true);
  viewport.setScrollBarThickness(kScrollBar);
  addAndMakeVisible(*content);

  powered.onClick = [this] {
    engine.run("block.setEnabled", object({{"blockId", context.blockId}, {"enabled", powered.getToggleState()}}));
    if (onChanged)
      onChanged();
  };
  assign.onClick = [this] {
    showFootswitchMenu(engine, context.blockId, assign, [this] {
      if (onChanged)
        onChanged();
    });
  };
  replace.onClick = [this] {
    if (onReplace)
      onReplace(context.blockId);
  };
  openEditor.onClick = [this] {
    if (onOpenEditor)
      onOpenEditor(context.blockId);
  };
  chevron.setKind(PushButton::Kind::ghost);
  chevron.setTitle("Collapse the settings");
  chevron.setIcon(&icons::path(icons::Icon::chevronDown));
  chevron.onClick = [this] {
    setCollapsed(!collapsed);
    if (onCollapsedChanged)
      onCollapsedChanged(collapsed);
  };
  for (juce::Component* c : std::initializer_list<juce::Component*>{&powered, &assign, &replace, &openEditor, &chevron})
    addChildComponent(c);
  chevron.setVisible(true);
}

BlockSettingsPanel::~BlockSettingsPanel() { viewport.setViewedComponent(nullptr, false); }

void BlockSettingsPanel::setCollapsed(bool shouldCollapse) {
  collapsed = shouldCollapse;
  chevron.setIcon(&icons::path(collapsed ? icons::Icon::chevronUp : icons::Icon::chevronDown));
  chevron.setTitle(collapsed ? "Expand the settings" : "Collapse the settings");
  content->setVisible(!collapsed && shown);
  updateButtons();
  resized();
  repaint();
}

void BlockSettingsPanel::updateButtons() {
  powered.setVisible(shown);
  for (juce::Component* c : std::initializer_list<juce::Component*>{&assign, &replace, &openEditor})
    c->setVisible(shown && !collapsed);
}

juce::Rectangle<int> BlockSettingsPanel::contentBounds() const { return content->getBounds(); }

std::optional<double> BlockSettingsPanel::shownValue(const juce::String& paramId) const {
  if (faceplate != nullptr)
    return faceplate->shownValue(paramId);
  if (effect != nullptr)
    for (const auto& spec : effect->currentParams())
      if (spec.id == paramId)
        return spec.value;
  return std::nullopt;
}

void BlockSettingsPanel::clearBlock() {
  shown = false;
  context = {};
  viewport.setViewedComponent(nullptr, false);
  effect = nullptr;
  faceplate = nullptr;
  body.reset();
  bodyKind.clear();
  visual.reset();
  readings.clear();
  updateButtons();
  content->setVisible(false);
  resized();
  repaint();
}

void BlockSettingsPanel::readKinds() {
  if (kindsRead)
    return;
  kindsRead = true;
  const juce::var listed = engine.read("kinds.list");
  if (const auto* kinds = listed["kinds"].getArray())
    for (const auto& kind : *kinds)
      if (const auto* list = kind["readings"].getArray())
        for (const auto& item : *list) {
          ReadingSpec spec;
          spec.id = item["id"].toString();
          spec.name = item["name"].toString();
          spec.unit = item["unit"].toString();
          spec.lo = item["min"];
          spec.hi = item["max"];
          readingSpecs[kind["id"].toString()].push_back(spec);
        }
}

void BlockSettingsPanel::makeBody() {
  const juce::String kind = context.kindId == "nam" ? "nam"
                            : context.kindId == "ir"      ? "ir"
                            : context.kindId == "plugin"  ? "plugin"
                            : context.kindId.startsWith("amp.") ? "amp"
                                                                : "effect";
  viewport.setViewedComponent(nullptr, false);
  effect = nullptr;
  faceplate = nullptr;
  body.reset();
  visual.reset();
  bodyKind = kind;
  if (kind == "plugin")
    return;
  if (kind == "amp") {
    // The faceplate is the control surface: no sections, no picture.
    auto plate = std::make_unique<AmpFaceplate>(engine);
    faceplate = plate.get();
    content->addAndMakeVisible(*plate);
    plate->onChanged = [this] {
      if (onChanged)
        onChanged();
    };
    body = std::move(plate);
    return;
  }
  if (kind == "nam") {
    body = std::make_unique<NamEditor>(engine);
  } else if (kind == "ir") {
    body = std::make_unique<IrEditor>(engine);
  } else {
    auto editor = std::make_unique<EffectEditor>(engine);
    editor->setStyle({true, accent});
    effect = editor.get();
    visual = makeBlockVisual(context.category, context.kindId);
    if (visual != nullptr) {
      visual->setAccent(accent);
      content->addAndMakeVisible(*visual);
    }
    editor->onParamsChanged = [this](const std::vector<KnobParam>& specs) {
      if (visual != nullptr)
        visual->setParams(specs, context.enabled);
    };
    body = std::move(editor);
  }
  body->onChanged = [this] {
    if (onChanged)
      onChanged();
  };
  body->onReplace = [this](int) {
    if (onReplace)
      onReplace(context.blockId);
  };
  viewport.setViewedComponent(body.get(), false);
}

void BlockSettingsPanel::showBlock(const GridModel& model, const juce::String& id) {
  BlockContext next;
  if (id.isEmpty() || !readBlockContext(engine, model, id, next)) {
    if (shown || context.blockId.isNotEmpty())
      clearBlock();
    return;
  }
  readKinds();
  const bool other = !shown || next.blockId != context.blockId;
  context = next;
  shown = true;
  accent = categoryColour(context.category);
  if (other)
    readings.clear();
  if (other || body == nullptr) {
    makeBody();
    viewport.setViewPosition(0, 0);
  }
  powered.setToggleState(context.enabled, juce::dontSendNotification);
  updateButtons();
  content->setVisible(!collapsed);
  if (body != nullptr) {
    if (faceplate == nullptr)
      body->setSize(juce::jmax(100, viewport.getWidth() - kScrollBar), juce::jmax(100, viewport.getHeight()));
    body->showBlock(context);
  }
  if (visual != nullptr)
    visual->setAccent(accent);
  if (effect != nullptr && visual != nullptr)
    visual->setTempo(effect->tempoBpm());
  resized();
  layoutBody();
  repaint();
  if (other && !collapsed)
    motion::fadeIn(*content, motion::panelMs);
}

// ####################
// LIVE
// ####################
void BlockSettingsPanel::showMeters(const juce::var& meters) {
  if (!shown || collapsed)
    return;
  const BlockLive live = BlockLive::fromVar(meters["blocks"][juce::Identifier(context.blockId)]);
  if (visual != nullptr)
    visual->setLive(live);
  if (faceplate != nullptr)
    faceplate->setLive(live);
  if (body != nullptr)
    body->tick();
  bool changed = live.readings.size() != readings.size();
  for (const auto& [key, value] : live.readings) {
    const auto found = readings.find(key);
    if (found == readings.end() || std::abs(found->second - value) > 0.05)
      changed = true;
  }
  if (changed) {
    readings = live.readings;
    repaint(readingsBounds);
  }
}

juce::String BlockSettingsPanel::readingText(const ReadingSpec& spec, double value) const {
  if (spec.id.containsIgnoreCase("note"))
    return value >= 0.0 ? midiNoteName(juce::roundToInt(value)) : juce::String("--");
  if (spec.unit.isNotEmpty())
    return juce::String(value, 1) + " " + spec.unit;
  if (spec.hi <= 1.0 + 1.0e-6 && spec.lo >= 0.0)
    return juce::String(juce::roundToInt(value * 100.0)) + "%";
  return juce::String(value, 1);
}

// ####################
// LAYOUT AND PAINT
// ####################
juce::String BlockSettingsPanel::kindLine() const {
  return whatIs(context.kindId) + " " + type::dot() + " Row " + juce::String(context.row + 1) + ", slot " +
         juce::String(context.slot + 1);
}

juce::Rectangle<int> BlockSettingsPanel::headerArea() const {
  return getLocalBounds().withTrimmedTop(kRule).withHeight(collapsed ? kBarHeight - kRule : kHeaderHeight);
}

void BlockSettingsPanel::resized() {
  auto header = headerArea().withTrimmedRight(kPadX - 8);
  chevron.setBounds(header.removeFromRight(36).withSizeKeepingCentre(36, 32));
  header.removeFromRight(8);
  if (shown) {
    if (!collapsed) {
      openEditor.setBounds(header.removeFromRight(openEditor.idealWidth()).withSizeKeepingCentre(openEditor.idealWidth(), 32));
      header.removeFromRight(8);
      replace.setBounds(header.removeFromRight(replace.idealWidth()).withSizeKeepingCentre(replace.idealWidth(), 32));
      header.removeFromRight(8);
      assign.setBounds(header.removeFromRight(assign.idealWidth()).withSizeKeepingCentre(assign.idealWidth(), 32));
      header.removeFromRight(16);
    }
    const int textWidth = 112;
    powered.setBounds(header.removeFromRight(textWidth + 48).withWidth(44).withSizeKeepingCentre(44, 28));
    header.removeFromRight(12);
    // The readings sit between the name and the switch.
    const int nameWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(type::sans(22.0f, type::Weight::bold), context.name));
    const int kindWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(
        type::sans(12.0f), categoryName(context.category) + "  " + type::dot() + " " + kindLine()));
    const int left = kPadX + (collapsed ? 28 + 10 : 44 + 14) + juce::jmax(nameWidth, collapsed ? 0 : kindWidth) + 28;
    // An amp shows its sag in the strip of its faceplate.
    readingsBounds = collapsed || faceplate != nullptr ? juce::Rectangle<int>()
                                                       : juce::Rectangle<int>(left, header.getY(), juce::jmax(0, header.getRight() - left), header.getHeight());
  } else {
    readingsBounds = {};
  }
  auto area = getLocalBounds().withTrimmedTop(kRule + kHeaderHeight).reduced(kPadX, 0).withTrimmedBottom(14);
  area.removeFromTop(2);
  content->setBounds(area);
}

void BlockSettingsPanel::layoutBody() {
  auto area = content->getLocalBounds();
  if (faceplate != nullptr) {
    // The whole area is the faceplate; there is nothing to scroll.
    faceplate->setBounds(area);
    return;
  }
  if (visual != nullptr) {
    visual->setBounds(area.removeFromLeft(kPictureWidth));
    area.removeFromLeft(kBodyGap);
  }
  viewport.setBounds(area);
  if (body == nullptr)
    return;
  const bool full = effect == nullptr;
  const int width = full ? juce::jmax(kFullEditorWidth, area.getWidth() - kScrollBar) : juce::jmax(100, area.getWidth() - kScrollBar);
  int height = juce::jmax(100, area.getHeight());
  if (full) {
    height = juce::jmax(height, kFullEditorHeight);
    body->setSize(width, height);
  } else {
    body->setSize(width, height);
    body->setSize(width, juce::jmax(height, effect->getContentHeight()));
  }
}

void BlockSettingsPanel::paint(juce::Graphics& g) {
  g.fillAll(colour::surface);
  g.setColour(colour::line);
  g.fillRect(0, 0, getWidth(), 1);
  auto header = headerArea();
  if (!shown) {
    g.setFont(type::body());
    g.setColour(colour::muted);
    if (collapsed) {
      g.drawText("Block settings", header.withTrimmedLeft(kPadX), juce::Justification::centredLeft, false);
      return;
    }
    auto text = getLocalBounds().reduced(kPadX, 0).withTrimmedTop(kRule + 18);
    g.setFont(type::heading());
    g.setColour(colour::text);
    g.drawText("Select a block to see its settings", text.removeFromTop(28), juce::Justification::centredLeft, false);
    g.setFont(type::compact());
    g.setColour(colour::muted);
    g.drawText("Click a block on the grid, or move between blocks with the arrow keys. Escape clears the selection.",
               text.removeFromTop(22), juce::Justification::centredLeft, true);
    return;
  }
  // The category colour across the top, then the identity.
  g.setColour(accent);
  g.fillRect(0, 0, getWidth(), kRule);
  if (!collapsed) {
    g.setColour(accent.withAlpha(0.08f));
    g.fillRect(0, kRule, getWidth(), kHeaderHeight);
  }
  g.setColour(colour::line);
  g.fillRect(0, kRule + (collapsed ? kBarHeight - kRule : kHeaderHeight) - 1, getWidth(), 1);

  const int iconSize = collapsed ? 28 : 44;
  auto row = header.withTrimmedLeft(kPadX);
  const auto icon = row.removeFromLeft(iconSize).withSizeKeepingCentre(iconSize, iconSize).toFloat();
  g.setColour(categoryTint(context.category));
  g.fillRoundedRectangle(icon, size::panelRadius);
  g.setColour(accent.withAlpha(0.5f));
  g.drawRoundedRectangle(icon.reduced(0.5f), size::panelRadius, 1.0f);
  icons::draw(g, icons::path(context.category), icon.reduced(iconSize * 0.2f), accent);
  row.removeFromLeft(collapsed ? 10 : 14);

  const juce::Font nameFont = type::sans(collapsed ? 15.0f : 22.0f, type::Weight::bold);
  const int nameWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(nameFont, context.name)) + 4;
  g.setFont(nameFont);
  g.setColour(colour::text);
  if (collapsed) {
    g.drawText(context.name, row.withWidth(nameWidth), juce::Justification::centredLeft, true);
    g.setFont(type::sans(12.0f));
    g.setColour(accent);
    g.drawText(categoryName(context.category), row.withTrimmedLeft(nameWidth + 12).withWidth(120), juce::Justification::centredLeft, false);
  } else {
    auto lines = row.withWidth(juce::jmax(nameWidth, 200)).withSizeKeepingCentre(juce::jmax(nameWidth, 200), 46);
    g.drawText(context.name, lines.removeFromTop(28), juce::Justification::centredLeft, true);
    g.setFont(type::sans(12.0f, type::Weight::semibold));
    g.setColour(accent);
    const juce::String category = categoryName(context.category);
    const int categoryWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), category));
    g.drawText(category, lines.withHeight(16), juce::Justification::centredLeft, false);
    g.setFont(type::sans(12.0f));
    g.setColour(colour::muted);
    g.drawText(" " + type::dot() + " " + kindLine(), lines.withTrimmedLeft(categoryWidth).withHeight(16).withWidth(420),
               juce::Justification::centredLeft, false);
  }

  // On or off in the active scene, beside the switch.
  g.setFont(type::compact());
  g.setColour(colour::text);
  g.drawText((powered.getToggleState() ? "On in scene " : "Off in scene ") + context.activeScene, powered.getRight() + 8, powered.getY(),
             112, powered.getHeight(), juce::Justification::centredLeft, false);

  // The readings the kind reports, live.
  if (!readingsBounds.isEmpty()) {
    const auto found = readingSpecs.find(context.kindId);
    int x = readingsBounds.getX();
    if (found != readingSpecs.end())
      for (const ReadingSpec& spec : found->second) {
        const auto value = readings.find(spec.id);
        const juce::String text = readingText(spec, value != readings.end() ? value->second : spec.lo);
        const int labelWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(type::sans(11.0f), spec.name));
        const int valueWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(type::mono(12.0f), text));
        const int width = labelWidth + valueWidth + 28;
        if (x + width > readingsBounds.getRight())
          break;
        const auto chip = juce::Rectangle<int>(x, readingsBounds.getCentreY() - 14, width, 28).toFloat();
        g.setColour(colour::ground);
        g.fillRoundedRectangle(chip, 8.0f);
        g.setColour(colour::line);
        g.drawRoundedRectangle(chip.reduced(0.5f), 8.0f, 1.0f);
        g.setFont(type::sans(11.0f));
        g.setColour(colour::muted);
        g.drawText(spec.name, chip.toNearestInt().withTrimmedLeft(10).withWidth(labelWidth + 2), juce::Justification::centredLeft, false);
        g.setFont(type::mono(12.0f));
        g.setColour(value != readings.end() ? accent : colour::dim);
        g.drawText(text, chip.toNearestInt().withTrimmedRight(10), juce::Justification::centredRight, false);
        x += width + 8;
      }
  }

  if (!collapsed && bodyKind == "plugin") {
    g.setFont(type::body());
    g.setColour(colour::muted);
    g.drawText("This plugin has its own window. Double-click the block, or press Full editor, to open it.",
               content->getBounds(), juce::Justification::centredLeft, true);
  }
}

}  // namespace pedaleira::ui

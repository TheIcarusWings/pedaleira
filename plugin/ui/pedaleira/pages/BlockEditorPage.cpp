#include "BlockEditorPage.h"

#include "../Icons.h"
#include "../Paint.h"
#include "../Type.h"
#include "EffectEditor.h"
#include "GridPage.h"
#include "IrEditor.h"
#include "NamEditor.h"

namespace pedaleira::ui {

namespace {

constexpr int kScenesHeight = 56;
constexpr int kSignalHeight = 100;
constexpr int kPadX = 24;
constexpr int kPadY = 20;
constexpr int kHeaderHeight = 40;
constexpr int kHeaderGap = 16;
constexpr int kEndBoxWidth = 84;
constexpr int kChipHeight = 56;
constexpr int kLinkWidth = 16;
constexpr int kSignalLabelWidth = 56;
constexpr int kChipWidth = 120;

juce::String whatIs(const juce::String& kindId) {
  if (kindId == "nam")
    return "NAM capture";
  if (kindId == "ir")
    return "Impulse response";
  if (kindId == "plugin")
    return "Hosted plugin";
  return "Built-in effect";
}

}  // namespace

// ####################
// SIGNAL ROW
// ####################
void SignalRow::setRow(const GridRow& source, const juce::String& selectedBlock) {
  row = source.index;
  inputName = source.inputName;
  outputName = source.outputName;
  chips.clear();
  for (const auto& slot : source.slots) {
    if (slot.empty)
      continue;
    Chip chip;
    chip.blockId = slot.blockId;
    chip.name = slot.name;
    chip.category = slot.category;
    chip.enabled = slot.enabled;
    chip.selected = slot.blockId == selectedBlock;
    chips.push_back(chip);
  }
  repaint();
}

int SignalRow::chipWidth() const {
  const int count = juce::jmax(1, static_cast<int>(chips.size()));
  const int room = getWidth() - 2 * kPadX - kSignalLabelWidth - 16 - 2 * kEndBoxWidth - (count + 1) * kLinkWidth - 8;
  return juce::jlimit(80, kChipWidth, room / count);
}

juce::Rectangle<int> SignalRow::chipBounds(int index) const {
  const int left = kPadX + kSignalLabelWidth + 16 + kEndBoxWidth + kLinkWidth;
  const int width = chipWidth();
  return {left + index * (width + kLinkWidth), (getHeight() - kChipHeight) / 2, width, kChipHeight};
}

void SignalRow::paint(juce::Graphics& g) {
  g.fillAll(colour::ground);
  g.setColour(colour::line);
  g.fillRect(0, getHeight() - 1, getWidth(), 1);

  auto label = juce::Rectangle<int>(kPadX, 0, kSignalLabelWidth, getHeight()).withSizeKeepingCentre(kSignalLabelWidth, 34);
  paint::label(g, "Signal", label.removeFromTop(16));
  g.setFont(type::mono(12.0f, type::Weight::regular));
  g.setColour(colour::text);
  g.drawText("Row " + juce::String(row + 1), label.removeFromTop(18), juce::Justification::centredLeft, false);

  const int centre = getHeight() / 2;
  const auto link = [&](int x) {
    g.setColour(colour::faint);
    g.fillRect(x, centre - 1, kLinkWidth, 2);
  };
  const auto endBox = [&](int x, const juce::String& tag, const juce::String& name) {
    const auto box = juce::Rectangle<int>(x, (getHeight() - kChipHeight) / 2, kEndBoxWidth, kChipHeight);
    paint::panel(g, box.toFloat());
    auto inner = box.reduced(10, 0);
    g.setFont(type::sans(10.0f, type::Weight::semibold));
    g.setColour(colour::muted);
    g.drawText(tag, inner.withTrimmedTop(14).withHeight(12), juce::Justification::centredLeft, false);
    g.setFont(type::sans(12.0f, type::Weight::semibold));
    g.setColour(colour::text);
    g.drawText(name, inner.withTrimmedTop(26).withHeight(16), juce::Justification::centredLeft, true);
  };
  const int start = kPadX + kSignalLabelWidth + 16;
  endBox(start, "IN", inputName);
  for (size_t i = 0; i < chips.size(); ++i) {
    const auto at = chipBounds(static_cast<int>(i));
    link(at.getX() - kLinkWidth);
    const Chip& chip = chips[i];
    const auto area = at.toFloat();
    g.setColour(colour::drop);
    g.fillRoundedRectangle(area.translated(0.0f, size::dropDepth), size::panelRadius);
    g.setColour(chip.enabled ? categoryTint(chip.category) : colour::raised);
    g.fillRoundedRectangle(area, size::panelRadius);
    if (!chip.enabled)
      paint::dashedOutline(g, area, size::panelRadius, colour::faint);
    if (chip.selected) {
      g.setColour(colour::iris);
      g.drawRoundedRectangle(area.expanded(3.0f), size::panelRadius + 3.0f, 2.0f);
    }
    auto inner = at.reduced(10, 0);
    const auto icon = inner.removeFromLeft(20).withSizeKeepingCentre(20, 20).toFloat();
    icons::draw(g, icons::path(chip.category), icon, chip.enabled ? categoryColour(chip.category) : colour::dim);
    inner.removeFromLeft(8);
    g.setFont(type::sans(12.0f, type::Weight::semibold));
    g.setColour(chip.enabled ? colour::text : colour::muted);
    g.drawText(chip.name, inner.withTrimmedTop(kChipHeight / 2 - 15).withHeight(14), juce::Justification::centredLeft, true);
    g.setFont(type::sans(11.0f));
    g.setColour(colour::muted);
    g.drawText(categoryName(chip.category), inner.withTrimmedTop(kChipHeight / 2 + 1).withHeight(14),
               juce::Justification::centredLeft, true);
  }
  const int end = chips.empty() ? start + kEndBoxWidth : chipBounds(static_cast<int>(chips.size()) - 1).getRight();
  link(end);
  endBox(end + kLinkWidth, "OUT", outputName);
}

void SignalRow::mouseUp(const juce::MouseEvent& event) {
  for (size_t i = 0; i < chips.size(); ++i)
    if (chipBounds(static_cast<int>(i)).contains(event.getPosition()) && onSelect) {
      onSelect(chips[i].blockId);
      return;
    }
}

// ####################
// HEADER
// ####################
// The block's name and kind, whether it is on in this scene, and what can
// be done to the block as a whole.
class BlockEditorPage::Header : public juce::Component {
public:
  explicit Header(EngineClient& client) : engine(client) {
    enabled.onClick = [this] {
      engine.run("block.setEnabled", object({{"blockId", blockId}, {"enabled", enabled.getToggleState()}}));
      if (onChanged)
        onChanged();
    };
    assign.onClick = [this] {
      showFootswitchMenu(engine, blockId, assign, [this] {
        if (onChanged)
          onChanged();
      });
    };
    replace.onClick = [this] {
      if (onReplace)
        onReplace();
    };
    compare.setTitle("A/B compare");
    compare.setEnabled(false);
    compare.setTooltip(kLaterBuild);
    for (juce::Component* c : std::initializer_list<juce::Component*>{&enabled, &assign, &replace, &compare})
      addAndMakeVisible(c);
  }

  void show(const BlockContext& block) {
    blockId = block.blockId;
    title = block.name;
    category = block.category;
    activeScene = block.activeScene;
    subtitle = categoryName(block.category) + " " + type::dot() + " " + whatIs(block.kindId) + " " + type::dot() +
               " Row " + juce::String(block.row + 1) + ", slot " + juce::String(block.slot + 1);
    enabled.setToggleState(block.enabled, juce::dontSendNotification);
    repaint();
  }

  std::function<void()> onChanged;
  std::function<void()> onReplace;

  void paint(juce::Graphics& g) override {
    auto head = getLocalBounds();
    const auto icon = head.removeFromLeft(40).toFloat();
    g.setColour(categoryTint(category));
    g.fillRoundedRectangle(icon, size::panelRadius);
    icons::draw(g, icons::path(category), icon.reduced(9.0f), categoryColour(category));
    head.removeFromLeft(12);
    g.setFont(type::heading());
    g.setColour(colour::text);
    g.drawText(title, head.removeFromTop(26), juce::Justification::centredLeft, true);
    g.setFont(type::sans(12.0f));
    g.setColour(colour::muted);
    g.drawText(subtitle, head, juce::Justification::centredLeft, true);

    g.setFont(type::compact());
    g.setColour(colour::text);
    g.drawText((enabled.getToggleState() ? "On in scene " : "Off in scene ") + activeScene, enabled.getRight() + 8,
               enabled.getY(), 116, enabled.getHeight(), juce::Justification::centredLeft, false);
    g.setFont(type::sans(12.0f, type::Weight::semibold));
    g.setColour(colour::muted);
    g.drawText("A/B compare", compareLabel, juce::Justification::centredRight, false);
  }

  void resized() override {
    auto area = getLocalBounds();
    compare.setBounds(area.removeFromRight(compare.idealWidth()).withSizeKeepingCentre(compare.idealWidth(), 36));
    area.removeFromRight(8);
    compareLabel = area.removeFromRight(80).withSizeKeepingCentre(80, 16);
    area.removeFromRight(12);
    replace.setBounds(area.removeFromRight(replace.idealWidth()).withSizeKeepingCentre(replace.idealWidth(), 32));
    area.removeFromRight(12);
    assign.setBounds(area.removeFromRight(assign.idealWidth()).withSizeKeepingCentre(assign.idealWidth(), 32));
    area.removeFromRight(12);
    enabled.setBounds(area.removeFromRight(40 + 8 + 116).withWidth(40).withSizeKeepingCentre(40, 28));
  }

private:
  EngineClient& engine;
  juce::String blockId, title, subtitle, activeScene;
  Category category = Category::utility;
  Toggle enabled{"Block on"};
  PushButton assign{"Assign footswitch"};
  PushButton replace{"Replace"};
  SegmentedControl compare{{"A", "B"}};
  juce::Rectangle<int> compareLabel;
};

// ####################
// PAGE
// ####################
BlockEditorPage::BlockEditorPage(EngineClient& client)
    : engine(client), scenes(std::make_unique<ScenesStrip>()), header(std::make_unique<Header>(client)) {
  setTitle("Block editor");
  setWantsKeyboardFocus(true);
  back.setIcon(&icons::path(icons::Icon::arrowLeft));
  back.onClick = [this] {
    if (onBack)
      onBack();
  };
  scenes->onSelect = [this](const juce::String& scene) {
    engine.run("scene.select", object({{"scene", scene}}));
    if (onChanged)
      onChanged();
  };
  signalRow.onSelect = [this](const juce::String& id) {
    if (id != block.blockId && onOpenBlock)
      onOpenBlock(id);
  };
  header->onChanged = [this] {
    if (onChanged)
      onChanged();
  };
  header->onReplace = [this] {
    if (onReplace)
      onReplace(block.row, block.slot, 0);
  };
  addAndMakeVisible(*scenes);
  addAndMakeVisible(signalRow);
  addAndMakeVisible(back);
  addAndMakeVisible(*header);
}

BlockEditorPage::~BlockEditorPage() { stopTimer(); }

bool BlockEditorPage::read(const GridModel& model, juce::String id) {
  int row = 0, slot = 0;
  const GridSlot* found = model.find(id, &row, &slot);
  if (found == nullptr || found->empty)
    return false;
  const juce::var state = engine.read("state.get");
  juce::var entry;
  if (const auto* blocks = state["chain"]["grid"]["rows"][row]["blocks"].getArray())
    for (const auto& candidate : *blocks)
      if (candidate["blockId"].toString() == id)
        entry = candidate;
  if (entry.isVoid())
    return false;
  block = {};
  block.blockId = id;
  block.name = found->name;
  block.kindId = found->kindId;
  block.category = found->category;
  block.row = row;
  block.slot = slot;
  block.enabled = found->enabled;
  block.activeScene = model.activeScene;
  block.block = entry;
  if (const auto* params = entry["sceneParams"].getArray())
    for (const auto& param : *params)
      block.sceneParams.add(param.toString());
  if (const auto* params = entry["expressionParams"].getArray())
    for (const auto& param : *params)
      block.expressionParams.add(param.toString());
  return true;
}

void BlockEditorPage::makeBody() {
  const juce::String kind = block.kindId == "nam" ? "nam" : block.kindId == "ir" ? "ir" : "effect";
  if (body != nullptr && kind == bodyKind)
    return;
  bodyKind = kind;
  body.reset();
  if (kind == "nam")
    body = std::make_unique<NamEditor>(engine);
  else if (kind == "ir")
    body = std::make_unique<IrEditor>(engine);
  else
    body = std::make_unique<EffectEditor>(engine);
  body->onChanged = [this] {
    if (onChanged)
      onChanged();
  };
  body->onReplace = [this](int source) {
    if (onReplace)
      onReplace(block.row, block.slot, source);
  };
  addAndMakeVisible(*body);
  resized();
}

void BlockEditorPage::open(const GridModel& model, const juce::String& id) {
  // Another block starts from a clean body, so no control carries over.
  if (id != block.blockId) {
    body.reset();
    bodyKind.clear();
  }
  refresh(model, id);
}

void BlockEditorPage::refresh(const GridModel& model) { refresh(model, block.blockId); }

void BlockEditorPage::refresh(const GridModel& model, juce::String id) {
  if (!read(model, id))
    return;
  scenes->setScenes(model.scenes);
  signalRow.setRow(model.rows[static_cast<size_t>(block.row)], block.blockId);
  header->show(block);
  makeBody();
  body->showBlock(block);
  repaint();
}

void BlockEditorPage::visibilityChanged() {
  if (isVisible())
    startTimerHz(30);
  else
    stopTimer();
}

void BlockEditorPage::timerCallback() {
  if (body != nullptr)
    body->tick();
}

bool BlockEditorPage::keyPressed(const juce::KeyPress& key) {
  if (key == juce::KeyPress::escapeKey && onBack) {
    onBack();
    return true;
  }
  return false;
}

void BlockEditorPage::paint(juce::Graphics& g) {
  g.fillAll(colour::ground);
  g.setColour(colour::line);
  g.fillRect(0, kScenesHeight + kSignalHeight - 1, getWidth(), 1);
  g.setColour(colour::surface);
  g.fillRect(0, kScenesHeight + kSignalHeight, getWidth(), getHeight() - kScenesHeight - kSignalHeight);
}

void BlockEditorPage::resized() {
  auto area = getLocalBounds();
  scenes->setBounds(area.removeFromTop(kScenesHeight));
  auto signalArea = area.removeFromTop(kSignalHeight);
  const int backWidth = back.idealWidth();
  back.setBounds(signalArea.getRight() - kPadX - backWidth, signalArea.getCentreY() - 18, backWidth, 36);
  signalRow.setBounds(signalArea.withTrimmedRight(backWidth + kPadX + 16));
  area.reduce(kPadX, kPadY);
  header->setBounds(area.removeFromTop(kHeaderHeight));
  area.removeFromTop(kHeaderGap);
  if (body != nullptr)
    body->setBounds(area);
}

}  // namespace pedaleira::ui

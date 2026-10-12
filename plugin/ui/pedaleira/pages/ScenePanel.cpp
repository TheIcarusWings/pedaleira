#include "ScenePanel.h"

#include "../Paint.h"
#include "../Type.h"

namespace pedaleira::ui {

namespace {

constexpr int kLeftWidth = 268;
constexpr int kRightWidth = 340;
constexpr int kParameterColumn = 190;
constexpr int kHeaderHeight = 30;
constexpr int kRowHeight = 33;
constexpr int kMaxRows = 6;
constexpr int kBlockRowHeight = 32;
constexpr int kBlockGap = 4;
constexpr int kMaxBlockRows = 6;
const char* const kHeads[5] = {"A", "B", "C", "D", "E-H"};

}  // namespace

// The small on/off switch of the bypass list (32 x 18, smaller than the
// page's Toggle).
class ScenePanel::MiniSwitch final : public juce::Button {
public:
  explicit MiniSwitch(const juce::String& accessibleName) : juce::Button(accessibleName) {
    setTitle(accessibleName);
    setClickingTogglesState(true);
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
  }

  void paintButton(juce::Graphics& g, bool, bool) override {
    const bool on = getToggleState();
    const auto track = getLocalBounds().toFloat().withSizeKeepingCentre(32.0f, 18.0f);
    g.setColour(on ? colour::iris : colour::line);
    g.fillRoundedRectangle(track, 9.0f);
    const float knob = 14.0f;
    g.setColour(on ? colour::ground : colour::muted);
    g.fillEllipse(on ? track.getRight() - 2.0f - knob : track.getX() + 2.0f, track.getCentreY() - knob * 0.5f, knob, knob);
    if (hasKeyboardFocus(false)) {
      g.setColour(colour::irisHover);
      g.drawRoundedRectangle(track.expanded(2.0f), 11.0f, 1.5f);
    }
  }
};

ScenePanel::ScenePanel(EngineClient& client) : engine(client) {
  setTitle("Scene");
  name.setTitle("Scene name");
  name.setFont(type::sans(14.0f, type::Weight::medium));
  name.setColour(juce::TextEditor::backgroundColourId, colour::ground);
  name.setColour(juce::TextEditor::outlineColourId, colour::line);
  name.setColour(juce::TextEditor::focusedOutlineColourId, colour::iris);
  name.setColour(juce::TextEditor::textColourId, colour::text);
  name.setIndents(12, 0);
  name.setJustification(juce::Justification::centredLeft);
  name.setMultiLine(false);
  name.onReturnKey = [this] { commitName(); };
  name.onFocusLost = [this] { commitName(); };
  copy.setTitle("Copy scene");
  copy.onClick = [this] {
    copiedScene = activeId;
    resized();
    repaint();
  };
  paste.onClick = [this] {
    if (copiedScene.isEmpty() || copiedScene == activeId)
      return;
    engine.run("scene.copy", object({{"from", copiedScene}, {"to", activeId}}));
    if (onChanged)
      onChanged();
  };
  reset.onClick = [this] {
    engine.run("scene.reset", object({{"scene", activeId}}));
    if (onChanged)
      onChanged();
  };
  modes.onClick = [this] {
    if (onOpenModes)
      onOpenModes();
  };
  for (juce::Component* c : std::initializer_list<juce::Component*>{&name, &copy, &paste, &reset, &modes})
    addAndMakeVisible(c);
}

ScenePanel::~ScenePanel() = default;

void ScenePanel::commitName() {
  if (activeId.isEmpty() || name.getText() == activeName)
    return;
  engine.run("scene.rename", object({{"scene", activeId}, {"name", name.getText()}}));
  if (onChanged)
    onChanged();
}

void ScenePanel::refresh(const GridModel& model) {
  activeId = model.activeScene;
  const juce::var scenes = engine.read("scene.get");
  const int active = scenes["active"];
  const auto* list = scenes["scenes"].getArray();
  activeName.clear();
  if (list != nullptr && active >= 0 && active < list->size())
    activeName = list->getReference(active)["name"].toString();
  if (!name.hasKeyboardFocus(true))
    name.setText(activeName, false);

  // The blocks of the rig, in row order.
  std::map<juce::String, std::pair<juce::String, Category>> known;
  blocks.clear();
  for (const int r : model.visibleRows())
    for (const auto& slot : model.rows[static_cast<size_t>(r)].slots) {
      if (slot.empty)
        continue;
      known[slot.blockId] = {slot.name, slot.category};
      blocks.push_back({slot.blockId, slot.name, slot.category, slot.enabled});
    }

  // The parameters with a value per scene.
  rows.clear();
  std::map<juce::String, std::vector<KnobParam>> specs;
  if (const auto* assigned = scenes["assigned"].getArray())
    for (const auto& entry : *assigned) {
      const juce::String blockId = entry["blockId"].toString();
      const juce::String paramId = entry["param"].toString();
      const auto found = known.find(blockId);
      if (found == known.end())
        continue;
      if (specs.count(blockId) == 0) {
        std::vector<KnobParam>& out = specs[blockId];
        const juce::var listed = engine.read("block.listParams", object({{"blockId", blockId}}));
        if (const auto* params = listed["params"].getArray())
          for (const auto& item : *params)
            out.push_back(KnobParam::fromVar(item));
      }
      ParamRow row;
      row.blockName = found->second.first;
      row.category = found->second.second;
      for (const auto& spec : specs[blockId])
        if (spec.id == paramId)
          row.spec = spec;
      if (row.spec.id.isEmpty())
        continue;
      row.paramName = row.spec.name;
      for (int i = 0; list != nullptr && i < list->size() && i < 8; ++i)
        if (const auto* values = list->getReference(i)["values"].getArray())
          for (const auto& value : *values)
            if (value["blockId"].toString() == blockId && value["param"].toString() == paramId)
              row.values[static_cast<size_t>(i)] = value["value"];
      rows.push_back(row);
    }

  paste.setEnabled(copiedScene.isNotEmpty() && copiedScene != activeId);
  layoutBlocks();
  resized();
  repaint();
}

void ScenePanel::layoutBlocks() {
  const int shown = juce::jmin(static_cast<int>(blocks.size()), kMaxBlockRows * 2);
  hiddenBlocks = static_cast<int>(blocks.size()) - shown;
  // The same blocks again update their switches in place; a switch is never
  // deleted while its own click is running.
  if (static_cast<int>(switches.size()) != shown) {
    switches.clear();
    for (int i = 0; i < shown; ++i) {
      auto toggle = std::make_unique<MiniSwitch>(blocks[static_cast<size_t>(i)].name);
      MiniSwitch* raw = toggle.get();
      juce::Component::SafePointer<ScenePanel> self(this);
      const int index = i;
      toggle->onClick = [self, raw, index] {
        if (self == nullptr || index >= static_cast<int>(self->blocks.size()))
          return;
        // The switch has flipped itself; the engine takes the new state.
        self->engine.run("block.setEnabled", object({{"blockId", self->blocks[static_cast<size_t>(index)].blockId},
                                                     {"enabled", raw->getToggleState()}}));
        juce::MessageManager::callAsync([self] {
          if (self != nullptr && self->onChanged)
            self->onChanged();
        });
      };
      addAndMakeVisible(*toggle);
      switches.push_back(std::move(toggle));
    }
  }
  for (int i = 0; i < shown; ++i) {
    const BlockRow& block = blocks[static_cast<size_t>(i)];
    MiniSwitch& toggle = *switches[static_cast<size_t>(i)];
    toggle.setTitle(block.name + (block.enabled ? " on" : " off") + " in scene " + activeId);
    toggle.setToggleState(block.enabled, juce::dontSendNotification);
  }
}

juce::String ScenePanel::cellText(const ParamRow& row, int column) const {
  if (column < 4)
    return row.spec.text(row.values[static_cast<size_t>(column)]);
  // E to H share a value until edited: shown as one, or "as A" when it is A's.
  const juce::String first = row.spec.text(row.values[4]);
  for (int i = 5; i < 8; ++i)
    if (row.spec.text(row.values[static_cast<size_t>(i)]) != first)
      return "varies";
  return first == row.spec.text(row.values[0]) ? juce::String("as A") : first;
}

juce::Rectangle<int> ScenePanel::headerCell(int column) const {
  const int cell = (tableBox.getWidth() - kParameterColumn) / 5;
  return {tableBox.getX() + kParameterColumn + column * cell, tableBox.getY(), cell, kHeaderHeight};
}

void ScenePanel::mouseUp(const juce::MouseEvent& event) {
  if (!event.mouseWasClicked() || !onSelectScene)
    return;
  for (int column = 0; column < 5; ++column)
    if (headerCell(column).contains(event.getPosition())) {
      onSelectScene(column < 4 ? juce::String(kHeads[column]) : juce::String("E"));
      return;
    }
}

void ScenePanel::resized() {
  const int width = getWidth();
  modes.setBounds(width - 24 - modes.idealWidth(), 16, modes.idealWidth(), 28);

  // Left: the name, then the three buttons.
  name.setBounds(24, 54 + 14 + 6, kLeftWidth, 36);
  int x = 24;
  int y = 54 + 14 + 6 + 36 + 12;
  for (PushButton* button : {&copy, &paste, &reset}) {
    const int w = button->idealWidth();
    if (x + w > 24 + kLeftWidth) {
      x = 24;
      y += 40;
    }
    button->setBounds(x, y, w, 32);
    x += w + 8;
  }
  noteArea = {24, y + 40, kLeftWidth, 34};

  // Middle: the table; right: the bypass list.
  const int middleLeft = 24 + kLeftWidth + 24;
  const int rightLeft = width - 24 - kRightWidth;
  const int shownRows = juce::jmin(static_cast<int>(rows.size()), kMaxRows - (rows.size() > kMaxRows ? 1 : 0));
  hiddenRows = static_cast<int>(rows.size()) - shownRows;
  const int rowCount = juce::jmax(1, shownRows + (hiddenRows > 0 ? 1 : 0));
  tableBox = {middleLeft, 54 + 14 + 8, rightLeft - 24 - middleLeft, kHeaderHeight + rowCount * kRowHeight};
  blocksBox = {rightLeft, 54 + 14 + 8, kRightWidth, getHeight() - (54 + 14 + 8) - 16};

  const int perColumn = juce::jmax(1, juce::jmin(kMaxBlockRows, (static_cast<int>(switches.size()) + 1) / 2));
  const int columnWidth = (kRightWidth - 12) / 2;
  for (size_t i = 0; i < switches.size(); ++i) {
    const int column = static_cast<int>(i) / perColumn;
    const int row = static_cast<int>(i) % perColumn;
    const int left = blocksBox.getX() + column * (columnWidth + 12);
    const int top = blocksBox.getY() + row * (kBlockRowHeight + kBlockGap);
    switches[i]->setBounds(left + columnWidth - 8 - 32 - 2, top + (kBlockRowHeight - 22) / 2, 36, 22);
  }
}

void ScenePanel::paint(juce::Graphics& g) {
  g.fillAll(colour::surface);

  // Title row: "Scene C, middle dot, Lead", the hint under the buttons' reach.
  g.setFont(type::heading());
  g.setColour(colour::text);
  const juce::String title = "Scene " + activeId + (activeName.isNotEmpty() && activeName != "Scene " + activeId
                                                      ? " " + type::dot() + " " + activeName
                                                      : juce::String());
  g.drawText(title, 24, 16, 420, 26, juce::Justification::centredLeft, true);
  g.setFont(type::sans(12.0f));
  g.setColour(colour::muted);
  const int noteLeft = 24 + 420 + 12;
  g.drawText("Right-click or long-press any knob to assign it to scenes. Expression-assigned parameters are not stored in scenes.",
             noteLeft, 16, modes.getX() - 12 - noteLeft, 26, juce::Justification::centredRight, true);

  paint::label(g, "Scene name", {24, 54, kLeftWidth, 14});
  if (copiedScene.isNotEmpty()) {
    g.setFont(type::sans(12.0f));
    g.setColour(colour::muted);
    g.drawFittedText(copiedScene == activeId ? "Scene " + copiedScene + " is copied. Select another scene to paste it."
                                             : "Scene " + copiedScene + " is copied. Paste puts it on scene " + activeId + ".",
                     noteArea, juce::Justification::topLeft, 2);
  }

  // The table of scene-assigned parameters.
  paint::label(g, "Scene-assigned parameters", {tableBox.getX(), 54, tableBox.getWidth(), 14});
  const auto box = tableBox.toFloat();
  g.setColour(colour::ground);
  g.fillRoundedRectangle(box, size::panelRadius);
  const int activeColumn = activeId.isEmpty() ? -1 : juce::jmin(4, juce::String("ABCDEFGH").indexOf(activeId));
  {
    juce::Graphics::ScopedSaveState state(g);
    juce::Path clip;
    clip.addRoundedRectangle(box, size::panelRadius);
    g.reduceClipRegion(clip);
    paint::label(g, "Parameter", {tableBox.getX() + 12, tableBox.getY(), kParameterColumn - 12, kHeaderHeight});
    for (int column = 0; column < 5; ++column) {
      const auto head = headerCell(column);
      if (column == activeColumn) {
        g.setColour(colour::iris);
        g.fillRect(head);
      }
      paint::label(g, kHeads[column], head.reduced(10, 0), juce::Justification::centredLeft,
                   column == activeColumn ? colour::ground : colour::muted);
    }
    g.setColour(colour::line);
    g.fillRect(tableBox.getX(), tableBox.getY() + kHeaderHeight - 1, tableBox.getWidth(), 1);

    int y = tableBox.getY() + kHeaderHeight;
    if (rows.empty()) {
      g.setFont(type::sans(12.0f));
      g.setColour(colour::muted);
      g.drawText("No parameter has a value per scene yet.", tableBox.getX() + 12, y, tableBox.getWidth() - 24,
                 kRowHeight, juce::Justification::centredLeft, true);
    }
    const int shown = static_cast<int>(rows.size()) - hiddenRows;
    for (int i = 0; i < shown; ++i) {
      const ParamRow& row = rows[static_cast<size_t>(i)];
      if (activeColumn >= 0) {
        g.setColour(colour::iris.withAlpha(0.16f));
        g.fillRect(headerCell(activeColumn).withY(y).withHeight(kRowHeight));
      }
      g.setColour(categoryColour(row.category));
      g.fillRoundedRectangle(static_cast<float>(tableBox.getX() + 12), y + (kRowHeight - 8) * 0.5f, 8.0f, 8.0f, 2.0f);
      g.setFont(type::sans(13.0f, type::Weight::semibold));
      g.setColour(colour::text);
      g.drawText(row.blockName + " " + type::dot() + " " + row.paramName, tableBox.getX() + 28, y,
                 kParameterColumn - 36, kRowHeight, juce::Justification::centredLeft, true);
      for (int column = 0; column < 5; ++column) {
        const juce::String text = cellText(row, column);
        g.setFont(type::mono(12.0f));
        g.setColour(text == "as A" ? colour::muted : colour::text);
        g.drawText(text, headerCell(column).withY(y).withHeight(kRowHeight).reduced(10, 0),
                   juce::Justification::centredLeft, true);
      }
      y += kRowHeight;
      g.setColour(colour::line);
      g.fillRect(tableBox.getX(), y - 1, tableBox.getWidth(), 1);
    }
    if (hiddenRows > 0) {
      g.setFont(type::sans(12.0f));
      g.setColour(colour::muted);
      g.drawText("and " + juce::String(hiddenRows) + " more " + (hiddenRows == 1 ? "parameter" : "parameters") +
                     ", on their blocks",
                 tableBox.getX() + 12, y, tableBox.getWidth() - 24, kRowHeight, juce::Justification::centredLeft, true);
    }
  }
  g.setColour(colour::line);
  g.drawRoundedRectangle(box.reduced(0.5f), size::panelRadius, 1.0f);

  // The bypass states.
  paint::label(g, "Bypass states in scene " + activeId, {blocksBox.getX(), 54, kRightWidth, 14});
  const int perColumn = juce::jmax(1, juce::jmin(kMaxBlockRows, (static_cast<int>(switches.size()) + 1) / 2));
  const int columnWidth = (kRightWidth - 12) / 2;
  for (size_t i = 0; i < switches.size(); ++i) {
    const BlockRow& block = blocks[i];
    const int column = static_cast<int>(i) / perColumn;
    const int row = static_cast<int>(i) % perColumn;
    const juce::Rectangle<int> item(blocksBox.getX() + column * (columnWidth + 12),
                                    blocksBox.getY() + row * (kBlockRowHeight + kBlockGap), columnWidth, kBlockRowHeight);
    g.setColour(colour::raised);
    g.fillRoundedRectangle(item.toFloat(), size::buttonRadius);
    g.setColour(categoryColour(block.category));
    g.fillRoundedRectangle(static_cast<float>(item.getX() + 10), item.getCentreY() - 4.0f, 8.0f, 8.0f, 2.0f);
    g.setFont(type::sans(12.0f, type::Weight::semibold));
    g.setColour(block.enabled ? colour::text : colour::muted);
    g.drawText(block.name, item.getX() + 26, item.getY(), columnWidth - 26 - 44, kBlockRowHeight,
               juce::Justification::centredLeft, true);
  }
  if (blocks.empty()) {
    g.setFont(type::sans(12.0f));
    g.setColour(colour::muted);
    g.drawText("No blocks yet.", blocksBox.withHeight(kBlockRowHeight), juce::Justification::centredLeft, true);
  } else if (hiddenBlocks > 0) {
    g.setFont(type::sans(12.0f));
    g.setColour(colour::muted);
    g.drawText("and " + juce::String(hiddenBlocks) + " more, on the grid",
               blocksBox.getX(), blocksBox.getY() + perColumn * (kBlockRowHeight + kBlockGap), kRightWidth, 18,
               juce::Justification::centredLeft, true);
  }
}

}  // namespace pedaleira::ui

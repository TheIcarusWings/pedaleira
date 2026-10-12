#include "SettingsExpression.h"

#include "../Icons.h"
#include "../Paint.h"
#include "../Type.h"
#include "EditorParts.h"
#include "GridModel.h"
#include "ParamKnobs.h"

#include <algorithm>

namespace pedaleira::ui {

namespace {

constexpr int kRowHeight = 48;
constexpr int kFooterHeight = 52;

const char* const kCalibrationLater =
    "Rocking the pedal to heel and to toe, the live position and the MIDI source come in a later build. The engine "
    "does not read a pedal yet.";
const char* const kResponseLater =
    "Min, max, reverse and the curve (linear, log, exp, S-curve) come in a later build.";

}  // namespace

SettingsExpression::SettingsExpression(EngineClient& client)
    : SettingsSection(client, "Expression pedals",
                      "Set parameters aside for an expression pedal. The pedal's input and its calibration come in a "
                      "later build.") {
  assignButton.setIcon(&icons::path(icons::Icon::plus));
  assignButton.onClick = [this] { openAssignMenu(); };
  addAndMakeVisible(assignButton);
}

// ####################
// DATA
// ####################
void SettingsExpression::refresh() {
  assignments.clear();
  // Read in two steps: the blocks that carry a flagged parameter, then the
  // names and values of those parameters.
  const juce::var state = engine.read("state.get");
  if (const auto* rows = state["chain"]["grid"]["rows"].getArray())
    for (const auto& row : *rows) {
      const auto* blocks = row["blocks"].getArray();
      if (blocks == nullptr)
        continue;
      for (const auto& block : *blocks) {
        if (block["kind"].toString() == "insert")
          continue;
        const auto* flagged = block["expressionParams"].getArray();
        if (flagged == nullptr || flagged->isEmpty())
          continue;
        const juce::String blockId = block["blockId"].toString();
        const juce::var listed = engine.read("block.listParams", object({{"blockId", blockId}}));
        const auto* specs = listed["params"].getArray();
        for (const auto& id : *flagged) {
          Assignment a;
          a.blockId = blockId;
          a.blockName = block["tone"]["title"].toString();
          a.category = categoryFromId(block["category"].toString());
          a.paramId = id.toString();
          a.paramName = a.paramId;
          if (specs != nullptr)
            for (const auto& spec : *specs)
              if (spec["id"].toString() == a.paramId) {
                const KnobParam knob = KnobParam::fromVar(spec);
                a.paramName = knob.name;
                a.value = knob.text(knob.value);
              }
          assignments.push_back(a);
        }
      }
    }
  rebuildButtons();
  resized();
  heightChanged();
  repaint();
}

void SettingsExpression::rebuildButtons() {
  removeButtons.clear();
  for (size_t i = 0; i < assignments.size(); ++i) {
    auto button = std::make_unique<IconButton>(
        "Remove assignment " + assignments[i].blockName + " " + assignments[i].paramName, &icons::path(icons::Icon::close));
    const int index = static_cast<int>(i);
    // The list is rebuilt, button included: after the click returns.
    button->onClick = [this, index] { settings::afterClick(*this, [this, index] { unassign(index); }); };
    addAndMakeVisible(*button);
    removeButtons.push_back(std::move(button));
  }
}

void SettingsExpression::assign(const juce::String& blockId, const juce::String& paramId) {
  if (run("Could not assign the parameter", "expression.assignParam", object({{"blockId", blockId}, {"param", paramId}})))
    refresh();
}

void SettingsExpression::unassign(int index) {
  if (index < 0 || index >= assignmentCount())
    return;
  const Assignment a = assignments[static_cast<size_t>(index)];
  if (run("Could not remove the assignment", "expression.unassignParam",
          object({{"blockId", a.blockId}, {"param", a.paramId}})))
    refresh();
}

void SettingsExpression::openAssignMenu() {
  // A submenu of parameters per block of the preset.
  const GridModel grid = GridModel::read(engine);
  juce::PopupMenu menu;
  juce::Component::SafePointer<SettingsExpression> self(this);
  bool any = false;
  for (const auto& row : grid.rows)
    for (const auto& slot : row.slots) {
      if (slot.empty)
        continue;
      const juce::var listed = engine.read("block.listParams", object({{"blockId", slot.blockId}}));
      const auto* specs = listed["params"].getArray();
      if (specs == nullptr)
        continue;
      juce::PopupMenu sub;
      for (const auto& spec : *specs) {
        const KnobParam knob = KnobParam::fromVar(spec);
        // Switches and choices have no travel for a pedal to follow.
        if (knob.type != "float")
          continue;
        const bool already = std::any_of(assignments.begin(), assignments.end(), [&](const Assignment& a) {
          return a.blockId == slot.blockId && a.paramId == knob.id;
        });
        const juce::String blockId = slot.blockId, paramId = knob.id;
        sub.addItem(knob.name, !already, already, [self, blockId, paramId] {
          if (self != nullptr)
            self->assign(blockId, paramId);
        });
      }
      menu.addSubMenu(slot.name + "  (row " + juce::String(row.index + 1) + ")", sub);
      any = true;
    }
  if (!any)
    menu.addItem("No blocks in this preset", false, false, [] {});
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&assignButton));
}

// ####################
// LAYOUT
// ####################
int SettingsExpression::heightFor(int width) const { return layoutFor(width).height; }

SettingsExpression::Layout SettingsExpression::layoutFor(int width) const {
  Layout l;
  using namespace settings;
  // The board's columns: 620 and the rest.
  const int leftWidth = juce::jmin(620, (width - kCardGap) * 11 / 21);
  const int rightWidth = width - kCardGap - leftWidth;
  const int rightX = leftWidth + kCardGap;

  // Right: response, then the assignments.
  int y = 0;
  {
    const int box = 96;
    l.response = {rightX, y, rightWidth, kCardTop + box + kCardBottom};
    l.responseBox = cardContent(l.response).withHeight(box);
    y = l.response.getBottom() + kCardGap;
  }
  {
    const int rows = juce::jmax(1, static_cast<int>(assignments.size()));
    const int content = rows * kRowHeight + kFooterHeight;
    l.assignments = {rightX, y, rightWidth, kCardTop + content + kCardBottom};
    auto area = cardContent(l.assignments);
    if (assignments.empty())
      l.empty = area.removeFromTop(kRowHeight);
    else
      for (size_t i = 0; i < assignments.size(); ++i)
        l.rows.push_back(area.removeFromTop(kRowHeight));
    l.footer = area.withHeight(kFooterHeight);
    y = l.assignments.getBottom();
  }
  l.height = y;
  // Left: calibration, with room for what will be there.
  l.calibration = {0, 0, leftWidth, kCardTop + 180 + kCardBottom};
  l.calibrationBox = cardContent(l.calibration);
  l.height = juce::jmax(l.height, l.calibration.getBottom());
  return l;
}

void SettingsExpression::resized() {
  lay = layoutFor(getWidth());
  for (size_t i = 0; i < removeButtons.size() && i < lay.rows.size(); ++i)
    removeButtons[i]->setBounds(lay.rows[i].withLeft(lay.rows[i].getRight() - 28).withSizeKeepingCentre(28, 28));
  assignButton.setBounds(lay.footer.withTrimmedTop(14).withHeight(34).withWidth(assignButton.idealWidth()));
}

// ####################
// PAINT
// ####################
void SettingsExpression::paint(juce::Graphics& g) {
  using namespace settings;
  paintCard(g, lay.calibration, "Calibration");
  paintLater(g, lay.calibrationBox, kCalibrationLater);
  paintCard(g, lay.response, "Response");
  paintLater(g, lay.responseBox, kResponseLater);
  paintCard(g, lay.assignments, "Assignments in this preset");

  if (assignments.empty()) {
    g.setFont(type::body());
    g.setColour(colour::muted);
    g.drawText("Nothing is assigned to the pedal.", lay.empty, juce::Justification::centredLeft, true);
  }
  for (size_t i = 0; i < assignments.size() && i < lay.rows.size(); ++i) {
    auto row = lay.rows[i];
    paintRowLine(g, row);
    // The block's colour in a dot, as on the grid.
    g.setColour(categoryColour(assignments[i].category));
    g.fillEllipse(juce::Rectangle<float>(8.0f, 8.0f).withCentre({static_cast<float>(row.getX()) + 4.0f, row.getCentreY() * 1.0f}));
    row.removeFromLeft(20);
    row.removeFromRight(36);
    g.setFont(type::mono(13.0f));
    g.setColour(colour::ok);
    g.drawText(assignments[i].value, row.removeFromRight(110), juce::Justification::centredRight, false);
    g.setFont(type::body());
    g.setColour(colour::text);
    g.drawText(assignments[i].blockName + " " + type::dot() + " " + assignments[i].paramName, row,
               juce::Justification::centredLeft, true);
  }
  paintNote(g, lay.footer.withLeft(lay.footer.getX() + assignButton.getWidth() + 16).withTrimmedTop(14),
            "Expression-assigned parameters are not stored in scenes.", 12.0f);
}

}  // namespace pedaleira::ui

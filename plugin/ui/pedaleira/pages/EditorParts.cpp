#include "EditorParts.h"

#include "../Paint.h"
#include "../Type.h"

namespace pedaleira::ui {

const char* const kLaterBuild = "Comes in a later build";

juce::Rectangle<int> paintSection(juce::Graphics& g, juce::Rectangle<int> box, const juce::String& title,
                                  const juce::String& note) {
  g.setColour(colour::ground);
  g.fillRoundedRectangle(box.toFloat(), size::panelRadius);
  g.setColour(colour::line);
  g.drawRoundedRectangle(box.toFloat().reduced(0.5f), size::panelRadius, 1.0f);
  auto inner = box.reduced(16);
  auto heading = inner.removeFromTop(16);
  paint::label(g, title, heading);
  if (note.isNotEmpty()) {
    const int labelWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(type::label(), title.toUpperCase()));
    g.setFont(type::sans(12.0f));
    g.setColour(colour::muted);
    g.drawText(note, heading.withTrimmedLeft(labelWidth + 12), juce::Justification::centredLeft, true);
  }
  inner.removeFromTop(12);
  return inner;
}

void showFootswitchMenu(EngineClient& engine, const juce::String& blockId, juce::Component& target,
                        std::function<void()> done) {
  juce::PopupMenu menu;
  for (const char* letter : {"A", "B", "C", "D", "E", "F", "G", "H"})
    menu.addItem(juce::String("Footswitch ") + letter, [&engine, blockId, letter, done] {
      engine.run("stomp.assign", object({{"switch", letter}, {"blockIds", juce::Array<juce::var>{blockId}}}));
      if (done)
        done();
    });
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&target));
}

// ####################
// PARAM KNOB
// ####################
ParamKnob::ParamKnob(EngineClient& client, const juce::String& block, const KnobParam& spec)
    : Knob(spec.name), engine(client), blockId(block), param(spec) {
  setDiameter(72);
  setDefaultPosition(param.toPosition(param.defaultValue));
  onChange = [this](double position) {
    param.value = param.fromPosition(position);
    setValueText(textOf());
    engine.run("block.setParam", object({{"blockId", blockId}, {"param", param.id}, {"value", param.value}}));
    if (onValueChanged)
      onValueChanged(param);
  };
  onContextMenu = [this] { openMenu(); };
  update(spec);
}

juce::Point<int> ParamKnob::cellSize(int diameter) { return {diameter + 16, diameter + 4 + 18 + 18 + 14}; }

juce::String ParamKnob::textOf() const { return valueText ? valueText(param) : param.text(param.value); }

void ParamKnob::update(const KnobParam& spec) {
  param = spec;
  setDefaultPosition(param.toPosition(param.defaultValue));
  // A knob under the mouse keeps the position the player is giving it.
  if (!isMouseButtonDown(true))
    setPosition(param.toPosition(param.value));
  setValueText(textOf());
  if (param.expressionAssigned) {
    setAssignment(Assignment::expression);
    setTag("Expression pedal", colour::ok);
  } else if (param.sceneAssigned) {
    setAssignment(Assignment::scene);
    setTag("Scene-assigned", colour::iris);
  } else {
    setAssignment(Assignment::none);
    setTag({});
  }
}

void ParamKnob::openMenu() {
  juce::Component::SafePointer<ParamKnob> self(this);
  const auto assignmentChanged = [self] {
    if (self != nullptr && self->onAssignmentChanged)
      self->onAssignmentChanged();
  };
  juce::PopupMenu menu;
  // A pedal-driven value stays out of the scenes, and so does a parameter
  // that changes the block's latency (the engine refuses both).
  const bool canScene = param.sceneAssigned || (!param.expressionAssigned && !param.affectsLatency);
  menu.addItem("Assign to scenes", canScene, param.sceneAssigned, [self, assignmentChanged] {
    // The menu is open after this call returns; the knob may be gone.
    if (self == nullptr)
      return;
    self->engine.run(self->param.sceneAssigned ? "scene.unassignParam" : "scene.assignParam",
                     object({{"blockId", self->blockId}, {"param", self->param.id}}));
    assignmentChanged();
  });
  menu.addItem("Assign to expression pedal", true, param.expressionAssigned, [self, assignmentChanged] {
    if (self == nullptr)
      return;
    self->engine.run(self->param.expressionAssigned ? "expression.unassignParam" : "expression.assignParam",
                     object({{"blockId", self->blockId}, {"param", self->param.id}}));
    assignmentChanged();
  });
  // The MIDI map reaches the global controls and block power only; a block
  // parameter has no CC target yet.
  menu.addItem("Assign to MIDI CC", false, false, [] {});
  menu.addSeparator();
  menu.addItem("Reset to default", [self] {
    if (self != nullptr)
      self->setPosition(self->param.toPosition(self->param.defaultValue), juce::sendNotification);
  });
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this));
}

// ####################
// SWITCH ROW
// ####################
SwitchRow::SwitchRow(const juce::String& text) : toggle(text), label(text) {
  toggle.onClick = [this] {
    if (onToggle)
      onToggle(toggle.getToggleState());
  };
  addAndMakeVisible(toggle);
}

void SwitchRow::setState(bool on) { toggle.setToggleState(on, juce::dontSendNotification); }

void SwitchRow::setDetail(const juce::String& text) {
  detail = text;
  repaint();
}

void SwitchRow::setSwitchEnabled(bool enabled) {
  toggle.setEnabled(enabled);
  setAlpha(enabled ? 1.0f : 0.5f);
}

int SwitchRow::idealWidth() const {
  int width = 48 + juce::roundToInt(juce::GlyphArrangement::getStringWidth(type::sans(13.0f), label));
  if (detail.isNotEmpty())
    width += 12 + juce::roundToInt(juce::GlyphArrangement::getStringWidth(type::mono(12.0f, type::Weight::regular), detail));
  return width + 4;
}

void SwitchRow::paint(juce::Graphics& g) {
  auto area = getLocalBounds().withTrimmedLeft(48);
  g.setFont(type::sans(13.0f));
  g.setColour(colour::text);
  const int labelWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(type::sans(13.0f), label)) + 1;
  g.drawText(label, area.removeFromLeft(labelWidth), juce::Justification::centredLeft, false);
  if (detail.isNotEmpty()) {
    area.removeFromLeft(12);
    g.setFont(type::mono(12.0f, type::Weight::regular));
    g.setColour(colour::muted);
    g.drawText(detail, area, juce::Justification::centredLeft, false);
  }
}

void SwitchRow::resized() { toggle.setBounds(0, (getHeight() - 28) / 2, 40, 28); }

}  // namespace pedaleira::ui

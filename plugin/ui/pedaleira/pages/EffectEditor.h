// The editor of a built-in effect (the board "Block editor, effect"),
// generated from the effect's parameter description (block.listParams): a
// Time section when the effect has a tempo-synced time or rate, its other
// parameters, then the output knobs and the switches such as spillover.
// Right-click on a knob opens its assignments; the effect presets column is
// still to come.
#pragma once

#include "EditorParts.h"
#include "GraphicEqView.h"

#include <map>
#include <memory>
#include <vector>

namespace pedaleira::ui {

class EffectEditor : public BlockEditorBody {
public:
  explicit EffectEditor(EngineClient& engine);
  ~EffectEditor() override;

  void showBlock(const BlockContext& block) override;

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  struct ChoiceCell;
  // A time or rate with its tempo sync switch and note value.
  struct TimeGroup {
    KnobParam time, sync, division;
    std::unique_ptr<ParamKnob> knob;
    std::unique_ptr<SwitchRow> syncRow;
    std::unique_ptr<SegmentedControl> note, variant;
    std::unique_ptr<PushButton> tap;
  };
  struct Section {
    juce::String title;
    std::vector<juce::Component*> items;
    // The items that start a new line of the section.
    std::vector<bool> breaks;
    TimeGroup* time = nullptr;
    juce::Rectangle<int> bounds;
  };

  void rebuild(const std::vector<KnobParam>& specs);
  void updateValues(const std::vector<KnobParam>& specs);
  void updateTime(TimeGroup& group);
  juce::String timeText(const TimeGroup& group, const KnobParam& spec) const;
  void setParam(const juce::String& id, double value);
  void readTempo();
  void layoutSections();
  int naturalWidth(const Section& section) const;
  int sectionHeight(const Section& section, int width) const;
  void placeItems(Section& section);
  void addItem(Section& section, juce::Component* item, bool breakBefore = false);
  ParamKnob* makeKnob(const KnobParam& spec, int diameter = 72);

  BlockContext context;
  juce::String signature;
  double bpm = 120.0;
  std::vector<std::unique_ptr<juce::Component>> owned;
  std::vector<std::unique_ptr<TimeGroup>> timeGroups;
  std::vector<Section> sections;
  // The parameters by id, to update their controls in place.
  std::map<juce::String, ParamKnob*> knobsById;
  std::map<juce::String, ChoiceCell*> choicesById;
  std::map<juce::String, SwitchRow*> switchesById;

  // The graphic equaliser's bands, drawn as sliders over their curve.
  std::unique_ptr<GraphicEqView> graphicEq;

  juce::Rectangle<int> presetsBounds, noteBounds;
  PushButton saveAs{"Save as"};
};

}  // namespace pedaleira::ui

// A parameter as a knob: the spec block.listParams and split.listParams
// report, its position along the arc (with the table's skew) and the text a
// player reads. Shared by the block and split panels.
#pragma once

#include <juce_core/juce_core.h>

namespace pedaleira::ui {

struct KnobParam {
  juce::String id;
  juce::String name;
  juce::String unit;
  juce::String type;  // float, boolean or choice
  double minValue = 0.0, maxValue = 1.0, skew = 1.0, defaultValue = 0.0, value = 0.0;
  bool common = false;
  bool sceneAssigned = false;
  bool expressionAssigned = false;
  bool affectsLatency = false;
  bool tempoSync = false;
  juce::StringArray choices;  // a choice parameter's labels

  // From one entry of a listParams result.
  static KnobParam fromVar(const juce::var& spec);

  double toPosition(double v) const;
  double fromPosition(double position) const;
  // "-2.0 dB", "30%", "450 ms", "1.2 kHz".
  juce::String text(double v) const;
};

juce::String signedDb(double db);

}  // namespace pedaleira::ui

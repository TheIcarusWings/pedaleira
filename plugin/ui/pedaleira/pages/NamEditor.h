// The editor of a NAM capture (the board "Block editor, NAM capture"): what
// the capture file says about itself, its model size, the level knobs, the
// block EQ and the block's input and output meters. The noise gate of the
// board is not an engine setting of a capture block yet and says so.
#pragma once

#include "BlockEqPanel.h"
#include "EditorParts.h"

#include <memory>
#include <vector>

namespace pedaleira::ui {

class NamEditor : public BlockEditorBody {
public:
  explicit NamEditor(EngineClient& engine);
  ~NamEditor() override;

  void showBlock(const BlockContext& block) override;
  void tick() override;

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  // One line of the meter bar: name, bar, level, peak.
  class MeterRow;
  struct Info {
    juce::String file, source, type, gainTag, calibration;
  };

  void readAsset();
  void updateKnobs();

  BlockContext context;
  juce::String assetKey, knobKey;
  Info info;

  std::vector<std::unique_ptr<ParamKnob>> knobs;
  SegmentedControl modelSize{{"Lite", "Half", "Full"}};
  SwitchRow normalize{"Normalize loudness"};
  BlockEqPanel eq;
  std::unique_ptr<MeterRow> inputMeter, outputMeter;
  PushButton swap{"Swap capture"};

  juce::Rectangle<int> capture, levels, gate, meters;
  std::vector<juce::Rectangle<int>> captureRows;
};

}  // namespace pedaleira::ui

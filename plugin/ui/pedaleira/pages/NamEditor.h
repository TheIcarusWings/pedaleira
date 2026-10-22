// The editor of a NAM capture (the board "Block editor, NAM capture"): what
// the capture file says about itself (and, for a capture from TONE3000, the
// tone's image, title, gear, format, creator and licence), its model size, the
// level knobs, the block EQ and the block's input and output meters. The noise gate of the
// board is not an engine setting of a capture block yet and says so.
#pragma once

#include "BlockEqPanel.h"
#include "EditorParts.h"
#include "Tone3000Credit.h"

#include <memory>
#include <vector>

namespace pedaleira::ui {

class NamEditor : public BlockEditorBody {
public:
  explicit NamEditor(EngineClient& engine);
  ~NamEditor() override;

  void showBlock(const BlockContext& block) override;
  void tick() override;
  // Replaces this block's capture with a file, keeping its settings (what
  // Load file... does with the file picked; public for the testbed).
  void swapTo(const juce::File& file);
  // Whether the model size choice is shown: only for a capture that comes in sizes.
  bool modelSizeShown() const { return modelSize.isVisible(); }

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  // One line of the meter bar: name, bar, level, peak.
  class MeterRow;
  struct Info {
    juce::String file, source, type, gainTag, calibration;
    // False for a capture of one size, which has no model size to choose.
    bool sized = true;
    // Where it came from on TONE3000, when it did.
    Tone3000Credit credit;
  };

  void readAsset();
  void updateKnobs();
  void chooseFile();

  BlockContext context;
  juce::String assetKey, knobKey;
  Info info;
  Tone3000Images images{*this};

  std::vector<std::unique_ptr<ParamKnob>> knobs;
  SegmentedControl modelSize{{"Lite", "Half", "Full"}};
  SwitchRow normalize{"Normalize loudness"};
  BlockEqPanel eq;
  std::unique_ptr<MeterRow> inputMeter, outputMeter;
  PushButton swap{"Swap capture"};
  PushButton loadFile{"Load file..."};
  std::unique_ptr<juce::FileChooser> chooser;

  juce::Rectangle<int> capture, levels, gate, meters;
  std::vector<juce::Rectangle<int>> captureRows;
};

}  // namespace pedaleira::ui

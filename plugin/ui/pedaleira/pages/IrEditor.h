// The editor of an impulse response (the board "Block editor, IR"): the IR
// file with its length, rate and channels, the other IRs of its folder, and
// the level knobs. The waveform and frequency response are empty frames
// until the engine can report an IR's samples; phase invert, dual IR and the
// shaping knobs (cuts, trim, length) are not engine settings yet.
#pragma once

#include "EditorParts.h"

#include <memory>
#include <vector>

namespace pedaleira::ui {

class IrEditor : public BlockEditorBody {
public:
  explicit IrEditor(EngineClient& engine);
  ~IrEditor() override;

  void showBlock(const BlockContext& block) override;

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  class FolderButton;
  struct Info {
    juce::String file, length, sampleRate, channels, folder;
    juce::String samples;
    juce::String listTitle;
  };
  struct Alternate {
    juce::String name, path, length;
    bool current = false;
  };

  void readAsset();
  void updateKnobs();
  void swapTo(const juce::File& file);
  void chooseFile();

  BlockContext context;
  juce::String assetKey, knobKey;
  Info info;
  std::vector<Alternate> alternates;

  std::vector<std::unique_ptr<ParamKnob>> knobs;
  std::vector<std::unique_ptr<FolderButton>> folderButtons;
  PushButton loadFile{"Load IR file"};
  std::unique_ptr<juce::FileChooser> chooser;

  juce::Rectangle<int> filePanel, previewPanel, levelsPanel;
  juce::Rectangle<int> fileCard, folderLabel, waveFrame, responseFrame, responseLabel, knobNote, dualDrop;
};

}  // namespace pedaleira::ui

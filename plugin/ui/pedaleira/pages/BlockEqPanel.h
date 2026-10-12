// The block EQ of the NAM editor (the board "Block editor, NAM capture"): the
// response curve of the six bands with a handle on each, the six bands as
// cards, and the fields of the selected one. Everything goes through
// block.setEqBand, block.setEqEnabled, block.setEqPre and block.resetEq; the
// curve is the same RBJ response the engine's filters run.
#pragma once

#include "../EngineClient.h"
#include "../widgets/Controls.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>

namespace pedaleira::ui {

class BlockEqPanel : public juce::Component {
public:
  struct Band {
    juce::String type = "bell";
    double freqHz = 1000.0, gainDb = 0.0, q = 1.0;
  };

  explicit BlockEqPanel(EngineClient& engine);
  ~BlockEqPanel() override;

  // `eq` is the block's params.eq in state.get ({ enabled, pre, bands });
  // `model` names what the EQ sits after or before ("capture", "IR").
  void showBlock(const juce::String& blockId, const juce::var& eq, const juce::String& model);

  // The EQ was reset: the owner reads the block again.
  std::function<void()> onChanged;

  void paint(juce::Graphics& g) override;
  void resized() override;
  void mouseDown(const juce::MouseEvent& event) override;
  void mouseDrag(const juce::MouseEvent& event) override;
  void mouseUp(const juce::MouseEvent& event) override;
  void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;

  // The response of the bands at a frequency, in dB (the engine's filters at
  // 48 kHz), for the curve and for tests.
  static double responseDb(const std::array<Band, 6>& bands, double freqHz);

private:
  class Field;
  void setBand(int index, const Band& band);
  void pushBand(int index);
  void selectBand(int index);
  void updateFields();
  int handleAt(juce::Point<float> position) const;
  juce::Point<float> handlePosition(int index) const;
  juce::Rectangle<float> graphArea() const;
  juce::Rectangle<int> chipBounds(int index) const;
  juce::Point<float> toGraph(juce::Point<float> position) const;

  EngineClient& engine;
  juce::String blockId, model;
  std::array<Band, 6> bands;
  bool enabled = true;
  bool pre = false;
  int selected = 0;
  int dragging = -1;

  juce::Rectangle<int> graph, chips, fieldsRow;
  Toggle power{"Block EQ on"};
  SegmentedControl position{{"Before", "After"}};
  PushButton reset{"Reset", PushButton::Kind::ghost};
  std::array<std::unique_ptr<Field>, 4> fields;
};

}  // namespace pedaleira::ui

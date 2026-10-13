// The graphic equaliser's own view: one slider per band over the response
// the bands make together. Drag a band's handle up or down (shift for fine
// steps), double-click it for 0 dB, or turn the wheel over it. The curve is
// the sum of the bands as the block runs them (peaking filters, about an
// octave wide), so what is drawn is what is heard.
#pragma once

#include "../Tokens.h"
#include "../widgets/Motion.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>

namespace pedaleira::ui {

class GraphicEqView : public juce::Component {
public:
  static constexpr int kBands = 10;

  GraphicEqView();

  // A band's gain from the engine (-12 to 12 dB); does not call onChange.
  void setGain(int band, double db);
  void setBypassed(bool bypassed);
  // The band labels under the sliders ("31.5", "63" ... "16k").
  static juce::String bandLabel(int band);

  std::function<void(int band, double db)> onChange;

  void paint(juce::Graphics& g) override;
  void mouseMove(const juce::MouseEvent& event) override;
  void mouseExit(const juce::MouseEvent& event) override;
  void mouseDown(const juce::MouseEvent& event) override;
  void mouseDrag(const juce::MouseEvent& event) override;
  void mouseUp(const juce::MouseEvent& event) override;
  void mouseDoubleClick(const juce::MouseEvent& event) override;
  void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;

private:
  juce::Rectangle<float> plot() const;
  float bandX(int band) const;
  float yOf(double db) const;
  double dbAt(float y) const;
  int bandAt(juce::Point<float> position) const;
  void set(int band, double db);
  // The response of every band together, in dB, at a frequency.
  double responseDb(double hz) const;

  std::array<double, kBands> gains{};
  bool bypassed = false;
  int hovered = -1;
  int dragging = -1;
  double dragStartDb = 0.0;
  Tween lit{*this};
};

}  // namespace pedaleira::ui

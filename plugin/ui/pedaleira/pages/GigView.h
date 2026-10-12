// The Gig View (the board "Gig View", with the tuner strip of the board
// "Tuner"): a full-window view for the stage. The tuner strip on top, the
// preset and its setlist in large type, the scene, the next preset, the
// tempo with a beat light, the CPU, and the footswitches at 96 px that work
// by click: footswitch.press when the mouse goes down, footswitch.release
// when it lets go. Each shows what the current mode makes of it.
#pragma once

#include "../EngineClient.h"
#include "../widgets/Controls.h"
#include "../widgets/Tile.h"
#include "FootswitchModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cmath>
#include <functional>
#include <memory>

namespace pedaleira::ui {

class GigView : public juce::Component, private juce::Timer {
public:
  explicit GigView(EngineClient& engine);
  ~GigView() override;

  // The tuner reading as the strip draws it: the nearest note, how far off
  // it is in cents and the frequency. Public for the tests.
  struct Reading {
    bool valid = false;
    juce::String note;
    bool sharp = false;
    double cents = 0.0;
    double hz = 0.0;
  };
  // From a frequency, against A = 440.
  static Reading analyse(double frequency, double confidence);
  static bool inTune(double cents) { return std::abs(cents) <= 5.0; }

  // Reads the rig again (called when it changes, and on opening).
  void refresh();

  std::function<void()> onExit;

  // A pedal by index (0 for A), for the tests that click it.
  Footswitch& footswitchAt(int index) { return *switches[static_cast<size_t>(juce::jlimit(0, 7, index))]; }

  void paint(juce::Graphics& g) override;
  void resized() override;
  bool keyPressed(const juce::KeyPress& key) override;
  void visibilityChanged() override;

private:
  void timerCallback() override;
  void readTuner();
  void press(const char* name);
  void release(const char* name);
  void paintStrip(juce::Graphics& g);
  juce::String modeNote() const;

  EngineClient& engine;
  FootswitchModel model;
  juce::uint32 seenRevision = 0;
  double cpu = 0.0;
  Reading reading;
  // Switches pressed and not yet released, released when the view hides
  // (Escape while one is held must not leave a momentary stomp on).
  juce::StringArray held;
  double shownCents = 0.0;
  int ticks = 0;
  // The beat light: which beat of the bar, and whether it is lit.
  int beat = 1;
  bool beatLit = true;

  std::array<std::unique_ptr<Footswitch>, 8> switches;
  Footswitch bankUp{"Bank Up"};
  Footswitch bankDown{"Bank Down"};
  Footswitch tap{"Tap"};
  PushButton leave{"Exit Gig View"};

  juce::Rectangle<int> stripArea, noteArea, scaleArea, readoutArea;
  juce::Rectangle<int> setlistArea, nameArea, sceneArea, nextArea, tempoBox, cpuArea, panelBox, panelHead;
};

}  // namespace pedaleira::ui

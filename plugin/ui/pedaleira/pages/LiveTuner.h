// The Live page's Tuner tab (the board "Live tuner"): the nearest note in
// large type with its octave, the cents off, the frequency, the cents meter
// with the in-tune band, the string guide for standard tuning, and the tuner's
// settings beside them. The tuner listens (tuner.setEnabled) and is polled only
// while the tab is up; the maths is the Gig View strip's (GigView::analyse).
//
// What the engine has no part for yet (the reference pitch, muting the output
// while tuning, hiding the Gig View strip, the strobe, choosing the input) is
// drawn disabled with a line saying it comes in a later build.
#pragma once

#include "../EngineClient.h"
#include "../Type.h"
#include "../widgets/Controls.h"
#include "GigView.h"
#include "LiveParts.h"
#include "SettingsParts.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

namespace pedaleira::ui {

class TunerView : public juce::Component, private juce::Timer {
public:
  explicit TunerView(EngineClient& engine);
  ~TunerView() override;

  // Starts or stops listening: tuner.setEnabled and the poll that follows the
  // reading. The Live page calls it as the page and the tab show and hide.
  void setActive(bool shouldListen);
  bool isActive() const { return active; }
  // Asks the engine to listen again while the tab is up (the Gig View stops
  // the tuner when it closes).
  void resume();
  // Reads what the tab shows of the rig again (the input, the MIDI control).
  void refresh();
  // The tuner's reading as the tab draws it. For the tests.
  const GigView::Reading& currentReading() const { return reading; }
  // The string the guide marks (0 for the sixth string, 5 for the first), or -1.
  int markedString() const { return marked; }

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  void timerCallback() override;
  void readTuner();
  void chooseDisplay();
  void paintReadout(juce::Graphics& g);
  void paintMeter(juce::Graphics& g);
  void paintStrings(juce::Graphics& g);
  void paintSettings(juce::Graphics& g);

  EngineClient& engine;
  bool active = false;
  GigView::Reading reading;
  double shownCents = 0.0;
  int marked = -1;
  // Read from the rig: the row the tuner listens to and the MIDI control.
  juce::String sourceText = "Row 1 " + type::dot() + " Input 1";
  juce::String listeningText = "Listening on Input 1";
  int toggleCc = 45;

  StepButton lower;
  StepButton raise;
  SettingToggle muteToggle{"Mute output while tuning", live::kLater};
  SettingToggle stripToggle{"Show tuner strip in Gig View", live::kLater};
  SelectBox displayBox{"Tuner display"};
  SelectBox sourceBox{"Tuner input source"};

  juce::Rectangle<int> card, head, chip, big, meter, legend, strings, panel, panelTitle;
  juce::Rectangle<int> referenceLabel, referenceRow, referenceValue, referenceHint;
  juce::Rectangle<int> displayLabel, sourceLabel, sourceHint, midiRow;
  std::array<int, 3> dividers{};
};

}  // namespace pedaleira::ui

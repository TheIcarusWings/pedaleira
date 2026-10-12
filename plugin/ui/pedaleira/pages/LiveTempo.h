// The Live page's Tempo tab (the board "Live tempo"): the rig's tempo in
// large type with minus and plus and a typed entry, Tap (tempo.tap) with a
// beat light and the last intervals, where the tempo comes from, and the
// blocks of the preset that follow the tempo with the time of their note.
//
// The metronome, the choice of tempo source and the MIDI clock have no part in
// the engine yet; they are drawn disabled with a line saying they come in a
// later build.
#pragma once

#include "../EngineClient.h"
#include "../Tokens.h"
#include "../widgets/Controls.h"
#include "LiveParts.h"
#include "SettingsParts.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace pedaleira::ui {

class TempoView : public juce::Component, private juce::Timer {
public:
  explicit TempoView(EngineClient& engine);
  ~TempoView() override;

  // Starts or stops following the tempo and the beat light, as the page and
  // the tab show and hide.
  void setActive(bool shouldFollow);
  bool isActive() const { return active; }
  // Reads the tempo, the synced blocks and the MIDI control again.
  void refresh();

  // A tap, at the time the player made it (milliseconds on any clock that
  // moves forward; negative for now). Public for the tests and the testbed.
  void tap(double timeMs = -1.0);
  // Sets the tempo (clamped to 20 to 300) and reads it back.
  void setBpm(double newBpm);
  double tempo() const { return bpm; }

  // The tempo changed here: the header's figure reads it again.
  std::function<void()> onTempoChanged;

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  class TapButton;
  struct SyncedBlock {
    juce::String name;
    Category category = Category::utility;
    bool enabled = true;
    juce::String note;
    double ms = 0.0;
  };

  void timerCallback() override;
  void readTempo();
  void readSynced();
  void commitEdited();
  void layoutNumber();
  juce::String bpmText() const;
  bool controllable() const { return !hostSync; }
  void paintTempoCard(juce::Graphics& g);
  void paintMetronome(juce::Graphics& g);
  void paintSynced(juce::Graphics& g);

  EngineClient& engine;
  bool active = false;
  // The rig's own tempo, what the audio ran with, and whether that came from the host.
  double bpm = 120.0;
  double effectiveBpm = 120.0;
  bool hostSync = false;
  int tapCc = 44;
  juce::uint32 seenRevision = 0;
  int ticks = 0;
  bool beatLit = true;

  // The intervals between the last taps, newest last (up to four), and the
  // time of the last tap.
  std::vector<double> intervals;
  double lastTapMs = -1.0;

  std::vector<SyncedBlock> synced;
  int syncedTotal = 0;

  StepButton down;
  StepButton up;
  juce::Label number;
  std::unique_ptr<TapButton> tapButton;
  SegmentedControl source{{"Preset", "Global", "MIDI clock"}};
  Toggle metronomeSwitch{"Metronome"};
  Toggle sendClock{"Send clock"};
  Toggle receiveClock{"Receive clock"};

  juce::Rectangle<int> tempoCard, metronomeCard, syncedCard, tempoLabel, bpmUnit, tapCaption, tapsArea, sourceArea;
  juce::Rectangle<int> sourceText, sourceHint, metronomeHead, metronomeBody, syncedHead, syncedColumns, clockRow;
  std::vector<juce::Rectangle<int>> syncedRows;
};

}  // namespace pedaleira::ui

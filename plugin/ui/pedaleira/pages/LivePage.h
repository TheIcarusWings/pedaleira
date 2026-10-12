// The Live page (the boards "Live tuner", "Live tempo" and "Looper"): a tab bar
// over the tools for playing, Tuner, Tempo and Looper. Each tab is a view that
// runs only while it is on screen: the tuner listens and is polled, the tempo
// follows the beat light, and the page stops both when it is hidden. The
// Looper is not built; its tab says so.
#pragma once

#include "../EngineClient.h"
#include "LiveTempo.h"
#include "LiveTuner.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>

namespace pedaleira::ui {

class LivePage : public juce::Component {
public:
  enum class Tab { tuner, tempo, looper };

  explicit LivePage(EngineClient& engine);
  ~LivePage() override;

  void showTab(Tab tab);
  Tab currentTab() const { return tab; }
  // The rig changed (an edit, a preset, a footswitch): the tab on screen reads it again.
  void rigChanged();
  // The Gig View closed over this page, and stopped the tuner with it.
  void resumeListening() { tuner.resume(); }

  TunerView& tunerView() { return tuner; }
  TempoView& tempoView() { return tempo; }

  // The tempo was set or tapped here: the header's figure reads it again.
  std::function<void()> onTempoChanged;

  void paint(juce::Graphics& g) override;
  void resized() override;
  void visibilityChanged() override;

private:
  class TabButton;

  // Starts and stops the views to match the page and the tab on screen.
  void updateActive();

  EngineClient& engine;
  Tab tab = Tab::tuner;
  TunerView tuner;
  TempoView tempo;
  std::array<std::unique_ptr<TabButton>, 3> tabButtons;
  juce::Rectangle<int> barArea, looperArea;
};

}  // namespace pedaleira::ui

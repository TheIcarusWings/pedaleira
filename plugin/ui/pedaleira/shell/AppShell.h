// The window's frame on every board: the navigation rail on the left
// (Grid, Library, Capture, Import, Live, Settings), the header along the top
// (setlist and preset, unsaved state, Save, the footswitch mode, tempo, the
// tuner and Gig View buttons, the CPU meter) and the page below it.
#pragma once

#include "../EngineClient.h"
#include "../Icons.h"
#include "../Type.h"
#include "../widgets/Controls.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>

namespace pedaleira::ui {

enum class Page { grid, library, capture, import, live, settings };

class NavRail : public juce::Component {
public:
  NavRail();

  void setPage(Page page);
  std::function<void(Page)> onSelect;
  // The master meters at the foot of the rail: the device input and the
  // main output, [left, right] in dB (meters.get).
  void setMasterLevels(const std::array<float, 2>& input, const std::array<float, 2>& output);

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  struct Item;
  std::array<std::unique_ptr<Item>, 6> items;
  Page page = Page::grid;
  // Levels, peak holds and until when each shows a clip: input 0, output 1.
  std::array<std::array<float, 2>, 2> master{{{-60.0f, -60.0f}, {-60.0f, -60.0f}}};
  std::array<std::array<float, 2>, 2> masterHold{{{-60.0f, -60.0f}, {-60.0f, -60.0f}}};
  std::array<juce::uint32, 2> masterClipUntil{};
  juce::Rectangle<int> meterArea() const;
};

class Header : public juce::Component {
public:
  explicit Header(EngineClient& engine);

  // Reads the library, the footswitch state and the meters again.
  void refresh();
  // Reads only the meters: the CPU figure, and the master levels for the
  // rail (cheap; called more often).
  void refreshMeters();
  // The capture wizard shows Save as a secondary button.
  void setSaveIsPrimary(bool primary);

  std::function<void()> onTuner;
  std::function<void()> onGigView;
  std::function<void(const juce::var& meters)> onMeters;
  // New: start an empty preset; the flag says whether there are unsaved
  // changes to ask about first.
  std::function<void(bool unsaved)> onNewPreset;

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  EngineClient& engine;
  juce::String setlistLine;
  juce::String presetName;
  bool unsaved = false;
  bool saveIsPrimary = true;
  double cpu = 0.0;

  PushButton save{"Save", PushButton::Kind::primary};
  PushButton newPreset{"New"};
  SegmentedControl mode{{"Preset", "Scene", "Stomp", "Hybrid"}};
  PushButton tempo{"120.0 BPM"};
  PushButton tuner{"Tuner"};
  PushButton gigView{"Gig View"};
  juce::Rectangle<int> titleArea, unsavedArea, cpuArea;
};

class AppShell : public juce::Component, private juce::Timer {
public:
  explicit AppShell(EngineClient& engine);
  ~AppShell() override;

  // The page component for each rail item; the shell owns none of them.
  void setPageComponent(Page page, juce::Component* component);
  void showPage(Page page);
  Page currentPage() const { return page; }

  // The Gig View covers the whole window (rail and header too) while it is
  // up; the header's Gig View button opens it. The shell owns neither view.
  void setGigView(juce::Component* component);
  void showGigView(bool shown);
  bool isGigViewShown() const;
  // The footswitch modes overview takes the place of the page, under the
  // header; choosing a page closes it.
  void setModesView(juce::Component* component);
  void showModesView(bool shown);
  bool isModesViewShown() const;

  // Called when the rig's revision moves (an edit here, by an agent or
  // from a footswitch), so the pages can read it again.
  std::function<void()> onRigChanged;
  // A page was chosen on the rail (or by showPage): it can read its data again.
  std::function<void(Page)> onPageShown;
  // The header's Tuner button opened the Live page (after onPageShown): the
  // page can put its Tuner tab up.
  std::function<void()> onTunerRequested;

  Header& header() { return top; }
  NavRail& rail() { return nav; }

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  void timerCallback() override;

  // Keeps the typefaces for as long as the UI lives (type::FontCache).
  type::FontHold fonts;
  EngineClient& engine;
  NavRail nav;
  Header top;
  std::array<juce::Component*, 6> pages{};
  juce::Component* gigView = nullptr;
  juce::Component* modesView = nullptr;
  Page page = Page::grid;
  juce::uint32 seenRevision = 0;
};

}  // namespace pedaleira::ui

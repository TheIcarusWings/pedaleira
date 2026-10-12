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

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  struct Item;
  std::array<std::unique_ptr<Item>, 6> items;
  Page page = Page::grid;
};

class Header : public juce::Component {
public:
  explicit Header(EngineClient& engine);

  // Reads the library, the footswitch state and the meters again.
  void refresh();
  // Reads only the CPU figure (cheap; called more often).
  void refreshMeters();
  // The capture wizard shows Save as a secondary button.
  void setSaveIsPrimary(bool primary);

  std::function<void()> onTuner;
  std::function<void()> onGigView;

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
  int ticks = 0;
};

}  // namespace pedaleira::ui

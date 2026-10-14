// The Settings page (the boards "Settings, audio", "Settings, MIDI",
// "Settings, expression", "Settings, plugins" and "Settings, MCP"): a list of
// sections on the left and the chosen one on the right, in a column that
// scrolls. Each section is a SettingsSection; the page owns them, shows the
// chosen one under its title and keeps one timer, running only while the
// page is on screen, that gives the section a tick for levels and scans.
#pragma once

#include "../DeviceSettings.h"
#include "../EngineClient.h"
#include "../widgets/Dialog.h"
#include "SettingsAgents.h"
#include "SettingsAudio.h"
#include "SettingsCaptures.h"
#include "SettingsExpression.h"
#include "SettingsInfo.h"
#include "SettingsPlugins.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <memory>

namespace pedaleira::ui {

class SettingsPage : public juce::Component, private juce::Timer {
public:
  enum class Part { audio, midi, expression, plugins, captures, agents, remote, about };

  // The device is null when this process has none of its own (a plugin in a
  // host); the audio section then says where it is set.
  SettingsPage(EngineClient& engine, DeviceSettings* device);
  ~SettingsPage() override;

  // The page was opened or the rig changed: the chosen section reads again.
  void refresh();
  // The rig changed: the sections that show the rig read it again.
  void rigChanged();
  void showPart(Part part);
  Part currentPart() const { return part; }
  SettingsSection& section(Part which) { return *sections[static_cast<size_t>(which)]; }
  Dialog& dialog() { return prompt; }
  // Testbed and tests: what the timer does, and the scroll position.
  void tickNow() { timerCallback(); }
  void scrollTo(int y) { viewport.setViewPosition(0, y); }

  void paint(juce::Graphics& g) override;
  void resized() override;
  void visibilityChanged() override;

private:
  class NavButton;

  void timerCallback() override;
  void deviceChanged();
  void fitSection();
  juce::Rectangle<int> mainArea() const;
  int blurbHeight() const;

  EngineClient& engine;
  DeviceSettings* device;
  Part part = Part::audio;
  std::array<std::unique_ptr<SettingsSection>, 8> sections;
  std::array<std::unique_ptr<NavButton>, 8> navButtons;
  juce::Viewport viewport;
  Dialog prompt;
};

}  // namespace pedaleira::ui

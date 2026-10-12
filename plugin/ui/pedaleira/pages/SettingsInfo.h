// The three Settings sub pages that mostly tell: "MIDI mapping" (the board
// "Settings, MIDI", read only: the ui-plan defers editing), "Remote" (a later
// build, UI step 10) and "About" (name, version, licences and credits).
#pragma once

#include "../DeviceSettings.h"
#include "SettingsParts.h"

#include <memory>
#include <vector>

namespace pedaleira::ui {

// What midi.getMap returns, as the board's table: the controllers the
// footswitch conventions listen to and the mappings the player added, and
// the MIDI inputs the standalone application listens to (those switches are
// the device layer, DeviceSettings). Learn, the clock and outgoing messages
// are not built.
class SettingsMidi : public SettingsSection {
public:
  SettingsMidi(EngineClient& engine, DeviceSettings* device);

  void refresh() override;
  int heightFor(int width) const override;
  void deviceChanged() { refresh(); }

  // The rows of the table as "function | message | values", for tests.
  juce::StringArray tableRows() const;

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  struct Row {
    juce::String function, message, values;
    bool replaced = false;
  };
  struct Layout {
    juce::Rectangle<int> mapping, head, devices, outgoing, note, channel, inputsLabel, inputsNote, outgoingBox;
    std::vector<juce::Rectangle<int>> rows, inputRows;
    int height = 0;
  };
  Layout layoutFor(int width) const;

  DeviceSettings* device;
  std::vector<Row> table;
  std::vector<std::pair<juce::String, juce::String>> inputs;
  std::vector<std::unique_ptr<SettingToggle>> inputToggles;
  juce::String channelText;
  Layout lay;
};

// Remote: the phone Gig View comes with UI step 10.
class SettingsRemote : public SettingsSection {
public:
  explicit SettingsRemote(EngineClient& engine);
  int heightFor(int width) const override;
  void paint(juce::Graphics& g) override;
};

class SettingsAbout : public SettingsSection {
public:
  explicit SettingsAbout(EngineClient& engine);

  void refresh() override;
  int heightFor(int width) const override;
  const juce::String& version() const { return versionText; }

  void paint(juce::Graphics& g) override;

private:
  struct Block {
    const char* heading;
    juce::String text;
  };
  struct Layout {
    std::vector<juce::Rectangle<int>> cards;
    std::vector<juce::Rectangle<int>> texts;
    int height = 0;
  };
  Layout layoutFor(int width) const;
  std::vector<Block> blocks() const;

  juce::String versionText;
};

}  // namespace pedaleira::ui

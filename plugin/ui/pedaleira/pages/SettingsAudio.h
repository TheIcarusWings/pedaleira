// Settings, "Audio and calibration" (the board "Settings, audio"): the device
// of the standalone application (driver, devices, rate, buffer, channels, the
// one part that is not a command, see DeviceSettings.h), the inputs and
// outputs with what uses them, the calibration of the interface, the
// oversampling and the noise gate. Everything the engine holds goes through
// param.list and param.set.
#pragma once

#include "../DeviceSettings.h"
#include "GridModel.h"
#include "SettingsParts.h"

#include <memory>
#include <vector>

namespace pedaleira::ui {

class SettingsAudio : public SettingsSection {
public:
  // The device is null when this process has no audio device of its own (the
  // card says where it is set).
  SettingsAudio(EngineClient& engine, DeviceSettings* device);
  ~SettingsAudio() override;

  void refresh() override;
  void tick() override;
  int heightFor(int width) const override;
  // The device manager moved (a hot plug, a driver panel): read it again.
  void deviceChanged();
  // What the Buffer size menu does; public for the testbed.
  void chooseBuffer(int samples);

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  struct Layout {
    juce::Rectangle<int> device, inputs, gate, outputs, calibration, performance;
    juce::Rectangle<int> noDevice, driver, output, input, linked, rate, buffer;
    juce::Rectangle<int> strip, error, mic, hear, buttons;
    juce::Rectangle<int> inputHead, inputMore, outputHead;
    std::vector<juce::Rectangle<int>> inputRows, outputRows;
    juce::Rectangle<int> gateSwitch, threshold, release, hold, range;
    juce::Rectangle<int> maxLabel, maxField, maxNote, applyRow, loudRow, measure, measureNote, levelRow;
    juce::Rectangle<int> osRow, factorRow, multicoreRow, cpuRow;
    int height = 0;
  };

  Layout layoutFor(int width) const;
  void relayout();
  void applyLayout();
  void readParams();
  void refreshDevice();
  void rebuildRows();
  void applied(const juce::var& result);
  void setParam(const juce::String& id, double value);
  double param(const juce::String& id, double fallback = 0.0) const;
  void toggleInput(int channel, bool on);
  void toggleOutput(int pair, bool on);
  juce::String usedByInput(int channel) const;
  juce::String usedByOutput(int pair) const;
  int inputRowCount() const;
  int outputRowCount() const;
  bool micDenied() const;
  void menuFor(SelectBox& box, const juce::String& key);

  DeviceSettings* device;
  juce::var dev, params;
  GridModel grid;
  juce::String lastError;
  double inputDb = -60.0;
  double latency = 0.0;
  int dropouts = -1;
  Layout lay;

  SelectBox driverBox{"Driver"};
  SelectBox outputBox{"Output device"};
  SelectBox inputBox{"Input device"};
  SelectBox linkedBox{"Device"};
  SelectBox rateBox{"Sample rate"};
  SelectBox bufferBox{"Buffer size"};
  SettingToggle hearToggle{"Hear yourself", "Plays your input through the outputs."};
  PushButton testTone{"Play test tone"};
  PushButton controlPanel{"Driver panel"};
  PushButton micFix{"Open microphone settings"};
  std::vector<std::unique_ptr<Toggle>> inputToggles, outputToggles;

  SettingToggle gateSwitch{"Gate on"};
  UnitField threshold{"Gate threshold", "dB", 1};
  UnitField release{"Gate release", "ms", 0};
  UnitField hold{"Gate hold", "ms", 0};
  UnitField range{"Gate range", "dB", 0};

  UnitField maxLevel{"Interface max input level", "dBu", 1};
  SettingToggle applyCalibration{"Apply input calibration to NAM blocks"};
  UnitField loudness{"Loudness target", "LUFS", 1};
  PushButton measure{"Measure with a test tone"};

  SettingToggle oversampling{"Oversampling for NAM blocks"};
  SelectBox factorBox{"Oversampling factor"};
};

}  // namespace pedaleira::ui

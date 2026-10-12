// The standalone application's audio device, behind DeviceSettings. The one
// place in the Pedaleira UI that reaches JUCE's standalone holder (and so
// the AudioDeviceManager) instead of a command, because the device belongs
// to the application window and not to the engine. It wraps the engine's StandaloneAudioSettings, which
// already carries the product's device policy (48 kHz and 128 samples first,
// remembered setups, monitoring against feedback), so this page and the
// classic editor treat a device the same way.
//
// Compiled into the application's target only; the testbed has no holder.
#pragma once

#include "../DeviceSettings.h"

#include <memory>

class StandaloneAudioSettings;
class TONE3000Processor;

namespace pedaleira::ui {

class StandaloneDevice final : public DeviceSettings {
public:
  // Null when this process has no standalone holder (a plugin in a host).
  static std::unique_ptr<StandaloneDevice> create(TONE3000Processor& processor);
  ~StandaloneDevice() override;

  juce::var state() override;
  juce::var setDeviceType(const juce::String& typeName) override;
  juce::var setDevice(const juce::String& kind, const juce::String& name) override;
  juce::var setInputChannels(const juce::Array<juce::var>& indices) override;
  juce::var setOutputPair(int pair) override;
  juce::var setSampleRate(double rate) override;
  juce::var setBufferSize(int samples) override;
  juce::var setHearYourself(bool hear) override;
  juce::var playTestTone() override;
  juce::var openControlPanel() override;
  juce::var openMicSettings() override;
  juce::var setMidiInputEnabled(const juce::String& id, bool enabled) override;
  double latencyMs() override;
  int dropouts() override;

private:
  explicit StandaloneDevice(TONE3000Processor& processor);

  std::unique_ptr<StandaloneAudioSettings> settings;
};

}  // namespace pedaleira::ui

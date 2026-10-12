// The audio device of the standalone application: driver, input and output
// device, sample rate, buffer size, channels. This is the one part of the
// Settings page that is not a command (with PluginWindow, the other
// documented exception): the device belongs to the standalone
// window and not to the engine, so no command covers it. The page talks to
// this interface; the standalone application backs it with the JUCE holder
// (standalone/StandaloneDevice.h), the testbed with a canned device.
//
// Message thread only.
#pragma once

#include <juce_core/juce_core.h>

#include <functional>

namespace pedaleira::ui {

class DeviceSettings {
public:
  virtual ~DeviceSettings() = default;

  // The device as the page draws it: deviceTypes, currentType, separateIO,
  // inputDevices, outputDevices, inputDevice, outputDevice, deviceOpen,
  // inputChannels (index, name, active), outputPairs, activeOutputPair,
  // sampleRates, bufferSizes, sampleRate, bufferSize, hasControlPanel,
  // hearYourself, micPermission, midiInputs (id, name, enabled).
  virtual juce::var state() = 0;

  // Each answers { ok, error }; the error is a sentence for the player.
  virtual juce::var setDeviceType(const juce::String& typeName) = 0;
  // kind: input, output or linked (a driver with one device for both).
  virtual juce::var setDevice(const juce::String& kind, const juce::String& name) = 0;
  // Active input channels by device index: one for mono, two for stereo.
  virtual juce::var setInputChannels(const juce::Array<juce::var>& indices) = 0;
  virtual juce::var setOutputPair(int pair) = 0;
  virtual juce::var setSampleRate(double rate) = 0;
  virtual juce::var setBufferSize(int samples) = 0;
  virtual juce::var setHearYourself(bool hear) = 0;
  virtual juce::var playTestTone() = 0;
  virtual juce::var openControlPanel() = 0;
  virtual juce::var openMicSettings() = 0;
  virtual juce::var setMidiInputEnabled(const juce::String& id, bool enabled) = 0;

  // Input plus output latency in milliseconds, 0 when the driver does not say.
  virtual double latencyMs() = 0;
  // Dropouts since the device opened, -1 when the driver does not count them.
  virtual int dropouts() = 0;

  // The device changed (a hot plug, a driver panel, a setter): the page
  // reads it again. The page clears this when it goes.
  std::function<void()> onChanged;
};

}  // namespace pedaleira::ui

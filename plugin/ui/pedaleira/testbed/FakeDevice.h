// A canned audio device for the testbed: the standalone application's device
// without a sound card (the testbed is headless), so the Settings page can be
// drawn and driven. Setters change what state() answers, as a real device
// would; nothing here makes a sound.
#pragma once

#include "DeviceSettings.h"

namespace pedaleira::ui::testbed {

class FakeDevice final : public DeviceSettings {
public:
  struct Setup {
    // More than one driver shows the driver select.
    int drivers = 1;
    int inputChannels = 2;
    // Output channels; more than two give a pair to pick.
    int outputChannels = 4;
    int dropouts = 0;
    bool micDenied = false;
    bool controlPanel = false;
    // An error sentence shown under the latency strip, as after a refused device.
    juce::String error;
  };

  explicit FakeDevice(const Setup& setup) : config(setup) {
    // Guitar on the first input, as the interface's own default.
    for (int i = 0; i < config.inputChannels; ++i)
      active.add(i == 0);
  }

  juce::var state() override {
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    juce::Array<juce::var> types;
    types.add("CoreAudio");
    if (config.drivers > 1)
      types.add("ASIO");
    obj->setProperty("deviceTypes", types);
    obj->setProperty("currentType", "CoreAudio");
    obj->setProperty("separateIO", true);
    juce::Array<juce::var> devices;
    devices.add("Universal Audio Volt 876");
    devices.add("MacBook Pro Microphone");
    obj->setProperty("inputDevices", devices);
    juce::Array<juce::var> outputs;
    outputs.add("Universal Audio Volt 876");
    outputs.add("MacBook Pro Speakers");
    obj->setProperty("outputDevices", outputs);
    obj->setProperty("inputDevice", "Universal Audio Volt 876");
    obj->setProperty("outputDevice", "Universal Audio Volt 876");
    obj->setProperty("deviceOpen", true);
    juce::Array<juce::var> channels;
    for (int i = 0; i < config.inputChannels; ++i) {
      juce::DynamicObject::Ptr channel = new juce::DynamicObject();
      channel->setProperty("index", i);
      channel->setProperty("name", "Input " + juce::String(i + 1));
      channel->setProperty("active", active[i]);
      channels.add(juce::var(channel.get()));
    }
    obj->setProperty("inputChannels", channels);
    juce::Array<juce::var> pairs;
    if (config.outputChannels > 2)
      for (int i = 0; i + 1 < config.outputChannels; i += 2)
        pairs.add("Output " + juce::String(i + 1) + " + " + juce::String(i + 2));
    obj->setProperty("outputPairs", pairs);
    obj->setProperty("activeOutputPair", pairs.isEmpty() ? -1 : pair);
    juce::Array<juce::var> rates;
    for (double r : {44100.0, 48000.0, 88200.0, 96000.0})
      rates.add(r);
    obj->setProperty("sampleRates", rates);
    juce::Array<juce::var> buffers;
    for (int b : {32, 64, 128, 256, 512})
      buffers.add(b);
    obj->setProperty("bufferSizes", buffers);
    obj->setProperty("sampleRate", rate);
    obj->setProperty("bufferSize", buffer);
    obj->setProperty("hasControlPanel", config.controlPanel);
    obj->setProperty("hearYourself", hear);
    obj->setProperty("micPermission", config.micDenied ? "denied" : "granted");
    juce::Array<juce::var> midi;
    for (const char* name : {"Chocolate Footswitch", "IAC Driver Bus 1"}) {
      juce::DynamicObject::Ptr input = new juce::DynamicObject();
      input->setProperty("id", name);
      input->setProperty("name", name);
      input->setProperty("enabled", midiOn.contains(name));
      midi.add(juce::var(input.get()));
    }
    obj->setProperty("midiInputs", midi);
    return juce::var(obj.get());
  }

  juce::var setDeviceType(const juce::String&) override { return ok(); }
  juce::var setDevice(const juce::String&, const juce::String&) override { return ok(); }
  juce::var setInputChannels(const juce::Array<juce::var>& indices) override {
    for (int i = 0; i < active.size(); ++i)
      active.set(i, indices.contains(i));
    return ok();
  }
  juce::var setOutputPair(int index) override {
    pair = index;
    return ok();
  }
  juce::var setSampleRate(double value) override {
    rate = value;
    return ok();
  }
  juce::var setBufferSize(int samples) override {
    buffer = samples;
    return ok();
  }
  juce::var setHearYourself(bool on) override {
    hear = on;
    return ok();
  }
  juce::var playTestTone() override { return ok(); }
  juce::var openControlPanel() override { return ok(); }
  juce::var openMicSettings() override { return ok(); }
  juce::var setMidiInputEnabled(const juce::String& id, bool enabled) override {
    if (enabled)
      midiOn.addIfNotAlreadyThere(id);
    else
      midiOn.removeString(id);
    return ok();
  }
  double latencyMs() override { return 7.9; }
  int dropouts() override { return config.dropouts; }

private:
  juce::var ok() const {
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("ok", config.error.isEmpty());
    obj->setProperty("error", config.error);
    return juce::var(obj.get());
  }

  Setup config;
  juce::Array<bool> active;
  juce::StringArray midiOn{"Chocolate Footswitch"};
  double rate = 48000.0;
  int buffer = 128;
  int pair = 0;
  bool hear = true;
};

}  // namespace pedaleira::ui::testbed

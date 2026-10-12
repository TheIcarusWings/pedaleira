#include "StandaloneDevice.h"

#include "Processor.h"
#include "StandaloneAudioSettings.h"

// The standalone filter window header expects the full GUI/audio module set
// to be visible first (the order StandaloneAudioSettings.cpp uses).
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_plugin_client/juce_audio_plugin_client.h>

#if JucePlugin_Build_Standalone && !JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif

namespace pedaleira::ui {

namespace {

// The device the holder has open, or null. This is the reach into the
// holder; everything else goes through StandaloneAudioSettings.
juce::AudioIODevice* openDevice() {
#if JucePlugin_Build_Standalone && !JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP
  if (auto* holder = juce::StandalonePluginHolder::getInstance())
    return holder->deviceManager.getCurrentAudioDevice();
#endif
  return nullptr;
}

}  // namespace

std::unique_ptr<StandaloneDevice> StandaloneDevice::create(TONE3000Processor& processor) {
  if (!StandaloneAudioSettings::isAvailable())
    return nullptr;
  return std::unique_ptr<StandaloneDevice>(new StandaloneDevice(processor));
}

StandaloneDevice::StandaloneDevice(TONE3000Processor& processor) {
  settings = std::make_unique<StandaloneAudioSettings>(processor, [this] {
    if (onChanged)
      onChanged();
  });
}

StandaloneDevice::~StandaloneDevice() = default;

juce::var StandaloneDevice::state() { return settings->getState(); }
juce::var StandaloneDevice::setDeviceType(const juce::String& typeName) { return settings->setDeviceType(typeName); }
juce::var StandaloneDevice::setDevice(const juce::String& kind, const juce::String& name) {
  return settings->setDevice(kind, name);
}
juce::var StandaloneDevice::setInputChannels(const juce::Array<juce::var>& indices) {
  return settings->setInputChannels(indices);
}
juce::var StandaloneDevice::setOutputPair(int pair) { return settings->setOutputPair(pair); }
juce::var StandaloneDevice::setSampleRate(double rate) { return settings->setSampleRate(rate); }
juce::var StandaloneDevice::setBufferSize(int samples) { return settings->setBufferSize(samples); }
juce::var StandaloneDevice::setHearYourself(bool hear) { return settings->setHearYourself(hear); }
juce::var StandaloneDevice::playTestTone() { return settings->playTestTone(); }
juce::var StandaloneDevice::openControlPanel() { return settings->openControlPanel(); }
juce::var StandaloneDevice::openMicSettings() { return settings->openMicSettings(); }
juce::var StandaloneDevice::setMidiInputEnabled(const juce::String& id, bool enabled) {
  return settings->setMidiInputEnabled(id, enabled);
}

double StandaloneDevice::latencyMs() {
  auto* device = openDevice();
  if (device == nullptr || device->getCurrentSampleRate() <= 0.0)
    return 0.0;
  const int samples = device->getInputLatencyInSamples() + device->getOutputLatencyInSamples();
  return samples * 1000.0 / device->getCurrentSampleRate();
}

int StandaloneDevice::dropouts() {
  auto* device = openDevice();
  return device != nullptr ? device->getXRunCount() : -1;
}

}  // namespace pedaleira::ui

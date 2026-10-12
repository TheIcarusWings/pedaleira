// PedaleiraTestFx: the plugin the hosting tests host. Built as a VST3 with the
// test suite only, never installed and never shipped.
//
// What it does is exact on purpose, so a test can state the samples it
// expects: out = gain * in delayed by `delaySamples`, the same on every
// channel, with no smoothing. The delay is real and is what the plugin
// reports as its latency, so the host's compensation has something true to
// compensate. State is the two parameters.
//
// Layouts: mono or stereo in, mono or stereo out, never more outputs than a
// stereo pair. PEDALEIRA_TESTFX_LAYOUT in the environment narrows that to one
// layout ("mono", "monoToStereo") so the host's layout negotiation can be
// walked down its list without a second plugin binary; PEDALEIRA_TESTFX_THROW
// makes processBlock throw, for the host's failed-block path, and
// PEDALEIRA_TESTFX_EDITOR gives it a generic editor (it has none otherwise,
// so a test without a display never opens one).
#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

#if JUCE_WINDOWS
#include <windows.h>
#endif

namespace {

constexpr int kMaxDelay = 4096;

// The process environment, not the C runtime's copy of it: on Windows a
// plugin can carry its own runtime, whose copy was taken when the module was
// loaded and never sees what the host sets afterwards.
std::string environment(const char* name) {
#if JUCE_WINDOWS
  char value[64] = {};
  const DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
  return length > 0 && length < sizeof(value) ? std::string(value, length) : std::string();
#else
  const char* value = std::getenv(name);
  return value != nullptr ? value : "";
#endif
}

}  // namespace

class PedaleiraTestFx final : public juce::AudioProcessor,
                            private juce::AudioProcessorParameter::Listener,
                            private juce::AsyncUpdater {
public:
  PedaleiraTestFx()
      : AudioProcessor(initialBuses()), layoutMode(environment("PEDALEIRA_TESTFX_LAYOUT")),
        throwInProcess(!environment("PEDALEIRA_TESTFX_THROW").empty()) {
    addParameter(gain = new juce::AudioParameterFloat(juce::ParameterID{"gain", 1}, "Gain",
                                                      juce::NormalisableRange<float>(0.0f, 2.0f),
                                                      1.0f));
    addParameter(delay = new juce::AudioParameterInt(juce::ParameterID{"delaySamples", 1},
                                                     "Delay Samples", 0, kMaxDelay, 0));
    delay->addListener(this);
  }

  ~PedaleiraTestFx() override {
    delay->removeListener(this);
    cancelPendingUpdate();
  }

  const juce::String getName() const override { return "PedaleiraTestFx"; }
  bool acceptsMidi() const override { return false; }
  bool producesMidi() const override { return false; }
  double getTailLengthSeconds() const override { return 0.0; }
  bool hasEditor() const override { return withEditor; }
  juce::AudioProcessorEditor* createEditor() override {
    return withEditor ? new juce::GenericAudioProcessorEditor(*this) : nullptr;
  }
  int getNumPrograms() override { return 1; }
  int getCurrentProgram() override { return 0; }
  void setCurrentProgram(int) override {}
  const juce::String getProgramName(int) override { return {}; }
  void changeProgramName(int, const juce::String&) override {}

  // The layout the plugin comes up in has to be one it supports.
  static BusesProperties initialBuses() {
    const std::string layout = environment("PEDALEIRA_TESTFX_LAYOUT");
    const bool monoIn = layout == "mono" || layout == "monoToStereo";
    const bool monoOut = layout == "mono";
    return BusesProperties()
        .withInput("Input",
                   monoIn ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo(), true)
        .withOutput("Output",
                    monoOut ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo(),
                    true);
  }

  bool isBusesLayoutSupported(const BusesLayout& layouts) const override {
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    const bool monoIn = in == juce::AudioChannelSet::mono();
    const bool monoOut = out == juce::AudioChannelSet::mono();
    const bool stereoIn = in == juce::AudioChannelSet::stereo();
    const bool stereoOut = out == juce::AudioChannelSet::stereo();
    if (layoutMode == "mono")
      return monoIn && monoOut;
    if (layoutMode == "monoToStereo")
      return monoIn && stereoOut;
    return (monoIn || stereoIn) && (monoOut || stereoOut) && !(stereoIn && monoOut);
  }

  void prepareToPlay(double, int) override {
    for (auto& line : lines)
      line.assign(static_cast<size_t>(kMaxDelay + 1), 0.0f);
    writeIndex = 0;
    setLatencySamples(delay->get());
  }

  void releaseResources() override {}

  void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
    if (throwInProcess)
      throw std::runtime_error("PedaleiraTestFx: asked to fail");

    const int inChannels = getTotalNumInputChannels();
    const int outChannels = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();
    const float g = gain->get();
    const int d = juce::jlimit(0, kMaxDelay, delay->get());
    const int size = kMaxDelay + 1;

    // A mono input feeds every output; otherwise channel for channel.
    if (inChannels < outChannels)
      for (int ch = inChannels; ch < outChannels; ++ch)
        buffer.copyFrom(ch, 0, buffer, 0, 0, numSamples);

    for (int ch = 0; ch < juce::jmin(outChannels, 2); ++ch) {
      auto& line = lines[static_cast<size_t>(ch)];
      if (line.empty())
        continue;
      float* x = buffer.getWritePointer(ch);
      int w = writeIndex;
      for (int i = 0; i < numSamples; ++i) {
        line[static_cast<size_t>(w)] = x[i];
        int r = w - d;
        if (r < 0)
          r += size;
        x[i] = g * line[static_cast<size_t>(r)];
        if (++w == size)
          w = 0;
      }
    }
    writeIndex = (writeIndex + numSamples) % size;
  }

  void getStateInformation(juce::MemoryBlock& destData) override {
    juce::XmlElement state("PedaleiraTestFx");
    state.setAttribute("gain", static_cast<double>(gain->get()));
    state.setAttribute("delaySamples", delay->get());
    copyXmlToBinary(state, destData);
  }

  void setStateInformation(const void* data, int sizeInBytes) override {
    if (const auto state = getXmlFromBinary(data, sizeInBytes)) {
      *gain = static_cast<float>(state->getDoubleAttribute("gain", 1.0));
      *delay = state->getIntAttribute("delaySamples", 0);
    }
  }

private:
  // The delay is the latency. Reported from the message thread, where a host
  // expects the notification; a change that arrives on the audio thread is
  // bounced there.
  void parameterValueChanged(int, float) override {
    if (juce::MessageManager::existsAndIsCurrentThread())
      setLatencySamples(delay->get());
    else
      triggerAsyncUpdate();
  }
  void parameterGestureChanged(int, bool) override {}
  void handleAsyncUpdate() override { setLatencySamples(delay->get()); }

  juce::AudioParameterFloat* gain = nullptr;
  juce::AudioParameterInt* delay = nullptr;
  const std::string layoutMode;
  const bool throwInProcess;
  const bool withEditor = !environment("PEDALEIRA_TESTFX_EDITOR").empty();
  std::vector<float> lines[2];
  int writeIndex = 0;
};

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PedaleiraTestFx(); }

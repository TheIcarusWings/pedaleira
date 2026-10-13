#pragma once
// The plugin reference mode of PedaleiraMatch (docs/matching.md, "Matching
// against a plugin"): a plugin the user owns is hosted offline, driven with
// the test signal, and what it returns is written as a reference recording
// to be compared with one of our own effects.
//
// Nothing a third-party plugin produces is shipped or committed: the output
// goes where --out says, and the tool refuses a folder inside this
// repository's checkout.
//
// The hosting is the app's own (HostedPluginInstance, PluginScanner), so the
// channel handling and the latency are the ones a hosted block has.
#include "HostedPluginBlockProcessor.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace MatchPlugin {

struct PluginError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// The rate and the largest block the plugin is prepared for.
constexpr double kRate = 48000.0;
constexpr int kDefaultBlock = 512;

// One parameter to set. The plugin's own parameter row (`index`, as
// plugin-params lists it) or its name; a normalised value or text the plugin
// converts.
struct Setting {
  enum class Target { name, index };
  enum class Kind { value, text };
  Target target{Target::name};
  juce::String name;
  int index{-1};
  Kind kind{Kind::value};
  double value{0.0};
  juce::String text;
};

// A "NAME=VALUE" / "INDEX=VALUE" argument of the command line.
Setting parseSetting(Setting::Target target, Setting::Kind kind, const juce::String& argument);

struct ParamInfo {
  int index{0};
  juce::String name;
  juce::String id;
  juce::String label;
  float value{0.0f};
  juce::String text;
  int steps{0};
};

// What a setting did: the row it hit and the value and text the plugin holds
// afterwards.
struct AppliedSetting {
  int index{0};
  juce::String name;
  Setting request;
  float value{0.0f};
  juce::String text;
};

struct Options {
  // A VST3 bundle's path, or an Audio Unit identifier ("AudioUnit:Effects/aufx,dely,appl").
  juce::String plugin;
  juce::File state;
  juce::File input;
  juce::File output;
  std::vector<Setting> settings;
  int block{kDefaultBlock};
  double tailSeconds{4.0};
  // 0 = the plugin's output width (2 when it is stereo, else 1).
  int laneChannels{0};
  bool json{false};
};

// The scanner's scratch cache, removed when the last reference goes.
struct ScratchDir {
  ScratchDir();
  ~ScratchDir();
  ScratchDir(const ScratchDir&) = delete;
  ScratchDir& operator=(const ScratchDir&) = delete;
  juce::File dir;
  juce::File previous;
};
// A plugin loaded and ready: scanned in the helper process first (a broken
// plugin costs the helper, not this tool), then created on this thread,
// which must be the message thread.
struct Loaded {
  std::shared_ptr<ScratchDir> scratch;
  std::shared_ptr<HostedPluginInstance> instance;
  juce::PluginDescription description;
  juce::String stateKind;  // "", "vstpreset", "chunk"
};

// Throws PluginError. Redirects the scanner to a scratch cache and keeps it
// off the machine's plugin folders for the rest of the process.
Loaded load(const juce::String& plugin, const juce::File& state);

std::vector<ParamInfo> listParameters(const HostedPluginInstance& instance);

// Throws PluginError for an unknown or ambiguous name or index, or text the
// plugin does not understand.
AppliedSetting apply(HostedPluginInstance& instance, const Setting& setting);

struct Rendered {
  juce::AudioBuffer<float> audio;  // laneChannels x (input frames + tail frames)
  int latencySamples{0};            // compensated: already cut off the head
  int laneChannels{0};
};

// Runs `input` (one channel, 48 kHz) through the prepared plugin and returns
// the result with the plugin's reported latency removed, so sample n of the
// output answers sample n of the input. `tailFrames` more are rendered past
// the end of the input.
Rendered render(HostedPluginInstance& instance, const juce::AudioBuffer<float>& input,
                int tailFrames, int block, int laneChannels);

// The repository checkout `file` lives in (a folder holding .git and
// plugin/src/EffectMatch.cpp), or an empty File.
juce::File repositoryOf(const juce::File& file);

// plugin-params and plugin-render. Exit codes as the other commands: 0, or
// an exception.
int runParams(const Options& options);
int runRender(const Options& options);

}  // namespace MatchPlugin

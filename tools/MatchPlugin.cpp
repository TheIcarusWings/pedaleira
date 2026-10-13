#include "MatchPlugin.h"

#include "EffectMatch.h"
#include "PluginScanner.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>

namespace MatchPlugin {

namespace {

// Pumps the message loop for a moment: a plugin reports some changes (its
// latency after a parameter moved) in a message of its own.
void settle(int milliseconds) {
  if (auto* manager = juce::MessageManager::getInstanceWithoutCreating())
    manager->runDispatchLoopUntil(milliseconds);
}

juce::String trimmedLower(const juce::String& text) { return text.trim().toLowerCase(); }

bool isContinuous(int steps) { return steps <= 0 || steps >= 0x7fffffff; }

juce::File absoluteFile(const juce::File& file) {
  return juce::File::getCurrentWorkingDirectory().getChildFile(file.getFullPathName());
}

}  // namespace

ScratchDir::ScratchDir()
    : dir(juce::File::getSpecialLocation(juce::File::tempDirectory)
              .getChildFile("PedaleiraMatch-plugins-" + juce::Uuid().toDashedString())) {
  if (!dir.createDirectory())
    throw PluginError("cannot create a temporary directory in " +
                      dir.getParentDirectory().getFullPathName().toStdString());
  // A scratch cache and no default locations: this run neither reads nor
  // writes the user's plugin cache, and scans nothing but the one file.
  previous = PluginScanner::setCacheDirForTesting(dir);
  PluginScanner::setDefaultLocationsEnabled(false);
}

ScratchDir::~ScratchDir() {
  PluginScanner::setCacheDirForTesting(previous);
  dir.deleteRecursively();
}

// ####################
// SETTINGS
// ####################
Setting parseSetting(Setting::Target target, Setting::Kind kind, const juce::String& argument) {
  const int equals = argument.indexOfChar('=');
  if (equals <= 0 || equals == argument.length() - 1)
    throw PluginError("expected NAME=VALUE, got '" + argument.toStdString() + "'");
  Setting setting;
  setting.target = target;
  setting.kind = kind;
  const juce::String key = argument.substring(0, equals).trim();
  const juce::String rest = argument.substring(equals + 1).trim();
  if (target == Setting::Target::index) {
    if (key.isEmpty() || !key.containsOnly("0123456789"))
      throw PluginError("bad parameter index in '" + argument.toStdString() + "'");
    setting.index = key.getIntValue();
  } else {
    setting.name = key;
  }
  if (kind == Setting::Kind::text) {
    setting.text = rest;
  } else {
    if (!rest.containsOnly("0123456789.-+eE") || rest.isEmpty())
      throw PluginError("bad value in '" + argument.toStdString() + "' (a number from 0 to 1)");
    setting.value = rest.getDoubleValue();
    if (!std::isfinite(setting.value) || setting.value < 0.0 || setting.value > 1.0)
      throw PluginError("value out of range in '" + argument.toStdString() +
                        "' (a normalised value is 0 to 1; use --set-text for a value in the plugin's units)");
  }
  return setting;
}

// ####################
// LOADING
// ####################
Loaded load(const juce::String& pluginKey, const juce::File& state) {
  const bool isIdentifier = pluginKey.startsWith("AudioUnit:");
  juce::String key = pluginKey;
  if (!isIdentifier) {
    const juce::File file = absoluteFile(juce::File(pluginKey));
    if (!file.exists())
      throw PluginError("plugin not found: " + pluginKey.toStdString());
    key = file.getFullPathName();
  }

  Loaded loaded;
  loaded.scratch = std::make_shared<ScratchDir>();
  PluginScanner& scanner = PluginScanner::instance();
  PluginScanner::ScanOptions options;
  options.files.add(key);
  if (!scanner.startScan(options))
    throw PluginError("the plugin scan helper is missing (expected at " +
                      scanner.helperFile().getFullPathName().toStdString() + ")");
  if (!scanner.waitForScan(3 * 60 * 1000)) {
    scanner.cancelScan();
    throw PluginError("timed out scanning " + key.toStdString());
  }

  juce::String why;
  const auto description = scanner.findEffect(key, &why);
  if (!description.has_value()) {
    for (const auto& listed : scanner.list())
      if (listed.status != PluginScanner::PluginStatus::ok)
        why = juce::String(PluginScanner::statusToString(listed.status)) +
              (listed.reason.isNotEmpty() ? " (" + listed.reason + ")" : juce::String());
    throw PluginError("cannot use " + key.toStdString() + ": " +
                      (why.isNotEmpty() ? why.toStdString() : std::string("not an audio effect")));
  }
  loaded.description = *description;

  juce::String error;
  std::unique_ptr<juce::AudioPluginInstance> created = hostedPluginFormats().createPluginInstance(
      loaded.description, kRate, kDefaultBlock, error);
  if (created == nullptr)
    throw PluginError("cannot create " + loaded.description.name.toStdString() + ": " +
                      (error.isNotEmpty() ? error.toStdString() : std::string("unknown error")));
  auto processor = HostedPluginBlockProcessor::create(std::move(created), loaded.description,
                                                      nullptr, error);
  if (processor == nullptr)
    throw PluginError("cannot host " + loaded.description.name.toStdString() + ": " +
                      error.toStdString());
  loaded.instance = processor->instance();
  processor.reset();

  if (state != juce::File()) {
    const juce::File stateFile = absoluteFile(state);
    juce::MemoryBlock bytes;
    if (!stateFile.existsAsFile() || !stateFile.loadFileAsData(bytes) || bytes.isEmpty())
      throw PluginError("state file not found or empty: " + stateFile.getFullPathName().toStdString());
    // A .vstpreset starts with the tag "VST3"; anything else is the plugin's
    // own chunk (what the host's getStateInformation wrote, or what its
    // format saves: an .aupreset for an Audio Unit).
    const bool vstpreset = bytes.getSize() > 48 && std::memcmp(bytes.getData(), "VST3", 4) == 0;
    if (vstpreset) {
      if (!loaded.instance->applyVst3Preset(bytes))
        throw PluginError("the plugin did not accept the .vstpreset " +
                          stateFile.getFullPathName().toStdString());
      loaded.stateKind = "vstpreset";
    } else {
      loaded.instance->applyState(bytes);
      loaded.stateKind = "chunk";
    }
    settle(50);
  }
  return loaded;
}

// ####################
// PARAMETERS
// ####################
std::vector<ParamInfo> listParameters(const HostedPluginInstance& instance) {
  std::vector<ParamInfo> out;
  const auto& table = instance.parameterTable();
  for (size_t i = 0; i < table.size(); ++i) {
    ParamInfo info;
    info.index = static_cast<int>(i);
    info.name = table[i].name;
    info.id = table[i].id;
    info.label = table[i].unit;
    info.value = instance.getParameter(info.index);
    info.text = instance.parameterText(info.index);
    const int steps = instance.parameterNumSteps(info.index);
    info.steps = isContinuous(steps) ? 0 : steps;
    out.push_back(info);
  }
  return out;
}

AppliedSetting apply(HostedPluginInstance& instance, const Setting& setting) {
  const auto& table = instance.parameterTable();
  int row = -1;
  if (setting.target == Setting::Target::index) {
    if (setting.index < 0 || setting.index >= static_cast<int>(table.size()))
      throw PluginError("the plugin has no parameter " + std::to_string(setting.index) + " (0 to " +
                        std::to_string(static_cast<int>(table.size()) - 1) +
                        "; plugin-params lists them)");
    row = setting.index;
  } else {
    const juce::String wanted = trimmedLower(setting.name);
    std::vector<int> byName, byId;
    for (size_t i = 0; i < table.size(); ++i) {
      if (trimmedLower(table[i].name) == wanted)
        byName.push_back(static_cast<int>(i));
      else if (trimmedLower(table[i].id) == wanted)
        byId.push_back(static_cast<int>(i));
    }
    const std::vector<int>& found = !byName.empty() ? byName : byId;
    if (found.empty())
      throw PluginError("the plugin has no parameter named '" + setting.name.toStdString() +
                        "' (plugin-params lists them)");
    if (found.size() > 1) {
      std::string rows;
      for (const int candidate : found)
        rows += (rows.empty() ? "" : ", ") + std::to_string(candidate);
      throw PluginError("the name '" + setting.name.toStdString() +
                        "' matches several parameters (rows " + rows + "); use --set-index");
    }
    row = found.front();
  }

  float value = static_cast<float>(setting.value);
  if (setting.kind == Setting::Kind::text) {
    if (!instance.valueForText(row, setting.text, value))
      throw PluginError("the plugin cannot read '" + setting.text.toStdString() + "' as a value of '" +
                        table[static_cast<size_t>(row)].name.toStdString() + "'");
    value = juce::jlimit(0.0f, 1.0f, value);
  }
  instance.setParameter(row, value);
  settle(10);

  AppliedSetting applied;
  applied.index = row;
  applied.name = table[static_cast<size_t>(row)].name;
  applied.request = setting;
  applied.value = instance.getParameter(row);
  applied.text = instance.parameterText(row);
  return applied;
}

// ####################
// RENDER
// ####################
Rendered render(HostedPluginInstance& instance, const juce::AudioBuffer<float>& input, int tailFrames,
                int block, int laneChannels) {
  if (input.getNumChannels() < 1 || input.getNumSamples() < 1)
    throw PluginError("the input signal is empty");
  if (block < 16 || block > 16384)
    throw PluginError("--block must be 16 to 16384 samples");
  if (laneChannels < 1)
    laneChannels = instance.outputChannels() >= 2 ? 2 : 1;
  laneChannels = juce::jlimit(1, 2, laneChannels);

  instance.prepare(kRate, block);
  settle(50);
  if (instance.latencyChangePending())
    instance.commitLatency();
  const int latency = instance.latchedLatency();
  const int inFrames = input.getNumSamples();
  const int outFrames = inFrames + juce::jmax(0, tailFrames);
  const int total = outFrames + latency;

  juce::AudioBuffer<float> lane(laneChannels, total);
  lane.clear();
  for (int channel = 0; channel < laneChannels; ++channel)
    lane.copyFrom(channel, 0, input, 0, 0, inFrames);

  BlockTransport transport;
  transport.playing = true;
  for (int offset = 0; offset < total; offset += block) {
    const int count = juce::jmin(block, total - offset);
    juce::AudioBuffer<float> slice(lane.getArrayOfWritePointers(), laneChannels, offset, count);
    transport.samplePosition = offset;
    transport.timeSeconds = offset / kRate;
    if (instance.process(slice, transport) != BlockProcessResult::Processed)
      throw PluginError("the plugin failed or was not ready while processing");
  }
  settle(10);
  if (instance.latencyChangePending())
    throw PluginError("the plugin changed its latency while it was rendering; the take is not reliable");

  Rendered rendered;
  rendered.latencySamples = latency;
  rendered.laneChannels = laneChannels;
  rendered.audio.setSize(laneChannels, outFrames);
  for (int channel = 0; channel < laneChannels; ++channel)
    rendered.audio.copyFrom(channel, 0, lane, channel, latency, outFrames);
  return rendered;
}

juce::File repositoryOf(const juce::File& file) {
  juce::File folder = absoluteFile(file);
  while (true) {
    if (folder.getChildFile(".git").exists() &&
        folder.getChildFile("plugin/src/EffectMatch.cpp").existsAsFile())
      return folder;
    const juce::File parent = folder.getParentDirectory();
    if (parent == folder)
      return {};
    folder = parent;
  }
}

// ####################
// COMMANDS
// ####################
namespace {

juce::var pluginVar(const Loaded& loaded) {
  auto* object = new juce::DynamicObject();
  object->setProperty("name", loaded.description.name);
  object->setProperty("vendor", loaded.description.manufacturerName);
  object->setProperty("version", loaded.description.version);
  object->setProperty("format", loaded.description.pluginFormatName);
  object->setProperty("file", loaded.description.fileOrIdentifier);
  return juce::var(object);
}

juce::var paramVar(const ParamInfo& info) {
  auto* object = new juce::DynamicObject();
  object->setProperty("index", info.index);
  object->setProperty("name", info.name);
  object->setProperty("id", info.id);
  if (info.label.isNotEmpty())
    object->setProperty("label", info.label);
  object->setProperty("value", static_cast<double>(info.value));
  object->setProperty("text", info.text);
  object->setProperty("steps", info.steps);
  object->setProperty("continuous", info.steps == 0);
  return juce::var(object);
}

juce::var appliedVar(const AppliedSetting& applied) {
  auto* object = new juce::DynamicObject();
  object->setProperty("index", applied.index);
  object->setProperty("name", applied.name);
  auto* request = new juce::DynamicObject();
  if (applied.request.target == Setting::Target::index)
    request->setProperty("index", applied.request.index);
  else
    request->setProperty("name", applied.request.name);
  if (applied.request.kind == Setting::Kind::text)
    request->setProperty("text", applied.request.text);
  else
    request->setProperty("value", applied.request.value);
  object->setProperty("requested", juce::var(request));
  object->setProperty("value", static_cast<double>(applied.value));
  object->setProperty("text", applied.text);
  return juce::var(object);
}

juce::String pretty(const juce::var& json) { return juce::JSON::toString(json, false, 4) + "\n"; }

void writeText(const juce::File& file, const juce::String& text) {
  if (!file.getParentDirectory().createDirectory() || !file.replaceWithText(text))
    throw PluginError("cannot write " + file.getFullPathName().toStdString());
}

void writeWav(const juce::File& file, const juce::AudioBuffer<float>& audio) {
  if (!file.getParentDirectory().createDirectory())
    throw PluginError("cannot create " + file.getParentDirectory().getFullPathName().toStdString());
  file.deleteFile();
  std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
  if (stream == nullptr)
    throw PluginError("cannot write " + file.getFullPathName().toStdString());
  juce::WavAudioFormat wav;
  auto writer = wav.createWriterFor(
      stream, juce::AudioFormatWriterOptions{}
                  .withSampleRate(kRate)
                  .withNumChannels(audio.getNumChannels())
                  .withBitsPerSample(32)
                  .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
  if (writer == nullptr)
    throw PluginError("cannot write a WAV file");
  if (!writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples()))
    throw PluginError("failed writing " + file.getFullPathName().toStdString());
}

// The first channel of a 48 kHz WAV.
juce::AudioBuffer<float> readSignal(const juce::File& file) {
  if (!file.existsAsFile())
    throw PluginError("input file not found: " + file.getFullPathName().toStdString());
  juce::WavAudioFormat wav;
  std::unique_ptr<juce::AudioFormatReader> reader(
      wav.createReaderFor(file.createInputStream().release(), true));
  if (reader == nullptr || reader->numChannels < 1 || reader->lengthInSamples < 1 ||
      reader->lengthInSamples > (1 << 28))
    throw PluginError("not a usable WAV file: " + file.getFullPathName().toStdString());
  if (std::abs(reader->sampleRate - kRate) > 0.5)
    throw PluginError("the input signal must be 48000 Hz (this one is " +
                      std::to_string(reader->sampleRate) + " Hz)");
  juce::AudioBuffer<float> all(static_cast<int>(juce::jmin<unsigned int>(reader->numChannels, 2u)),
                              static_cast<int>(reader->lengthInSamples));
  reader->read(&all, 0, all.getNumSamples(), 0, true, true);
  juce::AudioBuffer<float> mono(1, all.getNumSamples());
  mono.copyFrom(0, 0, all, 0, 0, all.getNumSamples());
  return mono;
}

// Lets the plugin go on the message thread before the process ends.
void release(Loaded& loaded) {
  loaded.instance.reset();
  settle(100);
}

}  // namespace

int runParams(const Options& options) {
  if (options.plugin.isEmpty())
    throw PluginError("plugin-params needs --plugin <file>");
  Loaded loaded = load(options.plugin, options.state);
  for (const Setting& setting : options.settings)
    apply(*loaded.instance, setting);
  const std::vector<ParamInfo> params = listParameters(*loaded.instance);

  if (options.json) {
    auto* root = new juce::DynamicObject();
    root->setProperty("plugin", pluginVar(loaded));
    juce::Array<juce::var> list;
    for (const ParamInfo& info : params)
      list.add(paramVar(info));
    root->setProperty("parameters", juce::var(list));
    std::cout << pretty(juce::var(root)).toStdString();
  } else {
    std::cout << loaded.description.name << " " << loaded.description.version << " ("
              << loaded.description.pluginFormatName << "), " << params.size() << " parameters, "
              << loaded.instance->inputChannels() << " in / " << loaded.instance->outputChannels()
              << " out, latency " << loaded.instance->latchedLatency() << " samples\n";
    std::cout << "index\tname\ttext\tsteps\tvalue\n";
    for (const ParamInfo& info : params)
      std::cout << info.index << "\t" << info.name << "\t" << info.text
                << (info.label.isNotEmpty() ? " " + info.label : juce::String()) << "\t"
                << (info.steps == 0 ? juce::String("continuous") : juce::String(info.steps))
                << "\t" << juce::String(info.value, 4) << "\n";
  }
  release(loaded);
  return 0;
}

int runRender(const Options& options) {
  if (options.plugin.isEmpty())
    throw PluginError("plugin-render needs --plugin <file>");
  if (options.output == juce::File())
    throw PluginError("plugin-render needs --out <reference.wav>");
  const juce::File out = absoluteFile(options.output);
  if (!out.hasFileExtension("wav"))
    throw PluginError("--out must be a .wav file");
  if (const juce::File repository = repositoryOf(out); repository != juce::File())
    throw PluginError("refusing to write inside the repository (" +
                      repository.getFullPathName().toStdString() +
                      "): what a plugin produces stays in your data folder");

  juce::AudioBuffer<float> signalBuffer;
  juce::String signalName = "generated test signal";
  if (options.input != juce::File()) {
    signalBuffer = readSignal(absoluteFile(options.input));
    signalName = absoluteFile(options.input).getFullPathName();
  } else {
    const EffectMatch::TestSignal signal = EffectMatch::makeTestSignal(kRate);
    signalBuffer.setSize(1, static_cast<int>(signal.samples.size()));
    std::copy(signal.samples.begin(), signal.samples.end(), signalBuffer.getWritePointer(0));
  }

  Loaded loaded = load(options.plugin, options.state);
  std::vector<AppliedSetting> applied;
  for (const Setting& setting : options.settings)
    applied.push_back(apply(*loaded.instance, setting));

  const int tailFrames = static_cast<int>(std::llround(options.tailSeconds * kRate));
  const Rendered rendered = render(*loaded.instance, signalBuffer, tailFrames, options.block,
                                   options.laneChannels);
  writeWav(out, rendered.audio);

  auto* root = new juce::DynamicObject();
  root->setProperty("format", "pedaleira-plugin-reference");
  root->setProperty("version", 1);
  root->setProperty("plugin", pluginVar(loaded));
  if (loaded.stateKind.isNotEmpty()) {
    auto* state = new juce::DynamicObject();
    state->setProperty("file", absoluteFile(options.state).getFullPathName());
    state->setProperty("kind", loaded.stateKind);
    root->setProperty("state", juce::var(state));
  }
  juce::Array<juce::var> list;
  for (const AppliedSetting& item : applied)
    list.add(appliedVar(item));
  root->setProperty("parameters", juce::var(list));
  root->setProperty("sampleRate", kRate);
  root->setProperty("block", options.block);
  root->setProperty("inputChannels", loaded.instance->inputChannels());
  root->setProperty("outputChannels", loaded.instance->outputChannels());
  root->setProperty("channels", rendered.laneChannels);
  root->setProperty("latencySamples", rendered.latencySamples);
  root->setProperty("latencyCompensated", true);
  root->setProperty("tailSeconds", options.tailSeconds);
  root->setProperty("signal", signalName);
  root->setProperty("frames", rendered.audio.getNumSamples());
  writeText(out.withFileExtension("json"), pretty(juce::var(root)));

  std::cerr << "wrote " << out.getFullPathName() << " (" << rendered.laneChannels
            << " channel(s), " << rendered.audio.getNumSamples() << " frames, latency "
            << rendered.latencySamples << " samples compensated)\n";
  release(loaded);
  return 0;
}

}  // namespace MatchPlugin

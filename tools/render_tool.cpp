// PedaleiraRender: runs a rig over a WAV through the real processor and writes
// the result, with no window and no audio device. The rig comes from a
// preset file (a .t3kpreset, or a JSON preset of the setlist library, see
// docs/library.md), from a script of engine commands (docs/commands.md), or
// from a preset edited by a script. It is the safety net for engine refactors
// (render before and after, compare the files), the same driving sequence the
// golden render tests in test/ use, and how an agent hears what it built.
//
//   PedaleiraRender [--preset <file.t3kpreset|file.json>] [--run <commands.json>]
//                 --in <input.wav> --out <output.wav>
//                 [--rate <hz>] [--block <samples>] [--tail <seconds>]
//                 [--oversample <1|2|4|8>] [--out-channels <1..8>] [--info]
//                 [--state-out <state.json>] [--plugin-path <folder>]...
//                 [--library <folder>]
//   PedaleiraRender --describe | --describe-markdown
//
// A JSON preset's model files are looked for in an Assets folder beside it,
// in the Assets folder of the library it sits in, and in the library the
// tool runs with. That library is an empty scratch folder, gone when the tool
// exits, so a script's setlist.*, preset.* and library.* commands never reach
// the user's own library in Documents; --library points the tool at a library
// root instead (it is then read and written like the app would).
//
// --run takes a JSON array of {"command": "...", "args": {...}} applied in
// order after the preset (if any) has loaded; the first failing command stops
// the run with its error. --state-out writes the state.get result of the rig
// that was rendered. --plugin-path (repeatable) scans a folder for VST3
// plugins before the preset loads and the script runs, so chain.addPlugin can
// name one by its file path or its name; the scan uses a scratch cache and
// looks nowhere else, so the user's own plugin cache and plugin folders are
// neither read nor written (without the option no plugin is known, and a rig
// that hosts one renders it as a bypassed placeholder). --describe prints
// every command as JSON and
// --describe-markdown the reference checked in as docs/commands.md; both exit
// without rendering.
//
// The input file may have 1 to 8 channels, and the processor's main bus gets
// that many inputs: mono input renders through a mono layout, stereo through
// a stereo one, and a wider file reaches the processor as that many device
// channels. --out-channels sets the number of outputs independently (default:
// as many as the input has). The output is a 32-bit float WAV at the render
// rate with that many channels, trimmed to the input length plus --tail, with
// the processor's reported latency compensated. Two runs with the same
// arguments produce the same bytes. Diagnostics go to stderr; --info prints one JSON object to
// stdout. Exit code 0 on success, 1 with a one-line error otherwise.
//
// Build: cmake --build build --target PedaleiraRender
#include "EngineCommands.h"
#include "LibraryPaths.h"
#include "PresetManager.h"
#include "Processor.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int kLoadTimeoutMs = 30000;
// Fixed Spread/Align wobble seed (see setDeterministicRandomForOfflineRender).
constexpr std::int64_t kWobbleSeed = 0x47524944;

struct RenderError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// A bad command line, as opposed to a render that failed.
struct UsageError : RenderError {
  using RenderError::RenderError;
};

// The processor attaches its own file logger when none is set (Processor.cpp
// constructor); installing this first keeps renders from writing a log file
// into the user's app-data folder. The processor only ever unsets the logger,
// never deletes it, so a stack instance is safe.
struct QuietLogger : juce::Logger {
  void logMessage(const juce::String&) override {}
};

// Scratch preset store, removed on exit however the run ends.
struct ScopedTempDir {
  ScopedTempDir()
      : dir(juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("PedaleiraRender-" + juce::Uuid().toDashedString())) {
    if (!dir.createDirectory())
      throw RenderError("cannot create a temporary directory in " +
                        dir.getParentDirectory().getFullPathName().toStdString());
  }
  ~ScopedTempDir() { dir.deleteRecursively(); }
  ScopedTempDir(const ScopedTempDir&) = delete;
  ScopedTempDir& operator=(const ScopedTempDir&) = delete;

  juce::File dir;
};

struct Options {
  juce::File preset, input, output, runScript, stateOut, library;
  juce::StringArray pluginPaths;
  double rate = 0.0;  // 0 = the input file's rate
  int block = 128;
  double tailSeconds = 0.0;
  int oversample = 0;  // 0 = leave the processor's own setting
  int outChannels = 0;  // 0 = as many as the input file has
  bool info = false;
  bool describe = false, describeMarkdown = false;
};

constexpr const char* kUsage =
    "usage: PedaleiraRender [--preset <file.t3kpreset|file.json>] [--run <commands.json>]\n"
    "                     --in <input.wav> --out <output.wav>\n"
    "                     [--rate <hz, default: input file rate>] [--block <samples, default 128>]\n"
    "                     [--tail <seconds, default 0>] [--oversample <1|2|4|8>]\n"
    "                     [--out-channels <1..8, default: the input's channel count>] [--info]\n"
    "                     [--state-out <state.json>] [--plugin-path <folder>]...\n"
    "                     [--library <folder, default: an empty scratch library>]\n"
    "       PedaleiraRender --describe | --describe-markdown\n"
    "  --run       JSON array of {\"command\": ..., \"args\": {...}}, applied after the preset\n"
    "  --state-out write the rendered rig's state (the state.get command) as JSON\n"
    "  --plugin-path scan this folder for VST3 plugins first (repeatable; scratch cache)\n"
    "  --library   the setlist library root the rig's library commands work on\n"
    "  --describe  print every engine command as JSON (--describe-markdown: as a reference)";

Options parseOptions(int argc, char** argv) {
  Options opts;
  auto value = [&](int& i) -> juce::String {
    if (i + 1 >= argc)
      throw UsageError(std::string("missing value for ") + argv[i]);
    return juce::String::fromUTF8(argv[++i]);
  };
  auto number = [&](int& i) {
    const std::string flag = argv[i];
    const juce::String text = value(i);
    if (!text.containsOnly("0123456789.") || text.isEmpty())
      throw UsageError("bad value for " + flag + ": " + text.toStdString());
    return text.getDoubleValue();
  };
  auto absolute = [](const juce::String& path) {
    return juce::File::getCurrentWorkingDirectory().getChildFile(path);
  };

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--preset") opts.preset = absolute(value(i));
    else if (arg == "--in") opts.input = absolute(value(i));
    else if (arg == "--out") opts.output = absolute(value(i));
    else if (arg == "--rate") opts.rate = number(i);
    else if (arg == "--block") opts.block = static_cast<int>(number(i));
    else if (arg == "--tail") opts.tailSeconds = number(i);
    else if (arg == "--oversample") opts.oversample = static_cast<int>(number(i));
    else if (arg == "--out-channels") {
      opts.outChannels = static_cast<int>(number(i));
      if (opts.outChannels < 1 || opts.outChannels > kMaxIoChannels)
        throw UsageError("--out-channels must be between 1 and 8");
    }
    else if (arg == "--info") opts.info = true;
    else if (arg == "--run") opts.runScript = absolute(value(i));
    else if (arg == "--state-out") opts.stateOut = absolute(value(i));
    else if (arg == "--plugin-path") opts.pluginPaths.add(absolute(value(i)).getFullPathName());
    else if (arg == "--library") opts.library = absolute(value(i));
    else if (arg == "--describe") opts.describe = true;
    else if (arg == "--describe-markdown") opts.describeMarkdown = true;
    else throw UsageError("unknown argument: " + arg);
  }

  // Describing the commands needs no rig and no audio.
  if (opts.describe || opts.describeMarkdown)
    return opts;

  if (opts.input == juce::File())
    throw UsageError("--in is required");
  if (opts.preset == juce::File() && opts.runScript == juce::File())
    throw UsageError("--preset or --run is required (or both)");
  if (opts.output == juce::File() && !opts.info && opts.stateOut == juce::File())
    throw UsageError("--out is required (or --info / --state-out to inspect only)");
  if (opts.block < 1 || opts.block > 65536)
    throw UsageError("--block must be between 1 and 65536");
  if (opts.oversample != 0 && opts.oversample != 1 && opts.oversample != 2 &&
      opts.oversample != 4 && opts.oversample != 8)
    throw UsageError("--oversample must be 1, 2, 4 or 8");
  if (opts.rate != 0.0 && (opts.rate < 8000.0 || opts.rate > 192000.0))
    throw UsageError("--rate must be between 8000 and 192000");
  return opts;
}

struct Audio {
  juce::AudioBuffer<float> samples;
  double rate = 0.0;
};

Audio readWav(const juce::File& file) {
  if (!file.existsAsFile())
    throw RenderError("input file not found: " + file.getFullPathName().toStdString());

  juce::WavAudioFormat wav;
  std::unique_ptr<juce::AudioFormatReader> reader(
      wav.createReaderFor(file.createInputStream().release(), true));
  if (reader == nullptr)
    throw RenderError("not a readable WAV file: " + file.getFullPathName().toStdString());
  if (reader->numChannels < 1 || reader->numChannels > static_cast<unsigned int>(kMaxIoChannels))
    throw RenderError("unsupported layout: " + std::to_string(reader->numChannels) +
                      " input channels (1 to 8 only)");
  if (reader->lengthInSamples < 1 || reader->lengthInSamples > (1 << 28))
    throw RenderError("input file is empty or too long");

  Audio audio;
  audio.rate = reader->sampleRate;
  audio.samples.setSize(static_cast<int>(reader->numChannels),
                        static_cast<int>(reader->lengthInSamples));
  reader->read(&audio.samples, 0, audio.samples.getNumSamples(), 0, true, true);
  return audio;
}

// Only used when --rate differs from the file's rate. The ratio is exact and
// the interpolator is reset per channel, so the result is deterministic.
juce::AudioBuffer<float> resample(const juce::AudioBuffer<float>& in, double fromRate,
                                  double toRate) {
  const int outFrames = static_cast<int>(std::llround(in.getNumSamples() * toRate / fromRate));
  juce::AudioBuffer<float> out(in.getNumChannels(), outFrames);
  for (int ch = 0; ch < in.getNumChannels(); ++ch) {
    juce::LagrangeInterpolator interpolator;
    interpolator.process(fromRate / toRate, in.getReadPointer(ch), out.getWritePointer(ch),
                         outFrames);
  }
  return out;
}

void writeWav(const juce::File& file, const juce::AudioBuffer<float>& audio, double rate) {
  // FileOutputStream appends to an existing file; a shorter render would leave
  // the old tail behind.
  file.deleteFile();
  std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
  if (stream == nullptr)
    throw RenderError("cannot write output file: " + file.getFullPathName().toStdString());

  juce::WavAudioFormat wav;
  auto writer = wav.createWriterFor(
      stream, juce::AudioFormatWriterOptions{}
                  .withSampleRate(rate)
                  .withNumChannels(audio.getNumChannels())
                  .withBitsPerSample(32)
                  .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
  if (writer == nullptr)
    throw RenderError("cannot write a WAV at " + std::to_string(rate) + " Hz");
  if (!writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples()))
    throw RenderError("failed writing output file: " + file.getFullPathName().toStdString());
}

// Blocks until every tone block has loaded and the chain-edit fade has
// released, pumping the message loop the processor's deferred work runs on.
// A preset restore holds the chain muted on a wall-clock waiter, so rendering
// before it lets go would capture silence or the dry input.
void waitForChainLoaded(TONE3000Processor& proc) {
  const auto deadline = juce::Time::getMillisecondCounter() + kLoadTimeoutMs;
  while (juce::Time::getMillisecondCounter() < deadline) {
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    // Parameter edits defer the tail and latency they change to a timer
    // this short pump may not reach; apply them before anything is rendered
    // or the latency is read.
    proc.flushDeferredBlockRefresh();

    const juce::var state = proc.getChainState(-1);
    bool allLoaded = true;
    // The two lanes of a legacy rig, or every row of a grid.
    std::vector<const juce::Array<juce::var>*> lanes{state["chain"].getArray(),
                                                     state["chainRight"].getArray()};
    if (const auto* rows = state["grid"]["rows"].getArray()) {
      lanes.clear();
      for (const auto& row : *rows)
        lanes.push_back(row["blocks"].getArray());
    }
    for (const auto* lane : lanes) {
      if (lane == nullptr)
        continue;
      for (const auto& item : *lane) {
        if (item["kind"].toString() != "tone")
          continue;
        if (static_cast<bool>(item["loadFailed"]))
          throw RenderError("a block failed to load: " + item["tone"]["title"].toString().toStdString());
        if (static_cast<bool>(item["processingFailed"]))
          throw RenderError("a block failed while processing: " +
                            item["tone"]["title"].toString().toStdString());
        // A placeholder for a kind this build lacks never loads; it renders
        // as a bypass.
        if (!static_cast<bool>(item["loaded"]) && !static_cast<bool>(item["missing"]))
          allLoaded = false;
      }
    }
    if (allLoaded && !proc.isChainEditFadeHeld())
      return;
  }
  throw RenderError("timed out after 30 s waiting for the rig to load");
}

// Scans the --plugin-path folders (each plugin in the helper process, see
// PluginScanner.h) and reports what was found on stderr.
void scanPlugins(const juce::StringArray& folders) {
  for (const auto& folder : folders)
    if (!juce::File(folder).isDirectory())
      throw RenderError("plugin folder not found: " + folder.toStdString());

  PluginScanner& scanner = PluginScanner::instance();
  PluginScanner::ScanOptions options;
  options.folders = folders;
  if (!scanner.startScan(options))
    throw RenderError("the plugin scan helper is missing (expected at " +
                      scanner.helperFile().getFullPathName().toStdString() + ")");
  if (!scanner.waitForScan(10 * 60 * 1000)) {
    scanner.cancelScan();
    throw RenderError("timed out scanning the plugin folders");
  }
  for (const auto& listed : scanner.list())
    std::cerr << "plugin " << listed.description.name << " ("
              << listed.description.pluginFormatName << "): "
              << PluginScanner::statusToString(listed.status)
              << (listed.reason.isNotEmpty() ? " (" + listed.reason.toStdString() + ")" : "")
              << "\n";
}

// Applies a --run script: a JSON array of {"command", "args"} objects, in
// order, through the same registry every other client of the engine uses.
void runScript(CommandRegistry& registry, const juce::File& file) {
  if (!file.existsAsFile())
    throw RenderError("command file not found: " + file.getFullPathName().toStdString());

  juce::var script;
  const juce::Result parsed = juce::JSON::parse(file.loadFileAsString(), script);
  if (parsed.failed())
    throw RenderError("command file is not valid JSON: " + parsed.getErrorMessage().toStdString());
  const auto* steps = script.getArray();
  if (steps == nullptr)
    throw RenderError("command file must be a JSON array of {\"command\", \"args\"} objects");

  for (int i = 0; i < steps->size(); ++i) {
    const juce::var& step = steps->getReference(i);
    const std::string where = "command " + std::to_string(i + 1);
    if (step.getDynamicObject() == nullptr || !step["command"].isString())
      throw RenderError(where + " must be an object with a \"command\" name");

    const juce::String name = step["command"].toString();
    const CommandResult result = registry.run(name, step["args"]);
    if (!result.ok)
      throw RenderError(where + " (" + name.toStdString() + ") failed: " +
                        result.errorCode.toStdString() + ": " + result.errorMessage.toStdString());
    std::cerr << "ran " << name << "\n";
  }
}

void writeText(const juce::File& file, const juce::String& text) {
  // replaceWithText converts line ends per platform unless told not to; the
  // output should be the same bytes everywhere.
  if (!file.replaceWithText(text, false, false, "\n"))
    throw RenderError("cannot write file: " + file.getFullPathName().toStdString());
}

// osFactor is a choice index: 0 = 2x, 1 = 4x, 2 = 8x.
void setOversampling(TONE3000Processor& proc, int factor) {
  auto* enabled = proc.parameters.getParameter("osEnabled");
  auto* choice = proc.parameters.getParameter("osFactor");
  enabled->setValueNotifyingHost(factor > 1 ? 1.0f : 0.0f);
  if (factor > 1)
    choice->setValueNotifyingHost(choice->convertTo0to1(static_cast<float>(factor == 2 ? 0 : factor == 4 ? 1 : 2)));
}

int oversamplingOf(TONE3000Processor& proc) {
  if (proc.parameters.getRawParameterValue("osEnabled")->load() < 0.5f)
    return 1;
  const int index = static_cast<int>(std::lround(proc.parameters.getRawParameterValue("osFactor")->load()));
  return 1 << (juce::jlimit(0, 2, index) + 1);
}

juce::var describeBlocks(TONE3000Processor& proc) {
  juce::Array<juce::var> blocks;
  const juce::var state = proc.getChainState(-1);
  // A legacy rig lists its two lanes; a grid its four rows.
  std::vector<std::pair<const juce::Array<juce::var>*, juce::String>> lanes{
      {state["chain"].getArray(), "left"}, {state["chainRight"].getArray(), "right"}};
  if (const auto* rows = state["grid"]["rows"].getArray()) {
    lanes.clear();
    for (int row = 0; row < rows->size(); ++row)
      lanes.emplace_back(rows->getReference(row)["blocks"].getArray(), "row " + juce::String(row));
  }
  for (const auto& [items, lane] : lanes) {
    if (items == nullptr)
      continue;
    for (const auto& item : *items) {
      if (item["kind"].toString() != "tone")
        continue;  // empty insert slots are layout, not blocks
      juce::DynamicObject::Ptr block = new juce::DynamicObject();
      block->setProperty("lane", lane);
      block->setProperty("type", item["tone"]["format"]);
      block->setProperty("name", item["tone"]["title"]);
      block->setProperty("enabled", item["params"]["enabled"]);
      blocks.add(juce::var(block.get()));
    }
  }
  return blocks;
}

int run(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h") {
      std::cerr << kUsage << "\n";
      return 0;
    }
  const Options opts = parseOptions(argc, argv);

  if (opts.describe || opts.describeMarkdown) {
    // Same isolation as a render: no log file, no machine settings.
    QuietLogger quietLogger;
    juce::Logger::setCurrentLogger(&quietLogger);
    juce::ScopedJuceInitialiser_GUI juceInit;
    TONE3000Processor::disableMachineDefaultParametersForTesting();
    // The user's app data folder is neither read nor made (the processor looks
    // at it as it starts).
    ScopedTempDir appData;
    LibraryPaths::setAppDataDirForTesting(appData.dir);
    ScopedTempDir settings;
    TONE3000Processor::setFootswitchSettingsFileForTesting(
        settings.dir.getChildFile("footswitch.settings"));
    TONE3000Processor::setLibraryRootForTesting(settings.dir.getChildFile("Library"));
    TONE3000Processor proc;
    EngineCommands commands(proc);
    if (opts.describeMarkdown)
      std::cout << commands.markdownReference().toStdString();
    else
      std::cout << juce::JSON::toString(commands.registry().describe()).toStdString() << "\n";
    return 0;
  }

  const bool hasPreset = opts.preset != juce::File();
  if (hasPreset && !opts.preset.existsAsFile())
    throw RenderError("preset file not found: " + opts.preset.getFullPathName().toStdString());
  Audio input = readWav(opts.input);

  const double rate = opts.rate > 0.0 ? opts.rate : input.rate;
  if (std::abs(rate - input.rate) > 0.01) {
    input.samples = resample(input.samples, input.rate, rate);
    std::cerr << "resampled input " << input.rate << " Hz -> " << rate << " Hz\n";
  }
  const int channels = input.samples.getNumChannels();
  const int outChannels = opts.outChannels > 0 ? opts.outChannels : channels;
  // processBlock gets one buffer for both directions, as wide as the wider.
  const int bufferChannels = std::max(channels, outChannels);
  const int inFrames = input.samples.getNumSamples();
  const int tailFrames = static_cast<int>(std::llround(opts.tailSeconds * rate));

  // No log file, no machine settings, one thread: the render must not depend
  // on, or leave anything in, the user's setup.
  QuietLogger quietLogger;
  juce::Logger::setCurrentLogger(&quietLogger);
  juce::ScopedJuceInitialiser_GUI juceInit;
  TONE3000Processor::disableMachineDefaultParametersForTesting();

  // The user's app data folder (settings, presets, model stash, plugin cache,
  // the agent layer's files) is a scratch one too, so the processor's start-up
  // neither reads nor makes it. What has a hook of its own below is put
  // somewhere more specific.
  ScopedTempDir appData;
  LibraryPaths::setAppDataDirForTesting(appData.dir);

  // The preset goes through the real preset load (which also sets the
  // faceplate parameters, unlike a bare chain-snapshot restore), straight
  // from its file. The stores are scratch folders all the same: the old
  // .t3kpreset store, and the setlist library unless --library names one, so
  // a script's preset, setlist and library commands never reach the user's
  // own folders. What a script deletes from the scratch library goes to a
  // scratch folder rather than the user's trash; from a library named with
  // --library it goes to the real trash, where it can be got back.
  // The footswitch mode is a machine-wide setting in the user's settings
  // file: a render starts from the default mode and a script's
  // footswitch.setMode stays in this scratch file.
  ScopedTempDir settings;
  TONE3000Processor::setFootswitchSettingsFileForTesting(
      settings.dir.getChildFile("footswitch.settings"));

  ScopedTempDir store;
  ScopedTempDir trash;
  if (opts.library == juce::File())
    SetlistLibrary::setTrashDirForTesting(trash.dir);
  TONE3000Processor::setLibraryRootForTesting(store.dir.getChildFile("Library"));
  if (opts.library != juce::File() && !opts.library.isDirectory() &&
      !opts.library.createDirectory())
    throw RenderError("cannot create the library folder: " +
                      opts.library.getFullPathName().toStdString());

  // Drop-loaded files (chain.addToneFromFiles in a script) are stashed under
  // the app-data folder by default; a scratch folder keeps them out of the
  // user's. Set before the processor exists, since its constructor starts the
  // stash GC. The wobble seed is fixed so a stereo rig with Spread/Align
  // wobble renders identically every run.
  ScopedTempDir localModels;
  TONE3000Processor::setLocalModelsDirForTesting(localModels.dir);
  TONE3000Processor::setDeterministicRandomForOfflineRender(kWobbleSeed);

  // Plugins: a scratch cache and no default locations, so a render never
  // reads or writes the user's plugin cache nor scans their plugin folders.
  // Only what --plugin-path names is known to this run.
  ScopedTempDir pluginCache;
  PluginScanner::setCacheDirForTesting(pluginCache.dir);
  PluginScanner::setDefaultLocationsEnabled(false);
  if (!opts.pluginPaths.isEmpty())
    scanPlugins(opts.pluginPaths);

  TONE3000Processor proc;
  proc.setMultiCoreEnabled(false, /*persist=*/false);
  proc.setPresetStoreForTesting(store.dir, opts.library);

  proc.setPlayConfigDetails(channels, outChannels, rate, opts.block);
  if (proc.getTotalNumInputChannels() != channels ||
      proc.getTotalNumOutputChannels() != outChannels)
    throw RenderError("the processor does not take " + std::to_string(channels) + " inputs and " +
                      std::to_string(outChannels) + " outputs");
  if (opts.oversample != 0)
    setOversampling(proc, opts.oversample);
  proc.prepareToPlay(rate, opts.block);

  if (hasPreset) {
    if (!proc.loadPresetFile(opts.preset))
      throw RenderError("not a valid preset file, or one without a chain: " +
                        opts.preset.getFullPathName().toStdString());
    waitForChainLoaded(proc);
  }

  EngineCommands commands(proc);
  if (opts.runScript != juce::File()) {
    runScript(commands.registry(), opts.runScript);
    waitForChainLoaded(proc);
  }

  // Prepared again with the rig in place, like a host starting transport
  // after a session load: the post-chain image matrix and the chain-edit fade
  // prime to the loaded rig instead of ramping in from the empty one.
  proc.prepareToPlay(rate, opts.block);
  waitForChainLoaded(proc);

  if (opts.stateOut != juce::File())
    writeText(opts.stateOut,
              juce::JSON::toString(commands.registry().run("state.get").value) + "\n");

  const int latency = proc.getLatencySamples();
  const juce::String presetName = proc.getChainState(-1)["preset"]["name"].toString();

  int renderedFrames = 0;
  if (opts.output != juce::File()) {
    // Render `latency` extra frames and drop that many leading ones, so the
    // file lines up sample for sample with the input.
    renderedFrames = inFrames + tailFrames;
    const int64_t total = static_cast<int64_t>(renderedFrames) + latency;
    const int numBlocks = static_cast<int>((total + opts.block - 1) / opts.block);

    juce::AudioBuffer<float> rendered(outChannels, renderedFrames);
    juce::AudioBuffer<float> block(bufferChannels, opts.block);
    juce::MidiBuffer midi;
    for (int b = 0; b < numBlocks; ++b) {
      const int start = b * opts.block;
      block.clear();
      const int fromInput = juce::jlimit(0, opts.block, inFrames - start);
      for (int ch = 0; ch < channels && fromInput > 0; ++ch)
        block.copyFrom(ch, 0, input.samples, ch, start, fromInput);

      proc.processBlock(block, midi);

      // Output frame n of the file is processed frame n + latency.
      const int dest = std::max(0, start - latency);
      const int skip = dest + latency - start;
      const int count = std::min(opts.block - skip, renderedFrames - dest);
      for (int ch = 0; ch < outChannels && count > 0; ++ch)
        rendered.copyFrom(ch, dest, block, ch, skip, count);
    }
    writeWav(opts.output, rendered, rate);
  }

  if (opts.info) {
    juce::DynamicObject::Ptr info = new juce::DynamicObject();
    info->setProperty("preset", presetName);
    info->setProperty("blocks", describeBlocks(proc));
    info->setProperty("sampleRate", rate);
    info->setProperty("blockSize", opts.block);
    info->setProperty("channels", channels);
    info->setProperty("outputChannels", outChannels);
    info->setProperty("oversample", oversamplingOf(proc));
    info->setProperty("latencySamples", latency);
    info->setProperty("renderedFrames", renderedFrames);
    std::cout << juce::JSON::toString(juce::var(info.get()), true).toStdString() << "\n";
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const UsageError& e) {
    std::cerr << "PedaleiraRender: " << e.what() << " (see --help)\n";
    return 1;
  } catch (const RenderError& e) {
    std::cerr << "PedaleiraRender: " << e.what() << "\n";
    return 1;
  }
}

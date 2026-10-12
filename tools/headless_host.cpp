// PedaleiraHeadless: the engine with its command server and nothing else. No
// window, no audio device: a processor that sits there and is driven through
// commands. It is what the command server's and the MCP server's tests run
// against, and what lets an agent build and measure tones on a machine where
// the application is not open.
//
//   PedaleiraHeadless [--port <n>] [--data <folder>] [--library <folder>]
//                   [--preset <file.t3kpreset>] [--allow-root <folder>]...
//                   [--allow <class>]... [--rate <hz>] [--block <samples>]
//                   [--no-stdin]
//
// --port        the port to listen on (default 0: any free port)
// --data        where settings, presets, the model stash and the connection
//               file live (default: a scratch folder removed on exit, so the
//               user's real settings, presets and library are never touched)
// --library     the library root, where guitar profiles live (default: a
//               folder inside --data)
// --preset      a preset to load into the rig at start
// --allow-root  a folder clients may read files from, on top of the library
//               (repeatable; not saved)
// --allow       switch a permission class on for clients: live, delete or
//               settings (repeatable; read, edit and render are on already)
// --rate/--block the sample rate and block size the rig is prepared with
//               (default 48000 / 128)
//
// On start it prints one line of JSON to stdout:
//   {"ready":true,"connectionFile":"...","port":N,"data":"...","library":"..."}
// Clients read the port and the token from the connection file.
//
// Standard input is the local side, standing in for the user at the
// application: each line is a command as JSON ({"command":"...","args":{..}}),
// run without the permission gate, and answered with one line on stdout.
// That is how a test approves a waiting request (approvals.resolve) or
// changes the permissions (permissions.set). The line `exit`, the command
// host.exit, end of input, SIGINT and SIGTERM all end the process cleanly
// (so a host whose parent has gone does not linger). --no-stdin leaves
// standard input alone, for running it detached from a terminal.
//
// Build: cmake --build build --target PedaleiraHeadless
#include "AgentHub.h"
#include "CommandServer.h"
#include "EngineCommands.h"
#include "LibraryPaths.h"
#include "PluginScanner.h"
#include "PresetManager.h"
#include "Processor.h"

#include <atomic>
#include <csignal>
#include <deque>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

std::atomic<bool> quitRequested{false};

void onSignal(int) { quitRequested.store(true); }

struct HostError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// See render_tool.cpp: keeps the processor from attaching a file logger in
// the user's app-data folder.
struct QuietLogger : juce::Logger {
  void logMessage(const juce::String&) override {}
};

struct Options {
  int port = 0;
  juce::File data, library, preset;
  juce::StringArray allowRoots, allow;
  double rate = 48000.0;
  int block = 128;
  bool readStdin = true;
};

constexpr const char* kUsage =
    "usage: PedaleiraHeadless [--port <n>] [--data <folder>] [--library <folder>]\n"
    "                       [--preset <file.t3kpreset>] [--allow-root <folder>]...\n"
    "                       [--allow <live|delete|settings>]... [--rate <hz>] [--block <samples>]\n"
    "                       [--no-stdin]\n"
    "  Runs the engine with its command server and no window or audio device.\n"
    "  Prints {\"ready\":true,\"connectionFile\":...} on stdout; commands typed on stdin run\n"
    "  locally; `exit`, end of input, SIGINT or SIGTERM stop it.";

Options parseOptions(int argc, char** argv) {
  Options opts;
  auto value = [&](int& i) -> juce::String {
    if (i + 1 >= argc)
      throw HostError(std::string("missing value for ") + argv[i]);
    return juce::String::fromUTF8(argv[++i]);
  };
  auto absolute = [](const juce::String& path) {
    return juce::File::getCurrentWorkingDirectory().getChildFile(path);
  };
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--port") opts.port = value(i).getIntValue();
    else if (arg == "--data") opts.data = absolute(value(i));
    else if (arg == "--library") opts.library = absolute(value(i));
    else if (arg == "--preset") opts.preset = absolute(value(i));
    else if (arg == "--allow-root") opts.allowRoots.add(absolute(value(i)).getFullPathName());
    else if (arg == "--allow") opts.allow.add(value(i));
    else if (arg == "--rate") opts.rate = value(i).getDoubleValue();
    else if (arg == "--block") opts.block = value(i).getIntValue();
    else if (arg == "--no-stdin") opts.readStdin = false;
    else throw HostError("unknown argument: " + arg);
  }
  if (opts.port < 0 || opts.port > 65535)
    throw HostError("--port must be between 0 and 65535");
  if (opts.rate < 8000.0 || opts.rate > 192000.0)
    throw HostError("--rate must be between 8000 and 192000");
  if (opts.block < 1 || opts.block > 8192)
    throw HostError("--block must be between 1 and 8192");
  for (const auto& name : opts.allow)
    if (name != "live" && name != "delete" && name != "settings")
      throw HostError("--allow takes live, delete or settings");
  return opts;
}

// Lines typed on stdin, read on a thread of their own (reading blocks) and
// handed to the message loop.
struct StdinLines {
  StdinLines() : reader([this] { run(); }) { reader.detach(); }

  bool next(std::string& line) {
    const std::lock_guard<std::mutex> lock(mutex);
    if (lines.empty())
      return false;
    line = std::move(lines.front());
    lines.pop_front();
    return true;
  }

  // True once standard input has closed and every line has been taken.
  bool drained() {
    const std::lock_guard<std::mutex> lock(mutex);
    return ended.load() && lines.empty();
  }

private:
  std::atomic<bool> ended{false};
  void run() {
    std::string line;
    while (std::getline(std::cin, line)) {
      const std::lock_guard<std::mutex> lock(mutex);
      lines.push_back(line);
    }
    ended.store(true);
  }

  std::mutex mutex;
  std::deque<std::string> lines;
  // Detached: nothing portable interrupts a blocked read of stdin, and the
  // process is about to end whenever this would be joined. It only touches
  // this object, which lives as long as the process (see run()).
  std::thread reader;
};

int run(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h") {
      std::cerr << kUsage << "\n";
      return 0;
    }
  const Options opts = parseOptions(argc, argv);

  QuietLogger quietLogger;
  juce::Logger::setCurrentLogger(&quietLogger);
  juce::ScopedJuceInitialiser_GUI juceInit;

  // Everything the process reads or writes lives under one data folder: a
  // scratch one unless --data names a place to keep.
  const bool scratch = opts.data == juce::File();
  const juce::File data =
      scratch ? juce::File::getSpecialLocation(juce::File::tempDirectory)
                    .getChildFile("PedaleiraHeadless-" + juce::Uuid().toDashedString())
              : opts.data;
  if (!data.createDirectory().wasOk())
    throw HostError("cannot create the data folder " + data.getFullPathName().toStdString());
  // A scratch folder goes however the run ends.
  struct ScratchCleanup {
    ~ScratchCleanup() {
      if (enabled)
        dir.deleteRecursively();
    }
    juce::File dir;
    bool enabled;
  } cleanup{data, scratch};
  const juce::File library = opts.library != juce::File() ? opts.library : data.getChildFile("Library");
  if (!library.createDirectory().wasOk())
    throw HostError("cannot create the library folder " + library.getFullPathName().toStdString());

  TONE3000Processor::disableMachineDefaultParametersForTesting();
  TONE3000Processor::setFootswitchSettingsFileForTesting(data.getChildFile("footswitch.settings"));
  data.getChildFile("LocalModels").createDirectory();
  TONE3000Processor::setLocalModelsDirForTesting(data.getChildFile("LocalModels"));
  // Renders repeat: the wobble noise of Spread and Align starts from the
  // same seed at every prepare (the render tool's value).
  TONE3000Processor::setDeterministicRandomForOfflineRender(0x47524944);
  data.getChildFile("PluginCache").createDirectory();
  PluginScanner::setCacheDirForTesting(data.getChildFile("PluginCache"));
  PluginScanner::setDefaultLocationsEnabled(false);
  AgentSettings::setFileForTesting(data.getChildFile("agents.settings"));
  CommandServer::setConnectionFileForTesting(data.getChildFile("command-server.json"));
  LibraryPaths::setLibraryRootForTesting(library);
  // A scratch run leaves nothing behind, the system trash included: what a
  // client deletes goes to a folder that is removed with the rest.
  if (scratch)
    SetlistLibrary::setTrashDirForTesting(data.getChildFile("Trash"));
  data.getChildFile("Scratch").createDirectory();
  LibraryPaths::setScratchDirForTesting(data.getChildFile("Scratch"));

  {
    TONE3000Processor proc;
    proc.setMultiCoreEnabled(false, /*persist=*/false);
    data.getChildFile("Presets").createDirectory();
    // The old preset store in the data folder; the library (setlists,
    // assets, guitar profiles) all under the one library folder.
    proc.setPresetStoreForTesting(data.getChildFile("Presets"), library);
    proc.setPlayConfigDetails(2, 2, opts.rate, opts.block);
    proc.prepareToPlay(opts.rate, opts.block);

    EngineCommands commands(proc);
    AgentHub& hub = commands.agents();

    // The one command that only this tool has. Local only: a client cannot
    // end the process it is talking to.
    commands.registry().add({
        "host.exit",
        "Ends the headless host.",
        {},
        true,
        CommandPermission::local,
        [](const juce::var&) {
          quitRequested.store(true);
          return CommandResult::success();
        },
    });

    if (opts.preset != juce::File()) {
      if (!opts.preset.existsAsFile())
        throw HostError("preset file not found: " + opts.preset.getFullPathName().toStdString());
      const juce::File copy = data.getChildFile("Presets").getChildFile(
          opts.preset.getFileNameWithoutExtension() + PresetManager::kFileExtension);
      if (copy != opts.preset && !opts.preset.copyFileTo(copy))
        throw HostError("cannot copy the preset into the data folder");
      const juce::var presets = proc.getPresetList()["presets"];
      bool loaded = false;
      for (int i = 0; i < presets.size() && !loaded; ++i)
        if (!static_cast<bool>(presets[i]["factory"]))
          loaded = proc.loadPreset(presets[i]["id"].toString());
      if (!loaded)
        throw HostError("not a loadable preset: " + opts.preset.getFullPathName().toStdString());
    }

    for (const auto& root : opts.allowRoots) {
      if (!juce::File(root).isDirectory())
        throw HostError("--allow-root is not a folder: " + root.toStdString());
      hub.addSessionRoot(juce::File(root));
    }
    AgentPolicy policy = hub.policy();
    policy.live = policy.live || opts.allow.contains("live");
    policy.destroy = policy.destroy || opts.allow.contains("delete");
    policy.settings = policy.settings || opts.allow.contains("settings");
    hub.setPolicy(policy, /*persist=*/false);

    // Started for this run whatever the settings file says: serving is the
    // tool's whole purpose, and the choice is not written back.
    if (const juce::String error = hub.setServerEnabled(true, /*persist=*/false, opts.port);
        error.isNotEmpty())
      throw HostError(error.toStdString());

    {
      juce::DynamicObject::Ptr ready = new juce::DynamicObject();
      ready->setProperty("ready", true);
      ready->setProperty("connectionFile", CommandServer::connectionFile().getFullPathName());
      ready->setProperty("port", hub.server()->port());
      ready->setProperty("data", data.getFullPathName());
      ready->setProperty("library", library.getFullPathName());
      std::cout << juce::JSON::toString(juce::var(ready.get()), true) << std::endl;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#if !JUCE_WINDOWS
    std::signal(SIGPIPE, SIG_IGN);
#endif

    // Lives until the process ends (its reader thread is detached).
    auto* input = opts.readStdin ? new StdinLines() : nullptr;
    while (!quitRequested.load()) {
      juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
      // Model loads and edits defer work to timers a console app has to
      // drive; this is the turn of the crank the editor's poll gives the
      // application.
      proc.flushDeferredBlockRefresh();

      std::string line;
      while (!quitRequested.load() && input != nullptr && input->next(line)) {
        const juce::String text = juce::String::fromUTF8(line.c_str()).trim();
        if (text.isEmpty())
          continue;
        if (text == "exit" || text == "quit") {
          quitRequested.store(true);
          break;
        }
        juce::var request;
        CommandResult result;
        if (juce::JSON::parse(text, request).failed() || request.getDynamicObject() == nullptr ||
            !request["command"].isString())
          result = CommandResult::failure(CommandError::invalidArgs,
                                          "a line is a command as JSON, like {\"command\":"
                                          "\"approvals.list\"}, or the word exit");
        else
          result = hub.run(hub.localSession(), request["command"].toString(), request["args"]);
        juce::var envelope = result.toVar();
        if (!request["id"].isVoid())
          envelope.getDynamicObject()->setProperty("id", request["id"]);
        std::cout << juce::JSON::toString(envelope, true, 9) << std::endl;
      }
      if (input != nullptr && input->drained())
        quitRequested.store(true);
    }

    hub.setServerEnabled(false, /*persist=*/false);
  }

  // Hosted plugin instances are destroyed on a later turn of the loop.
  juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const HostError& e) {
    std::cerr << "PedaleiraHeadless: " << e.what() << " (see --help)\n";
    return 1;
  }
}

// PedaleiraPluginScan: scans ONE plugin file or identifier and reports what it
// holds. The engine's PluginScanner runs it once per candidate, in a process
// of its own, because scanning means loading the plugin's code: a plugin that
// crashes, hangs or corrupts memory while being looked at then takes this
// process down, never the app or the DAW hosting it.
//
//   PedaleiraPluginScan <format> <fileOrIdentifier> [--out <file>]
//
// <format> is a JUCE format name ("VST3", "AudioUnit"). The result is one XML
// document, on stdout or in --out:
//
//   <PedaleiraPluginScan complete="1">
//     <PLUGIN .../>      one juce::PluginDescription per plugin found
//   </PedaleiraPluginScan>
//
// The scanner uses --out: a plugin may print whatever it likes to stdout
// while it loads, and a file that is written in one piece at the very end
// tells "finished" from "died half way" on every platform (a missing or
// incomplete file is a crash). Exit code 0 when the scan ran to the end, even
// with no plugin found; 2 for a bad command line.
//
// No window, no audio device. The scan itself needs a running message loop
// (an Audio Unit v3 is instantiated asynchronously), so the main thread
// dispatches while the format does its work: VST3 on the message thread, as
// the VST3 rules ask, Audio Units on a worker so the loop stays unblocked.
#include <juce_audio_processors_headless/juce_audio_processors_headless.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#if JUCE_WINDOWS
#include <windows.h>
#endif

namespace {

// Test hooks for the scanner's own failure paths (test/src/plugin_scanner_tests.cpp):
// a helper that dies or never returns, without needing a plugin that does.
// Read from the environment so no command line a user could type reaches them.
void runSelfTestHook() {
  const char* hook = std::getenv("PEDALEIRA_PLUGINSCAN_SELFTEST");
  if (hook == nullptr)
    return;
  const std::string mode = hook;
  // A death without a result is all the scanner can see of a crash; a real
  // abort would add nothing but a crash report on the developer's machine.
  if (mode == "crash")
    std::_Exit(70);
  if (mode == "hang")
    for (;;)
      juce::Thread::sleep(1000);
}

std::unique_ptr<juce::AudioPluginFormat> makeFormat(const juce::String& name) {
#if JUCE_PLUGINHOST_VST3 && (JUCE_MAC || JUCE_WINDOWS || JUCE_LINUX || JUCE_BSD)
  if (name == juce::VST3PluginFormatHeadless::getFormatName())
    return std::make_unique<juce::VST3PluginFormatHeadless>();
#endif
#if JUCE_PLUGINHOST_AU && JUCE_MAC
  if (name == juce::AudioUnitPluginFormatHeadless::getFormatName())
    return std::make_unique<juce::AudioUnitPluginFormatHeadless>();
#endif
  juce::ignoreUnused(name);
  return nullptr;
}

juce::String scan(juce::AudioPluginFormat& format, const juce::String& fileOrIdentifier) {
  juce::OwnedArray<juce::PluginDescription> found;
  format.findAllTypesForFile(found, fileOrIdentifier);

  juce::XmlElement root("PedaleiraPluginScan");
  for (const auto* description : found)
    root.addChildElement(description->createXml().release());
  // Last, so a document cut short never carries it.
  root.setAttribute("complete", 1);
  return root.toString();
}

}  // namespace

int main(int argc, char** argv) {
#if JUCE_WINDOWS
  // A module that fails to load, or a crash inside one, must end this
  // process quietly instead of parking it behind a system dialog nobody is
  // there to dismiss.
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif

  juce::String formatName, fileOrIdentifier;
  juce::File outFile;
  int positional = 0;
  for (int i = 1; i < argc; ++i) {
    const juce::String arg = juce::String::fromUTF8(argv[i]);
    if (arg == "--out" && i + 1 < argc)
      outFile = juce::File::getCurrentWorkingDirectory().getChildFile(
          juce::String::fromUTF8(argv[++i]));
    else if (positional == 0)
      formatName = arg, ++positional;
    else if (positional == 1)
      fileOrIdentifier = arg, ++positional;
    else
      positional = 99;
  }
  if (positional != 2) {
    std::cerr << "usage: PedaleiraPluginScan <format> <fileOrIdentifier> [--out <file>]\n";
    return 2;
  }

  runSelfTestHook();

  juce::ScopedJuceInitialiser_GUI juceInit;
  std::unique_ptr<juce::AudioPluginFormat> format = makeFormat(formatName);
  if (format == nullptr) {
    std::cerr << "PedaleiraPluginScan: this build cannot scan the format '" << formatName << "'\n";
    return 2;
  }

  juce::String result;
  std::atomic<bool> finished{false};
  std::thread worker;
  if (formatName == "AudioUnit") {
    worker = std::thread([&] {
      result = scan(*format, fileOrIdentifier);
      finished.store(true);
    });
  } else {
    juce::MessageManager::callAsync([&] {
      result = scan(*format, fileOrIdentifier);
      finished.store(true);
    });
  }
  // Dispatched in slices rather than with runDispatchLoop: on macOS that one
  // runs NSApp, which a console tool without the GUI modules never creates.
  while (!finished.load())
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
  if (worker.joinable())
    worker.join();

  if (outFile != juce::File()) {
    // Written next to the target and moved into place: the scanner never
    // sees half a document.
    const juce::File partial = outFile.getSiblingFile(outFile.getFileName() + ".part");
    if (!partial.replaceWithText(result) || !partial.moveFileTo(outFile)) {
      std::cerr << "PedaleiraPluginScan: cannot write " << outFile.getFullPathName() << "\n";
      return 1;
    }
  } else {
    std::cout << result << std::endl;
  }
  return 0;
}

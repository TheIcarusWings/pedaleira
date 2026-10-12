#pragma once
// Shared by the plugin scanner, hosting and golden render tests: where the
// test plugin lives, a scanner sandbox per test, and waits that pump the
// message loop (a hosted plugin is created on the message thread, which in
// this binary is the thread the tests run on).

#include "PluginScanner.h"
#include "Processor.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

// PedaleiraTestFx.vst3 in the build tree (test/plugins/PedaleiraTestFx.cpp), and
// the folder that holds it and nothing else.
inline juce::File testPluginFile() { return juce::File(T3K_TEST_PLUGIN_FILE); }
inline juce::File testPluginFolder() { return testPluginFile().getParentDirectory(); }

// A scanned description names the bundle or, on some platforms, the binary
// inside it.
inline bool isTestPluginFile(const juce::String& fileOrIdentifier) {
  const juce::File file(fileOrIdentifier);
  return file == testPluginFile() || file.isAChildOf(testPluginFile());
}

// Sets or clears (empty value) a variable in the process environment, where
// both the scan helper (a child process) and the test plugin (loaded into
// this one) read their test switches.
inline void setEnvironment(const char* name, const std::string& value) {
#if JUCE_WINDOWS
  _putenv_s(name, value.c_str());
#else
  if (value.empty())
    unsetenv(name);
  else
    setenv(name, value.c_str(), 1);
#endif
}

struct ScopedEnvironment {
  ScopedEnvironment(const char* variable, const std::string& value) : name(variable) {
    setEnvironment(name, value);
  }
  ~ScopedEnvironment() { setEnvironment(name, {}); }
  const char* name;
};

// A scanner with a cache folder of its own, empty to start with. main()
// already keeps the scanner away from the user's cache and plugin folders;
// this keeps the tests away from each other when one process runs several.
struct ScannerSandbox {
  ScannerSandbox()
      : dir(juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("t3k-scan-test-" + juce::Uuid().toString())) {
    dir.createDirectory();
    previous = PluginScanner::setCacheDirForTesting(dir);
  }
  ~ScannerSandbox() {
    PluginScanner::setCacheDirForTesting(previous);
    PluginScanner::setHelperForTesting(juce::File());
    PluginScanner::setScanTimeoutMs(45000);
    dir.deleteRecursively();
  }

  // A scratch folder inside the sandbox.
  juce::File folder(const juce::String& name) const {
    const juce::File made = dir.getChildFile(name);
    made.createDirectory();
    return made;
  }

  juce::File dir;
  juce::File previous;
};

// Scans `folders` (explicitly, nothing else) and waits for the end.
inline PluginScanner::ScanStatus scanFolders(const juce::StringArray& folders,
                                             bool rescanAll = false) {
  PluginScanner& scanner = PluginScanner::instance();
  PluginScanner::ScanOptions options;
  options.folders = folders;
  options.rescanAll = rescanAll;
  EXPECT_TRUE(scanner.startScan(options)) << "scan helper missing: "
                                          << scanner.helperFile().getFullPathName();
  EXPECT_TRUE(scanner.waitForScan(120000)) << "the scan never finished";
  return scanner.status();
}

// The listed entry called `name`, or one with an empty name.
inline PluginScanner::ListedPlugin listedPlugin(const juce::String& name) {
  for (const auto& listed : PluginScanner::instance().list())
    if (listed.description.name == name)
      return listed;
  return {};
}

// Scans the test plugin's folder and returns the identifier chain.addPlugin
// takes for it.
inline juce::String scanTestPlugin() {
  EXPECT_TRUE(testPluginFile().exists())
      << "test plugin not built: " << testPluginFile().getFullPathName();
  scanFolders({testPluginFolder().getFullPathName()});
  const PluginScanner::ListedPlugin fx = listedPlugin("PedaleiraTestFx");
  EXPECT_EQ(fx.status, PluginScanner::PluginStatus::ok) << fx.reason;
  EXPECT_TRUE(fx.description.name.isNotEmpty()) << "the scan did not find PedaleiraTestFx";
  return fx.description.createIdentifierString();
}

// waitForChainLoaded for rigs with hosted plugins: the same condition, but
// the message loop runs while waiting (that is where a plugin is created and
// installed) and the deferred refresh is delivered, as state.waitUntilLoaded
// does for a headless caller.
inline bool waitForPluginsLoaded(TONE3000Processor& proc, int timeoutMs = 30000) {
  const auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(timeoutMs);
  while (juce::Time::getMillisecondCounter() < deadline) {
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    proc.flushDeferredBlockRefresh();
    const juce::var state = proc.getChainState(-1);
    bool allLoaded = true;
    for (const auto* lane : {state["chain"].getArray(), state["chainRight"].getArray()}) {
      if (lane == nullptr)
        continue;
      for (const auto& item : *lane)
        if (item["kind"].toString() == "tone" && !static_cast<bool>(item["missing"]) &&
            (!static_cast<bool>(item["loaded"]) || static_cast<bool>(item["modelLoading"])))
          allLoaded = false;
    }
    if (allLoaded && !proc.isChainEditFadeHeld())
      return true;
  }
  return false;
}

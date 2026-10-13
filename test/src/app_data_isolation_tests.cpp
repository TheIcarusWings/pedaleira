// The test binary leaves the developer's own folders alone
//
// A run of the application keeps files per user: settings, presets, the model
// stash, the plugin cache and the agent layer's files in the app data folder
// (~/Library/Application Support/Pedaleira on macOS), the diagnostic log, and
// the library in Documents. main() in dsp_tests.cpp points each of them at a
// scratch folder before the first processor exists. These tests are the net
// under that: they look at the real folders themselves, so a path that goes
// round the redirects, or a redirect somebody resets, fails here instead of
// leaving a folder behind on every machine that runs the suite.
//
//   - A guard for the whole process: the real folders are listed before the
//     first test and again after the last, so it covers every test, filtered
//     runs (--gtest_filter='*Golden*') included.
//   - A processor is built, prepared and run for a few blocks, the way a host
//     does, with the real folders listed on either side.
//   - Every default location follows the one app data override
//     (LibraryPaths::setAppDataDirForTesting), which is what catches whatever
//     has no hook of its own.
#include "AgentHub.h"
#include "AppIdentity.h"
#include "CommandServer.h"
#include "LibraryPaths.h"
#include "PluginScanner.h"
#include "PresetManager.h"
#include "Processor.h"

#include <gtest/gtest.h>

#include <cmath>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

struct RealFolder {
  const char* what;
  juce::File folder;
};

// The folders as the platform names them, worked out here and not through
// LibraryPaths: a mistake there must not be able to hide from this check.
std::vector<RealFolder> realFolders() {
  juce::File appData = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
#if JUCE_MAC
  appData = appData.getChildFile("Application Support");
#endif
  return {
      {"app data folder", appData.getChildFile(kAppDataFolderName)},
      {"log folder", juce::FileLogger::getSystemLogFileFolder().getChildFile(kAppDataFolderName)},
      {"library folder",
       juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
           .getChildFile(kAppDataFolderName)},
  };
}

// What is in a folder: every path under it, relative to it, a folder's with a
// trailing slash. Nothing at all when the folder is not there, which is not
// the same as there and empty. Hidden entries are left out (a Finder window
// open on the folder makes a .DS_Store in it), and so is anything inside a
// link.
using Listing = std::optional<std::set<std::string>>;

Listing list(const juce::File& folder) {
  if (!folder.isDirectory())
    return std::nullopt;
  std::set<std::string> paths;
  for (const auto& entry : juce::RangedDirectoryIterator(
           folder, true, "*", juce::File::findFilesAndDirectories | juce::File::ignoreHiddenFiles,
           juce::File::FollowSymlinks::no))
    paths.insert(entry.getFile().getRelativePathFrom(folder).toStdString() +
                 (entry.isDirectory() ? "/" : ""));
  return paths;
}

using Listings = std::vector<Listing>;

Listings listAll() {
  Listings all;
  for (const auto& real : realFolders())
    all.push_back(list(real.folder));
  return all;
}

// Reports every way the real folders differ from `before`. A folder that was
// not there must still not be; one that was must hold the same paths (what is
// inside a path is the application's own business: a copy running next to the
// tests keeps writing its log).
void expectUnchanged(const Listings& before, const char* during) {
  const std::vector<RealFolder> folders = realFolders();
  ASSERT_EQ(before.size(), folders.size());
  for (size_t i = 0; i < folders.size(); ++i) {
    const std::string name =
        std::string(folders[i].what) + " " + folders[i].folder.getFullPathName().toStdString();
    const Listing now = list(folders[i].folder);
    if (!before[i].has_value()) {
      EXPECT_FALSE(now.has_value()) << name << " was not there and " << during << " made it";
      continue;
    }
    if (!now.has_value()) {
      ADD_FAILURE() << name << " was there and " << during << " removed it";
      continue;
    }
    for (const auto& path : *now)
      EXPECT_TRUE(before[i]->count(path) > 0) << name << ": " << during << " added " << path;
    for (const auto& path : *before[i])
      EXPECT_TRUE(now->count(path) > 0) << name << ": " << during << " removed " << path;
  }
}

// Lists the real folders before the first test of the process and checks them
// after the last. It cannot say which test did it: run with --gtest_filter to
// find out, and look at whether the path it used goes through
// LibraryPaths::appDataDir().
class RealFoldersGuard : public ::testing::Environment {
public:
  void SetUp() override { atStart = listAll(); }
  void TearDown() override { expectUnchanged(atStart, "the test run"); }

private:
  Listings atStart;
};

[[maybe_unused]] ::testing::Environment* const realFoldersGuard =
    ::testing::AddGlobalTestEnvironment(new RealFoldersGuard);

// Puts back the hooks main() set, whatever the test did to them.
struct HookRestorer {
  ~HookRestorer() {
    LibraryPaths::setAppDataDirForTesting(appData);
    TONE3000Processor::setLocalModelsDirForTesting(models);
    PluginScanner::setCacheDirForTesting(pluginCache);
    AgentSettings::setFileForTesting(agentSettings);
    CommandServer::setConnectionFileForTesting(connectionFile);
  }
  juce::File appData, models, pluginCache, agentSettings, connectionFile;
};

}  // namespace

// What a host does with a plugin: construct it, prepare it, run a few blocks
// of audio through it, release it.
TEST(AppDataIsolation, BuildingAndRenderingAProcessorLeavesTheRealFoldersAlone) {
  const Listings before = listAll();
  {
    TONE3000Processor proc;
    proc.setMultiCoreEnabled(false, /*persist=*/false);
    proc.setPlayConfigDetails(2, 2, 48000.0, 128);
    proc.prepareToPlay(48000.0, 128);

    // The log it writes is not in a real folder either (a log that already
    // exists there keeps its name, so the listing could not tell).
    if (const auto* fileLogger =
            dynamic_cast<const juce::FileLogger*>(juce::Logger::getCurrentLogger()))
      for (const auto& real : realFolders())
        EXPECT_FALSE(fileLogger->getLogFile().isAChildOf(real.folder))
            << "the processor logs to " << fileLogger->getLogFile().getFullPathName();

    juce::AudioBuffer<float> buffer(2, 128);
    juce::MidiBuffer midi;
    for (int block = 0; block < 16; ++block) {
      for (int channel = 0; channel < 2; ++channel)
        for (int i = 0; i < 128; ++i)
          buffer.setSample(channel, i,
                           0.1f * std::sin(0.05f * static_cast<float>(block * 128 + i)));
      proc.processBlock(buffer, midi);
    }
    proc.releaseResources();
  }
  expectUnchanged(before, "building and rendering a processor");
}

// Everything the application keeps per user hangs off one folder, and the one
// override moves all of it: settings, log, presets, model stash, plugin cache,
// agent settings and the connection file.
TEST(AppDataIsolation, EveryDefaultLocationFollowsTheAppDataOverride) {
  const juce::File realDefault = LibraryPaths::defaultAppDataDir();
  const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                             .getChildFile("t3k-appdata-override-" + juce::Uuid().toString());
  // The hooks main() set for single files would hide the defaults under them.
  HookRestorer restore{LibraryPaths::setAppDataDirForTesting(dir),
                       TONE3000Processor::setLocalModelsDirForTesting({}),
                       PluginScanner::setCacheDirForTesting({}),
                       AgentSettings::setFileForTesting({}),
                       CommandServer::setConnectionFileForTesting({})};

  EXPECT_EQ(LibraryPaths::appDataDir(), dir);
  EXPECT_TRUE(LibraryPaths::appDataDirIsOverridden());
  // The user's own is still known, for the code that has to tell whether a
  // folder is theirs.
  EXPECT_EQ(LibraryPaths::defaultAppDataDir(), realDefault);
  EXPECT_NE(realDefault, dir);

  EXPECT_EQ(TONE3000Processor::getSettingsFile().getParentDirectory(), dir);
  EXPECT_EQ(TONE3000Processor::uiPreferencesOptions().getDefaultFile().getParentDirectory(), dir);
  EXPECT_EQ(TONE3000Processor::getLogFile().getParentDirectory(), dir);
  EXPECT_EQ(PresetManager().userPresetsDir(), dir.getChildFile("Presets"));
  EXPECT_EQ(TONE3000Processor::getLocalModelsDir(), dir.getChildFile("LocalModels"));
  EXPECT_EQ(PluginScanner::instance().cacheFile().getParentDirectory(), dir);
  EXPECT_EQ(AgentSettings::file().getParentDirectory(), dir);
  EXPECT_EQ(CommandServer::connectionFile().getParentDirectory(), dir);
  // Looking at them made nothing.
  EXPECT_FALSE(dir.exists());

  // Without the override they are the user's again.
  LibraryPaths::setAppDataDirForTesting({});
  EXPECT_EQ(LibraryPaths::appDataDir(), realDefault);
  EXPECT_FALSE(LibraryPaths::appDataDirIsOverridden());
  EXPECT_EQ(TONE3000Processor::getSettingsFile().getParentDirectory(), realDefault);
  EXPECT_EQ(PresetManager().userPresetsDir(), realDefault.getChildFile("Presets"));
}

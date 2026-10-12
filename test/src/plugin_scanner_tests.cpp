// PluginScanner (plugin/include/PluginScanner.h): finding the user's plugins
// through the helper executable, without ever loading one into this process.
//
//   - the test plugin (PedaleiraTestFx.vst3, built with this suite) is found
//   - the cache file round-trips, and an unchanged file is not scanned again
//   - a file that is not a plugin, a helper that dies and a helper that
//     hangs are each blacklisted with a reason, and this process lives on
//   - a scan can be cancelled; without the helper the scanner is unavailable
//   - instruments are listed as "not an effect" and cannot be added
//   - folders: a plugin whose folder is no longer searched shows as missing
//
// Every test scans folders it names itself (the build tree, or scratch
// folders); main() turns the formats' default locations off, so nothing here
// looks at the plugins installed on the machine.
#include "EngineCommands.h"
#include "hosted_plugin_test_helpers.h"

#include <gtest/gtest.h>

namespace {

using Status = PluginScanner::PluginStatus;
using State = PluginScanner::ScanState;

// A ".vst3" that is not one: a plain file of junk, and (where VST3s are
// bundles) a bundle folder whose binary is junk.
void makeBrokenPlugins(const juce::File& folder) {
  juce::MemoryBlock junk(4096, true);
  for (size_t i = 0; i < junk.getSize(); ++i)
    junk[i] = static_cast<char>((i * 131 + 7) & 0xff);

  folder.getChildFile("BrokenFile.vst3").replaceWithData(junk.getData(), junk.getSize());
#if !JUCE_WINDOWS
  const juce::File binary = folder.getChildFile("BrokenBundle.vst3")
                                .getChildFile("Contents")
#if JUCE_MAC
                                .getChildFile("MacOS")
                                .getChildFile("BrokenBundle");
#else
                                .getChildFile("x86_64-linux")
                                .getChildFile("BrokenBundle.so");
#endif
  binary.getParentDirectory().createDirectory();
  binary.replaceWithData(junk.getData(), junk.getSize());
#endif
}

int countWithStatus(Status status) {
  int count = 0;
  for (const auto& listed : PluginScanner::instance().list())
    if (listed.status == status)
      ++count;
  return count;
}

}  // namespace

TEST(PluginScannerTest, FindsTheTestPluginThroughTheHelper) {
  ScannerSandbox sandbox;
  PluginScanner& scanner = PluginScanner::instance();
  ASSERT_TRUE(scanner.isAvailable()) << scanner.helperFile().getFullPathName();
  EXPECT_EQ(scanner.status().state, State::idle);
  EXPECT_TRUE(scanner.list().empty());

  const PluginScanner::ScanStatus status = scanFolders({testPluginFolder().getFullPathName()});
  EXPECT_EQ(status.state, State::finished);
  EXPECT_EQ(status.total, 1);
  EXPECT_EQ(status.done, 1);
  EXPECT_EQ(status.scanned, 1);
  EXPECT_EQ(status.failed, 0);
  EXPECT_TRUE(status.currentFile.isEmpty());

  const PluginScanner::ListedPlugin fx = listedPlugin("PedaleiraTestFx");
  ASSERT_EQ(fx.status, Status::ok) << fx.reason;
  EXPECT_EQ(fx.description.pluginFormatName, juce::String("VST3"));
  EXPECT_EQ(fx.description.manufacturerName, juce::String("PedaleiraTests"));
  EXPECT_FALSE(fx.description.isInstrument);
  EXPECT_EQ(fx.description.numInputChannels, 2);
  EXPECT_EQ(fx.description.numOutputChannels, 2);
  EXPECT_TRUE(isTestPluginFile(fx.description.fileOrIdentifier))
      << fx.description.fileOrIdentifier;

  // Addable, by identifier, by file path and by name.
  ASSERT_EQ(scanner.knownEffects().size(), 1);
  const juce::String identifier = fx.description.createIdentifierString();
  EXPECT_TRUE(scanner.findEffect(identifier).has_value());
  EXPECT_TRUE(scanner.findEffect(testPluginFile().getFullPathName()).has_value());
  EXPECT_TRUE(scanner.findEffect("pedaleiratestfx").has_value());
  juce::String error;
  EXPECT_FALSE(scanner.findEffect("No Such Plugin", &error).has_value());
  EXPECT_TRUE(error.contains("No Such Plugin"));
  EXPECT_TRUE(scanner.findEffect(HostedPluginIdentity::fromDescription(fx.description)).has_value());
}

TEST(PluginScannerTest, CacheRoundTripsAndUnchangedFilesAreNotRescanned) {
  ScannerSandbox sandbox;
  PluginScanner& scanner = PluginScanner::instance();
  const juce::String folder = testPluginFolder().getFullPathName();
  scanFolders({folder});
  const juce::uint32 generation = scanner.generation();
  ASSERT_TRUE(scanner.cacheFile().existsAsFile());
  EXPECT_TRUE(scanner.cacheFile().isAChildOf(sandbox.dir)) << "the cache left its sandbox";
  {
    // The addable effects are in the file as a juce::KnownPluginList.
    const auto cache = juce::parseXML(scanner.cacheFile());
    ASSERT_NE(cache, nullptr);
    const auto* knownXml = cache->getChildByName("KNOWNPLUGINS");
    ASSERT_NE(knownXml, nullptr);
    juce::KnownPluginList known;
    known.recreateFromXml(*knownXml);
    ASSERT_EQ(known.getNumTypes(), 1);
    EXPECT_EQ(known.getTypes()[0].name, juce::String("PedaleiraTestFx"));
  }
  const juce::String identifier = listedPlugin("PedaleiraTestFx").description.createIdentifierString();

  // The same file again: found, not scanned.
  PluginScanner::ScanStatus again = scanFolders({folder});
  EXPECT_EQ(again.state, State::finished);
  EXPECT_EQ(again.done, 1);
  EXPECT_EQ(again.scanned, 0) << "an unchanged file went through the helper again";
  EXPECT_GT(scanner.generation(), generation);
  EXPECT_EQ(listedPlugin("PedaleiraTestFx").status, Status::ok);

  // Unless asked to.
  EXPECT_EQ(scanFolders({folder}, /*rescanAll=*/true).scanned, 1);

  // A scanner that starts from the cache file knows the plugin without a
  // scan, under the same identifier.
  PluginScanner::setCacheDirForTesting(sandbox.dir);
  EXPECT_EQ(scanner.status().state, State::idle);
  const PluginScanner::ListedPlugin cached = listedPlugin("PedaleiraTestFx");
  EXPECT_EQ(cached.status, Status::ok);
  EXPECT_EQ(cached.description.createIdentifierString(), identifier);
  EXPECT_EQ(cached.description.manufacturerName, juce::String("PedaleiraTests"));
  EXPECT_EQ(scanner.knownEffects().size(), 1);
  // And still treats the file as scanned.
  EXPECT_EQ(scanFolders({folder}).scanned, 0);

  // A file that changed is scanned again. (Touch a copy, not the build's.)
  const juce::File copyFolder = sandbox.folder("copy");
  ASSERT_TRUE(testPluginFile().copyDirectoryTo(copyFolder.getChildFile("PedaleiraTestFx.vst3")) ||
              testPluginFile().copyFileTo(copyFolder.getChildFile("PedaleiraTestFx.vst3")));
  EXPECT_EQ(scanFolders({copyFolder.getFullPathName()}).scanned, 1);
  EXPECT_EQ(scanFolders({copyFolder.getFullPathName()}).scanned, 0);
  const juce::File marker = copyFolder.getChildFile("PedaleiraTestFx.vst3").isDirectory()
                                ? copyFolder.getChildFile("PedaleiraTestFx.vst3")
                                      .getChildFile("Contents")
                                      .getChildFile("touched")
                                : copyFolder.getChildFile("PedaleiraTestFx.vst3");
  if (marker.getFileName() == "touched")
    marker.replaceWithText("x");
  marker.setLastModificationTime(juce::Time::getCurrentTime() + juce::RelativeTime::seconds(5));
  EXPECT_EQ(scanFolders({copyFolder.getFullPathName()}).scanned, 1) << "a changed file was skipped";
}

TEST(PluginScannerTest, BrokenPluginsAreBlacklistedWithoutTakingTheProcessDown) {
  ScannerSandbox sandbox;
  const juce::File folder = sandbox.folder("broken");
  makeBrokenPlugins(folder);

  const PluginScanner::ScanStatus status = scanFolders({folder.getFullPathName()});
  EXPECT_EQ(status.state, State::finished);
  ASSERT_GE(status.total, 1);
  EXPECT_EQ(status.failed, status.total);
  EXPECT_EQ(countWithStatus(Status::failed), status.total);
  EXPECT_EQ(countWithStatus(Status::ok), 0);

  const PluginScanner::ListedPlugin broken = listedPlugin("BrokenFile");
  EXPECT_EQ(broken.status, Status::failed);
  EXPECT_TRUE(broken.reason.isNotEmpty());
  juce::String error;
  EXPECT_FALSE(PluginScanner::instance().findEffect("BrokenFile", &error).has_value());
  EXPECT_TRUE(error.contains("failed scan")) << error;

  // Blacklisted means not scanned again, until the file changes.
  EXPECT_EQ(scanFolders({folder.getFullPathName()}).scanned, 0);
  EXPECT_EQ(listedPlugin("BrokenFile").status, Status::failed);

  // A good plugin next to the bad ones is unaffected.
  const PluginScanner::ScanStatus mixed =
      scanFolders({folder.getFullPathName(), testPluginFolder().getFullPathName()});
  EXPECT_EQ(mixed.failed, 0);
  EXPECT_EQ(mixed.scanned, 1);
  EXPECT_EQ(listedPlugin("PedaleiraTestFx").status, Status::ok);
}

TEST(PluginScannerTest, AHelperThatCrashesOrHangsIsBlacklistedWithTheReason) {
  ScannerSandbox sandbox;
  const juce::String folder = testPluginFolder().getFullPathName();

  {
    // The helper dies before it reports: the plugin "crashed".
    ScopedEnvironment crash("PEDALEIRA_PLUGINSCAN_SELFTEST", "crash");
    const PluginScanner::ScanStatus status = scanFolders({folder});
    EXPECT_EQ(status.state, State::finished);
    EXPECT_EQ(status.failed, 1);
    const PluginScanner::ListedPlugin fx = listedPlugin("PedaleiraTestFx");
    EXPECT_EQ(fx.status, Status::failed);
    EXPECT_TRUE(fx.reason.contains("crashed")) << fx.reason;
  }
  {
    // The helper never returns: killed at the timeout.
    ScopedEnvironment hang("PEDALEIRA_PLUGINSCAN_SELFTEST", "hang");
    PluginScanner::setScanTimeoutMs(400);
    const juce::uint32 started = juce::Time::getMillisecondCounter();
    const PluginScanner::ScanStatus status = scanFolders({folder}, /*rescanAll=*/true);
    EXPECT_LT(juce::Time::getMillisecondCounter() - started, 10000u);
    EXPECT_EQ(status.failed, 1);
    const PluginScanner::ListedPlugin fx = listedPlugin("PedaleiraTestFx");
    EXPECT_EQ(fx.status, Status::failed);
    EXPECT_TRUE(fx.reason.contains("timed out")) << fx.reason;
  }

  // With a helper that behaves, a forced rescan clears the verdict.
  PluginScanner::setScanTimeoutMs(45000);
  EXPECT_EQ(scanFolders({folder}).scanned, 0) << "a blacklisted file was rescanned unasked";
  scanFolders({folder}, /*rescanAll=*/true);
  EXPECT_EQ(listedPlugin("PedaleiraTestFx").status, Status::ok);
}

TEST(PluginScannerTest, AScanCanBeCancelled) {
  ScannerSandbox sandbox;
  ScopedEnvironment hang("PEDALEIRA_PLUGINSCAN_SELFTEST", "hang");
  PluginScanner& scanner = PluginScanner::instance();

  PluginScanner::ScanOptions options;
  options.folders.add(testPluginFolder().getFullPathName());
  ASSERT_TRUE(scanner.startScan(options));

  // Progress is visible while it runs: the file the helper is on.
  const juce::uint32 deadline = juce::Time::getMillisecondCounter() + 10000;
  while (scanner.status().currentFile.isEmpty() && juce::Time::getMillisecondCounter() < deadline)
    juce::Thread::sleep(5);
  const PluginScanner::ScanStatus running = scanner.status();
  EXPECT_EQ(running.state, State::scanning);
  EXPECT_EQ(running.total, 1);
  EXPECT_EQ(running.done, 0);
  EXPECT_EQ(juce::File(running.currentFile), testPluginFile());
  EXPECT_FALSE(scanner.waitForScan(50));

  // The helper would hang for 45 s; cancelling kills it and returns.
  const juce::uint32 started = juce::Time::getMillisecondCounter();
  scanner.cancelScan();
  EXPECT_LT(juce::Time::getMillisecondCounter() - started, 5000u);
  EXPECT_EQ(scanner.status().state, State::cancelled);
  EXPECT_TRUE(scanner.waitForScan(0));
  // Nothing was learnt about the plugin, so nothing is recorded against it.
  EXPECT_TRUE(listedPlugin("PedaleiraTestFx").description.name.isEmpty());
}

TEST(PluginScannerTest, WithoutTheHelperScanningIsUnavailable) {
  ScannerSandbox sandbox;
  PluginScanner& scanner = PluginScanner::instance();
  PluginScanner::setHelperForTesting(sandbox.dir.getChildFile("no-such-helper"));
  EXPECT_FALSE(scanner.isAvailable());

  PluginScanner::ScanOptions options;
  options.folders.add(testPluginFolder().getFullPathName());
  EXPECT_FALSE(scanner.startScan(options));
  EXPECT_EQ(scanner.status().state, State::unavailable);
  EXPECT_TRUE(scanner.list().empty()) << "the scanner fell back to scanning in-process";

  // The command says the same, as an error a client can act on.
  TONE3000Processor proc;
  EngineCommands commands(proc);
  const CommandResult scan = commands.registry().run(
      "plugins.scan", juce::JSON::parse("{\"folders\":[" +
                                        juce::JSON::toString(juce::var(
                                            testPluginFolder().getFullPathName())) +
                                        "]}"));
  EXPECT_FALSE(scan.ok);
  EXPECT_EQ(scan.errorCode, juce::String(CommandError::unavailable));
  const CommandResult status = commands.registry().run("plugins.scanStatus");
  ASSERT_TRUE(status.ok);
  EXPECT_EQ(status.value["state"].toString(), juce::String("unavailable"));
  EXPECT_FALSE(static_cast<bool>(status.value["available"]));
}

TEST(PluginScannerTest, InstrumentsAreListedButNotOffered) {
  // What a scan of a synth and of a MIDI effect leaves behind, written as
  // the cache file a scan would have saved (no such plugins are built here).
  ScannerSandbox sandbox;
  juce::PluginDescription synth;
  synth.name = "Test Synth";
  synth.pluginFormatName = "VST3";
  synth.fileOrIdentifier = "/nowhere/TestSynth.vst3";
  synth.manufacturerName = "PedaleiraTests";
  synth.isInstrument = true;
  synth.numInputChannels = 0;
  synth.numOutputChannels = 2;
  synth.uniqueId = 11;
  juce::PluginDescription midiFx = synth;
  midiFx.name = "Test Arp";
  midiFx.fileOrIdentifier = "/nowhere/TestArp.vst3";
  midiFx.isInstrument = false;
  midiFx.numOutputChannels = 0;
  midiFx.uniqueId = 12;

  juce::XmlElement root("PedaleiraPluginCache");
  root.setAttribute("version", 1);
  for (const auto& plugin : {synth, midiFx}) {
    auto* record = root.createNewChildElement("Record");
    record->setAttribute("format", plugin.pluginFormatName);
    record->setAttribute("file", plugin.fileOrIdentifier);
    record->setAttribute("stamp", "1");
    record->setAttribute("present", true);
    record->addChildElement(plugin.createXml().release());
  }
  ASSERT_TRUE(root.writeTo(sandbox.dir.getChildFile("PluginCache.xml")));
  PluginScanner::setCacheDirForTesting(sandbox.dir);

  PluginScanner& scanner = PluginScanner::instance();
  const PluginScanner::ListedPlugin listedSynth = listedPlugin("Test Synth");
  EXPECT_EQ(listedSynth.status, Status::notEffect);
  EXPECT_EQ(listedSynth.reason, juce::String("instrument"));
  EXPECT_EQ(listedPlugin("Test Arp").status, Status::notEffect);
  EXPECT_TRUE(scanner.knownEffects().isEmpty());

  juce::String error;
  EXPECT_FALSE(scanner.findEffect("Test Synth", &error).has_value());
  EXPECT_TRUE(error.contains("not an effect")) << error;

  TONE3000Processor proc;
  const juce::var added = proc.addPluginBlock(synth.createIdentifierString());
  EXPECT_TRUE(added["blockId"].toString().isEmpty());
  EXPECT_TRUE(added["error"].toString().contains("not an effect")) << added["error"].toString();
}

TEST(PluginScannerTest, FoldersAreKeptAndAPluginOutsideThemGoesMissing) {
  ScannerSandbox sandbox;
  TONE3000Processor proc;
  EngineCommands commands(proc);
  const auto run = [&](const char* name, const juce::var& args = {}) {
    const CommandResult result = commands.registry().run(name, args);
    EXPECT_TRUE(result.ok) << name << ": " << result.errorMessage;
    return result.value;
  };
  const auto pathArg = [](const juce::File& folder) {
    juce::DynamicObject::Ptr args = new juce::DynamicObject();
    args->setProperty("path", folder.getFullPathName());
    return juce::var(args.get());
  };
  const juce::var waitArgs = juce::JSON::parse(R"({"waitMs":120000})");
  const juce::File empty = sandbox.folder("empty");

  // Added folders are searched by a scan of the usual places (the formats'
  // own default locations are switched off for this binary).
  EXPECT_EQ(run("plugins.addFolder", pathArg(testPluginFolder()))["folders"].size(), 1);
  EXPECT_FALSE(static_cast<bool>(run("plugins.addFolder", pathArg(testPluginFolder()))["changed"]));
  juce::var status = run("plugins.scan", waitArgs);
  EXPECT_EQ(status["state"].toString(), juce::String("finished"));
  EXPECT_EQ(static_cast<int>(status["total"]), 1);

  juce::var listed = run("plugins.list");
  ASSERT_EQ(listed["plugins"].size(), 1);
  const juce::var fx = listed["plugins"][0];
  EXPECT_EQ(fx["name"].toString(), juce::String("PedaleiraTestFx"));
  EXPECT_EQ(fx["vendor"].toString(), juce::String("PedaleiraTests"));
  EXPECT_EQ(fx["format"].toString(), juce::String("VST3"));
  EXPECT_EQ(fx["status"].toString(), juce::String("ok"));
  EXPECT_EQ(static_cast<int>(fx["inputChannels"]), 2);
  EXPECT_EQ(static_cast<int>(fx["outputChannels"]), 2);
  EXPECT_TRUE(fx["identifier"].toString().isNotEmpty());
  EXPECT_EQ(listed["folders"].size(), 1);
  EXPECT_EQ(listed["scan"]["state"].toString(), juce::String("finished"));

  // The folders survive a restart of the scanner (they live in the cache).
  PluginScanner::setCacheDirForTesting(sandbox.dir);
  EXPECT_EQ(PluginScanner::instance().userFolders().size(), 1);

  // Point the scanner at an empty folder instead: the plugin is still
  // known, but no longer found, and cannot be added.
  run("plugins.removeFolder", pathArg(testPluginFolder()));
  run("plugins.addFolder", pathArg(empty));
  status = run("plugins.scan", waitArgs);
  EXPECT_EQ(static_cast<int>(status["total"]), 0);
  listed = run("plugins.list");
  ASSERT_EQ(listed["plugins"].size(), 1);
  EXPECT_EQ(listed["plugins"][0]["status"].toString(), juce::String("missing"));
  EXPECT_TRUE(PluginScanner::instance().knownEffects().isEmpty());
  const CommandResult add = commands.registry().run(
      "chain.addPlugin", juce::JSON::parse(R"({"identifier":"PedaleiraTestFx"})"));
  EXPECT_FALSE(add.ok);
  EXPECT_EQ(add.errorCode, juce::String(CommandError::notFound));

  // Back in a searched folder it is ok again, without another helper run.
  run("plugins.addFolder", pathArg(testPluginFolder()));
  status = run("plugins.scan", waitArgs);
  EXPECT_EQ(static_cast<int>(status["scanned"]), 0);
  EXPECT_EQ(run("plugins.list")["plugins"][0]["status"].toString(), juce::String("ok"));

  // Argument checks.
  EXPECT_EQ(commands.registry().run("plugins.addFolder", juce::JSON::parse(R"({"path":"relative"})"))
                .errorCode,
            juce::String(CommandError::invalidArgs));
  EXPECT_EQ(commands.registry().run("plugins.removeFolder", pathArg(sandbox.folder("other")))
                .errorCode,
            juce::String(CommandError::notFound));
  EXPECT_EQ(commands.registry()
                .run("plugins.scan", juce::JSON::parse(R"({"formats":["VST9"]})"))
                .errorCode,
            juce::String(CommandError::invalidArgs));
}

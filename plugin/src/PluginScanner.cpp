#include "PluginScanner.h"
#include "LibraryPaths.h"

#include <algorithm>
#include <mutex>

namespace {

constexpr const char* kHelperName =
#if JUCE_WINDOWS
    "PedaleiraPluginScan.exe";
#else
    "PedaleiraPluginScan";
#endif

constexpr const char* kCacheFileName = "PluginCache.xml";
constexpr int kCacheVersion = 1;

// Test / tool overrides (see the setters). Read from several threads.
std::mutex overrideMutex;
juce::File cacheDirOverride;
juce::File helperOverride;
std::atomic<bool> defaultLocationsEnabled{true};
std::atomic<int> scanTimeoutMs{45000};

juce::File cacheDir() {
  {
    const std::lock_guard<std::mutex> guard(overrideMutex);
    if (cacheDirOverride != juce::File())
      return cacheDirOverride;
  }
  return LibraryPaths::appDataDir();
}

bool isAudioUnit(const juce::String& formatName) { return formatName == "AudioUnit"; }

// An effect has audio going in and coming out and is not an instrument.
// Everything else (synths, generators, MIDI processors) is listed but not
// offered to a chain. Returns why not, or an empty string for an effect.
juce::String whyNotAnEffect(const juce::PluginDescription& description) {
  if (description.isInstrument)
    return "instrument";
  if (description.numInputChannels <= 0)
    return "no audio input";
  if (description.numOutputChannels <= 0)
    return "no audio output";
  return {};
}

// What a file looked like when it was scanned. A VST3 is usually a bundle
// folder, and rebuilding or updating the plugin rewrites the binary inside
// without touching the folder's own time, so the stamp is the newest time of
// anything in it.
juce::int64 stampOf(const juce::String& formatName, const juce::String& fileOrIdentifier) {
  if (isAudioUnit(formatName))
    return 0;
  const juce::File file(fileOrIdentifier);
  juce::int64 stamp = file.getLastModificationTime().toMilliseconds();
  if (file.isDirectory())
    for (const auto& entry : juce::RangedDirectoryIterator(file, true, "*", juce::File::findFiles))
      stamp = std::max(stamp, entry.getModificationTime().toMilliseconds());
  return stamp;
}

// An Audio Unit that is certainly not an effect can be told from its
// identifier ("AudioUnit:Synths/aumu,...") without loading it. Worth doing:
// instruments are the slowest plugins to scan, and none of them is addable.
bool isAudioUnitNonEffect(const juce::String& identifier) {
  return identifier.startsWith("AudioUnit:Synths/") ||
         identifier.startsWith("AudioUnit:Generators/");
}

juce::String displayNameFor(juce::AudioPluginFormat* format, const juce::String& formatName,
                            const juce::String& fileOrIdentifier) {
  if (format != nullptr) {
    const juce::String name = format->getNameOfPluginFromIdentifier(fileOrIdentifier);
    if (name.isNotEmpty())
      return name;
  }
  return isAudioUnit(formatName) ? fileOrIdentifier
                                 : juce::File(fileOrIdentifier).getFileNameWithoutExtension();
}

}  // namespace

// ####################
// FORMATS AND IDENTITY
// ####################
juce::AudioPluginFormatManager& hostedPluginFormats() {
  // The full formats, not the headless ones: an instance made here can open
  // its editor once the UI asks for it.
  static juce::AudioPluginFormatManager manager;
  static std::once_flag once;
  std::call_once(once, [] {
#if JUCE_PLUGINHOST_VST3 && (JUCE_MAC || JUCE_WINDOWS || JUCE_LINUX || JUCE_BSD)
    manager.addFormat(std::make_unique<juce::VST3PluginFormat>());
#endif
#if JUCE_PLUGINHOST_AU && JUCE_MAC
    manager.addFormat(std::make_unique<juce::AudioUnitPluginFormat>());
#endif
  });
  return manager;
}

HostedPluginIdentity HostedPluginIdentity::fromDescription(
    const juce::PluginDescription& description) {
  HostedPluginIdentity identity;
  identity.format = description.pluginFormatName;
  identity.fileOrIdentifier = description.fileOrIdentifier;
  identity.uniqueId = description.uniqueId;
  identity.deprecatedUid = description.deprecatedUid;
  identity.name = description.name;
  identity.vendor = description.manufacturerName;
  identity.version = description.version;
  return identity;
}

HostedPluginIdentity HostedPluginIdentity::fromValueTree(const juce::ValueTree& node) {
  HostedPluginIdentity identity;
  identity.format = node.getProperty("format").toString();
  identity.fileOrIdentifier = node.getProperty("file").toString();
  identity.uniqueId = static_cast<int>(node.getProperty("uid", 0));
  identity.deprecatedUid = static_cast<int>(node.getProperty("deprecatedUid", 0));
  identity.name = node.getProperty("name").toString();
  identity.vendor = node.getProperty("vendor").toString();
  identity.version = node.getProperty("version").toString();
  return identity;
}

juce::ValueTree HostedPluginIdentity::toValueTree() const {
  juce::ValueTree node("Plugin");
  node.setProperty("format", format, nullptr);
  node.setProperty("uid", uniqueId, nullptr);
  node.setProperty("deprecatedUid", deprecatedUid, nullptr);
  node.setProperty("file", fileOrIdentifier, nullptr);
  node.setProperty("name", name, nullptr);
  node.setProperty("vendor", vendor, nullptr);
  node.setProperty("version", version, nullptr);
  return node;
}

juce::var HostedPluginIdentity::toVar() const {
  juce::DynamicObject::Ptr object = new juce::DynamicObject();
  object->setProperty("name", name);
  object->setProperty("vendor", vendor);
  object->setProperty("format", format);
  object->setProperty("version", version);
  object->setProperty("file", fileOrIdentifier);
  return juce::var(object.get());
}

bool HostedPluginIdentity::samePluginAs(const HostedPluginIdentity& other) const {
  return format == other.format && uniqueId == other.uniqueId && name == other.name;
}

// ####################
// SCANNER
// ####################
PluginScanner& PluginScanner::instance() {
  static PluginScanner scanner;
  return scanner;
}

PluginScanner::PluginScanner() : juce::Thread("Plugin Scan") {}

PluginScanner::~PluginScanner() { stopThread(5000); }

const char* PluginScanner::statusToString(PluginStatus status) {
  switch (status) {
    case PluginStatus::failed: return "failed scan";
    case PluginStatus::notEffect: return "not an effect";
    case PluginStatus::missing: return "missing";
    case PluginStatus::ok: break;
  }
  return "ok";
}

const char* PluginScanner::stateToString(ScanState state) {
  switch (state) {
    case ScanState::scanning: return "scanning";
    case ScanState::finished: return "finished";
    case ScanState::cancelled: return "cancelled";
    case ScanState::unavailable: return "unavailable";
    case ScanState::idle: break;
  }
  return "idle";
}

juce::File PluginScanner::setCacheDirForTesting(const juce::File& dir) {
  PluginScanner& scanner = instance();
  scanner.cancelScan();

  juce::File previous;
  {
    const std::lock_guard<std::mutex> guard(overrideMutex);
    previous = cacheDirOverride;
    cacheDirOverride = dir;
  }
  {
    juce::ScopedLock scoped(scanner.lock);
    scanner.records.clear();
    scanner.folders.clear();
    scanner.known.clear();
    scanner.scanStatus = {};
    scanner.loaded = false;
  }
  scanner.generationCounter.fetch_add(1);
  return previous;
}

juce::File PluginScanner::setHelperForTesting(const juce::File& helper) {
  const std::lock_guard<std::mutex> guard(overrideMutex);
  const juce::File previous = helperOverride;
  helperOverride = helper;
  return previous;
}

void PluginScanner::setDefaultLocationsEnabled(bool enabled) {
  defaultLocationsEnabled.store(enabled);
}

void PluginScanner::setScanTimeoutMs(int timeoutMs) {
  scanTimeoutMs.store(juce::jmax(100, timeoutMs));
}

juce::File PluginScanner::cacheFile() const { return cacheDir().getChildFile(kCacheFileName); }

juce::File PluginScanner::helperFile() const {
  {
    const std::lock_guard<std::mutex> guard(overrideMutex);
    if (helperOverride != juce::File())
      return helperOverride;
  }

  // currentExecutableFile is this module even inside a host: the plugin
  // binary, not the DAW. The helper sits beside it; in a macOS bundle
  // (Contents/MacOS/<binary>) it is in Contents/Helpers.
  const juce::File moduleDir =
      juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
  const juce::File candidates[] = {
      moduleDir.getChildFile(kHelperName),
      moduleDir.getSiblingFile("Helpers").getChildFile(kHelperName),
  };
  for (const auto& candidate : candidates)
    if (candidate.existsAsFile())
      return candidate;
  return candidates[0];
}

// ---- Cache ------------------------------------------------------------------
//
//   <PedaleiraPluginCache version="1">
//     <Folder path="..."/>                     the user's folders
//     <KNOWNPLUGINS> <PLUGIN .../> </KNOWNPLUGINS>   the addable effects, as
//                                              juce::KnownPluginList writes them
//     <Record format file stamp present failure>     one per scanned file
//       <PLUGIN .../>                          everything it holds
//     </Record>
//   </PedaleiraPluginCache>
//
// The records are what the scanner works from: they also know the files that
// failed, the plugins that are not effects, and which file a plugin came out
// of (a description's own file can be a path inside the bundle that was
// scanned). The KnownPluginList is rebuilt from them; a cache that only has
// the list (written by something else) is read as plugins still to be
// confirmed by a scan.

void PluginScanner::ensureLoaded() const {
  // Logically const: the cache is read on first use, whoever asks first.
  auto& self = const_cast<PluginScanner&>(*this);
  juce::ScopedLock scoped(lock);
  if (loaded)
    return;
  loaded = true;
  self.loadCache();
}

void PluginScanner::loadCache() {
  records.clear();
  folders.clear();
  known.clear();

  const std::unique_ptr<juce::XmlElement> root = juce::parseXML(cacheFile());
  if (root == nullptr || !root->hasTagName("PedaleiraPluginCache") ||
      root->getIntAttribute("version", 0) > kCacheVersion)
    return;

  for (const auto* folder : root->getChildWithTagNameIterator("Folder"))
    folders.addIfNotAlreadyThere(folder->getStringAttribute("path"));

  for (const auto* recordXml : root->getChildWithTagNameIterator("Record")) {
    Record record;
    record.format = recordXml->getStringAttribute("format");
    record.fileOrIdentifier = recordXml->getStringAttribute("file");
    record.stamp = recordXml->getStringAttribute("stamp").getLargeIntValue();
    record.present = recordXml->getBoolAttribute("present", true);
    record.failure = recordXml->getStringAttribute("failure");
    for (const auto* pluginXml : recordXml->getChildWithTagNameIterator("PLUGIN")) {
      juce::PluginDescription description;
      if (description.loadFromXml(*pluginXml))
        record.plugins.add(description);
    }
    records.push_back(std::move(record));
  }

  // Only a plugin list: usable as it stands, and scanned at the next scan
  // (a stamp no file has).
  if (records.empty())
    if (const auto* knownXml = root->getChildByName("KNOWNPLUGINS")) {
      juce::KnownPluginList cached;
      cached.recreateFromXml(*knownXml);
      for (const auto& plugin : cached.getTypes()) {
        Record* record = findRecord(plugin.pluginFormatName, plugin.fileOrIdentifier);
        if (record == nullptr) {
          records.push_back({plugin.pluginFormatName, plugin.fileOrIdentifier, -1, true, {}, {}});
          record = &records.back();
        }
        record->plugins.add(plugin);
      }
    }
  rebuildKnownList();
}

void PluginScanner::saveCache() const {
  juce::XmlElement root("PedaleiraPluginCache");
  root.setAttribute("version", kCacheVersion);
  for (const auto& folder : folders)
    root.createNewChildElement("Folder")->setAttribute("path", folder);

  root.addChildElement(known.createXml().release());

  for (const auto& record : records) {
    auto* recordXml = root.createNewChildElement("Record");
    recordXml->setAttribute("format", record.format);
    recordXml->setAttribute("file", record.fileOrIdentifier);
    recordXml->setAttribute("stamp", juce::String(record.stamp));
    recordXml->setAttribute("present", record.present);
    if (record.failure.isNotEmpty())
      recordXml->setAttribute("failure", record.failure);
    for (const auto& plugin : record.plugins)
      recordXml->addChildElement(plugin.createXml().release());
  }

  const juce::File file = cacheFile();
  file.getParentDirectory().createDirectory();
  if (!root.writeTo(file))
    juce::Logger::writeToLog("[PluginScan] Could not write " + file.getFullPathName());
}

void PluginScanner::rebuildKnownList() {
  known.clear();
  for (const auto& record : records)
    if (record.present && record.failure.isEmpty())
      for (const auto& plugin : record.plugins)
        if (whyNotAnEffect(plugin).isEmpty())
          known.addType(plugin);
}

PluginScanner::Record* PluginScanner::findRecord(const juce::String& format,
                                                 const juce::String& fileOrIdentifier) {
  for (auto& record : records)
    if (record.format == format && record.fileOrIdentifier == fileOrIdentifier)
      return &record;
  return nullptr;
}

// ---- Listing ------------------------------------------------------------------

std::vector<PluginScanner::ListedPlugin> PluginScanner::list() const {
  ensureLoaded();
  std::vector<ListedPlugin> out;
  {
    juce::ScopedLock scoped(lock);
    for (const auto& record : records) {
      if (record.failure.isNotEmpty()) {
        ListedPlugin listed;
        listed.description.pluginFormatName = record.format;
        listed.description.fileOrIdentifier = record.fileOrIdentifier;
        listed.description.name =
            displayNameFor(nullptr, record.format, record.fileOrIdentifier);
        listed.status = record.present ? PluginStatus::failed : PluginStatus::missing;
        listed.reason = record.failure;
        out.push_back(std::move(listed));
        continue;
      }
      for (const auto& plugin : record.plugins) {
        ListedPlugin listed;
        listed.description = plugin;
        const juce::String notEffect = whyNotAnEffect(plugin);
        if (!record.present) {
          listed.status = PluginStatus::missing;
          listed.reason = "not found by the last scan";
        } else if (notEffect.isNotEmpty()) {
          listed.status = PluginStatus::notEffect;
          listed.reason = notEffect;
        }
        out.push_back(std::move(listed));
      }
    }
  }
  std::sort(out.begin(), out.end(), [](const ListedPlugin& a, const ListedPlugin& b) {
    const int byName = a.description.name.compareIgnoreCase(b.description.name);
    if (byName != 0)
      return byName < 0;
    return a.description.createIdentifierString() < b.description.createIdentifierString();
  });
  return out;
}

juce::Array<juce::PluginDescription> PluginScanner::knownEffects() const {
  ensureLoaded();
  juce::ScopedLock scoped(lock);
  return known.getTypes();
}

std::optional<juce::PluginDescription> PluginScanner::findEffect(const juce::String& key,
                                                                 juce::String* error) const {
  const auto fail = [error](const juce::String& message) -> std::optional<juce::PluginDescription> {
    if (error != nullptr)
      *error = message;
    return std::nullopt;
  };
  if (key.isEmpty())
    return fail("no plugin named");

  const std::vector<ListedPlugin> all = list();
  const auto pick = [&](const std::vector<const ListedPlugin*>& matches)
      -> std::optional<juce::PluginDescription> {
    std::vector<const ListedPlugin*> usable;
    for (const auto* match : matches)
      if (match->status == PluginStatus::ok)
        usable.push_back(match);
    if (usable.size() == 1)
      return usable.front()->description;
    if (usable.size() > 1)
      return fail("'" + key + "' names " + juce::String(static_cast<int>(usable.size())) +
                  " plugins; use an identifier from plugins.list");
    const ListedPlugin& first = *matches.front();
    return fail("plugin '" + first.description.name + "' cannot be added: " +
                statusToString(first.status) +
                (first.reason.isNotEmpty() ? " (" + first.reason + ")" : juce::String()));
  };

  // An identifier from the list, then a file path, then a name.
  std::vector<const ListedPlugin*> matches;
  for (const auto& listed : all)
    if (listed.description.createIdentifierString() == key)
      matches.push_back(&listed);
  if (matches.empty() && juce::File::isAbsolutePath(key)) {
    // The file itself, or the bundle it sits in (on Windows a description
    // names the binary inside the .vst3 folder).
    const juce::File wanted(key);
    for (const auto& listed : all) {
      const juce::File file(listed.description.fileOrIdentifier);
      if (!isAudioUnit(listed.description.pluginFormatName) &&
          (file == wanted || file.isAChildOf(wanted)))
        matches.push_back(&listed);
    }
  }
  if (matches.empty())
    for (const auto& listed : all)
      if (listed.description.fileOrIdentifier == key ||
          listed.description.name.equalsIgnoreCase(key))
        matches.push_back(&listed);
  if (matches.empty())
    return fail("no scanned plugin matches '" + key + "' (see plugins.list)");
  return pick(matches);
}

std::optional<juce::PluginDescription>
PluginScanner::findEffect(const HostedPluginIdentity& identity) const {
  // Known is not enough: a file deleted since the last scan would only fail
  // later, at instantiation. One stat per candidate settles it here.
  juce::Array<juce::PluginDescription> effects = knownEffects();
  effects.removeIf([](const juce::PluginDescription& effect) {
    return !isAudioUnit(effect.pluginFormatName) && !juce::File(effect.fileOrIdentifier).exists();
  });
  for (const auto& effect : effects)
    if (effect.pluginFormatName == identity.format &&
        effect.fileOrIdentifier == identity.fileOrIdentifier &&
        (effect.uniqueId == identity.uniqueId || effect.name == identity.name))
      return effect;
  // The same plugin somewhere else: a plugin folder that moved, a rig made
  // on another machine.
  for (const auto& effect : effects)
    if (HostedPluginIdentity::fromDescription(effect).samePluginAs(identity))
      return effect;
  return std::nullopt;
}

// ---- Folders ------------------------------------------------------------------

juce::StringArray PluginScanner::userFolders() const {
  ensureLoaded();
  juce::ScopedLock scoped(lock);
  return folders;
}

bool PluginScanner::addFolder(const juce::File& folder) {
  ensureLoaded();
  juce::ScopedLock scoped(lock);
  if (folders.contains(folder.getFullPathName()))
    return false;
  folders.add(folder.getFullPathName());
  saveCache();
  return true;
}

bool PluginScanner::removeFolder(const juce::File& folder) {
  ensureLoaded();
  juce::ScopedLock scoped(lock);
  const int index = folders.indexOf(folder.getFullPathName());
  if (index < 0)
    return false;
  folders.remove(index);
  saveCache();
  return true;
}

// ---- Scanning -----------------------------------------------------------------

PluginScanner::ScanStatus PluginScanner::status() const {
  juce::ScopedLock scoped(lock);
  return scanStatus;
}

bool PluginScanner::startScan(const ScanOptions& options) {
  cancelScan();
  ensureLoaded();

  juce::ScopedLock scoped(lock);
  scanStatus = {};
  if (!isAvailable()) {
    // Never in-process: the whole point of the helper is that a plugin
    // cannot take this process down.
    scanStatus.state = ScanState::unavailable;
    juce::Logger::writeToLog("[PluginScan] Scan helper not found at " +
                             helperFile().getFullPathName() + "; scanning is unavailable");
    return false;
  }
  pendingOptions = options;
  scanStatus.state = ScanState::scanning;
  startThread();
  return true;
}

void PluginScanner::cancelScan() {
  if (isThreadRunning())
    stopThread(10000);
}

bool PluginScanner::waitForScan(int timeoutMs) {
  return waitForThreadToExit(timeoutMs);
}

bool PluginScanner::scanCandidate(const Candidate& candidate, juce::int64 stamp, Record& result) {
  result.format = candidate.format->getName();
  result.fileOrIdentifier = candidate.fileOrIdentifier;
  result.stamp = stamp;
  result.present = true;
  result.failure.clear();
  result.plugins.clear();

  if (isAudioUnit(result.format) && isAudioUnitNonEffect(result.fileOrIdentifier)) {
    juce::PluginDescription description;
    description.pluginFormatName = result.format;
    description.fileOrIdentifier = result.fileOrIdentifier;
    description.name = displayNameFor(candidate.format, result.format, result.fileOrIdentifier);
    description.isInstrument = result.fileOrIdentifier.startsWith("AudioUnit:Synths/");
    result.plugins.add(description);
    return true;
  }

#if JUCE_IOS
  result.failure = "scanning is not available on this platform";
  return true;
#else
  // One temp file per run, next to the cache: the helper writes its whole
  // result there at the very end, so a file that is missing afterwards
  // means the helper did not get that far.
  const juce::File outFile =
      cacheDir().getChildFile("scan-" + juce::Uuid().toString() + ".xml");
  outFile.getParentDirectory().createDirectory();
  const struct Cleanup {
    const juce::File& file;
    ~Cleanup() {
      file.deleteFile();
      file.getSiblingFile(file.getFileName() + ".part").deleteFile();
    }
  } cleanup{outFile};

  juce::StringArray command;
  command.add(helperFile().getFullPathName());
  command.add(result.format);
  command.add(result.fileOrIdentifier);
  command.add("--out");
  command.add(outFile.getFullPathName());

  // No stream flags: the child's output goes nowhere, so a chatty plugin
  // can never fill a pipe nobody reads and stall its own scan.
  juce::ChildProcess helper;
  if (!helper.start(command, 0)) {
    result.failure = "the scan helper could not be started";
    return true;
  }

  const int timeout = scanTimeoutMs.load();
  const juce::uint32 started = juce::Time::getMillisecondCounter();
  while (helper.isRunning()) {
    if (threadShouldExit()) {
      helper.kill();
      return false;
    }
    if (juce::Time::getMillisecondCounter() - started > static_cast<juce::uint32>(timeout)) {
      helper.kill();
      result.failure = "timed out after " + juce::String(timeout / 1000.0, 1) + " s";
      return true;
    }
    juce::Thread::sleep(5);
  }

  const std::unique_ptr<juce::XmlElement> root = juce::parseXML(outFile);
  if (root == nullptr || !root->hasTagName("PedaleiraPluginScan") ||
      root->getIntAttribute("complete", 0) != 1) {
    result.failure = "crashed while being scanned";
    return true;
  }
  for (const auto* pluginXml : root->getChildWithTagNameIterator("PLUGIN")) {
    juce::PluginDescription description;
    if (description.loadFromXml(*pluginXml))
      result.plugins.add(description);
  }
  if (result.plugins.isEmpty())
    result.failure = "no plugin found in it";
  return true;
#endif
}

void PluginScanner::run() {
  ScanOptions options;
  juce::StringArray userFolderList;
  {
    juce::ScopedLock scoped(lock);
    options = pendingOptions;
    userFolderList = folders;
  }

  // What to look at. File formats are searched in folders; Audio Units are
  // asked of the system and have none.
  const bool explicitFiles = !options.files.isEmpty();
  const bool explicitFolders = !options.folders.isEmpty();
  std::vector<Candidate> candidates;
  juce::StringArray scannedFormats;
  for (auto* format : hostedPluginFormats().getFormats()) {
    const juce::String name = format->getName();
    if (!options.formats.isEmpty() && !options.formats.contains(name))
      continue;

    if (explicitFiles) {
      for (const auto& file : options.files)
        if (format->fileMightContainThisPluginType(file))
          candidates.push_back({format, file});
      continue;
    }

    juce::FileSearchPath path;
    if (isAudioUnit(name)) {
      const bool wanted = explicitFolders ? options.formats.contains(name)
                                          : defaultLocationsEnabled.load();
      if (!wanted)
        continue;
    } else if (explicitFolders) {
      for (const auto& folder : options.folders)
        path.addIfNotAlreadyThere(juce::File(folder));
    } else {
      if (defaultLocationsEnabled.load())
        path = format->getDefaultLocationsToSearch();
      for (const auto& folder : userFolderList)
        path.addIfNotAlreadyThere(juce::File(folder));
    }

    scannedFormats.add(name);
    for (const auto& found : format->searchPathsForPlugins(path, true, false))
      candidates.push_back({format, found});
    if (threadShouldExit())
      break;
  }

  {
    juce::ScopedLock scoped(lock);
    scanStatus.total = static_cast<int>(candidates.size());
  }

  bool cancelled = threadShouldExit();
  for (const auto& candidate : candidates) {
    if (cancelled || threadShouldExit()) {
      cancelled = true;
      break;
    }
    const juce::String formatName = candidate.format->getName();
    const juce::int64 stamp = stampOf(formatName, candidate.fileOrIdentifier);

    bool upToDate = false;
    {
      juce::ScopedLock scoped(lock);
      scanStatus.currentFile = candidate.fileOrIdentifier;
      if (Record* record = findRecord(formatName, candidate.fileOrIdentifier))
        if (!options.rescanAll && record->stamp == stamp) {
          // Unchanged since it was scanned, whatever the verdict was: a
          // blacklisted file stays blacklisted until it changes.
          record->present = true;
          upToDate = true;
        }
    }

    Record scanned;
    bool failed = false;
    if (!upToDate) {
      if (!scanCandidate(candidate, stamp, scanned)) {
        cancelled = true;
        break;
      }
      failed = scanned.failure.isNotEmpty();
      if (failed)
        juce::Logger::writeToLog("[PluginScan] Blacklisted " + candidate.fileOrIdentifier +
                                 ": " + scanned.failure);
    }

    juce::ScopedLock scoped(lock);
    if (!upToDate) {
      if (Record* record = findRecord(formatName, candidate.fileOrIdentifier))
        *record = std::move(scanned);
      else
        records.push_back(std::move(scanned));
      ++scanStatus.scanned;
      if (failed)
        ++scanStatus.failed;
    }
    ++scanStatus.done;
  }

  juce::ScopedLock scoped(lock);
  if (!cancelled && !explicitFiles) {
    // What this scan should have found and did not is missing: a file that
    // is gone, or (for a scan of the usual places) one that is no longer
    // under any searched folder. A scan of explicit folders only speaks for
    // what lives inside them.
    const auto wasCandidate = [&](const Record& record) {
      for (const auto& candidate : candidates)
        if (candidate.format->getName() == record.format &&
            candidate.fileOrIdentifier == record.fileOrIdentifier)
          return true;
      return false;
    };
    const auto inScannedFolders = [&](const Record& record) {
      const juce::File file(record.fileOrIdentifier);
      for (const auto& folder : options.folders)
        if (file.isAChildOf(juce::File(folder)))
          return true;
      return false;
    };
    for (auto& record : records) {
      if (!scannedFormats.contains(record.format) || wasCandidate(record))
        continue;
      if (isAudioUnit(record.format) || !explicitFolders || inScannedFolders(record) ||
          !juce::File(record.fileOrIdentifier).exists())
        record.present = false;
    }
  }
  rebuildKnownList();
  saveCache();
  scanStatus.currentFile.clear();
  scanStatus.state = cancelled ? ScanState::cancelled : ScanState::finished;
  generationCounter.fetch_add(1);
}

#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <optional>
#include <vector>

// The plugin formats this build can host: VST3 on the desktop platforms,
// Audio Units on macOS. One manager for the process; instances are created
// through it on the message thread (see HostedPluginBlockProcessor.h).
juce::AudioPluginFormatManager& hostedPluginFormats();

// Which plugin a hosted block is, as saved with the block: enough to find the
// plugin again on this or another machine, and to name the block when it
// cannot be found. Written as the "Plugin" child of the block's KindState.
struct HostedPluginIdentity {
  juce::String format;            // "VST3", "AudioUnit"
  juce::String fileOrIdentifier;  // a path (VST3) or a component id (AU)
  int uniqueId{0};                // juce::PluginDescription::uniqueId
  int deprecatedUid{0};
  juce::String name;
  juce::String vendor;
  juce::String version;

  static HostedPluginIdentity fromDescription(const juce::PluginDescription& description);
  static HostedPluginIdentity fromValueTree(const juce::ValueTree& pluginNode);
  juce::ValueTree toValueTree() const;
  // { name, vendor, format, version, file }
  juce::var toVar() const;

  // The same plugin, wherever its file lives: format, unique id and name.
  bool samePluginAs(const HostedPluginIdentity& other) const;
};

/**
 * Finds the user's plugins without ever loading one into this process.
 *
 * Scanning a plugin means running its code, and a broken plugin can crash or
 * hang while it is merely being looked at. So every candidate is scanned by
 * a helper executable (PedaleiraPluginScan, tools/plugin_scan.cpp), one process
 * per plugin, with a timeout. A helper that dies or overruns costs nothing
 * but an entry in the blacklist, with the reason. If the helper is not
 * installed the scanner reports itself unavailable; it never falls back to
 * scanning in-process.
 *
 * Results are kept per file: the plugins it holds, or why its scan failed,
 * with the file's modification stamp, so the next scan only runs the helper
 * for files that are new or changed. Effects that scanned cleanly also live
 * in a juce::KnownPluginList. Everything is cached in one XML file in the
 * app-data folder and read back on first use.
 *
 * Only effects are offered to a chain: instruments and plugins without an
 * audio input and output are listed as "not an effect" and cannot be added.
 *
 * One scanner per process (several plugin instances in a DAW share it).
 * Every method may be called from any thread except the audio thread; the
 * scan itself runs on the scanner's own thread.
 */
class PluginScanner final : private juce::Thread {
public:
  static PluginScanner& instance();
  ~PluginScanner() override;

  // ---- What was found -----------------------------------------------------

  enum class PluginStatus {
    ok,         // an effect that scanned cleanly: can be added to a chain
    failed,     // the scan crashed, timed out or found nothing (blacklisted)
    notEffect,  // an instrument, a generator or a MIDI-only plugin
    missing     // known from an earlier scan, no longer on disk or in a scanned folder
  };
  static const char* statusToString(PluginStatus status);

  struct ListedPlugin {
    // For a failed scan only the format, the file and a name derived from
    // the file are filled in.
    juce::PluginDescription description;
    PluginStatus status{PluginStatus::ok};
    // Why it is not ok; empty for ok.
    juce::String reason;
  };

  // Everything the scanner knows about, sorted by name.
  std::vector<ListedPlugin> list() const;

  // The effects that can be added, as the KnownPluginList holds them.
  juce::Array<juce::PluginDescription> knownEffects() const;

  // The addable effect that `key` names: an identifier from list()
  // (juce::PluginDescription::createIdentifierString), or, for a caller that
  // cannot list first, a plugin's file path or its exact name when that
  // names exactly one effect. `error` says why there is none.
  std::optional<juce::PluginDescription> findEffect(const juce::String& key,
                                                    juce::String* error = nullptr) const;
  // The addable effect a saved block names: the same file first, then the
  // same plugin at another path (a plugin folder that moved).
  std::optional<juce::PluginDescription> findEffect(const HostedPluginIdentity& identity) const;

  // Goes up whenever the set of addable effects may have changed (a scan
  // ended, the cache was redirected). A processor compares it to decide
  // whether a block waiting for a missing plugin is worth another try.
  juce::uint32 generation() const noexcept { return generationCounter.load(); }

  // ---- Scanning -----------------------------------------------------------

  struct ScanOptions {
    // Scan these folders only, instead of the formats' default locations
    // plus the user's folders. A one-off: nothing outside them is touched,
    // and Audio Units (which have no folders) are skipped unless `formats`
    // names them.
    juce::StringArray folders;
    // Scan exactly these plugin files or identifiers (a .vst3 path, an
    // "AudioUnit:Effects/aufx,dely,appl" identifier) and search nowhere.
    // Takes precedence over `folders`.
    juce::StringArray files;
    // Format names to scan ("VST3", "AudioUnit"); empty = every format.
    juce::StringArray formats;
    // Run the helper again for files whose stamp has not changed, the
    // blacklisted ones included.
    bool rescanAll{false};
  };

  enum class ScanState { idle, scanning, finished, cancelled, unavailable };
  static const char* stateToString(ScanState state);

  struct ScanStatus {
    ScanState state{ScanState::idle};
    // The file or identifier the helper is looking at right now.
    juce::String currentFile;
    int done{0};
    int total{0};
    // Of `done`: how many needed the helper (the rest were unchanged), and
    // how many of those failed.
    int scanned{0};
    int failed{0};
  };

  // Starts a scan, replacing one that is running. False, with the state set
  // to unavailable, when the helper executable is missing.
  bool startScan(const ScanOptions& options);
  // Stops a running scan after the plugin it is on (the helper is killed)
  // and returns once the scan thread has gone. What was scanned is kept.
  void cancelScan();
  ScanStatus status() const;
  // Blocks until no scan is running; false on timeout.
  bool waitForScan(int timeoutMs);

  // The helper executable, or a non-existent file when it was not found.
  juce::File helperFile() const;
  bool isAvailable() const { return helperFile().existsAsFile(); }

  // ---- The user's folders -------------------------------------------------
  // Searched on top of the formats' default locations. Saved with the cache.

  juce::StringArray userFolders() const;
  // False when the folder was already there (add) or was not (remove).
  bool addFolder(const juce::File& folder);
  bool removeFolder(const juce::File& folder);

  // ---- Tests and tools ----------------------------------------------------

  // Where the cache file lives (default: the app-data folder). Redirecting
  // drops everything in memory and reads the new location on next use, so a
  // test or the render tool never sees or touches the user's real cache. An
  // empty File restores the default. Returns the previous override.
  static juce::File setCacheDirForTesting(const juce::File& dir);
  // Stand-in for the helper lookup; a file that does not exist makes the
  // scanner unavailable. An empty File restores the lookup.
  static juce::File setHelperForTesting(const juce::File& helper);
  // Off: a scan without explicit folders only searches the user's folders,
  // never the formats' default locations or the system's Audio Unit list.
  // Tests and tools turn it off so they never scan the machine's plugins.
  static void setDefaultLocationsEnabled(bool enabled);
  // How long one helper run may take before it is killed and the plugin
  // blacklisted (default 45 s).
  static void setScanTimeoutMs(int timeoutMs);

  juce::File cacheFile() const;

private:
  PluginScanner();
  void run() override;

  struct Record {
    juce::String format;
    juce::String fileOrIdentifier;
    // The file's modification stamp when it was scanned (0 for an Audio
    // Unit, which is not a file).
    juce::int64 stamp{0};
    // Found by the last scan that looked where it lives.
    bool present{true};
    // Not empty: the scan failed, and this is why.
    juce::String failure;
    juce::Array<juce::PluginDescription> plugins;
  };

  struct Candidate {
    juce::AudioPluginFormat* format{nullptr};
    juce::String fileOrIdentifier;
  };

  void ensureLoaded() const;
  void loadCache();
  void saveCache() const;
  void rebuildKnownList();
  Record* findRecord(const juce::String& format, const juce::String& fileOrIdentifier);
  // Runs the helper on one candidate. False when the scan was cancelled.
  bool scanCandidate(const Candidate& candidate, juce::int64 stamp, Record& result);

  mutable juce::CriticalSection lock;
  mutable bool loaded{false};
  std::vector<Record> records;
  juce::StringArray folders;
  juce::KnownPluginList known;
  ScanOptions pendingOptions;
  ScanStatus scanStatus;
  std::atomic<juce::uint32> generationCounter{1};

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginScanner)
};

// The JSON preset format ("pedaleira.preset", version 1) and the asset store
// its binary payloads live in. docs/library.md is the reference; in short:
//
// A preset is the same tree a .t3kpreset holds (PresetFile.h), written as
// text a person can read and git can diff. The mapping is mechanical and
// lossless, so nothing here knows what a block or a scene is: a tree node
// becomes an object with "node" (its type), its properties in their saved
// order, and "children". Two shortcuts keep the top of the file readable: the
// preset's ChainSnapshot is the "chain" key and its faceplate parameters are
// the "params" map.
//
// Binary values never go inline as megabytes of text. Model bytes (NAM
// captures, IRs) always go to the asset store; any other binary value (a
// hosted plugin's state chunk) goes inline as base64 up to
// kInlineBinaryLimit bytes and to the asset store above it. The store is
// content addressed: Assets/<sha256>.<ext>, one file however many presets
// use it.
//
// Depends on juce_core, juce_data_structures and juce_cryptography only.
#pragma once

#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>
#include <juce_data_structures/juce_data_structures.h>

#include <map>
#include <optional>
#include <vector>

namespace t3k::presetjson {

constexpr const char* kFormat = "pedaleira.preset";
constexpr int kVersion = 1;
constexpr const char* kExtension = ".json";
// A binary value that is not model data stays in the preset file up to this
// many bytes. Small enough that a preset stays a text file one can read,
// large enough for the state of most plugins that keep a handful of values.
constexpr int kInlineBinaryLimit = 2048;

// ####################
// FILES
// ####################
// The two things every file of the library goes through: a write that never
// costs the file it replaces, and a read that a hostile or broken file
// cannot bring the process down with.

// The most a preset file may be (one with its assets inline is megabytes of
// base64), the most a small file (library.json, setlist.json) may be, and
// how deep arrays and objects may nest. A real preset nests under twenty
// levels deep.
constexpr juce::int64 kMaxPresetFileBytes = 64 * 1024 * 1024;
constexpr juce::int64 kMaxMetaFileBytes = 1024 * 1024;
constexpr int kMaxJsonDepth = 64;

/** Writes the bytes as `target`, replacing what is there in one step: they
    go to a temporary file beside it, and only once every byte is written and
    flushed does that file take the target's place (File::replaceFileIn, an
    atomic rename). On any failure the target is what it was, the temporary
    file is gone and the result says what went wrong. Never deletes the
    target first. */
juce::Result writeFileAtomically(const juce::File& target, const void* data, size_t size);
juce::Result writeFileAtomically(const juce::File& target, const juce::String& text);

/** JSON::parse behind a check of the nesting depth: JUCE's parser recurses
    once per level without a limit, so a file of a hundred thousand "[" would
    overflow the stack. Brackets inside strings do not count. */
juce::Result parseJsonGuarded(const juce::String& text, juce::var& out);
/** The same with the caller's own limit: the command server allows a request
    far less nesting than a preset file (CommandServer.cpp). */
juce::Result parseJsonGuarded(const juce::String& text, juce::var& out, int maxDepth);

/** The text of a file of at most `maxBytes`; a larger one is not read. */
juce::Result readTextFile(const juce::File& file, juce::int64 maxBytes, juce::String& text);

/** readTextFile and parseJsonGuarded. */
juce::Result readJsonFile(const juce::File& file, juce::int64 maxBytes, juce::var& out);

// Process-wide fault injection for tests: the step of the next file
// operations that fails as a full disk or a locked file would.
enum class FileFault {
  none,
  write,       // writeFileAtomically: the bytes cannot be written
  replace,     // writeFileAtomically: the temporary file cannot take the target's place
  renameHalf,  // SetlistLibrary's renumbering stops between its two passes
};
FileFault setFileFaultForTesting(FileFault fault);
FileFault fileFaultForTesting();

/** A reference to a file of the asset store, as a preset carries it. */
struct AssetRef {
  juce::String hash;  // lower case hex SHA-256 of the bytes
  juce::String ext;   // without the dot: "nam", "wav", "bin"
  juce::String name;  // what the bytes were called when they were stored
  juce::int64 size{0};
};

/**
 * Content-addressed files in one flat folder. Writes are write-then-rename
 * (writeFileAtomically) and a file that is already there and intact is left
 * alone, so several plugin instances (or git) may work in the folder at
 * once. Never deletes.
 * Not for the audio thread.
 */
class AssetStore {
public:
  AssetStore() = default;
  explicit AssetStore(const juce::File& directory) : dir(directory) {}

  juce::File directory() const { return dir; }
  juce::File fileFor(const juce::String& hash, const juce::String& ext) const;

  /** Stores the bytes and fills in `ref`'s hash, ext and size; the name is
      the caller's. A file that is there already is left alone once its
      bytes have been checked against the hash (and written again when they
      are not those bytes). False when the file could not be written. */
  bool put(const void* data, size_t size, const juce::String& ext, AssetRef& ref) const;

  enum class Status { ok, missing, corrupt };
  /** The bytes behind `ref`. `corrupt` when the file is there but is not
      what the reference names (another size, or another hash). */
  Status get(const AssetRef& ref, juce::MemoryBlock& out) const;

  /** Copies the file behind `ref` from `other` into this store. */
  bool adopt(const AssetStore& other, const AssetRef& ref) const;

  static juce::String hashOf(const void* data, size_t size);
  /** Lower case letters and digits, at most eight; "bin" for anything else. */
  static juce::String cleanExtension(const juce::String& ext);

private:
  /** True when `file` is there and is the bytes `ref` names (hashed, unless
      it has been since it last changed). */
  bool holds(const juce::File& file, const AssetRef& ref) const;
  bool isVerified(const juce::File& file) const;
  void noteVerified(const juce::File& file) const;

  juce::File dir;
  // A file that has passed the hash check is not hashed again until it
  // changes: switching presets on stage must not re-read megabytes to check
  // what the last switch already checked.
  struct Verified {
    juce::int64 modificationMs{0};
    juce::int64 size{0};
  };
  mutable juce::CriticalSection verifiedLock;
  mutable std::map<juce::String, Verified> verified;  // file name -> what was checked
};

/** What a preset file says about itself, read without touching any asset. */
struct Meta {
  bool valid{false};
  juce::String id;    // may be empty (a file written by hand)
  juce::String name;  // may be empty
  // Scenes a player would call used: the active one, the named ones and the
  // ones with a block switched their own way. 1 for a rig without scenes.
  int sceneCount{1};
  std::optional<double> tempo;  // only when the file stores one
  juce::StringArray tags;
  juce::String problem;  // why it is not valid
};

struct WriteOptions {
  // Where binary payloads go. Null: every one goes inline as base64, which
  // gives one self-contained (and large) file.
  const AssetStore* assets{nullptr};
};

struct ReadOptions {
  // Asked in order for each referenced asset.
  std::vector<const AssetStore*> assets;
};

struct ReadResult {
  juce::ValueTree preset;  // invalid when the text is not a preset
  juce::String error;
  // Assets the file names that could not be read (missing, or not the bytes
  // the reference names). The preset is still valid: see kMissingAssetPrefix.
  std::vector<AssetRef> missing;
};

/** The preset tree (type "T3KPreset") as JSON text. Empty with `error` set
    when the tree is not a preset or an asset could not be stored. The text
    of a given tree is always the same bytes. */
juce::String toJson(const juce::ValueTree& preset, const WriteOptions& options,
                    juce::String& error);

/** The tree a JSON preset describes. */
ReadResult fromJson(const juce::String& text, const ReadOptions& options);
/** The same from a file of at most kMaxPresetFileBytes. */
ReadResult fromJsonFile(const juce::File& file, const ReadOptions& options);

/** Parses `text` only as far as its identity and summary. */
Meta readMeta(const juce::String& text);
/** The same from a file of at most kMaxPresetFileBytes. */
Meta readMetaFile(const juce::File& file);
Meta metaOf(const juce::var& parsed);

/** The canonical text of a JSON value: two space indent, keys in the order
    the object holds them, small flat objects on one line, "\n" line ends, a
    final newline. Numbers are written so that reading them back gives the
    same value and the same kind (a double keeps a decimal point). */
juce::String writeCanonical(const juce::var& value);

/** Every asset a parsed preset references. */
std::vector<AssetRef> referencedAssets(const juce::var& parsed);

// A binary value whose asset could not be read is put in the tree as a
// string: this prefix followed by the reference. Writing the tree back as
// JSON turns it into the same reference again, so a preset with a missing
// asset can be loaded, moved and saved without losing what it pointed at.
constexpr const char* kMissingAssetPrefix = "pedaleira-missing-asset:";
bool isMissingAsset(const juce::var& value);
std::optional<AssetRef> missingAssetOf(const juce::var& value);
/** True when any property of `node` or of its descendants is one. */
bool hasMissingAsset(const juce::ValueTree& node);

}  // namespace t3k::presetjson

#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "PresetJson.h"

/**
 * The preset library as folders of JSON files, made to be backed up and
 * versioned with git (docs/library.md is the user-facing description).
 *
 *   <root>/library.json                      favourites, recents, position, setlist order
 *   <root>/Setlists/<Setlist>/setlist.json   the setlist's id and name
 *   <root>/Setlists/<Setlist>/012 Glass Cleans.json
 *   <root>/Assets/<sha256>.<ext>             model files and large plugin states
 *
 * Pure file layer, like PresetManager: it moves preset trees in and out and
 * knows nothing of what is in them (PresetJson.h does the format). The
 * factory presets are not here; the processor adds them as a read-only
 * setlist of its own.
 *
 * The folders are the truth. Nothing is kept between calls but a cache of
 * what each file said, keyed on its time and size, so files a person or git
 * added, removed, renamed or edited are simply what the next call sees. A
 * scan never writes: a file that is not a preset is listed as ignored and
 * left alone, a preset without a slot number or with a number another file
 * also claims is given a free slot in the listing, and its file keeps its
 * name until an edit of that slot (or tidy()) renames it.
 *
 * Identity. A preset's id is the "id" inside its file, exposed as
 * "user:<id>"; the slot and the name live in the file name, which is a view
 * of them. A setlist's id is the "id" in its setlist.json. Files written by
 * hand without one get an id derived from their name, replaced by a real one
 * the first time the library writes the file.
 *
 * Slots are numbered 1 to 256, as the file names show them.
 *
 * Any thread but the audio thread; calls are serialised.
 */
class SetlistLibrary {
public:
  static constexpr int kMaxSlots = 256;
  static constexpr int kMaxRecents = 32;
  static constexpr int kMaxNameBytes = 100;
  static constexpr const char* kIdPrefix = "user:";
  static constexpr const char* kSetlistFormat = "pedaleira.setlist";
  static constexpr const char* kLibraryFormat = "pedaleira.library";

  struct Setlist {
    juce::String id;
    juce::String name;
    juce::File folder;
  };

  struct Slot {
    int slot{0};        // 1 to kMaxSlots
    juce::String id;    // "user:<id>"
    juce::String name;
    juce::File file;
    int sceneCount{1};
    std::optional<double> tempo;
    juce::StringArray tags;
    // False when the file is not called what its slot and name make it (no
    // number, a number already taken, a name edited inside the file).
    bool tidy{true};
    // True when the id is not one the file holds (see the class comment).
    bool derivedId{false};
  };

  struct Ignored {
    juce::String file;
    juce::String reason;
  };

  struct Listing {
    bool found{false};
    Setlist setlist;
    std::vector<Slot> slots;  // by slot, empty slots left out
    std::vector<Ignored> ignored;

    const Slot* at(int slot) const;
    /** The first empty slot from `from` on (then from 1), 0 when full. */
    int firstFree(int from = 1) const;
  };

  struct Located {
    Setlist setlist;
    Slot slot;
  };

  struct Position {
    juce::String setlistId;
    int slot{0};  // 0 = none
  };

  explicit SetlistLibrary(const juce::File& rootDir);

  /** <Documents>/Pedaleira, or what setRootForTesting names. This is
      LibraryPaths::libraryRoot(): the one place the root is resolved, which
      the guitar profiles and the agent layer's allowed folders use too. */
  static juce::File defaultRoot();
  /** Process-wide: every library made without an explicit root is rooted
      here (an invalid File restores the real one). Returns the previous
      override. Tests and tools set it before any processor exists. The same
      override as LibraryPaths::setLibraryRootForTesting. */
  static juce::File setRootForTesting(const juce::File& dir);
  /** Process-wide: called each time a library reads a folder (the setlists,
      or the presets of one). A test uses it to see what a caller holds
      while the disk is being read. Returns the previous observer. */
  static std::function<void()> setScanObserverForTesting(std::function<void()> observer);
  /** True while setRootForTesting names a root. */
  static bool rootIsOverridden();
  /** Process-wide: "moved to the trash" means moved into this folder, so a
      test run leaves nothing in the user's real trash. */
  static juce::File setTrashDirForTesting(const juce::File& dir);
  /** What "to the trash" means for anything kept under a library root (a
      preset, a setlist, a guitar profile): the system trash, or where there
      is none a Trash folder in `libraryRoot`, or the folder
      setTrashDirForTesting names. Never a delete. */
  static bool moveToTrash(const juce::File& file, const juce::File& libraryRoot);
  /** A guest reads and writes the presets like anyone, but leaves no trace
      in library.json of where it went: its position is held in memory and
      it adds nothing to the recents. What a sandbox rig's library is (the
      position and the recents are the player's). */
  void setGuest(bool isGuest) {
    const juce::ScopedLock guard(lock);
    guest = isGuest;
  }

  juce::File root() const { return rootDir; }
  juce::File setlistsDir() const { return rootDir.getChildFile("Setlists"); }
  juce::File stateFile() const { return rootDir.getChildFile("library.json"); }
  const t3k::presetjson::AssetStore& assets() const { return assetStore; }

  // Setlists
  std::vector<Setlist> setlists() const;
  std::optional<Setlist> findSetlist(const juce::String& id) const;
  juce::Result createSetlist(const juce::String& name, Setlist* created = nullptr);
  juce::Result renameSetlist(const juce::String& id, const juce::String& newName);
  /** Moves the folder to the trash. */
  juce::Result deleteSetlist(const juce::String& id);
  /** Moves a setlist to position `index` of the list (0 = first). */
  juce::Result reorderSetlist(const juce::String& id, int index);

  // Presets of one setlist
  Listing list(const juce::String& setlistId) const;
  /** Where the preset with this id is: in `preferSetlistId` when it is
      there, else in the first setlist that has it. */
  std::optional<Located> find(const juce::String& presetId,
                              const juce::String& preferSetlistId = {}) const;

  struct SaveOptions {
    // 0: the slot of a preset of the same name, else the first empty one.
    int slot{0};
    // Keep the tree's own "id" when no preset of the library has it (an
    // import that must stay findable under the id it had).
    bool keepId{false};
  };
  /** Writes `preset` (its "name" property is the name) into a slot. A preset
      already in the slot is replaced and its id kept. */
  juce::Result save(const juce::String& setlistId, juce::ValueTree preset,
                    const SaveOptions& options, Slot* saved = nullptr);
  /** The preset tree of a slot; `result.error` says why not. */
  t3k::presetjson::ReadResult load(const Located& located) const;
  juce::Result rename(const juce::String& setlistId, int slot, const juce::String& newName);
  /** Moves a preset to another slot. `insert`: the presets (and gaps) in
      between shift one slot towards where it came from; otherwise the two
      slots swap. */
  juce::Result move(const juce::String& setlistId, int from, int to, bool insert);
  /** Reorders by position among the occupied slots, which keep their
      numbers (what the dense preset list's move does). */
  juce::Result moveByDelta(const juce::String& setlistId, int slot, int delta);
  /** A copy under a new id, named "<name> copy", in `toSlot` or the first
      empty slot after the original. */
  juce::Result duplicate(const juce::String& setlistId, int slot, int toSlot, Slot* made = nullptr);
  /** Moves the file to the trash. */
  juce::Result remove(const juce::String& setlistId, int slot);
  /** A copy under a new id in another setlist (`toSlot` 0 = first empty). */
  juce::Result copyTo(const juce::String& setlistId, int slot, const juce::String& toSetlistId,
                      int toSlot, Slot* made = nullptr);
  /** Renames every preset file of the setlist to what its slot and name
      make it. Returns how many files were renamed. */
  int tidy(const juce::String& setlistId);
  /** Forgets what the files said, so the next call reads them all again. */
  void forgetCache();

  // Library state (library.json)
  Position current() const;
  void setCurrent(const Position& position);
  juce::StringArray favourites() const;
  bool isFavourite(const juce::String& presetId) const;
  bool setFavourite(const juce::String& presetId, bool favourite);
  juce::StringArray recents() const;
  void noteRecent(const juce::String& presetId);
  /** What a preset load changes, in one write: the preset goes to the top
      of the recents and, when given, the position moves. */
  void noteLoaded(const juce::String& presetId, const std::optional<Position>& position);
  /** Makes `position` this instance's own without writing library.json: a
      session a host restores is where it was saved, whatever the file says
      now, and restoring it is not a choice of the player's to remember. */
  void holdPosition(const Position& position);
  /** Whether the presets of the old .t3kpreset store have been brought over
      (the processor does that, once; see docs/library.md). Kept in
      library.json, so emptying the library later does not bring them back. */
  bool legacyMigrationRecorded() const;
  bool recordLegacyMigration();

  // Names. Static and pure, so the rules can be tested on any platform.
  /** Canonical composition (NFC) of the pairs Unicode composes, in the
      order they appear: what makes a name typed on macOS and the same name
      typed on Linux the same file name. */
  static juce::String normaliseName(const juce::String& name);
  /** A file or folder name component valid on macOS, Windows and Linux:
      composed, without invisible characters, then presetfile::sanitizeStem
      (forbidden characters, reserved device names, leading and trailing dots
      and spaces, kMaxNameBytes of UTF-8). `fallback` when nothing is left. */
  static juce::String sanitiseComponent(const juce::String& name, const juce::String& fallback);
  /** True when two names would be one file on a case-insensitive or
      normalising file system. */
  static bool namesCollide(const juce::String& a, const juce::String& b);
  /** "012 Glass Cleans.json". */
  static juce::String slotFileName(int slot, const juce::String& name);
  /** The slot and the name part of a preset file name; false when it does
      not start with a number from 1 to kMaxSlots. */
  static bool parseSlotFileName(const juce::String& fileName, int& slot, juce::String& stem);

private:
  struct Cached {
    juce::int64 modificationMs{0};
    juce::int64 size{0};
    t3k::presetjson::Meta meta;
  };
  // Id and name of a setlist.json; `valid` is false without a readable one.
  struct SetlistMeta {
    bool valid{false};
    juce::String id;
    juce::String name;
  };

  t3k::presetjson::Meta metaOf(const juce::File& file) const;
  /** The listing of a setlist that is already resolved (list() minus the
      lookup of the id). */
  Listing listFolder(const Setlist& setlist) const;
  /** The listing with every file under its proper name: tidy() first when
      one is not, so an edit can never find another preset's file under the
      name it is about to write. */
  Listing tidied(const juce::String& setlistId);
  SetlistMeta readSetlistMeta(const juce::File& folder) const;
  bool writeSetlistMeta(const juce::File& folder, const juce::String& id,
                        const juce::String& name) const;
  /** Gives a setlist made by hand its setlist.json, and moves what
      library.json said about its derived id to the new one. */
  Setlist withMeta(const Setlist& setlist);
  juce::File uniqueFolder(const juce::String& name, const juce::File& self = {}) const;

  /** Writes a preset's text as the file of `slot`. `replacing` is the file
      the slot has now (renamed when the name changed), invalid for an empty
      slot. */
  juce::Result writeSlot(const Setlist& setlist, int slot, const juce::String& name,
                         const juce::String& text, const juce::File& replacing, Slot* written);
  static juce::Result writeText(const juce::File& file, const juce::String& text);
  bool renameAll(const std::vector<std::pair<juce::File, juce::File>>& renames);
  bool trash(const juce::File& file) const;
  bool idInUse(const juce::String& rawId) const;

  /** library.json as an object, or the defaults when it is missing, empty
      or broken. `broken`: it is there and is not a library state. */
  juce::var readState(bool* broken = nullptr) const;
  /** Writes library.json. A broken one is first copied aside (see
      docs/library.md); when that fails nothing is written. */
  bool writeState(const juce::var& state) const;
  bool setBrokenStateAside(const juce::File& file) const;
  void replaceSetlistId(const juce::String& oldId, const juce::String& newId);

  juce::File rootDir;
  t3k::presetjson::AssetStore assetStore;
  // This instance's own position once it has chosen one: several plugin
  // instances share library.json, and one changing setlist must not move
  // the program numbers of another under its feet.
  std::optional<Position> ownPosition;
  bool guest{false};  // see setGuest; guarded by lock

  mutable juce::CriticalSection lock;
  mutable std::map<juce::String, Cached> cache;  // full path -> what the file said
};

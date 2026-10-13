#include "Processor.h"

#include "PresetFile.h"
#include "PresetJson.h"

#include <cmath>
#include <map>
#include <set>

// #############################
// SETLIST LIBRARY
// #############################
//
// The processor's side of the library (docs/library.md): which setlist is
// current, the factory presets as a read-only setlist beside the user's
// folders, turning the rig into a preset tree and back, and the "unsaved
// changes" flag. The folders and files themselves are SetlistLibrary's, the
// JSON format PresetJson's. All of it runs on the message thread; nothing
// here is reachable from the audio callback.

namespace presetjson = t3k::presetjson;

namespace {

constexpr const char* kFactorySetlistName = "Factory";
constexpr const char* kLegacyImportSetlistName = "Imported Presets";
// The setlist a first save makes, and the one the old store's presets are
// brought over into.
constexpr const char* kDefaultSetlistName = "My Presets";

juce::Result factoryIsReadOnly() {
  return juce::Result::fail("the factory setlist is read-only");
}

}  // namespace

// ####################
// A GUEST OF ANOTHER RIG'S LIBRARY
// ####################

void TONE3000Processor::sharePresetStoreWith(const TONE3000Processor& other) {
  // The other's library as it is once its old presets have been brought
  // over: that is its own business, and a guest never does it.
  const SetlistLibrary& theirs = other.userLibrary();
  presetManager = other.presetManager;
  library = std::make_unique<SetlistLibrary>(theirs.root());
  library->setGuest(true);
  // It starts where the other is, and goes its own way from there.
  library->holdPosition(theirs.current());
  libraryGuest = true;
  legacyMigrationAllowed = false;
  {
    const juce::ScopedLock guard(legacyMigrationLock);
    legacyMigrationStarted = true;
  }
  legacyMigrationDone.store(true);
  hostProgramInfoCache.clear();
}

juce::Result TONE3000Processor::guestCannotReplace(const juce::String& what) {
  return juce::Result::fail(
      "a sandbox rig only adds to the library and " + what +
      " is already there: save under another name or into an empty slot, or apply the "
      "sandbox to the live rig (sandbox.applyToLive) and save from there");
}

juce::String TONE3000Processor::resolveSetlistId(const juce::String& setlistId) const {
  return setlistId.isEmpty() ? currentSetlistId() : setlistId;
}

bool TONE3000Processor::presetExists(const juce::String& presetId) const {
  if (presetId.startsWith("factory:")) {
    for (const auto& info : factoryPresets())
      if (info.id == presetId)
        return true;
    return false;
  }
  return userLibrary().find(presetId, currentSetlistId()).has_value();
}

juce::var TONE3000Processor::slotVar(const SetlistLibrary::Slot& slot,
                                     const juce::StringArray& favourites) {
  juce::DynamicObject::Ptr obj = new juce::DynamicObject();
  obj->setProperty("slot", slot.slot);
  obj->setProperty("id", slot.id);
  obj->setProperty("name", slot.name);
  obj->setProperty("sceneCount", slot.sceneCount);
  obj->setProperty("tempo", slot.tempo.has_value() ? juce::var(*slot.tempo) : juce::var());
  juce::Array<juce::var> tags;
  for (const auto& tag : slot.tags)
    tags.add(tag);
  obj->setProperty("tags", tags);
  obj->setProperty("favourite", favourites.contains(slot.id));
  obj->setProperty("file", slot.file.getFileName());
  return juce::var(obj.get());
}

// ####################
// READING
// ####################

juce::var TONE3000Processor::getSetlists() const {
  const juce::String current = currentSetlistId();
  juce::Array<juce::var> list;
  const auto add = [&](const juce::String& id, const juce::String& name, bool readOnly,
                       int presetCount) {
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("id", id);
    obj->setProperty("name", name);
    obj->setProperty("readOnly", readOnly);
    obj->setProperty("presetCount", presetCount);
    obj->setProperty("current", id == current);
    list.add(juce::var(obj.get()));
  };
  for (const auto& setlist : userLibrary().setlists())
    add(setlist.id, setlist.name, false,
        static_cast<int>(userLibrary().list(setlist.id).slots.size()));
  // Always there, after the user's own.
  add(kFactorySetlistId, kFactorySetlistName, true, static_cast<int>(factoryPresets().size()));

  juce::DynamicObject::Ptr root = new juce::DynamicObject();
  root->setProperty("setlists", list);
  root->setProperty("current", current);
  return juce::var(root.get());
}

juce::var TONE3000Processor::getSetlistState(const juce::String& setlistIdArg) const {
  const juce::String setlistId = resolveSetlistId(setlistIdArg);
  const juce::String current = currentSetlistId();
  const juce::StringArray favourites = userLibrary().favourites();
  const SetlistLibrary::Position position = userLibrary().current();

  juce::DynamicObject::Ptr root = new juce::DynamicObject();
  juce::Array<juce::var> slots, ignored;
  if (setlistId == kFactorySetlistId) {
    root->setProperty("id", setlistId);
    root->setProperty("name", kFactorySetlistName);
    root->setProperty("readOnly", true);
    const auto factory = factoryPresets();
    for (size_t i = 0; i < factory.size(); ++i) {
      // What a factory file holds beyond its id and name is megabytes of
      // model data away (PresetFile.h), so its summary stays at that.
      juce::DynamicObject::Ptr obj = new juce::DynamicObject();
      obj->setProperty("slot", static_cast<int>(i) + 1);
      obj->setProperty("id", factory[i].id);
      obj->setProperty("name", factory[i].name);
      obj->setProperty("sceneCount", juce::var());
      obj->setProperty("tempo", juce::var());
      obj->setProperty("tags", juce::Array<juce::var>());
      obj->setProperty("favourite", favourites.contains(factory[i].id));
      slots.add(juce::var(obj.get()));
    }
  } else {
    const SetlistLibrary::Listing listing = userLibrary().list(setlistId);
    if (!listing.found)
      return {};
    root->setProperty("id", listing.setlist.id);
    root->setProperty("name", listing.setlist.name);
    root->setProperty("readOnly", false);
    root->setProperty("folder", listing.setlist.folder.getFullPathName());
    for (const auto& slot : listing.slots)
      slots.add(slotVar(slot, favourites));
    for (const auto& entry : listing.ignored) {
      juce::DynamicObject::Ptr obj = new juce::DynamicObject();
      obj->setProperty("file", entry.file);
      obj->setProperty("reason", entry.reason);
      ignored.add(juce::var(obj.get()));
    }
  }
  root->setProperty("capacity", SetlistLibrary::kMaxSlots);
  root->setProperty("current", setlistId == current);
  root->setProperty("currentSlot",
                    setlistId == current && position.setlistId == current ? position.slot : 0);
  root->setProperty("slots", slots);
  root->setProperty("ignored", ignored);
  return juce::var(root.get());
}

juce::var TONE3000Processor::getRecentPresets() const {
  // One pass over the library for all of them, the current setlist first so
  // a preset that is in two setlists is reported where the player is.
  const juce::String current = currentSetlistId();
  std::map<juce::String, std::pair<juce::String, SetlistLibrary::Slot>> index;
  const auto addSetlist = [&](const juce::String& setlistId) {
    for (const auto& slot : userLibrary().list(setlistId).slots)
      index.emplace(slot.id, std::pair{setlistId, slot});
  };
  if (current != kFactorySetlistId)
    addSetlist(current);
  for (const auto& setlist : userLibrary().setlists())
    if (setlist.id != current)
      addSetlist(setlist.id);
  const auto factory = factoryPresets();

  juce::Array<juce::var> list;
  for (const auto& id : userLibrary().recents()) {
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("id", id);
    if (const auto it = index.find(id); it != index.end()) {
      obj->setProperty("name", it->second.second.name);
      obj->setProperty("setlist", it->second.first);
      obj->setProperty("slot", it->second.second.slot);
      obj->setProperty("factory", false);
    } else {
      int slot = 0;
      for (size_t i = 0; i < factory.size(); ++i)
        if (factory[i].id == id)
          slot = static_cast<int>(i) + 1;
      if (slot == 0)
        continue;  // deleted, or on a branch that is not checked out
      obj->setProperty("name", factory[static_cast<size_t>(slot - 1)].name);
      obj->setProperty("setlist", kFactorySetlistId);
      obj->setProperty("slot", slot);
      obj->setProperty("factory", true);
    }
    list.add(juce::var(obj.get()));
  }
  return list;
}

juce::var TONE3000Processor::getLibraryState() {
  const juce::String currentId = currentSetlistId();
  const SetlistLibrary::Position position = userLibrary().current();

  juce::DynamicObject::Ptr current = new juce::DynamicObject();
  current->setProperty("setlist", currentId);
  current->setProperty("slot", position.setlistId == currentId ? position.slot : 0);
  {
    juce::ScopedLock lock(chainMutex);
    current->setProperty("presetId", activePresetId);
  }

  juce::Array<juce::var> favourites;
  for (const auto& id : userLibrary().favourites())
    favourites.add(id);

  // The old store's presets that are not in the library yet: what
  // library.importLegacyPresets would bring over.
  std::set<juce::String> known;
  for (const auto& setlist : userLibrary().setlists())
    for (const auto& slot : userLibrary().list(setlist.id).slots)
      known.insert(slot.id);
  int legacy = 0;
  for (const auto& info : presetManager.list())
    if (!info.factory && known.count(info.id) == 0)
      ++legacy;

  juce::DynamicObject::Ptr root = new juce::DynamicObject();
  root->setProperty("root", userLibrary().root().getFullPathName());
  root->setProperty("setlists", getSetlists()["setlists"]);
  root->setProperty("current", juce::var(current.get()));
  root->setProperty("favourites", favourites);
  root->setProperty("recents", getRecentPresets());
  root->setProperty("unsaved", hasUnsavedChanges());
  root->setProperty("legacyPresets", legacy);
  return juce::var(root.get());
}

// ####################
// SETLISTS
// ####################

juce::Result TONE3000Processor::createSetlist(const juce::String& name, bool select,
                                              juce::String* id) {
  SetlistLibrary::Setlist made;
  if (const juce::Result result = userLibrary().createSetlist(name, &made); result.failed())
    return result;
  if (id != nullptr)
    *id = made.id;
  if (select)
    return selectSetlist(made.id);
  // The first setlist of the user's own becomes the current one by itself
  // (see currentSetlistId), which moves the preset list.
  presetListChanged();
  return juce::Result::ok();
}

juce::Result TONE3000Processor::renameSetlist(const juce::String& setlistId,
                                              const juce::String& name) {
  if (setlistId == kFactorySetlistId)
    return factoryIsReadOnly();
  return userLibrary().renameSetlist(setlistId, name);
}

juce::Result TONE3000Processor::deleteSetlist(const juce::String& setlistId) {
  if (setlistId == kFactorySetlistId)
    return factoryIsReadOnly();
  const juce::Result result = userLibrary().deleteSetlist(setlistId);
  if (result.wasOk())
    presetListChanged();  // it may have been the current one
  return result;
}

juce::Result TONE3000Processor::reorderSetlist(const juce::String& setlistId, int index) {
  if (setlistId == kFactorySetlistId)
    return juce::Result::fail("the factory setlist is always last");
  return userLibrary().reorderSetlist(setlistId, index);
}

juce::Result TONE3000Processor::selectSetlist(const juce::String& setlistId) {
  if (setlistId != kFactorySetlistId && !userLibrary().findSetlist(setlistId).has_value())
    return juce::Result::fail("no setlist with id '" + setlistId + "'");
  if (setlistId != currentSetlistId() || userLibrary().current().setlistId != setlistId) {
    // The slot the active preset has in the setlist, if it is one of its
    // presets; else no slot yet.
    juce::String activeId;
    {
      juce::ScopedLock lock(chainMutex);
      activeId = activePresetId;
    }
    userLibrary().setCurrent({setlistId, 0});
    const auto slots = currentPresetSlots();
    for (size_t i = 0; i < slots.size(); ++i)
      if (activeId.isNotEmpty() && slots[i].id == activeId)
        userLibrary().setCurrent({setlistId, static_cast<int>(i) + 1});
    // The footswitch banks belong to the list they paged.
    footswitchBank.store(0);
    bumpChainRevision();
    presetListChanged();
  }
  return juce::Result::ok();
}

// ####################
// SLOTS
// ####################

juce::Result TONE3000Processor::savePresetToSlot(const juce::String& setlistIdArg, int slot,
                                                 const juce::String& rawName, juce::var* saved) {
  const juce::String setlistId = resolveSetlistId(setlistIdArg);
  if (setlistId == kFactorySetlistId)
    return factoryIsReadOnly();
  const SetlistLibrary::Listing listing = userLibrary().list(setlistId);
  if (!listing.found)
    return juce::Result::fail("no setlist with id '" + setlistId + "'");

  // No name given: the slot's preset is saved over under its own.
  juce::String name = rawName.trim();
  if (name.isEmpty())
    if (const auto* existing = listing.at(slot))
      name = existing->name;
  if (name.isEmpty())
    return juce::Result::fail("a preset needs a name");

  if (libraryGuest) {
    // The slot the save would land in, as SetlistLibrary::save picks it.
    const SetlistLibrary::Slot* taken = slot != 0 ? listing.at(slot) : nullptr;
    if (slot == 0)
      for (const auto& entry : listing.slots)
        if (taken == nullptr &&
            SetlistLibrary::namesCollide(entry.name, SetlistLibrary::normaliseName(name)))
          taken = &entry;
    if (taken != nullptr)
      return guestCannotReplace("the preset '" + taken->name + "' (slot " +
                                juce::String(taken->slot) + ")");
  }

  juce::ValueTree preset = buildPresetTree();
  preset.setProperty("name", name, nullptr);
  SetlistLibrary::SaveOptions options;
  options.slot = slot;
  SetlistLibrary::Slot written;
  if (const juce::Result result = userLibrary().save(setlistId, preset, options, &written);
      result.failed())
    return result;

  // The rig is this preset now, in this setlist (which a save into another
  // setlist thereby selects).
  setActivePreset(written.id, written.name);
  if (const juce::Result selected = selectSetlist(setlistId); selected.failed())
    return selected;
  userLibrary().setCurrent({setlistId, written.slot});
  markPresetBaseline();
  presetListChanged();
  juce::Logger::writeToLog("[Presets] Saved preset: " + written.name);

  if (saved != nullptr) {
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("id", written.id);
    obj->setProperty("name", written.name);
    obj->setProperty("setlist", setlistId);
    obj->setProperty("slot", written.slot);
    *saved = juce::var(obj.get());
  }
  return juce::Result::ok();
}

juce::Result TONE3000Processor::loadPresetSlot(const juce::String& setlistIdArg, int slot) {
  const juce::String setlistId = resolveSetlistId(setlistIdArg);
  juce::String presetId;
  if (setlistId == kFactorySetlistId) {
    const auto factory = factoryPresets();
    if (slot >= 1 && slot <= static_cast<int>(factory.size()))
      presetId = factory[static_cast<size_t>(slot - 1)].id;
  } else {
    const SetlistLibrary::Listing listing = userLibrary().list(setlistId);
    if (!listing.found)
      return juce::Result::fail("no setlist with id '" + setlistId + "'");
    if (const auto* entry = listing.at(slot))
      presetId = entry->id;
  }
  if (presetId.isEmpty())
    return juce::Result::fail("slot " + juce::String(slot) + " is empty");

  // Loading out of another setlist is going there.
  if (const juce::Result selected = selectSetlist(setlistId); selected.failed())
    return selected;
  if (!loadPreset(presetId))
    return juce::Result::fail("the preset in slot " + juce::String(slot) + " could not be read");
  return juce::Result::ok();
}

juce::Result TONE3000Processor::movePresetSlot(const juce::String& setlistIdArg, int from, int to,
                                               bool insert) {
  const juce::String setlistId = resolveSetlistId(setlistIdArg);
  if (setlistId == kFactorySetlistId)
    return factoryIsReadOnly();
  const juce::Result result = userLibrary().move(setlistId, from, to, insert);
  if (result.wasOk()) {
    // The position is a preset, not a number: it follows the preset.
    if (setlistId == currentSetlistId()) {
      juce::String activeId;
      {
        juce::ScopedLock lock(chainMutex);
        activeId = activePresetId;
      }
      if (const auto located = userLibrary().find(activeId, setlistId);
          located.has_value() && located->setlist.id == setlistId)
        userLibrary().setCurrent({setlistId, located->slot.slot});
    }
    presetListChanged();
  }
  return result;
}

juce::Result TONE3000Processor::duplicatePresetSlot(const juce::String& setlistIdArg, int slot,
                                                    int toSlot, juce::var* made) {
  const juce::String setlistId = resolveSetlistId(setlistIdArg);
  if (setlistId == kFactorySetlistId)
    return factoryIsReadOnly();
  SetlistLibrary::Slot written;
  const juce::Result result = userLibrary().duplicate(setlistId, slot, toSlot, &written);
  if (result.wasOk()) {
    if (made != nullptr)
      *made = slotVar(written, userLibrary().favourites());
    presetListChanged();
  }
  return result;
}

juce::Result TONE3000Processor::deletePresetSlot(const juce::String& setlistIdArg, int slot) {
  const juce::String setlistId = resolveSetlistId(setlistIdArg);
  if (setlistId == kFactorySetlistId)
    return factoryIsReadOnly();
  const SetlistLibrary::Listing listing = userLibrary().list(setlistId);
  if (!listing.found)
    return juce::Result::fail("no setlist with id '" + setlistId + "'");
  const auto* entry = listing.at(slot);
  if (entry == nullptr)
    return juce::Result::fail("slot " + juce::String(slot) + " is empty");
  const juce::String presetId = entry->id;
  if (const juce::Result result = userLibrary().remove(setlistId, slot); result.failed())
    return result;
  // Whether a copy of it is left in another setlist is a scan of the
  // folders: before the lock, which the audio thread try-locks every block.
  const bool gone = !userLibrary().find(presetId).has_value();
  {
    // The rig stays as it is; it just is no preset's any more.
    juce::ScopedLock lock(chainMutex);
    if (activePresetId == presetId && gone) {
      activePresetId.clear();
      activePresetName.clear();
      presetBaseline = {};
      bumpChainRevision();
    }
  }
  presetListChanged();
  return juce::Result::ok();
}

juce::Result TONE3000Processor::copyPresetToSetlist(const juce::String& setlistIdArg, int slot,
                                                    const juce::String& toSetlistId, int toSlot,
                                                    juce::var* made) {
  const juce::String setlistId = resolveSetlistId(setlistIdArg);
  if (toSetlistId == kFactorySetlistId)
    return factoryIsReadOnly();
  SetlistLibrary::Slot written;
  if (setlistId == kFactorySetlistId) {
    // Out of the factory setlist: its .t3kpreset becomes a JSON preset of
    // the user's, under an id of its own.
    const auto factory = factoryPresets();
    if (slot < 1 || slot > static_cast<int>(factory.size()))
      return juce::Result::fail("slot " + juce::String(slot) + " is empty");
    juce::ValueTree preset = presetManager.load(factory[static_cast<size_t>(slot - 1)].id);
    if (!preset.isValid())
      return juce::Result::fail("the factory preset could not be read");
    const SetlistLibrary::Listing target = userLibrary().list(toSetlistId);
    if (!target.found)
      return juce::Result::fail("no setlist with id '" + toSetlistId + "'");
    SetlistLibrary::SaveOptions options;
    options.slot = toSlot != 0 ? toSlot : target.firstFree();
    if (options.slot == 0)
      return juce::Result::fail("the setlist is full");
    if (target.at(options.slot) != nullptr)
      return juce::Result::fail("slot " + juce::String(options.slot) + " is taken");
    preset.setProperty("name", factory[static_cast<size_t>(slot - 1)].name, nullptr);
    if (const juce::Result result = userLibrary().save(toSetlistId, preset, options, &written);
        result.failed())
      return result;
  } else if (const juce::Result result =
                 userLibrary().copyTo(setlistId, slot, toSetlistId, toSlot, &written);
             result.failed()) {
    return result;
  }
  if (made != nullptr)
    *made = slotVar(written, userLibrary().favourites());
  presetListChanged();
  return juce::Result::ok();
}

// ####################
// FILES OUTSIDE THE LIBRARY
// ####################

juce::Result TONE3000Processor::exportPresetJson(const juce::String& presetId,
                                                 const juce::File& file, bool embedAssets,
                                                 const juce::String& rawName) {
  if (libraryGuest && file.exists())
    return guestCannotReplace("the file " + file.getFullPathName());
  juce::ValueTree preset;
  if (presetId.isEmpty()) {
    // The rig as it is, saved or not.
    preset = buildPresetTree();
    juce::String name = rawName.trim(), id;
    {
      juce::ScopedLock lock(chainMutex);
      if (name.isEmpty())
        name = activePresetName;
      id = activePresetId.fromFirstOccurrenceOf(":", false, false);
    }
    preset.setProperty("name", name.isNotEmpty() ? name : juce::String("Untitled"), nullptr);
    preset.setProperty("id", id.isNotEmpty() ? id : juce::Uuid().toString(), nullptr);
  } else {
    preset = readPresetTree(presetId);
    if (!preset.isValid())
      return juce::Result::fail("no preset with id '" + presetId + "'");
    if (rawName.trim().isNotEmpty())
      preset.setProperty("name", rawName.trim(), nullptr);
  }

  if (!file.getParentDirectory().createDirectory())
    return juce::Result::fail("cannot create " + file.getParentDirectory().getFullPathName());
  // The old format on request (its extension): one binary file with the
  // models inside, for a build from before the library.
  if (file.hasFileExtension(t3k::presetfile::kExtension))
    return t3k::presetfile::write(file, preset)
               ? juce::Result::ok()
               : juce::Result::fail("cannot write " + file.getFullPathName());
  const juce::File assetsDir = file.getParentDirectory().getChildFile("Assets");
  // The assets are written into a folder beside the file, never through a
  // link that happens to be there under that name: a link would put them
  // wherever it points, outside the folder the caller named (and outside the
  // folders a command server client is confined to; the agent layer checks
  // where this folder resolves to as well, AgentHub::pathRefusal).
  if (!embedAssets && assetsDir.isSymbolicLink())
    return juce::Result::fail("the Assets folder beside " + file.getFullPathName() +
                              " is a symbolic link; the export does not write through it "
                              "(remove the link, or export with embedAssets)");
  const presetjson::AssetStore beside(assetsDir);
  presetjson::WriteOptions options;
  options.assets = embedAssets ? nullptr : &beside;
  juce::String error;
  const juce::String text = presetjson::toJson(preset, options, error);
  if (text.isEmpty())
    return juce::Result::fail(error);
  // Not replaceWithText: that converts line ends per platform, and the file
  // should be the same bytes everywhere. And never a delete first: a file
  // that was there is there until the new one is whole.
  return presetjson::writeFileAtomically(file, text);
}

namespace {

// A preset tree out of a file someone handed us: a .t3kpreset, or a JSON
// preset whose assets are in the file, in an Assets folder beside it, in the
// library the file sits in (<root>/Setlists/<Setlist>/<file>), or in ours.
presetjson::ReadResult readPresetFileTree(const juce::File& file,
                                          const presetjson::AssetStore& ours) {
  presetjson::ReadResult result;
  if (!file.existsAsFile()) {
    result.error = "file not found: " + file.getFullPathName();
    return result;
  }
  if (file.hasFileExtension(t3k::presetfile::kExtension)) {
    result.preset = t3k::presetfile::read(file);
    if (!result.preset.isValid())
      result.error = "not a preset file: " + file.getFullPathName();
    return result;
  }
  const juce::File folder = file.getParentDirectory();
  const presetjson::AssetStore beside(folder.getChildFile("Assets"));
  const presetjson::AssetStore ofItsLibrary(
      folder.getParentDirectory().getParentDirectory().getChildFile("Assets"));
  presetjson::ReadOptions options;
  options.assets = {&beside, &ofItsLibrary, &ours};
  return presetjson::fromJsonFile(file, options);
}

}  // namespace

juce::Result TONE3000Processor::importPresetJson(const juce::File& file,
                                                 const juce::String& setlistIdArg, int slot,
                                                 juce::var* made) {
  const juce::String setlistId = resolveSetlistId(setlistIdArg);
  if (setlistId == kFactorySetlistId)
    return factoryIsReadOnly();
  const SetlistLibrary::Listing listing = userLibrary().list(setlistId);
  if (!listing.found)
    return juce::Result::fail("no setlist with id '" + setlistId + "'");

  presetjson::ReadResult read = readPresetFileTree(file, userLibrary().assets());
  if (!read.preset.isValid())
    return juce::Result::fail(read.error);
  if (read.preset.getProperty("name").toString().trim().isEmpty())
    read.preset.setProperty("name", file.getFileNameWithoutExtension(), nullptr);
  return importPresetTree(read.preset, read.missing, setlistId, slot, made);
}

juce::Result TONE3000Processor::importPresetTree(
    const juce::ValueTree& preset, const std::vector<presetjson::AssetRef>& missingAssets,
    const juce::String& setlistIdArg, int slot, juce::var* made) {
  const juce::String setlistId = resolveSetlistId(setlistIdArg);
  if (setlistId == kFactorySetlistId)
    return factoryIsReadOnly();
  const SetlistLibrary::Listing listing = userLibrary().list(setlistId);
  if (!listing.found)
    return juce::Result::fail("no setlist with id '" + setlistId + "'");

  SetlistLibrary::SaveOptions options;
  options.slot = slot != 0 ? slot : listing.firstFree();
  // The id it had, when the library does not know it yet: importing a file
  // twice gives two presets, not two files fighting over one id.
  options.keepId = true;
  if (options.slot == 0)
    return juce::Result::fail("the setlist is full");
  if (listing.at(options.slot) != nullptr)
    return juce::Result::fail("slot " + juce::String(options.slot) + " is taken");
  SetlistLibrary::Slot written;
  if (const juce::Result result = userLibrary().save(setlistId, preset, options, &written);
      result.failed())
    return result;
  if (made != nullptr) {
    juce::var description = slotVar(written, userLibrary().favourites());
    // Assets the file names and nobody has: the preset is in, the blocks
    // concerned will come up missing until the files turn up.
    juce::Array<juce::var> missing;
    for (const auto& asset : missingAssets)
      missing.add(asset.name.isNotEmpty() ? asset.name : asset.hash);
    description.getDynamicObject()->setProperty("missingAssets", missing);
    *made = description;
  }
  presetListChanged();
  return juce::Result::ok();
}

bool TONE3000Processor::loadPresetFile(const juce::File& file) {
  serviceHostedPlugins();  // as loadPreset
  presetjson::ReadResult read = readPresetFileTree(file, userLibrary().assets());
  if (!read.preset.isValid()) {
    juce::Logger::writeToLog("[Presets] Cannot load " + file.getFullPathName() + ": " +
                             read.error);
    return false;
  }
  if (read.preset.getProperty("name").toString().isEmpty())
    read.preset.setProperty("name", file.getFileNameWithoutExtension(), nullptr);
  // Not a preset of any setlist: an id no list holds, so no program number
  // or footswitch claims it. (Without the extension: the same rig from a
  // .t3kpreset and from its JSON is the same state.)
  return applyPresetTree(read.preset, "file:" + file.getFileNameWithoutExtension());
}

juce::var TONE3000Processor::importLegacyPresets(const juce::String& rawSetlistName) {
  const juce::String baseName =
      rawSetlistName.trim().isNotEmpty() ? rawSetlistName.trim()
                                         : juce::String(kLegacyImportSetlistName);
  int imported = 0, skipped = 0, failed = 0;
  juce::StringArray used;

  // What the library holds already, read once: a preset keeps the id it
  // always had ("user:<id>" in both stores), so a session or a favourite
  // that names it finds the imported preset, and a second import knows it is
  // here already.
  std::set<juce::String> known;
  for (const auto& setlist : userLibrary().setlists())
    for (const auto& slot : userLibrary().list(setlist.id).slots)
      known.insert(slot.id);

  // The setlist of that name, made when missing; "<name> 2" and so on once
  // one is full. Its empty slots are read once and handed out from here.
  int sequence = 1;
  juce::String targetId;
  std::vector<int> freeSlots;
  const auto nextSlot = [&]() -> int {
    while (freeSlots.empty()) {
      const juce::String name = sequence == 1 ? baseName : baseName + " " + juce::String(sequence);
      ++sequence;
      targetId.clear();
      for (const auto& setlist : userLibrary().setlists())
        if (SetlistLibrary::namesCollide(setlist.name, name))
          targetId = setlist.id;
      if (targetId.isEmpty()) {
        SetlistLibrary::Setlist made;
        if (userLibrary().createSetlist(name, &made).failed())
          return 0;
        targetId = made.id;
      }
      const SetlistLibrary::Listing listing = userLibrary().list(targetId);
      for (int slot = SetlistLibrary::kMaxSlots; slot >= 1; --slot)
        if (listing.at(slot) == nullptr)
          freeSlots.push_back(slot);  // lowest last, taken first
    }
    const int slot = freeSlots.back();
    freeSlots.pop_back();
    return slot;
  };

  for (const auto& info : presetManager.list()) {
    if (info.factory)
      continue;
    if (known.count(info.id) != 0) {
      ++skipped;
      continue;
    }
    juce::ValueTree preset = presetManager.load(info.id);
    // An empty slot named outright: never over a preset of the same name.
    SetlistLibrary::SaveOptions options;
    options.slot = preset.isValid() ? nextSlot() : 0;
    options.keepId = true;
    if (options.slot == 0) {
      ++failed;
      continue;
    }
    preset.setProperty("name", info.name, nullptr);
    preset.setProperty("id", info.id.fromFirstOccurrenceOf(":", false, false), nullptr);
    if (userLibrary().save(targetId, preset, options).failed()) {
      ++failed;
      continue;
    }
    ++imported;
    known.insert(info.id);
    used.addIfNotAlreadyThere(targetId);
  }
  if (imported > 0)
    presetListChanged();
  juce::Logger::writeToLog("[Library] Imported " + juce::String(imported) + " presets from " +
                           presetManager.userPresetsDir().getFullPathName());

  juce::Array<juce::var> setlists;
  for (const auto& id : used)
    setlists.add(id);
  juce::DynamicObject::Ptr root = new juce::DynamicObject();
  root->setProperty("imported", imported);
  root->setProperty("skipped", skipped);
  root->setProperty("failed", failed);
  root->setProperty("setlists", setlists);
  return juce::var(root.get());
}

// ####################
// THE OLD STORE
// ####################

SetlistLibrary& TONE3000Processor::userLibrary() const {
  ensureLegacyMigration();
  return *library;
}

void TONE3000Processor::ensureLegacyMigration() const {
  if (legacyMigrationDone.load())
    return;
  // One at a time, and the others wait: a listing must not see the setlist
  // half filled. (Recursive, so the migration's own calls come straight
  // back out.)
  const juce::ScopedLock guard(legacyMigrationLock);
  if (legacyMigrationStarted)
    return;
  legacyMigrationStarted = true;
  if (legacyMigrationAllowed)
    migrateLegacyStore();
  legacyMigrationDone.store(true);
}

void TONE3000Processor::migrateLegacyStore() const {
  // Only into a library nobody has put a setlist in, and only once for a
  // library: the record is what keeps presets a player deletes afterwards
  // from coming back.
  if (!library->setlists().empty() || library->legacyMigrationRecorded())
    return;
  const auto old = presetManager.list();
  int userPresets = 0;
  for (const auto& info : old)
    userPresets += info.factory ? 0 : 1;
  // Nothing a player made (a first install): no setlist of copies of the
  // factory presets, and no library folder before something is saved.
  if (userPresets == 0)
    return;

  const double startMs = juce::Time::getMillisecondCounterHiRes();
  juce::Logger::writeToLog(
      "[Library] First use of " + library->root().getFullPathName() + ": bringing over the " +
      juce::String(static_cast<int>(old.size())) + " presets of the old preset list (" +
      juce::String(userPresets) + " of the user's own) from " +
      presetManager.userPresetsDir().getFullPathName() + ". This can take a while.");

  // The old list was the program list: entry n was program n. So entry n
  // goes to slot n + 1 of the default setlist, the factory presets (as
  // copies) included, and a preset that cannot be read leaves its slot
  // empty rather than moving the ones after it. Past 256 a second setlist
  // starts; programs only reach 128.
  std::vector<juce::String> setlistIds;
  int imported = 0, failed = 0;
  for (size_t index = 0; index < old.size(); ++index) {
    const size_t page = index / static_cast<size_t>(SetlistLibrary::kMaxSlots);
    while (setlistIds.size() <= page) {
      const juce::String name =
          setlistIds.empty() ? juce::String(kDefaultSetlistName)
                             : juce::String(kDefaultSetlistName) + " " +
                                   juce::String(static_cast<int>(setlistIds.size()) + 1);
      SetlistLibrary::Setlist made;
      if (const juce::Result created = library->createSetlist(name, &made); created.failed()) {
        // Not recorded: tried again the next time the library is opened
        // empty. library.importLegacyPresets brings over what is missing.
        juce::Logger::writeToLog("[Library] The old presets were not brought over: " +
                                 created.getErrorMessage());
        return;
      }
      setlistIds.push_back(made.id);
    }

    const PresetManager::Info& info = old[index];
    juce::ValueTree preset = presetManager.load(info.id);
    if (!preset.isValid()) {
      juce::Logger::writeToLog("[Library] Could not read the old preset '" + info.name + "'");
      ++failed;
      continue;
    }
    SetlistLibrary::SaveOptions options;
    options.slot = static_cast<int>(index % static_cast<size_t>(SetlistLibrary::kMaxSlots)) + 1;
    // A preset of the user's keeps the id it always had ("user:<id>" in
    // both stores), so a host session or a favourite that names it finds
    // it. A factory preset's copy is a preset of the user's now, under an
    // id of its own; the original stays in the factory setlist.
    options.keepId = !info.factory;
    preset.setProperty("name", info.name, nullptr);
    if (info.factory)
      preset.removeProperty("id", nullptr);
    else
      preset.setProperty("id", info.id.fromFirstOccurrenceOf(":", false, false), nullptr);
    if (const juce::Result saved = library->save(setlistIds[page], preset, options);
        saved.failed()) {
      juce::Logger::writeToLog("[Library] Could not bring over '" + info.name +
                               "': " + saved.getErrorMessage());
      ++failed;
      continue;
    }
    ++imported;
  }

  if (!library->recordLegacyMigration())
    juce::Logger::writeToLog("[Library] Could not record the migration in " +
                             library->stateFile().getFullPathName());
  juce::Logger::writeToLog(
      "[Library] Brought over " + juce::String(imported) + " presets (" + juce::String(failed) +
      " failed) in " +
      juce::String(juce::roundToInt(juce::Time::getMillisecondCounterHiRes() - startMs)) + " ms");
}

void TONE3000Processor::holdRestoredPosition() {
  juce::String activeId;
  {
    juce::ScopedLock lock(chainMutex);
    activeId = activePresetId;
  }
  // The setlist the session's preset is in is the session's setlist,
  // whatever library.json names today (another instance, another project or
  // another machine may have moved it since). The program numbers a host
  // holds for this session were numbers of that setlist: with it current
  // again, the host echoing the session's program back finds the active
  // preset there and nothing is loaded (see applyHostProgram). A session
  // whose preset is in no setlist (a factory preset, a file, a deleted one)
  // stays with the setlist selected now, but pinned: it no longer follows
  // the file either.
  const juce::String selected = currentSetlistId();
  const SetlistLibrary::Position saved = userLibrary().current();
  SetlistLibrary::Position position{selected, saved.setlistId == selected ? saved.slot : 0};
  if (const auto located = userLibrary().find(activeId, selected))
    position = {located->setlist.id, located->slot.slot};
  userLibrary().holdPosition(position);
  if (juce::MessageManager::existsAndIsCurrentThread())
    hostProgramInfoCache.clear();
}

juce::var TONE3000Processor::rescanLibrary(bool tidy) {
  userLibrary().forgetCache();
  int renamed = 0;
  if (tidy)
    for (const auto& setlist : userLibrary().setlists())
      renamed += userLibrary().tidy(setlist.id);
  presetListChanged();
  bumpChainRevision();
  juce::var state = getLibraryState();
  state.getDynamicObject()->setProperty("renamed", renamed);
  return state;
}

juce::Result TONE3000Processor::setPresetFavourite(const juce::String& presetId, bool favourite) {
  return userLibrary().setFavourite(presetId, favourite)
             ? juce::Result::ok()
             : juce::Result::fail("cannot write " + userLibrary().stateFile().getFullPathName());
}

// ####################
// UNSAVED CHANGES
// ####################

juce::MemoryBlock TONE3000Processor::presetChainSignatureLocked() const {
  // The settings-only snapshot undo works with: everything a preset holds
  // of the chain but the model bytes, which no edit changes on their own.
  // Hosted plugins contribute a digest of their cached chunk, not the bytes:
  // this runs under the chain lock on every rig change.
  const BlockProcessor::ChunkDigestScope digestScope;
  juce::ValueTree snapshot = captureChainSnapshot(false);
  // Solo is not part of a preset (see buildPresetTree).
  juce::ValueTree gridNode = snapshot.getChildWithName("Grid");
  for (int i = 0; i < gridNode.getNumChildren(); ++i)
    if (juce::ValueTree output = gridNode.getChild(i).getChildWithName("Output");
        output.isValid() && output.hasProperty("solo"))
      output.setProperty("solo", false, nullptr);
  juce::MemoryBlock bytes;
  juce::MemoryOutputStream out(bytes, false);
  snapshot.writeToStream(out);
  out.flush();
  return bytes;
}

void TONE3000Processor::markPresetBaseline() {
  juce::ScopedLock lock(chainMutex);
  presetBaseline = {};
  if (activePresetId.isEmpty())
    return;
  presetBaseline.valid = true;
  presetBaseline.chain = presetChainSignatureLocked();
  for (const auto& paramId : presetParameterIds())
    if (auto* p = parameters.getParameter(paramId))
      presetBaseline.params.push_back(p->getValue());
  presetBaseline.awaitingSettle = true;
  presetBaseline.editSerial = presetEditSerial;
}

bool TONE3000Processor::hasUnsavedChanges() {
  // The snapshot below reads hosted plugins from their cached chunks. A
  // chatty plugin is read at most every few hundred ms here.
  serviceHostedPlugins(false, /*throttled=*/true);

  juce::ScopedLock lock(chainMutex);
  // No preset to differ from: upstream's own notion, a rig that is not the
  // empty default has something in it nobody saved.
  if (activePresetId.isEmpty())
    return !isChainAtDefault();
  if (!presetBaseline.valid)
    return false;
  if (presetBaseline.knownUnsaved)
    return true;

  // The same tolerance isChainAtDefault gives a knob dragged back.
  size_t index = 0;
  for (const auto& paramId : presetParameterIds())
    if (auto* p = parameters.getParameter(paramId)) {
      if (index >= presetBaseline.params.size() ||
          std::abs(p->getValue() - presetBaseline.params[index]) > 1.0e-4f)
        return true;
      ++index;
    }

  if (presetBaseline.awaitingSettle) {
    if (presetBaseline.editSerial != presetEditSerial) {
      // Edited before the load had settled: what was captured at the load
      // is the reference.
      presetBaseline.awaitingSettle = false;
    } else {
      for (const auto& l : lanes)
        for (const auto& block : l)
          if (block && block->modelLoading)
            return false;  // still arriving, and nothing has been edited
      presetBaseline.chain = presetChainSignatureLocked();
      presetBaseline.awaitingSettle = false;
      return false;
    }
  }
  return presetChainSignatureLocked() != presetBaseline.chain;
}

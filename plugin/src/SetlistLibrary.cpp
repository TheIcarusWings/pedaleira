#include "SetlistLibrary.h"

#include "AppIdentity.h"
#include "LibraryPaths.h"
#include "PresetFile.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <set>
#include <utility>

namespace presetjson = t3k::presetjson;

namespace {

constexpr const char* kSetlistMetaFile = "setlist.json";
constexpr const char* kDerivedSetlistPrefix = "folder:";
constexpr const char* kDerivedPresetPrefix = "file:";
constexpr const char* kLegacyMigratedKey = "legacyPresetsMigrated";

// Process-wide test overrides (setTrashDirForTesting, the scan observer).
// The root's override is LibraryPaths', where the root is resolved.
juce::CriticalSection overrideLock;
juce::File trashOverride;
std::function<void()> scanObserver;

void noteScan() {
  std::function<void()> observer;
  {
    const juce::ScopedLock guard(overrideLock);
    observer = scanObserver;
  }
  if (observer)
    observer();
}

struct ComposePair {
  juce::juce_wchar first, second, composed;
};
constexpr ComposePair kComposePairs[] = {
#include "UnicodeComposePairs.inc"
};

// The canonical composition of two code points, or 0.
juce::juce_wchar compose(juce::juce_wchar first, juce::juce_wchar second) {
  // Hangul: a leading consonant and a vowel, then that syllable and a
  // trailing consonant (Unicode, section 3.12).
  constexpr juce::juce_wchar sBase = 0xAC00, lBase = 0x1100, vBase = 0x1161, tBase = 0x11A7;
  constexpr int lCount = 19, vCount = 21, tCount = 28;
  if (first >= lBase && first < lBase + lCount && second >= vBase && second < vBase + vCount)
    return static_cast<juce::juce_wchar>(sBase +
                                         ((first - lBase) * vCount + (second - vBase)) * tCount);
  if (first >= sBase && first < sBase + lCount * vCount * tCount &&
      (first - sBase) % tCount == 0 && second > tBase && second < tBase + tCount)
    return static_cast<juce::juce_wchar>(first + (second - tBase));

  const auto* end = std::end(kComposePairs);
  const auto* it = std::lower_bound(std::begin(kComposePairs), end, std::pair{first, second},
                                    [](const ComposePair& pair, const auto& key) {
                                      return std::pair{pair.first, pair.second} < key;
                                    });
  return it != end && it->first == first && it->second == second ? it->composed : 0;
}

// Characters that take no room and tell two names apart only to a computer.
bool isInvisible(juce::juce_wchar c) {
  return c == 0x00AD || (c >= 0x200B && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) ||
         (c >= 0x2060 && c <= 0x2069) || c == 0xFEFF;
}

// Lower case for the scripts a setlist is likely to be named in. Our own
// table, not towlower: what counts as the same name must not depend on the
// machine's locale.
juce::juce_wchar foldCase(juce::juce_wchar character) {
  // In a type of one width everywhere (juce_wchar is 16 bits on Windows).
  const auto c = static_cast<juce::uint32>(character);
  juce::uint32 folded = c;
  if (c >= 'A' && c <= 'Z') {
    folded = c + 0x20;
  } else if (c >= 0xC0 && c <= 0xDE) {  // Latin-1
    folded = c == 0xD7 ? c : c + 0x20;
  } else if (c >= 0x100 && c <= 0x17F) {  // Latin Extended-A
    const bool caseless = c == 0x130 || c == 0x131 || c == 0x138 || c == 0x149 || c == 0x17F;
    const bool upperIsOdd = (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E);
    if (c == 0x178)
      folded = 0xFF;
    else if (!caseless && (c % 2 == 1) == upperIsOdd)
      folded = c + 1;
  } else if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) {  // Greek
    folded = c + 0x20;
  } else if (c >= 0x400 && c <= 0x40F) {  // Cyrillic
    folded = c + 0x50;
  } else if (c >= 0x410 && c <= 0x42F) {
    folded = c + 0x20;
  }
  return static_cast<juce::juce_wchar>(folded);
}

juce::String foldName(const juce::String& name) {
  juce::String out;
  const juce::String composed = SetlistLibrary::normaliseName(name);
  for (auto t = composed.getCharPointer(); !t.isEmpty();)
    out += juce::String::charToString(foldCase(t.getAndAdvance()));
  return out;
}

juce::String newId() { return juce::Uuid().toString(); }

juce::String withoutPrefix(const juce::String& presetId) {
  return presetId.substring(static_cast<int>(juce::String(SetlistLibrary::kIdPrefix).length()));
}

// A file as JSON, or a void var: one that is too large, nests too deep or is
// not JSON is none (see presetjson::readJsonFile).
juce::var parseFile(const juce::File& file, juce::int64 maxBytes) {
  juce::var parsed;
  if (presetjson::readJsonFile(file, maxBytes, parsed).failed())
    return {};
  return parsed;
}

juce::var parsePresetFile(const juce::File& file) {
  return parseFile(file, presetjson::kMaxPresetFileBytes);
}

juce::var parseMetaFile(const juce::File& file) {
  return parseFile(file, presetjson::kMaxMetaFileBytes);
}

juce::StringArray stringsOf(const juce::var& list) {
  juce::StringArray out;
  if (const auto* items = list.getArray())
    for (const auto& item : *items)
      if (item.toString().isNotEmpty())
        out.add(item.toString());
  return out;
}

// Copies onto `fresh` (a preset just written from the rig) the top-level
// keys of `old` (the file it replaces) that the rig does not write: a tempo,
// tags, a note someone added by hand. They go before the chain, where the
// rest of the header is. False when there was nothing to copy.
bool carryOverHeader(const juce::var& old, juce::var& fresh) {
  const auto* oldObject = old.getDynamicObject();
  const auto* freshObject = fresh.getDynamicObject();
  if (oldObject == nullptr || freshObject == nullptr)
    return false;
  const auto isBody = [](const juce::String& key) {
    return key == "chain" || key == "params" || key == "children";
  };
  juce::NamedValueSet extras;
  for (const auto& property : oldObject->getProperties())
    if (!isBody(property.name.toString()) && !freshObject->hasProperty(property.name))
      extras.set(property.name, property.value);
  if (extras.isEmpty())
    return false;

  juce::DynamicObject::Ptr merged = new juce::DynamicObject();
  bool placed = false;
  for (const auto& property : freshObject->getProperties()) {
    if (!placed && isBody(property.name.toString())) {
      for (const auto& extra : extras)
        merged->setProperty(extra.name, extra.value);
      placed = true;
    }
    merged->setProperty(property.name, property.value);
  }
  if (!placed)
    for (const auto& extra : extras)
      merged->setProperty(extra.name, extra.value);
  fresh = juce::var(merged.get());
  return true;
}

juce::var varOf(const juce::StringArray& strings) {
  juce::Array<juce::var> list;
  for (const auto& text : strings)
    list.add(text);
  return list;
}

}  // namespace

// ####################
// NAMES
// ####################

juce::String SetlistLibrary::normaliseName(const juce::String& name) {
  std::vector<juce::juce_wchar> out;
  for (auto t = name.getCharPointer(); !t.isEmpty();) {
    const juce::juce_wchar c = t.getAndAdvance();
    if (const juce::juce_wchar joined = out.empty() ? 0 : compose(out.back(), c); joined != 0)
      out.back() = joined;
    else
      out.push_back(c);
  }
  juce::String result;
  for (const juce::juce_wchar c : out)
    result += juce::String::charToString(c);
  return result;
}

juce::String SetlistLibrary::sanitiseComponent(const juce::String& name,
                                               const juce::String& fallback) {
  juce::String visible;
  const juce::String composed = normaliseName(name);
  for (auto t = composed.getCharPointer(); !t.isEmpty();) {
    const juce::juce_wchar c = t.getAndAdvance();
    if (!isInvisible(c))
      visible += juce::String::charToString(c);
  }
  const juce::String stem = t3k::presetfile::sanitizeStem(visible);
  // sanitizeStem's own floor is "Preset"; anything else that ends up as
  // that word had nothing usable in it.
  if (stem == "Preset" && !visible.containsIgnoreCase("Preset"))
    return fallback;
  return stem;
}

bool SetlistLibrary::namesCollide(const juce::String& a, const juce::String& b) {
  return foldName(a) == foldName(b);
}

juce::String SetlistLibrary::slotFileName(int slot, const juce::String& name) {
  return juce::String(slot).paddedLeft('0', 3) + " " + sanitiseComponent(name, "Preset") +
         presetjson::kExtension;
}

bool SetlistLibrary::parseSlotFileName(const juce::String& fileName, int& slot,
                                       juce::String& stem) {
  slot = 0;
  stem = fileName.endsWithIgnoreCase(presetjson::kExtension)
             ? fileName.dropLastCharacters(5)
             : fileName;
  int digits = 0;
  while (digits < stem.length() && juce::CharacterFunctions::isDigit(stem[digits]))
    ++digits;
  // One to three digits, then the end or a separator: "2024 Tour" and
  // "007Bond" are names, not slots.
  if (digits < 1 || digits > 3)
    return false;
  const juce::juce_wchar next = stem[digits];
  if (next != 0 && next != ' ' && next != '-' && next != '_' && next != '.')
    return false;
  const int number = stem.substring(0, digits).getIntValue();
  if (number < 1 || number > kMaxSlots)
    return false;
  slot = number;
  stem = stem.substring(digits).trimCharactersAtStart(" -_.").trim();
  return true;
}

// ####################
// SET UP
// ####################

SetlistLibrary::SetlistLibrary(const juce::File& root)
    : rootDir(root), assetStore(root.getChildFile("Assets")) {}

juce::File SetlistLibrary::defaultRoot() { return LibraryPaths::libraryRoot(); }

juce::File SetlistLibrary::setRootForTesting(const juce::File& dir) {
  return LibraryPaths::setLibraryRootForTesting(dir);
}

std::function<void()> SetlistLibrary::setScanObserverForTesting(std::function<void()> observer) {
  const juce::ScopedLock guard(overrideLock);
  return std::exchange(scanObserver, std::move(observer));
}

bool SetlistLibrary::rootIsOverridden() { return LibraryPaths::libraryRootIsOverridden(); }

juce::File SetlistLibrary::setTrashDirForTesting(const juce::File& dir) {
  const juce::ScopedLock guard(overrideLock);
  return std::exchange(trashOverride, dir);
}

void SetlistLibrary::forgetCache() {
  const juce::ScopedLock guard(lock);
  cache.clear();
}

// ####################
// FILES
// ####################

juce::Result SetlistLibrary::writeText(const juce::File& file, const juce::String& text) {
  // Write-then-replace: the file is the old text or the new, never half of
  // one and never neither (presetjson::writeFileAtomically).
  const juce::Result written = presetjson::writeFileAtomically(file, text);
  if (written.failed())
    juce::Logger::writeToLog("[Library] " + written.getErrorMessage());
  return written;
}

bool SetlistLibrary::renameAll(const std::vector<std::pair<juce::File, juce::File>>& renames) {
  // Nothing here ever deletes, and moveFileTo would delete a file in the
  // way: a target that exists has to be one of the files that are moving
  // (two presets trading names, or one changing only the case of its own).
  for (const auto& [from, to] : renames) {
    if (from.getFullPathName() == to.getFullPathName() || !to.exists())
      continue;
    // Our own folding, not File's ==: whether "001 CAFÉ" is "001 café" must
    // be what namesCollide says, not what this machine's locale makes of an
    // accented letter.
    bool moving = false;
    for (const auto& other : renames)
      moving = moving || (other.first.getParentDirectory() == to.getParentDirectory() &&
                          namesCollide(other.first.getFileName(), to.getFileName()));
    if (!moving)
      return false;
  }

  // Through a scratch name each, so the trade works in any order and on
  // every file system. The scratch names are still preset files, and each
  // starts with the number of the slot its file is on its way to: should the
  // process die half way, the next scan lists every file that had left its
  // old name in the slot it was meant for, not in whichever one is free.
  std::vector<std::pair<juce::File, juce::File>> parked;
  bool ok = true;
  for (const auto& [from, to] : renames) {
    if (from.getFullPathName() == to.getFullPathName())
      continue;
    int targetSlot = 0, ownSlot = 0;
    juce::String targetStem, ownStem;
    const juce::String tag =
        "moving-" + juce::String::toHexString(juce::Random::getSystemRandom().nextInt());
    parseSlotFileName(from.getFileName(), ownSlot, ownStem);
    const juce::File scratch = from.getSiblingFile(
        parseSlotFileName(to.getFileName(), targetSlot, targetStem)
            ? juce::String(targetSlot).paddedLeft('0', 3) + " " + tag + " " + ownStem +
                  presetjson::kExtension
            : tag + " " + from.getFileName());
    if (from.moveFileTo(scratch))
      parked.emplace_back(scratch, to);
    else
      ok = false;
    // What a path said is no longer what is at that path. The time and size
    // check cannot be trusted for this: two files of one size written in the
    // same second look alike on a file system with one second times.
    cache.erase(from.getFullPathName());
    cache.erase(to.getFullPathName());
  }
  // (A test's stand-in for the process dying here.)
  if (presetjson::fileFaultForTesting() == presetjson::FileFault::renameHalf)
    return false;
  for (const auto& [scratch, to] : parked)
    if (to.exists() || !scratch.moveFileTo(to))
      ok = false;
  return ok;
}

bool SetlistLibrary::trash(const juce::File& file) const { return moveToTrash(file, rootDir); }

bool SetlistLibrary::moveToTrash(const juce::File& file, const juce::File& libraryRoot) {
  if (!file.exists())
    return false;
  juce::File testTrash;
  {
    const juce::ScopedLock guard(overrideLock);
    testTrash = trashOverride;
  }
  if (testTrash == juce::File() && file.moveToTrash())
    return true;
  // No system trash to be had (a headless Linux box, a test run): a Trash
  // folder of our own. Never a delete.
  const juce::File folder =
      testTrash != juce::File() ? testTrash : libraryRoot.getChildFile("Trash");
  if (!folder.createDirectory())
    return false;
  juce::File target = folder.getChildFile(file.getFileName());
  if (target.exists())
    target = folder.getChildFile(file.getFileNameWithoutExtension() + " " + newId().substring(0, 8) +
                                 file.getFileExtension());
  return file.moveFileTo(target);
}

presetjson::Meta SetlistLibrary::metaOf(const juce::File& file) const {
  // Same (time, size) as last time: the file says what it said then. Every
  // write here is write-then-rename, so a change always moves the time.
  const juce::int64 modificationMs = file.getLastModificationTime().toMilliseconds();
  const juce::int64 size = file.getSize();
  Cached& cached = cache[file.getFullPathName()];
  if (cached.modificationMs != modificationMs || cached.size != size ||
      (modificationMs == 0 && size == 0)) {
    cached.modificationMs = modificationMs;
    cached.size = size;
    // Behind the size and depth limits: a file too large or nested too deep
    // to be a preset is listed as ignored with that for a reason.
    cached.meta = presetjson::readMetaFile(file);
  }
  return cached.meta;
}

// ####################
// LIBRARY STATE
// ####################

juce::var SetlistLibrary::readState(bool* broken) const {
  if (broken != nullptr)
    *broken = false;
  juce::var state;
  const juce::File file = stateFile();
  if (file.existsAsFile()) {
    juce::String text;
    const juce::Result read =
        presetjson::readTextFile(file, presetjson::kMaxMetaFileBytes, text);
    // An empty file has nothing in it to lose; anything else that is there
    // and is not a JSON object (unreadable, cut short, a merge conflict,
    // too large) is a broken one.
    const bool blank = read.wasOk() && text.trim().isEmpty();
    if (!blank && (read.failed() || presetjson::parseJsonGuarded(text, state).failed() ||
                   state.getDynamicObject() == nullptr)) {
      state = juce::var();
      if (broken != nullptr)
        *broken = true;
    }
  }
  // Missing, empty or broken: the defaults, in memory.
  if (state.getDynamicObject() == nullptr) {
    juce::DynamicObject::Ptr fresh = new juce::DynamicObject();
    fresh->setProperty("format", kLibraryFormat);
    fresh->setProperty("version", 1);
    state = juce::var(fresh.get());
  }
  return state;
}

bool SetlistLibrary::writeState(const juce::var& state) const {
  if (!rootDir.createDirectory())
    return false;
  const juce::String text = presetjson::writeCanonical(state);
  const juce::File file = stateFile();
  if (file.existsAsFile()) {
    // What is on disk now, not what the caller read a moment ago.
    bool broken = false;
    readState(&broken);
    if (broken) {
      // Never written over as it is: what it held (favourites, recents, the
      // setlist order) may still be got out of it by hand. A copy is put
      // aside first; without one the file stays and nothing is written.
      if (!setBrokenStateAside(file))
        return false;
    } else {
      // Nothing to say that the file does not say already: leave its time
      // alone (and git's view of it).
      juce::String onDisk;
      if (presetjson::readTextFile(file, presetjson::kMaxMetaFileBytes, onDisk).wasOk() &&
          onDisk == text)
        return true;
    }
  }
  return writeText(file, text).wasOk();
}

bool SetlistLibrary::setBrokenStateAside(const juce::File& file) const {
  // Once: a copy of these very bytes from an earlier try (the write after
  // it failed, or the same conflict came back) is the copy.
  for (const auto& earlier : file.getParentDirectory().findChildFiles(
           juce::File::findFiles, false, file.getFileName() + ".broken-*"))
    if (earlier.getSize() == file.getSize() && earlier.hasIdenticalContentTo(file))
      return true;
  const juce::String stamp = juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S");
  juce::File copy = file.getSiblingFile(file.getFileName() + ".broken-" + stamp);
  for (int n = 2; copy.exists(); ++n)
    copy = file.getSiblingFile(file.getFileName() + ".broken-" + stamp + "-" + juce::String(n));
  if (!file.copyFileTo(copy)) {
    juce::Logger::writeToLog("[Library] " + file.getFullPathName() +
                             " cannot be read as a library state and could not be copied "
                             "aside; it is left as it is and nothing is saved to it");
    return false;
  }
  juce::Logger::writeToLog("[Library] " + file.getFullPathName() +
                           " could not be read as a library state; kept as " +
                           copy.getFileName() + " and started again");
  return true;
}

bool SetlistLibrary::legacyMigrationRecorded() const {
  const juce::ScopedLock guard(lock);
  return static_cast<bool>(readState()[kLegacyMigratedKey]);
}

bool SetlistLibrary::recordLegacyMigration() {
  const juce::ScopedLock guard(lock);
  juce::var state = readState();
  state.getDynamicObject()->setProperty(kLegacyMigratedKey, true);
  return writeState(state);
}

void SetlistLibrary::holdPosition(const Position& position) {
  const juce::ScopedLock guard(lock);
  ownPosition = position;
}

SetlistLibrary::Position SetlistLibrary::current() const {
  const juce::ScopedLock guard(lock);
  if (ownPosition.has_value())
    return *ownPosition;
  const juce::var saved = readState()["current"];
  Position position;
  position.setlistId = saved["setlist"].toString();
  position.slot = juce::jlimit(0, kMaxSlots, static_cast<int>(saved["slot"]));
  return position;
}

void SetlistLibrary::setCurrent(const Position& position) {
  const juce::ScopedLock guard(lock);
  ownPosition = position;
  if (guest)
    return;
  juce::var state = readState();
  juce::DynamicObject::Ptr saved = new juce::DynamicObject();
  saved->setProperty("setlist", position.setlistId);
  saved->setProperty("slot", position.slot);
  state.getDynamicObject()->setProperty("current", juce::var(saved.get()));
  writeState(state);
}

juce::StringArray SetlistLibrary::favourites() const {
  const juce::ScopedLock guard(lock);
  return stringsOf(readState()["favourites"]);
}

bool SetlistLibrary::isFavourite(const juce::String& presetId) const {
  return favourites().contains(presetId);
}

bool SetlistLibrary::setFavourite(const juce::String& presetId, bool favourite) {
  const juce::ScopedLock guard(lock);
  juce::var state = readState();
  juce::StringArray ids = stringsOf(state["favourites"]);
  if (ids.contains(presetId) == favourite)
    return true;
  if (favourite)
    ids.add(presetId);
  else
    ids.removeString(presetId);
  // Sorted: the file is a set, and two machines adding favourites should
  // merge line by line.
  ids.sort(false);
  state.getDynamicObject()->setProperty("favourites", varOf(ids));
  return writeState(state);
}

juce::StringArray SetlistLibrary::recents() const {
  const juce::ScopedLock guard(lock);
  return stringsOf(readState()["recents"]);
}

void SetlistLibrary::noteRecent(const juce::String& presetId) {
  if (presetId.isEmpty())
    return;
  const juce::ScopedLock guard(lock);
  if (guest)
    return;
  juce::var state = readState();
  juce::StringArray ids = stringsOf(state["recents"]);
  if (!ids.isEmpty() && ids[0] == presetId)
    return;  // already on top: no write per reload of the same preset
  ids.removeString(presetId);
  ids.insert(0, presetId);
  while (ids.size() > kMaxRecents)
    ids.remove(ids.size() - 1);
  state.getDynamicObject()->setProperty("recents", varOf(ids));
  writeState(state);
}

void SetlistLibrary::noteLoaded(const juce::String& presetId,
                                const std::optional<Position>& position) {
  const juce::ScopedLock guard(lock);
  if (guest) {
    if (position.has_value())
      ownPosition = position;
    return;
  }
  juce::var state = readState();
  if (presetId.isNotEmpty()) {
    juce::StringArray ids = stringsOf(state["recents"]);
    ids.removeString(presetId);
    ids.insert(0, presetId);
    while (ids.size() > kMaxRecents)
      ids.remove(ids.size() - 1);
    state.getDynamicObject()->setProperty("recents", varOf(ids));
  }
  if (position.has_value()) {
    ownPosition = position;
    juce::DynamicObject::Ptr saved = new juce::DynamicObject();
    saved->setProperty("setlist", position->setlistId);
    saved->setProperty("slot", position->slot);
    state.getDynamicObject()->setProperty("current", juce::var(saved.get()));
  }
  writeState(state);  // a no-op when the file says all of this already
}

void SetlistLibrary::replaceSetlistId(const juce::String& oldId, const juce::String& newId) {
  juce::var state = readState();
  juce::StringArray order = stringsOf(state["setlistOrder"]);
  bool changed = false;
  for (auto& id : order)
    if (id == oldId) {
      id = newId;
      changed = true;
    }
  if (changed)
    state.getDynamicObject()->setProperty("setlistOrder", varOf(order));
  if (state["current"]["setlist"].toString() == oldId) {
    state["current"].getDynamicObject()->setProperty("setlist", newId);
    changed = true;
  }
  if (ownPosition.has_value() && ownPosition->setlistId == oldId)
    ownPosition->setlistId = newId;
  if (changed)
    writeState(state);
}

// ####################
// SETLISTS
// ####################

SetlistLibrary::SetlistMeta SetlistLibrary::readSetlistMeta(const juce::File& folder) const {
  SetlistMeta meta;
  const juce::File file = folder.getChildFile(kSetlistMetaFile);
  if (!file.existsAsFile())
    return meta;
  const juce::var parsed = parseMetaFile(file);
  if (parsed.getDynamicObject() == nullptr || parsed["format"].toString() != kSetlistFormat)
    return meta;
  meta.valid = true;
  meta.id = parsed["id"].toString().trim();
  meta.name = parsed["name"].toString().trim();
  return meta;
}

bool SetlistLibrary::writeSetlistMeta(const juce::File& folder, const juce::String& id,
                                      const juce::String& name) const {
  // Onto what the file holds, so keys this build does not know survive.
  juce::var parsed = parseMetaFile(folder.getChildFile(kSetlistMetaFile));
  if (parsed.getDynamicObject() == nullptr || parsed["format"].toString() != kSetlistFormat) {
    juce::DynamicObject::Ptr fresh = new juce::DynamicObject();
    fresh->setProperty("format", kSetlistFormat);
    fresh->setProperty("version", 1);
    parsed = juce::var(fresh.get());
  }
  parsed.getDynamicObject()->setProperty("id", id);
  parsed.getDynamicObject()->setProperty("name", name);
  return writeText(folder.getChildFile(kSetlistMetaFile), presetjson::writeCanonical(parsed))
      .wasOk();
}

std::vector<SetlistLibrary::Setlist> SetlistLibrary::setlists() const {
  const juce::ScopedLock guard(lock);
  noteScan();
  std::vector<Setlist> out;
  if (!setlistsDir().isDirectory())
    return out;

  auto folders = setlistsDir().findChildFiles(
      juce::File::findDirectories | juce::File::ignoreHiddenFiles, false);
  // Name order, so which of two folders claiming one id keeps it does not
  // depend on the order the OS lists them in.
  std::sort(folders.begin(), folders.end(), [](const juce::File& a, const juce::File& b) {
    const int order = a.getFileName().compareIgnoreCase(b.getFileName());
    return order != 0 ? order < 0 : a.getFileName().compare(b.getFileName()) < 0;
  });

  std::set<juce::String> ids;
  for (const auto& folder : folders) {
    if (folder.getFileName().startsWithChar('.'))
      continue;
    const SetlistMeta meta = readSetlistMeta(folder);
    Setlist setlist;
    setlist.folder = folder;
    setlist.name = meta.name.isNotEmpty() ? meta.name : normaliseName(folder.getFileName());
    setlist.id = meta.id;
    // No id of its own (a folder made by hand), or the id of a folder it was
    // copied from: known by its folder name until it is written to.
    if (setlist.id.isEmpty() || ids.count(setlist.id) != 0)
      setlist.id = kDerivedSetlistPrefix + folder.getFileName();
    if (!ids.insert(setlist.id).second)
      continue;
    out.push_back(std::move(setlist));
  }

  // The saved order first; setlists it does not name (made on another
  // machine, made by hand) follow by name.
  const juce::StringArray order = stringsOf(readState()["setlistOrder"]);
  std::stable_sort(out.begin(), out.end(), [&order](const Setlist& a, const Setlist& b) {
    const int ia = order.indexOf(a.id), ib = order.indexOf(b.id);
    if (ia >= 0 || ib >= 0)
      return ia >= 0 && (ib < 0 || ia < ib);
    return a.name.compareIgnoreCase(b.name) < 0;
  });
  return out;
}

std::optional<SetlistLibrary::Setlist> SetlistLibrary::findSetlist(const juce::String& id) const {
  if (id.isEmpty())
    return std::nullopt;
  for (auto& setlist : setlists())
    if (setlist.id == id)
      return setlist;
  return std::nullopt;
}

SetlistLibrary::Setlist SetlistLibrary::withMeta(const Setlist& setlist) {
  const SetlistMeta meta = readSetlistMeta(setlist.folder);
  if (meta.valid && meta.id == setlist.id)
    return setlist;
  Setlist made = setlist;
  made.id = newId();
  if (!writeSetlistMeta(setlist.folder, made.id, setlist.name))
    return setlist;
  replaceSetlistId(setlist.id, made.id);
  return made;
}

juce::File SetlistLibrary::uniqueFolder(const juce::String& name, const juce::File& self) const {
  const juce::String stem = sanitiseComponent(name, "Setlist");
  // Against the names that are there, folded: "Blues" and "blues" are one
  // folder on a Mac or on Windows whatever this file system thinks.
  juce::StringArray taken;
  for (const auto& entry : setlistsDir().findChildFiles(juce::File::findFilesAndDirectories, false))
    if (entry != self)
      taken.add(entry.getFileName());
  const auto isTaken = [&taken, &self, this](const juce::String& candidate) {
    for (const auto& existing : taken)
      if (namesCollide(existing, candidate))
        return true;
    // And against the disk itself: this file system may take two names for
    // one where our own folding does not (a script it does not cover), and
    // a folder that is there is somebody's, never ours to move into.
    const juce::File onDisk = setlistsDir().getChildFile(candidate);
    return onDisk.exists() && (self == juce::File() || onDisk != self);
  };
  juce::String candidate = stem;
  for (int n = 2; isTaken(candidate); ++n)
    candidate = stem + " " + juce::String(n);
  return setlistsDir().getChildFile(candidate);
}

juce::Result SetlistLibrary::createSetlist(const juce::String& rawName, Setlist* created) {
  const juce::ScopedLock guard(lock);
  const juce::String name = normaliseName(rawName.trim());
  if (name.isEmpty())
    return juce::Result::fail("a setlist needs a name");
  const auto existing = setlists();
  for (const auto& setlist : existing)
    if (namesCollide(setlist.name, name))
      return juce::Result::fail("there is already a setlist called '" + setlist.name + "'");
  if (!setlistsDir().createDirectory())
    return juce::Result::fail("cannot create " + setlistsDir().getFullPathName());

  Setlist setlist;
  setlist.id = newId();
  setlist.name = name;
  setlist.folder = uniqueFolder(name);
  // A new folder of its own, or nothing: createDirectory is content with a
  // folder that is there already, and its setlist.json is not ours to write.
  if (setlist.folder.exists() || !setlist.folder.createDirectory() ||
      !writeSetlistMeta(setlist.folder, setlist.id, name))
    return juce::Result::fail("cannot create " + setlist.folder.getFullPathName());

  // Last in the list, wherever its name would sort.
  juce::var state = readState();
  juce::StringArray order;
  for (const auto& known : existing)
    order.add(known.id);
  order.add(setlist.id);
  state.getDynamicObject()->setProperty("setlistOrder", varOf(order));
  writeState(state);
  if (created != nullptr)
    *created = setlist;
  return juce::Result::ok();
}

juce::Result SetlistLibrary::renameSetlist(const juce::String& id, const juce::String& rawName) {
  const juce::ScopedLock guard(lock);
  const juce::String name = normaliseName(rawName.trim());
  if (name.isEmpty())
    return juce::Result::fail("a setlist needs a name");
  auto found = findSetlist(id);
  if (!found.has_value())
    return juce::Result::fail("no setlist with id '" + id + "'");
  for (const auto& other : setlists())
    if (other.id != id && namesCollide(other.name, name))
      return juce::Result::fail("there is already a setlist called '" + other.name + "'");

  Setlist setlist = withMeta(*found);
  if (!writeSetlistMeta(setlist.folder, setlist.id, name))
    return juce::Result::fail("cannot write " + setlist.folder.getFullPathName());
  const juce::File target = uniqueFolder(name, setlist.folder);
  if (target.getFullPathName() != setlist.folder.getFullPathName() &&
      !setlist.folder.moveFileTo(target))
    // The name is in setlist.json, which is what counts; the folder keeps
    // its old spelling.
    juce::Logger::writeToLog("[Library] Could not rename folder " +
                             setlist.folder.getFullPathName());
  cache.clear();
  return juce::Result::ok();
}

juce::Result SetlistLibrary::deleteSetlist(const juce::String& id) {
  const juce::ScopedLock guard(lock);
  const auto found = findSetlist(id);
  if (!found.has_value())
    return juce::Result::fail("no setlist with id '" + id + "'");
  if (!trash(found->folder))
    return juce::Result::fail("could not move " + found->folder.getFullPathName() +
                              " to the trash");

  juce::var state = readState();
  juce::StringArray order = stringsOf(state["setlistOrder"]);
  order.removeString(id);
  state.getDynamicObject()->setProperty("setlistOrder", varOf(order));
  if (state["current"]["setlist"].toString() == id)
    state.getDynamicObject()->removeProperty("current");
  writeState(state);
  if (ownPosition.has_value() && ownPosition->setlistId == id)
    ownPosition.reset();
  cache.clear();
  return juce::Result::ok();
}

juce::Result SetlistLibrary::reorderSetlist(const juce::String& id, int index) {
  const juce::ScopedLock guard(lock);
  juce::StringArray order;
  for (const auto& setlist : setlists())
    order.add(setlist.id);
  const int from = order.indexOf(id);
  if (from < 0)
    return juce::Result::fail("no setlist with id '" + id + "'");
  order.move(from, juce::jlimit(0, order.size() - 1, index));
  juce::var state = readState();
  state.getDynamicObject()->setProperty("setlistOrder", varOf(order));
  return writeState(state) ? juce::Result::ok()
                           : juce::Result::fail("cannot write " + stateFile().getFullPathName());
}

// ####################
// LISTING
// ####################

const SetlistLibrary::Slot* SetlistLibrary::Listing::at(int slot) const {
  for (const auto& entry : slots)
    if (entry.slot == slot)
      return &entry;
  return nullptr;
}

int SetlistLibrary::Listing::firstFree(int from) const {
  for (int i = 0; i < kMaxSlots; ++i) {
    const int slot = (juce::jlimit(1, kMaxSlots, from) - 1 + i) % kMaxSlots + 1;
    if (at(slot) == nullptr)
      return slot;
  }
  return 0;
}

SetlistLibrary::Listing SetlistLibrary::list(const juce::String& setlistId) const {
  const juce::ScopedLock guard(lock);
  const auto setlist = findSetlist(setlistId);
  return setlist.has_value() ? listFolder(*setlist) : Listing();
}

SetlistLibrary::Listing SetlistLibrary::tidied(const juce::String& setlistId) {
  Listing listing = list(setlistId);
  for (const auto& slot : listing.slots)
    if (!slot.tidy) {
      tidy(setlistId);
      return list(setlistId);
    }
  return listing;
}

SetlistLibrary::Listing SetlistLibrary::listFolder(const Setlist& setlist) const {
  const juce::ScopedLock guard(lock);
  noteScan();
  Listing out;
  out.found = true;
  out.setlist = setlist;

  auto files = setlist.folder.findChildFiles(
      juce::File::findFiles | juce::File::ignoreHiddenFiles, false);
  // File name order decides which of two files claiming one slot keeps it.
  std::sort(files.begin(), files.end(), [](const juce::File& a, const juce::File& b) {
    const int order = a.getFileName().compareIgnoreCase(b.getFileName());
    return order != 0 ? order < 0 : a.getFileName().compare(b.getFileName()) < 0;
  });

  struct Candidate {
    juce::File file;
    presetjson::Meta meta;
    juce::String stem;
    int wanted{0};
  };
  std::map<int, Candidate> bySlot;
  std::vector<Candidate> waiting;
  for (const auto& file : files) {
    const juce::String fileName = file.getFileName();
    // Whatever else is in the folder (notes, a README, .DS_Store, an editor's
    // backup) is not ours to judge: only .json files are looked at.
    if (fileName.startsWithChar('.') || !file.hasFileExtension("json") ||
        fileName.equalsIgnoreCase(kSetlistMetaFile))
      continue;
    Candidate candidate;
    candidate.file = file;
    candidate.meta = metaOf(file);
    if (!candidate.meta.valid) {
      out.ignored.push_back({fileName, candidate.meta.problem});
      continue;
    }
    if (parseSlotFileName(fileName, candidate.wanted, candidate.stem) &&
        bySlot.count(candidate.wanted) == 0)
      bySlot.emplace(candidate.wanted, std::move(candidate));
    else
      waiting.push_back(std::move(candidate));
  }

  // No number, or a number that was taken: the next free slot from the one
  // it asked for. Nothing is renamed here.
  for (auto& candidate : waiting) {
    int slot = 0;
    for (int i = 0; i < kMaxSlots && slot == 0; ++i) {
      const int trySlot = (juce::jmax(1, candidate.wanted) - 1 + i) % kMaxSlots + 1;
      if (bySlot.count(trySlot) == 0)
        slot = trySlot;
    }
    if (slot == 0) {
      out.ignored.push_back({candidate.file.getFileName(), "the setlist is full"});
      continue;
    }
    bySlot.emplace(slot, std::move(candidate));
  }

  std::set<juce::String> ids;
  for (const auto& [slotNumber, candidate] : bySlot) {
    Slot slot;
    slot.slot = slotNumber;
    slot.file = candidate.file;
    slot.name = candidate.meta.name.trim().isNotEmpty()
                    ? candidate.meta.name.trim()
                    : candidate.stem.isNotEmpty() ? normaliseName(candidate.stem)
                                                  : juce::String("Preset");
    slot.sceneCount = candidate.meta.sceneCount;
    slot.tempo = candidate.meta.tempo;
    slot.tags = candidate.meta.tags;
    slot.tidy = candidate.file.getFileName() == slotFileName(slotNumber, slot.name);

    // A file without an id is known by its name; a second file with an id
    // already listed (a copy made in a file browser) by a numbered variant.
    juce::String rawId = candidate.meta.id.isNotEmpty()
                             ? candidate.meta.id
                             : kDerivedPresetPrefix + setlist.folder.getFileName() + "/" +
                                   candidate.stem;
    slot.derivedId = candidate.meta.id.isEmpty();
    for (int n = 2; !ids.insert(rawId).second; ++n) {
      rawId = rawId.upToLastOccurrenceOf("~", false, false) + "~" + juce::String(n);
      slot.derivedId = true;
    }
    slot.id = kIdPrefix + rawId;
    out.slots.push_back(std::move(slot));
  }
  return out;
}

std::optional<SetlistLibrary::Located> SetlistLibrary::find(
    const juce::String& presetId, const juce::String& preferSetlistId) const {
  const juce::ScopedLock guard(lock);
  if (!presetId.startsWith(kIdPrefix))
    return std::nullopt;
  const auto lookIn = [&presetId](const Listing& listing) -> std::optional<Located> {
    for (const auto& slot : listing.slots)
      if (slot.id == presetId)
        return Located{listing.setlist, slot};
    return std::nullopt;
  };
  // The setlists are resolved once for the whole search.
  const auto all = setlists();
  for (const auto& setlist : all)
    if (setlist.id == preferSetlistId)
      if (auto found = lookIn(listFolder(setlist)))
        return found;
  for (const auto& setlist : all)
    if (setlist.id != preferSetlistId)
      if (auto found = lookIn(listFolder(setlist)))
        return found;
  return std::nullopt;
}

bool SetlistLibrary::idInUse(const juce::String& rawId) const {
  return find(kIdPrefix + rawId).has_value();
}

// ####################
// PRESETS
// ####################

juce::Result SetlistLibrary::writeSlot(const Setlist& setlist, int slot, const juce::String& name,
                                       const juce::String& text, const juce::File& replacing,
                                       Slot* written) {
  const juce::File target = setlist.folder.getChildFile(slotFileName(slot, name));
  // The slot's own file under its old name moves first, so the write below
  // replaces it and the file's history follows the rename.
  if (replacing != juce::File() && replacing.getFullPathName() != target.getFullPathName()) {
    if (!renameAll({{replacing, target}}))
      return juce::Result::fail("'" + target.getFileName() + "' is in the way");
  } else if (replacing == juce::File() && target.exists()) {
    // Not a preset the listing knows (it would be in this slot): somebody
    // else's file, left alone.
    return juce::Result::fail("a file that is not a preset is in the way: " +
                              target.getFileName());
  }
  if (const juce::Result result = writeText(target, text); result.failed())
    return result;
  cache.erase(target.getFullPathName());  // see renameAll
  if (written != nullptr) {
    const presetjson::Meta meta = metaOf(target);
    written->slot = slot;
    written->id = kIdPrefix + meta.id;
    written->name = name;
    written->file = target;
    written->sceneCount = meta.sceneCount;
    written->tempo = meta.tempo;
    written->tags = meta.tags;
    written->tidy = true;
    written->derivedId = false;
  }
  return juce::Result::ok();
}

juce::Result SetlistLibrary::save(const juce::String& setlistId, juce::ValueTree preset,
                                  const SaveOptions& options, Slot* saved) {
  const juce::ScopedLock guard(lock);
  const auto found = findSetlist(setlistId);
  if (!found.has_value())
    return juce::Result::fail("no setlist with id '" + setlistId + "'");
  const juce::String name = normaliseName(preset.getProperty("name").toString().trim());
  if (name.isEmpty())
    return juce::Result::fail("a preset needs a name");
  if (options.slot < 0 || options.slot > kMaxSlots)
    return juce::Result::fail("slots are numbered 1 to " + juce::String(kMaxSlots));

  const Setlist setlist = withMeta(*found);
  // Tidied, so a file name below can only be in its own slot's way.
  const Listing listing = tidied(setlist.id);

  int slot = options.slot;
  if (slot == 0) {
    // Saving under the name of a preset of this setlist updates that preset.
    for (const auto& entry : listing.slots)
      if (slot == 0 && namesCollide(entry.name, name))
        slot = entry.slot;
    if (slot == 0)
      slot = listing.firstFree();
    if (slot == 0)
      return juce::Result::fail("the setlist is full (" + juce::String(kMaxSlots) + " presets)");
  }

  // The slot's preset keeps its id through the save, so favourites, recents
  // and a host's saved program keep pointing at it.
  const Slot* existing = listing.at(slot);
  juce::String rawId;
  if (existing != nullptr && !existing->derivedId)
    rawId = withoutPrefix(existing->id);
  else if (existing == nullptr && options.keepId &&
           preset.getProperty("id").toString().trim().isNotEmpty() &&
           !idInUse(preset.getProperty("id").toString().trim()))
    rawId = preset.getProperty("id").toString().trim();
  else
    rawId = newId();

  preset.setProperty("name", name, nullptr);
  preset.setProperty("id", rawId, nullptr);
  presetjson::WriteOptions writeOptions;
  writeOptions.assets = &assetStore;
  juce::String error;
  juce::String text = presetjson::toJson(preset, writeOptions, error);
  if (text.isEmpty())
    return juce::Result::fail(error);
  // Saving the rig over a preset must not cost it what only the file knew.
  if (existing != nullptr) {
    juce::var fresh;
    presetjson::parseJsonGuarded(text, fresh);
    if (carryOverHeader(parsePresetFile(existing->file), fresh))
      text = presetjson::writeCanonical(fresh);
  }
  return writeSlot(setlist, slot, name, text,
                   existing != nullptr ? existing->file : juce::File(), saved);
}

presetjson::ReadResult SetlistLibrary::load(const Located& located) const {
  if (!located.slot.file.existsAsFile()) {
    presetjson::ReadResult result;
    result.error = "the preset file is gone: " + located.slot.file.getFullPathName();
    return result;
  }
  presetjson::ReadOptions options;
  options.assets.push_back(&assetStore);
  presetjson::ReadResult result = presetjson::fromJsonFile(located.slot.file, options);
  // The listing's id and name are the ones callers know the preset by (a
  // file without an id, a name taken from the file name).
  if (result.preset.isValid()) {
    result.preset.setProperty("name", located.slot.name, nullptr);
    result.preset.setProperty("id", withoutPrefix(located.slot.id), nullptr);
  }
  return result;
}

juce::Result SetlistLibrary::rename(const juce::String& setlistId, int slot,
                                    const juce::String& rawName) {
  const juce::ScopedLock guard(lock);
  const juce::String name = normaliseName(rawName.trim());
  if (name.isEmpty())
    return juce::Result::fail("a preset needs a name");
  const Listing listing = tidied(setlistId);
  const Slot* entry = listing.at(slot);
  if (entry == nullptr)
    return juce::Result::fail("slot " + juce::String(slot) + " is empty");

  juce::var parsed = parsePresetFile(entry->file);
  if (parsed.getDynamicObject() == nullptr)
    return juce::Result::fail("cannot read " + entry->file.getFullPathName());
  parsed.getDynamicObject()->setProperty("name", name);
  // A file that came without an id gets one now that it is being written.
  if (entry->derivedId)
    parsed.getDynamicObject()->setProperty("id", newId());
  return writeSlot(withMeta(listing.setlist), slot, name, presetjson::writeCanonical(parsed),
                   entry->file, nullptr);
}

juce::Result SetlistLibrary::move(const juce::String& setlistId, int from, int to, bool insert) {
  const juce::ScopedLock guard(lock);
  if (from < 1 || from > kMaxSlots || to < 1 || to > kMaxSlots)
    return juce::Result::fail("slots are numbered 1 to " + juce::String(kMaxSlots));
  const Listing listing = tidied(setlistId);
  if (!listing.found)
    return juce::Result::fail("no setlist with id '" + setlistId + "'");
  if (listing.at(from) == nullptr)
    return juce::Result::fail("slot " + juce::String(from) + " is empty");
  if (from == to)
    return juce::Result::ok();

  std::vector<std::pair<juce::File, juce::File>> renames;
  const auto renumber = [&](const Slot& entry, int slot) {
    renames.emplace_back(entry.file, listing.setlist.folder.getChildFile(
                                         slotFileName(slot, entry.name)));
  };
  for (const auto& entry : listing.slots) {
    if (entry.slot == from)
      renumber(entry, to);
    else if (!insert && entry.slot == to)
      renumber(entry, from);
    else if (insert && from < to && entry.slot > from && entry.slot <= to)
      renumber(entry, entry.slot - 1);
    else if (insert && from > to && entry.slot >= to && entry.slot < from)
      renumber(entry, entry.slot + 1);
  }
  return renameAll(renames) ? juce::Result::ok()
                            : juce::Result::fail("could not rename the preset files");
}

juce::Result SetlistLibrary::moveByDelta(const juce::String& setlistId, int slot, int delta) {
  const juce::ScopedLock guard(lock);
  const Listing listing = tidied(setlistId);
  int index = -1;
  for (size_t i = 0; i < listing.slots.size(); ++i)
    if (listing.slots[i].slot == slot)
      index = static_cast<int>(i);
  if (index < 0)
    return juce::Result::fail("slot " + juce::String(slot) + " is empty");
  const int count = static_cast<int>(listing.slots.size());
  const int target = juce::jlimit(0, count - 1, index + delta);
  if (target == index)
    return juce::Result::fail("the preset is already at that end of the setlist");

  // The same slot numbers, handed out in the new order.
  std::vector<const Slot*> order;
  for (const auto& entry : listing.slots)
    order.push_back(&entry);
  const Slot* moving = order[static_cast<size_t>(index)];
  order.erase(order.begin() + index);
  order.insert(order.begin() + target, moving);
  std::vector<std::pair<juce::File, juce::File>> renames;
  for (size_t i = 0; i < order.size(); ++i)
    renames.emplace_back(order[i]->file,
                         listing.setlist.folder.getChildFile(
                             slotFileName(listing.slots[i].slot, order[i]->name)));
  return renameAll(renames) ? juce::Result::ok()
                            : juce::Result::fail("could not rename the preset files");
}

juce::Result SetlistLibrary::duplicate(const juce::String& setlistId, int slot, int toSlot,
                                       Slot* made) {
  const juce::ScopedLock guard(lock);
  const Listing listing = tidied(setlistId);
  const Slot* entry = listing.at(slot);
  if (entry == nullptr)
    return juce::Result::fail("slot " + juce::String(slot) + " is empty");
  if (toSlot < 0 || toSlot > kMaxSlots)
    return juce::Result::fail("slots are numbered 1 to " + juce::String(kMaxSlots));
  if (toSlot == 0)
    toSlot = listing.firstFree(slot + 1 > kMaxSlots ? 1 : slot + 1);
  if (toSlot == 0)
    return juce::Result::fail("the setlist is full (" + juce::String(kMaxSlots) + " presets)");
  if (listing.at(toSlot) != nullptr)
    return juce::Result::fail("slot " + juce::String(toSlot) + " is taken");

  juce::var parsed = parsePresetFile(entry->file);
  if (parsed.getDynamicObject() == nullptr)
    return juce::Result::fail("cannot read " + entry->file.getFullPathName());
  // A name of its own: preset.save finds a preset by its name.
  juce::String name = entry->name + " copy";
  const auto taken = [&listing](const juce::String& candidate) {
    for (const auto& other : listing.slots)
      if (namesCollide(other.name, candidate))
        return true;
    return false;
  };
  for (int n = 2; taken(name); ++n)
    name = entry->name + " copy " + juce::String(n);
  parsed.getDynamicObject()->setProperty("name", name);
  parsed.getDynamicObject()->setProperty("id", newId());
  return writeSlot(withMeta(listing.setlist), toSlot, name, presetjson::writeCanonical(parsed), {},
                   made);
}

juce::Result SetlistLibrary::remove(const juce::String& setlistId, int slot) {
  const juce::ScopedLock guard(lock);
  const Listing listing = list(setlistId);
  const Slot* entry = listing.at(slot);
  if (entry == nullptr)
    return juce::Result::fail("slot " + juce::String(slot) + " is empty");
  return trash(entry->file) ? juce::Result::ok()
                            : juce::Result::fail("could not move " +
                                                 entry->file.getFullPathName() + " to the trash");
}

juce::Result SetlistLibrary::copyTo(const juce::String& setlistId, int slot,
                                    const juce::String& toSetlistId, int toSlot, Slot* made) {
  const juce::ScopedLock guard(lock);
  const Listing source = list(setlistId);
  const Slot* entry = source.at(slot);
  if (entry == nullptr)
    return juce::Result::fail("slot " + juce::String(slot) + " is empty");
  if (toSlot < 0 || toSlot > kMaxSlots)
    return juce::Result::fail("slots are numbered 1 to " + juce::String(kMaxSlots));
  juce::var parsed = parsePresetFile(entry->file);
  if (parsed.getDynamicObject() == nullptr)
    return juce::Result::fail("cannot read " + entry->file.getFullPathName());
  parsed.getDynamicObject()->setProperty("name", entry->name);
  parsed.getDynamicObject()->setProperty("id", newId());

  const Listing target = tidied(toSetlistId);
  if (!target.found)
    return juce::Result::fail("no setlist with id '" + toSetlistId + "'");
  if (toSlot == 0)
    toSlot = target.firstFree();
  if (toSlot == 0)
    return juce::Result::fail("the setlist is full (" + juce::String(kMaxSlots) + " presets)");
  if (target.at(toSlot) != nullptr)
    return juce::Result::fail("slot " + juce::String(toSlot) + " is taken");
  return writeSlot(withMeta(target.setlist), toSlot, entry->name,
                   presetjson::writeCanonical(parsed), {}, made);
}

int SetlistLibrary::tidy(const juce::String& setlistId) {
  const juce::ScopedLock guard(lock);
  const Listing listing = list(setlistId);
  std::vector<std::pair<juce::File, juce::File>> renames;
  for (const auto& entry : listing.slots)
    if (!entry.tidy)
      renames.emplace_back(entry.file, listing.setlist.folder.getChildFile(
                                           slotFileName(entry.slot, entry.name)));
  if (renames.empty())
    return 0;
  if (!renameAll(renames))
    juce::Logger::writeToLog("[Library] Could not rename every preset file of " +
                             listing.setlist.folder.getFullPathName());
  return static_cast<int>(renames.size());
}

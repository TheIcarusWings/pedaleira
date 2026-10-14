#include "NamLibrary.h"

#include "LibraryPaths.h"
#include "NAM/wavenet/a2_fast.h"

#include <algorithm>
#include <map>
#include <mutex>

namespace NamLibrary {

namespace {

// Beyond this a capture is not read to be inspected (the loader's own cap on
// local files is 50 MB).
constexpr juce::int64 kMaxInspectBytes = 64 * 1024 * 1024;

const char* const kNotA2Advice =
    "Pedaleira loads only A2 captures, the current NAM format. Look for an A2 version of this "
    "tone on TONE3000, or train it again.";

bool configIsA2(const nlohmann::json& model);

bool wavenetIsA2(const nlohmann::json& model) {
  int channels = 0;
  return model.contains("config") && model["config"].is_object() &&
         nam::wavenet::a2_fast::is_a2_shape(model["config"], &channels);
}

// A container is A2 when it has submodels and every one of them is.
bool containerIsA2(const nlohmann::json& model) {
  if (!model.contains("config") || !model["config"].is_object() ||
      !model["config"].contains("submodels"))
    return false;
  const auto& submodels = model["config"]["submodels"];
  if (!submodels.is_array() || submodels.empty())
    return false;
  for (const auto& entry : submodels)
    if (!entry.is_object() || !entry.contains("model") || !configIsA2(entry["model"]))
      return false;
  return true;
}

bool configIsA2(const nlohmann::json& model) {
  if (!model.is_object())
    return false;
  const std::string architecture = model.value("architecture", "");
  if (architecture == "WaveNet")
    return wavenetIsA2(model);
  if (architecture == "SlimmableContainer")
    return containerIsA2(model);
  return false;
}

Info unreadable(const juce::String& reason) {
  Info info;
  info.reason = reason;
  return info;
}

}  // namespace

// ####################
// THE ARCHITECTURE CHECK
// ####################
Info inspectJson(const nlohmann::json& model) {
  if (!model.is_object() || !model.contains("architecture") || !model["architecture"].is_string())
    return unreadable("This file is not a NAM capture: it names no architecture.");

  Info info;
  info.readable = true;
  // A word out of someone else's file, shown to the player and handed to
  // agents: only letters, digits and a few marks, and not a sentence.
  juce::String named;
  for (const auto c : juce::String(model["architecture"].get<std::string>()))
    if (named.length() < 32 && (juce::CharacterFunctions::isLetterOrDigit(c) || c == ' ' || c == '.' || c == '_' || c == '-'))
      named << juce::String::charToString(c);
  info.architecture = named.trim();
  if (model.contains("metadata") && model["metadata"].is_object()) {
    const auto& metadata = model["metadata"];
    if (metadata.contains("name") && metadata["name"].is_string())
      info.name = juce::String(metadata["name"].get<std::string>()).trim();
  }

  info.supported = configIsA2(model);
  const std::string architecture = model["architecture"].get<std::string>();
  if (architecture == "WaveNet")
    info.label = info.supported ? "WaveNet A2" : "WaveNet A1";
  else if (architecture == "SlimmableContainer")
    info.label = info.supported ? "Slimmable A2" : "Slimmable";
  else
    info.label = info.architecture.isNotEmpty() ? info.architecture : juce::String("Unknown");

  if (!info.supported) {
    juce::String what;
    if (architecture == "WaveNet")
      what = "an older WaveNet architecture (A1)";
    else if (architecture == "SlimmableContainer")
      what = "a slimmable container that holds models that are not A2";
    else if (architecture == "LSTM" || architecture == "ConvNet" || architecture == "Linear")
      what = "the " + info.architecture + " architecture";
    else
      what = "an architecture Pedaleira does not know (\"" + info.architecture + "\")";
    info.reason = "This capture uses " + what + ". " + kNotA2Advice;
  }
  return info;
}

Info inspectBytes(const void* data, size_t size) {
  if (data == nullptr || size == 0)
    return unreadable("This file is empty.");
  const auto* begin = static_cast<const char*>(data);

  // The weights are most of a file and say nothing about its architecture, so
  // the arrays under a "weights" key are dropped as they are parsed.
  // The parser skips the elements of an array the callback drops by itself.
  bool weightsKeyed = false;
  const nlohmann::json::parser_callback_t skipWeights =
      [&weightsKeyed](int, nlohmann::json::parse_event_t event, nlohmann::json& parsed) {
        if (event == nlohmann::json::parse_event_t::key) {
          weightsKeyed = parsed.is_string() && parsed.get<std::string>() == "weights";
          return true;
        }
        const bool drop = weightsKeyed && event == nlohmann::json::parse_event_t::array_start;
        weightsKeyed = false;
        return !drop;
      };

  const nlohmann::json model = nlohmann::json::parse(begin, begin + size, skipWeights, false);
  if (model.is_discarded())
    return unreadable("This file is not a valid NAM capture.");
  return inspectJson(model);
}

namespace {

// What was found for a path, with the size and time of the file it was found
// in: a folder of hundreds of captures is listed again and again, and a file
// is parsed again only when it changed.
struct Inspected {
  juce::int64 size = 0;
  juce::int64 modified = 0;
  Info info;
};
constexpr size_t kMaxInspectedFiles = 5000;

std::mutex& inspectedMutex() {
  static std::mutex m;
  return m;
}
std::map<juce::String, Inspected>& inspectedFiles() {
  static std::map<juce::String, Inspected> files;
  return files;
}

}  // namespace

Info inspectFile(const juce::File& file) {
  if (!file.existsAsFile())
    return unreadable("This file is not there any more.");
  const juce::int64 size = file.getSize();
  if (size > kMaxInspectBytes)
    return unreadable("This file is too large to be a NAM capture.");
  const juce::int64 modified = file.getLastModificationTime().toMilliseconds();
  const juce::String key = file.getFullPathName();
  {
    const std::lock_guard<std::mutex> lock(inspectedMutex());
    const auto found = inspectedFiles().find(key);
    if (found != inspectedFiles().end() && found->second.size == size && found->second.modified == modified)
      return found->second.info;
  }
  juce::MemoryBlock bytes;
  if (!file.loadFileAsData(bytes))
    return unreadable("This file could not be read.");
  const Info info = inspectBytes(bytes.getData(), bytes.getSize());
  const std::lock_guard<std::mutex> lock(inspectedMutex());
  if (inspectedFiles().size() >= kMaxInspectedFiles)
    inspectedFiles().clear();
  inspectedFiles()[key] = {size, modified, info};
  return info;
}

// ####################
// NAMES OF LOADED FILES
// ####################
namespace {

struct Memory {
  std::mutex mutex;
  juce::File readFrom;
  bool valid = false;
  std::vector<Loaded> entries;  // newest first
};

Memory& memory() {
  static Memory instance;
  return instance;
}

juce::File recentsFile() { return LibraryPaths::appDataDir().getChildFile("NamRecents.json"); }

// Caller holds the mutex.
void ensureRead(Memory& m) {
  const juce::File file = recentsFile();
  if (m.valid && m.readFrom == file)
    return;
  m.valid = true;
  m.readFrom = file;
  m.entries.clear();
  if (!file.existsAsFile() || file.getSize() > 8 * 1024 * 1024)
    return;
  const juce::var parsed = juce::JSON::parse(file);
  if (const auto* list = parsed["files"].getArray())
    for (const auto& item : *list) {
      Loaded entry;
      entry.stashFile = item["stash"].toString();
      entry.name = item["name"].toString();
      entry.sourcePath = item["path"].toString();
      entry.kind = item["kind"].toString();
      entry.loadedAt = static_cast<juce::int64>(item["at"]);
      entry.recent = !item.hasProperty("recent") || static_cast<bool>(item["recent"]);
      if (entry.stashFile.isNotEmpty() && entry.name.isNotEmpty())
        m.entries.push_back(entry);
    }
  std::stable_sort(m.entries.begin(), m.entries.end(),
                   [](const Loaded& a, const Loaded& b) { return a.loadedAt > b.loadedAt; });
}

void save(Memory& m) {
  juce::Array<juce::var> list;
  for (const auto& entry : m.entries) {
    juce::DynamicObject::Ptr item = new juce::DynamicObject();
    item->setProperty("stash", entry.stashFile);
    item->setProperty("name", entry.name);
    item->setProperty("path", entry.sourcePath);
    item->setProperty("kind", entry.kind);
    item->setProperty("at", entry.loadedAt);
    item->setProperty("recent", entry.recent);
    list.add(juce::var(item.get()));
  }
  juce::DynamicObject::Ptr root = new juce::DynamicObject();
  root->setProperty("v", 1);
  root->setProperty("files", list);
  const juce::File file = recentsFile();
  file.getParentDirectory().createDirectory();
  file.replaceWithText(juce::JSON::toString(juce::var(root.get()), false));
}

}  // namespace

void noteLoaded(const juce::String& name, const juce::String& sourcePath,
                const juce::String& stashFile) {
  if (name.isEmpty() || stashFile.isEmpty())
    return;
  Memory& m = memory();
  const std::lock_guard<std::mutex> lock(m.mutex);
  ensureRead(m);
  m.entries.erase(std::remove_if(m.entries.begin(), m.entries.end(),
                                 [&](const Loaded& e) { return e.stashFile == stashFile; }),
                  m.entries.end());
  Loaded entry;
  entry.name = name;
  entry.sourcePath = sourcePath;
  entry.stashFile = stashFile;
  entry.kind = stashFile.endsWithIgnoreCase(".wav") ? "ir" : "capture";
  entry.loadedAt = juce::Time::currentTimeMillis();
  m.entries.insert(m.entries.begin(), entry);
  if (static_cast<int>(m.entries.size()) > kMaxRemembered)
    m.entries.resize(static_cast<size_t>(kMaxRemembered));
  save(m);
}

void touchLoaded(const juce::String& stashFile) {
  Memory& m = memory();
  const std::lock_guard<std::mutex> lock(m.mutex);
  ensureRead(m);
  const auto found = std::find_if(m.entries.begin(), m.entries.end(),
                                  [&](const Loaded& e) { return e.stashFile == stashFile; });
  if (found == m.entries.end())
    return;
  Loaded entry = *found;
  entry.loadedAt = juce::Time::currentTimeMillis();
  entry.recent = true;
  m.entries.erase(found);
  m.entries.insert(m.entries.begin(), entry);
  save(m);
}

Loaded loadedFor(const juce::String& stashFileName) {
  Memory& m = memory();
  const std::lock_guard<std::mutex> lock(m.mutex);
  ensureRead(m);
  for (const auto& entry : m.entries)
    if (entry.stashFile == stashFileName)
      return entry;
  return {};
}

std::vector<Loaded> loaded(bool onlyRecent) {
  Memory& m = memory();
  const std::lock_guard<std::mutex> lock(m.mutex);
  ensureRead(m);
  std::vector<Loaded> out;
  for (const auto& entry : m.entries)
    if (!onlyRecent || entry.recent)
      out.push_back(entry);
  return out;
}

void clearRecent() {
  Memory& m = memory();
  const std::lock_guard<std::mutex> lock(m.mutex);
  ensureRead(m);
  for (auto& entry : m.entries)
    entry.recent = false;
  save(m);
}

void resetForTesting() {
  {
    Memory& m = memory();
    const std::lock_guard<std::mutex> lock(m.mutex);
    m.valid = false;
    m.entries.clear();
  }
  const std::lock_guard<std::mutex> lock(inspectedMutex());
  inspectedFiles().clear();
}

// ####################
// THE TONE3000 APP'S FOLDERS
// ####################
std::vector<juce::File> tone3000Candidates(Platform platform, const juce::File& appData,
                                           const juce::File& documents, const juce::File& home) {
  std::vector<juce::File> out;
  // The app's data folder: the one the plugin of the same family keeps its
  // presets, its settings and its stash of local models in.
  out.push_back(appData.getChildFile("TONE3000"));
  // Windows keeps machine bound data in Local, beside Roaming (%APPDATA%).
  if (platform == Platform::windows)
    out.push_back(appData.getSiblingFile("Local").getChildFile("TONE3000"));
  out.push_back(documents.getChildFile("TONE3000"));
  out.push_back(home.getChildFile("Downloads").getChildFile("TONE3000"));
  return out;
}

namespace {
std::mutex& foldersMutex() {
  static std::mutex m;
  return m;
}
std::vector<juce::File>& foldersOverride() {
  static std::vector<juce::File> v;
  return v;
}
bool& foldersOverridden() {
  static bool b = false;
  return b;
}
}  // namespace

std::vector<juce::File> tone3000Folders() {
  std::vector<juce::File> found;
  for (const auto& folder : tone3000Searched())
    if (folder.isDirectory())
      found.push_back(folder);
  return found;
}

std::vector<juce::File> tone3000Searched() {
  {
    const std::lock_guard<std::mutex> lock(foldersMutex());
    if (foldersOverridden())
      return foldersOverride();
  }
  juce::File appData = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
  Platform platform = Platform::other;
#if JUCE_MAC
  appData = appData.getChildFile("Application Support");
  platform = Platform::mac;
#elif JUCE_WINDOWS
  platform = Platform::windows;
#endif
  return tone3000Candidates(platform, appData,
                            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
                            juce::File::getSpecialLocation(juce::File::userHomeDirectory));
}

void setTone3000FoldersForTesting(const std::vector<juce::File>* folders) {
  const std::lock_guard<std::mutex> lock(foldersMutex());
  foldersOverridden() = folders != nullptr;
  foldersOverride() = folders != nullptr ? *folders : std::vector<juce::File>();
}

}  // namespace NamLibrary

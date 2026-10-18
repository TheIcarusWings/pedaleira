#include "NamLibrary.h"

#include "LibraryPaths.h"
#include "NamModels.h"
#include "NAM/get_dsp.h"
#include "NAM/wavenet/a2_fast.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

namespace NamLibrary {

namespace {

// Beyond this a capture is not read to be inspected (the loader's own cap on
// local files is 50 MB).
constexpr juce::int64 kMaxInspectBytes = 64 * 1024 * 1024;

// Containers inside containers are legal; this only bounds a hostile file.
constexpr int kMaxNesting = 8;

// What one model (a whole file, or one model inside a container) turned out
// to be. `problem` completes "This <label> capture is damaged: ..." or, with
// `whole` set, is the whole sentence.
struct Verdict {
  bool ok = false;
  juce::String label;
  bool a2 = false;
  juce::String problem;
  bool whole = false;
};

Verdict damaged(const juce::String& label, const juce::String& problem) {
  Verdict v;
  v.label = label;
  v.problem = problem;
  return v;
}

Verdict refused(const juce::String& label, const juce::String& sentence) {
  Verdict v;
  v.label = label;
  v.problem = sentence;
  v.whole = true;
  return v;
}

// A word out of someone else's file, shown to the player and handed to
// agents: only letters, digits and a few marks, and not a sentence.
juce::String cleanWord(const std::string& raw) {
  juce::String named;
  for (const auto c : juce::String(raw))
    if (named.length() < 32 && (juce::CharacterFunctions::isLetterOrDigit(c) || c == ' ' ||
                                c == '.' || c == '_' || c == '-'))
      named << juce::String::charToString(c);
  return named.trim();
}

// The settings a model's config must have for the core to build it, checked
// without the weights. Returns an empty string when they are complete, else
// what is missing.
juce::String missingSetting(const nlohmann::json& config,
                            std::initializer_list<const char*> wholeNumbers,
                            std::initializer_list<const char*> switches = {}) {
  for (const char* key : wholeNumbers) {
    const auto found = config.find(key);
    if (found == config.end() || !found->is_number_integer() || found->get<long long>() < 1)
      return juce::String("its settings have no usable ") + key;
  }
  for (const char* key : switches) {
    const auto found = config.find(key);
    if (found == config.end() || !found->is_boolean())
      return juce::String("its settings do not say ") + key;
  }
  return {};
}

Verdict inspectModel(const nlohmann::json& model, int depth) {
  if (!model.is_object() || !model.contains("architecture") || !model["architecture"].is_string())
    return refused("Unknown", "This file is not a NAM capture: it names no architecture.");
  const std::string architecture = model["architecture"].get<std::string>();
  const juce::String word = cleanWord(architecture);

  // The label first: it names the capture in every sentence below.
  juce::String label = word.isNotEmpty() ? word : juce::String("Unknown");
  bool a2 = false;
  const auto config = model.find("config");
  const bool hasConfig = config != model.end() && config->is_object();
  if (architecture == "WaveNet") {
    int channels = 0;
    a2 = hasConfig && nam::wavenet::a2_fast::is_a2_shape(*config, &channels);
    label = a2 ? "WaveNet A2" : "WaveNet A1";
  } else if (architecture == "SlimmableContainer") {
    label = "Slimmable";
  }

  const bool known = architecture == "WaveNet" || architecture == "LSTM" ||
                     architecture == "ConvNet" || architecture == "Linear" ||
                     architecture == "SlimmableContainer";
  if (!known)
    return refused(label, "This capture uses an architecture Pedaleira does not know (\"" +
                              label + "\"), so it cannot be loaded.");

  const auto version = model.find("version");
  if (version == model.end() || !version->is_string())
    return refused(label, "This " + label +
                              " capture does not say which NAM version saved it, so it cannot "
                              "be loaded.");
  if (nam::is_version_supported(version->get<std::string>()) == nam::Supported::NO)
    return refused(label, "This " + label + " capture was saved by NAM file version " +
                              cleanWord(version->get<std::string>()) +
                              ", which Pedaleira cannot read.");

  if (const auto rate = model.find("sample_rate"); rate != model.end()) {
    if (!rate->is_number())
      return damaged(label, "its sample rate is not a number");
    const double hz = rate->get<double>();
    if (std::isfinite(hz) && hz > 0.0 &&
        (hz < NamModels::kLowestSampleRate || hz > NamModels::kHighestSampleRate))
      return damaged(label, "it says it was trained at " + juce::String(hz, 0) +
                                " Hz, outside the rates audio runs at");
  }

  if (!hasConfig)
    return damaged(label, "it has no settings");

  juce::String missing;
  if (architecture == "WaveNet") {
    const auto layers = config->find("layers");
    if (layers == config->end() || !layers->is_array() || layers->empty())
      missing = "its settings have no layers";
  } else if (architecture == "LSTM") {
    missing = missingSetting(*config, {"num_layers", "input_size", "hidden_size"});
  } else if (architecture == "ConvNet") {
    missing = missingSetting(*config, {"channels"}, {"batchnorm"});
    const auto dilations = config->find("dilations");
    if (missing.isEmpty() &&
        (dilations == config->end() || !dilations->is_array() || dilations->empty()))
      missing = "its settings have no dilations";
    if (missing.isEmpty() && !config->contains("activation"))
      missing = "its settings name no activation";
  } else if (architecture == "Linear") {
    missing = missingSetting(*config, {"receptive_field"}, {"bias"});
  } else {  // SlimmableContainer
    const auto submodels = config->find("submodels");
    if (submodels == config->end() || !submodels->is_array() || submodels->empty())
      return damaged(label, "it holds no models");
    if (depth >= kMaxNesting)
      return damaged(label, "its models are nested too deep");
    bool allA2 = true;
    for (const auto& entry : *submodels) {
      if (!entry.is_object() || !entry.contains("max_value") || !entry["max_value"].is_number() ||
          !entry.contains("model"))
        return damaged(label, "one of its models is incomplete");
      const Verdict inner = inspectModel(entry["model"], depth + 1);
      if (!inner.ok) {
        // The container is refused for what is wrong inside it.
        Verdict v = inner;
        v.label = label;
        return v;
      }
      allA2 = allA2 && inner.a2;
    }
    a2 = allA2;
    if (a2)
      label = "Slimmable A2";
  }
  if (missing.isNotEmpty())
    return damaged(label, missing);

  Verdict v;
  v.ok = true;
  v.label = label;
  v.a2 = a2;
  return v;
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
  info.architecture = cleanWord(model["architecture"].get<std::string>());
  if (model.contains("metadata") && model["metadata"].is_object()) {
    const auto& metadata = model["metadata"];
    if (metadata.contains("name") && metadata["name"].is_string())
      info.name = juce::String(metadata["name"].get<std::string>()).trim();
  }
  if (const auto rate = model.find("sample_rate"); rate != model.end() && rate->is_number()) {
    const double hz = rate->get<double>();
    info.sampleRate = std::isfinite(hz) && hz > 0.0 ? hz : 0.0;
  }

  const Verdict verdict = inspectModel(model, 0);
  info.supported = verdict.ok;
  info.label = verdict.label;
  info.a2 = verdict.ok && verdict.a2;
  if (!verdict.ok)
    info.reason = verdict.whole
                      ? verdict.problem
                      : "This " + verdict.label + " capture is damaged: " + verdict.problem + ".";
  return info;
}

juce::String damagedReason(const Info& info, const juce::String& what) {
  const juce::String label = info.label.isNotEmpty() ? info.label : juce::String("NAM");
  // NamModels' own reasons read on from "cannot be loaded: " ("it holds 5
  // weights where its settings need 70"); the core's are quoted as they are.
  juce::String detail = what.trim().trimCharactersAtEnd(".!\n");
  if (detail.startsWith("it ") || detail.startsWith("its ") || detail.startsWith("one of its"))
    return "This " + label + " capture is damaged and cannot be loaded: " + detail + ".";
  return "This " + label + " capture is damaged and cannot be loaded (" + detail + ").";
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
      if (item["tone3000"].isObject())
        entry.tone3000 = item["tone3000"];
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
    if (entry.tone3000.isObject())
      item->setProperty("tone3000", entry.tone3000);
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
  // The same bytes are the same capture: where it came from on TONE3000 stays.
  juce::var tone3000;
  for (const auto& e : m.entries)
    if (e.stashFile == stashFile && e.tone3000.isObject())
      tone3000 = e.tone3000;
  m.entries.erase(std::remove_if(m.entries.begin(), m.entries.end(),
                                 [&](const Loaded& e) { return e.stashFile == stashFile; }),
                  m.entries.end());
  Loaded entry;
  entry.tone3000 = tone3000;
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

void noteTone3000(const juce::String& stashFile, const juce::var& attribution) {
  if (stashFile.isEmpty() || !attribution.isObject())
    return;
  Memory& m = memory();
  const std::lock_guard<std::mutex> lock(m.mutex);
  ensureRead(m);
  for (auto& entry : m.entries)
    if (entry.stashFile == stashFile) {
      entry.tone3000 = attribution;
      save(m);
      return;
    }
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
// CAPTURES DOWNLOADED FROM TONE3000
// ####################
namespace {

constexpr int kMaxBaseNameLength = 120;
constexpr const char* kIndexName = "downloads.json";

std::mutex& downloadsMutex() {
  static std::mutex m;
  return m;
}

bool isReservedDeviceName(const juce::String& name) {
  // Windows refuses these, with or without an extension.
  const juce::String stem = name.upToFirstOccurrenceOf(".", false, false).trim().toUpperCase();
  static const char* const fixed[] = {"CON", "PRN", "AUX", "NUL"};
  for (const char* reserved : fixed)
    if (stem == reserved)
      return true;
  return (stem.startsWith("COM") || stem.startsWith("LPT")) && stem.length() == 4 &&
         stem[3] >= '1' && stem[3] <= '9';
}

juce::File indexFile(const juce::File& root) { return downloadsDir(root).getChildFile(kIndexName); }

// The index's rows. Caller holds the mutex.
juce::Array<juce::var> readIndex(const juce::File& root) {
  juce::Array<juce::var> rows;
  const juce::File file = indexFile(root);
  if (!file.existsAsFile() || file.getSize() > 8 * 1024 * 1024)
    return rows;
  const juce::var parsed = juce::JSON::parse(file);
  if (const auto* list = parsed["downloads"].getArray())
    for (const auto& row : *list)
      if (row.isObject() && row["file"].toString().isNotEmpty())
        rows.add(row);
  return rows;
}

// Caller holds the mutex.
bool writeIndex(const juce::File& root, const juce::Array<juce::var>& rows) {
  juce::DynamicObject::Ptr object = new juce::DynamicObject();
  object->setProperty("v", 1);
  object->setProperty("downloads", rows);
  return indexFile(root).replaceWithText(juce::JSON::toString(juce::var(object.get()), false));
}

// A name in the downloads folder is a plain file name there: no folders.
bool isPlainName(const juce::String& name) {
  return name.isNotEmpty() && !name.containsAnyOf("/\\:") && !name.startsWithChar('.');
}

}  // namespace

juce::File downloadsDir(const juce::File& root) { return root.getChildFile(kDownloadsFolder); }

juce::String downloadBaseName(const juce::String& toneTitle, const juce::String& captureName) {
  const juce::String title = toneTitle.trim();
  const juce::String capture = captureName.trim();
  const juce::String name =
      title.isNotEmpty() && capture.isNotEmpty() ? title + " - " + capture : title + capture;
  // Control characters and the characters Windows refuses become a hyphen.
  juce::String safe;
  for (auto c = name.getCharPointer(); !c.isEmpty(); ++c) {
    const juce::juce_wchar ch = *c;
    if (ch < 32 || ch == 127 || juce::String("<>:\"/\\|?*").containsChar(ch))
      safe += "-";
    else
      safe += juce::String::charToString(ch);
  }
  // Runs of white space and of hyphens made by the replacement shrink to one.
  while (safe.contains("  "))
    safe = safe.replace("  ", " ");
  while (safe.contains("--"))
    safe = safe.replace("--", "-");
  safe = safe.trim();
  if (safe.length() > kMaxBaseNameLength)
    safe = safe.substring(0, kMaxBaseNameLength);
  // No trailing dot or space (Windows drops them), no leading dot (hidden).
  while (safe.endsWithChar('.') || safe.endsWithChar(' '))
    safe = safe.dropLastCharacters(1);
  while (safe.startsWithChar('.') || safe.startsWithChar(' '))
    safe = safe.substring(1);
  if (safe.isEmpty())
    return "TONE3000 capture";
  if (isReservedDeviceName(safe))
    return "TONE3000 " + safe;
  return safe;
}

juce::File findDownload(const juce::File& root, int toneId, int captureId) {
  if (toneId <= 0 || captureId <= 0)
    return {};
  const std::lock_guard<std::mutex> lock(downloadsMutex());
  for (const auto& row : readIndex(root)) {
    if (static_cast<int>(row["tone"]) != toneId || static_cast<int>(row["capture"]) != captureId)
      continue;
    const juce::String name = row["file"].toString();
    const juce::File file = downloadsDir(root).getChildFile(name);
    if (isPlainName(name) && file.existsAsFile() && file.getSize() > 0)
      return file;
  }
  return {};
}

juce::Result keepDownload(const juce::File& root, const juce::File& temp, int captureId,
                          const juce::var& tone3000, juce::File& kept) {
  if (!temp.existsAsFile() || temp.getSize() == 0)
    return juce::Result::fail("the downloaded file is not there or is empty");
  const juce::String extension = temp.hasFileExtension("wav") ? ".wav" : ".nam";
  const int toneId = tone3000.isObject() ? static_cast<int>(tone3000["id"]) : 0;
  const std::lock_guard<std::mutex> lock(downloadsMutex());
  const juce::File folder = downloadsDir(root);
  if (!LibraryPaths::createPrivateDirectory(folder))
    return juce::Result::fail("the TONE3000 downloads folder could not be made");
  const juce::String base =
      downloadBaseName(tone3000["title"].toString(), tone3000["capture"].toString());
  juce::File target = folder.getChildFile(base + extension);
  for (int n = 2; target.exists() && n < 10000; ++n)
    target = folder.getChildFile(base + " (" + juce::String(n) + ")" + extension);
  if (target.exists())
    return juce::Result::fail("no free name for the download");
  if (!temp.moveFileTo(target))
    return juce::Result::fail("the download could not be saved in " + folder.getFullPathName());

  // The same tone and capture downloaded before (its file was deleted): the
  // new file takes its row.
  juce::Array<juce::var> rows = readIndex(root);
  for (int i = rows.size(); --i >= 0;)
    if (toneId > 0 && captureId > 0 && static_cast<int>(rows[i]["tone"]) == toneId &&
        static_cast<int>(rows[i]["capture"]) == captureId)
      rows.remove(i);
  juce::DynamicObject::Ptr row = new juce::DynamicObject();
  row->setProperty("file", target.getFileName());
  row->setProperty("tone", toneId);
  row->setProperty("capture", captureId);
  row->setProperty("tone3000", tone3000);
  rows.add(juce::var(row.get()));
  // A file without its row is still kept; it only loses the credit on a listing.
  writeIndex(root, rows);
  kept = target;
  return juce::Result::ok();
}

juce::var downloadCredit(const juce::File& root, const juce::File& file) {
  if (file.getParentDirectory() != downloadsDir(root))
    return {};
  const std::lock_guard<std::mutex> lock(downloadsMutex());
  for (const auto& row : readIndex(root))
    if (row["file"].toString() == file.getFileName() && row["tone3000"].isObject())
      return row["tone3000"];
  return {};
}

}  // namespace NamLibrary

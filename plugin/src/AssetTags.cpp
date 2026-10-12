#include "AssetTags.h"

#include "PresetJson.h"

#include <juce_cryptography/juce_cryptography.h>


namespace {

constexpr const char* kFormat = "pedaleira.asset-tags";
constexpr int kVersion = 1;
// More files than any library holds; past it the path cache stops growing
// (tags themselves are never dropped).
constexpr size_t kMaxKnownPaths = 20000;
// The index is small text; a file far over this is not ours.
constexpr juce::int64 kMaxIndexBytes = 16 * 1024 * 1024;

bool isHash(const juce::String& text) {
  return text.length() == 64 && text.containsOnly("0123456789abcdef");
}

}  // namespace

juce::File AssetTags::indexFile(const juce::File& root) { return root.getChildFile("asset-tags.json"); }

juce::String AssetTags::normalise(const juce::String& tag) {
  juce::String out;
  bool space = false;
  for (auto c : tag.trim().toLowerCase()) {
    if (juce::CharacterFunctions::isWhitespace(c)) {
      space = true;
      continue;
    }
    // Printable only: a tag is shown in a list and typed in a search.
    if (c < 0x20 || c == 0x7f)
      continue;
    if (space && out.isNotEmpty())
      out << ' ';
    space = false;
    out << juce::String::charToString(c);
  }
  return out;
}

void AssetTags::loadIfNeeded(const juce::File& root) {
  const juce::File file = indexFile(root);
  if (loaded && file == loadedFrom)
    return;
  loaded = true;
  loadedFrom = file;
  entries.clear();
  known.clear();
  if (!file.existsAsFile() || file.getSize() > kMaxIndexBytes)
    return;

  juce::var index;
  if (t3k::presetjson::parseJsonGuarded(file.loadFileAsString(), index, 8).failed() ||
      index["format"].toString() != kFormat || static_cast<int>(index["version"]) != kVersion)
    return;

  if (auto* assets = index["assets"].getDynamicObject()) {
    for (const auto& property : assets->getProperties()) {
      const juce::String hash = property.name.toString();
      if (!isHash(hash))
        continue;
      Entry entry;
      if (const auto* tags = property.value["tags"].getArray())
        for (const auto& tag : *tags)
          if (const juce::String clean = normalise(tag.toString());
              clean.isNotEmpty() && clean.length() <= kMaxTagLength && !entry.tags.contains(clean) &&
              entry.tags.size() < kMaxTagsPerAsset)
            entry.tags.add(clean);
      if (!entry.tags.isEmpty())
        entries[hash] = std::move(entry);
    }
  }
  if (auto* paths = index["paths"].getDynamicObject()) {
    for (const auto& property : paths->getProperties()) {
      Known k;
      k.size = static_cast<juce::int64>(property.value["size"]);
      k.modified = static_cast<juce::int64>(property.value["modified"]);
      k.hash = property.value["hash"].toString();
      if (isHash(k.hash) && known.size() < kMaxKnownPaths)
        known[property.name.toString()] = std::move(k);
    }
  }
}

juce::Result AssetTags::save(const juce::File& libraryRoot) {
  juce::DynamicObject::Ptr assets = new juce::DynamicObject();
  for (const auto& [hash, entry] : entries) {
    juce::Array<juce::var> tags;
    for (const auto& tag : entry.tags)
      tags.add(tag);
    juce::DynamicObject::Ptr item = new juce::DynamicObject();
    item->setProperty("tags", tags);
    assets->setProperty(juce::Identifier(hash), juce::var(item.get()));
  }
  // Only the paths of tagged files are worth keeping across a restart: the
  // rest are hashed again the first time they are asked about.
  juce::DynamicObject::Ptr paths = new juce::DynamicObject();
  for (const auto& [path, k] : known) {
    if (entries.count(k.hash) == 0)
      continue;
    juce::DynamicObject::Ptr item = new juce::DynamicObject();
    item->setProperty("size", k.size);
    item->setProperty("modified", k.modified);
    item->setProperty("hash", k.hash);
    paths->setProperty(juce::Identifier(path), juce::var(item.get()));
  }
  juce::DynamicObject::Ptr index = new juce::DynamicObject();
  index->setProperty("format", kFormat);
  index->setProperty("version", kVersion);
  index->setProperty("assets", juce::var(assets.get()));
  index->setProperty("paths", juce::var(paths.get()));

  const juce::File file = indexFile(libraryRoot);
  if (!file.getParentDirectory().createDirectory())
    return juce::Result::fail("cannot make the folder for " + file.getFullPathName());
  return t3k::presetjson::writeFileAtomically(file, juce::JSON::toString(juce::var(index.get())) + "\n");
}

juce::String AssetTags::hashOf(const juce::File& root, const juce::File& file) {
  if (!file.existsAsFile())
    return {};
  const juce::String path = file.getFullPathName();
  const juce::int64 size = file.getSize();
  const juce::int64 modified = file.getLastModificationTime().toMilliseconds();
  if (const auto found = known.find(path);
      found != known.end() && found->second.size == size && found->second.modified == modified)
    return found->second.hash;

  // A file in the asset store is named after its hash already.
  if (isHash(file.getFileNameWithoutExtension()) &&
      file.getParentDirectory() == root.getChildFile("Assets"))
    return file.getFileNameWithoutExtension();

  juce::FileInputStream in(file);
  if (!in.openedOk())
    return {};
  const juce::String hash =
      juce::SHA256(in).toHexString().removeCharacters(" ").toLowerCase();
  if (known.size() < kMaxKnownPaths || known.count(path) != 0)
    known[path] = Known{size, modified, hash};
  return hash;
}

juce::StringArray AssetTags::tagsFor(const juce::File& root, const juce::File& file) {
  const std::lock_guard<std::mutex> lock(mutex);
  loadIfNeeded(root);
  if (entries.empty())
    return {};
  const juce::String hash = hashOf(root, file);
  const auto found = entries.find(hash);
  return found == entries.end() ? juce::StringArray() : found->second.tags;
}

juce::Result AssetTags::update(const juce::File& root, const juce::File& file, const juce::StringArray& add,
                               const juce::StringArray& remove, juce::StringArray& result) {
  juce::StringArray adding, removing;
  for (const auto& tag : add) {
    const juce::String clean = normalise(tag);
    if (clean.isEmpty())
      return juce::Result::fail("a tag is empty");
    if (clean.length() > kMaxTagLength)
      return juce::Result::fail("tag '" + clean + "' is longer than " +
                                juce::String(kMaxTagLength) + " characters");
    adding.addIfNotAlreadyThere(clean);
  }
  for (const auto& tag : remove)
    removing.addIfNotAlreadyThere(normalise(tag));

  const std::lock_guard<std::mutex> lock(mutex);
  loadIfNeeded(root);
  const juce::String hash = hashOf(root, file);
  if (hash.isEmpty())
    return juce::Result::fail("cannot read " + file.getFullPathName());

  juce::StringArray tags = entries.count(hash) != 0 ? entries[hash].tags : juce::StringArray();
  for (const auto& tag : adding)
    if (!removing.contains(tag))
      tags.addIfNotAlreadyThere(tag);
  for (const auto& tag : removing)
    tags.removeString(tag);
  if (tags.size() > kMaxTagsPerAsset)
    return juce::Result::fail("a file can carry at most " + juce::String(kMaxTagsPerAsset) +
                              " tags");

  const auto before = entries;
  if (tags.isEmpty())
    entries.erase(hash);
  else
    entries[hash].tags = tags;
  if (const juce::Result saved = save(root); saved.failed()) {
    entries = before;
    return saved;
  }
  result = tags;
  return juce::Result::ok();
}

std::map<juce::String, int> AssetTags::tagCounts(const juce::File& root) {
  const std::lock_guard<std::mutex> lock(mutex);
  loadIfNeeded(root);
  std::map<juce::String, int> counts;
  for (const auto& [hash, entry] : entries)
    for (const auto& tag : entry.tags)
      ++counts[tag];
  return counts;
}

#pragma once

#include <juce_core/juce_core.h>

#include <map>
#include <mutex>

// The user's own tags on amp captures and impulse responses, the index the
// "Tone tagging" settings and an agent choosing a tone both read
// (docs/agents.md, "Captures and IR index").
//
// Kept in <library root>/asset-tags.json and keyed by the file's SHA-256, so
// a tag follows the file through a move, a rename or a copy into the asset
// store (whose names are that hash already). Hashing a file means reading
// it, so the hash of each path is remembered beside its size and
// modification time and only computed again when either changes.
//
// Tags are short lowercase words or phrases ("edge of breakup", "v30"):
// trimmed, spaces collapsed, at most kMaxTagLength characters and
// kMaxTagsPerAsset per file. Any thread; one mutex guards the index.
class AssetTags {
public:
  static constexpr int kMaxTagLength = 32;
  static constexpr int kMaxTagsPerAsset = 32;

  // Every call names the library root (read on the message thread, where
  // the library lives), so a library moved to another folder brings its own
  // index with it.
  AssetTags() = default;

  // The tags the user gave this file; empty for an untagged or unreadable
  // file.
  juce::StringArray tagsFor(const juce::File& root, const juce::File& file);

  // Adds and removes tags (normalised first; `remove` wins when a tag is in
  // both) and saves the index. Returns the file's tags after the change, or
  // an error when the file cannot be read, a tag is empty or too long once
  // normalised, the file would have too many, or the index cannot be saved.
  juce::Result update(const juce::File& root, const juce::File& file, const juce::StringArray& add,
                      const juce::StringArray& remove, juce::StringArray& result);

  // Every tag in use with the number of files that carry it.
  std::map<juce::String, int> tagCounts(const juce::File& root);

  // The form a tag is stored in, or empty when nothing is left of it.
  static juce::String normalise(const juce::String& tag);

  // Where the index lives for the current library.
  static juce::File indexFile(const juce::File& root);

private:
  struct Entry {
    juce::StringArray tags;
  };
  struct Known {
    juce::int64 size{0};
    juce::int64 modified{0};
    juce::String hash;
  };

  // Loads the index when the library changed or it was never read. Holds
  // the lock.
  void loadIfNeeded(const juce::File& root);
  juce::Result save(const juce::File& root);
  // The file's hash from the cache, or computed (and cached). Empty when
  // the file cannot be read. Holds the lock.
  juce::String hashOf(const juce::File& root, const juce::File& file);

  std::mutex mutex;
  juce::File loadedFrom;
  bool loaded{false};
  std::map<juce::String, Entry> entries;
  std::map<juce::String, Known> known;

  JUCE_DECLARE_NON_COPYABLE(AssetTags)
};

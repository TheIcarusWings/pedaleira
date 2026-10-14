#pragma once
// The speaker descriptions that ship inside the binary (resources/cabs/*.json,
// linked in by cmake/AmpFramework.cmake as PedaleiraCabData) and a cache of
// their parsed form. docs/cab-framework.md describes the format.
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "PedaleiraCabData.h"
#include "cab/SpeakerDescription.h"

namespace cabfx {

/** The speakers of the cab.model block's `speaker` choice, in the order the
    choice saves them. Append only: a preset stores the index. */
inline constexpr const char* kSpeakerFiles[] = {"granite.json", "ember.json", "velvet.json",
                                                "brass.json",   "oak.json",   "flint.json"};
inline constexpr int kBuiltinSpeakers = static_cast<int>(sizeof(kSpeakerFiles) / sizeof(kSpeakerFiles[0]));

/** The text of a shipped speaker description, by file name ("granite.json");
    empty when there is none of that name. */
inline std::string builtinSpeakerText(const char* filename) {
  for (int i = 0; i < PedaleiraCabData::namedResourceListSize; ++i) {
    if (std::string(PedaleiraCabData::originalFilenames[i]) == filename) {
      int size = 0;
      const char* data = PedaleiraCabData::getNamedResource(PedaleiraCabData::namedResourceList[i], size);
      if (data != nullptr)
        return std::string(data, static_cast<size_t>(size));
    }
  }
  return {};
}

/** A shipped speaker, parsed once. Null if the file is missing or does not
    parse (a build error: the tests load every shipped file). */
inline std::shared_ptr<const cab::SpeakerDescription> builtinSpeaker(const char* filename) {
  static std::mutex mutex;
  static std::map<std::string, std::shared_ptr<const cab::SpeakerDescription>> cache;
  const std::lock_guard<std::mutex> guard(mutex);
  const auto found = cache.find(filename);
  if (found != cache.end())
    return found->second;
  auto parsed = std::make_shared<cab::SpeakerDescription>();
  std::string error;
  std::shared_ptr<const cab::SpeakerDescription> result;
  if (cab::parseSpeakerDescription(builtinSpeakerText(filename), *parsed, error))
    result = parsed;
  cache[filename] = result;
  return result;
}

/** The shipped speaker at choice index `index` (clamped). */
inline std::shared_ptr<const cab::SpeakerDescription> builtinSpeakerAt(int index) {
  if (index < 0)
    index = 0;
  if (index >= kBuiltinSpeakers)
    index = kBuiltinSpeakers - 1;
  return builtinSpeaker(kSpeakerFiles[index]);
}

}  // namespace cabfx

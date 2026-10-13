#pragma once
// The amp descriptions that ship inside the binary (resources/amps/*.json,
// linked in by cmake/AmpData.cmake) and a cache of their parsed form.
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "PedaleiraAmpData.h"
#include "amp/AmpDescription.h"

namespace ampfx {

/** The text of a shipped description, by file name ("high-gain.json"); empty
    when there is none of that name. */
inline std::string builtinAmpText(const char* filename) {
  for (int i = 0; i < PedaleiraAmpData::namedResourceListSize; ++i) {
    if (std::string(PedaleiraAmpData::originalFilenames[i]) == filename) {
      int size = 0;
      const char* data = PedaleiraAmpData::getNamedResource(PedaleiraAmpData::namedResourceList[i], size);
      if (data != nullptr)
        return std::string(data, static_cast<size_t>(size));
    }
  }
  return {};
}

/** A shipped description, parsed once. Null if the file is missing or does not
    parse (a build error: the tests load every shipped file). */
inline std::shared_ptr<const amp::AmpDescription> builtinAmp(const char* filename) {
  static std::mutex mutex;
  static std::map<std::string, std::shared_ptr<const amp::AmpDescription>> cache;
  const std::lock_guard<std::mutex> guard(mutex);
  const auto found = cache.find(filename);
  if (found != cache.end())
    return found->second;
  auto parsed = std::make_shared<amp::AmpDescription>();
  std::string error;
  std::shared_ptr<const amp::AmpDescription> result;
  if (amp::parseAmpDescription(builtinAmpText(filename), *parsed, error))
    result = parsed;
  cache[filename] = result;
  return result;
}

}  // namespace ampfx

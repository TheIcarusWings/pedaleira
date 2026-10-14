#pragma once
// The amp descriptions that ship inside the binary (resources/amps/*.json,
// linked in by cmake/AmpData.cmake) and a cache of their parsed form.
#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "PedaleiraAmpData.h"
#include "amp/AmpDescription.h"

namespace ampfx {

/** The file names of every shipped description ("high-gain.json", "lead-100.json",
    ...), sorted. Whatever is in resources/amps when CMake runs: adding an amp
    is adding a file there, and it becomes a block kind of its own
    (BuiltinAmpKinds.cpp) without another edit. */
inline std::vector<std::string> builtinAmpFiles() {
  std::vector<std::string> files;
  for (int i = 0; i < PedaleiraAmpData::namedResourceListSize; ++i) {
    const std::string name = PedaleiraAmpData::originalFilenames[i];
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0)
      files.push_back(name);
  }
  std::sort(files.begin(), files.end());
  return files;
}

/** The block kind id of a shipped description: "amp." and the file name
    without ".json", lower case, letters and digits only ("lead-100.json" is
    "amp.lead100"; "high-gain.json" is "amp.highgain", the id the first amp
    always had, so a saved preset that names it still loads). */
inline std::string builtinAmpKindId(const std::string& filename) {
  std::string id = "amp.";
  for (const char c : filename.substr(0, filename.rfind('.')))
    if (std::isalnum(static_cast<unsigned char>(c)))
      id += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return id;
}

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

// The agent layer's commands over the NAM captures the player has loaded and
// the folders captures are found in: nam.inspect (can Pedaleira load this
// file), nam.recent and nam.clearRecent (the loaded captures, newest first),
// nam.tone3000Folders (where the TONE3000 app's downloads are looked for).
// Searching the captures is assets.list (GuitarCommands.cpp); the folders the
// player adds are roots.add and roots.remove (AgentHub.cpp).
#include "AgentHub.h"

// Processor.h first: it pulls in the JUCE module headers, which must come
// before anything that drags in <windows.h>.
#include "Processor.h"

#include "NamLibrary.h"

#include <utility>

namespace {

using P = CommandParam;
using T = CommandParamType;
using Properties = std::initializer_list<std::pair<const char*, juce::var>>;

juce::var object(Properties properties) {
  juce::DynamicObject::Ptr obj = new juce::DynamicObject();
  for (const auto& [name, value] : properties)
    obj->setProperty(name, value);
  return juce::var(obj.get());
}

// The architecture fields every capture in a reply carries.
void addArchitecture(juce::var& target, const NamLibrary::Info& info) {
  auto* obj = target.getDynamicObject();
  obj->setProperty("architecture", info.architecture);
  obj->setProperty("architectureLabel", info.label);
  obj->setProperty("supported", info.supported);
  if (!info.supported)
    obj->setProperty("unsupportedReason", info.reason);
}

}  // namespace

void AgentHub::registerNamCommands() {
  liveCommands.add({
      "nam.inspect",
      "Tells whether Pedaleira can load a NAM capture, without loading it: its architecture "
      "(architecture as the file says it, architectureLabel as a player knows it, such as "
      "WaveNet A2 or LSTM), supported (only A2 captures are loaded) and, when it is not "
      "supported or not a capture at all, unsupportedReason in a sentence that can be shown "
      "as it is. modelName is the name in the file's metadata, empty when it has none.",
      {P::required("path", T::string, "A .nam file, as an absolute path.").path()},
      false,
      CommandPermission::read,
      [](const juce::var& args) {
        const juce::File file(args["path"].toString());
        if (!file.existsAsFile())
          return CommandResult::failure(CommandError::notFound,
                                        "file not found: " + file.getFullPathName());
        if (!file.hasFileExtension("nam"))
          return CommandResult::failure(CommandError::invalidArgs,
                                        "nam.inspect: argument 'path' must be a .nam file");
        const NamLibrary::Info info = NamLibrary::inspectFile(file);
        juce::var result = object({
            {"path", file.getFullPathName()},
            {"name", file.getFileNameWithoutExtension()},
            {"modelName", info.name},
            {"readable", info.readable},
            {"sizeBytes", file.getSize()},
        });
        if (info.readable)
          addArchitecture(result, info);
        else {
          auto* obj = result.getDynamicObject();
          obj->setProperty("supported", false);
          obj->setProperty("unsupportedReason", info.reason);
        }
        return CommandResult::success(result);
      },
  });

  liveCommands.add({
      "nam.recent",
      "Lists the captures (or IRs) the player loaded from a file, newest first: name (the file "
      "name without its extension), path (the file to load again: the original when it is "
      "still there, else the copy the model stash kept), fromStash (true for that copy), "
      "loadedAt (milliseconds since 1970), for a capture what nam.inspect says, and tone3000 "
      "(the tone, creator and licence on TONE3000) for a file that came from there. A file "
      "that is gone from both places is left out. The stash drops copies that were not used "
      "for a week, so an old entry whose original moved away can disappear.",
      {P::optional("kind", T::string, "Captures, IRs or both.")
           .oneOf({"all", "capture", "ir"})
           .withDefault("capture"),
       P::optional("limit", T::integer, "The most entries to return.").range(1, 200).withDefault(30)},
      false,
      CommandPermission::read,
      [](const juce::var& args) {
        const juce::String kind = args["kind"].toString();
        const int limit = args["limit"];
        const juce::File stash = TONE3000Processor::getLocalModelsDir();
        juce::Array<juce::var> entries;
        int hidden = 0;
        for (const auto& record : NamLibrary::loaded(true)) {
          if (kind != "all" && record.kind != kind)
            continue;
          const juce::File original(record.sourcePath);
          const juce::File copy = stash.getChildFile(record.stashFile);
          const bool useOriginal = record.sourcePath.isNotEmpty() && original.existsAsFile();
          if (!useOriginal && !copy.existsAsFile()) {
            ++hidden;
            continue;
          }
          if (entries.size() >= limit)
            continue;
          const juce::File file = useOriginal ? original : copy;
          juce::var entry = object({
              {"name", record.name},
              {"kind", record.kind},
              {"path", file.getFullPathName()},
              {"fromStash", !useOriginal},
              {"loadedAt", record.loadedAt},
          });
          if (record.kind == "capture")
            addArchitecture(entry, NamLibrary::inspectFile(file));
          if (record.tone3000.isObject())
            entry.getDynamicObject()->setProperty("tone3000", record.tone3000);
          entries.add(entry);
        }
        return CommandResult::success(object({{"recent", entries}, {"gone", hidden}}));
      },
  });

  liveCommands.add({
      "nam.clearRecent",
      "Empties the list nam.recent returns. The files stay findable by name with assets.list.",
      {},
      true,
      CommandPermission::local,
      [](const juce::var&) {
        NamLibrary::clearRecent();
        return CommandResult::success(object({{"recent", juce::Array<juce::var>()}}));
      },
  });

  liveCommands.add({
      "nam.tone3000Folders",
      "Lists the folders where the TONE3000 app's downloaded models are looked for on this "
      "computer (its data folder, and its folders in Documents and Downloads) with whether "
      "each exists. The ones that exist are scanned by assets.list (source tone3000). A "
      "folder anywhere else is added with roots.add.",
      {},
      false,
      CommandPermission::read,
      [](const juce::var&) {
        juce::Array<juce::var> folders;
        for (const auto& folder : NamLibrary::tone3000Searched())
          folders.add(object({{"path", folder.getFullPathName()}, {"exists", folder.isDirectory()}}));
        return CommandResult::success(object({{"folders", folders}}));
      },
  });
}

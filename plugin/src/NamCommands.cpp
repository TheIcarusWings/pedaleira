// The agent layer's commands over the NAM captures the player has loaded and
// the folders captures are found in: nam.inspect (can Pedaleira load this
// file), nam.recent and nam.clearRecent (the loaded captures, newest first),
// nam.downloaded and nam.keepDownload (the captures downloaded from TONE3000,
// kept in the library's TONE3000 folder).
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
  obj->setProperty("a2", info.a2);
  if (info.sampleRate > 0.0)
    obj->setProperty("sampleRate", info.sampleRate);
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
      "WaveNet A2, WaveNet A1, LSTM, ConvNet, Linear or Slimmable A2), a2 (true for NAM A2), "
      "sampleRate (the rate it was trained at, when the file says; it always runs at that "
      "rate, resampled when the session runs at another, and at 48 kHz when the file does "
      "not say), supported (every architecture NAM builds loads; false for a file that is "
      "not a capture, a NAM version or architecture Pedaleira cannot read, or damaged "
      "settings) and, when it is not supported, unsupportedReason in a sentence that can be "
      "shown as it is. Weights that do not fit the settings are only found when the file is "
      "loaded. modelName is the name in the file's metadata, empty when it has none.",
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
      "nam.downloaded",
      "Finds the file a capture of a TONE3000 tone was downloaded to before, in the library's "
      "TONE3000 folder: {path} when it is there, {path: \"\"} when it is not (never "
      "downloaded, or the file was deleted). The TONE3000 page asks before it downloads, so a "
      "capture is fetched once.",
      {P::required("tone", T::integer, "The TONE3000 tone id.").range(1, 2147483647),
       P::required("capture", T::integer, "The TONE3000 capture (model) id.").range(1, 2147483647)},
      false,
      CommandPermission::read,
      [this](const juce::var& args) {
        const juce::File file =
            NamLibrary::findDownload(live.getLibraryRoot(), args["tone"], args["capture"]);
        return CommandResult::success(object({{"path", file.getFullPathName()}}));
      },
  });

  liveCommands.add({
      "nam.keepDownload",
      "Keeps a capture or IR just downloaded from TONE3000 in the library: moves `file` into "
      "<library>/TONE3000 under a name made from the tone's title and the capture's name "
      "(\"Title - Capture.nam\", unsafe characters replaced, a number added when the name is "
      "taken) and remembers which tone and capture it is and who made it, so it persists across "
      "restarts, is found by assets.list (origin tone3000, with its tone3000 credit) and is "
      "reused by nam.downloaded. Returns {path}, the kept file. The file is then loaded with "
      "chain.addToneFromFiles.",
      {P::required("file", T::string, "The downloaded .nam or .wav, an absolute path. It is moved.")
           .path(),
       P::required("capture", T::integer, "The TONE3000 capture (model) id.").range(1, 2147483647),
       P::required("tone3000", T::object,
                   "Where it came from: {id (the tone), title, url, gear, format, license, image, "
                   "capture (the capture's name), user: {username, display_name, avatar_url, "
                   "is_verified}}, as chain.addToneFromFiles takes it.")},
      true,
      CommandPermission::local,
      [this](const juce::var& args) {
        const juce::File file(args["file"].toString());
        if (!file.existsAsFile())
          return CommandResult::failure(CommandError::notFound,
                                        "file not found: " + file.getFullPathName());
        if (!file.hasFileExtension("nam;wav"))
          return CommandResult::failure(CommandError::invalidArgs,
                                        "nam.keepDownload: argument 'file' must be a .nam or .wav");
        const juce::var attribution = TONE3000Processor::tone3000Attribution(args["tone3000"]);
        if (!attribution.isObject() || static_cast<int>(attribution["id"]) <= 0)
          return CommandResult::failure(
              CommandError::invalidArgs,
              "nam.keepDownload: argument 'tone3000' needs the tone's id and a title or a user");
        // Not a capture at all (an error page saved as .nam) is not kept.
        if (file.hasFileExtension("nam") && !NamLibrary::inspectFile(file).readable)
          return CommandResult::failure(CommandError::invalidArgs,
                                        "nam.keepDownload: the file is not a NAM capture");
        juce::File kept;
        const juce::Result result = NamLibrary::keepDownload(live.getLibraryRoot(), file,
                                                             args["capture"], attribution, kept);
        if (result.failed())
          return CommandResult::failure(CommandError::failed, result.getErrorMessage());
        return CommandResult::success(object({{"path", kept.getFullPathName()}}));
      },
  });
}

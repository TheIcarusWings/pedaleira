// The agent layer's commands over what lies in the library beside the
// setlists: guitar.* (profiles and their clips, <library>/Guitars) and
// assets.list (the captures and IRs an agent can load). The setlist
// library's own commands are in LibraryCommands.cpp.
#include "AgentHub.h"

// Processor.h first: it pulls in the JUCE module headers, which must come
// before anything that drags in <windows.h>.
#include "Processor.h"

#include "AssetTags.h"
#include "GuitarProfiles.h"
#include "LibraryPaths.h"
#include "NamLibrary.h"
#include "PresetJson.h"
#include "SandboxRig.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

namespace {

using P = CommandParam;
using T = CommandParamType;
using Properties = std::initializer_list<std::pair<const char*, juce::var>>;

// A .nam file's metadata sits in a small object next to megabytes of
// weights; files beyond this are listed without it.
constexpr juce::int64 kMaxNamBytesToRead = 64 * 1024 * 1024;
constexpr int kMaxFilesScanned = 5000;

juce::var object(Properties properties) {
  juce::DynamicObject::Ptr obj = new juce::DynamicObject();
  for (const auto& [name, value] : properties)
    obj->setProperty(name, value);
  return juce::var(obj.get());
}

CommandResult fail(const char* code, const juce::String& message) {
  return CommandResult::failure(code, message);
}

// The "metadata" object of a .nam file, without parsing the weights: found
// by name and cut out by matching braces.
juce::var namMetadata(const juce::File& file) {
  if (file.getSize() > kMaxNamBytesToRead)
    return {};
  const juce::String text = file.loadFileAsString();
  const int key = text.indexOf("\"metadata\"");
  if (key < 0)
    return {};
  const int open = text.indexOfChar(key, '{');
  if (open < 0)
    return {};
  // Walked with a character pointer: indexing a String by position is a scan
  // from its start each time, and the object may sit megabytes in.
  auto begin = text.getCharPointer();
  begin += open;
  int depth = 0;
  bool inString = false;
  auto at = begin;
  for (int count = 0; !at.isEmpty() && count < 65536; ++count) {
    const juce::juce_wchar c = at.getAndAdvance();
    if (inString) {
      if (c == '\\' && !at.isEmpty())
        at.getAndAdvance();
      else if (c == '"')
        inString = false;
    } else if (c == '"') {
      inString = true;
    } else if (c == '{') {
      ++depth;
    } else if (c == '}' && --depth == 0) {
      juce::var metadata;
      // Behind the depth check: the object is the file's own, and this runs
      // on a connection's thread, whose stack is small.
      if (t3k::presetjson::parseJsonGuarded(juce::String(begin, at), metadata, 16).wasOk())
        return metadata;
      return {};
    }
  }
  return {};
}

// One capture or IR file as assets.list reports it.
juce::var describeAsset(const juce::File& file, const juce::File& root, const char* source) {
  const bool capture = file.hasFileExtension("nam");
  juce::Array<juce::var> tags;
  // The folders between the root and the file are the only tags a plain
  // folder of files has ("Amps/Marshall/JCM800 crunch.nam").
  const juce::String relative = file.getParentDirectory().getRelativePathFrom(root);
  if (relative != "." && !relative.startsWith(".."))
    for (const auto& part : juce::StringArray::fromTokens(relative, "/\\", ""))
      if (part.isNotEmpty())
        tags.add(part);

  juce::var asset = object({
      {"kind", capture ? "capture" : "ir"},
      {"name", file.getFileNameWithoutExtension()},
      {"path", file.getFullPathName()},
      {"source", source},
      {"tags", tags},
      {"sizeBytes", file.getSize()},
  });
  auto* obj = asset.getDynamicObject();

  if (capture) {
    const juce::var metadata = namMetadata(file);
    // Only the fields that describe the gear and the tone; the training
    // record is of no use for choosing.
    for (const char* field : {"name", "modeled_by", "gear_type", "gear_make", "gear_model",
                              "tone_type", "gain", "loudness", "input_level_dbu",
                              "output_level_dbu"})
      if (const juce::var value = metadata[juce::Identifier(field)];
          !value.isVoid() && !value.isUndefined() && !(value.isString() && value.toString().isEmpty()))
        obj->setProperty(juce::Identifier(juce::String("nam_") + field), value);
    // Whether Pedaleira can load it, before anyone tries.
    const NamLibrary::Info info = NamLibrary::inspectFile(file);
    obj->setProperty("architecture", info.architecture);
    obj->setProperty("architectureLabel", info.label);
    obj->setProperty("supported", info.supported);
    if (!info.supported)
      obj->setProperty("unsupportedReason", info.reason);
  } else {
    juce::WavAudioFormat wav;
    if (auto stream = file.createInputStream()) {
      std::unique_ptr<juce::AudioFormatReader> reader(wav.createReaderFor(stream.release(), true));
      if (reader != nullptr && reader->sampleRate > 0.0) {
        obj->setProperty("sampleRate", reader->sampleRate);
        obj->setProperty("channels", static_cast<int>(reader->numChannels));
        obj->setProperty("seconds", std::round(static_cast<double>(reader->lengthInSamples) /
                                                reader->sampleRate * 1000.0) /
                                        1000.0);
      }
    }
  }
  return asset;
}

// Words that keep tags consistent across a library: what assets.tagSummary
// suggests. A suggestion, not a rule; any tag is accepted.
const std::vector<std::pair<const char*, std::vector<const char*>>>& suggestedTagVocabulary() {
  static const std::vector<std::pair<const char*, std::vector<const char*>>> vocabulary = {
      {"gain", {"clean", "edge of breakup", "crunch", "high gain", "fuzz"}},
      {"character", {"bright", "dark", "warm", "tight", "loose", "scooped", "mid forward",
                     "compressed", "open"}},
      {"gear", {"amp", "amp and cab", "pedal", "preamp", "full rig", "di"}},
      {"speaker", {"1x12", "2x12", "4x12", "v30", "greenback", "alnico", "jensen"}},
      {"use", {"rhythm", "lead", "ambient", "blues", "rock", "metal", "jazz", "country"}},
  };
  return vocabulary;
}

// describeAsset plus the user's own tags: `tags` becomes the folder tags and
// the user's together, `userTags` the user's alone, `tagged` whether there
// are any.
juce::var describeTaggedAsset(const juce::File& file, const juce::File& root, const char* source,
                              const juce::StringArray& userTags) {
  juce::var asset = describeAsset(file, root, source);
  auto* obj = asset.getDynamicObject();
  juce::Array<juce::var> merged;
  if (const auto* folderTags = asset["tags"].getArray())
    merged = *folderTags;
  juce::Array<juce::var> own;
  for (const auto& tag : userTags) {
    own.add(tag);
    if (!merged.contains(tag))
      merged.add(tag);
  }
  obj->setProperty("tags", merged);
  obj->setProperty("userTags", own);
  obj->setProperty("tagged", !userTags.isEmpty());
  return asset;
}

// describeTaggedAsset for assets.list: adds where the file came from
// (origin: library, folder, tone3000 or loaded) and, for the copies the model
// stash and the asset store keep under content-hash names, the name the
// player knows the file by (the file name it was loaded from, else the name
// in its metadata), so a search can find it.
juce::var describeListed(const juce::File& file, const juce::File& tagRoot, const char* source,
                         const juce::StringArray& userTags, const char* origin) {
  juce::var asset = describeTaggedAsset(file, tagRoot, source, userTags);
  auto* obj = asset.getDynamicObject();
  obj->setProperty("origin", origin);
  if (juce::String(origin) != "loaded")
    return asset;
  const NamLibrary::Loaded record = NamLibrary::loadedFor(file.getFileName());
  if (record.name.isNotEmpty()) {
    obj->setProperty("name", record.name);
    if (record.sourcePath.isNotEmpty())
      obj->setProperty("loadedFrom", record.sourcePath);
    obj->setProperty("loadedAt", record.loadedAt);
    if (record.tone3000.isObject())
      obj->setProperty("tone3000", record.tone3000);
  } else if (const juce::String named = asset["nam_name"].toString(); named.isNotEmpty()) {
    obj->setProperty("name", named);
  }
  return asset;
}

// Every wanted tag (normalised) is among the asset's tags, folder tags
// compared in their normalised form.
bool hasAllTags(const juce::var& asset, const juce::StringArray& wanted) {
  juce::StringArray present;
  if (const auto* tags = asset["tags"].getArray())
    for (const auto& tag : *tags)
      present.add(AssetTags::normalise(tag.toString()));
  for (const auto& tag : wanted)
    if (!present.contains(tag))
      return false;
  return true;
}

// The tones already loaded in a rig, from its chain state.
juce::var rigTones(TONE3000Processor& rig) {
  juce::Array<juce::var> tones;
  const juce::var chain = rig.getChainState(-1);
  std::vector<juce::var> lists{chain["chain"], chain["chainRight"]};
  if (const auto* rows = chain["grid"]["rows"].getArray()) {
    lists.clear();
    for (const auto& row : *rows)
      lists.push_back(row["blocks"]);
  }
  for (const auto& list : lists)
    if (const auto* blocks = list.getArray())
      for (const auto& block : *blocks) {
        const juce::var tone = block["tone"];
        if (block["kind"].toString() != "tone" || tone.getDynamicObject() == nullptr)
          continue;
        const juce::String format = tone["format"].toString();
        if (format != "nam" && format != "ir")
          continue;
        tones.add(object({
            {"blockId", block["blockId"]},
            {"kind", format == "nam" ? "capture" : "ir"},
            {"title", tone["title"]},
            {"gear", tone["gear"]},
            {"local", static_cast<bool>(tone["local"])},
        }));
      }
  return tones;
}

}  // namespace

void AgentHub::registerGuitarCommands() {
  const auto positionParam = [](bool required) {
    const char* text = "The pickup position, as the profile names it (for example bridge).";
    return required ? P::required("position", T::string, text)
                    : P::optional("position", T::string, text);
  };
  const auto takeParam = P::required("take", T::string,
                                     "Which take: soft picking, hard picking, chords or single "
                                     "notes.")
                             .oneOf(GuitarLibrary::takeNames());

  // ---- guitar.* ----
  liveCommands.add({
      "guitar.list",
      "Lists the guitar profiles: id, name, pickup type, positions, tuning, how many takes "
      "each has, its summary (outputLevel low/medium/high, brightness dark/balanced/bright, "
      "noise quiet/moderate/noisy, with the numbers behind them) and which one is the default.",
      {},
      false,
      CommandPermission::read,
      [this](const juce::var&) { return CommandResult::success(guitars().list()); },
  });

  liveCommands.add({
      "guitar.get",
      "Returns one guitar profile in full: name, pickupType, positions, tuning, notes, takes "
      "(each a recorded DI clip of one position, soft, hard, chords or notes, with its "
      "measurements: peakDb, rmsDb, lufs, crestDb, centroidHz, the band balance, lowEndShare, "
      "hum at 50 and 60 Hz and the noise floor) and the summary. Tones are built for a guitar "
      "by rendering its clips (render.clip).",
      {P::required("guitar", T::string, "The guitar's id, from guitar.list.")},
      false,
      CommandPermission::read,
      [this](const juce::var& args) {
        const juce::var profile = guitars().get(args["guitar"].toString());
        if (profile.isVoid())
          return fail(CommandError::notFound,
                      "no guitar profile with id '" + args["guitar"].toString() + "'");
        return CommandResult::success(profile);
      },
  });

  liveCommands.add({
      "guitar.create",
      "Creates a guitar profile and returns it; the first one becomes the default. Clips are "
      "added afterwards with guitar.importClip or guitar.record.start.",
      {P::required("name", T::string, "The guitar's name."),
       P::required("positions", T::array,
                   "The pickup positions clips will be recorded for, for example bridge, "
                   "middle, neck (1 to 8).")
           .items(T::string),
       P::optional("pickupType", T::string, "The kind of pickups.")
           .oneOf(GuitarLibrary::pickupTypes())
           .withDefault("other"),
       P::optional("tuning", T::string, "The tuning, as free text (E standard, drop D).")
           .withDefault(""),
       P::optional("notes", T::string, "Anything else worth knowing.").withDefault("")},
      true,
      CommandPermission::edit,
      [this](const juce::var& args) { return guitars().create(args); },
  });

  liveCommands.add({
      "guitar.update",
      "Changes a guitar profile's name, pickup type, positions, tuning or notes; fields left "
      "out stay. A position that has takes cannot be removed.",
      {P::required("guitar", T::string, "The guitar's id."),
       P::optional("name", T::string, "The guitar's name."),
       P::optional("positions", T::array, "The pickup positions.").items(T::string),
       P::optional("pickupType", T::string, "The kind of pickups.")
           .oneOf(GuitarLibrary::pickupTypes()),
       P::optional("tuning", T::string, "The tuning."),
       P::optional("notes", T::string, "Notes.")},
      true,
      CommandPermission::edit,
      [this](const juce::var& args) { return guitars().update(args["guitar"].toString(), args); },
  });

  liveCommands.add({
      "guitar.delete",
      "Deletes a guitar profile with its clips (to the system trash where there is one) and "
      "returns the remaining list.",
      {P::required("guitar", T::string, "The guitar's id.")},
      true,
      CommandPermission::destroy,
      [this](const juce::var& args) { return guitars().remove(args["guitar"].toString()); },
  });

  liveCommands.add({
      "guitar.setDefault",
      "Makes a guitar the default one: the one render.clip uses when none is named.",
      {P::required("guitar", T::string, "The guitar's id.")},
      true,
      CommandPermission::edit,
      [this](const juce::var& args) { return guitars().setDefault(args["guitar"].toString()); },
  });

  liveCommands.add({
      "guitar.importClip",
      "Attaches an existing WAV file to a guitar profile as one take of one position (copied "
      "into the profile, replacing a take already there), measures it and returns the "
      "profile. The clip should be the dry DI signal of the guitar, mono, a few seconds long.",
      {P::required("guitar", T::string, "The guitar's id."), positionParam(true), takeParam,
       P::required("path", T::string,
                   "The WAV file, as an absolute path inside the allowed folders (see "
                   "roots.list); at most 120 seconds.")
           .path()},
      true,
      CommandPermission::edit,
      [this](const juce::var& args) {
        const juce::String path = args["path"].toString();
        if (!juce::File::isAbsolutePath(path))
          return fail(CommandError::invalidArgs,
                      "guitar.importClip: argument 'path' must be an absolute path");
        return guitars().importClip(args["guitar"].toString(), args["position"].toString(),
                                    args["take"].toString(), juce::File(path));
      },
  });

  liveCommands.add({
      "guitar.analyze",
      "Measures every take of a guitar profile again and rebuilds its summary (after clips "
      "were replaced on disk, or when the measurements changed with a new version); returns "
      "the profile, with unreadable listing take files that could not be read.",
      {P::required("guitar", T::string, "The guitar's id.")},
      true,
      CommandPermission::edit,
      [this](const juce::var& args) { return guitars().analyze(args["guitar"].toString()); },
  });

  liveCommands.add({
      "guitar.record.start",
      "Starts recording a take from the live audio input: the raw signal of one input "
      "channel, before the rig, for the given number of seconds, starting now. Poll "
      "guitar.record.status; when the time is up the take is saved into the profile and "
      "measured. Replaces a recording in progress. It listens to what the user is playing, so "
      "for a command server client it counts as changing the live rig (approval).",
      {P::required("guitar", T::string, "The guitar's id."), positionParam(true), takeParam,
       P::optional("seconds", T::number, "How long to record.").range(1.0, 60.0).withDefault(8.0),
       P::optional("channel", T::integer,
                   "The device input channel the guitar is on, counted from 0.")
           .range(0, 7)
           .withDefault(0)},
      true,
      CommandPermission::live,
      [this](const juce::var& args) {
        return guitars().startRecording(args["guitar"].toString(), args["position"].toString(),
                                        args["take"].toString(),
                                        static_cast<double>(args["seconds"]),
                                        static_cast<int>(args["channel"]));
      },
  });

  liveCommands.add({
      "guitar.record.status",
      "Returns the take recording's state: idle, recording (with recordedSeconds, progress "
      "from 0 to 1 and peakDb, the input's peak so far), writing (the buffer is full and the "
      "take is being saved and measured), done (with result, the new take and its "
      "measurements) or failed (with error).",
      {},
      false,
      CommandPermission::read,
      [this](const juce::var&) { return CommandResult::success(guitars().recordingStatus()); },
  });

  liveCommands.add({
      "guitar.record.cancel",
      "Stops a take recording that is in progress and discards what was recorded.",
      {},
      true,
      CommandPermission::edit,
      [this](const juce::var&) {
        guitars().cancelRecording();
        return CommandResult::success(guitars().recordingStatus());
      },
  });

  // ---- assets.list ----
  liveCommands.add({
      "assets.list",
      "Lists the amp captures (.nam) and impulse responses (.wav) an agent can load with "
      "chain.addToneFromFiles: the files under the library folder and the folders the user "
      "added (source library, tags from the folder names), the library's asset store (source "
      "assets: the model files of the saved presets, under content-hash names) and the local "
      "model stash (source stash: files added before, also under content-hash names), and the "
      "folders the TONE3000 app keeps its downloads in when they exist (source tone3000, "
      "captures only; see nam.tone3000Folders). origin says where a file came from: library, "
      "folder (one the user added), tone3000, or loaded (a copy kept for a file the user "
      "loaded). A copy kept under a hash name is listed by the name the file was loaded "
      "under (name, with loadedFrom and loadedAt, and tone3000 with the tone, creator and "
      "licence when it was downloaded from TONE3000), else by its metadata name, and query "
      "finds it by that name. A capture carries what its file "
      "says about itself as nam_name, nam_modeled_by, nam_gear_type, nam_gear_make, "
      "nam_gear_model, nam_tone_type, nam_gain and nam_loudness when present, and its architecture, "
      "architectureLabel and supported (false when Pedaleira can not load it, with "
      "unsupportedReason in a sentence); an IR its sampleRate, channels "
      "and seconds. inRig lists the tones the rig already holds, with their titles and gear "
      "tags. There is no searchable catalogue beyond this: tones from the online catalogue "
      "are only known once they are in a rig.",
      {P::optional("kind", T::string, "Only captures or only IRs.")
           .oneOf({"all", "capture", "ir"})
           .withDefault("all"),
       P::optional("query", T::string,
                   "Only assets whose name, tags or metadata contain this text (case does not "
                   "matter)."),
       P::optional("tags", T::array,
                   "Only assets carrying every one of these tags (the user's or a folder's)."),
       P::optional("untagged", T::boolean,
                   "Only assets the user has not tagged yet (for a review of untagged items).")
           .withDefault(false),
       P::optional("limit", T::integer, "The most assets to return.").range(1, 1000).withDefault(200)},
      false,
      CommandPermission::read,
      [this](const juce::var& args) {
        // What needs the message thread is gathered here; the files are read
        // on the caller's thread.
        const juce::String rigName = revisionFor(currentSession()).rig;
        const auto sandbox = sandboxFor(currentSession(), rigName);
        const juce::var inRig = rigTones(sandbox != nullptr ? sandbox->processor() : live);
        const std::vector<juce::File> roots = allowedRoots();
        const juce::File stash = TONE3000Processor::getLocalModelsDir();
        const juce::File scratch = LibraryPaths::scratchDir();
        const juce::File libraryRoot = live.getLibraryRoot();
        const juce::File guitarsDir = LibraryPaths::guitarsDir(libraryRoot);
        // The setlist library's asset store: model files under content-hash
        // names, like the stash. Its Trash is nobody's to load from.
        const juce::File assetStore = libraryRoot.getChildFile("Assets");
        const juce::File libraryTrash = libraryRoot.getChildFile("Trash");
        const juce::String kind = args["kind"].toString();
        const juce::String query = args["query"].toString().toLowerCase();
        const int limit = args["limit"];
        const bool untaggedOnly = static_cast<bool>(args["untagged"]);
        juce::StringArray wantedTags;
        if (const auto* wanted = args["tags"].getArray())
          for (const auto& tag : *wanted)
            if (const juce::String clean = AssetTags::normalise(tag.toString()); clean.isNotEmpty())
              wantedTags.addIfNotAlreadyThere(clean);
        const std::atomic<bool>* cancel = &slowWorkCancel();
        AssetTags& tags = assetTags();

        // The folders the TONE3000 app keeps downloads in, scanned for captures
        // too (a folder that is already one of the roots is scanned once).
        // Listed, not allowed: a client that wants to load from one adds it
        // with roots.add, like any other folder.
        std::vector<std::pair<juce::File, const char*>> scanRoots;
        for (const auto& root : roots)
          scanRoots.emplace_back(root, root == stash ? "stash" : "library");
        for (const auto& folder : NamLibrary::tone3000Folders()) {
          const bool covered = std::any_of(roots.begin(), roots.end(), [&](const juce::File& r) {
            return r == folder || LibraryPaths::isInside(folder, r);
          });
          if (!covered)
            scanRoots.emplace_back(folder, "tone3000");
        }

        return CommandWork::later([this, scanRoots, stash, scratch, guitarsDir, assetStore,
                                   libraryTrash, libraryRoot, kind, query, limit, untaggedOnly,
                                   wantedTags, &tags, inRig, rigName, cancel] {
          juce::Array<juce::var> assets;
          int scanned = 0, matched = 0;
          for (const auto& [root, rootSource] : scanRoots) {
            // Renders and guitar clips are WAVs too, and neither is an IR.
            if (root == scratch || !root.isDirectory())
              continue;
            const bool tone3000Root = juce::String(rootSource) == "tone3000";
            // The TONE3000 app's folders hold captures; the IRs there are its
            // own and stay out of this list.
            const char* patterns = tone3000Root ? "*.nam" : "*.nam;*.wav";
            for (const auto& entry : juce::RangedDirectoryIterator(
                     root, root != stash, patterns, juce::File::findFiles)) {
              if (shuttingDown().load() || cancel->load() || ++scanned > kMaxFilesScanned)
                break;
              const juce::File file = entry.getFile();
              if (file.isAChildOf(guitarsDir) || file.isAChildOf(libraryTrash) ||
                  LibraryPaths::isCaptureWorkFile(file, libraryRoot))
                continue;
              const bool stored = file.isAChildOf(assetStore);
              const char* source = stored ? "assets" : rootSource;
              const char* origin = stored || root == stash ? "loaded"
                                   : tone3000Root          ? "tone3000"
                                   : file.isAChildOf(libraryRoot) ? "library"
                                                                   : "folder";
              // No tags for the store: its one folder says nothing of a file.
              const juce::File tagRoot = stored ? file.getParentDirectory() : root;
              const bool capture = file.hasFileExtension("nam");
              if ((kind == "capture" && !capture) || (kind == "ir" && capture))
                continue;
              const juce::StringArray userTags = tags.tagsFor(libraryRoot, file);
              if (untaggedOnly && !userTags.isEmpty())
                continue;
              // The cheap test first: most queries are settled by the path,
              // the user's tags or the name the file was loaded under.
              juce::var asset;
              if (query.isNotEmpty() &&
                  !file.getRelativePathFrom(root).toLowerCase().contains(query) &&
                  !userTags.joinIntoString("\n").contains(query) &&
                  !(juce::String(origin) == "loaded" &&
                    NamLibrary::loadedFor(file.getFileName()).name.toLowerCase().contains(query))) {
                if (!capture)
                  continue;
                asset = describeListed(file, tagRoot, source, userTags, origin);
                if (!juce::JSON::toString(asset, true).toLowerCase().contains(query))
                  continue;
              }
              if (!wantedTags.isEmpty()) {
                if (asset.isVoid())
                  asset = describeListed(file, tagRoot, source, userTags, origin);
                if (!hasAllTags(asset, wantedTags))
                  continue;
              }
              ++matched;
              if (assets.size() < limit)
                assets.add(asset.isVoid() ? describeListed(file, tagRoot, source, userTags, origin)
                                          : asset);
            }
          }
          return CommandResult::success(object({
              {"assets", assets},
              {"total", matched},
              {"truncated", matched > assets.size() || scanned > kMaxFilesScanned},
              {"rig", rigName},
              {"inRig", inRig},
          }));
        });
      },
  });

  // ---- assets.tag ----
  liveCommands.add({
      "assets.tag",
      "Adds tags to a capture (.nam) or IR (.wav) and removes others, in the user's own index "
      "(asset-tags.json in the library folder). Tags are kept by the file's content, so they "
      "follow it through a move or a rename. A tag is lowercased, trimmed and at most 32 "
      "characters; a file carries at most 32. Returns the file's tags after the change. See "
      "assets.tagSummary for the tags in use and a suggested vocabulary.",
      {P::required("file", T::string, "The capture or IR, an absolute path.").path(),
       P::optional("add", T::array, "Tags to add.").items(T::string),
       P::optional("remove", T::array, "Tags to remove.").items(T::string)},
      true,
      CommandPermission::edit,
      [this](const juce::var& args) {
        const juce::File file(args["file"].toString());
        if (!file.existsAsFile())
          return fail(CommandError::notFound, "no file at " + file.getFullPathName());
        if (!file.hasFileExtension("nam;wav"))
          return fail(CommandError::invalidArgs, "only captures (.nam) and IRs (.wav) are tagged");
        juce::StringArray add, remove;
        if (const auto* list = args["add"].getArray())
          for (const auto& tag : *list)
            add.add(tag.toString());
        if (const auto* list = args["remove"].getArray())
          for (const auto& tag : *list)
            remove.add(tag.toString());
        juce::StringArray result;
        if (const juce::Result updated =
                assetTags().update(live.getLibraryRoot(), file, add, remove, result);
            updated.failed())
          return fail(CommandError::invalidArgs, updated.getErrorMessage());
        juce::Array<juce::var> tags;
        for (const auto& tag : result)
          tags.add(tag);
        return CommandResult::success(object({{"file", file.getFullPathName()}, {"tags", tags}}));
      },
  });

  // ---- assets.tagSummary ----
  liveCommands.add({
      "assets.tagSummary",
      "How far the user's captures and IRs are tagged (captures and irs: total and tagged, "
      "over the same files assets.list finds), every tag in use with how many files carry it, "
      "and a suggested vocabulary to keep tags consistent (gain, character, gear, speaker, "
      "use). Agents choose tones by these tags.",
      {},
      false,
      CommandPermission::read,
      [this](const juce::var&) {
        const std::vector<juce::File> roots = allowedRoots();
        const juce::File scratch = LibraryPaths::scratchDir();
        const juce::File libraryRoot = live.getLibraryRoot();
        const juce::File guitarsDir = LibraryPaths::guitarsDir(libraryRoot);
        const juce::File libraryTrash = libraryRoot.getChildFile("Trash");
        const juce::File stash = TONE3000Processor::getLocalModelsDir();
        const std::atomic<bool>* cancel = &slowWorkCancel();
        AssetTags& tags = assetTags();

        return CommandWork::later([this, roots, scratch, libraryRoot, guitarsDir, libraryTrash,
                                   stash, &tags, cancel] {
          int captures = 0, capturesTagged = 0, irs = 0, irsTagged = 0, scanned = 0;
          juce::StringArray seen;
          for (const auto& root : roots) {
            if (root == scratch || !root.isDirectory())
              continue;
            for (const auto& entry : juce::RangedDirectoryIterator(
                     root, root != stash, "*.nam;*.wav", juce::File::findFiles)) {
              if (shuttingDown().load() || cancel->load() || ++scanned > kMaxFilesScanned)
                break;
              const juce::File file = entry.getFile();
              if (file.isAChildOf(guitarsDir) || file.isAChildOf(libraryTrash) ||
                  LibraryPaths::isCaptureWorkFile(file, libraryRoot) ||
                  seen.contains(file.getFullPathName()))
                continue;
              seen.add(file.getFullPathName());
              const bool tagged = !tags.tagsFor(libraryRoot, file).isEmpty();
              if (file.hasFileExtension("nam")) {
                ++captures;
                capturesTagged += tagged ? 1 : 0;
              } else {
                ++irs;
                irsTagged += tagged ? 1 : 0;
              }
            }
          }
          juce::Array<juce::var> inUse;
          for (const auto& [tag, count] : tags.tagCounts(libraryRoot))
            inUse.add(object({{"tag", tag}, {"count", count}}));
          juce::DynamicObject::Ptr vocabulary = new juce::DynamicObject();
          for (const auto& [group, words] : suggestedTagVocabulary()) {
            juce::Array<juce::var> list;
            for (const auto& word : words)
              list.add(word);
            vocabulary->setProperty(group, list);
          }
          return CommandResult::success(object({
              {"captures", object({{"total", captures}, {"tagged", capturesTagged}})},
              {"irs", object({{"total", irs}, {"tagged", irsTagged}})},
              {"tags", inUse},
              {"vocabulary", juce::var(vocabulary.get())},
              {"truncated", scanned > kMaxFilesScanned},
          }));
        });
      },
  });
}

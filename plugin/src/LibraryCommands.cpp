#include "EngineCommands.h"

// Processor.h first: it pulls in the JUCE module headers, which must come
// before anything that drags in <windows.h> (see EngineCommands.cpp).
#include "Processor.h"

#include "PresetImporter.h"
#include "PresetJson.h"
#include "SetlistLibrary.h"

#include <initializer_list>
#include <optional>
#include <utility>

// ####################
// SETLIST LIBRARY
// ####################
// The library's commands (docs/library.md): setlist.* for the setlists,
// the slot commands of preset.* for the presets of one, library.* for the
// library as a whole, favourite.set and recent.list. All of them are thin
// calls into ProcessorLibrary.cpp. The older preset.* commands
// (EngineCommands.cpp) address presets by id and mean "in the current
// setlist"; these address them by slot.
//
// Slots are numbered from 1, as the preset files' names show them. An
// omitted `setlist` is the current setlist; "factory" is the read-only
// setlist of the factory presets.

namespace {

using P = CommandParam;
using T = CommandParamType;
using Properties = std::initializer_list<std::pair<const char*, juce::var>>;

CommandResult fail(const char* code, const juce::String& message) {
  return CommandResult::failure(code, message);
}

juce::var withProperties(const juce::var& base, Properties properties) {
  juce::DynamicObject::Ptr obj = new juce::DynamicObject();
  if (const auto* source = base.getDynamicObject())
    for (const auto& property : source->getProperties())
      obj->setProperty(property.name, property.value);
  for (const auto& [name, value] : properties)
    obj->setProperty(name, value);
  return juce::var(obj.get());
}

P slotParam(const char* name, const juce::String& description) {
  return P::required(name, T::integer, description).range(1, SetlistLibrary::kMaxSlots);
}

P setlistParam() {
  return P::optional("setlist", T::string,
                     "Setlist id, as setlist.list returns it; the current setlist when omitted.");
}

// A path argument as a file: absolute, or relative to the working directory
// (what a script run by the render tool means by a bare file name).
juce::File fileArgument(const juce::var& path) {
  return juce::File::getCurrentWorkingDirectory().getChildFile(path.toString());
}

}  // namespace

void EngineCommands::registerLibraryCommands() {
  // "no such setlist" apart from "the engine refused": the id is looked up
  // first, as the other commands do with block and preset ids.
  const auto setlistExists = [this](const juce::String& id) {
    // (Named: the array below lives in it.)
    const juce::var listed = processor.getSetlists();
    if (const auto* setlists = listed["setlists"].getArray())
      for (const auto& setlist : *setlists)
        if (setlist["id"].toString() == id)
          return true;
    return false;
  };
  const auto setlistNotFound = [](const juce::String& id) {
    return fail(CommandError::notFound, "no setlist with id '" + id + "'");
  };
  // The setlist a slot command names, or the failure to return.
  const auto resolveSetlist = [this, setlistExists, setlistNotFound](
                                  const juce::var& args,
                                  juce::String& id) -> std::optional<CommandResult> {
    id = args["setlist"].isVoid() ? processor.currentSetlistId() : args["setlist"].toString();
    if (!setlistExists(id))
      return setlistNotFound(id);
    return std::nullopt;
  };
  // What a setlist edit answers with: the setlist as setlist.get gives it.
  const auto setlistResult = [this](const juce::String& id, Properties extra) {
    return CommandResult::success(withProperties(processor.getSetlistState(id), extra));
  };
  const auto slotHasPreset = [this](const juce::String& setlistId, int slot) {
    const juce::var setlist = processor.getSetlistState(setlistId);
    if (const auto* slots = setlist["slots"].getArray())
      for (const auto& entry : *slots)
        if (static_cast<int>(entry["slot"]) == slot)
          return true;
    return false;
  };
  const auto slotEmpty = [](int slot) {
    return fail(CommandError::notFound, "slot " + juce::String(slot) + " is empty");
  };

  // Two kinds of command live here, and the scope tells them apart.
  //
  // Those with a rig in them are in every command set, a sandbox rig's
  // included: the reads, the loads, setlist.select (which setlist that rig's
  // preset list shows), and the three that write the rig out: preset.save,
  // preset.saveToSlot and preset.exportJson. On a sandbox rig the processor
  // keeps those to additions (TONE3000Processor::sharePresetStoreWith): a new
  // preset in an empty slot, a new file, and nothing in library.json.
  //
  // Those that only reorganise the library (rename, move, copy, delete,
  // import, favourites, rescan) have no rig in them and are registered in
  // the application's command set alone. A command server client that has a
  // sandbox selected therefore reaches them on the live side, through the
  // same gate as with the live rig selected (AgentHub::begin): an edit waits
  // for approval while `live` is off, and what goes to the trash needs
  // `delete`. Selecting a sandbox is never a way around either.
  const auto addToApp = [this](Command command) {
    if (scope == Scope::app)
      commands.add(std::move(command));
  };

  // ---- setlists ----

  commands.add({
      "setlist.list",
      "Lists the setlists in order as { setlists: [{ id, name, readOnly, presetCount, current "
      "}], current }. The factory presets are the read-only setlist \"factory\", always last.",
      {},
      false,
      CommandPermission::read,
      [this](const juce::var&) { return CommandResult::success(processor.getSetlists()); },
  });

  commands.add({
      "setlist.get",
      "Returns one setlist: id, name, readOnly, folder, capacity (256), current, currentSlot (0 "
      "when none) and slots, the presets by slot with empty slots left out, each with slot, id, "
      "name, sceneCount, tempo (null unless the preset stores one), tags, favourite and file. "
      "ignored lists the .json files of the folder that are not presets, with the reason. A "
      "factory preset's sceneCount and tempo are null.",
      {setlistParam()},
      false,
      CommandPermission::read,
      [this, resolveSetlist](const juce::var& args) {
        juce::String id;
        if (auto failure = resolveSetlist(args, id))
          return *failure;
        return CommandResult::success(processor.getSetlistState(id));
      },
  });

  addToApp({
      "setlist.create",
      "Creates an empty setlist (a folder under Setlists in the library) at the end of the list "
      "and returns its id. The name must not be that of another setlist.",
      {P::required("name", T::string, "Setlist name; must not be blank."),
       P::optional("select", T::boolean, "Make it the current setlist.").withDefault(false)},
      true,
      CommandPermission::edit,
      [this](const juce::var& args) {
        if (args["name"].toString().trim().isEmpty())
          return fail(CommandError::invalidArgs,
                      "setlist.create: argument 'name' must not be blank");
        juce::String id;
        if (const juce::Result result =
                processor.createSetlist(args["name"].toString(), args["select"], &id);
            result.failed())
          return fail(CommandError::failed, result.getErrorMessage());
        return CommandResult::success(withProperties(processor.getSetlists(), {{"id", id}}));
      },
  });

  addToApp({
      "setlist.rename",
      "Renames a setlist and its folder; its id stays.",
      {P::required("id", T::string, "Setlist id."),
       P::required("name", T::string, "The new name; must not be blank.")},
      true,
      CommandPermission::edit,
      [this, setlistExists, setlistNotFound](const juce::var& args) {
        const juce::String id = args["id"].toString();
        if (args["name"].toString().trim().isEmpty())
          return fail(CommandError::invalidArgs,
                      "setlist.rename: argument 'name' must not be blank");
        if (!setlistExists(id))
          return setlistNotFound(id);
        if (const juce::Result result = processor.renameSetlist(id, args["name"].toString());
            result.failed())
          return fail(CommandError::failed, result.getErrorMessage());
        return CommandResult::success(processor.getSetlists());
      },
  });

  addToApp({
      "setlist.delete",
      "Moves a setlist's folder, presets included, to the system trash (a Trash folder in the "
      "library where there is none). Model files in Assets stay. When it was the current "
      "setlist, the first remaining one becomes current.",
      {P::required("id", T::string, "Setlist id.")},
      true,
      CommandPermission::destroy,
      [this, setlistExists, setlistNotFound](const juce::var& args) {
        const juce::String id = args["id"].toString();
        if (!setlistExists(id))
          return setlistNotFound(id);
        if (const juce::Result result = processor.deleteSetlist(id); result.failed())
          return fail(CommandError::failed, result.getErrorMessage());
        return CommandResult::success(withProperties(processor.getSetlists(), {{"deletedId", id}}));
      },
  });

  addToApp({
      "setlist.reorder",
      "Moves a setlist to another place in the list of setlists.",
      {P::required("id", T::string, "Setlist id."),
       P::required("index", T::integer, "Its new place, 0 for the first.").range(0, 9999)},
      true,
      CommandPermission::edit,
      [this, setlistExists, setlistNotFound](const juce::var& args) {
        const juce::String id = args["id"].toString();
        if (!setlistExists(id))
          return setlistNotFound(id);
        if (const juce::Result result = processor.reorderSetlist(id, args["index"]);
            result.failed())
          return fail(CommandError::failed, result.getErrorMessage());
        return CommandResult::success(processor.getSetlists());
      },
  });

  commands.add({
      "setlist.select",
      "Makes a setlist the current one: the preset list, program changes, the footswitches' "
      "preset banks and the preset.* commands that take an id all follow. Loads nothing.",
      {P::required("id", T::string, "Setlist id, or \"factory\".")},
      true,
      CommandPermission::edit,
      [this, setlistExists, setlistNotFound, setlistResult](const juce::var& args) {
        const juce::String id = args["id"].toString();
        if (!setlistExists(id))
          return setlistNotFound(id);
        if (const juce::Result result = processor.selectSetlist(id); result.failed())
          return fail(CommandError::failed, result.getErrorMessage());
        return setlistResult(id, {});
      },
  });

  // ---- presets by slot ----

  commands.add({
      "preset.saveToSlot",
      "Saves the current chain and preset-managed parameters into a slot of a setlist, makes "
      "that the active preset and the setlist the current one. A preset already in the slot is "
      "replaced and keeps its id; name may then be omitted to keep its name too.",
      {slotParam("slot", "The slot, 1 to 256."),
       P::optional("name", T::string, "Preset name; required for an empty slot."), setlistParam()},
      true,
      CommandPermission::edit,
      [this, resolveSetlist](const juce::var& args) {
        juce::String setlistId;
        if (auto failure = resolveSetlist(args, setlistId))
          return *failure;
        juce::var saved;
        if (const juce::Result result = processor.savePresetToSlot(
                setlistId, args["slot"], args["name"].toString(), &saved);
            result.failed())
          return fail(CommandError::failed, result.getErrorMessage());
        return CommandResult::success(withProperties(
            saved, {{"revision", static_cast<int>(processor.getCurrentChainRevision())}}));
      },
  });

  commands.add({
      "preset.loadSlot",
      "Loads the preset in a slot (as preset.load does) and makes its setlist the current one.",
      {slotParam("slot", "The slot, 1 to 256."), setlistParam()},
      true,
      CommandPermission::edit,
      [this, resolveSetlist, slotHasPreset, slotEmpty](const juce::var& args) {
        juce::String setlistId;
        if (auto failure = resolveSetlist(args, setlistId))
          return *failure;
        const int slot = args["slot"];
        if (!slotHasPreset(setlistId, slot))
          return slotEmpty(slot);
        if (const juce::Result result = processor.loadPresetSlot(setlistId, slot);
            result.failed())
          return fail(CommandError::failed, result.getErrorMessage());
        return layoutResult(withProperties(
            {}, {{"preset", processor.getChainState(-1)["preset"]},
                 {"setlist", setlistId},
                 {"slot", slot}}));
      },
  });

  addToApp({
      "preset.moveSlot",
      "Moves a preset to another slot of its setlist and renumbers the files. insert (the "
      "default) shifts the presets and gaps in between one slot towards where it came from; "
      "swap exchanges the two slots.",
      {slotParam("from", "The slot the preset is in."), slotParam("to", "The slot it goes to."),
       P::optional("mode", T::string, "What happens to the slots in between.")
           .oneOf({"insert", "swap"})
           .withDefault("insert"),
       setlistParam()},
      true,
      CommandPermission::edit,
      [this, resolveSetlist, slotHasPreset, slotEmpty, setlistResult](const juce::var& args) {
        juce::String setlistId;
        if (auto failure = resolveSetlist(args, setlistId))
          return *failure;
        const int from = args["from"];
        if (setlistId != TONE3000Processor::kFactorySetlistId && !slotHasPreset(setlistId, from))
          return slotEmpty(from);
        if (const juce::Result result = processor.movePresetSlot(
                setlistId, from, args["to"], args["mode"].toString() == "insert");
            result.failed())
          return fail(CommandError::failed, result.getErrorMessage());
        return setlistResult(setlistId, {});
      },
  });

  addToApp({
      "preset.duplicateSlot",
      "Copies a preset within its setlist under a new id, as \"<name> copy\", into the given "
      "empty slot or the first empty slot after it.",
      {slotParam("slot", "The slot to copy."),
       P::optional("to", T::integer, "The empty slot the copy goes to.")
           .range(1, SetlistLibrary::kMaxSlots),
       setlistParam()},
      true,
      CommandPermission::edit,
      [this, resolveSetlist, slotHasPreset, slotEmpty, setlistResult](const juce::var& args) {
        juce::String setlistId;
        if (auto failure = resolveSetlist(args, setlistId))
          return *failure;
        const int slot = args["slot"];
        if (setlistId != TONE3000Processor::kFactorySetlistId && !slotHasPreset(setlistId, slot))
          return slotEmpty(slot);
        juce::var made;
        if (const juce::Result result = processor.duplicatePresetSlot(
                setlistId, slot, args["to"].isVoid() ? 0 : static_cast<int>(args["to"]), &made);
            result.failed())
          return fail(CommandError::failed, result.getErrorMessage());
        return setlistResult(setlistId, {{"preset", made}});
      },
  });

  addToApp({
      "preset.deleteSlot",
      "Moves the preset file of a slot to the system trash and leaves the slot empty; the other "
      "presets keep their slots and the loaded chain is left as it is.",
      {slotParam("slot", "The slot to empty."), setlistParam()},
      true,
      CommandPermission::destroy,
      [this, resolveSetlist, slotHasPreset, slotEmpty, setlistResult](const juce::var& args) {
        juce::String setlistId;
        if (auto failure = resolveSetlist(args, setlistId))
          return *failure;
        const int slot = args["slot"];
        if (setlistId != TONE3000Processor::kFactorySetlistId && !slotHasPreset(setlistId, slot))
          return slotEmpty(slot);
        if (const juce::Result result = processor.deletePresetSlot(setlistId, slot);
            result.failed())
          return fail(CommandError::failed, result.getErrorMessage());
        return setlistResult(setlistId, {{"deletedSlot", slot}});
      },
  });

  addToApp({
      "preset.copyToSetlist",
      "Copies a preset into another setlist under a new id, into the given empty slot or the "
      "first empty one. The source may be a factory preset; model files are shared, not "
      "copied.",
      {slotParam("slot", "The slot to copy."),
       P::required("to", T::string, "Id of the setlist the copy goes to."),
       P::optional("toSlot", T::integer, "The empty slot the copy goes to.")
           .range(1, SetlistLibrary::kMaxSlots),
       setlistParam()},
      true,
      CommandPermission::edit,
      [this, resolveSetlist, slotHasPreset, slotEmpty, setlistExists, setlistNotFound,
       setlistResult](const juce::var& args) {
        juce::String setlistId;
        if (auto failure = resolveSetlist(args, setlistId))
          return *failure;
        const juce::String target = args["to"].toString();
        if (!setlistExists(target))
          return setlistNotFound(target);
        const int slot = args["slot"];
        if (!slotHasPreset(setlistId, slot))
          return slotEmpty(slot);
        juce::var made;
        if (const juce::Result result = processor.copyPresetToSetlist(
                setlistId, slot, target,
                args["toSlot"].isVoid() ? 0 : static_cast<int>(args["toSlot"]), &made);
            result.failed())
          return fail(CommandError::failed, result.getErrorMessage());
        return setlistResult(target, {{"preset", made}});
      },
  });

  commands.add({
      "preset.exportJson",
      "Writes a preset as a JSON file at any path: the preset with the given id, or without an "
      "id the rig as it is now. Model files and large plugin states go into an Assets folder "
      "beside the file (the layout preset.importJson reads), or into the file itself as base64 "
      "with embedAssets.",
      {P::required("path", T::string, "The file to write.").path().withSibling("Assets"),
       P::optional("id", T::string, "Preset id; the current rig when omitted."),
       P::optional("name", T::string, "A name for the exported preset."),
       P::optional("embedAssets", T::boolean, "One self-contained file.").withDefault(false)},
      // Nothing of the rig changes, but a file is written: an edit, not a read
      // (a client that may only look must not be able to write files).
      true,
      CommandPermission::edit,
      [this](const juce::var& args) {
        const juce::String id = args["id"].toString();
        const juce::File file = fileArgument(args["path"]);
        if (args["path"].toString().trim().isEmpty())
          return fail(CommandError::invalidArgs, "preset.exportJson: 'path' must not be blank");
        if (id.isNotEmpty() && !processor.presetExists(id))
          return fail(CommandError::notFound, "no preset with id '" + id + "'");
        if (const juce::Result result = processor.exportPresetJson(
                id, file, args["embedAssets"], args["name"].toString());
            result.failed())
          return fail(CommandError::failed, result.getErrorMessage());
        juce::DynamicObject::Ptr obj = new juce::DynamicObject();
        obj->setProperty("path", file.getFullPathName());
        return CommandResult::success(juce::var(obj.get()));
      },
  });

  addToApp({
      "preset.importJson",
      "Reads a JSON preset (or a .t3kpreset) from any path into an empty slot of a setlist: the "
      "given one or the first empty one. Its assets are taken from the file, from an Assets "
      "folder beside it or from the library, and stored in the library; ones nobody has are "
      "listed in missingAssets and the preset is imported all the same. Loads nothing.",
      {P::required("path", T::string, "The file to read.").path().withSibling("Assets"),
       P::optional("slot", T::integer, "The empty slot it goes to.")
           .range(1, SetlistLibrary::kMaxSlots),
       setlistParam()},
      true,
      CommandPermission::edit,
      [this, resolveSetlist, setlistResult](const juce::var& args) {
        juce::String setlistId;
        if (auto failure = resolveSetlist(args, setlistId))
          return *failure;
        const juce::File file = fileArgument(args["path"]);
        if (!file.existsAsFile())
          return fail(CommandError::notFound, "file not found: " + file.getFullPathName());
        juce::var made;
        if (const juce::Result result = processor.importPresetJson(
                file, setlistId, args["slot"].isVoid() ? 0 : static_cast<int>(args["slot"]),
                &made);
            result.failed())
          return fail(CommandError::failed, result.getErrorMessage());
        return setlistResult(setlistId, {{"preset", made}});
      },
  });

  // preset.importForeign and its dry run, preset.previewForeign: read the
  // foreign file and the mapping table, convert, and check the result is a
  // preset the library can read. Fills `converted` and `tree`, or returns why
  // not.
  const auto convertForeign =
      [](const juce::var& args, t3k::importer::ImportResult& converted,
         t3k::presetjson::ReadResult& tree) -> std::optional<CommandResult> {
        const juce::File file = fileArgument(args["file"]);
        if (!file.existsAsFile())
          return fail(CommandError::notFound, "file not found: " + file.getFullPathName());
        juce::String problem;
        const juce::var foreign = t3k::importer::readJsonFile(file, problem);
        if (problem.isNotEmpty())
          return fail(CommandError::failed,
                      "cannot read " + file.getFullPathName() + ": " + problem);

        if (const juce::String issue = t3k::importer::checkForeignPreset(foreign);
            issue.isNotEmpty())
          return fail(CommandError::failed, issue);

        juce::var mapping;
        if (args["mapping"].isVoid() || args["mapping"].toString().trim().isEmpty()) {
          const juce::String source = foreign["source"].toString();
          mapping = t3k::importer::builtInMapping(source);
          if (mapping.isVoid())
            return fail(CommandError::failed,
                        "no built-in mapping table for the source \"" + source +
                            "\" (available: " +
                            t3k::importer::builtInMappingSources().joinIntoString(", ") +
                            "); pass one as `mapping`");
        } else {
          const juce::File mappingFile = fileArgument(args["mapping"]);
          if (!mappingFile.existsAsFile())
            return fail(CommandError::notFound,
                        "file not found: " + mappingFile.getFullPathName());
          mapping = t3k::importer::readJsonFile(mappingFile, problem);
          if (problem.isNotEmpty())
            return fail(CommandError::failed,
                        "cannot read " + mappingFile.getFullPathName() + ": " + problem);
        }

        converted = t3k::importer::convertForeignPreset(foreign, mapping);
        if (!converted.ok)
          return fail(CommandError::failed, converted.error);
        tree = t3k::presetjson::fromJson(converted.presetJson, {});
        if (!tree.preset.isValid())
          return fail(CommandError::failed, "the converted preset is not valid: " + tree.error);
        return std::nullopt;
      };

  commands.add({
      "preset.previewForeign",
      "The dry run of preset.importForeign: reads and converts a foreign preset the same way, "
      "with the same mapping table, and returns what the import would make without saving or "
      "loading anything: { name, report }, the report as preset.importForeign returns it. Every "
      "block of the file appears in exactly one of the report's mapped, approximated (type "
      "block) or unmapped lists, so a caller can show what each block becomes before importing.",
      {P::required("file", T::string, "The foreign preset JSON to read.").path(),
       P::optional("mapping", T::string,
                   "A mapping table to use instead of the built-in one for the file's source.")
           .path()},
      false,
      CommandPermission::read,
      [convertForeign](const juce::var& args) {
        t3k::importer::ImportResult converted;
        t3k::presetjson::ReadResult tree;
        if (auto failure = convertForeign(args, converted, tree))
          return *failure;
        return CommandResult::success(
            withProperties({}, {{"name", converted.preset["name"]}, {"report", converted.report}}));
      },
  });

  addToApp({
      "preset.importForeign",
      "Imports a preset read from another product (a foreign preset JSON, docs/import.md; "
      "tools/import/quadcortex_dump.py writes them from a Quad Cortex) into an empty slot of a "
      "setlist, as a grid preset: its blocks become ours through a mapping table (the built-in "
      "one for the file's source, or the one at `mapping`), rows and splits keep their layout as "
      "far as the grid allows, and scenes carry what the scene model can hold. Amps, cabs and "
      "captures are never mapped: their slots stay empty for the player to fill. Returns the new "
      "preset as preset.importJson does, plus report: { mapped, approximated, unmapped, "
      "droppedParams, notes }, each entry naming the block by row, position and the source's "
      "model. Loads nothing.",
      {P::required("file", T::string, "The foreign preset JSON to read.").path(),
       P::optional("mapping", T::string,
                   "A mapping table to use instead of the built-in one for the file's source.")
           .path(),
       P::optional("slot", T::integer, "The empty slot it goes to.")
           .range(1, SetlistLibrary::kMaxSlots),
       setlistParam()},
      true,
      CommandPermission::edit,
      [this, resolveSetlist, setlistResult, convertForeign](const juce::var& args) {
        juce::String setlistId;
        if (auto failure = resolveSetlist(args, setlistId))
          return *failure;
        t3k::importer::ImportResult converted;
        t3k::presetjson::ReadResult tree;
        if (auto failure = convertForeign(args, converted, tree))
          return *failure;

        juce::var made;
        if (const juce::Result result = processor.importPresetTree(
                tree.preset, tree.missing, setlistId,
                args["slot"].isVoid() ? 0 : static_cast<int>(args["slot"]), &made);
            result.failed())
          return fail(CommandError::failed, result.getErrorMessage());
        return setlistResult(setlistId, {{"preset", made}, {"report", converted.report}});
      },
  });

  // ---- the library ----

  commands.add({
      "library.get",
      "Returns the library: root (its folder), setlists (as setlist.list), current (setlist, "
      "slot or 0, and presetId, the active preset's id), favourites (preset ids), recents (as "
      "recent.list), unsaved (the rig differs from the active preset; with no active preset, "
      "from the empty default) and legacyPresets (how many presets of the old .t3kpreset "
      "store library.importLegacyPresets would bring over).",
      {},
      false,
      CommandPermission::read,
      [this](const juce::var&) { return CommandResult::success(processor.getLibraryState()); },
  });

  addToApp({
      "library.rescan",
      "Reads the library's folders again from scratch, for changes made outside the app (a file "
      "browser, git) that the usual time and size check could miss. With tidy the preset files "
      "are also renamed to what their slots and names make them (a file added without a slot "
      "number gets one). Returns library.get plus renamed.",
      {P::optional("tidy", T::boolean, "Also rename the preset files.").withDefault(false)},
      true,
      CommandPermission::edit,
      [this](const juce::var& args) {
        return CommandResult::success(processor.rescanLibrary(args["tidy"]));
      },
  });

  addToApp({
      "library.importLegacyPresets",
      "Copies the user presets of the old .t3kpreset store into a setlist as JSON presets, "
      "keeping their ids. The setlist is made when missing and a second one is started when "
      "it is full. The old files are neither moved nor deleted, and presets imported before "
      "are skipped. (An empty library brings the old preset list over by itself the first time "
      "it is used; this is for presets deleted since, or a library that was not empty.) Returns "
      "{ imported, skipped, failed, setlists }.",
      {P::optional("setlist", T::string, "Name of the setlist to import into.")
           .withDefault("Imported Presets")},
      true,
      CommandPermission::edit,
      [this](const juce::var& args) {
        return CommandResult::success(processor.importLegacyPresets(args["setlist"].toString()));
      },
  });

  addToApp({
      "favourite.set",
      "Marks a preset as a favourite or takes the mark away. Favourites are a set of preset "
      "ids kept in library.json; a factory preset can be one.",
      {P::required("id", T::string, "Preset id."),
       P::required("favourite", T::boolean, "Whether it is a favourite.")},
      true,
      CommandPermission::edit,
      [this](const juce::var& args) {
        const juce::String id = args["id"].toString();
        if (id.trim().isEmpty())
          return fail(CommandError::invalidArgs, "favourite.set: argument 'id' must not be blank");
        if (const juce::Result result = processor.setPresetFavourite(id, args["favourite"]);
            result.failed())
          return fail(CommandError::failed, result.getErrorMessage());
        return CommandResult::success(
            withProperties({}, {{"favourites", processor.getLibraryState()["favourites"]}}));
      },
  });

  commands.add({
      "recent.list",
      "Lists the presets loaded last, most recent first (at most 32), as { recents: [{ id, "
      "name, setlist, slot, factory }] }. Presets that no longer exist are left out.",
      {},
      false,
      CommandPermission::read,
      [this](const juce::var&) {
        return CommandResult::success(
            withProperties({}, {{"recents", processor.getRecentPresets()}}));
      },
  });
}

// The setlist library's file layer (SetlistLibrary.h, docs/library.md) on
// small hand-made presets: the folder layout, every setlist and slot
// operation, the naming rules for the three platforms, what a rescan makes
// of changes made behind its back, and the library's own state file. The
// processor on top of it is library_tests.cpp.
#include "PresetFile.h"
#include "SetlistLibrary.h"

#include <gtest/gtest.h>

#include <map>
#include <set>
#include <vector>

namespace {

namespace presetjson = t3k::presetjson;

// A library in a temp folder with a trash folder of its own.
struct TempLibrary {
  TempLibrary()
      : dir(juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("t3k-setlist-tests-" + juce::Uuid().toString())),
        root(dir.getChildFile("Pedaleira")),
        trashDir(dir.getChildFile("Trash")),
        library(root) {
    dir.createDirectory();
    previousTrash = SetlistLibrary::setTrashDirForTesting(trashDir);
  }
  ~TempLibrary() {
    SetlistLibrary::setTrashDirForTesting(previousTrash);
    dir.deleteRecursively();
  }

  juce::String create(const juce::String& name) {
    SetlistLibrary::Setlist made;
    const juce::Result result = library.createSetlist(name, &made);
    EXPECT_TRUE(result.wasOk()) << result.getErrorMessage();
    return made.id;
  }

  juce::File dir, root, trashDir, previousTrash;
  SetlistLibrary library;
};

juce::ValueTree presetNamed(const juce::String& name, double bass = 5.0,
                            const juce::MemoryBlock* model = nullptr) {
  juce::ValueTree preset(t3k::presetfile::kTag);
  preset.setProperty("schemaVersion", 1, nullptr);
  preset.setProperty("name", name, nullptr);
  juce::ValueTree chain("ChainSnapshot");
  chain.setProperty("stereoEnabled", false, nullptr);
  if (model != nullptr) {
    juce::ValueTree cached("CachedModel");
    cached.setProperty("modelId", 1, nullptr);
    cached.setProperty("data", juce::var(*model), nullptr);
    juce::ValueTree cache("ModelCache");
    cache.appendChild(cached, nullptr);
    juce::ValueTree block("ChainBlock");
    block.setProperty("type", "nam", nullptr);
    block.appendChild(cache, nullptr);
    juce::ValueTree blocks("ChainBlocks");
    blocks.appendChild(block, nullptr);
    chain.appendChild(blocks, nullptr);
  }
  preset.appendChild(chain, nullptr);
  juce::ValueTree params("Params");
  juce::ValueTree param("Param");
  param.setProperty("id", "toneBass", nullptr);
  param.setProperty("value", bass, nullptr);
  params.appendChild(param, nullptr);
  preset.appendChild(params, nullptr);
  return preset;
}

SetlistLibrary::Slot save(SetlistLibrary& library, const juce::String& setlistId,
                          const juce::String& name, int slot = 0, double bass = 5.0) {
  SetlistLibrary::SaveOptions options;
  options.slot = slot;
  SetlistLibrary::Slot saved;
  const juce::Result result = library.save(setlistId, presetNamed(name, bass), options, &saved);
  EXPECT_TRUE(result.wasOk()) << name << ": " << result.getErrorMessage();
  return saved;
}

// "1:Name 3:Other": the listing at a glance.
juce::String layout(const SetlistLibrary::Listing& listing) {
  juce::StringArray parts;
  for (const auto& slot : listing.slots)
    parts.add(juce::String(slot.slot) + ":" + slot.name);
  return parts.joinIntoString(" ");
}

juce::StringArray presetFiles(const juce::File& folder) {
  juce::StringArray names;
  for (const auto& file : folder.findChildFiles(juce::File::findFiles, false, "*.json"))
    if (file.getFileName() != "setlist.json")
      names.add(file.getFileName());
  names.sort(false);
  return names;
}

double bassOf(const SetlistLibrary& library, const SetlistLibrary::Listing& listing, int slot) {
  const auto* entry = listing.at(slot);
  if (entry == nullptr)
    return -1.0;
  const presetjson::ReadResult read = library.load({listing.setlist, *entry});
  EXPECT_TRUE(read.preset.isValid()) << read.error;
  return read.preset.getChildWithName("Params").getChild(0).getProperty("value");
}

juce::String utf8(const char* text) { return juce::String::fromUTF8(text); }

void writeFile(const juce::File& file, const juce::String& text) {
  file.deleteFile();
  ASSERT_TRUE(file.getParentDirectory().createDirectory());
  juce::FileOutputStream out(file);
  ASSERT_TRUE(out.openedOk());
  out.write(text.toRawUTF8(), text.getNumBytesAsUTF8());
}

}  // namespace

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

TEST(SetlistLibraryTest, AnEmptyLibraryIsNothingOnDisk) {
  TempLibrary temp;
  EXPECT_TRUE(temp.library.setlists().empty());
  EXPECT_TRUE(temp.library.favourites().isEmpty());
  EXPECT_TRUE(temp.library.recents().isEmpty());
  EXPECT_EQ(temp.library.current().slot, 0);
  EXPECT_FALSE(temp.library.list("nope").found);
  EXPECT_FALSE(temp.library.find("user:nope").has_value());
  // Looking never makes the folder: a fresh install has no Pedaleira folder in
  // Documents until something is saved.
  EXPECT_FALSE(temp.root.exists());
}

TEST(SetlistLibraryTest, TheFolderLayoutIsWhatTheDocsSay) {
  TempLibrary temp;
  const juce::String id = temp.create("Friday Gig");
  const juce::MemoryBlock model("model bytes, stored once", 24);
  SetlistLibrary::Slot first, second;
  ASSERT_TRUE(temp.library.save(id, presetNamed("Glass Cleans", 5.0, &model), {}, &first).wasOk());
  ASSERT_TRUE(temp.library.save(id, presetNamed("Big Lead", 7.0, &model), {}, &second).wasOk());
  SetlistLibrary::SaveOptions twelfth;
  twelfth.slot = 12;
  ASSERT_TRUE(temp.library.save(id, presetNamed("Encore"), twelfth).wasOk());

  const juce::File folder = temp.root.getChildFile("Setlists").getChildFile("Friday Gig");
  ASSERT_TRUE(folder.isDirectory());
  EXPECT_EQ(presetFiles(folder),
            juce::StringArray({"001 Glass Cleans.json", "002 Big Lead.json", "012 Encore.json"}));

  // setlist.json: the id and the name, and nothing the file names say.
  const juce::var meta = juce::JSON::parse(folder.getChildFile("setlist.json").loadFileAsString());
  EXPECT_EQ(meta["format"].toString(), juce::String("pedaleira.setlist"));
  EXPECT_EQ(static_cast<int>(meta["version"]), 1);
  EXPECT_EQ(meta["id"].toString(), id);
  EXPECT_EQ(meta["name"].toString(), juce::String("Friday Gig"));

  // The preset: text, its id inside, its model in the store once.
  const juce::String text = folder.getChildFile("001 Glass Cleans.json").loadFileAsString();
  const juce::var parsed = juce::JSON::parse(text);
  EXPECT_EQ(parsed["format"].toString(), juce::String("pedaleira.preset"));
  EXPECT_EQ(parsed["name"].toString(), juce::String("Glass Cleans"));
  EXPECT_EQ("user:" + parsed["id"].toString(), first.id);
  EXPECT_NE(first.id, second.id);
  const auto assets = presetjson::referencedAssets(parsed);
  ASSERT_EQ(assets.size(), 1u);
  const juce::File assetsDir = temp.root.getChildFile("Assets");
  EXPECT_TRUE(assetsDir.getChildFile(assets[0].hash + ".nam").existsAsFile());
  EXPECT_EQ(assetsDir.getNumberOfChildFiles(juce::File::findFiles), 1) << "two presets, one model";
  EXPECT_FALSE(text.contains("model bytes"));

  // library.json exists once the library has something to remember.
  const juce::var state = juce::JSON::parse(temp.root.getChildFile("library.json").loadFileAsString());
  EXPECT_EQ(state["format"].toString(), juce::String("pedaleira.library"));
  EXPECT_EQ(state["setlistOrder"][0].toString(), id);

  // No scratch files left behind.
  EXPECT_EQ(folder.findChildFiles(juce::File::findFiles, false, "*.tmp").size(), 0);
  EXPECT_EQ(folder.getNumberOfChildFiles(juce::File::findFiles), 4);

  // And it reads back.
  const SetlistLibrary::Listing listing = temp.library.list(id);
  EXPECT_EQ(layout(listing), juce::String("1:Glass Cleans 2:Big Lead 12:Encore"));
  EXPECT_TRUE(listing.ignored.empty());
  const presetjson::ReadResult read = temp.library.load({listing.setlist, *listing.at(1)});
  ASSERT_TRUE(read.preset.isValid()) << read.error;
  EXPECT_TRUE(read.missing.empty());
  const auto* data = read.preset.getChildWithName("ChainSnapshot")
                         .getChildWithName("ChainBlocks")
                         .getChild(0)
                         .getChildWithName("ModelCache")
                         .getChild(0)
                         .getProperty("data")
                         .getBinaryData();
  ASSERT_NE(data, nullptr);
  EXPECT_TRUE(*data == model);
}

// ---------------------------------------------------------------------------
// Setlists
// ---------------------------------------------------------------------------

TEST(SetlistLibraryTest, SetlistsAreCreatedRenamedReorderedAndTrashed) {
  TempLibrary temp;
  const juce::String a = temp.create("Rehearsal");
  const juce::String b = temp.create("Friday Gig");
  const juce::String c = temp.create("Covers");
  const auto names = [&] {
    juce::StringArray out;
    for (const auto& setlist : temp.library.setlists())
      out.add(setlist.name);
    return out.joinIntoString(", ");
  };
  // In the order they were made, not by name.
  EXPECT_EQ(names(), juce::String("Rehearsal, Friday Gig, Covers"));

  // Names are unique whatever their case, and never blank.
  EXPECT_TRUE(temp.library.createSetlist("friday gig").failed());
  EXPECT_TRUE(temp.library.createSetlist("   ").failed());
  EXPECT_TRUE(temp.library.renameSetlist(a, "COVERS").failed());
  EXPECT_TRUE(temp.library.renameSetlist(a, "").failed());
  EXPECT_TRUE(temp.library.renameSetlist("nope", "x").failed());

  // Reorder: to the front, to the end, past the end (clamped).
  ASSERT_TRUE(temp.library.reorderSetlist(c, 0).wasOk());
  EXPECT_EQ(names(), juce::String("Covers, Rehearsal, Friday Gig"));
  ASSERT_TRUE(temp.library.reorderSetlist(c, 99).wasOk());
  EXPECT_EQ(names(), juce::String("Rehearsal, Friday Gig, Covers"));
  ASSERT_TRUE(temp.library.reorderSetlist(b, 0).wasOk());
  EXPECT_EQ(names(), juce::String("Friday Gig, Rehearsal, Covers"));
  EXPECT_TRUE(temp.library.reorderSetlist("nope", 0).failed());
  // The order is on disk: another instance sees it.
  {
    SetlistLibrary other(temp.root);
    EXPECT_EQ(other.setlists()[0].id, b);
    EXPECT_EQ(other.setlists()[2].id, c);
  }

  // Rename: the folder follows, the id and the presets stay.
  const SetlistLibrary::Slot saved = save(temp.library, a, "Crunch");
  ASSERT_TRUE(temp.library.renameSetlist(a, "Rehearsal Room").wasOk());
  const juce::File setlists = temp.root.getChildFile("Setlists");
  EXPECT_FALSE(setlists.getChildFile("Rehearsal").exists());
  EXPECT_TRUE(setlists.getChildFile("Rehearsal Room").getChildFile("001 Crunch.json").existsAsFile());
  EXPECT_EQ(temp.library.findSetlist(a)->name, juce::String("Rehearsal Room"));
  EXPECT_EQ(layout(temp.library.list(a)), juce::String("1:Crunch"));
  EXPECT_EQ(temp.library.list(a).slots[0].id, saved.id);
  EXPECT_EQ(names(), juce::String("Friday Gig, Rehearsal Room, Covers"));
  // A change of case only is a rename too.
  ASSERT_TRUE(temp.library.renameSetlist(a, "rehearsal room").wasOk());
  EXPECT_EQ(temp.library.findSetlist(a)->folder.getFileName(), juce::String("rehearsal room"));
  EXPECT_EQ(layout(temp.library.list(a)), juce::String("1:Crunch"));

  // Delete: to the trash with what is in it, never gone.
  temp.library.setCurrent({a, 1});
  ASSERT_TRUE(temp.library.deleteSetlist(a).wasOk());
  EXPECT_EQ(names(), juce::String("Friday Gig, Covers"));
  EXPECT_FALSE(setlists.getChildFile("rehearsal room").exists());
  EXPECT_TRUE(temp.trashDir.getChildFile("rehearsal room").getChildFile("001 Crunch.json").existsAsFile());
  EXPECT_TRUE(temp.library.current().setlistId.isEmpty()) << "the position went with it";
  EXPECT_TRUE(temp.library.deleteSetlist(a).failed());
  EXPECT_FALSE(temp.library.find(saved.id).has_value());
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

TEST(SetlistLibraryTest, SavingFillsSlotsAndKeepsIdentity) {
  TempLibrary temp;
  const juce::String id = temp.create("Set");

  // No slot named: the first empty one.
  const SetlistLibrary::Slot a = save(temp.library, id, "Alpha", 0, 1.0);
  const SetlistLibrary::Slot b = save(temp.library, id, "Beta", 0, 2.0);
  EXPECT_EQ(a.slot, 1);
  EXPECT_EQ(b.slot, 2);
  EXPECT_TRUE(a.id.startsWith("user:"));
  // A slot named, leaving gaps.
  const SetlistLibrary::Slot g = save(temp.library, id, "Gamma", 7, 3.0);
  EXPECT_EQ(g.slot, 7);
  EXPECT_EQ(save(temp.library, id, "Delta", 0, 4.0).slot, 3) << "gaps fill from the front";
  EXPECT_EQ(layout(temp.library.list(id)), juce::String("1:Alpha 2:Beta 3:Delta 7:Gamma"));

  // Same name, no slot: that preset is updated in place, id and all.
  const SetlistLibrary::Slot again = save(temp.library, id, "beta", 0, 9.0);
  EXPECT_EQ(again.slot, 2);
  EXPECT_EQ(again.id, b.id);
  EXPECT_EQ(again.name, juce::String("beta"));
  const juce::File folder = temp.library.findSetlist(id)->folder;
  EXPECT_TRUE(folder.getChildFile("002 beta.json").existsAsFile());
  EXPECT_EQ(presetFiles(folder).size(), 4);
  EXPECT_DOUBLE_EQ(bassOf(temp.library, temp.library.list(id), 2), 9.0);

  // Saving into an occupied slot replaces it and keeps the slot's id, so
  // favourites and program numbers keep pointing at "the preset in slot 7".
  const SetlistLibrary::Slot over = save(temp.library, id, "Gamma Two", 7, 5.5);
  EXPECT_EQ(over.id, g.id);
  EXPECT_EQ(presetFiles(folder),
            juce::StringArray({"001 Alpha.json", "002 beta.json", "003 Delta.json",
                               "007 Gamma Two.json"}));
  EXPECT_DOUBLE_EQ(bassOf(temp.library, temp.library.list(id), 7), 5.5);

  // find() by id, anywhere.
  const auto found = temp.library.find(g.id);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->setlist.id, id);
  EXPECT_EQ(found->slot.slot, 7);
  EXPECT_FALSE(temp.library.find("factory:" + g.id.substring(5)).has_value());

  // What cannot be saved.
  SetlistLibrary::SaveOptions options;
  EXPECT_TRUE(temp.library.save("nope", presetNamed("x"), options).failed());
  EXPECT_TRUE(temp.library.save(id, presetNamed("   "), options).failed());
  options.slot = SetlistLibrary::kMaxSlots + 1;
  EXPECT_TRUE(temp.library.save(id, presetNamed("x"), options).failed());
  options.slot = -1;
  EXPECT_TRUE(temp.library.save(id, presetNamed("x"), options).failed());

  // keepId: an import stays findable under the id it had, once.
  juce::ValueTree imported = presetNamed("Imported");
  imported.setProperty("id", "legacy-id-1", nullptr);
  options.slot = 20;
  options.keepId = true;
  SetlistLibrary::Slot kept;
  ASSERT_TRUE(temp.library.save(id, imported, options, &kept).wasOk());
  EXPECT_EQ(kept.id, juce::String("user:legacy-id-1"));
  options.slot = 21;
  ASSERT_TRUE(temp.library.save(id, imported.createCopy(), options, &kept).wasOk());
  EXPECT_NE(kept.id, juce::String("user:legacy-id-1")) << "two presets must not share an id";
}

TEST(SetlistLibraryTest, RenameMoveDuplicateDeleteAndCopy) {
  TempLibrary temp;
  const juce::String id = temp.create("Set");
  const juce::String other = temp.create("Other");
  const juce::File folder = temp.library.findSetlist(id)->folder;
  std::map<juce::String, juce::String> ids;
  int bass = 1;
  for (const auto& [name, slot] : {std::pair<const char*, int>{"A", 1}, {"B", 2}, {"C", 3},
                                   {"E", 5}, {"H", 8}})
    ids[name] = save(temp.library, id, name, slot, bass++).id;
  const auto now = [&] { return layout(temp.library.list(id)); };
  const auto idAt = [&](int slot) {
    const auto listing = temp.library.list(id);
    const auto* entry = listing.at(slot);
    return entry != nullptr ? entry->id : juce::String();
  };
  ASSERT_EQ(now(), juce::String("1:A 2:B 3:C 5:E 8:H"));

  // Rename: file and name change, id and slot do not.
  ASSERT_TRUE(temp.library.rename(id, 2, "  Bee  ").wasOk());
  EXPECT_EQ(now(), juce::String("1:A 2:Bee 3:C 5:E 8:H"));
  EXPECT_EQ(idAt(2), ids["B"]);
  EXPECT_TRUE(folder.getChildFile("002 Bee.json").existsAsFile());
  EXPECT_FALSE(folder.getChildFile("002 B.json").exists());
  EXPECT_DOUBLE_EQ(bassOf(temp.library, temp.library.list(id), 2), 2.0);
  // A change of case only.
  ASSERT_TRUE(temp.library.rename(id, 2, "BEE").wasOk());
  EXPECT_EQ(presetFiles(folder)[1], juce::String("002 BEE.json"));
  ASSERT_TRUE(temp.library.rename(id, 2, "B").wasOk());
  EXPECT_TRUE(temp.library.rename(id, 4, "x").failed()) << "an empty slot";
  EXPECT_TRUE(temp.library.rename(id, 2, " ").failed());

  // Move into an empty slot.
  ASSERT_TRUE(temp.library.move(id, 1, 4, false).wasOk());
  EXPECT_EQ(now(), juce::String("2:B 3:C 4:A 5:E 8:H"));
  ASSERT_TRUE(temp.library.move(id, 4, 1, false).wasOk());
  // Swap two presets: the files trade numbers.
  ASSERT_TRUE(temp.library.move(id, 1, 3, false).wasOk());
  EXPECT_EQ(now(), juce::String("1:C 2:B 3:A 5:E 8:H"));
  EXPECT_EQ(presetFiles(folder),
            juce::StringArray({"001 C.json", "002 B.json", "003 A.json", "005 E.json", "008 H.json"}));
  EXPECT_EQ(idAt(1), ids["C"]);
  EXPECT_EQ(idAt(3), ids["A"]);
  ASSERT_TRUE(temp.library.move(id, 1, 3, false).wasOk());
  ASSERT_EQ(now(), juce::String("1:A 2:B 3:C 5:E 8:H"));

  // Insert later: what is in between moves up one, gaps included; the rest
  // stays.
  ASSERT_TRUE(temp.library.move(id, 1, 5, true).wasOk());
  EXPECT_EQ(now(), juce::String("1:B 2:C 4:E 5:A 8:H"));
  EXPECT_EQ(idAt(5), ids["A"]);
  EXPECT_DOUBLE_EQ(bassOf(temp.library, temp.library.list(id), 5), 1.0);
  // Insert earlier: back again.
  ASSERT_TRUE(temp.library.move(id, 5, 1, true).wasOk());
  EXPECT_EQ(now(), juce::String("1:A 2:B 3:C 5:E 8:H"));
  // Onto itself is nothing; from an empty slot, or out of range, fails.
  EXPECT_TRUE(temp.library.move(id, 3, 3, true).wasOk());
  EXPECT_TRUE(temp.library.move(id, 4, 1, true).failed());
  EXPECT_TRUE(temp.library.move(id, 1, 0, true).failed());
  EXPECT_TRUE(temp.library.move(id, 1, 257, false).failed());
  EXPECT_EQ(now(), juce::String("1:A 2:B 3:C 5:E 8:H"));

  // By position: the slots in use stay, the presets trade places in them.
  ASSERT_TRUE(temp.library.moveByDelta(id, 8, -2).wasOk());
  EXPECT_EQ(now(), juce::String("1:A 2:B 3:H 5:C 8:E"));
  ASSERT_TRUE(temp.library.moveByDelta(id, 3, 100).wasOk());
  EXPECT_EQ(now(), juce::String("1:A 2:B 3:C 5:E 8:H"));
  EXPECT_TRUE(temp.library.moveByDelta(id, 8, 1).failed()) << "already last";
  EXPECT_TRUE(temp.library.moveByDelta(id, 1, -1).failed()) << "already first";
  EXPECT_TRUE(temp.library.moveByDelta(id, 4, 1).failed());

  // Duplicate: a new preset with a new id, in the next empty slot.
  SetlistLibrary::Slot copy;
  ASSERT_TRUE(temp.library.duplicate(id, 2, 0, &copy).wasOk());
  EXPECT_EQ(copy.slot, 4);
  EXPECT_EQ(copy.name, juce::String("B copy"));
  EXPECT_NE(copy.id, ids["B"]);
  EXPECT_DOUBLE_EQ(bassOf(temp.library, temp.library.list(id), 4), 2.0);
  // Again: another name, another slot; into a named slot; not onto a preset.
  ASSERT_TRUE(temp.library.duplicate(id, 2, 0, &copy).wasOk());
  EXPECT_EQ(copy.slot, 6);
  EXPECT_EQ(copy.name, juce::String("B copy 2"));
  ASSERT_TRUE(temp.library.duplicate(id, 2, 100, &copy).wasOk());
  EXPECT_EQ(copy.slot, 100);
  EXPECT_TRUE(temp.library.duplicate(id, 2, 1).failed());
  EXPECT_TRUE(temp.library.duplicate(id, 50, 0).failed());
  EXPECT_EQ(now(), juce::String("1:A 2:B 3:C 4:B copy 5:E 6:B copy 2 8:H 100:B copy 3"));

  // Delete: the file goes to the trash, the others keep their slots.
  ASSERT_TRUE(temp.library.remove(id, 3).wasOk());
  EXPECT_EQ(now(), juce::String("1:A 2:B 4:B copy 5:E 6:B copy 2 8:H 100:B copy 3"));
  EXPECT_FALSE(folder.getChildFile("003 C.json").exists());
  EXPECT_TRUE(temp.trashDir.getChildFile("003 C.json").existsAsFile());
  EXPECT_FALSE(temp.library.find(ids["C"]).has_value());
  EXPECT_TRUE(temp.library.remove(id, 3).failed());
  // A second file of that name in the trash does not overwrite the first.
  save(temp.library, id, "C", 3, 42.0);
  ASSERT_TRUE(temp.library.remove(id, 3).wasOk());
  EXPECT_EQ(temp.trashDir.getNumberOfChildFiles(juce::File::findFiles), 2);

  // Copy to another setlist: a new id there, the original untouched.
  SetlistLibrary::Slot copied;
  ASSERT_TRUE(temp.library.copyTo(id, 5, other, 0, &copied).wasOk());
  EXPECT_EQ(copied.slot, 1);
  EXPECT_EQ(copied.name, juce::String("E"));
  EXPECT_NE(copied.id, ids["E"]);
  ASSERT_TRUE(temp.library.copyTo(id, 5, other, 30, &copied).wasOk());
  EXPECT_EQ(layout(temp.library.list(other)), juce::String("1:E 30:E"));
  EXPECT_DOUBLE_EQ(bassOf(temp.library, temp.library.list(other), 30), 4.0);
  EXPECT_TRUE(temp.library.copyTo(id, 5, other, 30).failed()) << "taken";
  EXPECT_TRUE(temp.library.copyTo(id, 3, other, 0).failed()) << "empty source";
  EXPECT_TRUE(temp.library.copyTo(id, 5, "nope", 0).failed());
  EXPECT_EQ(idAt(5), ids["E"]);
  // The id leads to the original, or to the copy where that is preferred.
  EXPECT_EQ(temp.library.find(ids["E"], other)->setlist.id, id);
  EXPECT_EQ(temp.library.find(copied.id)->setlist.id, other);
}

TEST(SetlistLibraryTest, ASetlistHolds256Presets) {
  TempLibrary temp;
  const juce::String id = temp.create("Full");
  const juce::String spare = temp.create("Spare");
  for (int i = 1; i <= SetlistLibrary::kMaxSlots; ++i)
    ASSERT_EQ(save(temp.library, id, "Preset " + juce::String(i)).slot, i);

  SetlistLibrary::Listing listing = temp.library.list(id);
  ASSERT_EQ(static_cast<int>(listing.slots.size()), 256);
  EXPECT_EQ(listing.firstFree(), 0);
  EXPECT_EQ(listing.slots.back().file.getFileName(), juce::String("256 Preset 256.json"));

  // Full is full, for every way in.
  SetlistLibrary::SaveOptions options;
  const juce::Result full = temp.library.save(id, presetNamed("One more"), options);
  EXPECT_TRUE(full.failed());
  EXPECT_TRUE(full.getErrorMessage().contains("full"));
  EXPECT_TRUE(temp.library.duplicate(id, 1, 0).failed());
  save(temp.library, spare, "Guest");
  EXPECT_TRUE(temp.library.copyTo(spare, 1, id, 0).failed());
  // A file dropped in by hand has nowhere to go and is said so, not lost.
  const juce::File folder = temp.library.findSetlist(id)->folder;
  ASSERT_TRUE(folder.getChildFile("001 Preset 1.json").copyFileTo(folder.getChildFile("Extra.json")));
  listing = temp.library.list(id);
  EXPECT_EQ(static_cast<int>(listing.slots.size()), 256);
  ASSERT_EQ(listing.ignored.size(), 1u);
  EXPECT_EQ(listing.ignored[0].file, juce::String("Extra.json"));
  EXPECT_EQ(listing.ignored[0].reason, juce::String("the setlist is full"));
  ASSERT_TRUE(folder.getChildFile("Extra.json").deleteFile());

  // Still editable: an update in place, a move across the whole range.
  EXPECT_EQ(save(temp.library, id, "Preset 200", 0, 8.0).slot, 200);
  ASSERT_TRUE(temp.library.move(id, 256, 1, true).wasOk());
  listing = temp.library.list(id);
  EXPECT_EQ(listing.at(1)->name, juce::String("Preset 256"));
  EXPECT_EQ(listing.at(2)->name, juce::String("Preset 1"));
  EXPECT_EQ(listing.at(256)->name, juce::String("Preset 255"));
  EXPECT_EQ(presetFiles(folder).size(), 256);
  EXPECT_TRUE(folder.getChildFile("201 Preset 200.json").existsAsFile());

  // One out, one in.
  ASSERT_TRUE(temp.library.remove(id, 77).wasOk());
  EXPECT_EQ(temp.library.list(id).firstFree(), 77);
  EXPECT_EQ(save(temp.library, id, "Newcomer").slot, 77);
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

TEST(SetlistLibraryTest, NamesAreValidOnMacWindowsAndLinux) {
  const auto clean = [](const char* name) {
    return SetlistLibrary::sanitiseComponent(utf8(name), "Fallback");
  };
  // Characters one of the three forbids (Windows: \ / : * ? " < > |).
  EXPECT_EQ(clean("AC/DC"), juce::String("AC-DC"));
  EXPECT_EQ(clean("a\\b:c*d?e\"f<g>h|i"), juce::String("a-b-c-d-e-f-g-h-i"));
  EXPECT_EQ(clean("what???"), juce::String("what-"));
  EXPECT_EQ(clean("tab\there\nnewline"), juce::String("tab-here-newline"));
  // Windows drops trailing dots and spaces itself; hidden and lock-file
  // prefixes go.
  EXPECT_EQ(clean("  Lead...  "), juce::String("Lead"));
  EXPECT_EQ(clean(".hidden"), juce::String("hidden"));
  EXPECT_EQ(clean("~$lock"), juce::String("$lock"));
  EXPECT_EQ(clean("Rhythm (v2.1)"), juce::String("Rhythm (v2.1)"));
  // Windows device names, in any case and before any dot.
  EXPECT_EQ(clean("CON"), juce::String("_CON"));
  EXPECT_EQ(clean("nul"), juce::String("_nul"));
  EXPECT_EQ(clean("Com1"), juce::String("_Com1"));
  EXPECT_EQ(clean("LPT9.backup"), juce::String("_LPT9.backup"));
  EXPECT_EQ(clean("CONSOLE"), juce::String("CONSOLE"));
  // Nothing usable left.
  EXPECT_EQ(clean(""), juce::String("Fallback"));
  EXPECT_EQ(clean("   "), juce::String("Fallback"));
  EXPECT_EQ(clean("///"), juce::String("Fallback"));
  EXPECT_EQ(clean("..."), juce::String("Fallback"));
  EXPECT_EQ(clean("Preset"), juce::String("Preset"));
  // Invisible characters make two names that look alike; they go.
  EXPECT_EQ(clean("Le\xe2\x80\x8b" "ad\xef\xbb\xbf"), juce::String("Lead"));
  EXPECT_EQ(clean("\xe2\x80\xae" "evil"), juce::String("evil"));
  // Other scripts and emoji are fine as they are.
  EXPECT_EQ(clean("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"), utf8("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"));
  EXPECT_EQ(clean("Solo \xf0\x9f\x8e\xb8"), utf8("Solo \xf0\x9f\x8e\xb8"));

  // Length: 100 bytes of UTF-8, cut between characters, so that with the
  // slot number and ".json" a file name stays far below every limit (255
  // bytes on Linux and macOS, 255 UTF-16 units on Windows).
  const juce::String longAscii = juce::String::repeatedString("x", 300);
  EXPECT_EQ(SetlistLibrary::sanitiseComponent(longAscii, "f").getNumBytesAsUTF8(), 100u);
  const juce::String longAccents = juce::String::repeatedString(utf8("\xc3\xa9"), 300);
  const juce::String cut = SetlistLibrary::sanitiseComponent(longAccents, "f");
  EXPECT_EQ(cut.getNumBytesAsUTF8(), 100u);
  EXPECT_EQ(cut.length(), 50);
  const juce::String longEmoji = juce::String::repeatedString(utf8("\xf0\x9f\x8e\xb8"), 100);
  EXPECT_EQ(SetlistLibrary::sanitiseComponent(longEmoji, "f").length(), 25);
  EXPECT_LE(SetlistLibrary::slotFileName(256, longAscii).getNumBytesAsUTF8(), 255u);
  // A cut that lands on a space does not leave it trailing.
  const juce::String cutOnSpace = juce::String::repeatedString("y", 99) + " tail";
  EXPECT_EQ(SetlistLibrary::sanitiseComponent(cutOnSpace, "f"), juce::String::repeatedString("y", 99));

  // File names.
  EXPECT_EQ(SetlistLibrary::slotFileName(12, "Glass Cleans"), juce::String("012 Glass Cleans.json"));
  EXPECT_EQ(SetlistLibrary::slotFileName(1, "AC/DC"), juce::String("001 AC-DC.json"));
  EXPECT_EQ(SetlistLibrary::slotFileName(256, "???"), juce::String("256 Preset.json"));
  EXPECT_EQ(SetlistLibrary::slotFileName(3, "CON"), juce::String("003 _CON.json"));
}

TEST(SetlistLibraryTest, UnicodeNamesAreComposedAndComparedFolded) {
  // "é" as one code point and as "e" plus a combining accent (what macOS
  // hands back for a file name on some volumes) are one name.
  const juce::String composed = utf8("Caf\xc3\xa9");
  const juce::String decomposed = utf8("Cafe\xcc\x81");
  ASSERT_NE(composed, decomposed);
  EXPECT_EQ(SetlistLibrary::normaliseName(decomposed), composed);
  EXPECT_EQ(SetlistLibrary::normaliseName(composed), composed);
  EXPECT_EQ(SetlistLibrary::sanitiseComponent(decomposed, "f"), composed);
  EXPECT_EQ(SetlistLibrary::slotFileName(1, decomposed), SetlistLibrary::slotFileName(1, composed));
  // Two marks on one letter (Vietnamese), Greek, Cyrillic, kana, Hangul.
  // (In the order a decomposing file system writes them: the dot below
  // before the circumflex.)
  EXPECT_EQ(SetlistLibrary::normaliseName(utf8("Vie\xcc\xa3\xcc\x82t")), utf8("Vi\xe1\xbb\x87t"));
  EXPECT_EQ(SetlistLibrary::normaliseName(utf8("\xce\xb1\xcc\x81")), utf8("\xce\xac"));
  EXPECT_EQ(SetlistLibrary::normaliseName(utf8("\xd0\xb8\xcc\x86")), utf8("\xd0\xb9"));
  EXPECT_EQ(SetlistLibrary::normaliseName(utf8("\xe3\x81\x8b\xe3\x82\x99")), utf8("\xe3\x81\x8c"));
  EXPECT_EQ(SetlistLibrary::normaliseName(utf8("\xe1\x84\x92\xe1\x85\xa1\xe1\x86\xab")),
            utf8("\xed\x95\x9c"));
  // Nothing to compose: untouched.
  EXPECT_EQ(SetlistLibrary::normaliseName("plain ASCII 123"), juce::String("plain ASCII 123"));
  EXPECT_EQ(SetlistLibrary::normaliseName(utf8("a\xcc\x81\xcc\x81")), utf8("\xc3\xa1\xcc\x81"));

  // Would they be one file on a Mac or on Windows?
  EXPECT_TRUE(SetlistLibrary::namesCollide("Blues", "blues"));
  EXPECT_TRUE(SetlistLibrary::namesCollide("BLUES", "bLuEs"));
  EXPECT_TRUE(SetlistLibrary::namesCollide(composed, decomposed));
  EXPECT_TRUE(SetlistLibrary::namesCollide(utf8("CAF\xc3\x89"), decomposed));
  EXPECT_TRUE(SetlistLibrary::namesCollide(utf8("\xc5\xbd\xc3\xa1r"), utf8("\xc5\xbe\xc3\x81R")));
  EXPECT_TRUE(SetlistLibrary::namesCollide(utf8("\xce\xa3\xce\x97"), utf8("\xcf\x83\xce\xb7")));
  EXPECT_TRUE(SetlistLibrary::namesCollide(utf8("\xd0\x96\xd0\xb0\xd1\x80"), utf8("\xd0\xb6\xd0\x90\xd0\xa0")));
  EXPECT_FALSE(SetlistLibrary::namesCollide("Blues", "Blues 2"));
  EXPECT_FALSE(SetlistLibrary::namesCollide("Cafe", composed));
  EXPECT_FALSE(SetlistLibrary::namesCollide(utf8("\xc3\x97"), utf8("\xc3\xb7")))
      << "the multiplication and division signs are not a case pair";

  // In the library: a name typed decomposed is stored composed, and the
  // other spelling is the same setlist and the same preset.
  TempLibrary temp;
  const juce::String id = temp.create(decomposed);
  EXPECT_EQ(temp.library.findSetlist(id)->name, composed);
  EXPECT_EQ(temp.library.findSetlist(id)->folder.getFileName(), composed);
  EXPECT_TRUE(temp.library.createSetlist(composed).failed());
  EXPECT_TRUE(temp.library.createSetlist(utf8("CAF\xc3\x89")).failed());
  const SetlistLibrary::Slot first = save(temp.library, id, decomposed);
  EXPECT_EQ(first.name, composed);
  EXPECT_EQ(first.file.getFileName(), "001 " + composed + ".json");
  EXPECT_EQ(save(temp.library, id, composed).slot, 1) << "the same name: an update";
  EXPECT_EQ(save(temp.library, id, utf8("caf\xc3\x89")).id, first.id);
}

TEST(SetlistLibraryTest, FoldersThatWouldCollideGetANumber) {
  TempLibrary temp;
  // Different names, the same folder once the forbidden characters go.
  const juce::String slash = temp.create("AC/DC");
  const juce::String colon = temp.create("AC:DC");
  const juce::String dash = temp.create("ac-dc");
  EXPECT_EQ(temp.library.findSetlist(slash)->folder.getFileName(), juce::String("AC-DC"));
  EXPECT_EQ(temp.library.findSetlist(colon)->folder.getFileName(), juce::String("AC-DC 2"));
  // Case alone would be one folder on a Mac or on Windows, also when this
  // file system could tell them apart.
  EXPECT_EQ(temp.library.findSetlist(dash)->folder.getFileName(), juce::String("ac-dc 3"));
  // The real names are in setlist.json.
  EXPECT_EQ(temp.library.findSetlist(slash)->name, juce::String("AC/DC"));
  EXPECT_EQ(temp.library.findSetlist(colon)->name, juce::String("AC:DC"));

  // Reserved, empty-after-cleaning and over-long names.
  EXPECT_EQ(temp.library.findSetlist(temp.create("NUL"))->folder.getFileName(), juce::String("_NUL"));
  EXPECT_EQ(temp.library.findSetlist(temp.create("???"))->folder.getFileName(), juce::String("Setlist"));
  EXPECT_EQ(temp.library.findSetlist(temp.create("***"))->folder.getFileName(), juce::String("Setlist 2"));
  const juce::String longName = juce::String::repeatedString("long ", 60).trim();
  const juce::String longId = temp.create(longName);
  EXPECT_LE(temp.library.findSetlist(longId)->folder.getFileName().getNumBytesAsUTF8(), 100u);
  EXPECT_EQ(temp.library.findSetlist(longId)->name, longName);

  // A stray file with a folder's would-be name is in the way too.
  writeFile(temp.root.getChildFile("Setlists").getChildFile("Notes"), "not a folder");
  EXPECT_EQ(temp.library.findSetlist(temp.create("notes"))->folder.getFileName(), juce::String("notes 2"));

  // Presets: the slot number keeps two cleaned-alike names apart.
  const SetlistLibrary::Slot one = save(temp.library, slash, "Intro/Outro");
  const SetlistLibrary::Slot two = save(temp.library, slash, "Intro:Outro");
  EXPECT_EQ(one.file.getFileName(), juce::String("001 Intro-Outro.json"));
  EXPECT_EQ(two.file.getFileName(), juce::String("002 Intro-Outro.json"));
  EXPECT_EQ(layout(temp.library.list(slash)), juce::String("1:Intro/Outro 2:Intro:Outro"));
}

TEST(SetlistLibraryTest, SlotNumbersAreReadFromFileNames) {
  const auto parse = [](const char* fileName) {
    int slot = 0;
    juce::String stem;
    const bool numbered = SetlistLibrary::parseSlotFileName(fileName, slot, stem);
    return (numbered ? juce::String(slot) : juce::String("-")) + "|" + stem;
  };
  EXPECT_EQ(parse("012 Glass Cleans.json"), juce::String("12|Glass Cleans"));
  EXPECT_EQ(parse("1 A.json"), juce::String("1|A"));
  EXPECT_EQ(parse("256 Last.JSON"), juce::String("256|Last"));
  EXPECT_EQ(parse("007-Bond.json"), juce::String("7|Bond"));
  EXPECT_EQ(parse("003_snake.json"), juce::String("3|snake"));
  EXPECT_EQ(parse("004. Dotted.json"), juce::String("4|Dotted"));
  EXPECT_EQ(parse("005.json"), juce::String("5|"));
  // Not slot numbers: no number, out of range, part of the name.
  EXPECT_EQ(parse("Glass Cleans.json"), juce::String("-|Glass Cleans"));
  EXPECT_EQ(parse("000 Zero.json"), juce::String("-|000 Zero"));
  EXPECT_EQ(parse("257 Too far.json"), juce::String("-|257 Too far"));
  EXPECT_EQ(parse("2024 Tour.json"), juce::String("-|2024 Tour"));
  EXPECT_EQ(parse("12Strings.json"), juce::String("-|12Strings"));
  EXPECT_EQ(parse("5150.json"), juce::String("-|5150"));
}

// ---------------------------------------------------------------------------
// Changes made outside the app
// ---------------------------------------------------------------------------

TEST(SetlistLibraryTest, JunkInAFolderIsToleratedAndLeftAlone) {
  TempLibrary temp;
  const juce::String id = temp.create("Set");
  save(temp.library, id, "Real", 2, 3.0);
  const juce::File folder = temp.library.findSetlist(id)->folder;

  // What ends up in a folder a person and git work in.
  writeFile(folder.getChildFile("README.md"), "# notes");
  writeFile(folder.getChildFile(".DS_Store"), "\x01\x02");
  writeFile(folder.getChildFile(".hidden.json"), "{}");
  writeFile(folder.getChildFile("003 Half.json.1a2b.tmp"), "{ \"format\": \"pedaleira.pre");
  writeFile(folder.getChildFile("004 Broken.json"), "{ this is not json");
  writeFile(folder.getChildFile("005 Empty.json"), "");
  writeFile(folder.getChildFile("006 Array.json"), "[1, 2, 3]");
  writeFile(folder.getChildFile("007 Other.json"), R"({"format":"someone.elses","version":1})");
  writeFile(folder.getChildFile("008 Future.json"), R"({"format":"pedaleira.preset","version":99})");
  writeFile(folder.getChildFile("009 Conflict.json"),
            "<<<<<<< HEAD\n{\"format\":\"pedaleira.preset\"}\n=======\n{}\n>>>>>>> theirs\n");
  writeFile(folder.getChildFile("010 Backup.json.orig"), R"({"format":"pedaleira.preset","version":1})");
  ASSERT_TRUE(folder.getChildFile("Old versions").createDirectory());
  writeFile(folder.getChildFile("Old versions").getChildFile("001 Nested.json"),
            R"({"format":"pedaleira.preset","version":1,"name":"Nested"})");
  const juce::StringArray before = presetFiles(folder);
  const int filesBefore = folder.getNumberOfChildFiles(juce::File::findFilesAndDirectories);

  const SetlistLibrary::Listing listing = temp.library.list(id);
  EXPECT_EQ(layout(listing), juce::String("2:Real"));
  // Every .json that is not a preset is named with the reason; the rest is
  // nobody's business.
  std::map<juce::String, juce::String> ignored;
  for (const auto& entry : listing.ignored)
    ignored[entry.file] = entry.reason;
  EXPECT_EQ(ignored.size(), 6u);
  EXPECT_TRUE(ignored["004 Broken.json"].startsWith("not valid JSON"));
  EXPECT_TRUE(ignored["005 Empty.json"].isNotEmpty());
  EXPECT_EQ(ignored["006 Array.json"], juce::String("not a JSON object"));
  EXPECT_EQ(ignored["007 Other.json"], juce::String("not a pedaleira.preset file"));
  EXPECT_TRUE(ignored["008 Future.json"].contains("newer version"));
  EXPECT_TRUE(ignored["009 Conflict.json"].startsWith("not valid JSON"));

  // Working around them changes none of them.
  EXPECT_EQ(save(temp.library, id, "First").slot, 1);
  EXPECT_EQ(save(temp.library, id, "Third").slot, 3);
  ASSERT_TRUE(temp.library.rename(id, 2, "Really").wasOk());
  ASSERT_TRUE(temp.library.move(id, 1, 3, false).wasOk());
  EXPECT_EQ(temp.library.tidy(id), 0);
  EXPECT_EQ(layout(temp.library.list(id)), juce::String("1:Third 2:Really 3:First"));
  // A slot whose file name a junk file has: refused, the junk stays.
  const juce::Result blocked = temp.library.save(id, presetNamed("Broken"), {4, false});
  EXPECT_TRUE(blocked.failed());
  EXPECT_TRUE(blocked.getErrorMessage().contains("in the way")) << blocked.getErrorMessage();
  EXPECT_EQ(folder.getChildFile("004 Broken.json").loadFileAsString(), juce::String("{ this is not json"));
  EXPECT_TRUE(temp.library.move(id, 1, 6, false).wasOk()) << "another name: no clash";
  ASSERT_TRUE(temp.library.move(id, 6, 1, false).wasOk());
  for (const auto& name : before)
    if (name != "002 Real.json")
      EXPECT_TRUE(folder.getChildFile(name).existsAsFile()) << name;
  EXPECT_EQ(folder.getChildFile("README.md").loadFileAsString(), juce::String("# notes"));
  EXPECT_EQ(folder.getNumberOfChildFiles(juce::File::findFilesAndDirectories), filesBefore + 2);
  EXPECT_EQ(temp.library.list(id).ignored.size(), 6u);

  // Junk beside the setlists and at the root.
  const juce::File setlists = temp.root.getChildFile("Setlists");
  writeFile(setlists.getChildFile("stray.json"), "{}");
  writeFile(setlists.getChildFile(".gitkeep"), "");
  ASSERT_TRUE(setlists.getChildFile(".git").createDirectory());
  writeFile(temp.root.getChildFile(".gitignore"), "Trash/\n");
  writeFile(temp.root.getChildFile("README.md"), "my tones");
  EXPECT_EQ(temp.library.setlists().size(), 1u);

  // A library.json somebody broke is a library without favourites, not a
  // dead library; the next thing worth remembering puts a copy of it aside
  // and starts it again (see ABrokenLibraryJsonIsSetAsideBeforeItIsWrittenOver).
  writeFile(temp.root.getChildFile("library.json"), "<<<<<<< HEAD\n{");
  EXPECT_TRUE(temp.library.favourites().isEmpty());
  EXPECT_EQ(temp.library.setlists().size(), 1u);
  EXPECT_TRUE(temp.library.setFavourite("user:x", true));
  EXPECT_EQ(temp.library.favourites(), juce::StringArray({"user:x"}));
}

TEST(SetlistLibraryTest, FilesAddedRemovedRenamedAndEditedOutsideAreSeen) {
  TempLibrary temp;
  const juce::String id = temp.create("Set");
  const SetlistLibrary::Slot a = save(temp.library, id, "A", 1, 1.0);
  const SetlistLibrary::Slot b = save(temp.library, id, "B", 2, 2.0);
  const SetlistLibrary::Slot c = save(temp.library, id, "C", 3, 3.0);
  const juce::File folder = temp.library.findSetlist(id)->folder;
  ASSERT_EQ(layout(temp.library.list(id)), juce::String("1:A 2:B 3:C"));

  // Removed (git rm, the Finder).
  ASSERT_TRUE(folder.getChildFile("002 B.json").deleteFile());
  EXPECT_EQ(layout(temp.library.list(id)), juce::String("1:A 3:C"));
  EXPECT_FALSE(temp.library.find(b.id).has_value());

  // Renumbered by hand: the file name is the truth for the slot.
  ASSERT_TRUE(folder.getChildFile("003 C.json").moveFileTo(folder.getChildFile("010 C.json")));
  SetlistLibrary::Listing listing = temp.library.list(id);
  EXPECT_EQ(layout(listing), juce::String("1:A 10:C"));
  EXPECT_EQ(listing.at(10)->id, c.id) << "the id is inside the file and survives";
  EXPECT_TRUE(listing.at(10)->tidy);

  // Renamed by hand: the name inside the file is the truth for the name, and
  // the file is merely untidy.
  ASSERT_TRUE(folder.getChildFile("010 C.json").moveFileTo(folder.getChildFile("010 See.json")));
  listing = temp.library.list(id);
  EXPECT_EQ(layout(listing), juce::String("1:A 10:C"));
  EXPECT_FALSE(listing.at(10)->tidy);

  // Edited in a text editor: name and a value. (Another size, so the change
  // is seen whatever the file system's time resolution.)
  juce::String text = folder.getChildFile("001 A.json").loadFileAsString();
  ASSERT_TRUE(text.contains("\"name\": \"A\""));
  ASSERT_TRUE(text.contains("\"toneBass\": 1.0"));
  text = text.replace("\"name\": \"A\"", "\"name\": \"Alpha edited\"")
             .replace("\"toneBass\": 1.0", "\"toneBass\": 8.25");
  writeFile(folder.getChildFile("001 A.json"), text);
  listing = temp.library.list(id);
  EXPECT_EQ(layout(listing), juce::String("1:Alpha edited 10:C"));
  EXPECT_EQ(listing.at(1)->id, a.id);
  EXPECT_DOUBLE_EQ(bassOf(temp.library, listing, 1), 8.25);

  // Added without a number (a preset a friend sent): the first free slot.
  // With a number that is taken (two branches merged): the next free one.
  // With no id inside (written by hand): listed under an id from its name.
  ASSERT_TRUE(folder.getChildFile("001 A.json").copyFileTo(folder.getChildFile("From a friend.json")));
  writeFile(folder.getChildFile("010 Clash.json"),
            R"({"format":"pedaleira.preset","version":1,"id":"clash-id","name":"Clash","chain":{}})");
  writeFile(folder.getChildFile("By hand.json"),
            R"({"format":"pedaleira.preset","version":1,"chain":{"stereoEnabled":false}})");
  listing = temp.library.list(id);
  // "010 Clash" sorts before "010 See", so it keeps slot 10 and C moves on.
  EXPECT_EQ(layout(listing), juce::String("1:Alpha edited 2:By hand 3:Alpha edited 10:Clash 11:C"));
  EXPECT_EQ(listing.at(11)->id, c.id);
  // The copy has the id of the file it was copied from: listed apart.
  EXPECT_EQ(listing.at(1)->id, a.id);
  EXPECT_EQ(listing.at(3)->id, a.id + "~2");
  EXPECT_TRUE(listing.at(3)->derivedId);
  EXPECT_EQ(listing.at(2)->id, juce::String("user:file:Set/By hand"));
  EXPECT_TRUE(listing.at(2)->derivedId);
  std::set<juce::String> ids;
  for (const auto& slot : listing.slots)
    EXPECT_TRUE(ids.insert(slot.id).second) << "ids are unique within a listing: " << slot.id;
  // All of it loads, and none of it was renamed by looking.
  for (const auto& slot : listing.slots)
    EXPECT_TRUE(temp.library.load({listing.setlist, slot}).preset.isValid()) << slot.name;
  EXPECT_EQ(presetFiles(folder),
            juce::StringArray({"001 A.json", "010 Clash.json", "010 See.json", "By hand.json",
                               "From a friend.json"}));

  // tidy() makes the file names say what the listing says, and nothing else
  // changes.
  EXPECT_EQ(temp.library.tidy(id), 4);
  EXPECT_EQ(presetFiles(folder),
            juce::StringArray({"001 Alpha edited.json", "002 By hand.json", "003 Alpha edited.json",
                               "010 Clash.json", "011 C.json"}));
  listing = temp.library.list(id);
  EXPECT_EQ(layout(listing), juce::String("1:Alpha edited 2:By hand 3:Alpha edited 10:Clash 11:C"));
  for (const auto& slot : listing.slots)
    EXPECT_TRUE(slot.tidy) << slot.name;
  EXPECT_EQ(temp.library.tidy(id), 0);

  // An edit of a slot whose file had no id of its own gives it one; from
  // then on it is a preset like any other.
  ASSERT_TRUE(temp.library.rename(id, 2, "Hand made").wasOk());
  ASSERT_TRUE(temp.library.rename(id, 3, "Friend's").wasOk());
  listing = temp.library.list(id);
  EXPECT_FALSE(listing.at(2)->derivedId);
  EXPECT_FALSE(listing.at(3)->derivedId);
  EXPECT_NE(listing.at(3)->id, a.id);
  EXPECT_EQ(listing.at(1)->id, a.id);
  EXPECT_TRUE(folder.getChildFile("002 Hand made.json").existsAsFile());

  // Without a tidy first, an edit still never writes over a file that was
  // displaced: saving into slot 12 while an unnumbered file resolves there.
  ASSERT_TRUE(folder.getChildFile("011 C.json").copyFileTo(folder.getChildFile("012 Twin.json")));
  writeFile(folder.getChildFile("Loose.json"),
            R"({"format":"pedaleira.preset","version":1,"name":"Loose","id":"loose-id","chain":{}})");
  ASSERT_EQ(temp.library.list(id).at(4)->name, juce::String("Loose"));
  EXPECT_EQ(save(temp.library, id, "New in four", 4).id, juce::String("user:loose-id"))
      << "slot 4 is Loose's, so this replaces it like any occupied slot";
  EXPECT_TRUE(folder.getChildFile("004 New in four.json").existsAsFile());
  EXPECT_FALSE(folder.getChildFile("Loose.json").exists());
  EXPECT_TRUE(folder.getChildFile("012 C.json").existsAsFile()) << "the twin was tidied, not lost";
}

TEST(SetlistLibraryTest, FoldersMadeCopiedAndRemovedOutsideAreSetlists) {
  TempLibrary temp;
  const juce::String id = temp.create("Original");
  const SetlistLibrary::Slot saved = save(temp.library, id, "Tone", 1, 6.0);
  const juce::File setlists = temp.root.getChildFile("Setlists");

  // A folder made by hand, with a preset copied into it: a setlist, known
  // by its folder until it is written to.
  const juce::File handMade = setlists.getChildFile("Hand Made");
  ASSERT_TRUE(handMade.createDirectory());
  ASSERT_TRUE(saved.file.copyFileTo(handMade.getChildFile("005 Tone.json")));
  // An empty folder is an empty setlist.
  ASSERT_TRUE(setlists.getChildFile("Empty").createDirectory());
  // A whole setlist duplicated in a file browser: same id inside.
  const juce::File copyFolder = setlists.getChildFile("Original copy");
  ASSERT_TRUE(temp.library.findSetlist(id)->folder.copyDirectoryTo(copyFolder));

  auto all = temp.library.setlists();
  ASSERT_EQ(all.size(), 4u);
  std::map<juce::String, juce::String> idByFolder;
  for (const auto& setlist : all)
    idByFolder[setlist.folder.getFileName()] = setlist.id;
  EXPECT_EQ(idByFolder["Original"], id) << "the first folder by name keeps the id";
  EXPECT_EQ(idByFolder["Original copy"], juce::String("folder:Original copy"));
  EXPECT_EQ(idByFolder["Hand Made"], juce::String("folder:Hand Made"));
  EXPECT_EQ(idByFolder["Empty"], juce::String("folder:Empty"));
  EXPECT_EQ(temp.library.findSetlist("folder:Hand Made")->name, juce::String("Hand Made"));
  EXPECT_EQ(layout(temp.library.list("folder:Hand Made")), juce::String("5:Tone"));
  EXPECT_TRUE(temp.library.list("folder:Empty").slots.empty());
  // Nothing was written by looking.
  EXPECT_FALSE(handMade.getChildFile("setlist.json").exists());
  EXPECT_EQ(juce::JSON::parse(copyFolder.getChildFile("setlist.json").loadFileAsString())["id"].toString(), id);

  // The same preset id in three setlists: found where the caller prefers,
  // else in the first.
  EXPECT_EQ(temp.library.find(saved.id)->setlist.id, id);
  EXPECT_EQ(temp.library.find(saved.id, "folder:Hand Made")->slot.slot, 5);

  // Writing to a hand-made setlist gives it an id of its own, and what the
  // library remembered about it under its folder name follows.
  temp.library.setCurrent({"folder:Hand Made", 5});
  ASSERT_TRUE(temp.library.reorderSetlist("folder:Hand Made", 0).wasOk());
  const SetlistLibrary::Slot added = save(temp.library, "folder:Hand Made", "Second");
  EXPECT_EQ(added.slot, 1);
  ASSERT_TRUE(handMade.getChildFile("setlist.json").existsAsFile());
  const juce::String mintedId =
      juce::JSON::parse(handMade.getChildFile("setlist.json").loadFileAsString())["id"].toString();
  EXPECT_FALSE(mintedId.startsWith("folder:"));
  EXPECT_FALSE(temp.library.findSetlist("folder:Hand Made").has_value());
  ASSERT_TRUE(temp.library.findSetlist(mintedId).has_value());
  EXPECT_EQ(temp.library.current().setlistId, mintedId);
  EXPECT_EQ(temp.library.setlists()[0].id, mintedId);
  EXPECT_EQ(layout(temp.library.list(mintedId)), juce::String("1:Second 5:Tone"));
  // The copied folder too: its own id, the original keeps its.
  ASSERT_TRUE(temp.library.renameSetlist("folder:Original copy", "Second Night").wasOk());
  all = temp.library.setlists();
  std::set<juce::String> ids;
  for (const auto& setlist : all)
    EXPECT_TRUE(ids.insert(setlist.id).second);
  EXPECT_TRUE(ids.count(id) == 1);
  EXPECT_TRUE(setlists.getChildFile("Second Night").isDirectory());

  // setlist.json deleted: the folder is still a setlist with its presets.
  ASSERT_TRUE(temp.library.findSetlist(id)->folder.getChildFile("setlist.json").deleteFile());
  EXPECT_FALSE(temp.library.findSetlist(id).has_value());
  EXPECT_EQ(layout(temp.library.list("folder:Original")), juce::String("1:Tone"));
  // A setlist.json that is not one: the same.
  writeFile(handMade.getChildFile("setlist.json"), "{ broken");
  EXPECT_EQ(layout(temp.library.list("folder:Hand Made")), juce::String("1:Second 5:Tone"));

  // A folder removed outside is simply gone, and so is a position in it.
  temp.library.setCurrent({"folder:Empty", 0});
  ASSERT_TRUE(setlists.getChildFile("Empty").deleteRecursively());
  EXPECT_FALSE(temp.library.findSetlist("folder:Empty").has_value());
  EXPECT_FALSE(temp.library.list("folder:Empty").found);
  EXPECT_EQ(temp.library.setlists().size(), 3u);
}

TEST(SetlistLibraryTest, ACacheNeverHidesAChangeTheLibraryMadeItself) {
  // Files of one size written within the same second: on a file system with
  // one second times nothing but the library's own bookkeeping tells them
  // apart.
  TempLibrary temp;
  const juce::String id = temp.create("Set");
  const SetlistLibrary::Slot a = save(temp.library, id, "Aaa", 1, 1.0);
  const SetlistLibrary::Slot b = save(temp.library, id, "Bbb", 2, 2.0);
  for (int i = 0; i < 6; ++i) {
    ASSERT_TRUE(temp.library.rename(id, 1, i % 2 == 0 ? "Ccc" : "Aaa").wasOk());
    ASSERT_EQ(temp.library.list(id).at(1)->name, juce::String(i % 2 == 0 ? "Ccc" : "Aaa"));
    ASSERT_TRUE(temp.library.move(id, 1, 2, false).wasOk());
    SetlistLibrary::Listing listing = temp.library.list(id);
    ASSERT_EQ(listing.at(1)->id, b.id);
    ASSERT_EQ(listing.at(2)->id, a.id);
    ASSERT_TRUE(temp.library.move(id, 1, 2, false).wasOk());
    listing = temp.library.list(id);
    ASSERT_EQ(listing.at(1)->id, a.id);
    ASSERT_DOUBLE_EQ(bassOf(temp.library, listing, 2), 2.0);
  }
  // forgetCache is the answer for edits from outside that keep time and size.
  temp.library.forgetCache();
  EXPECT_EQ(layout(temp.library.list(id)), juce::String("1:Aaa 2:Bbb"));
}

// ---------------------------------------------------------------------------
// library.json
// ---------------------------------------------------------------------------

TEST(SetlistLibraryTest, FavouritesRecentsAndThePositionAreOneSmallFile) {
  TempLibrary temp;
  const juce::String id = temp.create("Set");

  // Favourites: a set, kept sorted so two machines' edits merge by line.
  EXPECT_TRUE(temp.library.setFavourite("user:m", true));
  EXPECT_TRUE(temp.library.setFavourite("user:b", true));
  EXPECT_TRUE(temp.library.setFavourite("factory:z", true));
  EXPECT_TRUE(temp.library.setFavourite("user:b", true));  // already one
  EXPECT_EQ(temp.library.favourites(), juce::StringArray({"factory:z", "user:b", "user:m"}));
  EXPECT_TRUE(temp.library.isFavourite("user:m"));
  EXPECT_TRUE(temp.library.setFavourite("user:m", false));
  EXPECT_TRUE(temp.library.setFavourite("user:never", false));
  EXPECT_FALSE(temp.library.isFavourite("user:m"));
  EXPECT_EQ(temp.library.favourites(), juce::StringArray({"factory:z", "user:b"}));

  // Recents: most recent first, each once, capped.
  temp.library.noteRecent("user:1");
  temp.library.noteRecent("user:2");
  temp.library.noteRecent("user:3");
  EXPECT_EQ(temp.library.recents(), juce::StringArray({"user:3", "user:2", "user:1"}));
  temp.library.noteRecent("user:1");
  EXPECT_EQ(temp.library.recents(), juce::StringArray({"user:1", "user:3", "user:2"}));
  temp.library.noteRecent("");
  EXPECT_EQ(temp.library.recents().size(), 3);
  for (int i = 0; i < 100; ++i)
    temp.library.noteLoaded("user:n" + juce::String(i), std::nullopt);
  juce::StringArray recents = temp.library.recents();
  ASSERT_EQ(recents.size(), SetlistLibrary::kMaxRecents);
  EXPECT_EQ(recents[0], juce::String("user:n99"));
  EXPECT_EQ(recents[SetlistLibrary::kMaxRecents - 1],
            "user:n" + juce::String(100 - SetlistLibrary::kMaxRecents));

  // The position, alone and together with a load.
  temp.library.setCurrent({id, 12});
  EXPECT_EQ(temp.library.current().setlistId, id);
  EXPECT_EQ(temp.library.current().slot, 12);
  temp.library.noteLoaded("user:loaded", SetlistLibrary::Position{id, 3});
  EXPECT_EQ(temp.library.current().slot, 3);
  EXPECT_EQ(temp.library.recents()[0], juce::String("user:loaded"));

  // All of it is in library.json, readable, and nothing else is.
  const juce::File file = temp.root.getChildFile("library.json");
  const juce::String text = file.loadFileAsString();
  const juce::var state = juce::JSON::parse(text);
  EXPECT_EQ(state["format"].toString(), juce::String("pedaleira.library"));
  EXPECT_EQ(state["current"]["setlist"].toString(), id);
  EXPECT_EQ(static_cast<int>(state["current"]["slot"]), 3);
  EXPECT_EQ(state["favourites"].size(), 2);
  EXPECT_EQ(state["recents"].size(), SetlistLibrary::kMaxRecents);
  EXPECT_TRUE(text.contains("  \"favourites\": [\n    \"factory:z\",\n    \"user:b\"\n  ]")) << text;
  EXPECT_LT(file.getSize(), 4096);

  // Another instance (the next launch) starts from it.
  {
    SetlistLibrary next(temp.root);
    EXPECT_EQ(next.current().setlistId, id);
    EXPECT_EQ(next.current().slot, 3);
    EXPECT_EQ(next.favourites(), juce::StringArray({"factory:z", "user:b"}));
    EXPECT_EQ(next.recents()[0], juce::String("user:loaded"));

    // Two instances share favourites and recents, but each keeps the
    // position it chose: one changing setlist must not move the other's
    // program numbers.
    next.setCurrent({id, 40});
    EXPECT_EQ(temp.library.current().slot, 3);
    EXPECT_EQ(next.current().slot, 40);
    EXPECT_TRUE(next.setFavourite("user:shared", true));
    EXPECT_TRUE(temp.library.isFavourite("user:shared"));
  }

  // Saying the same thing again does not touch the file (no churn in git).
  temp.library.setCurrent({id, 3});
  const juce::Time written = file.getLastModificationTime();
  juce::Thread::sleep(1100);
  temp.library.setFavourite("user:shared", true);
  temp.library.noteLoaded(temp.library.recents()[0], temp.library.current());
  EXPECT_EQ(file.getLastModificationTime(), written);

  // Keys a later version adds survive this one's writes.
  juce::var edited = juce::JSON::parse(file.loadFileAsString());
  edited.getDynamicObject()->setProperty("somethingNew", "kept");
  writeFile(file, juce::JSON::toString(edited));
  EXPECT_TRUE(temp.library.setFavourite("user:after", true));
  EXPECT_EQ(juce::JSON::parse(file.loadFileAsString())["somethingNew"].toString(), juce::String("kept"));
  EXPECT_TRUE(temp.library.isFavourite("user:after"));
}

namespace {

// ---------------------------------------------------------------------------
// Writes that cannot cost a file, and files that cannot crash a read
// ---------------------------------------------------------------------------

// Sets a file fault for a scope (presetjson::setFileFaultForTesting).
struct ScopedFileFault {
  explicit ScopedFileFault(presetjson::FileFault fault)
      : previous(presetjson::setFileFaultForTesting(fault)) {}
  ~ScopedFileFault() { presetjson::setFileFaultForTesting(previous); }
  presetjson::FileFault previous;
};

juce::MemoryBlock bytesOf(const juce::File& file) {
  juce::MemoryBlock bytes;
  EXPECT_TRUE(file.loadFileAsData(bytes)) << file.getFullPathName();
  return bytes;
}

int tempFilesIn(const juce::File& folder) {
  return folder.findChildFiles(juce::File::findFiles, true, "*.tmp").size();
}

TEST(SetlistLibraryFilesTest, AWriteThatFailsLeavesTheOldFileWhole) {
  TempLibrary temp;
  const juce::String id = temp.create("Gig");
  const juce::File folder = temp.root.getChildFile("Setlists").getChildFile("Gig");
  const juce::MemoryBlock model("model bytes that go to the asset store", 38);
  SetlistLibrary::SaveOptions options;
  options.slot = 1;
  ASSERT_TRUE(temp.library.save(id, presetNamed("Crunch", 2.0, &model), options).wasOk());
  ASSERT_TRUE(temp.library.setFavourite("user:kept", true));
  const juce::File presetFile = folder.getChildFile("001 Crunch.json");
  const juce::File stateFile = temp.root.getChildFile("library.json");
  const juce::MemoryBlock presetBefore = bytesOf(presetFile), stateBefore = bytesOf(stateFile);
  const juce::MemoryBlock metaBefore = bytesOf(folder.getChildFile("setlist.json"));

  // Both ways a write can go wrong: the bytes do not all reach the disk, or
  // the finished file cannot take the old one's place. Either way the old
  // file is there, whole, the caller is told, and no scratch file is left.
  for (const auto fault : {presetjson::FileFault::write, presetjson::FileFault::replace}) {
    SCOPED_TRACE(fault == presetjson::FileFault::write ? "write" : "replace");
    const ScopedFileFault scoped(fault);

    // Saving over a preset (a new model too, so the asset store is asked).
    const juce::MemoryBlock other("another model altogether", 24);
    const juce::Result saved = temp.library.save(id, presetNamed("Crunch", 9.0, &other), options);
    EXPECT_TRUE(saved.failed());
    EXPECT_TRUE(bytesOf(presetFile) == presetBefore);
    // Renaming, duplicating and copying write preset files as well.
    EXPECT_TRUE(temp.library.duplicate(id, 1, 2).failed());
    // library.json.
    EXPECT_FALSE(temp.library.setFavourite("user:new", true));
    EXPECT_TRUE(bytesOf(stateFile) == stateBefore);
    // setlist.json.
    EXPECT_TRUE(temp.library.renameSetlist(id, "Other").failed());
    EXPECT_TRUE(bytesOf(folder.getChildFile("setlist.json")) == metaBefore);
    // And the helper itself, over a file and beside none.
    const juce::File plain = temp.dir.getChildFile("plain.txt");
    writeFile(plain, "old");
    const juce::Result written = presetjson::writeFileAtomically(plain, juce::String("new"));
    EXPECT_TRUE(written.failed());
    EXPECT_TRUE(written.getErrorMessage().contains(plain.getFullPathName()));
    EXPECT_EQ(plain.loadFileAsString(), juce::String("old"));
    EXPECT_TRUE(presetjson::writeFileAtomically(temp.dir.getChildFile("none.txt"), juce::String("x"))
                    .failed());
    EXPECT_FALSE(temp.dir.getChildFile("none.txt").exists());

    EXPECT_EQ(tempFilesIn(temp.dir), 0);
    temp.library.forgetCache();
    const SetlistLibrary::Listing listing = temp.library.list(id);
    EXPECT_EQ(layout(listing), juce::String("1:Crunch"));
    EXPECT_EQ(bassOf(temp.library, listing, 1), 2.0);
    EXPECT_TRUE(temp.library.isFavourite("user:kept"));
    EXPECT_FALSE(temp.library.isFavourite("user:new"));
  }

  // With the fault gone the same calls go through, replacing in place.
  const juce::File plain = temp.dir.getChildFile("plain.txt");
  EXPECT_TRUE(presetjson::writeFileAtomically(plain, juce::String("new")).wasOk());
  EXPECT_EQ(plain.loadFileAsString(), juce::String("new"));
  EXPECT_TRUE(temp.library.save(id, presetNamed("Crunch", 9.0), options).wasOk());
  EXPECT_EQ(bassOf(temp.library, temp.library.list(id), 1), 9.0);
  EXPECT_EQ(tempFilesIn(temp.dir), 0);
}

TEST(SetlistLibraryFilesTest, AnAssetThatCannotBeWrittenIsNotStored) {
  TempLibrary temp;
  const presetjson::AssetStore store(temp.dir.getChildFile("Assets"));
  const juce::String bytes = "the bytes of a model";
  presetjson::AssetRef ref;
  for (const auto fault : {presetjson::FileFault::write, presetjson::FileFault::replace}) {
    const ScopedFileFault scoped(fault);
    EXPECT_FALSE(store.put(bytes.toRawUTF8(), bytes.getNumBytesAsUTF8(), "nam", ref));
    EXPECT_FALSE(store.fileFor(ref.hash, ref.ext).exists());
    EXPECT_EQ(tempFilesIn(temp.dir), 0);
  }
  EXPECT_TRUE(store.put(bytes.toRawUTF8(), bytes.getNumBytesAsUTF8(), "nam", ref));
  EXPECT_EQ(store.fileFor(ref.hash, ref.ext).loadFileAsString(), bytes);
}

TEST(SetlistLibraryFilesTest, AnAssetFileOfTheRightSizeAndTheWrongBytesIsWrittenAgain) {
  TempLibrary temp;
  const presetjson::AssetStore store(temp.dir.getChildFile("Assets"));
  const juce::String bytes = "the bytes of a model, as they should be";
  presetjson::AssetRef ref;
  ASSERT_TRUE(store.put(bytes.toRawUTF8(), bytes.getNumBytesAsUTF8(), "nam", ref));
  const juce::File file = store.fileFor(ref.hash, ref.ext);

  // Damaged behind the store's back, the size unchanged (a bad copy, a
  // failing disk). The store that wrote it finds out once the file's time
  // has moved, and so does one that has never seen the file.
  const juce::String damaged = bytes.replaceCharacter('m', 'X');
  ASSERT_EQ(damaged.length(), bytes.length());
  ASSERT_NE(damaged, bytes);
  const presetjson::AssetStore fresh(temp.dir.getChildFile("Assets"));
  int round = 0;
  for (const presetjson::AssetStore* used : {&store, &fresh}) {
    writeFile(file, damaged);
    file.setLastModificationTime(juce::Time::getCurrentTime() +
                                 juce::RelativeTime::seconds(10.0 * ++round));
    presetjson::AssetRef again;
    EXPECT_TRUE(used->put(bytes.toRawUTF8(), bytes.getNumBytesAsUTF8(), "nam", again));
    EXPECT_EQ(again.hash, ref.hash);
    EXPECT_EQ(file.loadFileAsString(), bytes) << "a damaged asset was taken for the stored one";
    juce::MemoryBlock read;
    EXPECT_EQ(used->get(again, read), presetjson::AssetStore::Status::ok);
  }
  // A file that is what it should be is not written again.
  const juce::Time written = file.getLastModificationTime();
  presetjson::AssetRef third;
  EXPECT_TRUE(store.put(bytes.toRawUTF8(), bytes.getNumBytesAsUTF8(), "nam", third));
  EXPECT_EQ(file.getLastModificationTime(), written);
  EXPECT_EQ(tempFilesIn(temp.dir), 0);
}

TEST(SetlistLibraryFilesTest, HostileJsonIsIgnoredWithAReason) {
  // A hundred thousand open brackets: more levels than any parser that
  // recurses per level has stack for.
  const juce::String deep = juce::String::repeatedString("[", 100000);
  const juce::String deepObjects = juce::String::repeatedString("{\"a\":", 100000);
  const juce::String preset = "{\"format\":\"pedaleira.preset\",\"version\":1,\"name\":\"Deep\","
                              "\"chain\":{},\"params\":{},\"note\":";

  // The parse itself.
  juce::var parsed;
  EXPECT_TRUE(presetjson::parseJsonGuarded(deep, parsed).failed());
  EXPECT_TRUE(presetjson::parseJsonGuarded(deepObjects, parsed).failed());
  EXPECT_TRUE(presetjson::parseJsonGuarded(preset + deep + "}", parsed).failed());
  // Brackets in a string are text, and 64 real levels are fine.
  EXPECT_TRUE(presetjson::parseJsonGuarded(
                  "{\"a\":" + juce::JSON::toString(juce::var(deep + "\\\"" + deep)) + "}", parsed)
                  .wasOk());
  EXPECT_EQ(parsed["a"].toString().length(), 200002);
  const juce::String level64 = juce::String::repeatedString("[", presetjson::kMaxJsonDepth) +
                               juce::String::repeatedString("]", presetjson::kMaxJsonDepth);
  EXPECT_TRUE(presetjson::parseJsonGuarded(level64, parsed).wasOk());
  EXPECT_TRUE(presetjson::parseJsonGuarded("[" + level64 + "]", parsed).failed());

  // As a preset's text.
  EXPECT_FALSE(presetjson::readMeta(preset + deep + "}").valid);
  EXPECT_TRUE(presetjson::readMeta(preset + deep + "}").problem.contains("nested deeper"));
  const presetjson::ReadResult read = presetjson::fromJson(preset + deepObjects + "}", {});
  EXPECT_FALSE(read.preset.isValid());
  EXPECT_TRUE(read.error.contains("nested deeper")) << read.error;

  // In a library: a good preset, one nested too deep, one too large, and a
  // setlist.json and a library.json that are bombs themselves.
  TempLibrary temp;
  const juce::String id = temp.create("Gig");
  const juce::File folder = temp.root.getChildFile("Setlists").getChildFile("Gig");
  save(temp.library, id, "Good", 1);
  writeFile(folder.getChildFile("002 Deep.json"), preset + deep + "}");
  {
    // One byte over the limit, and a perfectly good preset but for that.
    juce::FileOutputStream out(folder.getChildFile("003 Huge.json"));
    ASSERT_TRUE(out.openedOk());
    const juce::String head = preset + "\"";
    out.write(head.toRawUTF8(), head.getNumBytesAsUTF8());
    juce::MemoryBlock spaces(1024 * 1024);
    spaces.fillWith(' ');
    juce::int64 left = presetjson::kMaxPresetFileBytes + 1 -
                       static_cast<juce::int64>(head.getNumBytesAsUTF8()) - 2;
    while (left > 0) {
      const auto chunk = static_cast<size_t>(juce::jmin<juce::int64>(left, static_cast<juce::int64>(spaces.getSize())));
      out.write(spaces.getData(), chunk);
      left -= static_cast<juce::int64>(chunk);
    }
    out.write("\"}", 2);
  }
  ASSERT_EQ(folder.getChildFile("003 Huge.json").getSize(), presetjson::kMaxPresetFileBytes + 1);
  temp.library.forgetCache();

  SetlistLibrary::Listing listing = temp.library.list(id);
  EXPECT_EQ(layout(listing), juce::String("1:Good"));
  ASSERT_EQ(listing.ignored.size(), 2u);
  std::map<juce::String, juce::String> reasons;
  for (const auto& entry : listing.ignored)
    reasons[entry.file] = entry.reason;
  EXPECT_TRUE(reasons["002 Deep.json"].contains("nested deeper than 64 levels")) << reasons["002 Deep.json"];
  EXPECT_TRUE(reasons["003 Huge.json"].contains("larger than")) << reasons["003 Huge.json"];
  // Loading the file directly is refused the same way.
  SetlistLibrary::Located located;
  located.setlist = listing.setlist;
  located.slot.file = folder.getChildFile("002 Deep.json");
  EXPECT_FALSE(temp.library.load(located).preset.isValid());
  located.slot.file = folder.getChildFile("003 Huge.json");
  EXPECT_TRUE(temp.library.load(located).error.contains("larger than"));
  folder.getChildFile("003 Huge.json").deleteFile();

  // setlist.json: the setlist is one made by hand, under its folder's name.
  writeFile(folder.getChildFile("setlist.json"), deepObjects);
  temp.library.forgetCache();
  ASSERT_EQ(temp.library.setlists().size(), 1u);
  EXPECT_EQ(temp.library.setlists()[0].name, juce::String("Gig"));
  EXPECT_EQ(layout(temp.library.list(temp.library.setlists()[0].id)), juce::String("1:Good"));
  writeFile(folder.getChildFile("setlist.json"),
            "{\"format\":\"pedaleira.setlist\",\"version\":1,\"id\":\"x\",\"name\":\"" +
                juce::String::repeatedString("n", static_cast<int>(presetjson::kMaxMetaFileBytes)) +
                "\"}");
  EXPECT_EQ(temp.library.setlists()[0].name, juce::String("Gig"));

  // library.json: a library without favourites, and no crash.
  writeFile(temp.root.getChildFile("library.json"), deep);
  EXPECT_TRUE(temp.library.favourites().isEmpty());
  EXPECT_EQ(temp.library.setlists().size(), 1u);
}

TEST(SetlistLibraryFilesTest, ABrokenLibraryJsonIsSetAsideBeforeItIsWrittenOver) {
  TempLibrary temp;
  const juce::String id = temp.create("Gig");
  ASSERT_TRUE(temp.library.setFavourite("user:precious", true));
  temp.library.noteRecent("user:played");
  const juce::File file = temp.root.getChildFile("library.json");
  const auto brokenCopies = [&temp] {
    return temp.root.findChildFiles(juce::File::findFiles, false, "library.json.broken-*");
  };

  // What a merge leaves behind: the favourites are in there, but it is no
  // JSON any more.
  const juce::String conflicted = "<<<<<<< HEAD\n" + file.loadFileAsString() + "=======\n{}\n>>>>>>> theirs\n";
  writeFile(file, conflicted);

  // Read: the defaults, and the file is left exactly as it is.
  SetlistLibrary reader(temp.root);
  EXPECT_TRUE(reader.favourites().isEmpty());
  EXPECT_TRUE(reader.recents().isEmpty());
  EXPECT_EQ(reader.current().slot, 0);
  EXPECT_EQ(reader.setlists().size(), 1u);
  EXPECT_EQ(layout(reader.list(id)), juce::String());
  EXPECT_EQ(file.loadFileAsString(), conflicted);
  EXPECT_EQ(brokenCopies().size(), 0);

  // The first write: the broken file is copied aside, byte for byte, and
  // only then replaced by what the library knows now.
  EXPECT_TRUE(reader.setFavourite("user:new", true));
  ASSERT_EQ(brokenCopies().size(), 1);
  EXPECT_TRUE(brokenCopies()[0].getFileName().startsWith("library.json.broken-"));
  EXPECT_EQ(brokenCopies()[0].loadFileAsString(), conflicted);
  EXPECT_TRUE(brokenCopies()[0].loadFileAsString().contains("user:precious"));
  juce::var written;
  ASSERT_TRUE(presetjson::readJsonFile(file, presetjson::kMaxMetaFileBytes, written).wasOk());
  EXPECT_EQ(written["format"].toString(), juce::String("pedaleira.library"));
  EXPECT_EQ(reader.favourites(), juce::StringArray({"user:new"}));

  // Once: the file is a good one again, so later writes copy nothing.
  reader.noteRecent("user:another");
  reader.setCurrent({id, 2});
  EXPECT_TRUE(reader.setFavourite("user:more", true));
  EXPECT_EQ(brokenCopies().size(), 1);
  EXPECT_EQ(SetlistLibrary(temp.root).favourites(), juce::StringArray({"user:more", "user:new"}));

  // Broken again later is set aside again, under a name of its own.
  writeFile(file, "{ \"favourites\": [ \"user:half");
  EXPECT_TRUE(reader.setFavourite("user:third", true));
  EXPECT_EQ(brokenCopies().size(), 2);

  // Not an object is broken too; an empty file is just empty.
  writeFile(file, "[1, 2, 3]");
  EXPECT_TRUE(reader.setFavourite("user:fourth", true));
  EXPECT_EQ(brokenCopies().size(), 3);
  writeFile(file, "  \n");
  EXPECT_TRUE(reader.setFavourite("user:fifth", true));
  EXPECT_EQ(brokenCopies().size(), 3);
  EXPECT_EQ(reader.favourites(), juce::StringArray({"user:fifth"}));

  // A write that fails after the copy leaves the broken file as it was.
  writeFile(file, conflicted);
  {
    const ScopedFileFault scoped(presetjson::FileFault::replace);
    EXPECT_FALSE(reader.setFavourite("user:sixth", true));
  }
  EXPECT_EQ(file.loadFileAsString(), conflicted);
  // (And trying again does not pile up copies of the same broken file.)
  const int copies = brokenCopies().size();
  {
    const ScopedFileFault scoped(presetjson::FileFault::replace);
    EXPECT_FALSE(reader.setFavourite("user:sixth", true));
  }
  EXPECT_EQ(brokenCopies().size(), copies);
}

TEST(SetlistLibraryFilesTest, ANewSetlistNeverMovesIntoAFolderThatIsThere) {
  TempLibrary temp;
  temp.create("First");
  // A folder somebody made, with a preset in it, under a name this file
  // system may take for the new setlist's although our own case folding
  // does not (Greek with a tonos is outside its table).
  const juce::String lower = juce::String::fromUTF8("\xce\xac\xce\xbb\xcf\x86\xce\xb1");  // άλφα
  const juce::String upper = juce::String::fromUTF8("\xce\x86\xce\xbb\xcf\x86\xce\xb1");  // Άλφα
  ASSERT_FALSE(SetlistLibrary::namesCollide(lower, upper));
  const juce::File theirs = temp.root.getChildFile("Setlists").getChildFile(lower);
  ASSERT_TRUE(theirs.createDirectory());
  writeFile(theirs.getChildFile("001 Theirs.json"),
            "{\"format\":\"pedaleira.preset\",\"version\":1,\"name\":\"Theirs\",\"chain\":{},\"params\":{}}");
  const juce::StringArray before = presetFiles(theirs);

  SetlistLibrary::Setlist made;
  const juce::Result created = temp.library.createSetlist(upper, &made);
  ASSERT_TRUE(created.wasOk()) << created.getErrorMessage();
  // A folder of its own, whatever the file system thinks of the two names.
  EXPECT_FALSE(theirs.getChildFile("setlist.json").exists())
      << "the new setlist moved into a folder that was there";
  EXPECT_EQ(presetFiles(theirs), before);
  EXPECT_TRUE(made.folder.getChildFile("setlist.json").existsAsFile());
  EXPECT_EQ(made.folder.getNumberOfChildFiles(juce::File::findFiles), 1);
  EXPECT_EQ(temp.library.setlists().size(), 3u);
  EXPECT_EQ(layout(temp.library.list(made.id)), juce::String());
}

TEST(SetlistLibraryFilesTest, ARenumberingCutShortKeepsEveryPresetInItsNewSlot) {
  TempLibrary temp;
  const juce::String id = temp.create("Gig");
  save(temp.library, id, "A", 1);
  save(temp.library, id, "B", 2);
  save(temp.library, id, "C", 3);
  save(temp.library, id, "D", 7);
  const juce::File folder = temp.root.getChildFile("Setlists").getChildFile("Gig");

  // A to slot 3, B and C move up, and the process dies between the two
  // passes of the rename: every file is under its scratch name.
  {
    const ScopedFileFault scoped(presetjson::FileFault::renameHalf);
    EXPECT_TRUE(temp.library.move(id, 1, 3, true).failed());
  }
  const juce::StringArray files = presetFiles(folder);
  ASSERT_EQ(files.size(), 4);
  int scratch = 0;
  for (const auto& name : files)
    scratch += name.contains("moving-") ? 1 : 0;
  EXPECT_EQ(scratch, 3);

  // The next launch (nothing remembered) lists them where they were going,
  // not in whichever slots happen to be free.
  {
    SetlistLibrary next(temp.root);
    const SetlistLibrary::Listing listing = next.list(id);
    EXPECT_EQ(layout(listing), juce::String("1:B 2:C 3:A 7:D"));
    EXPECT_TRUE(listing.ignored.empty());
    // And the first edit (or a tidy) gives the files their proper names.
    EXPECT_EQ(next.tidy(id), 3);
    EXPECT_EQ(presetFiles(folder),
              juce::StringArray({"001 B.json", "002 C.json", "003 A.json", "007 D.json"}));
  }

  // A swap cut short the same way.
  {
    const ScopedFileFault scoped(presetjson::FileFault::renameHalf);
    EXPECT_TRUE(temp.library.move(id, 1, 7, false).failed());
  }
  {
    SetlistLibrary next(temp.library.root());
    EXPECT_EQ(layout(next.list(id)), juce::String("1:D 2:C 3:A 7:B"));
  }
}

}  // namespace

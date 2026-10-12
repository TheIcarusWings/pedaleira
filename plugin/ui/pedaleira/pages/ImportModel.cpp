#include "ImportModel.h"

#include "PresetImporter.h"

#include <algorithm>
#include <functional>

namespace pedaleira::ui {

namespace {

// The importer compares names in a table and in a dump without regard to
// case, spaces and punctuation, and ignores a trailing "(M)", "(ST)" or
// "(S/C)" (plugin/src/PresetImporter.cpp, which these two follow). The
// preview has to group and match the same way the import will.
juce::String normalizeKey(const juce::String& text) {
  juce::String out;
  for (auto character : text.toLowerCase())
    if (juce::CharacterFunctions::isLetterOrDigit(character))
      out << juce::String::charToString(character);
  return out;
}

juce::String stripFormatSuffix(const juce::String& text) {
  const juce::String trimmed = text.trim();
  if (!trimmed.endsWithChar(')'))
    return trimmed;
  const int open = trimmed.lastIndexOfChar('(');
  if (open <= 0)
    return trimmed;
  const juce::String inside = trimmed.substring(open + 1, trimmed.length() - 1).trim().toLowerCase();
  for (const char* marker : {"m", "st", "s/c", "sc", "mono", "stereo"})
    if (inside == marker)
      return trimmed.substring(0, open).trim();
  return trimmed;
}

juce::String modelKey(const juce::String& name) { return normalizeKey(stripFormatSuffix(name)); }

// A sentence from the importer or the table: capital first, no full stop, so
// it can be followed by another.
juce::String sentence(const juce::String& text) {
  const juce::String trimmed = text.trim().trimCharactersAtEnd(".");
  return trimmed.substring(0, 1).toUpperCase() + trimmed.substring(1);
}

juce::String tempoText(double bpm) { return juce::String(bpm, 1) + " BPM"; }

juce::String where(const juce::var& entry) {
  return "Row " + juce::String(static_cast<int>(entry["row"]) + 1) + ", slot " +
         juce::String(static_cast<int>(entry["position"]) + 1);
}

juce::String kindNameOf(const KindIndex& kinds, const juce::String& id) {
  const auto found = kinds.find(id);
  return found != kinds.end() ? found->second.name : id;
}

juce::String valueText(const juce::var& value) {
  if (value.isDouble())
    return juce::String(static_cast<double>(value), 3).trimCharactersAtEnd("0").trimCharactersAtEnd(".");
  return value.toString();
}

}  // namespace

// ####################
// READING FILES
// ####################
juce::Array<juce::File> expandChoice(const juce::Array<juce::File>& chosen) {
  juce::Array<juce::File> files;
  for (const auto& entry : chosen) {
    if (entry.isDirectory()) {
      auto inside = entry.findChildFiles(juce::File::findFiles, false, "*.json");
      inside.sort();
      for (const auto& file : inside)
        files.addIfNotAlreadyThere(file);
    } else if (entry.existsAsFile()) {
      files.addIfNotAlreadyThere(entry);
    }
  }
  return files;
}

std::vector<ImportItem> readImportItems(ImportKind kind, const juce::Array<juce::File>& files) {
  std::vector<ImportItem> items;
  for (const auto& file : files) {
    ImportItem item;
    item.file = file;
    item.name = file.getFileNameWithoutExtension();
    juce::String problem;
    const juce::var parsed = t3k::importer::readJsonFile(file, problem);
    if (problem.isNotEmpty()) {
      item.problem = problem;
    } else if (kind == ImportKind::quadCortex) {
      item.problem = t3k::importer::checkForeignPreset(parsed);
      if (item.problem.isEmpty()) {
        if (parsed["name"].toString().trim().isNotEmpty())
          item.name = parsed["name"].toString();
        item.tempo = parsed["tempo"].isVoid() ? 0.0 : static_cast<double>(parsed["tempo"]);
        item.sourceVersion = parsed["sourceVersion"].toString();
        if (const auto* scenes = parsed["scenes"].getArray())
          item.scenes = scenes->size();
        if (const auto* rows = parsed["rows"].getArray())
          for (const auto& row : *rows)
            if (const auto* blocks = row["blocks"].getArray())
              for (const auto& block : *blocks) {
                SourceBlock b;
                b.row = row["index"].isVoid() ? 0 : static_cast<int>(row["index"]);
                b.position = block["position"].isVoid() ? 0 : static_cast<int>(block["position"]);
                b.model = block["model"].toString();
                b.category = block["category"].toString();
                item.blocks.push_back(b);
              }
        const juce::String source = parsed["source"].toString();
        if (!t3k::importer::builtInMappingSources().contains(source))
          item.problem = "No mapping table for the source \"" + source + "\" is built in";
      }
    } else {
      const juce::String format = parsed["format"].toString();
      if (format != "pedaleira.preset")
        item.problem = "Not a Pedaleira preset file";
      else if (parsed["name"].toString().trim().isNotEmpty())
        item.name = parsed["name"].toString();
    }
    item.selected = item.readable();
    items.push_back(std::move(item));
  }
  return items;
}

KindIndex readKinds(EngineClient& engine) {
  KindIndex index;
  const juce::var kinds = engine.read("kinds.list");
  if (const auto* list = kinds["kinds"].getArray())
    for (const auto& kind : *list) {
      KindInfo info;
      info.name = kind["name"].toString();
      info.category = categoryFromId(kind["category"].toString());
      info.addable = static_cast<bool>(kind["addable"]);
      index[kind["id"].toString()] = info;
    }
  return index;
}

// ####################
// MAPPING PREVIEW
// ####################
std::vector<MappingRow> previewMapping(EngineClient& engine, const juce::File& mappingFile,
                                       const std::vector<ImportItem>& items, const KindIndex& kinds) {
  struct Group {
    MappingRow row;
    juce::StringArray presets;
  };
  std::map<juce::String, Group> groups;
  for (const auto& item : items) {
    if (!item.selected || !item.readable())
      continue;
    // The importer's own dry run decides what each block becomes, so the
    // preview cannot drift from the import.
    juce::DynamicObject::Ptr args = new juce::DynamicObject();
    args->setProperty("file", item.file.getFullPathName());
    if (mappingFile != juce::File())
      args->setProperty("mapping", mappingFile.getFullPathName());
    const juce::var report = engine.read("preset.previewForeign", juce::var(args.get()))["report"];

    const auto add = [&](const juce::var& entry, const std::function<void(MappingRow&)>& fill) {
      const juce::String model = entry["model"].toString();
      const juce::String category = entry["category"].toString();
      // Display grouping only: the same model spelled "(M)" or "(ST)" is one row.
      const juce::String key = normalizeKey(category) + "|" + modelKey(model);
      auto found = groups.find(key);
      if (found == groups.end()) {
        Group group;
        group.row.sourceBlock =
            (category.isNotEmpty() ? stripFormatSuffix(category) + ": " : juce::String()) + stripFormatSuffix(model);
        fill(group.row);
        found = groups.emplace(key, std::move(group)).first;
      }
      found->second.presets.addIfNotAlreadyThere(item.file.getFullPathName());
    };
    const auto setKind = [&kinds](MappingRow& row, const juce::String& id, MappingRow::Kind kind) {
      row.kind = kind;
      row.kindId = id;
      const auto info = kinds.find(id);
      row.kindName = info != kinds.end() ? info->second.name : id;
      row.category = info != kinds.end() ? info->second.category : Category::utility;
    };

    if (const auto* mapped = report["mapped"].getArray())
      for (const auto& entry : *mapped)
        add(entry, [&](MappingRow& row) {
          setKind(row, entry["kind"].toString(), MappingRow::Kind::mapped);
          row.verified = static_cast<bool>(entry["verified"]);
        });
    if (const auto* approximated = report["approximated"].getArray())
      for (const auto& entry : *approximated)
        if (entry["type"].toString() == "block")
          add(entry, [&](MappingRow& row) { setKind(row, entry["kind"].toString(), MappingRow::Kind::fallback); });
    if (const auto* unmapped = report["unmapped"].getArray())
      for (const auto& entry : *unmapped)
        add(entry, [&](MappingRow& row) {
          const juce::String role = entry["role"].toString();
          if (role.isNotEmpty()) {
            row.kind = MappingRow::Kind::empty;
            row.role = role;
            row.note = role == "capture" ? "Cannot import, recapture it"
                       : role == "cab"   ? "Left empty: pick an IR"
                                         : "Left empty: pick a NAM capture";
          } else {
            row.kind = MappingRow::Kind::unmapped;
            const juce::String cause = entry["cause"].toString();
            row.note = cause == "noFallback"    ? juce::String("No block of ours for this category")
                       : cause == "notInTable"  ? juce::String("Not in the mapping table")
                       : cause == "mapsToNothing" ? juce::String("The table maps this model to nothing")
                       : cause == "unknownKind" ? juce::String("The table names a block this build does not have")
                                                : sentence(entry["reason"].toString());
          }
        });
  }

  std::vector<MappingRow> rows;
  for (auto& [key, group] : groups) {
    group.row.usedIn = group.presets.size();
    rows.push_back(std::move(group.row));
  }
  std::stable_sort(rows.begin(), rows.end(), [](const MappingRow& a, const MappingRow& b) {
    if (a.usedIn != b.usedIn)
      return a.usedIn > b.usedIn;
    return a.sourceBlock.compareNatural(b.sourceBlock) < 0;
  });
  return rows;
}

// ####################
// THE REPORT
// ####################
ImportOutcome::Status statusOf(const ImportOutcome& outcome) {
  if (outcome.error.isNotEmpty())
    return ImportOutcome::Status::failed;
  // A Pedaleira preset has no mapping: it is fine unless a model file is
  // missing.
  if (outcome.report.isVoid())
    return outcome.missingAssets.isEmpty() ? ImportOutcome::Status::ok : ImportOutcome::Status::check;

  const auto count = [&](const char* list) {
    const auto* array = outcome.report[list].getArray();
    return array != nullptr ? array->size() : 0;
  };
  if (count("unmapped") > 0)
    return ImportOutcome::Status::placeholder;
  if (count("approximated") > 0 || count("droppedParams") > 0)
    return ImportOutcome::Status::check;
  // Every mapped block counts as exact only once its entry was checked
  // against a real unit.
  if (const auto* mapped = outcome.report["mapped"].getArray())
    for (const auto& entry : *mapped)
      if (!static_cast<bool>(entry["verified"]))
        return ImportOutcome::Status::check;
  return ImportOutcome::Status::ok;
}

const char* statusName(ImportOutcome::Status status) {
  switch (status) {
    case ImportOutcome::Status::ok: return "OK";
    case ImportOutcome::Status::check: return "Check";
    case ImportOutcome::Status::placeholder: return "Placeholder";
    case ImportOutcome::Status::failed: break;
  }
  return "Failed";
}

std::vector<ReportSection> reportSections(ImportKind kind, const ImportOutcome& outcome, const KindIndex& kinds) {
  ReportSection exact, approximated, needs, notes;
  exact.kind = ReportSection::Kind::exact;
  exact.title = "Mapped exactly";
  approximated.kind = ReportSection::Kind::approximated;
  approximated.title = "Approximated: tune by ear";
  needs.kind = ReportSection::Kind::needs;
  needs.title = "Needs you";
  notes.kind = ReportSection::Kind::notes;
  notes.title = "Notes";

  const auto add = [](ReportSection& section, const juce::String& label, const juce::String& detail,
                      bool mono = false) {
    ReportRow row;
    row.label = label;
    row.detail = detail;
    row.mono = mono;
    row.key = section.title + "|" + label + "|" + detail;
    section.rows.push_back(row);
  };

  if (kind == ImportKind::pedaleira) {
    add(exact, "Blocks and scenes", "Kept as the preset was saved");
    for (const auto& asset : outcome.missingAssets) {
      ReportRow row;
      row.detail = "Model file " + asset + " was not found. Its block comes up missing until the file is in the library.";
      row.key = "Needs you|" + asset;
      needs.rows.push_back(row);
    }
    return {exact, approximated, needs, notes};
  }

  const juce::var& report = outcome.report;
  const auto* unmapped = report["unmapped"].getArray();
  const int unmappedCount = unmapped != nullptr ? unmapped->size() : 0;
  const int placed = juce::jmax(0, outcome.blocks - unmappedCount);
  if (outcome.blocks == 0)
    add(exact, "Grid layout", "An empty grid");
  else
    add(exact, "Grid layout",
        juce::String(placed) + " of " + juce::String(outcome.blocks) + " blocks placed in " + juce::String(outcome.rows) +
            (outcome.rows == 1 ? " row" : " rows"));
  if (outcome.scenes > 0)
    add(exact, "Scenes",
        juce::String(outcome.scenes) + (outcome.scenes == 1 ? " scene" : " scenes") + ", names and on or off states");
  if (outcome.tempo > 0.0)
    add(exact, "Tempo", tempoText(outcome.tempo), true);

  if (const auto* mapped = report["mapped"].getArray())
    for (const auto& entry : *mapped) {
      const juce::String label = entry["model"].toString() + " to " + kindNameOf(kinds, entry["kind"].toString());
      juce::String detail = where(entry);
      if (entry["notes"].toString().isNotEmpty())
        detail << ". " << sentence(entry["notes"].toString());
      if (static_cast<bool>(entry["verified"])) {
        add(exact, label, detail);
      } else {
        detail << ". The mapping is not verified against a real unit.";
        add(approximated, label, detail);
      }
    }
  if (const auto* list = report["approximated"].getArray())
    for (const auto& entry : *list) {
      if (entry["type"].toString() == "param")
        add(approximated, entry["model"].toString() + ": " + entry["param"].toString(),
            where(entry) + ". " + valueText(entry["from"]) + " became " + valueText(entry["to"]) + " on " +
                entry["ourParam"].toString() + ", out of range and clamped");
      else
        add(approximated, entry["model"].toString() + " to " + kindNameOf(kinds, entry["kind"].toString()),
            where(entry) + ". " + sentence(entry["reason"].toString()));
    }
  if (const auto* list = report["droppedParams"].getArray())
    for (const auto& entry : *list)
      add(approximated, entry["model"].toString() + ": " + entry["param"].toString(),
          where(entry) + ". Not carried over: " + sentence(entry["reason"].toString()));

  if (unmapped != nullptr)
    for (const auto& entry : *unmapped) {
      ReportRow row;
      row.label = entry["model"].toString();
      row.detail = where(entry) + ". " + sentence(entry["reason"].toString());
      row.row = entry["row"];
      row.position = entry["position"];
      row.role = entry["role"].toString();
      row.key = "Needs you|" + row.label + "|" + row.detail;
      needs.rows.push_back(row);
    }
  if (const auto* list = report["notes"].getArray())
    for (const auto& note : *list)
      add(notes, {}, note.toString());

  return {exact, approximated, needs, notes};
}

juce::String reportMarkdown(ImportKind kind, const std::vector<ImportOutcome>& outcomes, const KindIndex& kinds,
                            const juce::String& setlistName) {
  int counts[4] = {0, 0, 0, 0};
  for (const auto& outcome : outcomes)
    ++counts[static_cast<int>(statusOf(outcome))];
  const int imported = counts[0] + counts[1] + counts[2];

  juce::String md;
  md << "# Import report\n\n";
  md << (kind == ImportKind::quadCortex ? "Source: Quad Cortex (from dump files)" : "Source: Pedaleira presets")
     << ".\n";
  md << "Imported into the setlist \"" << setlistName << "\" on " << juce::Time::getCurrentTime().formatted("%Y-%m-%d")
     << ".\n\n";
  if (kind == ImportKind::quadCortex)
    md << "Every Quad Cortex mapping is unverified until it has been checked against a dump from a real unit, so "
          "nothing here counts as exact yet. Listen to each preset.\n\n";
  md << "- Presets imported: " << imported << "\n";
  md << "- OK: " << counts[0] << "\n";
  md << "- To check by ear: " << counts[1] << "\n";
  md << "- With placeholders: " << counts[2] << "\n";
  if (counts[3] > 0)
    md << "- Could not be imported: " << counts[3] << "\n";
  md << "\n";

  for (const auto& outcome : outcomes) {
    const auto status = statusOf(outcome);
    md << "## " << outcome.name;
    if (outcome.slot > 0)
      md << " (slot " << juce::String(outcome.slot).paddedLeft('0', 3) << ")";
    md << ": " << statusName(status) << "\n\n";
    if (status == ImportOutcome::Status::failed) {
      md << "Not imported: " << outcome.error << "\n\n";
      continue;
    }
    for (const auto& section : reportSections(kind, outcome, kinds)) {
      if (section.rows.empty())
        continue;
      md << "### " << section.title << "\n\n";
      for (const auto& row : section.rows) {
        md << "- ";
        if (row.label.isNotEmpty())
          md << row.label << ": ";
        md << row.detail << "\n";
      }
      md << "\n";
    }
  }
  return md;
}

}  // namespace pedaleira::ui

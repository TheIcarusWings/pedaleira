// What the importer's screens work from, kept as plain values: the files the
// player chose and what is in them, the mapping preview (the table read against
// the blocks of the chosen presets), the outcome of each import and the
// report sections built from it. No drawing and no engine writes in here, so
// the report text can be tested on its own.
#pragma once

#include "../EngineClient.h"
#include "../Tokens.h"

#include <juce_core/juce_core.h>

#include <map>
#include <vector>

namespace pedaleira::ui {

enum class ImportKind { quadCortex, pedaleira };

// A block of a foreign preset, by where it sits and the source's own names.
struct SourceBlock {
  int row = 0;
  int position = 0;
  juce::String model, category;
};

// One file the player chose.
struct ImportItem {
  juce::File file;
  juce::String name;
  // Why it cannot be imported; empty when it can.
  juce::String problem;
  std::vector<SourceBlock> blocks;
  int scenes = 0;
  // Zero when the file does not say.
  double tempo = 0.0;
  juce::String sourceVersion;
  bool selected = true;

  bool readable() const { return problem.isEmpty(); }
};

// The files of a choice: files as they are, a folder as its .json files.
juce::Array<juce::File> expandChoice(const juce::Array<juce::File>& chosen);
// Reads each file just enough to list it; a file that is not one of this
// source is kept with its problem.
std::vector<ImportItem> readImportItems(ImportKind kind, const juce::Array<juce::File>& files);

// Our block kinds by id, from kinds.list.
struct KindInfo {
  juce::String name;
  Category category = Category::utility;
  bool addable = false;
};
using KindIndex = std::map<juce::String, KindInfo>;
KindIndex readKinds(EngineClient& engine);

// One distinct source block of the chosen presets and what the table does
// with it.
struct MappingRow {
  enum class Kind {
    mapped,    // the table names the model
    fallback,  // only the category is named: a plain block at defaults
    empty,     // an amp, a cab or a capture: the slot stays empty
    unmapped,  // nothing in the table, or a kind this build lacks
  };
  juce::String sourceBlock;  // "Reverb: Hall"
  int usedIn = 0;            // presets that hold it
  Kind kind = Kind::unmapped;
  juce::String kindId, kindName;
  Category category = Category::utility;
  bool verified = false;
  // empty, unmapped and capture rows: what happens
  juce::String note;
  // amp, cab or capture for an empty row
  juce::String role;
};
// What the import will do with each distinct block of the selected, readable
// items, from the importer's dry run (preset.previewForeign) with the table
// at `mappingFile`, or the built-in one when that is empty.
std::vector<MappingRow> previewMapping(EngineClient& engine, const juce::File& mappingFile,
                                       const std::vector<ImportItem>& items, const KindIndex& kinds);

// What importing one file gave.
struct ImportOutcome {
  enum class Status { ok, check, placeholder, failed };
  juce::String name;
  juce::File file;
  Status status = Status::ok;
  juce::String error;
  juce::String presetId, setlistName;
  int slot = 0;
  // The importer's report (preset.importForeign); void for a Pedaleira preset.
  juce::var report;
  juce::StringArray missingAssets;
  // The source file's own numbers, for the report.
  int blocks = 0;
  int rows = 0;
  int scenes = 0;
  double tempo = 0.0;
};

// A line of a report section.
struct ReportRow {
  juce::String label, detail;
  // The detail is a number to read, set in mono.
  bool mono = false;
  // For a row of "Needs you": where the block was and what it was, so the
  // player can pick a capture for it.
  int row = -1, position = -1;
  juce::String role;
  juce::String key;
};
struct ReportSection {
  enum class Kind { exact, approximated, needs, notes };
  Kind kind = Kind::exact;
  juce::String title;
  std::vector<ReportRow> rows;
};

// The status of an outcome from its report.
ImportOutcome::Status statusOf(const ImportOutcome& outcome);
const char* statusName(ImportOutcome::Status status);
// The sections of one preset's report. `kind` decides the wording for a
// Pedaleira preset, which has no mapping.
std::vector<ReportSection> reportSections(ImportKind kind, const ImportOutcome& outcome, const KindIndex& kinds);
// The whole import as Markdown.
juce::String reportMarkdown(ImportKind kind, const std::vector<ImportOutcome>& outcomes, const KindIndex& kinds,
                            const juce::String& setlistName);

}  // namespace pedaleira::ui

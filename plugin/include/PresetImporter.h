// The importer: a "foreign preset" (a preset read from another product by one
// of the readers under tools/import, docs/import.md) turned into a Pedaleira
// grid preset, with a report of what was carried over and what was not.
//
// The importer is generic. It knows nothing about any one product: the
// reader hands it neutral JSON (model name, category, parameters in the
// source's own units, rows, splits, scenes) and a mapping table says, for
// that source, which of our block kinds each model becomes and how each
// parameter converts. Both are plain JSON, so a new source is a reader and a
// table, and no engine code.
//
// What it never does: an amp, a cab or a capture of the source is not
// mapped to a model of ours. The slot is left empty and the report says so,
// since the player picks a NAM capture or an IR. Nothing is guessed: a model
// or parameter the table does not know is named in the report.
//
// The output is a preset in exactly the shape preset.importJson reads: the
// preset tree is built and written by PresetJson, so the text is what the
// library would write itself. Depends on the block kind registry (for
// parameter ranges and defaults) and on the preset JSON; it touches no rig.
#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

namespace t3k::importer {

constexpr const char* kForeignFormat = "pedaleira-foreign-preset";
constexpr int kForeignVersion = 1;
constexpr int kMappingVersion = 1;

struct ImportResult {
  bool ok{false};
  // Why not, when !ok: names the field and what was expected.
  juce::String error;
  // The preset as text in the canonical JSON of the library (PresetJson.h).
  juce::String presetJson;
  // The same preset as a value (an object), for callers that want to look.
  juce::var preset;
  // { mapped: [], approximated: [], unmapped: [], droppedParams: [], notes: [] }.
  // Every entry names the block by row and position (zero based, as the
  // foreign file has them) and by the source's model name. See docs/import.md
  // for the fields of each list.
  juce::var report;
};

/** Empty when `foreign` is a foreign preset this build can read, else why not
    (not a JSON object, another format, no or a newer version, no source or
    rows). Says nothing about any mapping table. */
juce::String checkForeignPreset(const juce::var& foreign);

/** Converts a parsed foreign preset using a parsed mapping table. `foreign`
    and `mapping` are the JSON of docs/import.md, as juce::var objects.
    Never throws; a file that is not a foreign preset, a version this build
    does not read, or a mapping for another source comes back as !ok with the
    reason. */
ImportResult convertForeignPreset(const juce::var& foreign, const juce::var& mapping);

/** The mapping table built into the app for a source id ("quad-cortex"), or
    a void var when there is none. Parsed from resources/import/<source>.json
    as it was at build time. */
juce::var builtInMapping(const juce::String& source);

/** The source ids that have a built-in table. */
juce::StringArray builtInMappingSources();

/** Reads a JSON file with the guarded reader of the library (size and depth
    limits). On failure `error` says why and the result is void. */
juce::var readJsonFile(const juce::File& file, juce::String& error);

}  // namespace t3k::importer

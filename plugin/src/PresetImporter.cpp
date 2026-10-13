#include "PresetImporter.h"

#include "BlockEq.h"
#include "BlockKinds.h"
#include "GridTopology.h"
#include "PedaleiraImportData.h"
#include "PresetFile.h"
#include "PresetJson.h"
#include "Scenes.h"
#include "SplitJunction.h"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace t3k::importer {

namespace {

using Fields = std::initializer_list<std::pair<const char*, juce::var>>;

// The most a foreign preset or a mapping table may be. A real dump is tens of
// kilobytes; a table a few hundred.
constexpr juce::int64 kMaxImportFileBytes = 16 * 1024 * 1024;

// The kind-level note every block of the source that plays a model of its own
// (an amp, a cab, a capture) gets: we do not map those.
constexpr const char* kPickCaptureNote =
    "Not mapped on purpose: pick a NAM capture or an IR for this slot.";

juce::var makeObject(Fields fields) {
  juce::DynamicObject::Ptr object = new juce::DynamicObject();
  for (const auto& [name, value] : fields)
    object->setProperty(name, value);
  return juce::var(object.get());
}

juce::var asArray(const juce::Array<juce::var>& items) { return juce::var(items); }

// Lower case letters and digits only: "Plate Lush", "PLATE-LUSH" and
// "plate_lush" are one name. Names in a table and in a dump are compared
// this way.
juce::String normalizeKey(const juce::String& text) {
  juce::String out;
  for (auto character : text.toLowerCase())
    if (juce::CharacterFunctions::isLetterOrDigit(character))
      out << juce::String::charToString(character);
  return out;
}

// "Digital Delay (M)" and "Digital Delay (ST)" are one model for us: the
// source keeps a mono and a stereo version of many blocks. A trailing
// parenthesis holding only a mono/stereo marker is dropped.
juce::String stripFormatSuffix(const juce::String& text) {
  const juce::String trimmed = text.trim();
  if (!trimmed.endsWithChar(')'))
    return trimmed;
  const int open = trimmed.lastIndexOfChar('(');
  if (open <= 0)
    return trimmed;
  const juce::String inside = trimmed.substring(open + 1, trimmed.length() - 1).trim().toLowerCase();
  static const char* const markers[] = {"m", "st", "s/c", "sc", "mono", "stereo"};
  for (const char* marker : markers)
    if (inside == marker)
      return trimmed.substring(0, open).trim();
  return trimmed;
}

juce::String modelKey(const juce::String& name) { return normalizeKey(stripFormatSuffix(name)); }

bool isNumber(const juce::var& value) {
  return value.isInt() || value.isInt64() || value.isDouble() || value.isBool();
}

double numberOf(const juce::var& value) { return static_cast<double>(value); }

// A value as the text a table's "map" is keyed by: a whole number without a
// decimal point, a bool as true/false.
juce::String valueKey(const juce::var& value) {
  if (value.isBool())
    return static_cast<bool>(value) ? "true" : "false";
  if (value.isInt() || value.isInt64())
    return juce::String(static_cast<juce::int64>(value));
  if (value.isDouble()) {
    const double number = static_cast<double>(value);
    if (std::floor(number) == number && std::abs(number) < 1.0e15)
      return juce::String(static_cast<juce::int64>(number));
    return juce::String(number, 6).trimCharactersAtEnd("0");
  }
  return value.toString();
}

juce::String describeValue(const juce::var& value) {
  if (value.isString())
    return "\"" + value.toString() + "\"";
  return valueKey(value);
}

// A short, stable id from text: the first 32 hex digits of its SHA-256.
juce::String idFrom(const juce::String& text) {
  const juce::String utf8 = text;
  const juce::SHA256 sha(utf8.toRawUTF8(), utf8.getNumBytesAsUTF8());
  return sha.toHexString().substring(0, 32);
}

// ####################
// THE REPORT
// ####################

struct Report {
  juce::Array<juce::var> mapped, approximated, unmapped, droppedParams;
  juce::StringArray notes;

  juce::var toVar() const {
    juce::Array<juce::var> notesOut;
    for (const auto& note : notes)
      notesOut.add(note);
    return makeObject({{"mapped", asArray(mapped)},
                       {"approximated", asArray(approximated)},
                       {"unmapped", asArray(unmapped)},
                       {"droppedParams", asArray(droppedParams)},
                       {"notes", asArray(notesOut)}});
  }
  void note(const juce::String& text) {
    if (!notes.contains(text))
      notes.add(text);
  }
};

juce::var blockEntry(int row, int position, const juce::String& model, Fields extra) {
  juce::DynamicObject::Ptr object = new juce::DynamicObject();
  object->setProperty("row", row);
  object->setProperty("position", position);
  object->setProperty("model", model);
  for (const auto& [name, value] : extra)
    object->setProperty(name, value);
  return juce::var(object.get());
}

// ####################
// THE MAPPING TABLE
// ####################

class MappingTable {
public:
  // Empty on success, else why the table is not usable.
  juce::String load(const juce::var& mapping) {
    if (mapping.getDynamicObject() == nullptr)
      return "the mapping table is not a JSON object";
    source = mapping["source"].toString();
    if (source.isEmpty())
      return "the mapping table has no \"source\"";
    const int version = mapping["version"].isVoid() ? 0 : static_cast<int>(mapping["version"]);
    if (version < 1)
      return "the mapping table has no \"version\"";
    if (version > kMappingVersion)
      return "the mapping table is version " + juce::String(version) +
             " and this build reads up to " + juce::String(kMappingVersion);
    if (const auto* models = mapping["models"].getDynamicObject()) {
      for (const auto& property : models->getProperties())
        modelsByKey.emplace(modelKey(property.name.toString()), property.value);
    } else if (!mapping["models"].isVoid()) {
      return "\"models\" in the mapping table must be an object";
    }
    if (const auto* categories = mapping["categories"].getDynamicObject()) {
      for (const auto& property : categories->getProperties())
        categoriesByKey.emplace(normalizeKey(property.name.toString()), property.value);
    } else if (!mapping["categories"].isVoid()) {
      return "\"categories\" in the mapping table must be an object";
    }
    return {};
  }

  const juce::String& sourceId() const { return source; }

  const juce::var* model(const juce::String& name) const {
    const auto it = modelsByKey.find(modelKey(name));
    return it == modelsByKey.end() ? nullptr : &it->second;
  }

  const juce::var* category(const juce::String& name) const {
    const auto it = categoriesByKey.find(normalizeKey(name));
    return it == categoriesByKey.end() ? nullptr : &it->second;
  }

private:
  juce::String source;
  std::map<juce::String, juce::var> modelsByKey, categoriesByKey;
};

// The entry of a table's "params" for a source parameter, by normalized name.
const juce::var* findParamEntry(const juce::var& modelEntry, const juce::String& name) {
  const auto* params = modelEntry["params"].getDynamicObject();
  if (params == nullptr)
    return nullptr;
  const juce::String wanted = normalizeKey(name);
  for (const auto& property : params->getProperties())
    if (normalizeKey(property.name.toString()) == wanted)
      return &property.value;
  return nullptr;
}

// ####################
// PARAMETER VALUES
// ####################

// Where a mapped value lands on our block: one of the common settings or a
// parameter of the kind.
enum class TargetType { kind, mix, inputGain, outputGain, enabled };

struct Target {
  TargetType type{TargetType::kind};
  int index{-1};  // into the kind's table
};

struct Resolved {
  bool ok{false};
  float value{0.0f};
  bool clamped{false};
  float raw{0.0f};
  juce::String why;
  Target target;
  juce::String ourId;
};

std::optional<Target> resolveTarget(const juce::String& id, const BlockKindInfo& info) {
  if (id == "mix")
    return Target{TargetType::mix, -1};
  if (id == "inputGain")
    return Target{TargetType::inputGain, -1};
  if (id == "outputGain")
    return Target{TargetType::outputGain, -1};
  if (id == "enabled")
    return Target{TargetType::enabled, -1};
  const int index = findBlockParam(info.params, id);
  if (index < 0)
    return std::nullopt;
  return Target{TargetType::kind, index};
}

// The source value as a number for a target: numbers and bools as they are,
// the label of one of a choice parameter's options as its index, a numeric
// string as the number.
std::optional<double> numberFor(const juce::var& value, const BlockParamSpec* spec) {
  if (isNumber(value))
    return numberOf(value);
  if (value.isString()) {
    const juce::String text = value.toString().trim();
    if (spec != nullptr && spec->type == BlockParamType::Choice) {
      for (int i = 0; i < spec->choices.size(); ++i)
        if (normalizeKey(spec->choices[i]) == normalizeKey(text))
          return static_cast<double>(i);
    }
    if (text.containsOnly("+-.0123456789eE") && text.isNotEmpty())
      return text.getDoubleValue();
  }
  return std::nullopt;
}

// Applies a table entry's transform to a source value and brings the result
// into the range of the target. `entry` is the table's { param, transform }.
Resolved resolveValue(const juce::var& entry, const juce::var& sourceValue,
                      const BlockKindInfo& info) {
  Resolved out;
  const juce::String ourId = entry["param"].toString();
  out.ourId = ourId;
  const auto target = resolveTarget(ourId, info);
  if (!target) {
    out.why = "the mapping table names \"" + ourId + "\", which " + info.id + " does not have";
    return out;
  }
  out.target = *target;
  const BlockParamSpec* spec =
      target->type == TargetType::kind ? &info.params[static_cast<size_t>(target->index)] : nullptr;

  juce::var working = sourceValue;
  const juce::var transform = entry["transform"];
  if (const auto* map = transform["map"].getDynamicObject()) {
    const juce::String key = normalizeKey(valueKey(sourceValue));
    bool found = false;
    for (const auto& property : map->getProperties())
      if (normalizeKey(property.name.toString()) == key) {
        working = property.value;
        found = true;
        break;
      }
    if (!found) {
      out.why = "the value " + describeValue(sourceValue) + " is not in the table's map";
      return out;
    }
  }

  const std::optional<double> number = numberFor(working, spec);
  if (!number) {
    out.why = "the value " + describeValue(sourceValue) + " is not a number";
    return out;
  }
  double result = *number;
  if (transform.getDynamicObject() != nullptr && transform["map"].isVoid()) {
    const double scale = transform["scale"].isVoid() ? 1.0 : numberOf(transform["scale"]);
    const double offset = transform["offset"].isVoid() ? 0.0 : numberOf(transform["offset"]);
    if (!transform["exponent"].isVoid()) {
      // A curve: scale * (value / inputMax) ^ exponent + offset, for a
      // parameter whose law is not a straight line of the unit's knob
      // (inputMax is the knob's own range, 100 unless the table says).
      const double inputMax = transform["inputMax"].isVoid() ? 100.0 : numberOf(transform["inputMax"]);
      const double unit = inputMax > 0.0 ? juce::jmax(0.0, result / inputMax) : 0.0;
      result = scale * std::pow(unit, numberOf(transform["exponent"])) + offset;
    } else {
      result = result * scale + offset;
    }
  } else if (transform.isString() && transform.toString() != "linear") {
    out.why = "unknown transform \"" + transform.toString() + "\" in the mapping table";
    return out;
  }
  if (!std::isfinite(result)) {
    out.why = "the converted value is not a finite number";
    return out;
  }

  out.raw = static_cast<float>(result);
  switch (target->type) {
    case TargetType::kind:
      if (spec->type == BlockParamType::Bool) {
        out.value = result >= 0.5 ? 1.0f : 0.0f;
      } else {
        out.value = spec->clamp(out.raw);
        out.clamped = out.raw < spec->min - 1.0e-4f || out.raw > spec->max + 1.0e-4f;
      }
      break;
    case TargetType::enabled:
      out.value = result >= 0.5 ? 1.0f : 0.0f;
      break;
    case TargetType::mix:
    case TargetType::inputGain:
    case TargetType::outputGain:
      out.value = juce::jlimit(0.0f, 1.0f, out.raw);
      out.clamped = out.raw < -1.0e-4f || out.raw > 1.0f + 1.0e-4f;
      break;
  }
  out.ok = true;
  return out;
}

// The scalar of a foreign parameter ({ value, unit } or the bare value).
juce::var paramValueOf(const juce::var& param) {
  if (param.getDynamicObject() != nullptr)
    return param["value"];
  return param;
}

// ####################
// THE PRESET BEING BUILT
// ####################

constexpr const char* kGeneralInputNote =
    "The file has no routing; row 0 got input 1 and output 1/2.";

struct SceneValue {
  juce::String paramId;  // ours
  TargetType type{TargetType::kind};
  std::array<float, kNumScenes> values{};
  std::array<bool, kNumScenes> given{};
};

// One slot of the grid.
struct Slot {
  bool block{false};
  juce::String id;
  juce::String kindId;
  juce::String kindName;
  bool enabled{true};
  float mix{1.0f};
  float inputGain{0.5f};
  float outputGain{0.5f};
  std::map<juce::String, float> values;  // by our parameter id, real units
  // The source's description, for scene entries and the report.
  int sourceRow{0};
  int sourcePosition{0};
  juce::String sourceModel;
  const BlockKindInfo* info{nullptr};
  // Scenes: the enabled state per scene when the source gave any, and the
  // scene-followed parameters.
  std::array<std::optional<bool>, kNumScenes> sceneEnabled;
  std::vector<SceneValue> sceneValues;
  const juce::var* modelEntry{nullptr};
};

struct RowPlan {
  juce::String inputType{"none"};  // "device" or "none"
  int inputChannel{0};
  bool inputStereo{false};
  juce::String outputType{"none"};  // "device", "row" or "none"
  int outputPair{0};
  int outputToRow{-1};
};

struct SplitPlan {
  int upperRow{0};
  int splitAt{0};
  int mixAt{-1};
};

float readFloat(const juce::var& value, float fallback) {
  return isNumber(value) && std::isfinite(numberOf(value)) ? static_cast<float>(numberOf(value))
                                                           : fallback;
}

int readInt(const juce::var& value, int fallback) {
  if (!isNumber(value) || !std::isfinite(numberOf(value)) || std::abs(numberOf(value)) > 1.0e9)
    return fallback;
  return static_cast<int>(std::lround(numberOf(value)));
}

// A KindState node at the given values, written by the kind's own processor
// so it is exactly the node a save of the rig would hold.
juce::ValueTree makeKindState(const BlockKindInfo& info,
                              const std::map<juce::String, float>& values) {
  std::unique_ptr<BlockProcessor> processor = info.factory ? info.factory() : nullptr;
  if (processor == nullptr)
    return {};
  for (const auto& [id, value] : values) {
    const int index = processor->findParameter(id);
    if (index >= 0)
      processor->setParameter(index, value);
  }
  return processor->getKindState(info.name);
}

juce::ValueTree makeBlockNode(const Slot& slot, bool activeEnabled, float mix,
                              const std::map<juce::String, float>& values) {
  juce::ValueTree node("ChainBlock");
  node.setProperty("id", slot.id, nullptr);
  node.setProperty("type", slot.block ? "kind" : "insert", nullptr);
  node.setProperty("enabled", slot.block ? activeEnabled : true, nullptr);
  node.setProperty("normalize", true, nullptr);
  node.setProperty("slimSize", 0.0, nullptr);
  node.setProperty("inputGain", slot.block ? slot.inputGain : 0.5f, nullptr);
  node.setProperty("outputGain", slot.block ? slot.outputGain : 0.5f, nullptr);
  node.setProperty("mix", slot.block ? mix : 1.0f, nullptr);
  if (slot.block) {
    node.appendChild(BlockEq().toValueTree(), nullptr);
    node.appendChild(makeKindState(*slot.info, values), nullptr);
  }
  return node;
}

class Importer {
public:
  Importer(const juce::var& foreignIn, const MappingTable& tableIn)
      : foreign(foreignIn), table(tableIn) {}

  ImportResult run() {
    ImportResult result;
    if (const juce::String problem = checkForeign(); problem.isNotEmpty()) {
      result.error = problem;
      return result;
    }
    name = foreign["name"].toString().trim();
    if (name.isEmpty())
      name = "Imported preset";
    activeScene = juce::jlimit(0, kNumScenes - 1, readInt(foreign["defaultScene"], 0));

    placeBlocks();
    planRouting();
    carryScenes();

    juce::ValueTree preset = buildPresetTree();
    juce::String error;
    result.presetJson = presetjson::toJson(preset, {}, error);
    if (result.presetJson.isEmpty()) {
      result.error = "could not write the preset: " + error;
      return result;
    }
    if (const juce::Result parsed = presetjson::parseJsonGuarded(result.presetJson, result.preset);
        parsed.failed()) {
      result.error = "internal error: " + parsed.getErrorMessage();
      return result;
    }
    result.report = report.toVar();
    result.ok = true;
    return result;
  }

private:
  // ---- the file ----

  juce::String checkForeign() const {
    if (const juce::String problem = checkForeignPreset(foreign); problem.isNotEmpty())
      return problem;
    const juce::String source = foreign["source"].toString();
    if (normalizeKey(source) != normalizeKey(table.sourceId()))
      return "the mapping table is for \"" + table.sourceId() + "\" but the file comes from \"" +
             source + "\"";
    return {};
  }

  // ---- blocks ----

  Slot& slotAt(int row, int position) {
    return grid[static_cast<size_t>(row)][static_cast<size_t>(position)];
  }

  void placeBlocks() {
    for (int row = 0; row < kGridRows; ++row)
      for (int column = 0; column < kGridCols; ++column) {
        Slot& slot = slotAt(row, column);
        slot.id = idFrom("pedaleira-import|" + table.sourceId() + "|" + foreign["name"].toString() +
                         "|" + juce::String(row) + "|" + juce::String(column));
        slot.sourceRow = row;
        slot.sourcePosition = column;
      }

    const auto* rows = foreign["rows"].getArray();
    for (const auto& rowVar : *rows) {
      const int row = readInt(rowVar["index"], -1);
      if (row < 0 || row >= kGridRows) {
        report.note("A row with index " + describeValue(rowVar["index"]) +
                    " does not fit the grid (rows 0 to " + juce::String(kGridRows - 1) +
                    "); its blocks were left out.");
        if (const auto* blocks = rowVar["blocks"].getArray())
          for (const auto& block : *blocks)
            report.unmapped.add(blockEntry(
                row, readInt(block["position"], -1), block["model"].toString(),
                {{"category", block["category"].toString()},
                 {"cause", "position"},
                 {"reason", "the row is outside the grid"}}));
        continue;
      }
      const auto* blocks = rowVar["blocks"].getArray();
      if (blocks == nullptr)
        continue;
      for (const auto& block : *blocks)
        placeOne(row, block);
    }
  }

  void placeOne(int row, const juce::var& block) {
    const int position = readInt(block["position"], -1);
    const juce::String model = block["model"].toString();
    const juce::String category = block["category"].toString();

    if (position < 0 || position >= kGridCols) {
      report.unmapped.add(blockEntry(row, position, model,
                                     {{"category", category},
                                      {"cause", "position"},
                                      {"reason", "position " + describeValue(block["position"]) +
                                                     " does not fit the " +
                                                     juce::String(kGridCols) + " slots of a row"}}));
      return;
    }
    Slot& slot = slotAt(row, position);
    if (slot.block || occupied.count({row, position}) != 0) {
      report.unmapped.add(blockEntry(row, position, model,
                                     {{"category", category},
                                      {"cause", "occupied"},
                                      {"reason", "another block already sits at this position"}}));
      return;
    }
    occupied.insert({row, position});

    const juce::var* modelEntry = table.model(model);
    const juce::var* categoryEntry = table.category(category);

    // A model the table names outright.
    if (modelEntry != nullptr) {
      const juce::String role = (*modelEntry)["role"].toString();
      const juce::String kindId = (*modelEntry)["kind"].toString();
      if (role.isNotEmpty() || kindId.isEmpty()) {
        reportUnmapped(row, position, model, category, role.isNotEmpty() ? "role" : "mapsToNothing",
                       role.isNotEmpty() ? role + ": " + kPickCaptureNote
                                         : "the table maps this model to nothing",
                       role);
        return;
      }
      const BlockKindInfo* info = BlockKindRegistry::instance().find(kindId);
      if (info == nullptr || !info->addable) {
        reportUnmapped(row, position, model, category, "unknownKind",
                       "the table maps it to \"" + kindId + "\", which this build does not have", {});
        return;
      }
      placeMapped(slot, row, position, block, *info, modelEntry);
      return;
    }

    // Otherwise the category decides.
    if (categoryEntry != nullptr) {
      const juce::String role = (*categoryEntry)["role"].toString();
      if (role.isNotEmpty()) {
        reportUnmapped(row, position, model, category, "role", role + ": " + kPickCaptureNote, role);
        return;
      }
      if ((*categoryEntry).isString() && (*categoryEntry).toString().isNotEmpty()) {
        const juce::String kindId = categoryEntry->toString();
        const BlockKindInfo* info = BlockKindRegistry::instance().find(kindId);
        if (info == nullptr || !info->addable) {
          reportUnmapped(row, position, model, category, "unknownKind",
                         "the table's fallback for \"" + category + "\" is \"" + kindId +
                             "\", which this build does not have",
                         {});
          return;
        }
        placeFallback(slot, row, position, block, category, *info);
        return;
      }
      reportUnmapped(row, position, model, category, "noFallback",
                     "no model entry, and the table has no fallback for \"" + category + "\"", {});
      return;
    }
    reportUnmapped(row, position, model, category, "notInTable",
                   "no entry for this model or its category in the mapping table", {});
  }

  void reportUnmapped(int row, int position, const juce::String& model,
                      const juce::String& category, const char* cause,
                      const juce::String& reason, const juce::String& role) {
    juce::DynamicObject::Ptr entry = new juce::DynamicObject();
    entry->setProperty("row", row);
    entry->setProperty("position", position);
    entry->setProperty("model", model);
    entry->setProperty("category", category);
    entry->setProperty("cause", juce::String(cause));
    entry->setProperty("reason", reason);
    if (role.isNotEmpty())
      entry->setProperty("role", role);
    report.unmapped.add(juce::var(entry.get()));
  }

  void fillSlot(Slot& slot, const BlockKindInfo& info, const juce::var& block) {
    slot.block = true;
    slot.info = &info;
    slot.kindId = info.id;
    slot.kindName = info.name;
    slot.mix = info.defaultMix;
    slot.enabled = block["enabled"].isVoid() ? true : static_cast<bool>(block["enabled"]);
    slot.sourceModel = block["model"].toString();
    slot.id = idFrom("pedaleira-import|" + table.sourceId() + "|" + foreign["name"].toString() +
                     "|" + juce::String(slot.sourceRow) + "|" + juce::String(slot.sourcePosition) +
                     "|" + info.id);
    for (const auto& spec : info.params)
      slot.values[spec.id] = spec.defaultValue;
  }

  void placeFallback(Slot& slot, int row, int position, const juce::var& block,
                     const juce::String& category, const BlockKindInfo& info) {
    // No parameters are read for a block we only approximate: the source's
    // values are for a model we do not have.
    const juce::String model = block["model"].toString();
    fillSlot(slot, info, block);
    report.approximated.add(blockEntry(
        row, position, model,
        {{"type", "block"},
         {"category", category},
         {"kind", info.id},
         {"reason", "no entry for this model; placed " + info.name +
                        " at its default settings as the fallback for \"" + category + "\""}}));
  }

  void placeMapped(Slot& slot, int row, int position, const juce::var& block,
                   const BlockKindInfo& info, const juce::var* modelEntry) {
    fillSlot(slot, info, block);
    slot.modelEntry = modelEntry;
    const juce::String model = block["model"].toString();

    // Defaults the table gives for this model, before any parameter.
    if (const auto* defaults = (*modelEntry)["defaults"].getDynamicObject()) {
      for (const auto& property : defaults->getProperties()) {
        const juce::String ourId = property.name.toString();
        const juce::var pseudo = makeObject({{"param", ourId}});
        const Resolved resolved = resolveValue(pseudo, property.value, info);
        if (!resolved.ok) {
          report.note(model + " (row " + juce::String(row) + ", position " + juce::String(position) +
                      "): default for " + ourId + " skipped: " + resolved.why);
          continue;
        }
        apply(slot, resolved);
      }
    }

    // Each parameter of the source.
    if (const auto* params = block["params"].getDynamicObject()) {
      for (const auto& property : params->getProperties()) {
        const juce::String theirs = property.name.toString();
        const juce::var value = paramValueOf(property.value);
        const juce::var* entry = findParamEntry(*modelEntry, theirs);
        if (entry == nullptr) {
          report.droppedParams.add(blockEntry(
              row, position, model,
              {{"param", theirs},
               {"value", value},
               {"reason", "the mapping table has no entry for this parameter"}}));
          continue;
        }
        // A table entry may say which unit it was written for; a dump that
        // gives another one is not converted by a rule made for this one.
        const juce::String expectedUnit = (*entry)["unit"].toString().trim().toLowerCase();
        const juce::String givenUnit =
            property.value.getDynamicObject() != nullptr
                ? property.value["unit"].toString().trim().toLowerCase()
                : juce::String();
        if (expectedUnit.isNotEmpty() && givenUnit.isNotEmpty() && expectedUnit != givenUnit) {
          report.droppedParams.add(blockEntry(
              row, position, model,
              {{"param", theirs},
               {"value", value},
               {"unit", property.value["unit"].toString()},
               {"reason", "the table expects " + (*entry)["unit"].toString() + " and the file says " +
                              property.value["unit"].toString()}}));
          continue;
        }
        const Resolved resolved = resolveValue(*entry, value, info);
        if (!resolved.ok) {
          report.droppedParams.add(
              blockEntry(row, position, model,
                         {{"param", theirs}, {"value", value}, {"reason", resolved.why}}));
          continue;
        }
        if (resolved.clamped)
          report.approximated.add(blockEntry(
              row, position, model,
              {{"type", "param"},
               {"param", theirs},
               {"ourParam", resolved.ourId},
               {"from", resolved.raw},
               {"to", resolved.value},
               {"reason", "out of range for " + info.id + "." + resolved.ourId +
                              "; clamped"}}));
        apply(slot, resolved);
      }
    }

    juce::DynamicObject::Ptr entryObject = new juce::DynamicObject();
    entryObject->setProperty("row", row);
    entryObject->setProperty("position", position);
    entryObject->setProperty("model", model);
    entryObject->setProperty("category", block["category"].toString());
    entryObject->setProperty("kind", info.id);
    entryObject->setProperty("verified", static_cast<bool>((*modelEntry)["verified"]));
    if ((*modelEntry)["notes"].toString().isNotEmpty())
      entryObject->setProperty("notes", (*modelEntry)["notes"].toString());
    report.mapped.add(juce::var(entryObject.get()));
  }

  void apply(Slot& slot, const Resolved& resolved) {
    switch (resolved.target.type) {
      case TargetType::kind: slot.values[resolved.ourId] = resolved.value; break;
      case TargetType::mix: slot.mix = resolved.value; break;
      case TargetType::inputGain: slot.inputGain = resolved.value; break;
      case TargetType::outputGain: slot.outputGain = resolved.value; break;
      case TargetType::enabled: slot.enabled = resolved.value >= 0.5f; break;
    }
  }

  // ---- routing ----

  void planRouting() {
    const juce::var routing = foreign["routing"];
    const auto* rows = routing["rows"].getArray();
    if (rows == nullptr || rows->isEmpty()) {
      report.note(kGeneralInputNote);
      plans[0].inputType = "device";
      plans[0].outputType = "device";
    } else {
      for (const auto& rowVar : *rows) {
        const int row = readInt(rowVar["index"], -1);
        if (row < 0 || row >= kGridRows)
          continue;
        planRow(row, rowVar);
      }
    }
    planSplits(routing["splits"]);
    validateFeeds();
  }

  void planRow(int row, const juce::var& rowVar) {
    RowPlan& plan = plans[static_cast<size_t>(row)];
    const juce::var input = rowVar["input"];
    const juce::String inputType = input["type"].toString();
    const juce::String inputName = input["name"].toString();
    if (inputType == "device") {
      const int channel = juce::jlimit(1, kMaxIoChannels, readInt(input["channel"], 1));
      const bool stereo = static_cast<bool>(input["stereo"]);
      plan.inputType = "device";
      plan.inputStereo = stereo && channel < kMaxIoChannels;
      plan.inputChannel = juce::jmin(channel - 1, kMaxIoChannels - (plan.inputStereo ? 2 : 1));
    } else if (inputType == "other") {
      report.note("Row " + juce::String(row) + ": the input \"" + inputName +
                  "\" has no counterpart here; the row has no input.");
    }
    const juce::var output = rowVar["output"];
    const juce::String outputType = output["type"].toString();
    const juce::String outputName = output["name"].toString();
    if (outputType == "device") {
      plan.outputType = "device";
      plan.outputPair = juce::jlimit(1, kMaxIoPairs, readInt(output["pair"], 1)) - 1;
    } else if (outputType == "row") {
      plan.outputType = "row";
      plan.outputToRow = row + 1;
    } else if (outputType == "other") {
      plan.outputType = "device";
      plan.outputPair = 0;
      report.note("Row " + juce::String(row) + ": the output \"" + outputName +
                  "\" has no counterpart here; it goes to outputs 1/2.");
    }
  }

  void planSplits(const juce::var& splitsVar) {
    const auto* splits = splitsVar.getArray();
    if (splits == nullptr)
      return;
    std::set<int> seen;
    for (const auto& splitVar : *splits) {
      const int upper = readInt(splitVar["row"], -1);
      const int splitAt = readInt(splitVar["splitAt"], -1);
      const int mixAt = splitVar["mixAt"].isVoid() ? -1 : readInt(splitVar["mixAt"], -2);
      const juce::String label =
          "The split of row " + describeValue(splitVar["row"]) + " at " +
          describeValue(splitVar["splitAt"]);
      if (upper < 0 || upper >= kGridRows - 1) {
        report.note(label + " cannot be placed: a row splits into the row directly below it, so "
                            "the upper row is 0 to " + juce::String(kGridRows - 2) + ". Flattened.");
        continue;
      }
      if (splitAt < 0 || splitAt >= kGridCols) {
        report.note(label + " cannot be placed: the split point is 0 to " +
                    juce::String(kGridCols - 1) + ". Flattened.");
        continue;
      }
      if (mixAt == -2 || (mixAt >= 0 && (mixAt <= splitAt || mixAt > kGridCols))) {
        report.note(label + " cannot be placed: the rejoin point must lie after the split point, "
                            "up to " + juce::String(kGridCols) + ". Flattened.");
        continue;
      }
      if (seen.count(upper) != 0) {
        report.note(label + " dropped: a row can have only one split.");
        continue;
      }
      // A split nested in another has to lie inside its range.
      bool nestedOutside = false;
      for (const auto& parent : splitPlans)
        if (parent.upperRow == upper - 1 && parent.mixAt >= 0 &&
            (splitAt < parent.splitAt || (mixAt < 0 ? kGridCols : mixAt) > parent.mixAt))
          nestedOutside = true;
      if (nestedOutside) {
        report.note(label + " dropped: it does not lie inside the split above it.");
        continue;
      }
      seen.insert(upper);
      splitPlans.push_back({upper, splitAt, mixAt});
      RowPlan& lower = plans[static_cast<size_t>(upper) + 1];
      if (lower.inputType == "device")
        report.note("Row " + juce::String(upper + 1) + " has an input of its own and sits below a "
                    "split; the split feeds it, so the input was dropped.");
      lower.inputType = "none";
      if (mixAt >= 0)
        lower.outputType = "none";  // its signal joins the upper row
    }
    // Rows that hold blocks in the lane of a split that was dropped are
    // simply independent rows now.
    if (!splitPlans.empty())
      report.note("Split and rejoin points are taken as the boundaries between slots (0 is before "
                  "the first slot); check them against the original.");
  }

  void validateFeeds() {
    std::set<int> lowerRows;
    for (const auto& split : splitPlans)
      lowerRows.insert(split.upperRow + 1);
    for (int row = 0; row < kGridRows; ++row) {
      RowPlan& plan = plans[static_cast<size_t>(row)];
      if (plan.outputType != "row")
        continue;
      const int target = plan.outputToRow;
      const bool valid = target > row && target < kGridRows &&
                         plans[static_cast<size_t>(target)].inputType == "none" &&
                         lowerRows.count(target) == 0;
      if (!valid) {
        plan.outputType = "device";
        plan.outputPair = 0;
        plan.outputToRow = -1;
        report.note("Row " + juce::String(row) + " feeds the row below in the original, which this "
                    "grid cannot do here; it goes to outputs 1/2 instead.");
      }
    }
  }

  // ---- scenes ----

  void carryScenes() {
    const auto* scenes = foreign["scenes"].getArray();
    if (scenes == nullptr)
      return;
    int enabledCarried = 0, valuesCarried = 0, notCarried = 0;
    for (int index = 0; index < scenes->size() && index < kNumScenes; ++index) {
      const juce::var scene = scenes->getReference(index);
      const juce::String sceneName = scene["name"].toString().trim();
      if (sceneName.isNotEmpty() && sceneName != "Scene " + sceneLetter(index))
        sceneNames[static_cast<size_t>(index)] = sceneName;

      if (const auto* enabled = scene["enabled"].getDynamicObject()) {
        for (const auto& property : enabled->getProperties()) {
          Slot* slot = slotFor(property.name.toString());
          if (slot == nullptr || !slot->block) {
            ++notCarried;
            continue;
          }
          slot->sceneEnabled[static_cast<size_t>(index)] = static_cast<bool>(property.value);
          ++enabledCarried;
        }
      }
      if (const auto* params = scene["params"].getDynamicObject()) {
        for (const auto& blockProperty : params->getProperties()) {
          Slot* slot = slotFor(blockProperty.name.toString());
          const auto* values = blockProperty.value.getDynamicObject();
          if (values == nullptr)
            continue;
          for (const auto& valueProperty : values->getProperties()) {
            if (carrySceneValue(slot, index, valueProperty.name.toString(), valueProperty.value))
              ++valuesCarried;
            else
              ++notCarried;
          }
        }
      }
    }
    if (valuesCarried + enabledCarried > 0)
      report.note("Scenes carried: " + juce::String(enabledCarried) + " block on/off states and " +
                  juce::String(valuesCarried) + " parameter values.");
    if (notCarried > 0)
      report.note("Scenes not carried: " + juce::String(notCarried) +
                  " entries (blocks or parameters that were not mapped, or values that cannot follow "
                  "scenes here). See droppedParams for the parameters.");
  }

  // "row:position" to its slot.
  Slot* slotFor(const juce::String& key) {
    const int colon = key.indexOfChar(':');
    if (colon <= 0)
      return nullptr;
    const int row = key.substring(0, colon).getIntValue();
    const int position = key.substring(colon + 1).getIntValue();
    if (row < 0 || row >= kGridRows || position < 0 || position >= kGridCols)
      return nullptr;
    return &slotAt(row, position);
  }

  bool carrySceneValue(Slot* slot, int scene, const juce::String& theirs, const juce::var& value) {
    if (slot == nullptr || !slot->block || slot->modelEntry == nullptr)
      return false;
    const juce::String model = slot->sourceModel;
    const juce::var* entry = findParamEntry(*slot->modelEntry, theirs);
    if (entry == nullptr) {
      report.droppedParams.add(blockEntry(
          slot->sourceRow, slot->sourcePosition, model,
          {{"param", theirs},
           {"value", value},
           {"scene", scene},
           {"reason", "scene value: the mapping table has no entry for this parameter"}}));
      return false;
    }
    const Resolved resolved = resolveValue(*entry, value, *slot->info);
    if (!resolved.ok) {
      report.droppedParams.add(blockEntry(slot->sourceRow, slot->sourcePosition, model,
                                          {{"param", theirs},
                                           {"value", value},
                                           {"scene", scene},
                                           {"reason", "scene value: " + resolved.why}}));
      return false;
    }
    if (resolved.target.type == TargetType::enabled ||
        (resolved.target.type == TargetType::kind &&
         slot->info->params[static_cast<size_t>(resolved.target.index)].affectsLatency)) {
      report.droppedParams.add(blockEntry(
          slot->sourceRow, slot->sourcePosition, model,
          {{"param", theirs},
           {"value", value},
           {"scene", scene},
           {"reason", "scene value: " + resolved.ourId +
                          " cannot follow scenes (it is a block switch or changes latency)"}}));
      return false;
    }
    SceneValue* found = nullptr;
    for (auto& existing : slot->sceneValues)
      if (existing.paramId == resolved.ourId)
        found = &existing;
    if (found == nullptr) {
      slot->sceneValues.push_back({});
      found = &slot->sceneValues.back();
      found->paramId = resolved.ourId;
      found->type = resolved.target.type;
    }
    found->values[static_cast<size_t>(scene)] = resolved.value;
    found->given[static_cast<size_t>(scene)] = true;
    return true;
  }

  // The current value of a slot's parameter, by target.
  float baseValue(const Slot& slot, const SceneValue& scene) const {
    switch (scene.type) {
      case TargetType::mix: return slot.mix;
      case TargetType::inputGain: return slot.inputGain;
      case TargetType::outputGain: return slot.outputGain;
      case TargetType::enabled: return slot.enabled ? 1.0f : 0.0f;
      case TargetType::kind: break;
    }
    const auto it = slot.values.find(scene.paramId);
    return it == slot.values.end() ? 0.0f : it->second;
  }

  // ---- the tree ----

  juce::ValueTree buildScenesNode() {
    struct ParamPlan {
      const Slot* slot;
      juce::String paramId;
      std::array<float, kNumScenes> values;
    };
    std::vector<const Slot*> toggled;
    std::vector<ParamPlan> followed;
    for (int row = 0; row < kGridRows; ++row)
      for (int column = 0; column < kGridCols; ++column) {
        const Slot& slot = slotAt(row, column);
        if (!slot.block)
          continue;
        bool differs = false;
        for (int s = 0; s < kNumScenes; ++s)
          if (slot.sceneEnabled[static_cast<size_t>(s)].value_or(slot.enabled) != slot.enabled)
            differs = true;
        if (differs)
          toggled.push_back(&slot);
        for (const auto& scene : slot.sceneValues) {
          ParamPlan plan{&slot, scene.paramId, {}};
          const float base = baseValue(slot, scene);
          bool varies = false;
          for (int s = 0; s < kNumScenes; ++s) {
            const auto index = static_cast<size_t>(s);
            plan.values[index] = scene.given[index] ? scene.values[index] : base;
            if (std::abs(plan.values[index] - base) > 1.0e-6f)
              varies = true;
          }
          if (varies)
            followed.push_back(plan);
        }
      }

    bool named = false;
    for (const auto& text : sceneNames)
      named = named || text.isNotEmpty();
    if (toggled.empty() && followed.empty() && !named && activeScene == 0)
      return {};

    juce::ValueTree node("Scenes");
    node.setProperty("version", 1, nullptr);
    node.setProperty("active", activeScene, nullptr);
    for (int s = 0; s < kNumScenes; ++s) {
      juce::ValueTree scene("Scene");
      scene.setProperty("index", s, nullptr);
      if (sceneNames[static_cast<size_t>(s)].isNotEmpty())
        scene.setProperty("name", sceneNames[static_cast<size_t>(s)], nullptr);
      for (const Slot* slot : toggled) {
        juce::ValueTree block("Block");
        block.setProperty("id", slot->id, nullptr);
        block.setProperty("enabled",
                          slot->sceneEnabled[static_cast<size_t>(s)].value_or(slot->enabled),
                          nullptr);
        scene.appendChild(block, nullptr);
      }
      for (const auto& plan : followed) {
        juce::ValueTree value("Value");
        value.setProperty("blockId", plan.slot->id, nullptr);
        value.setProperty("param", plan.paramId, nullptr);
        value.setProperty("value", static_cast<double>(plan.values[static_cast<size_t>(s)]),
                          nullptr);
        scene.appendChild(value, nullptr);
      }
      node.appendChild(scene, nullptr);
    }
    return node;
  }

  // What the block holds while the active scene is the one playing.
  void activeSceneValues(const Slot& slot, bool& enabled, float& mix, float& inputGain,
                         float& outputGain, std::map<juce::String, float>& values) const {
    enabled = slot.sceneEnabled[static_cast<size_t>(activeScene)].value_or(slot.enabled);
    mix = slot.mix;
    inputGain = slot.inputGain;
    outputGain = slot.outputGain;
    values = slot.values;
    for (const auto& scene : slot.sceneValues) {
      if (!scene.given[static_cast<size_t>(activeScene)])
        continue;
      const float value = scene.values[static_cast<size_t>(activeScene)];
      switch (scene.type) {
        case TargetType::mix: mix = value; break;
        case TargetType::inputGain: inputGain = value; break;
        case TargetType::outputGain: outputGain = value; break;
        case TargetType::enabled: break;
        case TargetType::kind: values[scene.paramId] = value; break;
      }
    }
  }

  juce::ValueTree buildGridNode() {
    bool joins = !splitPlans.empty();
    for (const auto& plan : plans)
      joins = joins || plan.outputType == "row";

    juce::ValueTree node("Grid");
    node.setProperty("version", joins ? 2 : 1, nullptr);
    node.setProperty("minReader", joins ? 2 : 1, nullptr);
    node.setProperty("rows", kGridRows, nullptr);
    node.setProperty("cols", kGridCols, nullptr);
    node.setProperty("legacyControls", "", nullptr);

    for (int row = 0; row < kGridRows; ++row) {
      const RowPlan& plan = plans[static_cast<size_t>(row)];
      juce::ValueTree rowNode("Row");
      rowNode.setProperty("index", row, nullptr);
      rowNode.setProperty("format", "stereo", nullptr);

      juce::ValueTree input("Input");
      input.setProperty("type", plan.inputType, nullptr);
      input.setProperty("channel", plan.inputChannel, nullptr);
      input.setProperty("stereo", plan.inputStereo, nullptr);
      input.setProperty("fold", "none", nullptr);
      input.setProperty("trimDb", 0.0, nullptr);
      input.setProperty("gate", true, nullptr);
      input.setProperty("pitch", false, nullptr);
      rowNode.appendChild(input, nullptr);

      juce::ValueTree output("Output");
      output.setProperty("type", plan.outputType, nullptr);
      output.setProperty("pair", plan.outputPair, nullptr);
      if (plan.outputType == "row")
        output.setProperty("toRow", plan.outputToRow, nullptr);
      output.setProperty("volumeDb", 0.0, nullptr);
      output.setProperty("pan", 0.5, nullptr);
      output.setProperty("mute", false, nullptr);
      output.setProperty("solo", false, nullptr);
      output.setProperty("invert", false, nullptr);
      rowNode.appendChild(output, nullptr);

      juce::ValueTree slots("Slots");
      for (int column = 0; column < kGridCols; ++column) {
        const Slot& slot = slotAt(row, column);
        bool enabled = true;
        float mix = 1.0f, inputGain = 0.5f, outputGain = 0.5f;
        std::map<juce::String, float> values;
        if (slot.block)
          activeSceneValues(slot, enabled, mix, inputGain, outputGain, values);
        Slot shown = slot;
        shown.inputGain = inputGain;
        shown.outputGain = outputGain;
        slots.appendChild(makeBlockNode(shown, enabled, mix, values), nullptr);
      }
      rowNode.appendChild(slots, nullptr);
      node.appendChild(rowNode, nullptr);
    }

    for (const auto& split : splitPlans) {
      SplitJunction junction;
      junction.id = idFrom("pedaleira-import|" + table.sourceId() + "|" + foreign["name"].toString() +
                           "|split|" + juce::String(split.upperRow))
                        .toStdString();
      junction.upperRow = split.upperRow;
      junction.splitCol = split.splitAt;
      junction.mixCol = split.mixAt;
      junction.type = SplitterType::AB;
      node.appendChild(junction.getState(), nullptr);
    }
    return node;
  }

  juce::ValueTree buildPresetTree() {
    juce::ValueTree preset(presetfile::kTag);
    preset.setProperty("schemaVersion", 2, nullptr);
    preset.setProperty("name", name, nullptr);
    if (isNumber(foreign["tempo"]) && std::isfinite(numberOf(foreign["tempo"])) &&
        numberOf(foreign["tempo"]) > 0.0)
      preset.setProperty("tempo", numberOf(foreign["tempo"]), nullptr);
    preset.setProperty("importedFrom", table.sourceId(), nullptr);

    juce::ValueTree snapshot("ChainSnapshot");
    snapshot.setProperty("layout", "grid", nullptr);
    snapshot.appendChild(buildGridNode(), nullptr);
    if (juce::ValueTree scenes = buildScenesNode(); scenes.isValid())
      snapshot.appendChild(scenes, nullptr);
    preset.appendChild(snapshot, nullptr);
    preset.appendChild(juce::ValueTree("Params"), nullptr);
    return preset;
  }

  const juce::var& foreign;
  const MappingTable& table;
  Report report;
  juce::String name;
  int activeScene{0};
  std::array<std::array<Slot, kGridCols>, kGridRows> grid;
  std::array<RowPlan, kGridRows> plans;
  std::vector<SplitPlan> splitPlans;
  std::set<std::pair<int, int>> occupied;
  std::array<juce::String, kNumScenes> sceneNames;
};

}  // namespace

juce::String checkForeignPreset(const juce::var& foreign) {
  if (foreign.getDynamicObject() == nullptr)
    return "not a foreign preset: the file is not a JSON object";
  const juce::String format = foreign["format"].toString();
  if (format != kForeignFormat)
    return "not a foreign preset: \"format\" is " +
           (format.isEmpty() ? juce::String("missing") : "\"" + format + "\"") + ", expected \"" +
           kForeignFormat + "\"";
  if (!isNumber(foreign["version"]))
    return "the foreign preset has no \"version\"";
  const int version = readInt(foreign["version"], 0);
  if (version < 1)
    return "the foreign preset has an invalid \"version\"";
  if (version > kForeignVersion)
    return "the foreign preset is version " + juce::String(version) +
           " and this build reads up to " + juce::String(kForeignVersion);
  if (foreign["source"].toString().isEmpty())
    return "the foreign preset has no \"source\"";
  if (!foreign["rows"].isArray())
    return "the foreign preset has no \"rows\" array";
  return {};
}

ImportResult convertForeignPreset(const juce::var& foreign, const juce::var& mapping) {
  ImportResult result;
  if (const juce::String problem = checkForeignPreset(foreign); problem.isNotEmpty()) {
    result.error = problem;
    return result;
  }
  MappingTable table;
  if (const juce::String problem = table.load(mapping); problem.isNotEmpty()) {
    result.error = problem;
    return result;
  }
  return Importer(foreign, table).run();
}

juce::var builtInMapping(const juce::String& source) {
  for (int i = 0; i < PedaleiraImportData::namedResourceListSize; ++i) {
    const juce::String file = PedaleiraImportData::originalFilenames[i];
    if (file.equalsIgnoreCase(source + ".json")) {
      int size = 0;
      const char* data = PedaleiraImportData::getNamedResource(
          PedaleiraImportData::namedResourceList[i], size);
      juce::var parsed;
      if (data != nullptr && juce::JSON::parse(juce::String::fromUTF8(data, size), parsed).wasOk())
        return parsed;
    }
  }
  return {};
}

juce::StringArray builtInMappingSources() {
  juce::StringArray sources;
  for (int i = 0; i < PedaleiraImportData::namedResourceListSize; ++i)
    sources.add(juce::File(PedaleiraImportData::originalFilenames[i]).getFileNameWithoutExtension());
  sources.sort(true);
  return sources;
}

juce::var readJsonFile(const juce::File& file, juce::String& error) {
  juce::var parsed;
  if (const juce::Result result = presetjson::readJsonFile(file, kMaxImportFileBytes, parsed);
      result.failed()) {
    error = result.getErrorMessage();
    return {};
  }
  return parsed;
}

}  // namespace t3k::importer

// The amp description loader: JSON text to an AmpDescription, with checks.
//
// Not for the audio thread (it allocates). Every field is checked for its
// type and a sane range, unknown fields are errors (a misspelt field would
// otherwise silently keep its default), and every error names the path of the
// field. The format is documented in docs/amp-framework.md.
#include "amp/AmpDescription.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <stdexcept>
#include <string>

#include "json.hpp"

namespace amp {

const char* panelIdName(PanelId id) noexcept {
  switch (id) {
    case PanelId::Gain: return "gain";
    case PanelId::Bass: return "bass";
    case PanelId::Middle: return "middle";
    case PanelId::Treble: return "treble";
    case PanelId::Presence: return "presence";
    case PanelId::Depth: return "depth";
    case PanelId::Master: return "master";
    case PanelId::Bright: return "bright";
    case PanelId::Voicing: return "voicing";
  }
  return "";
}

const char* ampFamilyId(AmpFamily family) noexcept {
  switch (family) {
    case AmpFamily::None: return "";
    case AmpFamily::HighGain: return "high_gain";
    case AmpFamily::Clean: return "clean";
    case AmpFamily::Crunch: return "crunch";
  }
  return "";
}

const char* ampFamilyName(AmpFamily family) noexcept {
  switch (family) {
    case AmpFamily::None: return "";
    case AmpFamily::HighGain: return "High gain";
    case AmpFamily::Clean: return "Clean";
    case AmpFamily::Crunch: return "Crunch";
  }
  return "";
}

namespace {
constexpr const char* kNameplateIds[] = {"left", "right", "top", "bottom"};
constexpr const char* kKnobCapIds[] = {"round", "pointer", "chicken_head", "fader"};
constexpr const char* kStripIds[] = {"none",   "slash",    "dots",  "wave",  "cells", "zigzag", "chevron", "bells",
                                     "arcs",   "crosses",  "ticks", "flames", "rings", "steps",  "diamonds", "ripple"};
constexpr const char* kFamilyIds[] = {"high_gain", "clean", "crunch"};

template <size_t N>
const char* idOf(const char* const (&table)[N], int index) noexcept {
  return index >= 0 && static_cast<size_t>(index) < N ? table[index] : "";
}
}  // namespace

const char* nameplateId(Nameplate value) noexcept { return idOf(kNameplateIds, static_cast<int>(value)); }
const char* knobCapId(KnobCap value) noexcept { return idOf(kKnobCapIds, static_cast<int>(value)); }
const char* stripPatternId(StripPattern value) noexcept { return idOf(kStripIds, static_cast<int>(value)); }

namespace {

using json = nlohmann::json;

struct ParseError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

std::string child(const std::string& path, const std::string& key) {
  return path.empty() ? key : path + "." + key;
}
std::string element(const std::string& path, size_t index) { return path + "[" + std::to_string(index) + "]"; }

[[noreturn]] void fail(const std::string& path, const std::string& message) {
  throw ParseError((path.empty() ? std::string("description") : path) + ": " + message);
}

struct TubeEntry {
  const char* name;
  const char* alias;
  float mu;
  float rp;
};
// Small-signal constants at a typical operating point (manufacturer
// datasheets: RCA / GE / Philips 250 V plate characteristics).
constexpr TubeEntry kTubes[] = {
    {"12AX7", "ECC83", 100.0f, 62.5e3f},
    {"12AT7", "ECC81", 60.0f, 10.9e3f},
    {"12AU7", "ECC82", 17.0f, 7.7e3f},
};

class Reader {
public:
  explicit Reader(AmpDescription& target) : out(target) {}

  // ---- Generic access -----------------------------------------------------

  static void allowOnly(const json& obj, const std::string& path, std::initializer_list<const char*> keys) {
    for (auto it = obj.begin(); it != obj.end(); ++it) {
      bool known = false;
      for (const char* key : keys)
        known = known || it.key() == key;
      if (!known)
        fail(child(path, it.key()), "unknown field");
    }
  }

  static const json* find(const json& obj, const char* key) {
    const auto it = obj.find(key);
    return it == obj.end() ? nullptr : &*it;
  }

  const json& objectField(const json& obj, const std::string& path, const char* key, bool required, const json& absent) {
    const json* value = find(obj, key);
    if (value == nullptr) {
      if (required)
        fail(child(path, key), "missing");
      return absent;
    }
    if (!value->is_object())
      fail(child(path, key), "must be an object");
    return *value;
  }

  static bool readString(const json& obj, const std::string& path, const char* key, std::string& value,
                         bool required) {
    const json* v = find(obj, key);
    if (v == nullptr) {
      if (required)
        fail(child(path, key), "missing");
      return false;
    }
    if (!v->is_string())
      fail(child(path, key), "must be a string");
    value = v->get<std::string>();
    return true;
  }

  static bool readBool(const json& obj, const std::string& path, const char* key, bool& value) {
    const json* v = find(obj, key);
    if (v == nullptr)
      return false;
    if (!v->is_boolean())
      fail(child(path, key), "must be true or false");
    value = v->get<bool>();
    return true;
  }

  /** A number, or {"value": n, "provenance": "...", "note": "..."}. */
  bool readNumber(const json& obj, const std::string& path, const char* key, double& value, Provenance defaultSource,
                  double lo, double hi, bool required = false) {
    const json* v = find(obj, key);
    const std::string where = child(path, key);
    if (v == nullptr) {
      if (required)
        fail(where, "missing");
      return false;
    }
    Provenance source = defaultSource;
    std::string note;
    bool explicitForm = false;
    double number = 0.0;
    if (v->is_number()) {
      number = v->get<double>();
    } else if (v->is_object()) {
      allowOnly(*v, where, {"value", "provenance", "note"});
      const json* inner = find(*v, "value");
      if (inner == nullptr || !inner->is_number())
        fail(where + ".value", "must be a number");
      number = inner->get<double>();
      explicitForm = true;
      if (const json* p = find(*v, "provenance")) {
        if (!p->is_string())
          fail(where + ".provenance", "must be \"schematic\", \"measured\" or \"fitted\"");
        const std::string name = p->get<std::string>();
        if (name == "schematic") source = Provenance::Schematic;
        else if (name == "measured") source = Provenance::Measured;
        else if (name == "fitted") source = Provenance::Fitted;
        else fail(where + ".provenance", "must be \"schematic\", \"measured\" or \"fitted\", not \"" + name + "\"");
      }
      readString(*v, where, "note", note, false);
    } else {
      fail(where, "must be a number");
    }
    if (!std::isfinite(number) || number < lo || number > hi)
      fail(where, "must be between " + format(lo) + " and " + format(hi) + ", not " + format(number));
    value = number;
    if (explicitForm || source == Provenance::Fitted)
      out.notes.push_back({where, source, note, number});
    return true;
  }

  void readFloat(const json& obj, const std::string& path, const char* key, float& value, Provenance source, double lo,
                 double hi, bool required = false) {
    double v = value;
    if (readNumber(obj, path, key, v, source, lo, hi, required))
      value = static_cast<float>(v);
  }

  static std::string format(double v) {
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%g", v);
    return buffer;
  }

  // ---- Sections -----------------------------------------------------------

  void readAll(const json& root) {
    if (!root.is_object())
      fail("", "the top level must be an object");
    allowOnly(root, "", {"$schema", "format", "version", "metadata", "model", "input", "stages", "voicings",
                         "default_voicing", "tone_stack", "effects_loop", "power_amp", "calibration", "panel",
                         "default_cab"});

    std::string format;
    readString(root, "", "format", format, true);
    if (format != "pedaleira-amp")
      fail("format", "must be \"pedaleira-amp\", not \"" + format + "\"");
    const json* version = find(root, "version");
    if (version == nullptr || !version->is_number_integer())
      fail("version", "missing or not an integer");
    out.version = version->get<int>();
    if (out.version != kAmpDescriptionVersion)
      fail("version", "this build reads version " + std::to_string(kAmpDescriptionVersion) + ", the file is version " +
                           std::to_string(out.version));

    const json none = json::object();
    readMetadata(objectField(root, "", "metadata", true, none));
    readModel(objectField(root, "", "model", false, none));
    readInput(objectField(root, "", "input", false, none));
    const json* stages = find(root, "stages");
    if (stages == nullptr || !stages->is_array() || stages->empty())
      fail("stages", "must be a non-empty array");
    if (stages->size() > static_cast<size_t>(kMaxStages))
      fail("stages", "at most " + std::to_string(kMaxStages) + " stages");
    out.numStages = static_cast<int>(stages->size());
    for (size_t i = 0; i < stages->size(); ++i)
      readStage((*stages)[i], i);
    readTone(objectField(root, "", "tone_stack", true, none));
    readEffectsLoop(objectField(root, "", "effects_loop", false, none));
    readPower(objectField(root, "", "power_amp", true, none));
    readCalibration(objectField(root, "", "calibration", false, none));
    readVoicings(find(root, "voicings"));
    const json* defaultVoicing = find(root, "default_voicing");
    if (defaultVoicing != nullptr) {
      if (!defaultVoicing->is_number_integer())
        fail("default_voicing", "must be an integer");
      out.defaultVoicing = defaultVoicing->get<int>();
    }
    if (out.defaultVoicing < 0 || out.defaultVoicing >= out.numVoicings)
      fail("default_voicing", "must index one of the voicings");
    readPanel(find(root, "panel"));
    readDefaultCab(find(root, "default_cab"));
  }

  /** The cab the amp is meant for (DefaultCab): checked like the cab.model
      block's controls, kept as given. */
  void readDefaultCab(const json* cab) {
    if (cab == nullptr)
      return;
    if (!cab->is_object())
      fail("default_cab", "must be an object");
    const std::string path = "default_cab";
    allowOnly(*cab, path, {"speaker", "size", "back", "mic", "position_percent", "distance_cm", "angle_deg",
                           "room_percent", "low_cut_hz", "high_cut_hz"});
    DefaultCab& target = out.defaultCab;
    target = DefaultCab{};
    target.present = true;
    readString(*cab, path, "speaker", target.speaker, true);
    if (target.speaker.empty())
      fail(child(path, "speaker"), "must not be empty");
    const auto readChoice = [&](const char* key, std::string& value, std::initializer_list<const char*> allowed) {
      if (!readString(*cab, path, key, value, false))
        return;
      std::string list;
      for (const char* option : allowed) {
        if (value == option)
          return;
        list += list.empty() ? "" : ", ";
        list += option;
      }
      fail(child(path, key), "must be one of " + list + ", not \"" + value + "\"");
    };
    readChoice("size", target.size, {"1x12", "2x12", "4x12", "1x10", "2x10"});
    readChoice("back", target.back, {"closed", "open"});
    readChoice("mic", target.mic, {"dynamic", "ribbon", "condenser"});
    // Settings, not parts: no provenance note is kept for them.
    const auto readSetting = [&](const char* key, float& value, double lo, double hi) {
      double v = value;
      const size_t notesBefore = out.notes.size();
      if (readNumber(*cab, path, key, v, Provenance::Schematic, lo, hi))
        value = static_cast<float>(v);
      out.notes.resize(notesBefore);
    };
    readSetting("position_percent", target.positionPercent, 0.0, 100.0);
    readSetting("distance_cm", target.distanceCm, 0.5, 100.0);
    readSetting("angle_deg", target.angleDeg, 0.0, 60.0);
    readSetting("room_percent", target.roomPercent, 0.0, 100.0);
    readSetting("low_cut_hz", target.lowCutHz, 20.0, 400.0);
    readSetting("high_cut_hz", target.highCutHz, 2000.0, 20000.0);
  }

  void readMetadata(const json& obj) {
    allowOnly(obj, "metadata", {"name", "author", "licence", "summary", "family", "references"});
    readString(obj, "metadata", "name", out.name, true);
    if (out.name.empty())
      fail("metadata.name", "must not be empty");
    readString(obj, "metadata", "author", out.author, false);
    readString(obj, "metadata", "licence", out.licence, true);
    if (out.licence.empty())
      fail("metadata.licence", "must not be empty: say which licence this description is under");
    readString(obj, "metadata", "summary", out.summary, false);
    std::string family;
    if (readString(obj, "metadata", "family", family, false)) {
      int found = -1;
      for (int i = 0; i < 3; ++i)
        if (family == kFamilyIds[i])
          found = i;
      if (found < 0)
        fail("metadata.family", "must be \"high_gain\", \"clean\" or \"crunch\", not \"" + family + "\"");
      out.family = static_cast<AmpFamily>(found + 1);
    }
    if (const json* refs = find(obj, "references")) {
      if (!refs->is_array())
        fail("metadata.references", "must be an array of strings");
      for (const json& r : *refs)
        if (!r.is_string())
          fail("metadata.references", "must be an array of strings");
    }
  }

  void readModel(const json& obj) {
    allowOnly(obj, "model", {"stage_solver"});
    std::string solver = "classic";
    readString(obj, "model", "stage_solver", solver, false);
    if (solver == "classic") out.model = StageModel::Classic;
    else if (solver == "wdf") out.model = StageModel::Wdf;
    else fail("model.stage_solver", "must be \"classic\" or \"wdf\", not \"" + solver + "\"");
  }

  void readInput(const json& obj) {
    allowOnly(obj, "input", {"coupling_capacitor_f", "grid_leak_ohm", "grid_stopper_ohm", "miller_f", "source_ohm"});
    const Provenance s = Provenance::Schematic;
    readFloat(obj, "input", "coupling_capacitor_f", out.input.network.capacitance, s, 1.0e-12, 1.0e-3);
    readFloat(obj, "input", "grid_leak_ohm", out.input.network.gridLeak, s, 1.0e3, 1.0e8);
    readFloat(obj, "input", "grid_stopper_ohm", out.input.gridStopperOhms, s, 0.0, 1.0e7);
    readFloat(obj, "input", "miller_f", out.input.millerFarads, Provenance::Schematic, 1.0e-13, 1.0e-6);
    readFloat(obj, "input", "source_ohm", out.input.sourceOhms, Provenance::Schematic, 0.0, 1.0e6);
  }

  void readStage(const json& obj, size_t index) {
    const std::string path = element("stages", index);
    if (!obj.is_object())
      fail(path, "must be an object");
    allowOnly(obj, path, {"name", "tube", "plate_load_ohm", "plate_capacitance_f", "cathode", "grid", "coupling",
                          "gain_pot"});
    StageDescription& stage = out.stages[index];
    GainStageSpec& spec = stage.spec;
    spec = GainStageSpec{};
    readString(obj, path, "name", stage.name, false);
    if (stage.name.empty())
      stage.name = "stage " + std::to_string(index + 1);

    // Tube.
    const json* tube = find(obj, "tube");
    if (tube == nullptr)
      fail(child(path, "tube"), "missing");
    if (tube->is_string()) {
      const std::string name = tube->get<std::string>();
      bool found = false;
      for (const TubeEntry& t : kTubes) {
        if (name == t.name || name == t.alias) {
          stage.tubeType = t.name;
          spec.triode.tube = {t.mu, t.rp};
          found = true;
        }
      }
      if (!found)
        fail(child(path, "tube"), "unknown tube \"" + name + "\"; use 12AX7, 12AT7, 12AU7 or {\"mu\": ..., \"rp_ohm\": ...}");
    } else if (tube->is_object()) {
      allowOnly(*tube, child(path, "tube"), {"mu", "rp_ohm"});
      stage.tubeType = "custom";
      double mu = 0.0, rp = 0.0;
      readNumber(*tube, child(path, "tube"), "mu", mu, Provenance::Schematic, 1.0, 1000.0, true);
      readNumber(*tube, child(path, "tube"), "rp_ohm", rp, Provenance::Schematic, 100.0, 1.0e7, true);
      spec.triode.tube = {static_cast<float>(mu), static_cast<float>(rp)};
    } else {
      fail(child(path, "tube"), "must be a tube name or {\"mu\", \"rp_ohm\"}");
    }

    readFloat(obj, path, "plate_load_ohm", spec.triode.plateLoad, Provenance::Schematic, 1.0e3, 1.0e7, true);
    readFloat(obj, path, "plate_capacitance_f", spec.plateFarads, Provenance::Schematic, 0.0, 1.0e-6);

    // Cathode.
    const json none = json::object();
    const json& cathode = objectField(obj, path, "cathode", true, none);
    const std::string cathodePath = child(path, "cathode");
    allowOnly(cathode, cathodePath, {"resistor_ohm", "bias_current_a", "bypass_capacitor_f"});
    readFloat(cathode, cathodePath, "resistor_ohm", spec.triode.cathodeResistor, Provenance::Schematic, 0.0, 1.0e6, true);
    readFloat(cathode, cathodePath, "bias_current_a", spec.triode.biasCurrent, Provenance::Schematic, 1.0e-6, 1.0, true);
    if (const json* bypass = find(cathode, "bypass_capacitor_f")) {
      if (bypass->is_string()) {
        const std::string mode = bypass->get<std::string>();
        if (mode == "ideal") spec.bypass = CathodeBypass::Ideal;
        else if (mode == "none") spec.bypass = CathodeBypass::None;
        else fail(child(cathodePath, "bypass_capacitor_f"), "must be a capacitance, \"ideal\" or \"none\"");
      } else {
        double farads = 0.0;
        readNumber(cathode, cathodePath, "bypass_capacitor_f", farads, Provenance::Schematic, 1.0e-12, 1.0e-1);
        spec.bypass = CathodeBypass::Capacitor;
        spec.bypassFarads = static_cast<float>(farads);
      }
    }

    // Grid model: catalogue values for a 12AX7, required for other tubes.
    const json& grid = objectField(obj, path, "grid", false, none);
    const std::string gridPath = child(path, "grid");
    allowOnly(grid, gridPath, {"cutoff_v", "contact_potential_v", "conduction_ohm", "max_bias_shift_v"});
    if (stage.tubeType != "12AX7" && find(grid, "cutoff_v") == nullptr)
      fail(child(gridPath, "cutoff_v"), "required for a tube other than the 12AX7 (the grid voltage that cuts the plate current off)");
    readFloat(grid, gridPath, "cutoff_v", spec.triode.cutoffGridVolts, Provenance::Schematic, 0.1, 200.0);
    readFloat(grid, gridPath, "contact_potential_v", spec.triode.contactPotential, Provenance::Fitted, 0.0, 5.0);
    readFloat(grid, gridPath, "conduction_ohm", spec.triode.gridConductionResistance, Provenance::Fitted, 10.0, 1.0e6);
    readFloat(grid, gridPath, "max_bias_shift_v", spec.triode.maxBiasShiftVolts, Provenance::Fitted, 0.1, 100.0);

    // Coupling to the next grid (none after the last stage).
    const bool last = index + 1 == static_cast<size_t>(out.numStages);
    const json* coupling = find(obj, "coupling");
    if (last) {
      if (coupling != nullptr)
        fail(child(path, "coupling"), "nothing follows the last stage: remove it");
      spec.hasNext = false;
    } else {
      if (coupling == nullptr || !coupling->is_object())
        fail(child(path, "coupling"), "missing: every stage but the last couples to the next grid");
      const std::string couplingPath = child(path, "coupling");
      allowOnly(*coupling, couplingPath, {"capacitance_f", "series_ohm", "grid_leak_ohm"});
      spec.hasNext = true;
      // No capacitor until the file (or a voicing) gives one.
      spec.coupling = CouplingNetwork{0.0f, 0.0f, 1.0e6f};
      readFloat(*coupling, couplingPath, "capacitance_f", spec.coupling.capacitance, Provenance::Schematic, 1.0e-12, 1.0e-3);
      readFloat(*coupling, couplingPath, "series_ohm", spec.coupling.seriesResistance, Provenance::Schematic, 0.0, 1.0e8);
      readFloat(*coupling, couplingPath, "grid_leak_ohm", spec.coupling.gridLeak, Provenance::Schematic, 1.0e2, 1.0e8, true);
    }

    // Gain pot.
    if (const json* pot = find(obj, "gain_pot")) {
      if (!pot->is_object())
        fail(child(path, "gain_pot"), "must be an object");
      if (last)
        fail(child(path, "gain_pot"), "a gain pot sits between a plate and the next stage: not after the last stage");
      if (out.potStage >= 0)
        fail(child(path, "gain_pot"), "only one gain pot is supported");
      const std::string potPath = child(path, "gain_pot");
      allowOnly(*pot, potPath, {"resistance_ohm", "floor_ohm", "source_ohm", "load_ohm", "taper", "bright_capacitor_f"});
      spec.hasPot = true;
      out.potStage = static_cast<int>(index);
      spec.pot.sourceResistance = spec.triode.plateLoad;
      spec.pot.loadResistance = spec.coupling.gridLeak;
      readFloat(*pot, potPath, "resistance_ohm", spec.pot.potResistance, Provenance::Schematic, 1.0e3, 1.0e8, true);
      readFloat(*pot, potPath, "floor_ohm", spec.pot.floorResistance, Provenance::Schematic, 0.0, 1.0e7);
      readFloat(*pot, potPath, "source_ohm", spec.pot.sourceResistance, Provenance::Schematic, 0.0, 1.0e7);
      readFloat(*pot, potPath, "load_ohm", spec.pot.loadResistance, Provenance::Schematic, 1.0e3, 1.0e9);
      readFloat(*pot, potPath, "taper", spec.pot.taperExponent, Provenance::Schematic, 0.2, 10.0);
      readFloat(*pot, potPath, "bright_capacitor_f", spec.pot.brightCapacitance, Provenance::Schematic, 0.0, 1.0e-6);
    }
  }

  void readTone(const json& obj) {
    allowOnly(obj, "tone_stack", {"type", "treble_pot_ohm", "bass_pot_ohm", "middle_pot_ohm", "slope_ohm", "treble_cap_f",
                                  "bass_cap_f", "middle_cap_f"});
    readString(obj, "tone_stack", "type", out.tone.type, false);
    if (out.tone.type != "tmb")
      fail("tone_stack.type", "must be \"tmb\" (treble, middle, bass), not \"" + out.tone.type + "\"");
    ToneStackParts& p = out.tone.parts;
    const Provenance s = Provenance::Schematic;
    readNumber(obj, "tone_stack", "treble_pot_ohm", p.r1, s, 1.0e3, 1.0e8, true);
    readNumber(obj, "tone_stack", "bass_pot_ohm", p.r2, s, 1.0e3, 1.0e8, true);
    readNumber(obj, "tone_stack", "middle_pot_ohm", p.r3, s, 1.0e2, 1.0e8, true);
    readNumber(obj, "tone_stack", "slope_ohm", p.r4, s, 1.0e2, 1.0e8, true);
    readNumber(obj, "tone_stack", "treble_cap_f", p.c1, s, 1.0e-12, 1.0e-5, true);
    readNumber(obj, "tone_stack", "bass_cap_f", p.c2, s, 1.0e-10, 1.0e-4, true);
    readNumber(obj, "tone_stack", "middle_cap_f", p.c3, s, 1.0e-10, 1.0e-4, true);
  }

  void readEffectsLoop(const json& obj) {
    allowOnly(obj, "effects_loop", {"present", "note"});
    readBool(obj, "effects_loop", "present", out.effectsLoopPresent);
  }

  void readSupply(const json& obj, const std::string& path, SagSupply& supply, std::string& rectifier) {
    allowOnly(obj, path, {"b_plus_v", "source_ohm", "full_load_a", "reservoir_f", "attack_s", "rectifier"});
    readFloat(obj, path, "b_plus_v", supply.plateVolts, Provenance::Schematic, 5.0, 2000.0);
    readFloat(obj, path, "source_ohm", supply.sourceResistance, Provenance::Schematic, 0.0, 1.0e5);
    readFloat(obj, path, "full_load_a", supply.fullLoadAmps, Provenance::Schematic, 1.0e-4, 20.0);
    readFloat(obj, path, "reservoir_f", supply.filterCapacitance, Provenance::Schematic, 1.0e-7, 1.0);
    readFloat(obj, path, "attack_s", supply.attackSeconds, Provenance::Fitted, 1.0e-4, 5.0);
    readString(obj, path, "rectifier", rectifier, false);
    if (rectifier != "solid_state" && rectifier != "tube" && rectifier != "none")
      fail(child(path, "rectifier"), "must be \"solid_state\", \"tube\" or \"none\"");
  }

  void readPower(const json& obj) {
    const std::string path = "power_amp";
    allowOnly(obj, path, {"tube", "topology", "open_loop_gain", "feedback", "presence", "depth", "tone_cut", "transformer",
                          "knee_mix", "crossover", "supply"});
    PowerDescription& power = out.power;
    readString(obj, path, "tube", power.tube, false);
    static const char* const tubes[] = {"EL34", "6L6", "EL84", "6V6", "KT88", "6550", "custom"};
    if (std::none_of(std::begin(tubes), std::end(tubes), [&](const char* t) { return power.tube == t; }))
      fail(child(path, "tube"), "must be one of EL34, 6L6, EL84, 6V6, KT88, 6550, custom");
    readString(obj, path, "topology", power.topology, false);
    if (power.topology != "push_pull_ab" && power.topology != "push_pull_a" && power.topology != "solid_state")
      fail(child(path, "topology"), "must be \"push_pull_ab\", \"push_pull_a\" or \"solid_state\"");
    PowerAmpParams& p = power.params;
    readFloat(obj, path, "open_loop_gain", p.openLoopGain, Provenance::Fitted, 1.0, 1000.0);
    readFloat(obj, path, "knee_mix", p.kneeMix, Provenance::Fitted, 0.0, 1.0);

    const json none = json::object();
    const json& feedback = objectField(obj, path, "feedback", false, none);
    allowOnly(feedback, child(path, "feedback"), {"series_ohm", "shunt_ohm"});
    readFloat(feedback, child(path, "feedback"), "series_ohm", p.feedbackResistor, Provenance::Schematic, 1.0, 1.0e7);
    readFloat(feedback, child(path, "feedback"), "shunt_ohm", p.shuntResistor, Provenance::Schematic, 1.0, 1.0e7);

    const json& presence = objectField(obj, path, "presence", false, none);
    allowOnly(presence, child(path, "presence"), {"pot_ohm", "min_ohm", "capacitor_f"});
    readFloat(presence, child(path, "presence"), "pot_ohm", p.presencePot, Provenance::Schematic, 0.0, 1.0e7);
    readFloat(presence, child(path, "presence"), "min_ohm", p.presenceMinResistor, Provenance::Schematic, 0.1, 1.0e7);
    readFloat(presence, child(path, "presence"), "capacitor_f", p.presenceCapacitance, Provenance::Schematic, 1.0e-12, 1.0e-3);

    const json& depth = objectField(obj, path, "depth", false, none);
    allowOnly(depth, child(path, "depth"), {"inductance_h", "capacitance_f", "resistance_ohm", "max_cut"});
    readFloat(depth, child(path, "depth"), "inductance_h", p.depthInductance, Provenance::Schematic, 1.0e-4, 100.0);
    readFloat(depth, child(path, "depth"), "capacitance_f", p.depthCapacitance, Provenance::Schematic, 1.0e-9, 1.0);
    readFloat(depth, child(path, "depth"), "resistance_ohm", p.depthResistance, Provenance::Schematic, 0.1, 1.0e5);
    readFloat(depth, child(path, "depth"), "max_cut", p.depthMaxCut, Provenance::Fitted, 0.0, 1.0);

    if (find(obj, "tone_cut") != nullptr) {
      const std::string cutPath = child(path, "tone_cut");
      const json& cut = objectField(obj, path, "tone_cut", true, none);
      allowOnly(cut, cutPath, {"source_ohm", "pot_ohm", "min_ohm", "capacitor_f"});
      ToneCutParams& c = power.toneCut;
      readFloat(cut, cutPath, "source_ohm", c.sourceResistance, Provenance::Schematic, 100.0, 1.0e7, true);
      readFloat(cut, cutPath, "pot_ohm", c.potResistance, Provenance::Schematic, 0.0, 1.0e7, true);
      readFloat(cut, cutPath, "min_ohm", c.minResistance, Provenance::Schematic, 0.0, 1.0e7);
      readFloat(cut, cutPath, "capacitor_f", c.capacitance, Provenance::Schematic, 1.0e-12, 1.0e-4, true);
      power.hasToneCut = true;
    }

    const json& transformer = objectField(obj, path, "transformer", false, none);
    allowOnly(transformer, child(path, "transformer"), {"high_corner_hz", "low_corner_hz"});
    readFloat(transformer, child(path, "transformer"), "high_corner_hz", p.transformerLowpassHz, Provenance::Fitted, 500.0, 100000.0);
    readFloat(transformer, child(path, "transformer"), "low_corner_hz", power.transformerLowCornerHz, Provenance::Fitted, 1.0, 500.0);

    const json& crossover = objectField(obj, path, "crossover", false, none);
    allowOnly(crossover, child(path, "crossover"), {"ratio", "width"});
    readFloat(crossover, child(path, "crossover"), "ratio", p.crossoverRatio, Provenance::Fitted, 0.0, 1.0);
    readFloat(crossover, child(path, "crossover"), "width", p.crossoverWidth, Provenance::Fitted, 0.01, 10.0);
    if (power.topology != "push_pull_ab")
      p.crossoverRatio = 1.0f;
    p.hardClip = power.topology == "solid_state";

    const json& supply = objectField(obj, path, "supply", true, none);
    readSupply(supply, child(path, "supply"), power.supply, power.rectifier);
  }

  void readCalibration(const json& obj) {
    allowOnly(obj, "calibration", {"input_full_scale_v", "power_drive_v", "coupling_gain_rise", "output_trim", "control_smoothing_s"});
    CalibrationDescription& c = out.calibration;
    const Provenance f = Provenance::Fitted;
    readFloat(obj, "calibration", "input_full_scale_v", c.inputFullScaleVolts, f, 0.001, 1000.0);
    readFloat(obj, "calibration", "power_drive_v", c.powerDriveVolts, f, 0.01, 10000.0);
    readFloat(obj, "calibration", "coupling_gain_rise", c.couplingGainRise, f, 0.0, 20.0);
    readFloat(obj, "calibration", "output_trim", c.outputTrim, f, 1.0e-4, 100.0);
    readFloat(obj, "calibration", "control_smoothing_s", c.controlSmoothSeconds, f, 1.0e-4, 5.0);
  }

  void readVoicings(const json* voicings) {
    const int links = out.numStages - 1;
    if (voicings == nullptr) {
      out.numVoicings = 1;
      out.voicings[0].name = "Default";
      out.voicings[0].supply = out.power.supply;
      out.voicings[0].rectifier = out.power.rectifier;
    } else {
      if (!voicings->is_array() || voicings->empty() || voicings->size() > static_cast<size_t>(kMaxVoicings))
        fail("voicings", "must be an array of 1 to " + std::to_string(kMaxVoicings) + " voicings");
      out.numVoicings = static_cast<int>(voicings->size());
      for (size_t v = 0; v < voicings->size(); ++v) {
        const json& obj = (*voicings)[v];
        const std::string path = element("voicings", v);
        if (!obj.is_object())
          fail(path, "must be an object");
        allowOnly(obj, path, {"name", "coupling_capacitors_f", "supply"});
        VoicingDescription& voicing = out.voicings[v];
        readString(obj, path, "name", voicing.name, true);
        if (const json* caps = find(obj, "coupling_capacitors_f")) {
          if (!caps->is_array() || caps->size() != static_cast<size_t>(links))
            fail(child(path, "coupling_capacitors_f"), "must be an array with one capacitance per link (" + std::to_string(links) + ")");
          for (size_t k = 0; k < caps->size(); ++k) {
            const json& value = (*caps)[k];
            if (value.is_null())
              continue;
            if (!value.is_number() || value.get<double>() < 1.0e-12 || value.get<double>() > 1.0e-3)
              fail(element(child(path, "coupling_capacitors_f"), k), "must be a capacitance in farads or null");
            voicing.couplingFarads[k] = value.get<float>();
            out.notes.push_back({element(child(path, "coupling_capacitors_f"), k), Provenance::Fitted,
                                 "interstage capacitor chosen for the voicing", value.get<double>()});
          }
        }
        voicing.supply = out.power.supply;
        voicing.rectifier = out.power.rectifier;
        const json none = json::object();
        readSupply(objectField(obj, path, "supply", false, none), child(path, "supply"), voicing.supply, voicing.rectifier);
      }
    }
    // Every link needs a capacitor from its voicing or its stage.
    for (int v = 0; v < out.numVoicings; ++v)
      for (int k = 0; k < links; ++k)
        if (out.voicings[v].couplingFarads[k] <= 0.0f && out.stages[k].spec.coupling.capacitance <= 0.0f)
          fail(element("voicings", static_cast<size_t>(v)),
               "link " + std::to_string(k) + " (stage " + std::to_string(k) + " to " + std::to_string(k + 1) +
                   ") has no coupling capacitor: give stages[" + std::to_string(k) + "].coupling.capacitance_f or the voicing's coupling_capacitors_f");
  }

  std::string canonicalTarget(PanelId id) const {
    switch (id) {
      case PanelId::Gain: return "stages[" + std::to_string(out.potStage) + "].gain_pot";
      case PanelId::Bright: return "stages[" + std::to_string(out.potStage) + "].gain_pot.bright_capacitor_f";
      case PanelId::Bass: return "tone_stack.bass_pot_ohm";
      case PanelId::Middle: return "tone_stack.middle_pot_ohm";
      case PanelId::Treble: return "tone_stack.treble_pot_ohm";
      case PanelId::Presence: return "power_amp.presence";
      case PanelId::Depth: return "power_amp.depth";
      case PanelId::Master: return "master";
      case PanelId::Voicing: return "voicings";
    }
    return "";
  }

  static std::string defaultLabel(PanelId id) {
    std::string name = panelIdName(id);
    name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    return name;
  }

  void readPanel(const json* panel) {
    for (int i = 0; i < kPanelControls; ++i) {
      const PanelId id = static_cast<PanelId>(i);
      PanelControl& control = out.panel[i];
      control = PanelControl{};
      const bool needsPot = id == PanelId::Gain || id == PanelId::Bright;
      if (panel == nullptr) {
        // No panel given: every control the parts allow, linear, full travel.
        if (needsPot && out.potStage < 0)
          continue;
        control.present = true;
        control.label = defaultLabel(id);
        control.target = canonicalTarget(id);
        if (id == PanelId::Master)
          control.taper = 2.0f;
        continue;
      }
    }
    if (panel == nullptr)
      return;
    if (!panel->is_object())
      fail("panel", "must be an object keyed by control id");
    // A panel that only carries a faceplate has every control, as no panel does.
    bool anyControl = false;
    for (auto it = panel->begin(); it != panel->end(); ++it)
      anyControl = anyControl || it.key() != "faceplate";
    if (!anyControl) {
      readPanel(nullptr);
      readFaceplate(find(*panel, "faceplate"));
      return;
    }
    for (auto it = panel->begin(); it != panel->end(); ++it) {
      if (it.key() == "faceplate")
        continue;
      int index = -1;
      for (int i = 0; i < kPanelControls; ++i)
        if (it.key() == panelIdName(static_cast<PanelId>(i)))
          index = i;
      if (index < 0)
        fail(child("panel", it.key()), "unknown control; the controls are gain, bass, middle, treble, presence, depth, master, bright, voicing (and faceplate, which says how they are drawn)");
      const PanelId id = static_cast<PanelId>(index);
      const std::string path = child("panel", it.key());
      const json& obj = it.value();
      if (!obj.is_object())
        fail(path, "must be an object");
      allowOnly(obj, path, {"label", "target", "taper", "range"});
      PanelControl& control = out.panel[index];
      if ((id == PanelId::Gain || id == PanelId::Bright) && out.potStage < 0)
        fail(path, "this amp has no gain pot (give a stage a gain_pot)");
      control.present = true;
      control.label = defaultLabel(id);
      readString(obj, path, "label", control.label, false);
      std::string target;
      readString(obj, path, "target", target, true);
      const std::string expected = canonicalTarget(id);
      // The presence control can drive a tone cut instead (an amp without
      // global feedback has no presence network to move).
      const bool toneCut = id == PanelId::Presence && target == "power_amp.tone_cut";
      if (toneCut && !out.power.hasToneCut)
        fail(child(path, "target"), "this amp has no tone cut (give power_amp a tone_cut)");
      if (target != expected && !toneCut)
        fail(child(path, "target"), "the " + std::string(panelIdName(id)) + " control drives \"" + expected +
                                        "\" in this description" +
                                        (id == PanelId::Presence && out.power.hasToneCut ? " (or \"power_amp.tone_cut\")" : "") +
                                        ", not \"" + target + "\"");
      control.target = target;
      if (toneCut)
        out.presenceDrivesToneCut = true;
      control.taper = id == PanelId::Master ? 2.0f : 1.0f;
      readFloat(obj, path, "taper", control.taper, Provenance::Schematic, 0.2, 8.0);
      if (const json* range = find(obj, "range")) {
        if (!range->is_array() || range->size() != 2 || !(*range)[0].is_number() || !(*range)[1].is_number())
          fail(child(path, "range"), "must be [low, high] with both between 0 and 1");
        const double lo = (*range)[0].get<double>(), hi = (*range)[1].get<double>();
        if (lo < 0.0 || lo > 1.0 || hi < 0.0 || hi > 1.0 || lo == hi)
          fail(child(path, "range"), "must be [low, high] with both between 0 and 1 and different");
        control.low = static_cast<float>(lo);
        control.high = static_cast<float>(hi);
      }
    }
    readFaceplate(find(*panel, "faceplate"));
  }

  /** One of the ids in `table`, as an index; fails with the list otherwise. */
  template <size_t N>
  static int readChoice(const json& obj, const std::string& path, const char* key, const char* const (&table)[N],
                        int fallback) {
    std::string value;
    if (!readString(obj, path, key, value, false))
      return fallback;
    for (size_t i = 0; i < N; ++i)
      if (value == table[i])
        return static_cast<int>(i);
    std::string list;
    for (size_t i = 0; i < N; ++i)
      list += std::string(i == 0 ? "" : ", ") + "\"" + table[i] + "\"";
    fail(child(path, key), "must be one of " + list + ", not \"" + value + "\"");
  }

  static void readText(const json& obj, const std::string& path, const char* key, std::string& value, size_t longest) {
    if (!readString(obj, path, key, value, false))
      return;
    if (value.empty() || value.size() > longest)
      fail(child(path, key), "must be 1 to " + std::to_string(longest) + " characters");
    // Plain text on one line: no control characters.
    for (const char c : value)
      if (static_cast<unsigned char>(c) < 0x20)
        fail(child(path, key), "must be one line of plain text");
  }

  void readFaceplate(const json* plate) {
    Faceplate& face = out.faceplate;
    face = Faceplate{};
    if (plate == nullptr)
      return;
    const std::string path = "panel.faceplate";
    if (!plate->is_object())
      fail(path, "must be an object");
    allowOnly(*plate, path, {"accent", "nameplate", "tag", "character", "strip", "knob_cap", "groups"});
    face.styled = true;
    if (readString(*plate, path, "accent", face.accent, false)) {
      bool valid = face.accent.size() == 7 && face.accent[0] == '#';
      for (size_t i = 1; valid && i < face.accent.size(); ++i)
        valid = std::isxdigit(static_cast<unsigned char>(face.accent[i])) != 0;
      if (!valid)
        fail(child(path, "accent"), "must be a colour as #RRGGBB, not \"" + face.accent + "\"");
      for (char& c : face.accent)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    face.nameplate = static_cast<Nameplate>(readChoice(*plate, path, "nameplate", kNameplateIds, 0));
    readText(*plate, path, "tag", face.tag, 24);
    readText(*plate, path, "character", face.character, 96);
    face.knobCap = static_cast<KnobCap>(readChoice(*plate, path, "knob_cap", kKnobCapIds, 0));
    face.strip = static_cast<StripPattern>(readChoice(*plate, path, "strip", kStripIds, 0));

    const json* groups = find(*plate, "groups");
    if (groups == nullptr)
      return;
    if (!groups->is_array() || groups->empty() || groups->size() > 6)
      fail(child(path, "groups"), "must be a list of 1 to 6 boxes");
    bool placed[kPanelControls] = {};
    for (size_t g = 0; g < groups->size(); ++g) {
      const std::string where = element(child(path, "groups"), g);
      const json& box = (*groups)[g];
      if (!box.is_object())
        fail(where, "must be an object with a title and controls");
      allowOnly(box, where, {"title", "controls", "columns"});
      PanelGroup group;
      readString(box, where, "title", group.title, true);
      if (group.title.empty() || group.title.size() > 24)
        fail(child(where, "title"), "must be 1 to 24 characters");
      const json* controls = find(box, "controls");
      if (controls == nullptr || !controls->is_array() || controls->empty())
        fail(child(where, "controls"), "must be a list of control ids");
      for (size_t c = 0; c < controls->size(); ++c) {
        const std::string at = element(child(where, "controls"), c);
        if (!(*controls)[c].is_string())
          fail(at, "must be a control id");
        const std::string id = (*controls)[c].get<std::string>();
        int index = -1;
        for (int i = 0; i < kPanelControls; ++i)
          if (id == panelIdName(static_cast<PanelId>(i)))
            index = i;
        if (index < 0)
          fail(at, "unknown control \"" + id + "\"");
        if (!out.panel[index].present)
          fail(at, "the " + id + " control is not on this amp's panel");
        if (placed[index])
          fail(at, "the " + id + " control is already in a box");
        placed[index] = true;
        group.controls.push_back(static_cast<PanelId>(index));
      }
      if (const json* columns = find(box, "columns")) {
        if (!columns->is_number_integer() || columns->get<int>() < 1 || columns->get<int>() > 6)
          fail(child(where, "columns"), "must be a whole number from 1 to 6");
        group.columns = columns->get<int>();
      }
      face.groups.push_back(std::move(group));
    }
    for (int i = 0; i < kPanelControls; ++i)
      if (out.panel[i].present && !placed[i])
        fail(child(path, "groups"), std::string("the ") + panelIdName(static_cast<PanelId>(i)) +
                                        " control is on the panel but in no box");
  }

private:
  AmpDescription& out;
};

}  // namespace

Faceplate resolveFaceplate(const AmpDescription& description) {
  Faceplate face = description.faceplate;
  if (!face.groups.empty())
    return face;
  // The plain default: the knobs of the preamp, the knobs of the power amp.
  const auto box = [&description](const char* title, std::initializer_list<PanelId> ids) {
    PanelGroup group;
    group.title = title;
    for (const PanelId id : ids)
      if (description.control(id).present)
        group.controls.push_back(id);
    return group;
  };
  for (PanelGroup group : {box("Preamp", {PanelId::Gain, PanelId::Bass, PanelId::Middle, PanelId::Treble, PanelId::Bright}),
                           box("Power", {PanelId::Presence, PanelId::Depth, PanelId::Master, PanelId::Voicing})})
    if (!group.controls.empty())
      face.groups.push_back(std::move(group));
  return face;
}

bool parseAmpDescription(const std::string& text, AmpDescription& out, std::string& error) {
  error.clear();
  AmpDescription result;
  try {
    const json root = json::parse(text);
    Reader(result).readAll(root);
  } catch (const ParseError& e) {
    error = e.what();
    return false;
  } catch (const json::exception& e) {
    error = std::string("not valid JSON: ") + e.what();
    return false;
  } catch (const std::exception& e) {
    error = e.what();
    return false;
  }
  out = std::move(result);
  return true;
}

}  // namespace amp

// The speaker description loader: JSON text to a SpeakerDescription, with
// checks.
//
// Not for the audio thread (it allocates). Every field is checked for its type
// and a sane range, unknown fields are errors (a misspelt field would otherwise
// silently keep its default), and every error names the path of the field. The
// format is documented in docs/cab-framework.md.
#include "cab/SpeakerDescription.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>

#include "json.hpp"

namespace cab {

namespace {

using json = nlohmann::json;

struct ParseError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

std::string child(const std::string& path, const std::string& key) {
  return path.empty() ? key : path + "." + key;
}

[[noreturn]] void fail(const std::string& path, const std::string& message) {
  throw ParseError((path.empty() ? std::string("description") : path) + ": " + message);
}

std::string format(double v) {
  char buffer[40];
  std::snprintf(buffer, sizeof buffer, "%g", v);
  return buffer;
}

void allowOnly(const json& obj, const std::string& path, std::initializer_list<const char*> keys) {
  for (auto it = obj.begin(); it != obj.end(); ++it) {
    bool known = false;
    for (const char* key : keys)
      known = known || it.key() == key;
    if (!known)
      fail(child(path, it.key()), "unknown field");
  }
}

const json* find(const json& obj, const char* key) {
  const auto it = obj.find(key);
  return it == obj.end() ? nullptr : &*it;
}

const json& objectField(const json& obj, const std::string& path, const char* key, bool required,
                        const json& absent) {
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

void readString(const json& obj, const std::string& path, const char* key, std::string& value, bool required) {
  const json* v = find(obj, key);
  if (v == nullptr) {
    if (required)
      fail(child(path, key), "missing");
    return;
  }
  if (!v->is_string())
    fail(child(path, key), "must be a string");
  value = v->get<std::string>();
}

void readNumber(const json& obj, const std::string& path, const char* key, double& value, double lo, double hi,
                bool required) {
  const json* v = find(obj, key);
  const std::string where = child(path, key);
  if (v == nullptr) {
    if (required)
      fail(where, "missing");
    return;
  }
  if (!v->is_number())
    fail(where, "must be a number");
  const double number = v->get<double>();
  if (!std::isfinite(number) || number < lo || number > hi)
    fail(where, "must be between " + format(lo) + " and " + format(hi) + ", not " + format(number));
  value = number;
}

void readInteger(const json& obj, const std::string& path, const char* key, int& value, int lo, int hi,
                 bool required) {
  const json* v = find(obj, key);
  const std::string where = child(path, key);
  if (v == nullptr) {
    if (required)
      fail(where, "missing");
    return;
  }
  if (!v->is_number_integer())
    fail(where, "must be an integer");
  const auto number = v->get<long long>();
  if (number < lo || number > hi)
    fail(where, "must be between " + std::to_string(lo) + " and " + std::to_string(hi) + ", not " +
                    std::to_string(number));
  value = static_cast<int>(number);
}

void readAll(const json& root, SpeakerDescription& out) {
  if (!root.is_object())
    fail("", "the top level must be an object");
  allowOnly(root, "", {"$schema", "format", "version", "metadata", "diameter_in", "thiele_small", "voice_coil", "cone"});

  std::string formatName;
  readString(root, "", "format", formatName, true);
  if (formatName != "pedaleira-cab-speaker")
    fail("format", "must be \"pedaleira-cab-speaker\", not \"" + formatName + "\"");
  const json* version = find(root, "version");
  if (version == nullptr || !version->is_number_integer())
    fail("version", "missing or not an integer");
  out.version = version->get<int>();
  if (out.version != kSpeakerDescriptionVersion)
    fail("version", "this build reads version " + std::to_string(kSpeakerDescriptionVersion) +
                        ", the file is version " + std::to_string(out.version));

  const json none = json::object();
  const std::string top;
  const json& metadata = objectField(root, top, "metadata", true, none);
  allowOnly(metadata, "metadata", {"name", "author", "licence", "summary", "references"});
  readString(metadata, "metadata", "name", out.name, true);
  if (out.name.empty())
    fail("metadata.name", "must not be empty");
  readString(metadata, "metadata", "author", out.author, false);
  readString(metadata, "metadata", "licence", out.licence, true);
  if (out.licence.empty())
    fail("metadata.licence", "must not be empty: say which licence this description is under");
  readString(metadata, "metadata", "summary", out.summary, false);
  if (const json* refs = find(metadata, "references")) {
    if (!refs->is_array())
      fail("metadata.references", "must be an array of strings");
    for (const json& r : *refs)
      if (!r.is_string())
        fail("metadata.references", "must be an array of strings");
  }

  readNumber(root, "", "diameter_in", out.diameterInches, 6.0, 15.0, true);

  const json& ts = objectField(root, top, "thiele_small", true, none);
  allowOnly(ts, "thiele_small", {"resonance_hz", "q_total", "compliance_l"});
  readNumber(ts, "thiele_small", "resonance_hz", out.resonanceHz, 20.0, 300.0, true);
  readNumber(ts, "thiele_small", "q_total", out.qTotal, 0.2, 5.0, true);
  readNumber(ts, "thiele_small", "compliance_l", out.complianceLitres, 1.0, 500.0, true);

  const json& coil = objectField(root, top, "voice_coil", false, none);
  allowOnly(coil, "voice_coil", {"resistance_ohm", "inductance_mh", "loss_exponent"});
  readNumber(coil, "voice_coil", "resistance_ohm", out.coilResistanceOhm, 1.0, 32.0, false);
  readNumber(coil, "voice_coil", "inductance_mh", out.coilInductanceMh, 0.0, 10.0, false);
  readNumber(coil, "voice_coil", "loss_exponent", out.coilLossExponent, 0.3, 1.0, false);

  const json& cone = objectField(root, top, "cone", true, none);
  allowOnly(cone, "cone", {"radiating_fraction", "breakup", "rolloff_hz", "rolloff_order"});
  readNumber(cone, "cone", "radiating_fraction", out.radiatingFraction, 0.1, 1.0, false);
  readNumber(cone, "cone", "rolloff_hz", out.rolloffHz, 1000.0, 20000.0, true);
  readInteger(cone, "cone", "rolloff_order", out.rolloffOrder, 1, 12, false);
  out.breakup.clear();
  if (const json* breakup = find(cone, "breakup")) {
    if (!breakup->is_array())
      fail("cone.breakup", "must be an array of resonances");
    if (breakup->size() > static_cast<size_t>(kMaxBreakupResonances))
      fail("cone.breakup", "at most " + std::to_string(kMaxBreakupResonances) + " resonances");
    for (size_t i = 0; i < breakup->size(); ++i) {
      const std::string path = "cone.breakup[" + std::to_string(i) + "]";
      const json& entry = (*breakup)[i];
      if (!entry.is_object())
        fail(path, "must be an object");
      allowOnly(entry, path, {"frequency_hz", "gain_db", "q", "note"});
      ConeResonance resonance;
      readNumber(entry, path, "frequency_hz", resonance.frequencyHz, 50.0, 20000.0, true);
      readNumber(entry, path, "gain_db", resonance.gainDb, -24.0, 24.0, true);
      readNumber(entry, path, "q", resonance.q, 0.2, 30.0, true);
      std::string note;
      readString(entry, path, "note", note, false);
      out.breakup.push_back(resonance);
    }
  }
}

}  // namespace

bool parseSpeakerDescription(const std::string& text, SpeakerDescription& out, std::string& error) {
  try {
    const json root = json::parse(text);
    SpeakerDescription parsed;
    readAll(root, parsed);
    out = std::move(parsed);
    return true;
  } catch (const ParseError& e) {
    error = e.what();
  } catch (const json::exception& e) {
    error = std::string("not valid JSON: ") + e.what();
  }
  return false;
}

}  // namespace cab

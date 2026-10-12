#include "CaptureSession.h"

#include <cmath>

namespace Capture {

namespace {
template <typename Enum, size_t N>
const char* nameOf(const char* const (&names)[N], Enum value) {
  const size_t index = static_cast<size_t>(value);
  return index < N ? names[index] : names[0];
}
template <typename Enum, size_t N>
bool parseName(const char* const (&names)[N], const juce::String& text, Enum& out) {
  for (size_t i = 0; i < N; ++i)
    if (text == names[i]) {
      out = static_cast<Enum>(i);
      return true;
    }
  return false;
}

constexpr const char* kSources[] = {"pedal", "ampHead", "ampCab", "modeller"};
constexpr const char* kMethods[] = {"reamp", "dryWet"};
constexpr const char* kSteps[] = {"source", "wiring", "levels", "record", "check", "train", "review"};
constexpr const char* kGainTags[] = {"clean", "crunch", "highGain"};
constexpr const char* kSizes[] = {"full", "lite"};
}  // namespace

const char* name(Source value) { return nameOf(kSources, value); }
const char* name(Method value) { return nameOf(kMethods, value); }
const char* name(Step value) { return nameOf(kSteps, value); }
const char* name(GainTag value) { return nameOf(kGainTags, value); }
const char* name(ModelSize value) { return nameOf(kSizes, value); }
bool parse(const juce::String& text, Source& out) { return parseName(kSources, text, out); }
bool parse(const juce::String& text, Method& out) { return parseName(kMethods, text, out); }
bool parse(const juce::String& text, Step& out) { return parseName(kSteps, text, out); }
bool parse(const juce::String& text, GainTag& out) { return parseName(kGainTags, text, out); }
bool parse(const juce::String& text, ModelSize& out) { return parseName(kSizes, text, out); }

const juce::StringArray& gearTypes() {
  static const juce::StringArray types{"ampHead", "ampCab",   "cabinet",     "pedal",
                                       "outboard", "spaces", "experimental"};
  return types;
}

const char* defaultGearType(Source source) {
  switch (source) {
    case Source::pedal: return "pedal";
    case Source::ampHead: return "ampHead";
    case Source::ampCab: return "ampCab";
    case Source::modeller: return "outboard";
  }
  return "ampHead";
}

const char* namGearType(const juce::String& gearType) {
  if (gearType == "ampHead") return "amp";
  if (gearType == "ampCab" || gearType == "cabinet") return "amp_cab";
  if (gearType == "pedal") return "pedal";
  return "studio";
}

const char* namToneType(GainTag tag) {
  switch (tag) {
    case GainTag::clean: return "clean";
    case GainTag::crunch: return "crunch";
    case GainTag::highGain: return "hi_gain";
  }
  return "crunch";
}

}  // namespace Capture

namespace {

juce::var object() { return juce::var(new juce::DynamicObject()); }

juce::String nowText() { return juce::Time::getCurrentTime().toISO8601(true); }

double number(const juce::var& v, double fallback) {
  return v.isDouble() || v.isInt() || v.isInt64() ? static_cast<double>(v) : fallback;
}
int integer(const juce::var& v, int fallback) {
  return v.isDouble() || v.isInt() || v.isInt64() ? static_cast<int>(v) : fallback;
}

}  // namespace

CaptureSession CaptureSession::make(const juce::String& id, const juce::String& name) {
  CaptureSession s;
  s.id = id;
  s.name = name;
  s.created = s.updated = nowText();
  s.gear.type = Capture::defaultGearType(s.source);
  return s;
}

juce::var CaptureSession::toVar() const {
  juce::var out = object();
  auto* o = out.getDynamicObject();
  o->setProperty("version", version);
  o->setProperty("id", id);
  o->setProperty("name", name);
  o->setProperty("notes", notes);
  o->setProperty("created", created);
  o->setProperty("updated", updated);
  o->setProperty("source", Capture::name(source));
  o->setProperty("method", Capture::name(method));
  o->setProperty("gainTag", Capture::name(gainTag));
  o->setProperty("modelSize", Capture::name(modelSize));
  o->setProperty("step", Capture::name(step));

  juce::var g = object();
  g.getDynamicObject()->setProperty("type", gear.type);
  g.getDynamicObject()->setProperty("make", gear.make);
  g.getDynamicObject()->setProperty("model", gear.model);
  o->setProperty("gear", g);

  juce::var c = object();
  c.getDynamicObject()->setProperty("send", channels.send);
  c.getDynamicObject()->setProperty("returnIn", channels.returnIn);
  c.getDynamicObject()->setProperty("dry", channels.dry);
  c.getDynamicObject()->setProperty("wet", channels.wet);
  o->setProperty("channels", c);

  juce::var cal = object();
  cal.getDynamicObject()->setProperty("returnDbu", calibrationDbu);
  if (sendCalibrationDbu.has_value())
    cal.getDynamicObject()->setProperty("sendDbu", *sendCalibrationDbu);
  o->setProperty("calibration", cal);

  juce::var l = object();
  auto* lo = l.getDynamicObject();
  lo->setProperty("sendDb", levels.sendDb);
  lo->setProperty("targetDb", levels.targetDb);
  lo->setProperty("bandDb", levels.bandDb);
  lo->setProperty("measured", levels.measured);
  lo->setProperty("inRange", levels.inRange);
  lo->setProperty("peakDb", levels.peakDb);
  lo->setProperty("wetPeakDb", levels.wetPeakDb);
  lo->setProperty("rmsDb", levels.rmsDb);
  if (levels.noiseFloorDb.has_value())
    lo->setProperty("noiseFloorDb", *levels.noiseFloorDb);
  lo->setProperty("clippedSamples", levels.clippedSamples);
  o->setProperty("levels", l);

  juce::var r = object();
  auto* ro = r.getDynamicObject();
  ro->setProperty("state", recording.state);
  ro->setProperty("seconds", recording.seconds);
  ro->setProperty("samples", recording.samples);
  ro->setProperty("sampleRate", recording.sampleRate);
  ro->setProperty("inputFile", recording.inputFile);
  ro->setProperty("inputVersion", recording.inputVersion);
  ro->setProperty("error", recording.error);
  ro->setProperty("plannedSeconds", recording.plannedSeconds);
  o->setProperty("recording", r);

  if (!checks.isVoid())
    o->setProperty("checks", checks);
  o->setProperty("checksOverridden", checksOverridden);

  juce::var t = object();
  auto* to = t.getDynamicObject();
  to->setProperty("state", training.state);
  to->setProperty("epochs", training.epochs);
  to->setProperty("epoch", training.epoch);
  if (training.esr.has_value())
    to->setProperty("esr", *training.esr);
  if (training.latencySamples.has_value())
    to->setProperty("latencySamples", *training.latencySamples);
  if (training.trainerChecksPassed.has_value())
    to->setProperty("trainerChecksPassed", *training.trainerChecksPassed);
  to->setProperty("error", training.error);
  o->setProperty("training", t);

  juce::var f = object();
  f.getDynamicObject()->setProperty("return", returnFile);
  f.getDynamicObject()->setProperty("dry", dryFile);
  f.getDynamicObject()->setProperty("wet", wetFile);
  f.getDynamicObject()->setProperty("model", modelFile);
  f.getDynamicObject()->setProperty("saved", savedFile);
  o->setProperty("files", f);
  return out;
}

bool CaptureSession::fromVar(const juce::var& v, CaptureSession& out, juce::String& error) {
  if (v.getDynamicObject() == nullptr || v["id"].toString().isEmpty()) {
    error = "not a capture session (no id)";
    return false;
  }
  CaptureSession s;
  s.id = v["id"].toString();
  s.name = v["name"].toString();
  s.notes = v["notes"].toString();
  s.created = v["created"].toString();
  s.updated = v["updated"].toString();
  Capture::parse(v["source"].toString(), s.source);
  Capture::parse(v["method"].toString(), s.method);
  Capture::parse(v["gainTag"].toString(), s.gainTag);
  Capture::parse(v["modelSize"].toString(), s.modelSize);
  Capture::parse(v["step"].toString(), s.step);

  const juce::var gear = v["gear"];
  s.gear.type = gear["type"].toString();
  if (!Capture::gearTypes().contains(s.gear.type))
    s.gear.type = Capture::defaultGearType(s.source);
  s.gear.make = gear["make"].toString();
  s.gear.model = gear["model"].toString();

  const juce::var ch = v["channels"];
  s.channels.send = juce::jlimit(0, 63, integer(ch["send"], 0));
  s.channels.returnIn = juce::jlimit(0, 63, integer(ch["returnIn"], 0));
  s.channels.dry = juce::jlimit(0, 63, integer(ch["dry"], 0));
  s.channels.wet = juce::jlimit(0, 63, integer(ch["wet"], 1));

  const juce::var cal = v["calibration"];
  s.calibrationDbu = number(cal["returnDbu"], 12.5);
  if (!cal["sendDbu"].isVoid())
    s.sendCalibrationDbu = number(cal["sendDbu"], 0.0);

  const juce::var l = v["levels"];
  s.levels.sendDb = number(l["sendDb"], -24.0);
  s.levels.targetDb = number(l["targetDb"], -12.0);
  s.levels.bandDb = number(l["bandDb"], 3.0);
  s.levels.measured = static_cast<bool>(l["measured"]);
  s.levels.inRange = static_cast<bool>(l["inRange"]);
  s.levels.peakDb = number(l["peakDb"], -120.0);
  s.levels.wetPeakDb = number(l["wetPeakDb"], -120.0);
  s.levels.rmsDb = number(l["rmsDb"], -120.0);
  if (!l["noiseFloorDb"].isVoid())
    s.levels.noiseFloorDb = number(l["noiseFloorDb"], -120.0);
  s.levels.clippedSamples = integer(l["clippedSamples"], 0);

  const juce::var r = v["recording"];
  s.recording.state = r["state"].toString().isEmpty() ? "none" : r["state"].toString();
  s.recording.seconds = number(r["seconds"], 0.0);
  s.recording.samples = integer(r["samples"], 0);
  s.recording.sampleRate = number(r["sampleRate"], Capture::kCaptureSampleRate);
  s.recording.inputFile = r["inputFile"].toString();
  s.recording.inputVersion = r["inputVersion"].toString();
  s.recording.error = r["error"].toString();
  s.recording.plannedSeconds = number(r["plannedSeconds"], 180.0);

  s.checks = v["checks"];
  s.checksOverridden = static_cast<bool>(v["checksOverridden"]);

  const juce::var t = v["training"];
  s.training.state = t["state"].toString().isEmpty() ? "none" : t["state"].toString();
  s.training.epochs = juce::jlimit(1, 10000, integer(t["epochs"], 100));
  s.training.epoch = integer(t["epoch"], 0);
  if (!t["esr"].isVoid())
    s.training.esr = number(t["esr"], 1.0);
  if (!t["latencySamples"].isVoid())
    s.training.latencySamples = integer(t["latencySamples"], 0);
  if (!t["trainerChecksPassed"].isVoid())
    s.training.trainerChecksPassed = static_cast<bool>(t["trainerChecksPassed"]);
  s.training.error = t["error"].toString();

  const juce::var f = v["files"];
  s.returnFile = f["return"].toString();
  s.dryFile = f["dry"].toString();
  s.wetFile = f["wet"].toString();
  s.modelFile = f["model"].toString();
  s.savedFile = f["saved"].toString();

  out = std::move(s);
  return true;
}

void CaptureSession::markInterrupted(bool recordingGoing, bool trainingGoing) {
  if (!recordingGoing && (recording.state == "recording" || recording.state == "writing")) {
    recording.state = "failed";
    recording.error = "The recording was interrupted when the application stopped.";
  }
  if (!trainingGoing && training.state == "running") {
    training.state = "failed";
    training.error = "Training was interrupted when the application stopped.";
  }
}

// ####################
// THE STEPS
// ####################
juce::String CaptureSession::whatBlocks() const {
  switch (step) {
    case Step::source:
      if (name.trim().isEmpty())
        return "Give the capture a name.";
      if (!Capture::gearTypes().contains(gear.type))
        return "Pick the gear type.";
      return {};
    case Step::wiring:
      if (method == Method::dryWet && channels.dry == channels.wet)
        return "The dry and wet inputs must be different channels.";
      return {};
    case Step::levels:
      if (!levels.measured)
        return "Measure the levels first: play the test tone" +
               juce::String(method == Method::dryWet ? " or play the guitar." : ".");
      if (!levels.inRange)
        return "The level is outside the target band (" + juce::String(bandLow(), 1) + " to " +
               juce::String(bandHigh(), 1) + " dB). Adjust the send level or the input gain.";
      return {};
    case Step::record:
      return recording.complete() ? juce::String() : juce::String("The recording is not finished.");
    case Step::check:
      if (checks.isVoid())
        return "Run the checks first.";
      if (checks["verdict"].toString() == "fail" && !checksOverridden)
        return "The checks failed. Record again, or override them to train anyway.";
      return {};
    case Step::train:
      return training.done() ? juce::String() : juce::String("Training has not finished.");
    case Step::review:
      return "This is the last step.";
  }
  return {};
}

juce::String CaptureSession::whyCannotMoveTo(Step target) const {
  const int from = static_cast<int>(step), to = static_cast<int>(target);
  if (to <= from)
    return {};
  if (to > from + 1)
    return juce::String("Steps go one at a time: this capture is at ") + Capture::name(step) +
           ", and " + Capture::name(target) + " is further on.";
  return whatBlocks();
}

std::vector<juce::String> CaptureSession::clearFrom(Step from) {
  std::vector<juce::String> stale;
  const int f = static_cast<int>(from);
  const auto addFile = [&](juce::String& file) {
    if (file.isNotEmpty())
      stale.push_back(file);
    file.clear();
  };
  if (f <= static_cast<int>(Step::levels)) {
    levels.measured = false;
    levels.inRange = false;
    levels.peakDb = levels.wetPeakDb = levels.rmsDb = -120.0;
    levels.clippedSamples = 0;
    levels.noiseFloorDb.reset();
  }
  if (f <= static_cast<int>(Step::record)) {
    const double planned = recording.plannedSeconds;
    recording = {};
    recording.plannedSeconds = planned;
    addFile(returnFile);
    addFile(dryFile);
    addFile(wetFile);
  }
  if (f <= static_cast<int>(Step::check)) {
    checks = juce::var();
    checksOverridden = false;
  }
  if (f <= static_cast<int>(Step::train)) {
    const int epochs = training.epochs;
    training = {};
    training.epochs = epochs;
    addFile(modelFile);
    stale.push_back("training/");
  }
  if (f <= static_cast<int>(Step::review))
    savedFile.clear();
  return stale;
}

std::vector<juce::String> CaptureSession::moveTo(Step target) {
  std::vector<juce::String> stale;
  if (target == step)
    return stale;
  if (static_cast<int>(target) < static_cast<int>(step))
    stale = clearFrom(target);
  step = target;
  return stale;
}

// ####################
// METADATA AND NAMES
// ####################
juce::var CaptureSession::namMetadata() const {
  juce::var out = object();
  auto* o = out.getDynamicObject();
  o->setProperty("name", name.trim());
  o->setProperty("gear_type", juce::String(Capture::namGearType(gear.type)));
  if (gear.make.trim().isNotEmpty())
    o->setProperty("gear_make", gear.make.trim());
  if (gear.model.trim().isNotEmpty())
    o->setProperty("gear_model", gear.model.trim());
  o->setProperty("tone_type", juce::String(Capture::namToneType(gainTag)));
  o->setProperty("output_level_dbu", calibrationDbu);
  if (sendCalibrationDbu.has_value())
    o->setProperty("input_level_dbu", *sendCalibrationDbu);
  return out;
}

juce::String CaptureSession::readableFileName() const {
  juce::String out;
  for (const juce::juce_wchar c : name.trim()) {
    const bool illegal = c < 32 || juce::String("/\\:*?\"<>|").containsChar(c);
    out << juce::String::charToString(illegal ? juce::juce_wchar(' ') : c);
  }
  // Collapse runs of spaces, no leading dots.
  while (out.contains("  "))
    out = out.replace("  ", " ");
  out = out.trim().trimCharactersAtStart(".").trim();
  if (out.length() > 80)
    out = out.substring(0, 80).trim();
  return out.isEmpty() ? juce::String("Capture") : out;
}

juce::var CaptureSession::summary() const {
  juce::var out = object();
  auto* o = out.getDynamicObject();
  o->setProperty("id", id);
  o->setProperty("name", name);
  o->setProperty("source", Capture::name(source));
  o->setProperty("method", Capture::name(method));
  o->setProperty("step", Capture::name(step));
  o->setProperty("gainTag", Capture::name(gainTag));
  o->setProperty("created", created);
  o->setProperty("updated", updated);
  o->setProperty("trainingState", training.state);
  if (training.esr.has_value())
    o->setProperty("esr", *training.esr);
  if (savedFile.isNotEmpty())
    o->setProperty("savedFile", savedFile);
  return out;
}

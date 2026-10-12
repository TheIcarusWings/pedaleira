#include "CaptureStudio.h"

// Processor.h first: it pulls in the JUCE module headers, which must come
// before anything that drags in <windows.h>.
#include "Processor.h"

#include "AgentHub.h"
#include "CaptureChecks.h"
#include "GuitarProfiles.h"
#include "LibraryPaths.h"
#include "PresetJson.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>

namespace {

using Properties = std::initializer_list<std::pair<const char*, juce::var>>;

juce::var object(Properties properties = {}) {
  juce::DynamicObject::Ptr obj = new juce::DynamicObject();
  for (const auto& [name, value] : properties)
    obj->setProperty(name, value);
  return juce::var(obj.get());
}

CommandResult fail(const char* code, const juce::String& message) {
  return CommandResult::failure(code, message);
}

CommandResult wrongStep(const juce::String& what, const CaptureSession& s) {
  return fail(CommandError::failed, what + " (this capture is at the " + Capture::name(s.step) +
                                        " step)");
}

constexpr int kPreRollMs = 500;
constexpr int kTailMs = 250;
constexpr int kLiveLatencyWindowSeconds = 5;
constexpr double kMaxStandardInputSeconds = 400.0;
constexpr double kMaxPlannedSeconds = 600.0;

double round1(double v) { return std::round(v * 10.0) / 10.0; }
double round2(double v) { return std::round(v * 100.0) / 100.0; }

// A 24-bit mono WAV at 48 kHz: what the trainer reads and what TONE3000 asks
// for. Written beside the target and moved over it.
bool writeWav24(const juce::File& file, const std::vector<float>& samples, double rate) {
  if (!file.getParentDirectory().createDirectory().wasOk())
    return false;
  juce::TemporaryFile temporary(file);
  {
    std::unique_ptr<juce::OutputStream> stream = temporary.getFile().createOutputStream();
    if (stream == nullptr)
      return false;
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor(
        stream, juce::AudioFormatWriterOptions{}
                    .withSampleRate(rate)
                    .withNumChannels(1)
                    .withBitsPerSample(24)
                    .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::integral));
    if (writer == nullptr)
      return false;
    const float* channel = samples.data();
    if (!writer->writeFromFloatArrays(&channel, 1, static_cast<int>(samples.size())))
      return false;
  }
  return temporary.overwriteTargetFileWithTemporary();
}

// Mono samples of a WAV at the capture rate. `error` set when not.
bool readMono(const juce::File& file, std::vector<float>& out, juce::String& error,
              double maxSeconds = kMaxStandardInputSeconds) {
  juce::AudioBuffer<float> audio;
  double rate = 0.0;
  if (!AudioFiles::read(file, audio, rate, error, maxSeconds))
    return false;
  if (std::abs(rate - Capture::kCaptureSampleRate) > 0.5) {
    error = file.getFileName() + " is " + juce::String(rate, 0) +
            " Hz; captures work at 48000 Hz.";
    return false;
  }
  out.assign(audio.getReadPointer(0), audio.getReadPointer(0) + audio.getNumSamples());
  return true;
}

}  // namespace

// ####################
// THE STANDARD INPUT FILE
// ####################
// The trainer recognises the files the NAM project publishes by checksum (a
// strong match of the whole file) and, failing that, by hashes of stretches of
// the decoded samples. Only the strong match is checked here: a file that
// matches is known good, and one that does not is held back unless the caller
// says it knows better. 0.13.0's standard training accepts the v3 file only
// (older ones train with the trainer's checks failed).
const juce::StringArray& CaptureStudio::knownInputChecksums() {
  static const juce::StringArray sums{"36cd1af62985c2fac3e654333e36431e"};  // v3.0.0
  return sums;
}

CaptureStudio::StandardInput CaptureStudio::identifyInput(const juce::File& file) {
  StandardInput info;
  if (file == juce::File() || !file.existsAsFile()) {
    info.message = file == juce::File() ? "The standard input file is not set."
                                        : "There is no file at " + file.getFullPathName() + ".";
    return info;
  }
  info.exists = true;
  AudioFiles::Info header;
  juce::String error;
  if (!AudioFiles::inspect(file, header, error, kMaxStandardInputSeconds)) {
    info.message = error;
    return info;
  }
  info.readable = true;
  info.seconds = header.seconds();
  info.sampleRate = header.sampleRate;
  info.md5 = juce::MD5(file).toHexString();
  if (knownInputChecksums().contains(info.md5)) {
    info.recognised = true;
    info.version = "3.0.0";
    info.message = "The NAM standard input file, version 3.";
  } else {
    info.message =
        "This is not the NAM project's standard input file (version 3), so the trainer is likely "
        "to refuse it. Download v3_0_0.wav (input.wav) from the NAM trainer's documentation.";
  }
  if (std::abs(info.sampleRate - Capture::kCaptureSampleRate) > 0.5)
    info.message += " It is " + juce::String(info.sampleRate, 0) + " Hz; captures work at 48000 Hz.";
  return info;
}

// ####################
// CONSTRUCTION
// ####################
CaptureStudio::CaptureStudio(TONE3000Processor& liveRig, CommandRegistry& liveCommands)
    : live(liveRig), registry(liveCommands) {}

CaptureStudio::~CaptureStudio() {
  stopAll();
}

void CaptureStudio::stopAll() {
  stopTimer();
  live.captureAudio.stop();
  run.reset();
  audioOwner.clear();
  trainer.cancel();
  if (writer.joinable())
    writer.join();
}

juce::File CaptureStudio::capturesDir() const {
  return LibraryPaths::capturesDir(live.getLibraryRoot());
}

juce::File CaptureStudio::sessionDir(const juce::String& id) const {
  return capturesDir().getChildFile(id);
}

bool CaptureStudio::validId(const juce::String& id) {
  if (id.isEmpty() || id.length() > 80)
    return false;
  for (const juce::juce_wchar c : id)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
      return false;
  return true;
}

bool CaptureStudio::load(const juce::String& id, CaptureSession& session, juce::String& error) const {
  if (!validId(id)) {
    error = "'" + id + "' is not a capture id";
    return false;
  }
  const juce::File file = sessionDir(id).getChildFile("session.json");
  if (!file.existsAsFile()) {
    error = "no capture with id '" + id + "'";
    return false;
  }
  juce::String text;
  if (const auto read = t3k::presetjson::readTextFile(file, t3k::presetjson::kMaxMetaFileBytes, text);
      read.failed()) {
    error = read.getErrorMessage();
    return false;
  }
  juce::var parsed;
  if (const auto result = t3k::presetjson::parseJsonGuarded(text, parsed, 32); result.failed()) {
    error = "the capture's session file is damaged: " + result.getErrorMessage();
    return false;
  }
  if (!CaptureSession::fromVar(parsed, session, error))
    return false;
  // The folder is the id: a file moved between folders is not trusted on that.
  session.id = id;
  // A run this process started is never "interrupted": the runner can be
  // between its last state and the end of its thread, and the next poll
  // applies its result (applyTrainerResult). Only a run left by an earlier
  // launch is.
  session.markInterrupted(audioOwner == id, trainingId == id);
  return true;
}

bool CaptureStudio::store(const CaptureSession& session, juce::String* error) const {
  const juce::File dir = sessionDir(session.id);
  if (!LibraryPaths::createPrivateDirectory(dir)) {
    if (error != nullptr)
      *error = "could not make the folder " + dir.getFullPathName();
    return false;
  }
  CaptureSession copy = session;
  copy.updated = juce::Time::getCurrentTime().toISO8601(true);
  const auto written = t3k::presetjson::writeFileAtomically(
      dir.getChildFile("session.json"), juce::JSON::toString(copy.toVar(), false));
  if (written.failed() && error != nullptr)
    *error = written.getErrorMessage();
  return written.wasOk();
}

CommandResult CaptureStudio::withSession(const juce::String& id, CaptureSession& session) const {
  juce::String error;
  if (!load(id, session, error))
    return fail(validId(id) ? CommandError::notFound : CommandError::invalidArgs, error);
  return CommandResult::success();
}

void CaptureStudio::deleteStale(const CaptureSession& session,
                                const std::vector<juce::String>& stale) const {
  const juce::File dir = sessionDir(session.id);
  for (const auto& name : stale) {
    // Names of the session folder's own entries, nothing that leaves it.
    const bool folder = name.endsWith("/");
    const juce::String core = folder ? name.dropLastCharacters(1) : name;
    if (core.isEmpty() || core.contains("..") || core.containsAnyOf("/\\:"))
      continue;
    if (folder)
      dir.getChildFile(core).deleteRecursively();
    else
      dir.getChildFile(core).deleteFile();
  }
}

// ####################
// SETTINGS
// ####################
CommandResult CaptureStudio::getSettings() {
  const juce::String python = AgentSettings::capturePython();
  const juce::String input = AgentSettings::captureInputFile();
  const StandardInput info = identifyInput(input.isEmpty() ? juce::File() : juce::File(input));
  return CommandResult::success(object({
      {"pythonExecutable", python},
      {"effectivePython", python.isEmpty() ? TrainerRunner::defaultPython() : python},
      {"standardInputFile", input},
      {"levelTargetDb", AgentSettings::captureTargetDb()},
      {"minimumTrainerVersion", TrainerRunner::kMinimumVersion},
      {"standardInput", object({{"exists", info.exists},
                                {"readable", info.readable},
                                {"recognised", info.recognised},
                                {"version", info.version},
                                {"md5", info.md5},
                                {"seconds", round1(info.seconds)},
                                {"sampleRate", info.sampleRate},
                                {"message", info.message}})},
  }));
}

CommandResult CaptureStudio::setSettings(const juce::var& args) {
  if (!args["pythonExecutable"].isVoid())
    AgentSettings::setCapturePython(args["pythonExecutable"].toString());
  if (!args["standardInputFile"].isVoid()) {
    const juce::String path = args["standardInputFile"].toString().trim();
    if (path.isNotEmpty() && !juce::File::isAbsolutePath(path))
      return fail(CommandError::invalidArgs,
                  "capture.setSettings: 'standardInputFile' must be an absolute path");
    AgentSettings::setCaptureInputFile(path);
  }
  if (!args["levelTargetDb"].isVoid())
    AgentSettings::setCaptureTargetDb(static_cast<double>(args["levelTargetDb"]));
  return getSettings();
}

juce::var CaptureStudio::checkTrainer(const std::atomic<bool>* cancel) {
  const TrainerRunner::Install install =
      TrainerRunner::checkInstall(AgentSettings::capturePython(), cancel);
  return object({{"pythonFound", install.pythonFound},
                 {"packageFound", install.packageFound},
                 {"versionOk", install.versionOk},
                 {"python", install.python},
                 {"version", install.version},
                 {"minimumVersion", TrainerRunner::kMinimumVersion},
                 {"message", install.message}});
}

// ####################
// SESSIONS
// ####################
juce::var CaptureStudio::sessionVar(const CaptureSession& session) const {
  juce::var out = session.toVar();
  auto* o = out.getDynamicObject();
  const juce::File dir = sessionDir(session.id);
  o->setProperty("folder", dir.getFullPathName());
  const juce::String blocks = session.step == Capture::Step::review ? juce::String()
                                                                     : session.whatBlocks();
  o->setProperty("canAdvance", session.step != Capture::Step::review && blocks.isEmpty());
  o->setProperty("blocks", blocks);
  o->setProperty("bandLowDb", session.bandLow());
  o->setProperty("bandHighDb", session.bandHigh());
  o->setProperty("audioActive", audioOwner == session.id);
  juce::Array<juce::var> steps;
  for (int i = 0; i < Capture::kStepCount; ++i)
    steps.add(juce::String(Capture::name(static_cast<Capture::Step>(i))));
  o->setProperty("steps", steps);
  juce::var paths = object();
  const auto add = [&](const char* key, const juce::String& relative) {
    if (relative.isNotEmpty())
      paths.getDynamicObject()->setProperty(key, dir.getChildFile(relative).getFullPathName());
  };
  add("return", session.returnFile);
  add("dry", session.dryFile);
  add("wet", session.wetFile);
  add("model", session.modelFile);
  if (session.savedFile.isNotEmpty())
    paths.getDynamicObject()->setProperty("saved", session.savedFile);
  o->setProperty("paths", paths);
  o->setProperty("calibrationText",
                 "0 dBFS = " + juce::String(session.calibrationDbu >= 0 ? "+" : "") +
                     juce::String(session.calibrationDbu, 1) + " dBu at the return input");
  return out;
}

CommandResult CaptureStudio::get(const juce::String& id) {
  poll();
  CaptureSession session;
  if (auto r = withSession(id, session); !r.ok)
    return r;
  juce::var out = sessionVar(session);
  if (trainingId == id)
    out.getDynamicObject()->setProperty("trainer", trainer.status());
  return CommandResult::success(out);
}

CommandResult CaptureStudio::list() {
  poll();
  juce::Array<juce::var> sessions;
  std::vector<CaptureSession> found;
  for (const auto& entry : juce::RangedDirectoryIterator(capturesDir(), false, "*", juce::File::findDirectories)) {
    CaptureSession session;
    juce::String error;
    if (load(entry.getFile().getFileName(), session, error))
      found.push_back(session);
  }
  std::sort(found.begin(), found.end(),
            [](const CaptureSession& a, const CaptureSession& b) { return a.updated > b.updated; });
  for (const auto& session : found)
    sessions.add(session.summary());
  return CommandResult::success(object({{"captures", sessions},
                                         {"folder", capturesDir().getFullPathName()},
                                         {"active", audioOwner}}));
}

CommandResult CaptureStudio::create(const juce::var& args) {
  const juce::String name = args["name"].toString().trim();
  if (name.isEmpty())
    return fail(CommandError::invalidArgs, "capture.create: 'name' must not be empty");
  CaptureSession session = CaptureSession::make({}, name);

  juce::String id;
  const juce::String base = LibraryPaths::slug(name).substring(0, 40);
  for (int attempt = 0; attempt < 50; ++attempt) {
    id = base + "-" + juce::String::toHexString(juce::Random::getSystemRandom().nextInt(0xffff) + 0x1000);
    if (!sessionDir(id).exists())
      break;
  }
  if (sessionDir(id).exists())
    return fail(CommandError::failed, "could not find a free capture id");
  session.id = id;
  session.levels.targetDb = AgentSettings::captureTargetDb();

  // The same fields setSource edits; a new capture starts from the defaults.
  Capture::parse(args["source"].toString(), session.source);
  Capture::parse(args["method"].toString(), session.method);
  Capture::parse(args["gainTag"].toString(), session.gainTag);
  Capture::parse(args["modelSize"].toString(), session.modelSize);
  session.gear.type = Capture::defaultGearType(session.source);
  if (const juce::String type = args["gearType"].toString(); type.isNotEmpty()) {
    if (!Capture::gearTypes().contains(type))
      return fail(CommandError::invalidArgs, "capture.create: unknown gear type '" + type + "'");
    session.gear.type = type;
  }
  session.gear.make = args["gearMake"].toString();
  session.gear.model = args["gearModel"].toString();
  session.notes = args["notes"].toString();
  if (!args["calibrationDbu"].isVoid())
    session.calibrationDbu = static_cast<double>(args["calibrationDbu"]);

  juce::String error;
  if (!store(session, &error))
    return fail(CommandError::failed, "could not save the capture: " + error);
  return CommandResult::success(sessionVar(session));
}

CommandResult CaptureStudio::setSource(const juce::var& args) {
  CaptureSession session;
  if (auto r = withSession(args["id"].toString(), session); !r.ok)
    return r;

  const bool shapesTheRun = !args["source"].isVoid() || !args["method"].isVoid();
  if (shapesTheRun && static_cast<int>(session.step) > static_cast<int>(Capture::Step::wiring))
    return wrongStep("the source and the method can only change before the levels step", session);
  if (!args["name"].isVoid()) {
    const juce::String name = args["name"].toString().trim();
    if (name.isEmpty())
      return fail(CommandError::invalidArgs, "capture.setSource: 'name' must not be empty");
    session.name = name;
  }
  const Capture::Source before = session.source;
  if (!args["source"].isVoid())
    Capture::parse(args["source"].toString(), session.source);
  if (!args["method"].isVoid())
    Capture::parse(args["method"].toString(), session.method);
  if (!args["gainTag"].isVoid())
    Capture::parse(args["gainTag"].toString(), session.gainTag);
  if (!args["modelSize"].isVoid())
    Capture::parse(args["modelSize"].toString(), session.modelSize);
  // A gear type still at the old source's default follows the new source.
  if (session.source != before && session.gear.type == Capture::defaultGearType(before))
    session.gear.type = Capture::defaultGearType(session.source);
  if (!args["gearType"].isVoid()) {
    const juce::String type = args["gearType"].toString();
    if (!Capture::gearTypes().contains(type))
      return fail(CommandError::invalidArgs, "capture.setSource: unknown gear type '" + type + "'");
    session.gear.type = type;
  }
  if (!args["gearMake"].isVoid())
    session.gear.make = args["gearMake"].toString().trim();
  if (!args["gearModel"].isVoid())
    session.gear.model = args["gearModel"].toString().trim();
  if (!args["notes"].isVoid())
    session.notes = args["notes"].toString();

  juce::String error;
  if (!store(session, &error))
    return fail(CommandError::failed, "could not save the capture: " + error);
  return CommandResult::success(sessionVar(session));
}

CommandResult CaptureStudio::setChannels(const juce::var& args) {
  CaptureSession session;
  if (auto r = withSession(args["id"].toString(), session); !r.ok)
    return r;
  if (static_cast<int>(session.step) > static_cast<int>(Capture::Step::levels) ||
      audioOwner == session.id)
    return wrongStep("the channels can only change before recording", session);

  if (!args["send"].isVoid())
    session.channels.send = static_cast<int>(args["send"]);
  if (!args["returnIn"].isVoid())
    session.channels.returnIn = static_cast<int>(args["returnIn"]);
  if (!args["dry"].isVoid())
    session.channels.dry = static_cast<int>(args["dry"]);
  if (!args["wet"].isVoid())
    session.channels.wet = static_cast<int>(args["wet"]);
  if (!args["calibrationDbu"].isVoid())
    session.calibrationDbu = static_cast<double>(args["calibrationDbu"]);
  if (!args["sendCalibrationDbu"].isVoid())
    session.sendCalibrationDbu = static_cast<double>(args["sendCalibrationDbu"]);
  if (session.method == Capture::Method::dryWet && session.channels.dry == session.channels.wet)
    return fail(CommandError::invalidArgs,
                "capture.setChannels: the dry and wet inputs must be different channels");

  juce::String error;
  if (!store(session, &error))
    return fail(CommandError::failed, "could not save the capture: " + error);

  // The processor has the host's channels only (the main stereo pair today);
  // a channel beyond them plays or records on the last one. Said, not hidden.
  juce::Array<juce::var> warnings;
  const int inputs = live.getTotalNumInputChannels(), outputs = live.getTotalNumOutputChannels();
  const auto beyond = [&](int channel, int available, const char* what) {
    if (channel >= available)
      warnings.add(juce::String(what) + " " + juce::String(channel + 1) + " is not available: the " +
                   "device has " + juce::String(available) + " and the capture uses the last one.");
  };
  if (session.method == Capture::Method::reamp) {
    beyond(session.channels.send, outputs, "Output");
    beyond(session.channels.returnIn, inputs, "Input");
  } else {
    beyond(session.channels.dry, inputs, "Input");
    beyond(session.channels.wet, inputs, "Input");
  }
  juce::var out = sessionVar(session);
  out.getDynamicObject()->setProperty("deviceInputs", inputs);
  out.getDynamicObject()->setProperty("deviceOutputs", outputs);
  out.getDynamicObject()->setProperty("warnings", warnings);
  return CommandResult::success(out);
}

CommandResult CaptureStudio::setLevels(const juce::var& args) {
  CaptureSession session;
  if (auto r = withSession(args["id"].toString(), session); !r.ok)
    return r;
  const bool targetChange = !args["targetDb"].isVoid() || !args["bandDb"].isVoid();
  if (targetChange && static_cast<int>(session.step) > static_cast<int>(Capture::Step::levels))
    return wrongStep("the level target can only change before recording", session);

  if (!args["sendDb"].isVoid())
    session.levels.sendDb = static_cast<double>(args["sendDb"]);
  if (!args["targetDb"].isVoid())
    session.levels.targetDb = static_cast<double>(args["targetDb"]);
  if (!args["bandDb"].isVoid())
    session.levels.bandDb = static_cast<double>(args["bandDb"]);
  if (!args["recordSeconds"].isVoid()) {
    if (session.recording.state == "recording" || session.recording.state == "writing")
      return wrongStep("the recording time cannot change during a recording", session);
    session.recording.plannedSeconds = static_cast<double>(args["recordSeconds"]);
  }
  // A new target or send level makes what was measured stale.
  const bool changesTheReading = !args["sendDb"].isVoid() || targetChange;
  if (changesTheReading) {
    session.levels.measured = false;
    session.levels.inRange = false;
  }

  if (audioOwner == session.id && run != nullptr && run->kind == Run::Kind::levels) {
    if (!args["sendDb"].isVoid())
      live.captureAudio.setSendLevel(static_cast<float>(session.levels.sendDb));
    if (changesTheReading)
      live.captureAudio.resetHold();
  }
  juce::String error;
  if (!store(session, &error))
    return fail(CommandError::failed, "could not save the capture: " + error);
  return CommandResult::success(sessionVar(session));
}

CommandResult CaptureStudio::setStep(const juce::var& args) {
  poll();
  CaptureSession session;
  if (auto r = withSession(args["id"].toString(), session); !r.ok)
    return r;
  Capture::Step target;
  if (!Capture::parse(args["step"].toString(), target))
    return fail(CommandError::invalidArgs, "capture.setStep: unknown step '" + args["step"].toString() + "'");

  const bool forward = static_cast<int>(target) > static_cast<int>(session.step);
  const bool busyRecording = session.recording.state == "recording" || session.recording.state == "writing";
  if (!forward && busyRecording)
    return fail(CommandError::failed, "a recording is going; cancel it first");
  if (!forward && trainingId == session.id && trainer.isActive())
    return fail(CommandError::failed, "training is going; cancel it first");

  // The readings of a levels run that is going on count when leaving the step.
  if (session.step == Capture::Step::levels && audioOwner == session.id)
    refreshLevels(session);
  if (forward && session.step == Capture::Step::check && args["overrideChecks"].isBool() &&
      static_cast<bool>(args["overrideChecks"]) && !session.checks.isVoid())
    session.checksOverridden = true;

  if (const juce::String why = session.whyCannotMoveTo(target); why.isNotEmpty())
    return fail(CommandError::failed, why);

  // Leaving the levels step ends its tone and meters.
  if (session.step == Capture::Step::levels && target != Capture::Step::levels &&
      audioOwner == session.id && run != nullptr && run->kind == Run::Kind::levels) {
    live.captureAudio.stop();
    run.reset();
    audioOwner.clear();
    updateTimer();
  }
  const std::vector<juce::String> stale = session.moveTo(target);
  juce::String error;
  if (!store(session, &error))
    return fail(CommandError::failed, "could not save the capture: " + error);
  deleteStale(session, stale);
  return CommandResult::success(sessionVar(session));
}

CommandResult CaptureStudio::remove(const juce::String& id) {
  CaptureSession session;
  if (auto r = withSession(id, session); !r.ok)
    return r;
  if (audioOwner == id) {
    live.captureAudio.stop();
    run.reset();
    audioOwner.clear();
  }
  if (trainingId == id && trainer.isActive())
    trainer.cancel();
  if (writer.joinable()) {
    writer.join();
    writing.store(false);
    const std::lock_guard<std::mutex> lock(writtenMutex);
    written = {};
  }
  // The folder holds the recordings and the unsaved model. A saved capture is
  // a file of the library and stays.
  if (!sessionDir(id).deleteRecursively())
    return fail(CommandError::failed, "could not delete " + sessionDir(id).getFullPathName());
  updateTimer();
  return CommandResult::success(object({{"deleted", id}, {"savedFileKept", session.savedFile}}));
}

// ####################
// LEVELS
// ####################
juce::String CaptureStudio::audioRefusal() const {
  const auto wrapper = live.wrapperType;
  if (wrapper != juce::AudioProcessor::wrapperType_Standalone &&
      wrapper != juce::AudioProcessor::wrapperType_Undefined)
    return "Capturing runs in the standalone application only, not in a plugin inside a host.";
  if (live.getSampleRate() <= 0.0)
    return "The audio device is not running.";
  return {};
}

CommandResult CaptureStudio::audioBusy(const juce::String& id) const {
  if (audioOwner.isNotEmpty() && audioOwner != id)
    return fail(CommandError::failed,
                "another capture (" + audioOwner + ") is using the audio device; stop it first");
  return CommandResult::success();
}

void CaptureStudio::refreshLevels(CaptureSession& session) {
  if (run == nullptr || run->kind != Run::Kind::levels || audioOwner != session.id)
    return;
  const CaptureAudio& engine = live.captureAudio;
  const auto a = engine.meter(0);
  session.levels.peakDb = a.holdDb;
  session.levels.rmsDb = a.rmsDb;
  int clipped = a.clippedSamples;
  bool inRange = a.holdDb >= session.bandLow() && a.holdDb <= session.bandHigh();
  if (session.method == Capture::Method::dryWet) {
    const auto b = engine.meter(1);
    session.levels.wetPeakDb = b.holdDb;
    clipped += b.clippedSamples;
    inRange = inRange && b.holdDb >= session.bandLow() && b.holdDb <= session.bandHigh();
  }
  session.levels.clippedSamples = clipped;
  session.levels.measured = a.holdDb > -90.0f;
  session.levels.inRange = session.levels.measured && inRange && clipped == 0;
  if (engine.getNoiseState() == CaptureAudio::NoiseState::done)
    session.levels.noiseFloorDb = engine.noiseRmsDb();
}

juce::var CaptureStudio::levelsVar(const CaptureSession& session, bool active) const {
  const CaptureAudio& engine = live.captureAudio;
  juce::var out = object();
  auto* o = out.getDynamicObject();
  o->setProperty("id", session.id);
  o->setProperty("active", active);
  o->setProperty("method", Capture::name(session.method));
  o->setProperty("tone", active && engine.toneOn());
  o->setProperty("sendDb", session.levels.sendDb);
  o->setProperty("target", object({{"db", session.levels.targetDb},
                                   {"bandDb", session.levels.bandDb},
                                   {"lowDb", session.bandLow()},
                                   {"highDb", session.bandHigh()}}));
  const auto meterVar = [&](const CaptureAudio::Meter& m) {
    return object({{"peakDb", round1(m.peakDb)},
                   {"holdDb", round1(m.holdDb)},
                   {"rmsDb", round1(m.rmsDb)},
                   {"clippedSamples", m.clippedSamples}});
  };
  if (active) {
    o->setProperty(session.method == Capture::Method::reamp ? "returnLevel" : "dryLevel",
                   meterVar(engine.meter(0)));
    if (session.method == Capture::Method::dryWet)
      o->setProperty("wetLevel", meterVar(engine.meter(1)));
  } else {
    o->setProperty("returnLevel", object({{"holdDb", round1(session.levels.peakDb)},
                                          {"rmsDb", round1(session.levels.rmsDb)},
                                          {"clippedSamples", session.levels.clippedSamples}}));
  }
  const bool clipping = session.levels.clippedSamples > 0;
  const bool dryWet = session.method == Capture::Method::dryWet;
  const double quietest = dryWet ? std::min(session.levels.peakDb, session.levels.wetPeakDb)
                                 : session.levels.peakDb;
  const double loudest = dryWet ? std::max(session.levels.peakDb, session.levels.wetPeakDb)
                                : session.levels.peakDb;
  juce::String state, message;
  if (!session.levels.measured) {
    state = "silent";
    message = "No signal yet.";
  } else if (clipping) {
    state = "clipping";
    message = "The signal clips. Lower the send level or the input gain.";
  } else if (quietest < session.bandLow()) {
    state = "low";
    message = "The level is below the target band. Raise the send level or the input gain.";
  } else if (loudest > session.bandHigh()) {
    state = "high";
    message = "The level is above the target band. Lower the send level or the input gain.";
  } else {
    state = "inRange";
    message = "In range.";
  }
  o->setProperty("state", state);
  o->setProperty("message", message);
  o->setProperty("inRange", session.levels.inRange);
  o->setProperty("measured", session.levels.measured);

  juce::var noise = object({{"state", "none"}});
  if (active && engine.getNoiseState() == CaptureAudio::NoiseState::measuring)
    noise = object({{"state", "measuring"}});
  else if (session.levels.noiseFloorDb.has_value())
    noise = object({{"state", "done"}, {"rmsDb", round1(*session.levels.noiseFloorDb)}});
  o->setProperty("noiseFloor", noise);
  o->setProperty("calibration", object({{"returnDbu", session.calibrationDbu},
                                        {"text", "0 dBFS = " + juce::String(session.calibrationDbu >= 0 ? "+" : "") +
                                                     juce::String(session.calibrationDbu, 1) + " dBu"}}));
  return out;
}

CommandResult CaptureStudio::startTone(const juce::var& args) {
  poll();
  CaptureSession session;
  if (auto r = withSession(args["id"].toString(), session); !r.ok)
    return r;
  if (session.step != Capture::Step::levels)
    return wrongStep("the test tone plays in the levels step", session);
  if (const juce::String refusal = audioRefusal(); refusal.isNotEmpty())
    return fail(CommandError::failed, refusal);
  if (auto busy = audioBusy(session.id); !busy.ok)
    return busy;

  if (!args["sendDb"].isVoid())
    session.levels.sendDb = static_cast<double>(args["sendDb"]);
  const bool dryWet = session.method == Capture::Method::dryWet;
  CaptureAudio::Routing routing;
  routing.send = session.channels.send;
  routing.listenA = dryWet ? session.channels.dry : session.channels.returnIn;
  routing.listenB = dryWet ? session.channels.wet : -1;
  // Dry/wet plays nothing: the levels step only meters the two inputs while
  // the guitar is played.
  live.captureAudio.startLevels(live.getSampleRate(), routing,
                                static_cast<float>(session.levels.sendDb), !dryWet);
  run = std::make_unique<Run>();
  run->id = session.id;
  run->kind = Run::Kind::levels;
  run->method = session.method;
  audioOwner = session.id;
  session.levels.measured = false;
  session.levels.inRange = false;
  juce::String error;
  if (!store(session, &error))
    return fail(CommandError::failed, "could not save the capture: " + error);
  updateTimer();
  return CommandResult::success(levelsVar(session, true));
}

CommandResult CaptureStudio::stopTone(const juce::String& id) {
  CaptureSession session;
  if (auto r = withSession(id, session); !r.ok)
    return r;
  if (audioOwner == id && run != nullptr && run->kind == Run::Kind::levels) {
    refreshLevels(session);
    live.captureAudio.stop();
    run.reset();
    audioOwner.clear();
    updateTimer();
    juce::String error;
    store(session, &error);
  }
  return CommandResult::success(levelsVar(session, false));
}

CommandResult CaptureStudio::levelStatus(const juce::var& args) {
  CaptureSession session;
  if (auto r = withSession(args["id"].toString(), session); !r.ok)
    return r;
  const bool active = audioOwner == session.id && run != nullptr && run->kind == Run::Kind::levels;
  if (active) {
    if (args["resetHold"].isBool() && static_cast<bool>(args["resetHold"])) {
      live.captureAudio.resetHold();
      session.levels.measured = false;
      session.levels.inRange = false;
      session.levels.clippedSamples = 0;
    } else {
      refreshLevels(session);
    }
    juce::String error;
    store(session, &error);
  }
  return CommandResult::success(levelsVar(session, active));
}

CommandResult CaptureStudio::measureNoise(const juce::var& args) {
  CaptureSession session;
  if (auto r = withSession(args["id"].toString(), session); !r.ok)
    return r;
  if (!(audioOwner == session.id && run != nullptr && run->kind == Run::Kind::levels))
    return fail(CommandError::failed, "start the levels run first (capture.startTone)");
  live.captureAudio.measureNoise(args["seconds"].isVoid() ? 2.0 : static_cast<double>(args["seconds"]));
  return CommandResult::success(levelsVar(session, true));
}

// ####################
// RECORDING
// ####################
CommandResult CaptureStudio::startRecording(const juce::var& args) {
  poll();
  CaptureSession session;
  if (auto r = withSession(args["id"].toString(), session); !r.ok)
    return r;
  if (session.step != Capture::Step::record)
    return wrongStep("recording happens in the record step", session);
  if (session.recording.complete())
    return fail(CommandError::failed,
                "this capture already has a recording; cancel it (capture.cancelRecording) to "
                "record again");
  if (session.recording.state == "recording" || session.recording.state == "writing")
    return fail(CommandError::failed, "a recording is already going");
  if (const juce::String refusal = audioRefusal(); refusal.isNotEmpty())
    return fail(CommandError::failed, refusal);
  if (auto busy = audioBusy(session.id); !busy.ok)
    return busy;
  const double rate = live.getSampleRate();
  if (std::abs(rate - Capture::kCaptureSampleRate) > 0.5)
    return fail(CommandError::failed,
                "Captures are recorded at 48 kHz; the audio device runs at " +
                    juce::String(rate, 0) + " Hz. Set the device to 48 kHz first.");

  CaptureAudio::Routing routing;
  routing.send = session.channels.send;
  auto newRun = std::make_unique<Run>();
  newRun->id = session.id;
  newRun->kind = Run::Kind::recording;
  newRun->method = session.method;

  if (session.method == Capture::Method::reamp) {
    const juce::File inputFile(AgentSettings::captureInputFile());
    const StandardInput input = identifyInput(inputFile);
    if (!input.exists || !input.readable)
      return fail(CommandError::failed,
                  "The trainer's standard input file is needed to re-amp: " + input.message +
                      " Set it with capture.setSettings (standardInputFile).");
    const bool allow = args["allowUnrecognisedInput"].isBool() &&
                       static_cast<bool>(args["allowUnrecognisedInput"]);
    if (!input.recognised && !allow)
      return fail(CommandError::failed,
                  input.message + " Pass allowUnrecognisedInput to use it anyway.");
    std::vector<float> signal;
    juce::String error;
    if (!readMono(inputFile, signal, error))
      return fail(CommandError::failed, error);
    float peak = 0.0f;
    for (const float v : signal)
      peak = std::max(peak, std::abs(v));
    if (peak <= 0.0f)
      return fail(CommandError::failed, "The standard input file is silent.");
    // The file's peak goes out at the send level the tone had in the levels
    // step, so what was set there is what the gear gets.
    const float scale = CaptureAudio::dbToLinear(static_cast<float>(session.levels.sendDb)) / peak;
    for (float& v : signal)
      v *= scale;
    routing.listenA = session.channels.returnIn;
    const int preRoll = static_cast<int>(rate * kPreRollMs / 1000.0);
    const int tail = static_cast<int>(rate * kTailMs / 1000.0);
    newRun->playedSamples = static_cast<int>(signal.size());
    newRun->preRoll = preRoll;
    newRun->plannedSamples = preRoll + newRun->playedSamples + tail;
    const size_t window = static_cast<size_t>(kLiveLatencyWindowSeconds * rate);
    newRun->reference.assign(signal.begin(), signal.begin() + static_cast<std::ptrdiff_t>(std::min(window, signal.size())));
    session.recording.inputFile = inputFile.getFullPathName();
    session.recording.inputVersion = input.version;
    live.captureAudio.startPlay(rate, routing, std::move(signal), preRoll, tail);
  } else {
    routing.listenA = session.channels.dry;
    routing.listenB = session.channels.wet;
    const double seconds = juce::jlimit(10.0, kMaxPlannedSeconds, session.recording.plannedSeconds);
    newRun->plannedSamples = static_cast<int>(seconds * rate);
    live.captureAudio.startListen(rate, routing, newRun->plannedSamples);
  }

  run = std::move(newRun);
  audioOwner = session.id;
  session.recording.state = "recording";
  session.recording.error.clear();
  session.recording.sampleRate = rate;
  juce::String error;
  if (!store(session, &error)) {
    live.captureAudio.stop();
    run.reset();
    audioOwner.clear();
    return fail(CommandError::failed, "could not save the capture: " + error);
  }
  updateTimer();
  return recordingStatus(session.id);
}

CommandResult CaptureStudio::cancelRecording(const juce::String& id) {
  poll();
  CaptureSession session;
  if (auto r = withSession(id, session); !r.ok)
    return r;
  if (audioOwner == id && run != nullptr && run->kind == Run::Kind::recording) {
    live.captureAudio.stop();
    run.reset();
    audioOwner.clear();
  }
  if (writer.joinable()) {
    writer.join();
    writing.store(false);
    const std::lock_guard<std::mutex> lock(writtenMutex);
    written = {};
  }
  // Whatever state the recording was in, it starts again from nothing.
  std::vector<juce::String> stale;
  if (session.step == Capture::Step::record || session.step == Capture::Step::check) {
    stale = session.clearFrom(Capture::Step::record);
    session.step = Capture::Step::record;
  }
  juce::String error;
  if (!store(session, &error))
    return fail(CommandError::failed, "could not save the capture: " + error);
  deleteStale(session, stale);
  updateTimer();
  return CommandResult::success(recordingVar(session));
}

juce::var CaptureStudio::recordingVar(const CaptureSession& session) const {
  const CaptureAudio& engine = live.captureAudio;
  const bool mine = audioOwner == session.id && run != nullptr && run->kind == Run::Kind::recording;
  juce::var out = object();
  auto* o = out.getDynamicObject();
  o->setProperty("id", session.id);
  o->setProperty("method", Capture::name(session.method));
  juce::String state = session.recording.state;
  const double rate = engine.rate() > 0.0 ? engine.rate() : Capture::kCaptureSampleRate;
  if (mine) {
    state = engine.getRunState() == CaptureAudio::RunState::running ? "recording" : "writing";
    const int total = std::max(1, run->plannedSamples);
    const int done = engine.recordedSamples();
    // Seconds of the signal itself: the pre-roll is not counted.
    const double playedDone =
        session.method == Capture::Method::reamp ? std::max(0, done - run->preRoll) : done;
    const double playedTotal =
        session.method == Capture::Method::reamp ? run->playedSamples : run->plannedSamples;
    o->setProperty("progress", std::min(1.0, static_cast<double>(done) / total));
    o->setProperty("recordedSeconds", round1(std::min(playedDone, playedTotal) / rate));
    o->setProperty("totalSeconds", round1(playedTotal / rate));
    o->setProperty("secondsLeft", round1(std::max(0.0, static_cast<double>(total - done) / rate)));
    const auto a = engine.meter(0);
    o->setProperty(session.method == Capture::Method::reamp ? "returnPeakDb" : "dryPeakDb",
                   round1(a.holdDb));
    o->setProperty("rmsDb", round1(a.rmsDb));
    int clipped = a.clippedSamples;
    if (session.method == Capture::Method::dryWet) {
      const auto b = engine.meter(1);
      o->setProperty("wetPeakDb", round1(b.holdDb));
      clipped += b.clippedSamples;
    }
    o->setProperty("clippedSamples", clipped);
    if (run->latencyKnown)
      o->setProperty("latency", object({{"samples", run->latencySamples},
                                        {"ms", round2(1000.0 * run->latencySamples / rate)}}));
  } else {
    o->setProperty("progress", session.recording.complete() ? 1.0 : 0.0);
    o->setProperty("recordedSeconds", round1(session.recording.seconds));
    o->setProperty("totalSeconds", round1(session.recording.seconds));
  }
  o->setProperty("state", state);
  if (session.recording.error.isNotEmpty())
    o->setProperty("error", session.recording.error);
  o->setProperty("complete", session.recording.complete());
  return out;
}

CommandResult CaptureStudio::recordingStatus(const juce::String& id) {
  poll();
  CaptureSession session;
  if (auto r = withSession(id, session); !r.ok)
    return r;
  // The delay, once there is enough recorded to find it: the start of the
  // played signal against the start of what came back.
  if (audioOwner == id && run != nullptr && run->kind == Run::Kind::recording &&
      run->method == Capture::Method::reamp && !run->latencyTried && !run->reference.empty()) {
    const int window = static_cast<int>(run->reference.size());
    const int lag = CaptureChecks::Options().searchLagSamples;
    if (live.captureAudio.recordedSamples() >= run->preRoll + window + lag) {
      run->latencyTried = true;
      std::vector<float> response;
      if (live.captureAudio.copyRecorded(0, run->preRoll, window + lag, response)) {
        const auto estimate = CaptureChecks::estimateLatency(run->reference, response, lag, window);
        if (estimate.found && estimate.confidence >= 0.2) {
          run->latencyKnown = true;
          run->latencySamples = estimate.samples;
        }
      }
    }
  }
  return CommandResult::success(recordingVar(session));
}

// ####################
// WORK THAT FINISHES BEHIND THE SCENES
// ####################
void CaptureStudio::updateTimer() {
  const bool needed = run != nullptr || writing.load() || (trainer.isActive()) ||
                      (trainingId.isNotEmpty() && trainingState != trainer.state());
  if (needed && !isTimerRunning())
    startTimerHz(10);
  else if (!needed && isTimerRunning())
    stopTimer();
}

void CaptureStudio::timerCallback() {
  poll();
  updateTimer();
}

void CaptureStudio::poll() {
  applyFinishedRecording();
  applyTrainerResult();
}

void CaptureStudio::applyFinishedRecording() {
  // A recording that has filled its buffer goes to a thread that writes it.
  if (run != nullptr && run->kind == Run::Kind::recording && !writing.load() &&
      live.captureAudio.getRunState() == CaptureAudio::RunState::full) {
    std::vector<float> a, b;
    if (live.captureAudio.take(a, b)) {
      const juce::String id = run->id;
      const juce::File dir = sessionDir(id);
      const bool dryWet = run->method == Capture::Method::dryWet;
      const int preRoll = run->preRoll, played = run->playedSamples;
      const double rate = live.captureAudio.rate();
      {
        const std::lock_guard<std::mutex> lock(writtenMutex);
        written = {};
        written.id = id;
      }
      if (writer.joinable())
        writer.join();
      writing.store(true);
      writer = std::thread([this, a = std::move(a), b = std::move(b), id, dir, dryWet, preRoll,
                            played, rate]() mutable {
        Written result;
        result.id = id;
        if (dryWet) {
          result.ok = writeWav24(dir.getChildFile("dry.wav"), a, rate) &&
                      writeWav24(dir.getChildFile("wet.wav"), b, rate);
          result.dryFile = "dry.wav";
          result.wetFile = "wet.wav";
          result.samples = static_cast<int>(a.size());
        } else {
          // From the first played sample on, exactly as long as the signal:
          // what the trainer wants next to the input file.
          std::vector<float> cut;
          if (static_cast<int>(a.size()) >= preRoll + played)
            cut.assign(a.begin() + preRoll, a.begin() + preRoll + played);
          result.ok = !cut.empty() && writeWav24(dir.getChildFile("return.wav"), cut, rate);
          result.returnFile = "return.wav";
          result.samples = static_cast<int>(cut.size());
        }
        if (!result.ok)
          result.error = "Could not write the recording to " + dir.getFullPathName() + ".";
        result.done = true;
        const std::lock_guard<std::mutex> lock(writtenMutex);
        written = std::move(result);
      });
    }
  }

  if (!writing.load())
    return;
  Written result;
  {
    const std::lock_guard<std::mutex> lock(writtenMutex);
    if (!written.done)
      return;
    result = written;
  }
  if (writer.joinable())
    writer.join();
  writing.store(false);
  {
    const std::lock_guard<std::mutex> lock(writtenMutex);
    written = {};
  }
  const juce::String id = result.id;
  if (run != nullptr && run->id == id)
    run.reset();
  if (audioOwner == id)
    audioOwner.clear();

  CaptureSession session;
  juce::String error;
  if (!load(id, session, error))
    return;  // deleted meanwhile
  if (result.ok) {
    session.recording.state = "complete";
    session.recording.samples = result.samples;
    session.recording.sampleRate = Capture::kCaptureSampleRate;
    session.recording.seconds = result.samples / Capture::kCaptureSampleRate;
    session.recording.error.clear();
    session.returnFile = result.returnFile;
    session.dryFile = result.dryFile;
    session.wetFile = result.wetFile;
  } else {
    session.recording.state = "failed";
    session.recording.error = result.error;
  }
  store(session);
  updateTimer();
}

void CaptureStudio::applyTrainerResult() {
  if (trainingId.isEmpty())
    return;
  const juce::String state = trainer.state();
  if (state == trainingState)
    return;
  const bool terminal = state == "done" || state == "failed" || state == "cancelled";
  if (!terminal) {
    trainingState = state;
    return;
  }
  // The runner sets its state a moment before its thread is done.
  if (trainer.isActive())
    return;
  trainingState = state;

  CaptureSession session;
  juce::String error;
  if (!load(trainingId, session, error))
    return;  // deleted meanwhile
  const juce::var status = trainer.status();
  session.training.epoch = static_cast<int>(status["epoch"]);
  if (status["esr"].isDouble())
    session.training.esr = static_cast<double>(status["esr"]);
  if (!status["latencySamples"].isVoid())
    session.training.latencySamples = static_cast<int>(status["latencySamples"]);
  if (!status["trainerChecksPassed"].isVoid())
    session.training.trainerChecksPassed = static_cast<bool>(status["trainerChecksPassed"]);

  if (state == "done") {
    const juce::File produced(status["modelFile"].toString());
    const juce::File target = sessionDir(session.id).getChildFile("model.nam");
    if (produced.existsAsFile() && produced.copyFileTo(target)) {
      session.training.state = "done";
      session.training.error.clear();
      session.modelFile = "model.nam";
      sessionDir(session.id).getChildFile("training").deleteRecursively();
      session.moveTo(Capture::Step::review);
    } else {
      session.training.state = "failed";
      session.training.error = "The trained model could not be copied into the capture folder.";
    }
  } else if (state == "cancelled") {
    session.training.state = "cancelled";
    session.training.error.clear();
  } else {
    session.training.state = "failed";
    session.training.error = status["error"].toString();
  }
  store(session);
  updateTimer();
}

// ####################
// CHECKS
// ####################
CommandWork CaptureStudio::runChecks(const juce::var& args) {
  poll();
  CaptureSession session;
  if (auto r = withSession(args["id"].toString(), session); !r.ok)
    return CommandWork::done(r);
  if (session.step != Capture::Step::record && session.step != Capture::Step::check)
    return CommandWork::done(wrongStep("the checks run on a finished recording", session));
  if (!session.recording.complete())
    return CommandWork::done(wrongStep("there is no finished recording to check", session));

  CaptureChecks::Options options;
  options.sampleRate = Capture::kCaptureSampleRate;
  options.targetDb = session.levels.targetDb;
  options.bandDb = session.levels.bandDb;
  const bool dryWet = session.method == Capture::Method::dryWet;
  const juce::File dir = sessionDir(session.id);
  const juce::File first = dir.getChildFile(dryWet ? session.dryFile : session.returnFile);
  const juce::File second = dryWet ? dir.getChildFile(session.wetFile) : juce::File();
  const juce::File playedFile(session.recording.inputFile);
  const juce::String id = session.id;
  const juce::String recordedName = dryWet ? session.dryFile : session.returnFile;
  std::weak_ptr<bool> alive = lifetime;

  return CommandWork::later([this, alive, options, dryWet, first, second, playedFile, id,
                             recordedName]() -> CommandResult {
    std::vector<float> a, b;
    juce::String error;
    if (!readMono(first, a, error, kMaxPlannedSeconds + 10.0))
      return fail(CommandError::failed, "could not read the recording: " + error);
    CaptureChecks::Report report;
    if (dryWet) {
      if (!readMono(second, b, error, kMaxPlannedSeconds + 10.0))
        return fail(CommandError::failed, "could not read the wet recording: " + error);
      report = CaptureChecks::checkDryWet(a, b, options);
    } else {
      if (!readMono(playedFile, b, error))
        return fail(CommandError::failed,
                    "could not read the standard input file that was played: " + error);
      report = CaptureChecks::checkReamp(b, a, options);
    }

    CommandResult result = fail(CommandError::unavailable, "the application is shutting down");
    CommandRegistry::callOnMessageThread(
        [&] {
          if (alive.expired())
            return;
          CaptureSession session;
          juce::String loadError;
          if (!load(id, session, loadError)) {
            result = fail(CommandError::notFound, loadError);
            return;
          }
          // The recording may have been thrown away while the checks ran.
          if (!session.recording.complete() ||
              (dryWet ? session.dryFile : session.returnFile) != recordedName) {
            result = fail(CommandError::failed, "the recording changed while it was checked");
            return;
          }
          session.checks = report.toVar();
          session.checksOverridden = false;
          if (session.step == Capture::Step::record)
            session.moveTo(Capture::Step::check);
          juce::String saveError;
          store(session, &saveError);
          result = CommandResult::success(sessionVar(session));
        },
        15000);
    return result;
  });
}

// ####################
// TRAINING
// ####################
CommandResult CaptureStudio::startTraining(const juce::var& args) {
  poll();
  CaptureSession session;
  if (auto r = withSession(args["id"].toString(), session); !r.ok)
    return r;
  if (trainer.isActive())
    return fail(CommandError::failed, "a training run is already going (" + trainingId + ")");
  if (session.step != Capture::Step::check && session.step != Capture::Step::train &&
      session.step != Capture::Step::review)
    return wrongStep("training starts from the check step", session);

  if (session.step == Capture::Step::check && args["overrideChecks"].isBool() &&
      static_cast<bool>(args["overrideChecks"]) && !session.checks.isVoid())
    session.checksOverridden = true;
  if (session.step == Capture::Step::check) {
    if (const juce::String why = session.whyCannotMoveTo(Capture::Step::train); why.isNotEmpty())
      return fail(CommandError::failed, why);
  }
  if (!session.recording.complete())
    return wrongStep("there is no recording to train on", session);

  const bool dryWet = session.method == Capture::Method::dryWet;
  const juce::File dir = sessionDir(session.id);
  TrainerRunner::Job job;
  job.python = AgentSettings::capturePython();
  job.mode = dryWet ? "dryWet" : "reamp";
  job.workDir = dir;
  job.outDir = dir.getChildFile("training");
  job.modelName = "model";
  job.epochs = args["epochs"].isVoid() ? session.training.epochs : static_cast<int>(args["epochs"]);
  job.ignoreChecks = args["ignoreTrainerChecks"].isBool() &&
                     static_cast<bool>(args["ignoreTrainerChecks"]);
  job.metadata = session.namMetadata();
  if (dryWet) {
    job.input = dir.getChildFile(session.dryFile);
    job.output = dir.getChildFile(session.wetFile);
    // The trainer cannot calibrate a delay on playing, so it is given ours.
    job.latency = session.checks["latencySamples"].isVoid()
                      ? 0
                      : static_cast<int>(session.checks["latencySamples"]);
    job.validationSamples = juce::jlimit(
        static_cast<int>(9 * Capture::kCaptureSampleRate),
        static_cast<int>(30 * Capture::kCaptureSampleRate),
        session.recording.samples / 10);
    job.validationSamples = std::min(job.validationSamples, session.recording.samples / 2);
  } else {
    job.input = juce::File(session.recording.inputFile);
    job.output = dir.getChildFile(session.returnFile);
    // The trainer finds the delay itself on the standard file unless told.
    if (!args["latency"].isVoid())
      job.latency = static_cast<int>(args["latency"]);
  }
  if (!job.input.existsAsFile() || !job.output.existsAsFile())
    return fail(CommandError::failed, "the recording files are missing: " + job.input.getFullPathName() +
                                          " / " + job.output.getFullPathName());

  // Start over: a retrain must not find an earlier run's files.
  const std::vector<juce::String> stale = session.step == Capture::Step::check
                                              ? std::vector<juce::String>{"training/"}
                                              : session.clearFrom(Capture::Step::train);
  deleteStale(session, stale);
  if (session.step != Capture::Step::train)
    session.step = Capture::Step::train;
  session.training = {};
  session.training.epochs = job.epochs;
  session.training.state = "running";
  juce::String error;
  if (!store(session, &error))
    return fail(CommandError::failed, "could not save the capture: " + error);

  if (const auto started = trainer.start(job); started.failed()) {
    session.training.state = "failed";
    session.training.error = started.getErrorMessage();
    store(session);
    return fail(CommandError::failed, started.getErrorMessage());
  }
  trainingId = session.id;
  trainingState = trainer.state();
  updateTimer();
  return trainingStatus(session.id);
}

CommandResult CaptureStudio::cancelTraining(const juce::String& id) {
  CaptureSession session;
  if (auto r = withSession(id, session); !r.ok)
    return r;
  if (trainingId == id && trainer.isActive())
    trainer.cancel();
  // Give the runner's thread a moment to notice, so the answer is the real state.
  for (int i = 0; i < 100 && trainer.isActive(); ++i)
    juce::Thread::sleep(20);
  return trainingStatus(id);
}

CommandResult CaptureStudio::trainingStatus(const juce::String& id) {
  poll();
  CaptureSession session;
  if (auto r = withSession(id, session); !r.ok)
    return r;
  juce::var out = trainingId == id ? trainer.status() : object({{"state", session.training.state}});
  auto* o = out.getDynamicObject();
  o->setProperty("id", id);
  o->setProperty("step", Capture::name(session.step));
  // The persisted state wins once the run has been applied to the session.
  if (!(trainingId == id && trainer.isActive())) {
    o->setProperty("state", session.training.state);
    if (session.training.esr.has_value()) {
      o->setProperty("esr", *session.training.esr);
      o->setProperty("esrBand", TrainerRunner::esrBand(*session.training.esr));
      o->setProperty("esrBandLabel", TrainerRunner::esrBandLabel(*session.training.esr));
    }
    if (session.training.error.isNotEmpty())
      o->setProperty("error", session.training.error);
    if (session.training.done())
      o->setProperty("progress", 1.0);
  }
  return CommandResult::success(out);
}

// ####################
// REVIEW
// ####################
CommandResult CaptureStudio::review(const juce::String& id) {
  poll();
  CaptureSession session;
  if (auto r = withSession(id, session); !r.ok)
    return r;
  if (session.step != Capture::Step::review)
    return wrongStep("the review comes after training", session);
  const juce::File model = session.savedFile.isNotEmpty()
                               ? juce::File(session.savedFile)
                               : sessionDir(id).getChildFile(session.modelFile);
  juce::var out = object();
  auto* o = out.getDynamicObject();
  o->setProperty("id", id);
  o->setProperty("name", session.name);
  o->setProperty("file", model.getFullPathName());
  o->setProperty("saved", session.savedFile.isNotEmpty());
  o->setProperty("gainTag", Capture::name(session.gainTag));
  o->setProperty("gearType", session.gear.type);
  o->setProperty("notes", session.notes);
  if (session.training.esr.has_value()) {
    o->setProperty("esr", *session.training.esr);
    o->setProperty("band", TrainerRunner::esrBand(*session.training.esr));
    o->setProperty("bandLabel", TrainerRunner::esrBandLabel(*session.training.esr));
    o->setProperty("recapture", *session.training.esr >= 0.1);
  }
  if (session.training.latencySamples.has_value())
    o->setProperty("latencySamples", *session.training.latencySamples);
  if (session.training.trainerChecksPassed.has_value())
    o->setProperty("trainerChecksPassed", *session.training.trainerChecksPassed);
  o->setProperty("calibration", object({{"returnDbu", session.calibrationDbu},
                                        {"text", "0 dBFS = " + juce::String(session.calibrationDbu >= 0 ? "+" : "") +
                                                     juce::String(session.calibrationDbu, 1) + " dBu"}}));
  o->setProperty("metadata", session.namMetadata());
  NamFile::Info info;
  if (NamFile::read(model, info).wasOk()) {
    o->setProperty("architecture", info.architecture);
    o->setProperty("a2", info.architecture == "SlimmableContainer");
  }
  o->setProperty("sizeBytes", model.getSize());
  o->setProperty("ranges", object({{"great", "under 0.01"}, {"notBad", "under 0.035"},
                                   {"mightBeOkay", "under 0.1"}, {"poor", "under 0.3"},
                                   {"failed", "0.3 and above"}}));
  return CommandResult::success(out);
}

CommandResult CaptureStudio::addToRig(const juce::String& id) {
  CaptureSession session;
  if (auto r = withSession(id, session); !r.ok)
    return r;
  if (session.step != Capture::Step::review)
    return wrongStep("a capture is added to the rig from the review step", session);
  const juce::File model = session.savedFile.isNotEmpty()
                               ? juce::File(session.savedFile)
                               : sessionDir(id).getChildFile(session.modelFile);
  if (!model.existsAsFile())
    return fail(CommandError::notFound, "the trained model is missing: " + model.getFullPathName());
  juce::Array<juce::var> paths;
  paths.add(model.getFullPathName());
  CommandResult added = registry.run("chain.addToneFromFiles", object({{"paths", paths}}));
  if (!added.ok)
    return added;
  juce::var value = added.value;
  if (value.getDynamicObject() != nullptr)
    value.getDynamicObject()->setProperty("file", model.getFullPathName());
  return CommandResult::success(value);
}

CommandResult CaptureStudio::save(const juce::var& args) {
  poll();
  CaptureSession session;
  if (auto r = withSession(args["id"].toString(), session); !r.ok)
    return r;
  if (session.step != Capture::Step::review)
    return wrongStep("a capture is saved from the review step", session);
  if (session.savedFile.isNotEmpty() && juce::File(session.savedFile).existsAsFile())
    return CommandResult::success(object({{"file", session.savedFile}, {"alreadySaved", true}}));
  const juce::File model = sessionDir(session.id).getChildFile(session.modelFile);
  if (!model.existsAsFile())
    return fail(CommandError::notFound, "the trained model is missing: " + model.getFullPathName());

  const juce::File folder = capturesDir();
  if (!LibraryPaths::createPrivateDirectory(folder))
    return fail(CommandError::failed, "could not make " + folder.getFullPathName());
  const juce::String base = session.readableFileName();
  juce::File target = folder.getChildFile(base + ".nam");
  for (int n = 2; target.exists() && n < 1000; ++n)
    target = folder.getChildFile(base + " " + juce::String(n) + ".nam");

  // The capture goes in with its metadata complete: what the wizard knows
  // (name, gear, gain tag, calibration) is set on a copy when the trainer's
  // file does not already say so.
  NamFile::Info info;
  if (const auto read = NamFile::read(model, info); read.failed())
    return fail(CommandError::failed, read.getErrorMessage());
  if (const auto a2 = NamFile::requireA2(info); a2.failed())
    return fail(CommandError::failed, a2.getErrorMessage());
  const juce::var metadata = session.namMetadata();
  if (NamFile::metadataHas(info, metadata)) {
    if (!model.moveFileTo(target))
      return fail(CommandError::failed, "could not move the model to " + target.getFullPathName());
  } else {
    if (const auto written = NamFile::writeCopyWithMetadata(model, target, metadata); written.failed())
      return fail(CommandError::failed, written.getErrorMessage());
    model.deleteFile();
  }
  session.savedFile = target.getFullPathName();
  session.modelFile.clear();
  juce::String error;
  if (!store(session, &error))
    return fail(CommandError::failed, "could not save the capture: " + error);
  return CommandResult::success(object({{"file", target.getFullPathName()},
                                         {"name", session.name},
                                         {"folder", folder.getFullPathName()}}));
}

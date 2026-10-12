#include "GuitarProfiles.h"

// Processor.h first: it pulls in the JUCE module headers, which must come
// before anything that drags in <windows.h>.
#include "Processor.h"

#include "AudioMeasure.h"
#include "LibraryPaths.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace {

constexpr int kProfileVersion = 1;
constexpr int kMaxPositions = 8;
constexpr int kMaxTextLength = 2000;

juce::var object(std::initializer_list<std::pair<const char*, juce::var>> properties) {
  juce::DynamicObject::Ptr obj = new juce::DynamicObject();
  for (const auto& [name, value] : properties)
    obj->setProperty(name, value);
  return juce::var(obj.get());
}

CommandResult fail(const char* code, const juce::String& message) {
  return CommandResult::failure(code, message);
}

CommandResult noGuitar(const juce::String& id) {
  return fail(CommandError::notFound, "no guitar profile with id '" + id + "' (see guitar.list)");
}

// A deep copy, so editing a profile never reaches the var it was read into.
juce::var clone(const juce::var& value) { return juce::JSON::parse(juce::JSON::toString(value)); }

juce::var readJson(const juce::File& file) {
  if (!file.existsAsFile() || file.getSize() > 4 * 1024 * 1024)
    return {};
  juce::var value;
  if (juce::JSON::parse(file.loadFileAsString(), value).failed())
    return {};
  return value;
}

bool writeJson(const juce::File& file, const juce::var& value) {
  return file.getParentDirectory().createDirectory().wasOk() &&
         file.replaceWithText(juce::JSON::toString(value) + "\n", false, false, "\n");
}

// The position as the profile spells it, matched without regard to case or
// punctuation; empty when the profile has no such position.
juce::String matchPosition(const juce::var& profile, const juce::String& position) {
  const juce::String wanted = LibraryPaths::slug(position);
  if (const auto* positions = profile["positions"].getArray())
    for (const auto& known : *positions)
      if (LibraryPaths::slug(known.toString()) == wanted)
        return known.toString();
  return {};
}

juce::StringArray positionsFrom(const juce::var& list, juce::String& error) {
  juce::StringArray positions;
  if (const auto* items = list.getArray())
    for (const auto& item : *items) {
      const juce::String name = item.toString().trim().substring(0, 40);
      if (name.isEmpty())
        continue;
      bool duplicate = false;
      for (const auto& existing : positions)
        duplicate = duplicate || LibraryPaths::slug(existing) == LibraryPaths::slug(name);
      if (!duplicate)
        positions.add(name);
    }
  if (positions.isEmpty())
    error = "'positions' must name at least one pickup position (for example bridge, neck)";
  else if (positions.size() > kMaxPositions)
    error = "'positions' may hold at most " + juce::String(kMaxPositions) + " entries";
  return positions;
}

juce::var stringsVar(const juce::StringArray& strings) {
  juce::Array<juce::var> items;
  for (const auto& text : strings)
    items.add(text);
  return items;
}

}  // namespace

// ####################
// WAV FILES
// ####################
namespace AudioFiles {

namespace {

// Opens `file` as a WAV and holds what its header says against the limits and
// against the file itself, before a single sample is read or a buffer made
// for them. A header is the file's own claim: one that names an hour of
// audio in a file of a few bytes would otherwise have that hour allocated.
std::unique_ptr<juce::AudioFormatReader> openChecked(const juce::File& file, juce::String& error,
                                                     double maxSeconds) {
  const juce::String path = file.getFullPathName();
  if (!file.existsAsFile()) {
    error = "file not found: " + path;
    return nullptr;
  }
  juce::WavAudioFormat wav;
  std::unique_ptr<juce::AudioFormatReader> reader(
      wav.createReaderFor(file.createInputStream().release(), true));
  if (reader == nullptr) {
    error = "not a readable WAV file: " + path;
    return nullptr;
  }
  if (reader->numChannels < 1 || reader->numChannels > static_cast<unsigned int>(kMaxChannels)) {
    error = "a WAV file must be mono or stereo (this one has " +
            juce::String(static_cast<int>(reader->numChannels)) + " channels): " + path;
    return nullptr;
  }
  if (!(reader->sampleRate >= kLowestSampleRate && reader->sampleRate <= kHighestSampleRate)) {
    error = "unsupported WAV sample rate (8 to 192 kHz): " + path;
    return nullptr;
  }
  if (reader->bitsPerSample < 8 || reader->bitsPerSample > 64) {
    error = "unsupported WAV sample format: " + path;
    return nullptr;
  }
  if (reader->lengthInSamples < 1 ||
      static_cast<double>(reader->lengthInSamples) > maxSeconds * reader->sampleRate) {
    error = "the file is empty or longer than " + juce::String(maxSeconds, 0) + " seconds: " + path;
    return nullptr;
  }
  // The length is bounded by now, so this cannot overflow. The file's size
  // includes its headers, which only makes the test lenient by that much.
  const juce::int64 claimedBytes = reader->lengthInSamples *
                                   static_cast<juce::int64>(reader->numChannels) *
                                   static_cast<juce::int64>((reader->bitsPerSample + 7) / 8);
  if (claimedBytes > file.getSize()) {
    error = "the WAV header claims more audio than the file holds: " + path;
    return nullptr;
  }
  return reader;
}

}  // namespace

bool inspect(const juce::File& file, Info& info, juce::String& error, double maxSeconds) {
  const std::unique_ptr<juce::AudioFormatReader> reader = openChecked(file, error, maxSeconds);
  if (reader == nullptr)
    return false;
  info.sampleRate = reader->sampleRate;
  info.channels = static_cast<int>(reader->numChannels);
  info.frames = reader->lengthInSamples;
  return true;
}

bool read(const juce::File& file, juce::AudioBuffer<float>& audio, double& sampleRate,
          juce::String& error, double maxSeconds) {
  const std::unique_ptr<juce::AudioFormatReader> reader = openChecked(file, error, maxSeconds);
  if (reader == nullptr)
    return false;
  sampleRate = reader->sampleRate;
  audio.setSize(static_cast<int>(reader->numChannels), static_cast<int>(reader->lengthInSamples));
  reader->read(&audio, 0, audio.getNumSamples(), 0, true, true);
  return true;
}

bool write(const juce::File& file, const juce::AudioBuffer<float>& audio, double sampleRate) {
  if (!file.getParentDirectory().createDirectory().wasOk())
    return false;
  // FileOutputStream appends to an existing file.
  file.deleteFile();
  std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
  if (stream == nullptr)
    return false;
  juce::WavAudioFormat wav;
  auto writer = wav.createWriterFor(
      stream, juce::AudioFormatWriterOptions{}
                  .withSampleRate(sampleRate)
                  .withNumChannels(audio.getNumChannels())
                  .withBitsPerSample(32)
                  .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
  return writer != nullptr && writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples());
}

}  // namespace AudioFiles

// ####################
// LIBRARY
// ####################
GuitarLibrary::GuitarLibrary(TONE3000Processor& liveRig) : live(liveRig) {}

GuitarLibrary::~GuitarLibrary() {
  live.inputRecorder.cancel();
  joinWriter();
}

void GuitarLibrary::joinWriter() {
  if (writer.joinable())
    writer.join();
}

const juce::StringArray& GuitarLibrary::takeNames() {
  static const juce::StringArray names{"soft", "hard", "chords", "notes"};
  return names;
}

const juce::StringArray& GuitarLibrary::pickupTypes() {
  static const juce::StringArray types{"humbucker", "single-coil", "p90", "active", "piezo",
                                       "other"};
  return types;
}

juce::File GuitarLibrary::guitarsDir() const {
  // Beside the Setlists and Assets of the library the live rig uses.
  return LibraryPaths::guitarsDir(live.getLibraryRoot());
}

juce::File GuitarLibrary::profileDir(const juce::String& id) const {
  // The id is always a slug of our own making; anything else (a path) names
  // no profile.
  if (id.isEmpty() || LibraryPaths::slug(id) != id)
    return {};
  return guitarsDir().getChildFile(id);
}

juce::var GuitarLibrary::readProfile(const juce::String& id) const {
  const juce::File dir = profileDir(id);
  if (dir == juce::File())
    return {};
  const juce::var profile = readJson(dir.getChildFile("profile.json"));
  return profile.getDynamicObject() != nullptr ? profile : juce::var();
}

bool GuitarLibrary::writeProfile(const juce::String& id, const juce::var& profile) const {
  const juce::File dir = profileDir(id);
  return dir != juce::File() && writeJson(dir.getChildFile("profile.json"), profile);
}

juce::String GuitarLibrary::defaultId() const {
  const std::lock_guard<std::mutex> lock(fileMutex);
  const juce::String id =
      readJson(guitarsDir().getChildFile("guitars.json"))["default"].toString();
  if (id.isNotEmpty() && !readProfile(id).isVoid())
    return id;
  return {};
}

juce::var GuitarLibrary::list() const {
  const juce::String defaultGuitar = defaultId();
  const std::lock_guard<std::mutex> lock(fileMutex);
  juce::Array<juce::var> guitars;
  juce::Array<juce::File> dirs =
      guitarsDir().findChildFiles(juce::File::findDirectories, false);
  dirs.sort();
  for (const auto& dir : dirs) {
    const juce::String id = dir.getFileName();
    const juce::var profile = readProfile(id);
    if (profile.isVoid())
      continue;
    guitars.add(object({
        {"id", id},
        {"name", profile["name"]},
        {"pickupType", profile["pickupType"]},
        {"positions", profile["positions"]},
        {"tuning", profile["tuning"]},
        {"takeCount", profile["takes"].size()},
        {"isDefault", id == defaultGuitar},
        {"summary", profile["summary"]},
    }));
  }
  return object({{"guitars", guitars}, {"default", defaultGuitar}});
}

juce::var GuitarLibrary::get(const juce::String& id) const {
  const juce::String defaultGuitar = defaultId();
  const std::lock_guard<std::mutex> lock(fileMutex);
  juce::var profile = readProfile(id);
  if (profile.isVoid())
    return {};
  profile = clone(profile);
  profile.getDynamicObject()->setProperty("isDefault", id == defaultGuitar);
  profile.getDynamicObject()->setProperty("folder", profileDir(id).getFullPathName());
  return profile;
}

CommandResult GuitarLibrary::create(const juce::var& args) {
  const juce::String name = args["name"].toString().trim().substring(0, 80);
  if (name.isEmpty())
    return fail(CommandError::invalidArgs, "guitar.create: argument 'name' must not be empty");
  juce::String error;
  const juce::StringArray positions = positionsFrom(args["positions"], error);
  if (error.isNotEmpty())
    return fail(CommandError::invalidArgs, "guitar.create: " + error);

  juce::String id;
  {
    const std::lock_guard<std::mutex> lock(fileMutex);
    // A second guitar of the same name gets a numbered id.
    const juce::String base = LibraryPaths::slug(name).substring(0, 40);
    id = base;
    for (int n = 2; profileDir(id).exists(); ++n) {
      if (n > 99)
        return fail(CommandError::failed, "too many guitars named '" + name + "'");
      id = base + "-" + juce::String(n);
    }

    const juce::var profile = object({
        {"version", kProfileVersion},
        {"id", id},
        {"name", name},
        {"pickupType", args["pickupType"]},
        {"positions", stringsVar(positions)},
        {"tuning", args["tuning"].toString().substring(0, 80)},
        {"notes", args["notes"].toString().substring(0, kMaxTextLength)},
        {"created", juce::Time::getCurrentTime().toISO8601(true)},
        {"takes", juce::Array<juce::var>()},
        {"summary", summarise(juce::Array<juce::var>())},
    });
    if (!writeProfile(id, profile))
      return fail(CommandError::failed,
                  "could not write the profile under " +
                      guitarsDir().getFullPathName());
  }

  // The first guitar is the default one.
  if (defaultId().isEmpty())
    setDefault(id);
  return CommandResult::success(get(id));
}

CommandResult GuitarLibrary::update(const juce::String& id, const juce::var& args) {
  {
    const std::lock_guard<std::mutex> lock(fileMutex);
    juce::var profile = readProfile(id);
    if (profile.isVoid())
      return noGuitar(id);
    profile = clone(profile);
    auto* obj = profile.getDynamicObject();

    if (!args["name"].isVoid()) {
      const juce::String name = args["name"].toString().trim().substring(0, 80);
      if (name.isEmpty())
        return fail(CommandError::invalidArgs, "guitar.update: argument 'name' must not be empty");
      obj->setProperty("name", name);
    }
    if (!args["pickupType"].isVoid())
      obj->setProperty("pickupType", args["pickupType"]);
    if (!args["tuning"].isVoid())
      obj->setProperty("tuning", args["tuning"].toString().substring(0, 80));
    if (!args["notes"].isVoid())
      obj->setProperty("notes", args["notes"].toString().substring(0, kMaxTextLength));
    if (!args["positions"].isVoid()) {
      juce::String error;
      const juce::StringArray positions = positionsFrom(args["positions"], error);
      if (error.isNotEmpty())
        return fail(CommandError::invalidArgs, "guitar.update: " + error);
      // A position that has takes cannot be dropped from under them.
      if (const auto* takes = profile["takes"].getArray())
        for (const auto& take : *takes) {
          bool kept = false;
          for (const auto& position : positions)
            kept = kept ||
                   LibraryPaths::slug(position) == LibraryPaths::slug(take["position"].toString());
          if (!kept)
            return fail(CommandError::failed,
                        "guitar.update: position '" + take["position"].toString() +
                            "' has recorded takes and cannot be removed");
        }
      obj->setProperty("positions", stringsVar(positions));
    }
    if (!writeProfile(id, profile))
      return fail(CommandError::failed, "could not write the profile");
  }
  return CommandResult::success(get(id));
}

CommandResult GuitarLibrary::remove(const juce::String& id) {
  {
    const std::lock_guard<std::mutex> recordingLock(recordingMutex);
    if (recording.guitar == id && (recording.state == "recording" || recording.state == "writing"))
      return fail(CommandError::failed, "a take is being recorded for this guitar");
  }
  const bool wasDefault = defaultId() == id;
  {
    const std::lock_guard<std::mutex> lock(fileMutex);
    if (readProfile(id).isVoid())
      return noGuitar(id);
    // To the trash, like a deleted preset or setlist (the system's, or the
    // library's own Trash folder where there is none): a profile is
    // recordings the user made, and a mistaken delete should be recoverable.
    const juce::File dir = profileDir(id);
    if (!SetlistLibrary::moveToTrash(dir, live.getLibraryRoot()))
      return fail(CommandError::failed,
                  "could not move " + dir.getFullPathName() + " to the trash");
    if (wasDefault)
      writeJson(guitarsDir().getChildFile("guitars.json"), object({{"default", ""}}));
  }
  return CommandResult::success(list());
}

CommandResult GuitarLibrary::setDefault(const juce::String& id) {
  {
    const std::lock_guard<std::mutex> lock(fileMutex);
    if (readProfile(id).isVoid())
      return noGuitar(id);
    if (!writeJson(guitarsDir().getChildFile("guitars.json"),
                   object({{"default", id}})))
      return fail(CommandError::failed, "could not write the guitar list");
  }
  return CommandResult::success(object({{"default", id}}));
}

juce::var GuitarLibrary::measureClip(const juce::File& file, juce::String& error) {
  juce::AudioBuffer<float> audio;
  double rate = 0.0;
  if (!AudioFiles::read(file, audio, rate, error))
    return {};

  const AudioMeasure::Levels levels = AudioMeasure::measureLevels(audio, rate);
  const AudioMeasure::Spectrum spectrum = AudioMeasure::measureSpectrum(audio, rate);
  // Hum and the noise floor from the first channel: a DI clip is mono, and
  // summing channels could cancel the very thing being looked for.
  const AudioMeasure::Hum hum =
      AudioMeasure::measureHum(audio.getReadPointer(0), audio.getNumSamples(), rate);
  // The noise floor is the level of the clip's quietest stretches, which
  // only means something when the clip has a pause in it: without one (a
  // sustained chord from the first sample to the last) the quietest stretch
  // is still guitar, and no floor is reported.
  const double quietestDb =
      AudioMeasure::noiseFloorDb(audio.getReadPointer(0), audio.getNumSamples(), rate);
  const juce::var floorDb = quietestDb <= levels.rmsDb - 30.0
                                ? juce::var(AudioMeasure::tidyDb(quietestDb))
                                : juce::var();

  return object({
      {"peakDb", AudioMeasure::tidyDb(levels.peakDb)},
      {"rmsDb", AudioMeasure::tidyDb(levels.rmsDb)},
      {"lufs", AudioMeasure::tidyDb(levels.lufs)},
      {"crestDb", AudioMeasure::tidyDb(levels.crestDb)},
      {"centroidHz", std::round(spectrum.centroidHz * 10.0) / 10.0},
      {"lowEndShare", std::round(spectrum.lowEndShare * 10000.0) / 10000.0},
      {"spectrum", AudioMeasure::toVar(spectrum)},
      {"hum", AudioMeasure::toVar(hum)},
      {"noiseFloorDb", floorDb},
      {"sampleRate", rate},
      {"seconds", std::round(audio.getNumSamples() / rate * 1000.0) / 1000.0},
      {"channels", audio.getNumChannels()},
  });
}

// The overall picture of a guitar, from its takes. The classes are rough
// bins of the numbers beside them, for an agent that wants a word: output
// level from the loudest take's peak (below -18 dBFS low, below -6 medium,
// else high), brightness from the mean spectral centroid (below 700 Hz dark,
// below 1400 Hz balanced, else bright), noise from the worst take (a noise
// floor under -75 dBFS and no hum above -70 is quiet; a floor above -60 or
// hum above -50 is noisy; takes without a pause have no floor to count).
// Levels depend on the interface gain the clips were recorded with, so they
// compare takes of one setup, not two studios.
juce::var GuitarLibrary::summarise(const juce::var& takes) {
  double peak = AudioMeasure::kFloorDb, centroidSum = 0.0, rmsPower = 0.0, lowEnd = 0.0;
  double worstFloor = AudioMeasure::kFloorDb, worstHum = AudioMeasure::kFloorDb;
  int mains = 0, count = 0;
  if (const auto* list = takes.getArray())
    for (const auto& take : *list) {
      const juce::var m = take["measurements"];
      if (m.getDynamicObject() == nullptr)
        continue;
      ++count;
      peak = std::max(peak, static_cast<double>(m["peakDb"]));
      centroidSum += static_cast<double>(m["centroidHz"]);
      lowEnd += static_cast<double>(m["lowEndShare"]);
      rmsPower += std::pow(10.0, static_cast<double>(m["rmsDb"]) / 10.0);
      if (!m["noiseFloorDb"].isVoid())
        worstFloor = std::max(worstFloor, static_cast<double>(m["noiseFloorDb"]));
      if (static_cast<double>(m["hum"]["levelDb"]) > worstHum) {
        worstHum = static_cast<double>(m["hum"]["levelDb"]);
        mains = static_cast<int>(m["hum"]["mainsHz"]);
      }
    }
  if (count == 0)
    return object({{"takeCount", 0}});

  const double centroid = centroidSum / count;
  const char* outputLevel = peak < -18.0 ? "low" : peak < -6.0 ? "medium" : "high";
  const char* brightness = centroid < 700.0 ? "dark" : centroid < 1400.0 ? "balanced" : "bright";
  const char* noise = (worstFloor > -60.0 || worstHum > -50.0)   ? "noisy"
                      : (worstFloor > -75.0 || worstHum > -70.0) ? "moderate"
                                                                 : "quiet";
  return object({
      {"takeCount", count},
      {"outputLevel", outputLevel},
      {"brightness", brightness},
      {"noise", noise},
      {"peakDb", AudioMeasure::tidyDb(peak)},
      {"rmsDb", AudioMeasure::tidyDb(AudioMeasure::powerToDb(rmsPower / count))},
      {"centroidHz", std::round(centroid * 10.0) / 10.0},
      {"lowEndShare", std::round(lowEnd / count * 10000.0) / 10000.0},
      {"noiseFloorDb", worstFloor > AudioMeasure::kFloorDb
                           ? juce::var(AudioMeasure::tidyDb(worstFloor))
                           : juce::var()},
      {"humDb", AudioMeasure::tidyDb(worstHum)},
      {"mainsHz", mains},
  });
}

bool GuitarLibrary::storeTake(const juce::String& id, const juce::String& position,
                              const juce::String& take, const juce::File& clip,
                              juce::String& error) {
  const juce::var measurements = measureClip(clip, error);
  if (measurements.isVoid())
    return false;

  const std::lock_guard<std::mutex> lock(fileMutex);
  juce::var profile = readProfile(id);
  if (profile.isVoid()) {
    error = "the guitar profile is gone";
    return false;
  }
  profile = clone(profile);

  juce::Array<juce::var> takes;
  if (const auto* existing = profile["takes"].getArray())
    for (const auto& entry : *existing)
      if (!(entry["position"].toString() == position && entry["take"].toString() == take))
        takes.add(entry);
  takes.add(object({
      {"position", position},
      {"take", take},
      {"file", clip.getFileName()},
      {"sampleRate", measurements["sampleRate"]},
      {"seconds", measurements["seconds"]},
      {"channels", measurements["channels"]},
      {"measurements", measurements},
  }));
  profile.getDynamicObject()->setProperty("takes", takes);
  profile.getDynamicObject()->setProperty("summary", summarise(takes));
  if (!writeProfile(id, profile)) {
    error = "could not write the profile";
    return false;
  }
  return true;
}

CommandResult GuitarLibrary::importClip(const juce::String& id, const juce::String& position,
                                        const juce::String& take, const juce::File& source) {
  juce::String matched;
  juce::File target;
  {
    const std::lock_guard<std::mutex> lock(fileMutex);
    const juce::var profile = readProfile(id);
    if (profile.isVoid())
      return noGuitar(id);
    matched = matchPosition(profile, position);
    if (matched.isEmpty())
      return fail(CommandError::invalidArgs,
                  "guitar.importClip: the guitar has no position '" + position + "'");
    target = profileDir(id).getChildFile(LibraryPaths::slug(matched) + "-" + take + ".wav");
  }

  // Read before anything is copied: a file that is not a usable WAV must not
  // replace a take that is.
  juce::AudioBuffer<float> audio;
  double rate = 0.0;
  juce::String error;
  if (!AudioFiles::read(source, audio, rate, error))
    return fail(source.existsAsFile() ? CommandError::failed : CommandError::notFound, error);
  if (source != target && !source.copyFileTo(target))
    return fail(CommandError::failed, "could not copy the clip to " + target.getFullPathName());

  if (!storeTake(id, matched, take, target, error))
    return fail(CommandError::failed, error);
  return CommandResult::success(get(id));
}

CommandResult GuitarLibrary::analyze(const juce::String& id) {
  juce::var profile;
  {
    const std::lock_guard<std::mutex> lock(fileMutex);
    profile = readProfile(id);
  }
  if (profile.isVoid())
    return noGuitar(id);

  juce::StringArray missing;
  if (const auto* takes = profile["takes"].getArray())
    for (const auto& take : *takes) {
      const juce::File clip = profileDir(id).getChildFile(take["file"].toString());
      juce::String error;
      // A profile is user data in a folder they can edit: only files that
      // sit directly in it are take files.
      if (clip.getParentDirectory() != profileDir(id) ||
          !storeTake(id, take["position"].toString(), take["take"].toString(), clip, error))
        missing.add(take["file"].toString());
    }
  juce::var result = get(id);
  if (auto* obj = result.getDynamicObject())
    obj->setProperty("unreadable", stringsVar(missing));
  return CommandResult::success(result);
}

std::vector<GuitarLibrary::Clip> GuitarLibrary::clips(const juce::String& requestedId,
                                                      const juce::String& position,
                                                      const juce::StringArray& takes,
                                                      juce::String& error) const {
  const juce::String id = requestedId.isNotEmpty() ? requestedId : defaultId();
  if (id.isEmpty()) {
    error = "no guitar given and no default guitar set (see guitar.list, guitar.setDefault)";
    return {};
  }
  const std::lock_guard<std::mutex> lock(fileMutex);
  const juce::var profile = readProfile(id);
  if (profile.isVoid()) {
    error = "no guitar profile with id '" + id + "'";
    return {};
  }
  juce::String matched;
  if (position.isNotEmpty()) {
    matched = matchPosition(profile, position);
    if (matched.isEmpty()) {
      error = "the guitar has no position '" + position + "'";
      return {};
    }
  }

  std::vector<Clip> found;
  if (const auto* list = profile["takes"].getArray())
    for (const auto& take : *list) {
      if (matched.isNotEmpty() && take["position"].toString() != matched)
        continue;
      if (!takes.isEmpty() && !takes.contains(take["take"].toString()))
        continue;
      const juce::File file = profileDir(id).getChildFile(take["file"].toString());
      if (file.getParentDirectory() == profileDir(id) && file.existsAsFile())
        found.push_back({id, take["position"].toString(), take["take"].toString(), file});
    }
  if (found.empty())
    error = "guitar '" + id + "' has no matching takes (see guitar.get)";
  return found;
}

// ####################
// RECORDING
// ####################
CommandResult GuitarLibrary::startRecording(const juce::String& id, const juce::String& position,
                                            const juce::String& take, double seconds,
                                            int channel) {
  juce::String matched;
  {
    const std::lock_guard<std::mutex> lock(fileMutex);
    const juce::var profile = readProfile(id);
    if (profile.isVoid())
      return noGuitar(id);
    matched = matchPosition(profile, position);
  }
  if (matched.isEmpty())
    return fail(CommandError::invalidArgs,
                "guitar.record.start: the guitar has no position '" + position + "'");

  const double rate = live.getSampleRate();
  if (rate <= 0.0)
    return fail(CommandError::unavailable,
                "the audio device is not running, so there is no input to record");

  {
    const std::lock_guard<std::mutex> lock(recordingMutex);
    if (recording.state == "writing")
      return fail(CommandError::unavailable, "the previous take is still being saved");
  }
  joinWriter();

  live.inputRecorder.start(static_cast<int>(std::lround(seconds * rate)), channel);
  {
    const std::lock_guard<std::mutex> lock(recordingMutex);
    recording = Recording();
    recording.state = "recording";
    recording.guitar = id;
    recording.position = matched;
    recording.take = take;
    recording.sampleRate = rate;
    recording.seconds = seconds;
  }
  return CommandResult::success(recordingStatus());
}

void GuitarLibrary::cancelRecording() {
  const std::lock_guard<std::mutex> lock(recordingMutex);
  if (recording.state != "recording")
    return;
  live.inputRecorder.cancel();
  recording = Recording();
}

juce::var GuitarLibrary::recordingStatus() {
  // The buffer has filled: the samples leave the recorder here and the rest
  // (a file, the analysis, the profile) happens on the writer thread.
  bool handOff = false;
  Recording snapshot;
  {
    const std::lock_guard<std::mutex> lock(recordingMutex);
    if (recording.state == "recording" &&
        live.inputRecorder.getState() == InputRecorder::State::full) {
      recording.state = "writing";
      handOff = true;
    }
    snapshot = recording;
  }

  if (handOff) {
    joinWriter();
    std::vector<float> samples = live.inputRecorder.take();
    writer = std::thread([this, snapshot, samples = std::move(samples)] {
      juce::String error;
      juce::var result;
      const juce::File clip = profileDir(snapshot.guitar)
                                  .getChildFile(LibraryPaths::slug(snapshot.position) + "-" +
                                                snapshot.take + ".wav");
      juce::AudioBuffer<float> audio(1, static_cast<int>(samples.size()));
      std::copy(samples.begin(), samples.end(), audio.getWritePointer(0));
      if (samples.empty() || !AudioFiles::write(clip, audio, snapshot.sampleRate))
        error = "could not write " + clip.getFullPathName();
      else if (storeTake(snapshot.guitar, snapshot.position, snapshot.take, clip, error)) {
        const std::lock_guard<std::mutex> lock(fileMutex);
        // Held in a variable: the array below lives inside it.
        const juce::var profile = readProfile(snapshot.guitar);
        if (const auto* takes = profile["takes"].getArray())
          for (const auto& entry : *takes)
            if (entry["position"].toString() == snapshot.position &&
                entry["take"].toString() == snapshot.take)
              result = entry;
      }

      const std::lock_guard<std::mutex> lock(recordingMutex);
      recording.state = error.isEmpty() ? "done" : "failed";
      recording.error = error;
      recording.result = result;
    });
  }

  const bool active = snapshot.state == "recording";
  const double recorded = active && snapshot.sampleRate > 0.0
                              ? live.inputRecorder.recordedSamples() / snapshot.sampleRate
                              : (snapshot.state == "idle" ? 0.0 : snapshot.seconds);
  juce::var status = object({
      {"state", snapshot.state},
      {"guitar", snapshot.guitar},
      {"position", snapshot.position},
      {"take", snapshot.take},
      {"seconds", snapshot.seconds},
      {"recordedSeconds", std::round(recorded * 1000.0) / 1000.0},
      {"progress",
       snapshot.seconds > 0.0 ? juce::jlimit(0.0, 1.0, recorded / snapshot.seconds) : 0.0},
      {"peakDb", active ? juce::var(AudioMeasure::tidyDb(AudioMeasure::amplitudeToDb(
                              live.inputRecorder.peakLevel())))
                        : juce::var()},
  });
  if (snapshot.error.isNotEmpty())
    status.getDynamicObject()->setProperty("error", snapshot.error);
  if (!snapshot.result.isVoid())
    status.getDynamicObject()->setProperty("result", snapshot.result);
  return status;
}

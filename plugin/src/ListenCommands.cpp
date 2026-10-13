// The "listen" commands: render.clip, render.compare and render.probe. An
// agent cannot hear, so these play audio through a rig offline and answer
// with measurements (AudioMeasure.h). They never run through the rig being
// played: the rig is a sandbox the connection selected, or a private copy of
// the live rig made for the one call.
#include "AgentHub.h"

// Processor.h first: it pulls in the JUCE module headers, which must come
// before anything that drags in <windows.h>.
#include "Processor.h"

#include "AudioMeasure.h"
#include "GuitarProfiles.h"
#include "LibraryPaths.h"
#include "SandboxRig.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace {

using P = CommandParam;
using T = CommandParamType;
using Properties = std::initializer_list<std::pair<const char*, juce::var>>;

constexpr int kMaxRendersPerCall = 32;
// The most audio one call may have rendered, summed over all its renders
// (each clip with its tail, once per scene). Thirty-two renders of two
// minutes each would be more than an hour held in buffers; ten minutes is
// more than any comparison of takes needs.
constexpr double kMaxRenderedSecondsPerCall = 600.0;
constexpr double kPi = 3.14159265358979323846;

juce::var object(Properties properties) {
  juce::DynamicObject::Ptr obj = new juce::DynamicObject();
  for (const auto& [name, value] : properties)
    obj->setProperty(name, value);
  return juce::var(obj.get());
}

CommandResult fail(const char* code, const juce::String& message) {
  return CommandResult::failure(code, message);
}

// Rounded to `places` decimals (dividing last gives the double nearest the
// decimal, which prints short).
double rounded(double value, int places) {
  const double perUnit = std::pow(10.0, places);
  return std::round(value * perUnit) / perUnit;
}

// The rig a render runs through, held for the length of the render: nobody
// else may render through it or edit it meanwhile, and it cannot be
// discarded. A private copy of the live rig is removed when the render ends.
// It also holds one of the application's few render places
// (AgentHub::tryBeginRender), taken by reserveRig.
struct Reservation {
  Reservation(AgentHub& h, std::shared_ptr<SandboxRig> r) : hub(h), rig(std::move(r)) {
    rig->activeUses.fetch_add(1);
  }
  ~Reservation() {
    rig->rendering.store(false);
    rig->activeUses.fetch_sub(1);
    hub.endRender();
    // A private copy has served its purpose, and a sandbox whose owner
    // disconnected while this render ran was only waiting for it to end.
    if (!rig->temporary && !rig->orphaned.load())
      return;
    // Its processor has to go on the message thread; when the application
    // closes first, the hub takes it down.
    const juce::String id = rig->id;
    rig.reset();
    hub.discardSandboxSoon(id);
  }
  AgentHub& hub;
  std::shared_ptr<SandboxRig> rig;
};

// Picks and reserves the rig: the sandbox named by `sandbox`, else the one
// the calling connection selected, else a private copy of the live rig.
// Message thread.
std::optional<CommandResult> reserveRig(AgentHub& hub, const juce::var& args,
                                        std::shared_ptr<Reservation>& reservation) {
  AgentSession& session = hub.currentSession();
  juce::String id = args["sandbox"].toString();
  if (id.isEmpty())
    id = hub.revisionFor(session).rig;

  // A place among the renders first: the private copy below is a whole rig,
  // not to be made for a call that is then turned away.
  if (!hub.tryBeginRender())
    return fail(CommandError::unavailable,
                juce::String(AgentHub::kMaxConcurrentRenders) +
                    " renders are already running, which is as many as run at once; try "
                    "again when one is done");
  struct Place {
    AgentHub& hub;
    bool kept = false;
    ~Place() {
      if (!kept)
        hub.endRender();
    }
  } place{hub};

  std::shared_ptr<SandboxRig> rig;
  if (id.isEmpty() || id == "live") {
    juce::String error;
    rig = hub.createSandbox("live", {}, session.id, error, /*temporary=*/true);
    if (rig == nullptr)
      return fail(CommandError::failed, error);
    rig->name = "render copy of the live rig";
  } else {
    // The caller's own: another connection's sandbox, or another render's
    // private copy, is not there as far as this one can tell.
    rig = hub.sandboxFor(session, id);
    if (rig == nullptr)
      return fail(CommandError::notFound, "no sandbox rig with id '" + id + "'");
  }
  if (rig->rendering.exchange(true))
    return fail(CommandError::unavailable,
                "sandbox rig '" + rig->id + "' is already rendering; try again when it is done");
  place.kept = true;  // the reservation gives it back
  reservation = std::make_shared<Reservation>(hub, std::move(rig));
  return std::nullopt;
}

struct ClipSource {
  juce::String guitar, position, take;
  juce::File file;
  // From the file's header, checked when the call was planned.
  double seconds = 0.0;
};

// The clips a call names: files given directly, else the takes of a guitar.
std::optional<CommandResult> resolveClips(AgentHub& hub, const juce::var& args,
                                          const char* command, std::vector<ClipSource>& clips) {
  if (const auto* paths = args["clips"].getArray(); paths != nullptr && !paths->isEmpty()) {
    for (const auto& path : *paths) {
      const juce::String text = path.toString();
      if (!juce::File::isAbsolutePath(text))
        return fail(CommandError::invalidArgs,
                    juce::String(command) + ": 'clips' must hold absolute paths (got '" + text + "')");
      const juce::File file(text);
      if (!file.existsAsFile())
        return fail(CommandError::notFound, "file not found: " + text);
      clips.push_back({{}, {}, {}, file, 0.0});
    }
    return std::nullopt;
  }

  juce::StringArray takes;
  if (const auto* list = args["takes"].getArray())
    for (const auto& take : *list)
      takes.add(take.toString());
  juce::String error;
  for (const auto& clip : hub.guitars().clips(args["guitar"].toString(),
                                              args["position"].toString(), takes, error))
    clips.push_back({clip.guitar, clip.position, clip.take, clip.file, 0.0});
  if (clips.empty())
    return fail(CommandError::notFound, juce::String(command) + ": " + error);
  return std::nullopt;
}

// Scene letters or numbers ("A" to "H", 0 to 7) as indices.
std::optional<CommandResult> resolveScenes(const juce::var& args, const char* command,
                                           std::vector<int>& scenes) {
  if (const auto* list = args["scenes"].getArray())
    for (const auto& item : *list) {
      const juce::String text = item.toString().trim().toUpperCase();
      int index = -1;
      if (text.length() == 1 && text[0] >= 'A' && text[0] <= 'H')
        index = static_cast<int>(text[0] - 'A');
      else if (text.length() == 1 && text[0] >= '0' && text[0] <= '7')
        index = static_cast<int>(text[0] - '0');
      if (index < 0)
        return fail(CommandError::invalidArgs,
                    juce::String(command) + ": 'scenes' takes A to H (got '" + item.toString() + "')");
      scenes.push_back(index);
    }
  return std::nullopt;
}

juce::String sceneLetter(int index) { return juce::String::charToString(static_cast<juce::juce_wchar>('A' + index)); }

// The first `frames` samples of a buffer, without copying.
juce::AudioBuffer<float> head(juce::AudioBuffer<float>& audio, int frames) {
  return juce::AudioBuffer<float>(audio.getArrayOfWritePointers(), audio.getNumChannels(),
                                  juce::jlimit(0, audio.getNumSamples(), frames));
}

struct Measured {
  AudioMeasure::Levels levels;
  AudioMeasure::Spectrum spectrum;
  AudioMeasure::Stereo stereo;
};

Measured measure(const juce::AudioBuffer<float>& audio, double rate) {
  return {AudioMeasure::measureLevels(audio, rate), AudioMeasure::measureSpectrum(audio, rate),
          AudioMeasure::measureStereo(audio)};
}

// Distortion of a stereo output against the input's first channel: each side
// is fitted by itself (a stereo effect may treat them differently) and the
// two are pooled by energy.
AudioMeasure::Distortion stereoDistortion(const juce::AudioBuffer<float>& input,
                                          const juce::AudioBuffer<float>& output, int frames,
                                          double rate) {
  double residual = 0.0, total = 0.0;
  bool valid = false;
  for (int ch = 0; ch < output.getNumChannels(); ++ch) {
    const AudioMeasure::Distortion side = AudioMeasure::measureDistortion(
        input.getReadPointer(0), output.getReadPointer(ch), frames, rate);
    if (!side.valid)
      continue;
    double energy = 0.0;
    const float* y = output.getReadPointer(ch);
    for (int i = 0; i < frames; ++i)
      energy += static_cast<double>(y[i]) * y[i];
    residual += energy * (side.percent / 100.0) * (side.percent / 100.0);
    total += energy;
    valid = true;
  }
  AudioMeasure::Distortion pooled;
  if (valid && total > 0.0) {
    pooled.valid = true;
    pooled.percent = 100.0 * std::sqrt(residual / total);
    pooled.residualDb = AudioMeasure::powerToDb(residual / total);
  }
  return pooled;
}

// One clip through the rig, measured. `error` is set when it did not render.
struct ClipRender {
  juce::var report;
  // The output while the input played. Only kept when the caller asked
  // (render.compare joins them); empty otherwise, so a call that only wants
  // the numbers does not hold every render's audio until it answers.
  juce::AudioBuffer<float> programme;
  double rate = 0.0;
  double distortionPercent = 0.0;
  bool distortionValid = false;
  double inputLufs = 0.0, outputLufs = 0.0;
  juce::String error;
};

ClipRender renderClip(SandboxRig& rig, const ClipSource& clip, double tailSeconds,
                      const juce::File& writeTo, const std::atomic<bool>& cancel,
                      bool keepProgramme) {
  ClipRender out;
  juce::AudioBuffer<float> input;
  if (!AudioFiles::read(clip.file, input, out.rate, out.error))
    return out;
  if (input.getNumChannels() > 2) {
    out.error = "a clip must be mono or stereo: " + clip.file.getFullPathName();
    return out;
  }

  OfflineRender::Result rendered =
      OfflineRender::render(rig.processor(), input, out.rate, tailSeconds, cancel);
  if (rendered.error.isNotEmpty()) {
    out.error = rendered.error;
    return out;
  }

  const int frames = input.getNumSamples();
  // Levels, spectrum and stereo over the stretch the input played: the tail
  // is measured as a tail, and its silence must not thin the RMS.
  juce::AudioBuffer<float> programme = head(rendered.audio, frames);
  const Measured measured = measure(programme, out.rate);
  const AudioMeasure::Levels inputLevels = AudioMeasure::measureLevels(input, out.rate);
  // The fit sees the whole render, the input padded with the silence that
  // was played after it: an echo of the clip's last note is then explained
  // by the note, as it should be.
  juce::AudioBuffer<float> paddedInput(1, rendered.audio.getNumSamples());
  paddedInput.clear();
  paddedInput.copyFrom(0, 0, input, 0, 0, frames);
  const AudioMeasure::Distortion distortion =
      stereoDistortion(paddedInput, rendered.audio, rendered.audio.getNumSamples(), out.rate);
  bool truncated = false;
  const double tail = AudioMeasure::tailSeconds(rendered.audio, frames, out.rate, &truncated);

  juce::var clipInfo = object({{"file", clip.file.getFullPathName()}});
  if (clip.guitar.isNotEmpty()) {
    clipInfo.getDynamicObject()->setProperty("guitar", clip.guitar);
    clipInfo.getDynamicObject()->setProperty("position", clip.position);
    clipInfo.getDynamicObject()->setProperty("take", clip.take);
  }

  out.report = object({
      {"clip", clipInfo},
      {"sampleRate", out.rate},
      {"seconds", rounded(frames / out.rate, 3)},
      {"input", object({
                    {"peakDb", AudioMeasure::tidyDb(inputLevels.peakDb)},
                    {"rmsDb", AudioMeasure::tidyDb(inputLevels.rmsDb)},
                    {"lufs", AudioMeasure::tidyDb(inputLevels.lufs)},
                })},
      {"levels", AudioMeasure::toVar(measured.levels)},
      // By RMS, each side averaged over its channels, so a rig that passes a
      // mono clip to both outputs unchanged reads 0 (LUFS sums channels and
      // would read 3).
      {"gainDb", AudioMeasure::tidyDb(measured.levels.rmsDb - inputLevels.rmsDb)},
      {"spectrum", AudioMeasure::toVar(measured.spectrum)},
      {"stereo", AudioMeasure::toVar(measured.stereo)},
      {"distortion", AudioMeasure::toVar(distortion)},
      {"tail", object({{"seconds", rounded(tail, 3)}, {"truncated", truncated}})},
      {"latencySamples", rendered.latencySamples},
      {"latencyMs", rounded(rendered.latencySamples * 1000.0 / out.rate, 3)},
  });

  if (writeTo != juce::File()) {
    if (AudioFiles::write(writeTo, rendered.audio, out.rate))
      out.report.getDynamicObject()->setProperty("audioPath", writeTo.getFullPathName());
    else
      out.report.getDynamicObject()->setProperty("audioError", "could not write the WAV");
  }

  if (keepProgramme)
    out.programme.makeCopyOf(programme);
  out.distortionPercent = distortion.percent;
  out.distortionValid = distortion.valid;
  out.inputLufs = inputLevels.lufs;
  out.outputLufs = measured.levels.lufs;
  return out;
}

// Soft against hard picking, where a position (or a scene) has both: what
// "cleans up when I play softly" and "how much does it compress" look like
// in numbers.
juce::var dynamicsOf(const std::vector<std::pair<ClipSource, ClipRender>>& renders) {
  juce::Array<juce::var> pairs;
  for (const auto& [softClip, soft] : renders) {
    if (softClip.take != "soft")
      continue;
    for (const auto& [hardClip, hard] : renders) {
      if (hardClip.take != "hard" || hardClip.position != softClip.position ||
          hardClip.guitar != softClip.guitar)
        continue;
      const double inputDifference = hard.inputLufs - soft.inputLufs;
      const double outputDifference = hard.outputLufs - soft.outputLufs;
      juce::var entry = object({
          {"guitar", softClip.guitar},
          {"position", softClip.position},
          {"softDistortionPercent", rounded(soft.distortionPercent, 3)},
          {"hardDistortionPercent", rounded(hard.distortionPercent, 3)},
          {"inputLevelDifferenceDb", AudioMeasure::tidyDb(inputDifference)},
          {"outputLevelDifferenceDb", AudioMeasure::tidyDb(outputDifference)},
          // How many dB of the playing dynamics the rig takes away.
          {"compressionDb", AudioMeasure::tidyDb(inputDifference - outputDifference)},
      });
      if (soft.distortionValid && hard.distortionValid && hard.distortionPercent > 0.0)
        entry.getDynamicObject()->setProperty(
            "cleanupRatio", rounded(soft.distortionPercent / hard.distortionPercent, 3));
      pairs.add(entry);
    }
  }
  return pairs;
}

// Everything a clip render needs, decided on the message thread.
struct RenderPlan {
  std::shared_ptr<Reservation> reservation;
  std::vector<ClipSource> clips;
  std::vector<int> scenes;  // empty: the rig's current scene
  double tailSeconds = 2.0;
  bool writeAudio = false;
  // Where writeAudio writes: the scratch folder's renders.
  juce::File audioDir;
  // Each render's audio is wanted after the run (render.compare).
  bool keepProgramme = false;
  // The calling session's flag: the run stops when its connection or the
  // application closes.
  const std::atomic<bool>* cancel = nullptr;
};

std::optional<CommandResult> planRender(AgentHub& hub, const juce::var& args, const char* command,
                                        RenderPlan& plan) {
  if (auto error = resolveClips(hub, args, command, plan.clips))
    return error;
  if (auto error = resolveScenes(args, command, plan.scenes))
    return error;
  const size_t renders = plan.clips.size() * std::max<size_t>(1, plan.scenes.size());
  if (renders > static_cast<size_t>(kMaxRendersPerCall))
    return fail(CommandError::invalidArgs,
                juce::String(command) + ": that is " + juce::String(static_cast<int>(renders)) +
                    " renders; at most " + juce::String(kMaxRendersPerCall) +
                    " per call (name fewer takes or scenes)");
  plan.tailSeconds = args["tailSeconds"].isVoid() ? 2.0 : static_cast<double>(args["tailSeconds"]);
  plan.writeAudio = static_cast<bool>(args["writeAudio"]);
  plan.cancel = &hub.slowWorkCancel();

  // How much audio that is, from the files' headers (no samples are read
  // here): each clip with its tail, once per scene. A file that would be
  // refused when its turn came is refused now, before anything is rendered.
  double seconds = 0.0;
  for (auto& clip : plan.clips) {
    AudioFiles::Info info;
    juce::String error;
    if (!AudioFiles::inspect(clip.file, info, error))
      return fail(CommandError::failed, juce::String(command) + ": " + error);
    clip.seconds = info.seconds();
    seconds += clip.seconds + plan.tailSeconds;
  }
  seconds *= static_cast<double>(std::max<size_t>(1, plan.scenes.size()));
  if (seconds > kMaxRenderedSecondsPerCall)
    return fail(CommandError::invalidArgs,
                juce::String(command) + ": that is " + juce::String(seconds, 1) +
                    " seconds of audio to render (clips and tails, once per scene); at most " +
                    juce::String(kMaxRenderedSecondsPerCall, 0) +
                    " per call (name fewer takes or scenes, or a shorter tail)");

  if (plan.writeAudio) {
    juce::String problem;
    const juce::File scratch = LibraryPaths::scratchDir(&problem);
    if (scratch == juce::File())
      return fail(CommandError::failed, juce::String(command) + ": " + problem);
    plan.audioDir = scratch.getChildFile("renders");
  }
  return reserveRig(hub, args, plan.reservation);
}

// Runs a plan: every clip through every scene. Any thread.
struct RenderRun {
  std::vector<std::pair<ClipSource, ClipRender>> renders;
  juce::Array<juce::var> reports;
  juce::String error;
};

RenderRun runPlan(const RenderPlan& plan) {
  RenderRun run;
  SandboxRig& rig = *plan.reservation->rig;

  // Given up on when the caller's connection or the application closes: the
  // message thread may be the one closing it, and would never answer.
  const auto onMessageThread = [&](std::function<void()> call) {
    return CommandRegistry::callOnMessageThread(std::move(call), 10000, plan.cancel);
  };

  int originalScene = -1;
  if (!plan.scenes.empty() &&
      !onMessageThread([&] { originalScene = rig.processor().getActiveScene(); })) {
    run.error = "the application is busy or closing";
    return run;
  }

  // A scene the preset does not use cannot be heard: refuse before any render.
  if (!plan.scenes.empty()) {
    int sceneCount = 1;
    onMessageThread([&] { sceneCount = rig.processor().getSceneCount(); });
    for (const int scene : plan.scenes)
      if (scene >= sceneCount) {
        run.error = "scene " + sceneLetter(scene) + " is not in use (the preset has " +
                    juce::String(sceneCount) + ")";
        return run;
      }
  }

  const std::vector<int> scenes = plan.scenes.empty() ? std::vector<int>{-1} : plan.scenes;
  for (const int scene : scenes) {
    if (scene >= 0 && !onMessageThread([&] { rig.processor().selectScene(scene); })) {
      run.error = "the application is busy or closing";
      break;
    }
    for (const auto& clip : plan.clips) {
      juce::File writeTo;
      if (plan.writeAudio) {
        juce::String name = rig.id;
        if (scene >= 0)
          name << "-scene" << sceneLetter(scene);
        name << "-" << (clip.guitar.isNotEmpty()
                            ? clip.guitar + "-" + LibraryPaths::slug(clip.position) + "-" + clip.take
                            : LibraryPaths::slug(clip.file.getFileNameWithoutExtension()));
        writeTo = plan.audioDir.getChildFile(name + ".wav");
      }
      ClipRender rendered =
          renderClip(rig, clip, plan.tailSeconds, writeTo, *plan.cancel, plan.keepProgramme);
      if (rendered.error.isNotEmpty()) {
        run.error = rendered.error;
        break;
      }
      if (scene >= 0)
        rendered.report.getDynamicObject()->setProperty("scene", sceneLetter(scene));
      run.reports.add(rendered.report);
      run.renders.emplace_back(clip, std::move(rendered));
    }
    if (run.error.isNotEmpty())
      break;
  }

  // The rig goes back to the scene it was on, whatever happened.
  if (originalScene >= 0)
    onMessageThread([&] { rig.processor().selectScene(originalScene); });
  return run;
}

// Fixed-seed noise, the same on every platform (the standard distributions
// are not), uniform in [-amplitude, amplitude].
std::vector<float> probeNoise(int frames, float amplitude) {
  std::vector<float> x(static_cast<size_t>(frames));
  std::uint32_t state = 0x13579bdfu;
  for (auto& sample : x) {
    state = state * 1664525u + 1013904223u;
    sample = amplitude * (static_cast<float>(state >> 8) / 8388608.0f - 1.0f);
  }
  return x;
}

const std::vector<double>& thirdOctaves() {
  static const std::vector<double> centres{25,   31.5, 40,   50,   63,   80,   100,  125,
                                           160,  200,  250,  315,  400,  500,  630,  800,
                                           1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000,
                                           6300, 8000, 10000, 12500, 16000};
  return centres;
}

// How far two measured sounds are apart, per group and overall. Each group
// is scaled so that 1 is a difference a listener would not miss: 3 dB of
// loudness, 3 dB of crest factor, 3 dB RMS across the bands (or a third of
// an octave of centroid), 0.3 of correlation (or 0.2 of width). The overall
// score weighs tone at a half, loudness and dynamics at a fifth each and
// stereo at a tenth.
juce::var compareMeasured(const Measured& rig, const Measured& reference) {
  juce::DynamicObject::Ptr bandDifference = new juce::DynamicObject();
  double bandSquares = 0.0;
  int bandsCounted = 0;
  for (size_t band = 0; band < AudioMeasure::kBands.size(); ++band) {
    // A band neither has anything in says nothing about the match.
    if (rig.spectrum.share[band] < 1.0e-6 && reference.spectrum.share[band] < 1.0e-6) {
      bandDifference->setProperty(AudioMeasure::kBands[band].id, 0.0);
      continue;
    }
    const double difference = juce::jlimit(
        -40.0, 40.0,
        std::max(-60.0, rig.spectrum.db[band]) - std::max(-60.0, reference.spectrum.db[band]));
    bandDifference->setProperty(AudioMeasure::kBands[band].id, AudioMeasure::tidyDb(difference));
    bandSquares += difference * difference;
    ++bandsCounted;
  }
  const double bandRms = bandsCounted > 0 ? std::sqrt(bandSquares / bandsCounted) : 0.0;
  const double centroidOctaves =
      rig.spectrum.centroidHz > 0.0 && reference.spectrum.centroidHz > 0.0
          ? std::log2(rig.spectrum.centroidHz / reference.spectrum.centroidHz)
          : 0.0;

  const double loudnessDifference = rig.levels.lufs - reference.levels.lufs;
  const double crestDifference = rig.levels.crestDb - reference.levels.crestDb;
  const double correlationDifference = rig.stereo.correlation - reference.stereo.correlation;
  const double widthDifference = rig.stereo.width - reference.stereo.width;

  const double loudness = std::abs(loudnessDifference) / 3.0;
  const double dynamics = std::abs(crestDifference) / 3.0;
  const double tone = std::max(bandRms / 3.0, std::abs(centroidOctaves) * 3.0);
  const double stereo =
      std::max(std::abs(correlationDifference) / 0.3, std::abs(widthDifference) / 0.2);
  const double overall = 0.5 * tone + 0.2 * loudness + 0.2 * dynamics + 0.1 * stereo;

  return object({
      {"difference", object({
                         {"lufs", AudioMeasure::tidyDb(loudnessDifference)},
                         {"crestDb", AudioMeasure::tidyDb(crestDifference)},
                         {"bandsDb", juce::var(bandDifference.get())},
                         {"bandsRmsDb", AudioMeasure::tidyDb(bandRms)},
                         {"centroidOctaves", rounded(centroidOctaves, 3)},
                         {"correlation", rounded(correlationDifference, 4)},
                         {"width", rounded(widthDifference, 4)},
                     })},
      {"distance", object({
                       {"loudness", rounded(loudness, 3)},
                       {"dynamics", rounded(dynamics, 3)},
                       {"tone", rounded(tone, 3)},
                       {"stereo", rounded(stereo, 3)},
                       {"overall", rounded(overall, 3)},
                   })},
  });
}

juce::var measuredVar(const Measured& measured) {
  return object({
      {"levels", AudioMeasure::toVar(measured.levels)},
      {"spectrum", AudioMeasure::toVar(measured.spectrum)},
      {"stereo", AudioMeasure::toVar(measured.stereo)},
  });
}

// The arguments render.clip and render.compare share.
std::vector<CommandParam> clipParams() {
  return {
      P::optional("guitar", T::string,
                  "The guitar profile whose clips to play (see guitar.list). Default: the "
                  "default guitar."),
      P::optional("position", T::string, "Only that pickup position's takes."),
      P::optional("takes", T::array, "Only these takes: soft, hard, chords, notes. Default: all "
                                     "the guitar has.")
          .items(T::string),
      P::optional("clips", T::array,
                  "Instead of a guitar: WAV files to play, as absolute paths inside the "
                  "allowed folders (see roots.list); mono or stereo, at most 120 seconds each. "
                  "One call renders at most 600 seconds in all (clips and tails, once per "
                  "scene).")
          .items(T::string)
          .path(),
      P::optional("scenes", T::array,
                  "Render once per scene named here (A to H); the rig returns to its current "
                  "scene afterwards. Default: the current scene only.")
          .items(T::string),
      P::optional("tailSeconds", T::number,
                  "How long to keep rendering after a clip ends, to catch delay and reverb "
                  "tails.")
          .range(0.0, 30.0)
          .withDefault(2.0),
      P::optional("sandbox", T::string,
                  "The sandbox rig to render through. Default: the calling connection's "
                  "selected sandbox, or a private copy of the live rig."),
  };
}

}  // namespace

void AgentHub::registerListenCommands() {
  // ---- render.clip ----
  std::vector<CommandParam> clipArguments = clipParams();
  clipArguments.push_back(
      P::optional("writeAudio", T::boolean,
                  "Also write each render as a WAV into the scratch folder and return its "
                  "path as audioPath.")
          .withDefault(false));

  liveCommands.add({
      "render.clip",
      "Plays recorded guitar clips through a rig offline and returns measurements of what "
      "came out, one entry in renders per clip (and per scene): input (the clip's own peak, "
      "RMS and LUFS), levels (peakDb, truePeakDb, rmsDb, crestDb, lufs), gainDb (output RMS "
      "minus input RMS), spectrum (the share of energy in sub, bass, lowMid, mid, highMid, "
      "presence and air, in dB and as fractions, the spectral centroid and the low-end "
      "share), stereo (correlation, width, balanceDb), distortion (percent: how much of the "
      "output a linear filter of the input cannot explain; time effects longer than the fit "
      "and modulation count too, so bypass them to measure the amp alone), tail (seconds the "
      "output keeps sounding after the clip ends) and latency. dynamics pairs each position's "
      "soft and hard takes: their distortion, cleanupRatio (soft over hard) and compressionDb "
      "(how much of the playing dynamics the rig removes). The rig is the connection's "
      "selected sandbox, the one named, or a private copy of the live rig; the rig being "
      "played is never rendered through. The same call on the same rig returns the same "
      "numbers. A sandbox cannot be edited while it renders, and at most two renders run at "
      "a time in the whole application: a third answers unavailable.",
      clipArguments,
      false,
      CommandPermission::render,
      [this](const juce::var& args) {
        auto plan = std::make_shared<RenderPlan>();
        if (auto error = planRender(*this, args, "render.clip", *plan))
          return CommandWork::done(*error);
        return CommandWork::later([plan] {
          RenderRun run = runPlan(*plan);
          const juce::String rigId = plan->reservation->rig->temporary
                                         ? juce::String("live (private copy)")
                                         : plan->reservation->rig->id;
          plan->reservation.reset();
          if (run.error.isNotEmpty())
            return fail(CommandError::failed, "render.clip: " + run.error);
          return CommandResult::success(object({
              {"rig", rigId},
              {"renders", run.reports},
              {"dynamics", dynamicsOf(run.renders)},
          }));
        });
      },
  });

  // ---- render.compare ----
  std::vector<CommandParam> compareArguments = clipParams();
  compareArguments.insert(
      compareArguments.begin(),
      P::required("reference", T::string,
                  "The recording to aim at: a WAV file, as an absolute path inside the allowed "
                  "folders, mono or stereo, at most 120 seconds.")
          .path());

  liveCommands.add({
      "render.compare",
      "Measures a reference recording and the rig's render of the chosen clips (all of them "
      "joined) the same way and reports how far apart they are: reference and rig (levels, "
      "spectrum, stereo), difference (rig minus reference: lufs, crestDb, bandsDb per band, "
      "centroidOctaves, correlation, width) and distance, a score per group (loudness, "
      "dynamics, tone, stereo) and overall, where 0 is identical and 1 is a difference a "
      "listener would not miss (3 dB of loudness or crest factor, 3 dB RMS across the bands "
      "or a third of an octave of centroid, 0.3 of correlation). Takes the clip arguments of "
      "render.clip.",
      compareArguments,
      false,
      CommandPermission::render,
      [this](const juce::var& args) {
        const juce::File reference(args["reference"].toString());
        if (!juce::File::isAbsolutePath(args["reference"].toString()))
          return CommandWork::done(fail(CommandError::invalidArgs,
                                        "render.compare: 'reference' must be an absolute path"));
        if (!reference.existsAsFile())
          return CommandWork::done(
              fail(CommandError::notFound, "file not found: " + reference.getFullPathName()));
        auto plan = std::make_shared<RenderPlan>();
        // The one caller that needs the audio after the run: it joins it.
        plan->keepProgramme = true;
        if (auto error = planRender(*this, args, "render.compare", *plan))
          return CommandWork::done(*error);

        return CommandWork::later([plan, reference] {
          juce::AudioBuffer<float> referenceAudio;
          double referenceRate = 0.0;
          juce::String error;
          if (!AudioFiles::read(reference, referenceAudio, referenceRate, error)) {
            plan->reservation.reset();
            return fail(CommandError::failed, "render.compare: " + error);
          }

          RenderRun run = runPlan(*plan);
          plan->reservation.reset();
          if (run.error.isNotEmpty())
            return fail(CommandError::failed, "render.compare: " + run.error);

          // One buffer of everything the rig played, so the comparison is
          // with the rig's sound as a whole and not with one take.
          int total = 0;
          for (const auto& [clip, rendered] : run.renders)
            total += rendered.programme.getNumSamples();
          juce::AudioBuffer<float> joined(2, total);
          int at = 0;
          for (auto& [clip, rendered] : run.renders) {
            const int frames = rendered.programme.getNumSamples();
            for (int ch = 0; ch < 2; ++ch)
              joined.copyFrom(ch, at, rendered.programme, ch, 0, frames);
            at += frames;
            // Given back as it is copied: the audio is held once, not twice.
            rendered.programme.setSize(0, 0);
          }
          const double rigRate = run.renders.empty() ? referenceRate : run.renders.front().second.rate;

          juce::AudioBuffer<float> referenceView(referenceAudio.getArrayOfWritePointers(),
                                                 referenceAudio.getNumChannels(),
                                                 referenceAudio.getNumSamples());
          const Measured rigMeasured = measure(joined, rigRate);
          const Measured referenceMeasured = measure(referenceView, referenceRate);
          juce::var result = compareMeasured(rigMeasured, referenceMeasured);
          result.getDynamicObject()->setProperty("reference", measuredVar(referenceMeasured));
          result.getDynamicObject()->setProperty("rig", measuredVar(rigMeasured));
          result.getDynamicObject()->setProperty("renders", static_cast<int>(run.renders.size()));
          return CommandResult::success(result);
        });
      },
  });

  // ---- render.probe ----
  liveCommands.add({
      "render.probe",
      "Measures a rig with a test signal instead of a guitar clip. response: plays noise at "
      "levelDb and returns the rig's gain per frequency (points: hz, gainDb, and coherence, "
      "which falls below 1 where the rig is not behaving linearly at that level) plus the "
      "overall distortion percent; meaningful for linear-ish rigs or low levels. levelCurve: "
      "plays a sine at frequencyHz in steps from -60 to 0 dBFS and returns the output level "
      "for each (points: inputDb, outputDb, gainDb, all as sine peak levels), which shows "
      "compression and gain staging. noise: plays silence and returns the noise floor "
      "(noiseFloorDb RMS, peakDb) and any mains hum. Uses the rig render.clip would.",
      {P::required("kind", T::string, "The probe to run.").oneOf({"response", "levelCurve", "noise"}),
       P::optional("levelDb", T::number, "response: the noise level, dBFS RMS.")
           .range(-80.0, -6.0)
           .withDefault(-30.0),
       P::optional("frequencyHz", T::number, "levelCurve: the sine's frequency.")
           .range(40.0, 8000.0)
           .withDefault(1000.0),
       P::optional("frequencies", T::array,
                   "response: the frequencies to report, in Hz. Default: third octaves from 25 "
                   "Hz to 16 kHz.")
           .items(T::number),
       P::optional("sandbox", T::string,
                   "The sandbox rig to probe. Default: the calling connection's selected "
                   "sandbox, or a private copy of the live rig.")},
      false,
      CommandPermission::render,
      [this](const juce::var& args) {
        std::vector<double> frequencies;
        if (const auto* list = args["frequencies"].getArray()) {
          if (list->size() > 128)
            return CommandWork::done(fail(CommandError::invalidArgs,
                                          "render.probe: at most 128 'frequencies'"));
          for (const auto& hz : *list) {
            if (static_cast<double>(hz) < 10.0 || static_cast<double>(hz) > 22000.0)
              return CommandWork::done(fail(CommandError::invalidArgs,
                                            "render.probe: 'frequencies' must lie between 10 "
                                            "and 22000 Hz"));
            frequencies.push_back(static_cast<double>(hz));
          }
        }
        if (frequencies.empty())
          frequencies = thirdOctaves();

        std::shared_ptr<Reservation> reservation;
        if (auto error = reserveRig(*this, args, reservation))
          return CommandWork::done(*error);
        const juce::String kind = args["kind"].toString();
        const double levelDb = args["levelDb"];
        const double frequencyHz = args["frequencyHz"];
        const std::atomic<bool>* cancel = &slowWorkCancel();

        return CommandWork::later([reservation, kind, levelDb, frequencyHz, frequencies,
                                   cancel]() mutable {
          constexpr double rate = 48000.0;
          SandboxRig& rig = *reservation->rig;
          const auto renderMono = [&](const std::vector<float>& signal, double tail) {
            juce::AudioBuffer<float> input(1, static_cast<int>(signal.size()));
            std::copy(signal.begin(), signal.end(), input.getWritePointer(0));
            return OfflineRender::render(rig.processor(), input, rate, tail, *cancel);
          };
          const auto finish = [&](CommandResult result) {
            reservation.reset();
            return result;
          };

          if (kind == "response") {
            // Uniform noise of amplitude a has an RMS of a / sqrt(3).
            const float amplitude =
                static_cast<float>(std::pow(10.0, levelDb / 20.0) * std::sqrt(3.0));
            const std::vector<float> noise = probeNoise(static_cast<int>(4.0 * rate), amplitude);
            const OfflineRender::Result out = renderMono(noise, 0.0);
            if (out.error.isNotEmpty())
              return finish(fail(CommandError::failed, "render.probe: " + out.error));

            const int frames = static_cast<int>(noise.size());
            const AudioMeasure::LinearFit left(noise.data(), out.audio.getReadPointer(0), frames, rate);
            const AudioMeasure::LinearFit right(noise.data(), out.audio.getReadPointer(1), frames, rate);
            juce::Array<juce::var> points;
            for (const double hz : frequencies) {
              if (hz >= rate / 2.0)
                continue;
              // The two sides pooled by power, as a mono listener gets them.
              const double gain = AudioMeasure::powerToDb(
                  0.5 * (std::pow(10.0, left.gainDbAt(hz) / 10.0) +
                         std::pow(10.0, right.gainDbAt(hz) / 10.0)));
              points.add(object({
                  {"hz", hz},
                  {"gainDb", AudioMeasure::tidyDb(gain)},
                  {"coherence", rounded(0.5 * (left.coherenceAt(hz) + right.coherenceAt(hz)), 3)},
              }));
            }
            juce::AudioBuffer<float> input(1, frames);
            std::copy(noise.begin(), noise.end(), input.getWritePointer(0));
            return finish(CommandResult::success(object({
                {"kind", kind},
                {"levelDb", levelDb},
                {"points", points},
                {"distortion", AudioMeasure::toVar(stereoDistortion(input, out.audio, frames, rate))},
                {"latencySamples", out.latencySamples},
            })));
          }

          if (kind == "levelCurve") {
            // One continuous sine that steps up every 0.6 s; each step is
            // read over its second half, after a compressor has settled.
            constexpr int kSteps = 11;
            const int stepFrames = static_cast<int>(0.6 * rate);
            std::vector<float> sine(static_cast<size_t>(kSteps * stepFrames));
            for (int i = 0; i < kSteps * stepFrames; ++i) {
              const double inputDb = -60.0 + 6.0 * (i / stepFrames);
              sine[static_cast<size_t>(i)] = static_cast<float>(
                  std::pow(10.0, inputDb / 20.0) * std::sin(2.0 * kPi * frequencyHz * i / rate));
            }
            const OfflineRender::Result out = renderMono(sine, 0.0);
            if (out.error.isNotEmpty())
              return finish(fail(CommandError::failed, "render.probe: " + out.error));

            juce::Array<juce::var> points;
            for (int step = 0; step < kSteps; ++step) {
              const int start = step * stepFrames + stepFrames / 2;
              double sum = 0.0;
              for (int ch = 0; ch < 2; ++ch) {
                const float* y = out.audio.getReadPointer(ch);
                for (int i = start; i < (step + 1) * stepFrames; ++i)
                  sum += static_cast<double>(y[i]) * y[i];
              }
              const double inputDb = -60.0 + 6.0 * step;
              // As the peak of a sine of that RMS, so unity gain reads 0.
              const double outputDb =
                  AudioMeasure::powerToDb(sum / (2.0 * (stepFrames - stepFrames / 2))) + 3.0103;
              points.add(object({
                  {"inputDb", inputDb},
                  {"outputDb", AudioMeasure::tidyDb(outputDb)},
                  {"gainDb", AudioMeasure::tidyDb(outputDb - inputDb)},
              }));
            }
            return finish(CommandResult::success(object({
                {"kind", kind},
                {"frequencyHz", frequencyHz},
                {"points", points},
            })));
          }

          // noise: two seconds of silence in, read after the first half
          // second (a gate or a fade may still be moving before that).
          const std::vector<float> silence(static_cast<size_t>(2.0 * rate), 0.0f);
          OfflineRender::Result out = renderMono(silence, 0.0);
          if (out.error.isNotEmpty())
            return finish(fail(CommandError::failed, "render.probe: " + out.error));
          const int skip = static_cast<int>(0.5 * rate);
          juce::AudioBuffer<float> settled(out.audio.getArrayOfWritePointers(), 2, skip,
                                           out.audio.getNumSamples() - skip);
          const AudioMeasure::Levels levels = AudioMeasure::measureLevels(settled, rate);
          const AudioMeasure::Hum hum =
              AudioMeasure::measureHum(settled.getReadPointer(0), settled.getNumSamples(), rate);
          return finish(CommandResult::success(object({
              {"kind", kind},
              {"noiseFloorDb", AudioMeasure::tidyDb(levels.rmsDb)},
              {"peakDb", AudioMeasure::tidyDb(levels.peakDb)},
              {"hum", AudioMeasure::toVar(hum)},
          })));
        });
      },
  });
}

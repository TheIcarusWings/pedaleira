#pragma once

#include "CommandRegistry.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <mutex>
#include <thread>
#include <vector>

class TONE3000Processor;

// WAV in and out for the agent and library layer (clips, references, renders
// kept on request).
namespace AudioFiles {
// What is accepted: mono or stereo, 8 to 192 kHz.
constexpr int kMaxChannels = 2;
constexpr double kLowestSampleRate = 8000.0;
constexpr double kHighestSampleRate = 192000.0;

// What a WAV's header says, once it has passed the checks read() makes.
struct Info {
  double sampleRate = 0.0;
  int channels = 0;
  juce::int64 frames = 0;
  double seconds() const { return sampleRate > 0.0 ? static_cast<double>(frames) / sampleRate : 0.0; }
};
// The header alone, checked: no samples are read and nothing is allocated
// for them. False with `error` set when read() would refuse the file.
bool inspect(const juce::File& file, Info& info, juce::String& error, double maxSeconds = 120.0);

// Reads a mono or stereo WAV of at most `maxSeconds`. False with `error` set
// otherwise. Before anything is allocated the header is held against the
// limits (channels, sample rate, length) and against the file's real size: a
// header that claims more audio than the file holds is refused.
bool read(const juce::File& file, juce::AudioBuffer<float>& audio, double& sampleRate,
          juce::String& error, double maxSeconds = 120.0);
// Writes a 32-bit float WAV, replacing the file.
bool write(const juce::File& file, const juce::AudioBuffer<float>& audio, double sampleRate);
}  // namespace AudioFiles

// Guitar profiles: what one particular guitar puts into the rig, as recorded
// DI clips with their measurements. Different guitars drive the same rig
// differently, so a tone is built and measured with the clips of the guitar
// it is for (render.clip takes a guitar).
//
// On disk, under the library root (LibraryPaths):
//
//   Guitars/guitars.json            { "default": "<id>" }
//   Guitars/<id>/profile.json       the profile (below)
//   Guitars/<id>/<position>-<take>.wav
//
// A profile: { version, id, name, pickupType, positions: [..], tuning, notes,
// created, takes: [{ position, take, file, sampleRate, seconds, channels,
// measurements }], summary }. A take is one of soft, hard, chords or notes,
// recorded per pickup position. `measurements` and `summary` are computed
// (analyze), never taken from a caller.
//
// Message thread, except the writer thread a finished recording is handed
// to; the file mutex keeps the two apart.
class GuitarLibrary {
public:
  explicit GuitarLibrary(TONE3000Processor& liveRig);
  ~GuitarLibrary();

  static const juce::StringArray& takeNames();
  static const juce::StringArray& pickupTypes();

  // { guitars: [{ id, name, pickupType, positions, tuning, takeCount,
  // isDefault, summary }], default }
  juce::var list() const;
  // The whole profile plus isDefault, or void when there is none of that id.
  juce::var get(const juce::String& id) const;
  juce::String defaultId() const;

  CommandResult create(const juce::var& args);
  CommandResult update(const juce::String& id, const juce::var& args);
  CommandResult remove(const juce::String& id);
  CommandResult setDefault(const juce::String& id);
  // Copies `source` into the profile as the take of that position (replacing
  // one already there) and measures it.
  CommandResult importClip(const juce::String& id, const juce::String& position,
                           const juce::String& take, const juce::File& source);
  // Measures every take again and rebuilds the summary.
  CommandResult analyze(const juce::String& id);

  // The clips a render uses: the takes of `id` (empty: the default guitar),
  // narrowed to `position` and `takes` when given.
  struct Clip {
    juce::String guitar, position, take;
    juce::File file;
  };
  std::vector<Clip> clips(const juce::String& id, const juce::String& position,
                          const juce::StringArray& takes, juce::String& error) const;

  // Recording from the live input (see InputRecorder.h for the audio-thread
  // half). start arms it; the audio thread fills the buffer; the first
  // status call after it is full hands the samples to a writer thread, which
  // saves the WAV, measures it and adds the take to the profile.
  CommandResult startRecording(const juce::String& id, const juce::String& position,
                               const juce::String& take, double seconds, int channel);
  // { state: idle | recording | writing | done | failed, guitar, position,
  // take, seconds, recordedSeconds, progress, peakDb, error?, take? }
  juce::var recordingStatus();
  void cancelRecording();

  // The measurements of one clip file, as stored in a take.
  static juce::var measureClip(const juce::File& file, juce::String& error);

private:
  // <library root>/Guitars, in the library the live rig uses.
  juce::File guitarsDir() const;
  juce::File profileDir(const juce::String& id) const;
  juce::var readProfile(const juce::String& id) const;
  bool writeProfile(const juce::String& id, const juce::var& profile) const;
  // Adds or replaces the take's entry and rebuilds the summary. Locks.
  bool storeTake(const juce::String& id, const juce::String& position, const juce::String& take,
                 const juce::File& clip, juce::String& error);
  static juce::var summarise(const juce::var& takes);
  void joinWriter();

  TONE3000Processor& live;

  // Profile files are read and written by the message thread and by the
  // writer thread.
  mutable std::mutex fileMutex;

  struct Recording {
    juce::String state = "idle";
    juce::String guitar, position, take, error;
    double sampleRate = 0.0;
    double seconds = 0.0;
    juce::var result;
  };
  // Guards `recording`, which the writer thread finishes.
  std::mutex recordingMutex;
  Recording recording;
  std::thread writer;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GuitarLibrary)
};

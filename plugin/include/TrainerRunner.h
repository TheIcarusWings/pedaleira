#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>

// Runs the NAM trainer (the `neural-amp-modeler` Python package) as a child
// process for a capture, and reads the model it makes. The trainer is never
// linked or copied: Pedaleira starts Python with a small helper script of its
// own (writeHelperScript) that calls the trainer's public entry points, and
// reads the helper's progress lines from its output.
//
// Version 0.13.0 is the first whose standard training makes NAM A2 files
// (architecture "SlimmableContainer": the Full and Lite models in one file),
// which is the only kind the engine loads, so that is the minimum. A file
// that comes out as anything else is refused with a message saying so.
//
// Threads: start(), cancel() and status() are the message thread's (status()
// may be read from anywhere); the run itself is on a thread of its own and
// never touches the message thread.
class TrainerRunner {
public:
  static constexpr const char* kMinimumVersion = "0.13.0";

  // `python` -c "import nam; print(nam.__version__)", read.
  struct Install {
    bool pythonFound = false;
    bool packageFound = false;
    bool versionOk = false;
    juce::String python;   // what was run
    juce::String version;  // "0.13.0"
    juce::String message;  // what to do about it when not versionOk
  };

  // Blocks (importing the package takes seconds the first time), so not for
  // the message thread. `python` empty: python3 (python on Windows).
  static Install checkInstall(const juce::String& python,
                              const std::atomic<bool>* cancel = nullptr);
  static juce::String defaultPython();
  // True when version "a.b.c" (a leading v, a suffix such as ".dev1" or "rc1"
  // are tolerated) is at least `minimum`.
  static bool versionAtLeast(const juce::String& version, const juce::String& minimum);

  // The ESR bands of the NAM project: under 0.01 great, under 0.035 not bad,
  // under 0.1 might be okay, under 0.3 poor, above that failed.
  static const char* esrBand(double esr);
  static const char* esrBandLabel(double esr);

  struct Job {
    juce::String python;  // empty: the default
    juce::String mode;    // "reamp" (the standard input file) or "dryWet"
    juce::File workDir;   // the capture folder: the job file and the helper go here
    juce::File outDir;    // the trainer writes here (made if missing)
    juce::File input;     // x: the standard input file, or the dry take
    juce::File output;    // y: what came back, or the wet take
    juce::String modelName = "model";
    int epochs = 100;
    // The delay of the output behind the input, in samples. Unset: the
    // trainer calibrates it itself (re-amp with the standard file).
    std::optional<int> latency;
    bool ignoreChecks = false;
    // dryWet: how many samples at the end are held back for validation.
    int validationSamples = 480000;
    // The trainer's user metadata: name, modeled_by, gear_type, gear_make,
    // gear_model, tone_type, input_level_dbu, output_level_dbu.
    juce::var metadata;
    // Skip the install check (tests with a stand-in trainer that has no
    // package to import still answer it).
    bool checkInstall = true;
  };

  TrainerRunner() = default;
  ~TrainerRunner();

  // Starts the run. Fails at once only when a run is already going or the
  // job cannot be written; everything else (no Python, no package, an old
  // version) shows up in status() as `failed` with a message.
  juce::Result start(const Job& job);
  // Kills the child process; status() then says `cancelled`.
  void cancel();
  bool isActive() const;

  // { state: idle | checking | running | done | failed | cancelled,
  //   epoch, epochs, progress (0 to 1), secondsLeft?, esr?, esrBand?,
  //   esrBandLabel?, latencySamples?, trainerChecksPassed?, modelFile?,
  //   architecture?, message, error?, log: [last lines], elapsedSeconds }
  juce::var status() const;
  juce::String state() const;

  // The trainer's own words, newest last.
  juce::StringArray lastLines(int count) const;

  // The helper script the trainer is started with (for the reference docs
  // and for tests that check what is written).
  static juce::String helperScriptSource();

private:
  void run(Job job);
  void setState(const char* state, const juce::String& message = {});
  void noteLine(const juce::String& line, int epochs);
  void finishFromProcess(const Job& job, int exitCode, bool cancelled);

  mutable std::mutex mutex;
  std::thread worker;
  std::atomic<bool> cancelRequested{false};
  std::atomic<bool> active{false};
  juce::ChildProcess* activeProcess = nullptr;

  // Guarded by `mutex`.
  juce::String stateName = "idle";
  juce::String message, error;
  int epoch = 0, epochs = 0;
  double esr = -1.0;
  bool hasLatency = false;
  int latencySamples = 0;
  bool hasChecks = false;
  bool checksPassed = false;
  juce::String modelFile, architecture;
  juce::Time started;
  juce::Time finished;
  std::deque<juce::String> log;
  juce::var resultLine;
};

// Reading and copying NAM files, for what training makes.
namespace NamFile {

struct Info {
  juce::String architecture, version;
  juce::var metadata;
  double sampleRate = 0.0;
  bool hasEsr = false;
  double validationEsr = 0.0;
};

// Parses a .nam (JSON, up to 256 MB). Fails with a message for anything
// that is not a readable model file.
juce::Result read(const juce::File& file, Info& info);

// Fails with a message unless the file is NAM A2.
juce::Result requireA2(const Info& info);

// True when every key of `updates` (a flat object) already has that value in
// the file's metadata object.
bool metadataHas(const Info& info, const juce::var& updates);

// Writes `destination` as a copy of `source` whose "metadata" object has
// `updates` set (each key replaced; a null value is skipped). Nothing else in
// the file is touched. The source stays as it is.
juce::Result writeCopyWithMetadata(const juce::File& source, const juce::File& destination,
                                   const juce::var& updates);

}  // namespace NamFile

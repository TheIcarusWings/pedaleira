#pragma once

#include "CaptureSession.h"
#include "CommandRegistry.h"
#include "TrainerRunner.h"

#include <juce_events/juce_events.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class TONE3000Processor;

// What runs a capture (docs/capture.md): the sessions on disk, the audio run
// on the live processor's CaptureAudio, the data checks, the trainer and the
// review. The capture.* commands (CaptureCommands.cpp) are thin calls into
// this class, which is why its methods take the commands' arguments and answer
// with CommandResults.
//
// Threads: the message thread, like the commands. Slow parts leave it: the
// checks read the recordings on the thread of the command that asked for them
// (CommandWork::later), a finished recording is written by a thread of its
// own, and the trainer is a child process on a thread of its own. None of it
// touches the audio thread except through CaptureAudio's atomics.
//
// One audio run at a time (the device is one); training runs beside it, and
// beside playing, because it is another process.
class CaptureStudio : private juce::Timer {
public:
  CaptureStudio(TONE3000Processor& live, CommandRegistry& liveCommands);
  ~CaptureStudio() override;

  // ---- Settings (the agent settings file) ----
  // { pythonExecutable, standardInputFile, levelTargetDb, effectivePython,
  //   standardInput: { exists, recognised, version, md5, seconds, sampleRate,
  //   message } }
  CommandResult getSettings();
  CommandResult setSettings(const juce::var& args);
  // Python and the trainer: { found, version, versionOk, message }. Blocks
  // for seconds; run it off the message thread.
  juce::var checkTrainer(const std::atomic<bool>* cancel);

  // ---- Sessions ----
  CommandResult create(const juce::var& args);
  CommandResult get(const juce::String& id);
  CommandResult list();
  CommandResult setSource(const juce::var& args);
  CommandResult setChannels(const juce::var& args);
  CommandResult setLevels(const juce::var& args);
  CommandResult setStep(const juce::var& args);
  CommandResult remove(const juce::String& id);

  // ---- Levels ----
  CommandResult startTone(const juce::var& args);
  CommandResult stopTone(const juce::String& id);
  CommandResult levelStatus(const juce::var& args);
  CommandResult measureNoise(const juce::var& args);

  // ---- Recording ----
  CommandResult startRecording(const juce::var& args);
  CommandResult cancelRecording(const juce::String& id);
  CommandResult recordingStatus(const juce::String& id);

  // ---- Checks ----
  // Quick part: validates and reads the session. The returned work reads the
  // recordings and runs the checks off the message thread.
  CommandWork runChecks(const juce::var& args);

  // ---- Training ----
  CommandResult startTraining(const juce::var& args);
  CommandResult cancelTraining(const juce::String& id);
  CommandResult trainingStatus(const juce::String& id);

  // ---- Review ----
  CommandResult review(const juce::String& id);
  CommandResult addToRig(const juce::String& id);
  CommandResult save(const juce::var& args);

  // ---- For tests and the hub ----
  juce::File capturesDir() const;
  juce::File sessionDir(const juce::String& id) const;
  // Reads a session from disk; false with `error` when there is none.
  bool load(const juce::String& id, CaptureSession& session, juce::String& error) const;
  // Applies finished work (a written recording, a finished training) to the
  // sessions. The timer does this while anything is running; status commands
  // do it too.
  void poll();
  // Stops anything running (application quit, tests).
  void stopAll();
  // The id the audio run belongs to; empty when idle.
  juce::String audioRunOwner() const { return audioOwner; }

  // Where a re-amp recording is cut from, for the checks: the standard input
  // file as played.
  struct StandardInput {
    bool exists = false;
    bool readable = false;
    bool recognised = false;
    juce::String version;  // "3.0.0" when the checksum is a known one
    juce::String md5;
    double seconds = 0.0;
    double sampleRate = 0.0;
    juce::String message;
  };
  static StandardInput identifyInput(const juce::File& file);
  // The checksums of the standard input files the trainer recognises.
  static const juce::StringArray& knownInputChecksums();

private:
  void timerCallback() override;
  void updateTimer();

  bool store(const CaptureSession& session, juce::String* error = nullptr) const;
  CommandResult withSession(const juce::String& id, CaptureSession& session) const;
  static bool validId(const juce::String& id);
  void deleteStale(const CaptureSession& session, const std::vector<juce::String>& stale) const;

  // Refreshes `session.levels` from the meters of a running levels run.
  void refreshLevels(CaptureSession& session);
  CommandResult audioBusy(const juce::String& id) const;
  juce::String audioRefusal() const;
  juce::var sessionVar(const CaptureSession& session) const;
  juce::var levelsVar(const CaptureSession& session, bool active) const;
  juce::var recordingVar(const CaptureSession& session) const;
  void applyFinishedRecording();
  void applyTrainerResult();
  CommandResult stepResult(const CaptureSession& session, const std::vector<juce::String>& stale);

  TONE3000Processor& live;
  CommandRegistry& registry;

  // The audio run in progress, if any.
  struct Run {
    juce::String id;
    enum class Kind { levels, recording } kind = Kind::levels;
    Capture::Method method = Capture::Method::reamp;
    int playedSamples = 0;  // re-amp: length of the signal that goes out
    int preRoll = 0;
    int plannedSamples = 0;
    // The live latency estimate, found once enough has been recorded.
    bool latencyTried = false;
    bool latencyKnown = false;
    int latencySamples = 0;
    std::vector<float> reference;  // the start of the played signal, for it
  };
  std::unique_ptr<Run> run;
  juce::String audioOwner;

  // A finished recording on its way to disk.
  struct Written {
    bool done = false;
    bool ok = false;
    juce::String id, error;
    int samples = 0;
    juce::String returnFile, dryFile, wetFile;
  };
  std::thread writer;
  std::mutex writtenMutex;
  Written written;
  std::atomic<bool> writing{false};

  TrainerRunner trainer;
  juce::String trainingId;
  juce::String trainingState;  // the state last applied to the session

  std::shared_ptr<bool> lifetime = std::make_shared<bool>(true);

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CaptureStudio)
};

#pragma once

#include <juce_core/juce_core.h>

#include <optional>
#include <vector>

// The state of one capture, the wizard's data (docs/capture.md). A session is
// plain data: the steps, what each needs before the wizard may go on, and its
// JSON on disk. What runs the steps (the audio, the checks, the trainer) is
// CaptureStudio; this file knows nothing of the processor.
//
// A session lives in a folder of its own under the library,
//
//   <library>/Captures/<id>/session.json
//   <library>/Captures/<id>/return.wav | dry.wav, wet.wav   the recordings
//   <library>/Captures/<id>/training/                       the trainer's work (until it is done)
//   <library>/Captures/<id>/model.nam                       the trained model
//
// and a saved capture is moved to <library>/Captures/<readable name>.nam.
namespace Capture {

enum class Source { pedal, ampHead, ampCab, modeller };
enum class Method { reamp, dryWet };
enum class Step { source, wiring, levels, record, check, train, review };
enum class GainTag { clean, crunch, highGain };
enum class ModelSize { full, lite };

constexpr int kStepCount = 7;
constexpr double kCaptureSampleRate = 48000.0;

const char* name(Source value);
const char* name(Method value);
const char* name(Step value);
const char* name(GainTag value);
const char* name(ModelSize value);
bool parse(const juce::String& text, Source& out);
bool parse(const juce::String& text, Method& out);
bool parse(const juce::String& text, Step& out);
bool parse(const juce::String& text, GainTag& out);
bool parse(const juce::String& text, ModelSize& out);

// The gear types of TONE3000's upload form: ampHead, ampCab, cabinet, pedal,
// outboard, spaces, experimental.
const juce::StringArray& gearTypes();
// The gear type a source starts with.
const char* defaultGearType(Source source);
// The values NAM's own metadata allows (amp, pedal, pedal_amp, amp_cab,
// amp_pedal_cab, preamp, studio) for a TONE3000 gear type, and the tone type
// of a gain tag (clean, crunch, hi_gain).
const char* namGearType(const juce::String& gearType);
const char* namToneType(GainTag tag);

}  // namespace Capture

struct CaptureSession {
  using Source = Capture::Source;
  using Method = Capture::Method;
  using Step = Capture::Step;

  struct Gear {
    juce::String type;  // one of Capture::gearTypes()
    juce::String make, model;
  };
  // Channel numbers counted from 0 on the device's inputs and outputs. Today
  // the processor has the main stereo pair only, so 0 and 1 are the usable
  // values (CaptureAudio.h); grid stage 8 widens that.
  struct Channels {
    int send = 0;      // output the signal goes out on (re-amp)
    int returnIn = 0;  // input the gear comes back on (re-amp)
    int dry = 0;       // dry/wet: the guitar before the gear
    int wet = 1;       // dry/wet: the gear
  };
  struct Levels {
    double sendDb = -24.0;   // the test tone's and the signal's peak on the send
    double targetDb = -12.0;
    double bandDb = 3.0;
    bool measured = false;
    bool inRange = false;
    double peakDb = -120.0;     // peak hold of the return (dry/wet: the dry)
    double wetPeakDb = -120.0;  // dry/wet: the wet
    double rmsDb = -120.0;
    std::optional<double> noiseFloorDb;
    int clippedSamples = 0;
  };
  struct Recording {
    // none, recording, writing, complete, failed
    juce::String state = "none";
    double seconds = 0.0;
    int samples = 0;
    double sampleRate = Capture::kCaptureSampleRate;
    juce::String inputFile;  // re-amp: the standard input file that was played
    juce::String inputVersion;
    juce::String error;
    // dry/wet: how long to record
    double plannedSeconds = 180.0;
    bool complete() const { return state == "complete"; }
  };
  struct Training {
    // none, running, done, failed, cancelled
    juce::String state = "none";
    int epochs = 100;
    int epoch = 0;
    std::optional<double> esr;
    std::optional<int> latencySamples;
    std::optional<bool> trainerChecksPassed;
    juce::String error;
    bool done() const { return state == "done"; }
  };

  int version = 1;
  juce::String id, name, notes;
  juce::String created, updated;
  Source source = Source::ampHead;
  Method method = Method::reamp;
  Capture::GainTag gainTag = Capture::GainTag::crunch;
  Capture::ModelSize modelSize = Capture::ModelSize::full;
  Gear gear;
  Channels channels;
  // 0 dBFS at the return input is this many dBu (the Volt 876: +12.5), saved
  // in the capture as output_level_dbu. The send's own full scale
  // (input_level_dbu) is saved when known.
  double calibrationDbu = 12.5;
  std::optional<double> sendCalibrationDbu;
  Levels levels;
  Step step = Step::source;
  Recording recording;
  // The data checks' report (CaptureChecks::Report::toVar), void until run.
  juce::var checks;
  bool checksOverridden = false;
  Training training;
  // The trained file, relative to the session folder, and the saved capture.
  juce::String modelFile;
  juce::String savedFile;
  // The files the recordings were written to, relative to the session folder.
  juce::String returnFile, dryFile, wetFile;

  static CaptureSession make(const juce::String& id, const juce::String& name);

  juce::var toVar() const;
  // False with `error` for a file that is not a session at all; values it
  // does not understand fall back to the defaults.
  static bool fromVar(const juce::var& value, CaptureSession& out, juce::String& error);

  // A session read from disk whose recording or training was going when the
  // application stopped: neither is going now. The flags say which of them
  // really is going in this process.
  void markInterrupted(bool recordingGoing, bool trainingGoing);

  // ---- The steps ----
  // Empty when the wizard may leave the current step for the next one;
  // otherwise what is missing, in words.
  juce::String whatBlocks() const;
  // Empty when the move is allowed. Forward: one step, when whatBlocks() is
  // empty. Back: any earlier step. The same step: allowed.
  juce::String whyCannotMoveTo(Step target) const;
  // Moves, clearing everything that belongs to the steps left behind or
  // re-entered (going back to `record` throws the recording away, ...).
  // Returns the files of the session folder that are no longer valid.
  std::vector<juce::String> moveTo(Step target);
  // Clears what steps `from` and later own. Returns the stale files.
  std::vector<juce::String> clearFrom(Step from);

  // The metadata the trainer writes into the model, and what a saved copy
  // is made to carry (NAM metadata keys).
  juce::var namMetadata() const;
  // "Brit 800 Crunch": the readable file name of a saved capture, without
  // the extension.
  juce::String readableFileName() const;

  // The in-range band of the level target.
  double bandLow() const { return levels.targetDb - levels.bandDb; }
  double bandHigh() const { return levels.targetDb + levels.bandDb; }

  // A short summary for lists.
  juce::var summary() const;
};

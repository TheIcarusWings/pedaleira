#pragma once

#include <juce_core/juce_core.h>

#include <vector>

// Our own fast checks on a capture recording, run before the trainer is
// started (the trainer runs its own and its verdict is shown after training).
// Pure functions of the samples: no processor, no files, so they can be
// tested with synthetic recordings.
//
// A re-amp recording is checked against the signal that was played; a dry/wet
// recording is two takes of the same playing, one before the gear and one
// after it. Each check answers pass, warn or fail with a message the wizard
// can show as it is. The overall verdict is the worst of them: `fail` stops
// the wizard from going on (unless the user overrides), `warn` does not.
namespace CaptureChecks {

enum class Status { pass, warn, fail };

const char* statusName(Status status);

struct Options {
  double sampleRate = 48000.0;
  // The level the peak should reach and how far either side of it counts
  // as in range. Twice the band is the most a warning allows.
  double targetDb = -12.0;
  double bandDb = 3.0;
  // At or above this a sample counts as clipped.
  double clipDb = -0.1;
  // A delay beyond this is more than the trainer's own calibration can find
  // (it looks 10000 samples back).
  int maxLatencySamples = 9000;
  // How far to look for the delay, in samples.
  int searchLagSamples = 24000;
  // Dry/wet: the length wanted. Below `minSeconds` a warning, below
  // `failSeconds` a failure.
  double minSeconds = 120.0;
  double failSeconds = 20.0;
  // Longest stretch of the signals the delay is estimated over.
  double latencyWindowSeconds = 10.0;
};

struct Result {
  juce::String id, name;
  Status status = Status::pass;
  // The measured value as text ("-12.4 dBFS"), and what it means.
  juce::String value, message;
  // The number itself, when there is one.
  double number = 0.0;
  bool hasNumber = false;
};

struct Report {
  std::vector<Result> results;
  Status verdict = Status::pass;
  // The delay of the return behind what was played (dry/wet: of the wet
  // behind the dry; negative: the wet came first), when it could be found.
  bool latencyKnown = false;
  int latencySamples = 0;
  double latencyMs = 0.0;
  // 0 to 1: how well the two signals line up at that delay.
  double latencyConfidence = 0.0;

  const Result* find(const juce::String& id) const;
  int count(Status status) const;
  // { verdict, passed, warnings, failures, latencySamples?, latencyMs?,
  //   checks: [{ id, name, status, value, message }] }
  juce::var toVar() const;
  static Report fromResults(std::vector<Result> results);
};

// How far `response` lags `reference`: the lag in [0, maxLag] where they
// correlate best (the sign of the correlation is ignored), over at most
// `window` samples of the reference. `confidence` is the normalised
// correlation at that lag.
struct LatencyEstimate {
  bool found = false;
  int samples = 0;
  double confidence = 0.0;
};
LatencyEstimate estimateLatency(const std::vector<float>& reference,
                                const std::vector<float>& response, int maxLag, int window,
                                int referenceStart = 0);

// Level helpers, in dBFS (-120 for silence).
double peakDb(const std::vector<float>& x);
double rmsDb(const std::vector<float>& x);
double dcOffsetDb(const std::vector<float>& x);
// The RMS of the quietest 100 ms stretch.
double quietestStretchDb(const std::vector<float>& x, double sampleRate);

// Re-amp: `played` is what went out (the trainer's input file as played),
// `returned` what came back, cut to the played length from the first played
// sample on.
Report checkReamp(const std::vector<float>& played, const std::vector<float>& returned,
                  const Options& options);

// Dry/wet: the dry input and the wet input, recorded together.
Report checkDryWet(const std::vector<float>& dry, const std::vector<float>& wet,
                   const Options& options);

}  // namespace CaptureChecks

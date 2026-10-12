#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// The effect-matching measurement kit. A test signal with a fixed layout is
// played through a reference unit and through one of our effects; both
// recordings are analysed into the same set of numbers, and the numbers are
// compared. Nothing here knows what is inside the unit under test: it is
// measured from its audio output alone. The workflow is in docs/matching.md.
//
// Pure functions over buffers, offline only (allocates freely, never for the
// audio thread). The same samples always give the same numbers.
namespace EffectMatch {

// Bumped whenever the layout or the content of the signal changes; analyses
// and recordings made with another version cannot be compared.
constexpr int kSignalVersion = 1;
constexpr double kSignalRate = 48000.0;

// A recording that cannot be analysed (no sync markers, too short, ...). The
// message says what to look at.
struct MatchError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// ####################
// TEST SIGNAL
// ####################
struct Segment {
  std::string name;
  // "sync", "sweep", "sine", "staircase", "steps", "noiseBurst" or "silence".
  std::string kind;
  // Where the signal part starts, and how long it is. The silence that
  // follows (for tails) is params["tailSamples"], not part of lengthSamples.
  std::int64_t startSample = 0;
  std::int64_t lengthSamples = 0;
  // Everything a reader needs to interpret the segment (levels in dBFS,
  // frequencies in Hz, other positions in samples relative to startSample).
  std::map<std::string, double> params;
};

struct TestSignal {
  int version = kSignalVersion;
  double sampleRate = kSignalRate;
  std::vector<float> samples;
  std::vector<Segment> segments;

  const Segment* find(const std::string& name) const;
};

// The whole signal, mono, at `sampleRate` (positions scale with the rate).
TestSignal makeTestSignal(double sampleRate = kSignalRate);
// The manifest as JSON: version, rate, total length and the segments.
juce::var toVar(const TestSignal& signal);

// ####################
// ANALYSIS
// ####################

// 1/3-octave band centres, 31.5 Hz to 16 kHz.
constexpr int kNumBands = 28;
constexpr std::array<double, kNumBands> kBandCentresHz = {
    31.5,  40.0,   50.0,   63.0,   80.0,   100.0,  125.0,  160.0,  200.0,  250.0,
    315.0, 400.0,  500.0,  630.0,  800.0,  1000.0, 1250.0, 1600.0, 2000.0, 2500.0,
    3150.0, 4000.0, 5000.0, 6300.0, 8000.0, 10000.0, 12500.0, 16000.0};
constexpr int kReferenceBand = 15;  // 1 kHz

struct Alignment {
  // Where the first sync marker was found against where it was sent.
  double latencyMs = 0.0;
  // How much longer (positive) or shorter the recording's time runs than the
  // signal's, in parts per million, from the spacing of the two markers.
  double driftPpm = 0.0;
  // How well each marker matched (normalised correlation, 0 to 1).
  double startScore = 0.0;
  double endScore = 0.0;
};

struct Echo {
  // After the direct sound, and relative to it (energy in a 4 ms window).
  double timeMs = 0.0;
  double levelDb = 0.0;
};

struct Decay {
  // Reverberation times extrapolated to 60 dB: from the first 10 dB of the
  // Schroeder decay curve (EDT) and from -5 to -35 dB (T30).
  std::optional<double> edtS;
  std::optional<double> t30S;
};

struct SweepAnalysis {
  // Early response (direct sound and the 100 ms after it) per 1/3-octave
  // band, in dB, normalised to the 1 kHz band.
  std::array<double, kNumBands> responseDb{};
  // Level of the 1 kHz band against the same band of the unprocessed sweep.
  double levelGainDb = 0.0;
  // Echoes after the direct sound (the first peak of the impulse response).
  std::vector<Echo> echoes;
  // Spacing and level step of a regular echo train; unset when the echoes
  // are not regular (a reverb, or no echoes).
  std::optional<double> delayMs;
  std::optional<double> feedbackDb;
  // Left out for a clean echo train, where the decay curve is a staircase.
  Decay decay;
};

struct SineAnalysis {
  double hz = 0.0;
  // Level of the steady part, dBFS peak.
  double levelDb = 0.0;
  // Harmonics 2 to 10 over the fundamental; unset when the sine was too
  // quiet to judge.
  std::optional<double> thdPct;
  // Peak-to-peak variation of the envelope, dB; the dominant rate of that
  // variation, unset when there is none worth reporting.
  double ampDepthDb = 0.0;
  std::optional<double> ampRateHz;
  // Peak-to-peak variation of the instantaneous frequency, in cents.
  double pitchDepthCents = 0.0;
  std::optional<double> pitchRateHz;
};

struct CurvePoint {
  double inDb = 0.0;
  double outDb = 0.0;
};

struct StepResponse {
  // Time for the output level to cover 63% of its movement after the level
  // steps up (attack) and back down (release); unset when it did not move.
  std::optional<double> attackMs;
  std::optional<double> releaseMs;
};

struct BurstAnalysis {
  // Decay time of the tail after the noise burst, extrapolated to 60 dB.
  std::optional<double> tailT60s;
  // From the end of the burst until the output stays 60 dB under its level
  // during the burst; truncated when the recording ended first.
  double tailEndMs = 0.0;
  bool truncated = false;
};

struct ChannelAnalysis {
  std::optional<SweepAnalysis> sweepLow;
  std::optional<SweepAnalysis> sweepHigh;
  std::optional<SineAnalysis> sine100;
  std::optional<SineAnalysis> sine1k;
  std::vector<CurvePoint> staticCurve;
  StepResponse steps;
  BurstAnalysis burst;
  // Level of the silence segment, dBFS RMS.
  double noiseFloorDb = -120.0;
};

struct StereoAnalysis {
  // Left/right correlation and side-to-total amplitude ratio over the noise
  // burst and its tail (AudioMeasure::Stereo).
  double correlation = 1.0;
  double width = 0.0;
};

struct MatchAnalysis {
  int version = kSignalVersion;
  double sampleRate = kSignalRate;
  int channels = 1;
  Alignment alignment;
  ChannelAnalysis left;
  std::optional<ChannelAnalysis> right;
  std::optional<StereoAnalysis> stereo;
};

// Throws MatchError when the sync markers are not found. `reference` must be
// the signal that was played (makeTestSignal at the recording's rate).
MatchAnalysis analyseRecording(const juce::AudioBuffer<float>& recording, double sampleRate,
                               const TestSignal& reference);

juce::var toVar(const MatchAnalysis& analysis);
// The inverse of toVar; throws MatchError when the JSON is not an analysis.
MatchAnalysis analysisFromVar(const juce::var& json);

// ####################
// COMPARISON
// ####################

// A metric passes when |candidate - reference| <= max(absolute, relative x
// |reference|).
struct Tolerances {
  double responseDb = 1.0;
  double levelGainDb = 1.0;
  double delayMs = 1.0;
  double feedbackDb = 1.0;
  double decayRelative = 0.10;
  double decayAbsoluteS = 0.02;
  double thdRelative = 0.25;
  double thdAbsolutePct = 0.05;
  double rateRelative = 0.05;
  double rateAbsoluteHz = 0.05;
  double depthRelative = 0.10;
  double ampDepthAbsoluteDb = 0.1;
  double pitchDepthAbsoluteCents = 0.5;
  double staticCurveDb = 1.0;
  double stepRelative = 0.20;
  double stepAbsoluteMs = 2.0;
  double tailRelative = 0.10;
  double tailAbsoluteMs = 20.0;
  double noiseFloorDb = 3.0;
  double stereoCorrelation = 0.1;
  // The latency of the two paths is reported but only judged on request.
  bool judgeLatency = false;
  double latencyMs = 1.0;
};

struct MetricComparison {
  std::string group;
  std::string name;
  std::string unit;
  std::optional<double> reference;
  std::optional<double> candidate;
  // candidate - reference; zero when either is missing.
  double difference = 0.0;
  // The largest difference that still passes.
  double allowed = 0.0;
  // False for a metric that is shown but not judged.
  bool judged = true;
  bool pass = true;
  // Why a metric failed or was not judged, when the numbers do not say.
  std::string note;
};

struct MatchComparison {
  std::vector<MetricComparison> metrics;
  int judged = 0;
  int passed = 0;
  // 100 x passed / judged.
  double score = 100.0;
  bool allPass = true;
};

MatchComparison compareAnalyses(const MatchAnalysis& reference, const MatchAnalysis& candidate,
                                const Tolerances& tolerances = {});
juce::var toVar(const MatchComparison& comparison);
// A table a person can read: the score, the failures, then every metric by
// group.
juce::String toMarkdown(const MatchComparison& comparison);

}  // namespace EffectMatch

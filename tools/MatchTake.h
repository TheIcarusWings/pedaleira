#pragma once

#include "EffectMatch.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <stdexcept>
#include <string>
#include <vector>

// The parts of a recording session that are not audio I/O: the plan file, the
// channel lists of the command line, the check of a finished take and the WAV
// it is written to. Pure functions, so they are tested without a device.
namespace MatchTake {

struct PlanError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct PlanTake {
  std::string stem;
  std::string instruction;
};

// A file stem: letters, digits, '.', '-' and '_', not starting with '.', so it
// can never point outside the take folder.
bool isPlainStem(const std::string& stem);

// One take per line: `<file stem> | <instruction for the player>`. Blank lines
// and lines starting with # are skipped. Throws PlanError (with the line
// number) for a line without a separator, an empty instruction, a stem that is
// not a plain file name (letters, digits, '.', '-', '_') or a stem used twice.
std::vector<PlanTake> parsePlan(const juce::String& text);

// "3" or "3,4": 1-based channel numbers, at most `maxCount` of them, each
// once. Throws std::invalid_argument with a message for the command line.
std::vector<int> parseChannelList(const std::string& text, int maxCount, const std::string& flag);

// The mask with the (1-based) channels set.
juce::BigInteger channelMask(const std::vector<int>& channels);

// The result of checking a take.
struct Check {
  bool passed = false;
  // Highest absolute sample over all channels, dBFS.
  double peakDb = -200.0;
  double latencyMs = 0.0;
  // Level of the 1 kHz band of the quiet sweep against the signal played.
  double levelGainDb = 0.0;
  bool markersFound = false;
  bool clipped = false;
  // What is wrong, one sentence each, in the order of the checks.
  std::vector<std::string> problems;
};

// A take clips when it holds full scale: kClipRun samples in a row at or
// above kClipDb, or any sample past full scale. Not just one sample near it:
// the test signal's own staircase reaches 0 dBFS, so a clean unity path
// touches full scale for a sample at each peak, while a clipped one sits there.
constexpr double kClipDb = -0.01;
constexpr int kClipRun = 3;
// A take is too quiet when its peak stays under this level, and its level
// implausible when the 1 kHz level gain falls outside this range: the low bound
// against the signal played, the high bound against the nominal signal.
constexpr double kQuietPeakDb = -50.0;
constexpr double kMinLevelGainDb = -30.0;
constexpr double kMaxLevelGainDb = 20.0;

// Both sync markers found, no clipping, latency found, level plausible. Never
// throws: an unanalysable recording is reported as a problem. `reference` is the
// nominal test signal; when the take was played `signalGainDb` (0 or less) below
// it, the level gain is reported against what was played, not against nominal.
// The clip check always looks at the recording itself.
Check checkTake(const juce::AudioBuffer<float>& recording, double sampleRate,
                const EffectMatch::TestSignal& reference, double signalGainDb = 0.0);

// The test signal's samples scaled by `gainDb` (0 or less, finite), as played
// for a take with --gain-db. Throws std::invalid_argument for any other value.
std::vector<float> attenuatedSamples(const std::vector<float>& samples, double gainDb);

// Writes a 32-bit float WAV, 1 or 2 channels. Throws std::runtime_error.
void writeTakeWav(const juce::File& file, const juce::AudioBuffer<float>& recording,
                  double sampleRate);

}  // namespace MatchTake

#pragma once

#include <juce_core/juce_core.h>

#include <string>

// The recording half of PedaleiraMatch: list audio devices, record one take,
// run a session from a plan file (docs/matching.md, "Recording without a
// DAW"). The command line is parsed in match_tool.cpp; the audio itself is
// MatchRecorder, the plan, the check and the WAV are MatchTake.
namespace MatchRecord {

struct Options {
  std::string device;
  std::string type;
  std::string outChannels;
  std::string inChannels;
  std::string name;
  juce::File dir;
  juce::File plan;
  double rate = 48000.0;
  int bufferSize = 256;
  bool overwrite = false;
};

// Exit codes: 0 everything recorded and checked, 3 a take failed its check.
// Anything else that goes wrong is a std::exception (exit 1).
constexpr int kExitCheckFailed = 3;

int runDevices();
int runRecord(const Options& options);
int runSession(const Options& options);

}  // namespace MatchRecord

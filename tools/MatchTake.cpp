#include "MatchTake.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <sstream>

namespace MatchTake {

namespace {

std::string trimmed(const std::string& text) {
  return juce::String(text).trim().toStdString();
}

std::string dbText(double db) {
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out.precision(1);
  out << db;
  return out.str();
}

}  // namespace

bool isPlainStem(const std::string& stem) {
  if (stem.empty() || stem.front() == '.')
    return false;
  return std::all_of(stem.begin(), stem.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
           c == '-' || c == '_';
  });
}

std::vector<PlanTake> parsePlan(const juce::String& text) {
  std::vector<PlanTake> takes;
  std::set<std::string> seen;
  const juce::StringArray lines = juce::StringArray::fromLines(text);
  for (int i = 0; i < lines.size(); ++i) {
    const std::string line = trimmed(lines[i].toStdString());
    if (line.empty() || line.front() == '#')
      continue;
    const std::string where = "plan line " + std::to_string(i + 1) + ": ";
    const size_t bar = line.find('|');
    if (bar == std::string::npos)
      throw PlanError(where + "expected '<file stem> | <instruction>'");
    PlanTake take;
    take.stem = trimmed(line.substr(0, bar));
    take.instruction = trimmed(line.substr(bar + 1));
    if (!isPlainStem(take.stem))
      throw PlanError(where + "'" + take.stem +
                      "' is not a file stem (letters, digits, '.', '-' and '_' only)");
    if (take.instruction.empty())
      throw PlanError(where + "the instruction for '" + take.stem + "' is empty");
    if (!seen.insert(take.stem).second)
      throw PlanError(where + "'" + take.stem + "' is used twice");
    takes.push_back(std::move(take));
  }
  return takes;
}

std::vector<int> parseChannelList(const std::string& text, int maxCount, const std::string& flag) {
  std::vector<int> channels;
  const juce::StringArray parts = juce::StringArray::fromTokens(juce::String(text), ",", "");
  for (const auto& part : parts) {
    const juce::String token = part.trim();
    if (token.isEmpty() || !token.containsOnly("0123456789") || token.length() > 4 ||
        token.getIntValue() < 1)
      throw std::invalid_argument("bad value for " + flag + ": '" + text +
                                  "' (channel numbers start at 1, e.g. 1 or 1,2)");
    const int channel = token.getIntValue();
    if (std::find(channels.begin(), channels.end(), channel) != channels.end())
      throw std::invalid_argument(flag + " lists channel " + std::to_string(channel) + " twice");
    channels.push_back(channel);
  }
  if (channels.empty() || static_cast<int>(channels.size()) > maxCount)
    throw std::invalid_argument(flag + " takes 1 to " + std::to_string(maxCount) +
                                " channels, got '" + text + "'");
  return channels;
}

juce::BigInteger channelMask(const std::vector<int>& channels) {
  juce::BigInteger mask;
  for (const int channel : channels)
    mask.setBit(channel - 1);
  return mask;
}

Check checkTake(const juce::AudioBuffer<float>& recording, double sampleRate,
                const EffectMatch::TestSignal& reference) {
  Check check;

  float peak = 0.0f;
  for (int c = 0; c < recording.getNumChannels(); ++c)
    peak = std::max(peak, recording.getMagnitude(c, 0, recording.getNumSamples()));
  check.peakDb = peak > 0.0f ? 20.0 * std::log10(static_cast<double>(peak)) : -200.0;
  check.clipped = check.peakDb >= kClipDb;

  try {
    const EffectMatch::MatchAnalysis analysis =
        EffectMatch::analyseRecording(recording, sampleRate, reference);
    check.markersFound = true;
    check.latencyMs = analysis.alignment.latencyMs;
    if (analysis.left.sweepLow)
      check.levelGainDb = analysis.left.sweepLow->levelGainDb;
    else
      check.problems.push_back("the quiet sweep could not be measured, so the level is unknown");
  } catch (const EffectMatch::MatchError& e) {
    check.problems.push_back(std::string("sync markers not found: ") + e.what());
  }

  if (check.clipped)
    check.problems.push_back("the take clips (peak " + dbText(check.peakDb) +
                             " dBFS, the limit is " + dbText(kClipDb) +
                             "): lower the unit's output level or the interface input gain");
  if (check.peakDb < kQuietPeakDb)
    check.problems.push_back("the take is silent or far too quiet (peak " + dbText(check.peakDb) +
                             " dBFS): check the cables, the channels and the input gain");
  else if (check.markersFound && (check.levelGainDb < kMinLevelGainDb || check.levelGainDb > kMaxLevelGainDb))
    check.problems.push_back("the level is implausible (" + dbText(check.levelGainDb) +
                             " dB at 1 kHz against the signal played): check the gain staging");

  check.passed = check.problems.empty();
  return check;
}

void writeTakeWav(const juce::File& file, const juce::AudioBuffer<float>& recording,
                  double sampleRate) {
  const int channels = recording.getNumChannels();
  if (channels < 1 || channels > 2 || recording.getNumSamples() < 1)
    throw std::runtime_error("nothing to write");
  if (!file.getParentDirectory().createDirectory())
    throw std::runtime_error("cannot create " + file.getParentDirectory().getFullPathName().toStdString());
  // FileOutputStream appends to an existing file.
  file.deleteFile();
  std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
  if (stream == nullptr)
    throw std::runtime_error("cannot write " + file.getFullPathName().toStdString());
  juce::WavAudioFormat wav;
  auto writer = wav.createWriterFor(
      stream, juce::AudioFormatWriterOptions{}
                  .withSampleRate(sampleRate)
                  .withNumChannels(channels)
                  .withBitsPerSample(32)
                  .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
  if (writer == nullptr)
    throw std::runtime_error("cannot write a WAV at " + std::to_string(sampleRate) + " Hz");
  if (!writer->writeFromAudioSampleBuffer(recording, 0, recording.getNumSamples()))
    throw std::runtime_error("failed writing " + file.getFullPathName().toStdString());
}

}  // namespace MatchTake

#include "MatchTake.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>

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

std::vector<float> attenuatedSamples(const std::vector<float>& samples, double gainDb) {
  if (!std::isfinite(gainDb) || gainDb > 0.0)
    throw std::invalid_argument("the signal gain must be 0 dB or less (got " + dbText(gainDb) + ")");
  std::vector<float> out = samples;
  if (gainDb == 0.0)
    return out;
  const double gain = std::pow(10.0, gainDb / 20.0);
  for (float& sample : out)
    sample = static_cast<float>(static_cast<double>(sample) * gain);
  return out;
}

Check checkTake(const juce::AudioBuffer<float>& recording, double sampleRate,
                const EffectMatch::TestSignal& reference, double signalGainDb) {
  Check check;

  float peak = 0.0f;
  for (int c = 0; c < recording.getNumChannels(); ++c)
    peak = std::max(peak, recording.getMagnitude(c, 0, recording.getNumSamples()));
  check.peakDb = peak > 0.0f ? 20.0 * std::log10(static_cast<double>(peak)) : -200.0;
  const float clipLevel = static_cast<float>(std::pow(10.0, kClipDb / 20.0));
  for (int c = 0; c < recording.getNumChannels() && !check.clipped; ++c) {
    const float* samples = recording.getReadPointer(c);
    int run = 0;
    for (int n = 0; n < recording.getNumSamples(); ++n) {
      const float magnitude = std::abs(samples[n]);
      run = magnitude >= clipLevel ? run + 1 : 0;
      if (magnitude > 1.0f || run >= kClipRun) {
        check.clipped = true;
        break;
      }
    }
  }

  try {
    const EffectMatch::MatchAnalysis analysis =
        EffectMatch::analyseRecording(recording, sampleRate, reference);
    check.markersFound = true;
    check.latencyMs = analysis.alignment.latencyMs;
    if (analysis.left.sweepLow)
      check.levelGainDb = analysis.left.sweepLow->levelGainDb - signalGainDb;
    else
      check.problems.push_back("the quiet sweep could not be measured, so the level is unknown");
  } catch (const EffectMatch::MatchError& e) {
    check.problems.push_back(std::string("sync markers not found: ") + e.what());
  }

  if (check.clipped)
    check.problems.push_back("the take clips (it holds full scale, peak " + dbText(check.peakDb) +
                             " dBFS): lower the unit's output level or the interface input gain");
  if (check.peakDb < kQuietPeakDb)
    check.problems.push_back("the take is silent or far too quiet (peak " + dbText(check.peakDb) +
                             " dBFS): check the cables, the channels and the input gain");
  // The low bound is against what was played; the high bound against the nominal signal, so a
  // high-gain amp fed a quiet signal (signalGainDb well below 0) may make up the attenuation.
  else if (check.markersFound && (check.levelGainDb < kMinLevelGainDb ||
                                  check.levelGainDb + signalGainDb > kMaxLevelGainDb))
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

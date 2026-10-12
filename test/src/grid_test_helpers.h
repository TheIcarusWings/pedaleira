#pragma once

// Shared pieces of the grid test suites: a processor on a wide main bus with
// its command registry, commands written as JSON text, and multi-channel
// audio through processBlock.

#include "EngineCommands.h"
#include "Processor.h"
#include "chain_test_helpers.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using GridChannels = std::vector<std::vector<float>>;

struct GridRig {
  ChainTestProcessor proc;
  EngineCommands commands{proc};
  int numIns;
  int numOuts;
  double rate;
  int block;

  explicit GridRig(int ins = 2, int outs = 2, double sampleRate = 48000.0, int blockSize = 64,
                   int oversample = 1)
      : numIns(ins), numOuts(outs), rate(sampleRate), block(blockSize) {
    proc.setMultiCoreEnabled(false, /*persist=*/false);
    proc.setPlayConfigDetails(ins, outs, sampleRate, blockSize);
    if (oversample > 1) {
      proc.parameters.getParameter("osEnabled")->setValueNotifyingHost(1.0f);
      proc.parameters.getParameter("osFactor")
          ->setValueNotifyingHost((oversample == 2 ? 0 : oversample == 4 ? 1 : 2) / 2.0f);
    }
    // The analytic suites feed impulses and quiet tones; the gate would
    // have opinions about those.
    proc.parameters.getParameter("gateEnabled")->setValueNotifyingHost(0.0f);
    proc.prepareToPlay(sampleRate, blockSize);
  }

  static juce::var parse(const char* json) {
    juce::var args;
    if (json != nullptr && *json != 0) {
      const juce::Result parsed = juce::JSON::parse(json, args);
      EXPECT_TRUE(parsed.wasOk()) << json << ": " << parsed.getErrorMessage();
    }
    return args;
  }

  CommandResult tryRun(const char* name, const char* argsJson = "") {
    return commands.registry().run(name, parse(argsJson));
  }

  // A command that has to succeed.
  juce::var run(const char* name, const char* argsJson = "") {
    const CommandResult result = tryRun(name, argsJson);
    EXPECT_TRUE(result.ok) << name << " " << argsJson << ": " << result.errorCode << ": "
                           << result.errorMessage;
    return result.value;
  }

  juce::var run(const char* name, const juce::String& argsJson) {
    return run(name, argsJson.toRawUTF8());
  }

  // Add a registry kind at (row, slot) and set its parameters: `params` is
  // "id=value,id=value" in real units. Returns the block id.
  juce::String addKind(const char* kind, int row, int slot, const char* params = "") {
    const juce::String id =
        run("chain.addBlock", juce::String("{\"kind\":\"") + kind + "\",\"row\":" +
                                  juce::String(row) + ",\"slot\":" + juce::String(slot) + "}")["blockId"]
            .toString();
    EXPECT_TRUE(id.isNotEmpty());
    for (const auto& pair : juce::StringArray::fromTokens(params, ",", "")) {
      const juce::String name = pair.upToFirstOccurrenceOf("=", false, false);
      const double value = pair.fromFirstOccurrenceOf("=", false, false).getDoubleValue();
      EXPECT_TRUE(proc.setBlockParam(id.toStdString(), name, value)) << name;
    }
    return id;
  }

  // Everything loaded, the deferred refresh done, the device re-prepared:
  // what a host does before it starts to play.
  void settle() {
    EXPECT_TRUE(static_cast<bool>(run("state.waitUntilLoaded")["loaded"]));
    proc.prepareToPlay(rate, block);
    EXPECT_TRUE(waitForChainLoaded(proc));
    EXPECT_EQ(proc.validateActivePlanForTesting(), "");
  }

  // `in` holds numIns channels of equal length (a multiple of the block
  // size is not required; the tail is zero-padded). Returns numOuts
  // channels of the same length.
  GridChannels process(const GridChannels& in) {
    const int frames = static_cast<int>(in.empty() ? 0 : in[0].size());
    const int bufferChannels = std::max(numIns, numOuts);
    GridChannels out(static_cast<size_t>(numOuts), std::vector<float>(static_cast<size_t>(frames)));
    juce::AudioBuffer<float> buffer(bufferChannels, block);
    juce::MidiBuffer midi;
    for (int start = 0; start < frames; start += block) {
      const int count = std::min(block, frames - start);
      buffer.clear();
      for (int ch = 0; ch < numIns && ch < static_cast<int>(in.size()); ++ch)
        buffer.copyFrom(ch, 0, in[static_cast<size_t>(ch)].data() + start, count);
      proc.processBlock(buffer, midi);
      for (int ch = 0; ch < numOuts; ++ch)
        std::copy(buffer.getReadPointer(ch), buffer.getReadPointer(ch) + count,
                  out[static_cast<size_t>(ch)].begin() + start);
    }
    return out;
  }

  juce::var row(int index) { return proc.getGridState()["rows"][index]; }
};

inline std::vector<float> gridSilence(int frames) {
  return std::vector<float>(static_cast<size_t>(frames), 0.0f);
}

inline std::vector<float> gridImpulse(int frames, int at = 0, float level = 0.5f) {
  std::vector<float> x(static_cast<size_t>(frames), 0.0f);
  x[static_cast<size_t>(at)] = level;
  return x;
}

inline float gridPeak(const std::vector<float>& x) {
  float peak = 0.0f;
  for (const float v : x)
    peak = std::max(peak, std::abs(v));
  return peak;
}

inline int gridPeakIndex(const std::vector<float>& x) {
  int index = 0;
  for (size_t i = 0; i < x.size(); ++i)
    if (std::abs(x[i]) > std::abs(x[static_cast<size_t>(index)]))
      index = static_cast<int>(i);
  return index;
}

inline float gridMaxDiff(const std::vector<float>& a, const std::vector<float>& b) {
  EXPECT_EQ(a.size(), b.size());
  float diff = 0.0f;
  for (size_t i = 0; i < std::min(a.size(), b.size()); ++i)
    diff = std::max(diff, std::abs(a[i] - b[i]));
  return diff;
}

// The gain that takes `reference` to `scaled`, by least squares, and how far
// `scaled` is from exactly that multiple (relative to the reference's peak).
struct GridGainFit {
  double gain = 0.0;
  double residual = 0.0;
};

inline GridGainFit gridFitGain(const std::vector<float>& scaled, const std::vector<float>& reference,
                               size_t skip = 0) {
  double num = 0.0, den = 0.0;
  for (size_t i = skip; i < reference.size(); ++i) {
    num += static_cast<double>(scaled[i]) * reference[i];
    den += static_cast<double>(reference[i]) * reference[i];
  }
  GridGainFit fit;
  fit.gain = den > 0.0 ? num / den : 0.0;
  double peak = 0.0, worst = 0.0;
  for (size_t i = skip; i < reference.size(); ++i) {
    peak = std::max(peak, std::abs(static_cast<double>(reference[i])));
    worst = std::max(worst, std::abs(scaled[i] - fit.gain * reference[i]));
  }
  fit.residual = peak > 0.0 ? worst / peak : worst;
  return fit;
}

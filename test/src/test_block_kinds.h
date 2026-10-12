#pragma once
// Block kinds that exist only in the test binary, to exercise the parts of
// the block framework no shipped kind uses yet: latency (reported, dry-path
// and lane compensation), a parameter-dependent tail, the tempo context and
// the failed-block path.

#include "ParameterisedBlockProcessor.h"

#include <atomic>
#include <vector>

// test.latency: delays its input by `latencySamples` frames and reports that
// as its latency; reports `tailMs` as its tail; throws from the audio path
// while `fail` is on. Runs at the chain rate, so at 48 kHz without
// oversampling a frame is a host sample.
// test.latency.base: the same processor run at the base rate, for the
// oversampled-chain accounting.
class TestLatencyProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "test.latency";
  static constexpr const char* kBaseRateKindId = "test.latency.base";
  static constexpr int kDefaultLatency = 64;
  static constexpr int kMaxLatency = 512;

  enum Param { kLatencySamples = 0, kTailMs, kFail };
  static const BlockParamTable& parameterSpecs();

  explicit TestLatencyProcessor(bool atBaseRate);

  bool runsAtBaseRate() const noexcept override { return baseRate; }
  // Latched (see BlockProcessor::latencySamples): the delay in use, which
  // only moves to the parameter's value at a prepare or a commit.
  int latencySamples() const noexcept override { return activeLatency; }
  bool latencyChangePending() const noexcept override {
    return static_cast<int>(param(kLatencySamples)) != activeLatency;
  }
  void commitLatency() override;
  double tailSeconds() const noexcept override { return param(kTailMs) / 1000.0; }

  // What the last processed block's context carried, for the tempo tests.
  struct Seen {
    std::atomic<double> bpm{0.0};
    std::atomic<int> timeSigNumerator{0};
    std::atomic<int> timeSigDenominator{0};
    std::atomic<bool> playing{false};
    std::atomic<bool> fromHost{false};
    std::atomic<juce::int64> samplePosition{-1};
    std::atomic<double> sampleRate{0.0};
  };
  static Seen& seen();

protected:
  void prepareEffect(const EffectSpec& spec) override;
  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override;

private:
  const bool baseRate;
  // One ring of the maximum length per channel, sized at prepare; the delay
  // is how far behind the write position the read sits.
  std::vector<float> ring[2];
  int writePos{0};
  int activeLatency{kDefaultLatency};
  double preparedRate{0.0};
};

// test.spill: a gain (`wetGain`, unity by default, so a pass-through) that
// declares tail spillover and reports `tailMs` as its tail, and counts what
// the chain does to it: how often it is processed, how often reset, and how
// loud the input it was last handed was. It makes the spillover state machine
// and the mix arithmetic observable without any DSP in the way.
// test.spill.dry: the same processor registered with the dry-preserving mix
// law (BlockMixLaw), for the two laws side by side.
class TestSpillProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "test.spill";
  static constexpr const char* kDryPreservingKindId = "test.spill.dry";

  // `rightGain` is this kind's bit of stereo behaviour, applied the way
  // every effect's must be: to channel 1, and only when the buffer is a true
  // stereo signal (blockHasStereoImage).
  enum Param { kTailMs = 0, kWetGain, kRightGain, kSpillover };
  static const BlockParamTable& parameterSpecs();

  explicit TestSpillProcessor(const char* kindId = kKindId);

  // Exact at any rate, like a gain: no base-rate island around it.
  bool runsAtBaseRate() const noexcept override { return false; }
  double tailSeconds() const noexcept override {
    return static_cast<double>(param(kTailMs)) / 1000.0;
  }

  struct Seen {
    std::atomic<int> processCalls{0};
    std::atomic<int> resetCalls{0};
    std::atomic<float> lastInputPeak{0.0f};
    // The buffer the kind was last handed: its channel count and whether
    // the context called it dual mono.
    std::atomic<int> lastChannels{0};
    std::atomic<bool> lastDualMono{false};
    void clear() {
      processCalls.store(0);
      resetCalls.store(0);
      lastInputPeak.store(0.0f);
    }
  };
  static Seen& seen();

protected:
  void prepareEffect(const EffectSpec&) override {}
  void resetEffect() noexcept override { seen().resetCalls.fetch_add(1); }
  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override;
};

// Registers the kinds above. Called once from main().
void registerTestBlockKinds();

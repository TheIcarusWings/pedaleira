#include "test_block_kinds.h"

#include "BlockKinds.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

const BlockParamTable& TestLatencyProcessor::parameterSpecs() {
  static const BlockParamTable table = {
      BlockParamSpec::floatParam("latencySamples", "Latency", "samples", 0.0f,
                                 static_cast<float>(kMaxLatency),
                                 static_cast<float>(kDefaultLatency))
          .withStep(1.0f)
          .withAffectsLatency()
          .notAutomatable(),
      BlockParamSpec::floatParam("tailMs", "Tail", "ms", 0.0f, 20000.0f, 0.0f).withAffectsTail(),
      BlockParamSpec::boolParam("fail", "Fail", false).notAutomatable(),
  };
  return table;
}

TestLatencyProcessor::TestLatencyProcessor(bool atBaseRate)
    : ParameterisedBlockProcessor(atBaseRate ? kBaseRateKindId : kKindId, parameterSpecs()),
      baseRate(atBaseRate) {}

TestLatencyProcessor::Seen& TestLatencyProcessor::seen() {
  static Seen instance;
  return instance;
}

void TestLatencyProcessor::prepareEffect(const EffectSpec& spec) {
  for (auto& channel : ring)
    channel.assign(static_cast<size_t>(kMaxLatency) + 1, 0.0f);
  writePos = 0;
  activeLatency = static_cast<int>(param(kLatencySamples));
  preparedRate = spec.sampleRate;
}

void TestLatencyProcessor::commitLatency() {
  const int wanted = static_cast<int>(param(kLatencySamples));
  if (wanted == activeLatency)
    return;
  // Under the chain lock with the output muted: start the new delay from
  // silence, like the chain's own delay lines do.
  activeLatency = wanted;
  for (auto& channel : ring)
    std::fill(channel.begin(), channel.end(), 0.0f);
}

void TestLatencyProcessor::processEffect(juce::AudioBuffer<float>& buffer,
                                         const BlockProcessContext& ctx) {
  if (param(kFail) >= 0.5f)
    throw std::runtime_error("test.latency asked to fail");

  Seen& s = seen();
  s.bpm.store(ctx.transport.bpm);
  s.timeSigNumerator.store(ctx.transport.timeSigNumerator);
  s.timeSigDenominator.store(ctx.transport.timeSigDenominator);
  s.playing.store(ctx.transport.playing);
  s.fromHost.store(ctx.transport.fromHost);
  s.samplePosition.store(ctx.transport.samplePosition);
  s.sampleRate.store(preparedRate);

  const int delay = activeLatency;
  const int size = kMaxLatency + 1;
  const int numSamples = buffer.getNumSamples();
  const int numChannels = juce::jmin(2, buffer.getNumChannels());
  for (int ch = 0; ch < numChannels; ++ch) {
    float* x = buffer.getWritePointer(ch);
    float* line = ring[ch].data();
    int pos = writePos;
    for (int i = 0; i < numSamples; ++i) {
      line[pos] = x[i];
      int readPos = pos - delay;
      if (readPos < 0)
        readPos += size;
      x[i] = line[readPos];
      if (++pos == size)
        pos = 0;
    }
  }
  writePos = (writePos + numSamples) % size;
}

const BlockParamTable& TestSpillProcessor::parameterSpecs() {
  static const BlockParamTable table = {
      BlockParamSpec::floatParam("tailMs", "Tail", "ms", 0.0f, 600000.0f, 500.0f)
          .withAffectsTail(),
      BlockParamSpec::floatParam("wetGain", "Wet Gain", "x", -4.0f, 4.0f, 1.0f),
      BlockParamSpec::floatParam("rightGain", "Right Gain", "x", -4.0f, 4.0f, 1.0f),
      spilloverParam(),
  };
  return table;
}

TestSpillProcessor::TestSpillProcessor(const char* kindId)
    : ParameterisedBlockProcessor(kindId, parameterSpecs()) {}

TestSpillProcessor::Seen& TestSpillProcessor::seen() {
  static Seen instance;
  return instance;
}

void TestSpillProcessor::processEffect(juce::AudioBuffer<float>& buffer,
                                       const BlockProcessContext& ctx) {
  Seen& s = seen();
  s.processCalls.fetch_add(1);
  s.lastInputPeak.store(buffer.getMagnitude(0, buffer.getNumSamples()));
  // Exactly the input at unity (nothing is multiplied), so the pass-through
  // tests stay exact; a plain, unsmoothed gain otherwise.
  const float gain = param(kWetGain);
  if (gain != 1.0f)
    buffer.applyGain(gain);
  s.lastChannels.store(buffer.getNumChannels());
  s.lastDualMono.store(ctx.dualMono);
  const float rightGain = param(kRightGain);
  if (blockHasStereoImage(buffer.getNumChannels(), ctx) && rightGain != 1.0f)
    buffer.applyGain(1, 0, buffer.getNumSamples(), rightGain);
}

void registerTestBlockKinds() {
  BlockKindRegistry::instance().add(
      {TestLatencyProcessor::kKindId, "Test Latency", BlockCategory::Utility,
       TestLatencyProcessor::parameterSpecs(),
       [] { return std::make_unique<TestLatencyProcessor>(false); }, true});
  BlockKindRegistry::instance().add(
      {TestLatencyProcessor::kBaseRateKindId, "Test Latency (base rate)", BlockCategory::Utility,
       TestLatencyProcessor::parameterSpecs(),
       [] { return std::make_unique<TestLatencyProcessor>(true); }, true});
  BlockKindRegistry::instance().add(
      {TestSpillProcessor::kKindId, "Test Spillover", BlockCategory::Utility,
       TestSpillProcessor::parameterSpecs(),
       [] { return std::make_unique<TestSpillProcessor>(); }, true});
  BlockKindInfo dryPreserving{
      TestSpillProcessor::kDryPreservingKindId, "Test Spillover (dry preserving)",
      BlockCategory::Utility, TestSpillProcessor::parameterSpecs(),
      [] {
        return std::make_unique<TestSpillProcessor>(TestSpillProcessor::kDryPreservingKindId);
      },
      true};
  dryPreserving.mixLaw = BlockMixLaw::DryPreserving;
  BlockKindRegistry::instance().add(std::move(dryPreserving));
}

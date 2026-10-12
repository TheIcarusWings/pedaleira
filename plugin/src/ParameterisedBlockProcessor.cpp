#include "ParameterisedBlockProcessor.h"

#include <cmath>

namespace {
const BlockReadingTable& noReadings() {
  static const BlockReadingTable none;
  return none;
}
}  // namespace

ParameterisedBlockProcessor::ParameterisedBlockProcessor(const char* kindIdIn,
                                                         const BlockParamTable& tableIn)
    : ParameterisedBlockProcessor(kindIdIn, tableIn, noReadings()) {}

ParameterisedBlockProcessor::ParameterisedBlockProcessor(const char* kindIdIn,
                                                         const BlockParamTable& tableIn,
                                                         const BlockReadingTable& readingsIn)
    : id(kindIdIn), table(tableIn), spilloverIndex(findBlockParam(tableIn, kSpilloverParamId)),
      values(std::make_unique<std::atomic<float>[]>(juce::jmax<size_t>(1, tableIn.size()))),
      readings(readingsIn),
      readingValues(
          std::make_unique<std::atomic<float>[]>(juce::jmax<size_t>(1, readingsIn.size()))) {
  for (size_t i = 0; i < table.size(); ++i)
    values[i].store(table[i].defaultValue);
  for (size_t i = 0; i < readings.size(); ++i)
    readingValues[i].store(readings[i].min);
}

void ParameterisedBlockProcessor::reset() noexcept {
  resetEffect();
  for (size_t i = 0; i < readings.size(); ++i)
    readingValues[i].store(readings[i].min, std::memory_order_relaxed);
}

float ParameterisedBlockProcessor::getReading(int index) const noexcept {
  if (index < 0 || index >= static_cast<int>(readings.size()))
    return 0.0f;
  return readingValues[static_cast<size_t>(index)].load(std::memory_order_relaxed);
}

void ParameterisedBlockProcessor::setReading(int index, float value) noexcept {
  if (index < 0 || index >= static_cast<int>(readings.size()))
    return;
  const BlockReadingSpec& spec = readings[static_cast<size_t>(index)];
  // A non-finite value would poison every meter that reads it.
  const float kept = std::isfinite(value) ? juce::jlimit(spec.min, spec.max, value) : spec.min;
  readingValues[static_cast<size_t>(index)].store(kept, std::memory_order_relaxed);
}

float ParameterisedBlockProcessor::getParameter(int index) const noexcept {
  if (index < 0 || index >= static_cast<int>(table.size()))
    return 0.0f;
  return values[static_cast<size_t>(index)].load(std::memory_order_relaxed);
}

void ParameterisedBlockProcessor::setParameter(int index, float value) noexcept {
  if (index < 0 || index >= static_cast<int>(table.size()))
    return;
  values[static_cast<size_t>(index)].store(table[static_cast<size_t>(index)].clamp(value),
                                           std::memory_order_relaxed);
}

ParameterisedBlockProcessor::EffectSpec
ParameterisedBlockProcessor::effectSpecFor(const BlockPrepareSpec& spec) const noexcept {
  const bool atBaseRate = runsAtBaseRate();
  return {spec.runSampleRate(atBaseRate), spec.runBlockSize(atBaseRate), spec.numChannels};
}

void ParameterisedBlockProcessor::prepare(const BlockPrepareSpec& spec) {
  preparedSpec = effectSpecFor(spec);
  prepareEffect(preparedSpec);
}

void ParameterisedBlockProcessor::prepareForInstall(const BlockPrepareSpec& spec) {
  const EffectSpec wanted = effectSpecFor(spec);
  if (preparedSpec.maxBlockSize > 0 && preparedSpec.sampleRate == wanted.sampleRate &&
      preparedSpec.maxBlockSize == wanted.maxBlockSize &&
      preparedSpec.numChannels == wanted.numChannels)
    return;
  prepare(spec);
}

BlockProcessResult ParameterisedBlockProcessor::process(juce::AudioBuffer<float>& buffer,
                                                        const BlockProcessContext& ctx) {
  try {
    processEffect(buffer, ctx);
  } catch (...) {
    // Nothing can be said from here (no logging on this thread); the caller
    // restores the dry signal, drops the block and flags it.
    return BlockProcessResult::Failed;
  }
  return BlockProcessResult::Processed;
}

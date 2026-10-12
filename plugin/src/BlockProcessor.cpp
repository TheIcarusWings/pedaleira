#include "BlockProcessor.h"
#include "ChainOversampler.h"
#include "IrBlockProcessor.h"
#include "NamBlockProcessor.h"
#include <cmath>

// ####################
// PARAMETER SPECS
// ####################
BlockParamSpec BlockParamSpec::floatParam(const juce::String& id, const juce::String& name,
                                          const juce::String& unit, float min, float max,
                                          float defaultValue) {
  BlockParamSpec spec;
  spec.id = id;
  spec.name = name;
  spec.unit = unit;
  spec.type = BlockParamType::Float;
  spec.min = min;
  spec.max = max;
  spec.defaultValue = defaultValue;
  return spec;
}

BlockParamSpec BlockParamSpec::boolParam(const juce::String& id, const juce::String& name,
                                         bool defaultValue) {
  BlockParamSpec spec;
  spec.id = id;
  spec.name = name;
  spec.type = BlockParamType::Bool;
  spec.min = 0.0f;
  spec.max = 1.0f;
  spec.step = 1.0f;
  spec.defaultValue = defaultValue ? 1.0f : 0.0f;
  return spec;
}

BlockParamSpec BlockParamSpec::choiceParam(const juce::String& id, const juce::String& name,
                                           const juce::StringArray& choices, int defaultIndex) {
  BlockParamSpec spec;
  spec.id = id;
  spec.name = name;
  spec.type = BlockParamType::Choice;
  spec.choices = choices;
  spec.min = 0.0f;
  spec.max = static_cast<float>(juce::jmax(0, choices.size() - 1));
  spec.step = 1.0f;
  spec.defaultValue = static_cast<float>(juce::jlimit(0, juce::jmax(0, choices.size() - 1),
                                                      defaultIndex));
  return spec;
}

BlockParamSpec BlockParamSpec::withSkew(float newSkew) const {
  BlockParamSpec spec = *this;
  spec.skew = newSkew;
  return spec;
}

BlockParamSpec BlockParamSpec::withStep(float newStep) const {
  BlockParamSpec spec = *this;
  spec.step = newStep;
  return spec;
}

BlockParamSpec BlockParamSpec::withAffectsTail() const {
  BlockParamSpec spec = *this;
  spec.affectsTail = true;
  return spec;
}

BlockParamSpec BlockParamSpec::withAffectsLatency() const {
  BlockParamSpec spec = *this;
  spec.affectsLatency = true;
  return spec;
}

BlockParamSpec BlockParamSpec::withTempoSync() const {
  BlockParamSpec spec = *this;
  spec.tempoSync = true;
  return spec;
}

BlockParamSpec BlockParamSpec::notAutomatable() const {
  BlockParamSpec spec = *this;
  spec.automatable = false;
  return spec;
}

float BlockParamSpec::clamp(float value) const noexcept {
  if (!std::isfinite(value))
    return defaultValue;
  if (type == BlockParamType::Bool)
    return value >= 0.5f ? 1.0f : 0.0f;
  if (step > 0.0f)
    value = min + std::round((value - min) / step) * step;
  return juce::jlimit(min, max, value);
}

juce::var BlockParamSpec::toVar() const {
  juce::DynamicObject::Ptr obj = new juce::DynamicObject();
  obj->setProperty("id", id);
  obj->setProperty("name", name);
  obj->setProperty("type", type == BlockParamType::Bool     ? "boolean"
                           : type == BlockParamType::Choice ? "choice"
                                                            : "float");
  if (unit.isNotEmpty())
    obj->setProperty("unit", unit);
  obj->setProperty("min", static_cast<double>(min));
  obj->setProperty("max", static_cast<double>(max));
  obj->setProperty("default", static_cast<double>(defaultValue));
  if (skew != 1.0f)
    obj->setProperty("skew", static_cast<double>(skew));
  if (step > 0.0f)
    obj->setProperty("step", static_cast<double>(step));
  if (type == BlockParamType::Choice) {
    juce::Array<juce::var> labels;
    for (const auto& label : choices)
      labels.add(label);
    obj->setProperty("choices", labels);
  }
  obj->setProperty("automatable", automatable);
  obj->setProperty("affectsTail", affectsTail);
  obj->setProperty("affectsLatency", affectsLatency);
  obj->setProperty("tempoSync", tempoSync);
  return juce::var(obj.get());
}

juce::var BlockReadingSpec::toVar() const {
  juce::DynamicObject::Ptr obj = new juce::DynamicObject();
  obj->setProperty("id", id);
  obj->setProperty("name", name);
  if (unit.isNotEmpty())
    obj->setProperty("unit", unit);
  obj->setProperty("min", static_cast<double>(min));
  obj->setProperty("max", static_cast<double>(max));
  return juce::var(obj.get());
}

BlockParamSpec spilloverParam() {
  return BlockParamSpec::boolParam(kSpilloverParamId, "Spillover", true).notAutomatable();
}

int findBlockParam(const BlockParamTable& table, const juce::String& id) {
  for (size_t i = 0; i < table.size(); ++i)
    if (table[i].id == id)
      return static_cast<int>(i);
  return -1;
}

// ####################
// BLOCK PROCESSOR
// ####################

// Out of line: the island is an incomplete type in the header.
BlockProcessor::BlockProcessor() = default;
BlockProcessor::~BlockProcessor() = default;

const BlockParamTable& BlockProcessor::parameterTable() const noexcept {
  static const BlockParamTable none;
  return none;
}

const BlockReadingTable& BlockProcessor::readingTable() const noexcept {
  static const BlockReadingTable none;
  return none;
}

juce::ValueTree BlockProcessor::getKindState(const juce::String& displayName) const {
  juce::ValueTree state("KindState");
  state.setProperty("kind", juce::String(kindId()), nullptr);
  state.setProperty("version", kindStateVersion(), nullptr);
  state.setProperty("name", displayName, nullptr);

  juce::MemoryBlock chunk;
  getStateChunk(chunk);
  if (!chunk.isEmpty())
    state.setProperty("chunk", juce::var(chunk), nullptr);

  if (savesParameterValues()) {
    const BlockParamTable& table = parameterTable();
    for (size_t i = 0; i < table.size(); ++i) {
      juce::ValueTree param("Param");
      param.setProperty("id", table[i].id, nullptr);
      param.setProperty("value", static_cast<double>(getParameter(static_cast<int>(i))),
                        nullptr);
      state.appendChild(param, nullptr);
    }
  }
  appendKindState(state);
  return state;
}

void BlockProcessor::setKindState(const juce::ValueTree& kindState) {
  const BlockParamTable& table = parameterTable();
  for (size_t i = 0; i < table.size(); ++i)
    setParameter(static_cast<int>(i), table[i].defaultValue);

  for (int i = 0; i < kindState.getNumChildren(); ++i) {
    const juce::ValueTree param = kindState.getChild(i);
    if (!param.hasType("Param"))
      continue;
    const int index = findParameter(param.getProperty("id").toString());
    if (index >= 0)
      setParameter(index, static_cast<float>(static_cast<double>(param.getProperty("value"))));
  }
  if (const auto* chunk = kindState.getProperty("chunk").getBinaryData())
    setStateChunk(*chunk, static_cast<int>(kindState.getProperty("version", 1)));
}

void BlockProcessor::prepareIsland(const BlockPrepareSpec& spec) {
  preparedOversampleFactor = juce::jmax(1, spec.oversampleFactor);
  if (!runsAtBaseRate())
    return;
  // In step with the live factor (bypass at ×1). A processor built
  // mid-session was never seen by prepareChain, so the install path comes
  // through here too, with the same capacity/factor arguments as the
  // chain-wide oversampler. Cheap (three small work buffers).
  if (baseRateIsland == nullptr)
    baseRateIsland = std::make_unique<ChainOversampler>();
  baseRateIsland->prepare(spec.oversampleFactor, juce::jmax(1, spec.baseBlockSize));
}

void BlockProcessor::prepareForChain(const BlockPrepareSpec& spec) {
  prepare(spec);
  prepareIsland(spec);
}

void BlockProcessor::prepareForChainInstall(const BlockPrepareSpec& spec) {
  prepareForInstall(spec);
  prepareIsland(spec);
}

BlockProcessResult BlockProcessor::processInChain(juce::AudioBuffer<float>& buffer,
                                                  const BlockProcessContext& ctx) {
  if (baseRateIsland == nullptr || !hasEngine())
    return process(buffer, ctx);

  // When the chain is oversampled the island decimates the wet path, hands
  // the kind base-rate frames, and interpolates back (a direct pass at ×1).
  // The buffer over the island's frames refers to them; nothing is
  // allocated.
  const int numChannels = buffer.getNumChannels();
  BlockProcessResult result = BlockProcessResult::Processed;
  baseRateIsland->processBaseRateIsland(
      buffer.getArrayOfWritePointers(), numChannels, buffer.getNumSamples(),
      [this, &ctx, &result, numChannels](float* const* baseChannels, int baseFrames) {
        juce::AudioBuffer<float> baseBuffer(baseChannels, numChannels, baseFrames);
        result = process(baseBuffer, ctx);
      });
  if (result == BlockProcessResult::Processed)
    postProcessAtChainRate(buffer, ctx);
  return result;
}

std::unique_ptr<BlockProcessor> makeBlockProcessor(ChainBlockType kind) {
  switch (kind) {
    case ChainBlockType::NAM: return std::make_unique<NamBlockProcessor>();
    case ChainBlockType::IR: return std::make_unique<IrBlockProcessor>();
    case ChainBlockType::INSERT:
    case ChainBlockType::KIND: break;
  }
  return nullptr;
}

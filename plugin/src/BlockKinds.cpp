#include "BlockKinds.h"
#include "GainBlockProcessor.h"
#include "HostedPluginBlockProcessor.h"
#include "IrBlockProcessor.h"
#include "LegacyParamIds.h"
#include "NamBlockProcessor.h"
#include "effects/EffectRegistration.h"

juce::String blockCategoryToString(BlockCategory category) {
  switch (category) {
    case BlockCategory::Amp: return "amp";
    case BlockCategory::Drive: return "drive";
    case BlockCategory::Cab: return "cab";
    case BlockCategory::Dynamics: return "dynamics";
    case BlockCategory::Eq: return "eq";
    case BlockCategory::Delay: return "delay";
    case BlockCategory::Reverb: return "reverb";
    case BlockCategory::Modulation: return "modulation";
    case BlockCategory::Pitch: return "pitch";
    case BlockCategory::Filter: return "filter";
    case BlockCategory::Creative: return "creative";
    case BlockCategory::Plugin: return "plugin";
    case BlockCategory::Utility: break;
  }
  return "utility";
}

const std::vector<BlockCategory>& allBlockCategories() {
  static const std::vector<BlockCategory> categories = {
      BlockCategory::Amp,    BlockCategory::Drive,      BlockCategory::Cab,
      BlockCategory::Dynamics, BlockCategory::Eq,       BlockCategory::Delay,
      BlockCategory::Reverb, BlockCategory::Modulation, BlockCategory::Pitch,
      BlockCategory::Filter, BlockCategory::Creative,   BlockCategory::Utility,
      BlockCategory::Plugin,
  };
  return categories;
}

const BlockParamTable& commonBlockParams() {
  // Knob positions, not real units: these predate the parameter API and
  // saved state, the UI and block.setParam all carry them normalised.
  static const BlockParamTable table = {
      BlockParamSpec::boolParam("enabled", "Enabled", true).notAutomatable(),
      BlockParamSpec::boolParam("normalize", "Normalize", true).notAutomatable(),
      BlockParamSpec::floatParam("inputGain", "Input Gain", "normalized", 0.0f, 1.0f, 0.5f)
          .notAutomatable(),
      BlockParamSpec::floatParam("outputGain", "Output Gain", "normalized", 0.0f, 1.0f, 0.5f)
          .notAutomatable(),
      BlockParamSpec::floatParam("mix", "Mix", "normalized", 0.0f, 1.0f, 1.0f).notAutomatable(),
  };
  return table;
}

BlockKindRegistry& BlockKindRegistry::instance() {
  static BlockKindRegistry registry;
  return registry;
}

BlockKindRegistry::BlockKindRegistry() {
  registeringBuiltins = true;
  // The two model-based kinds, so a listing shows every kind a chain can
  // hold. Their blocks are created by loading a tone, and their category is
  // refined per block from the tone (see TONE3000Processor::getChainState).
  add({"nam", "NAM Capture", BlockCategory::Amp, {},
       [] { return std::make_unique<NamBlockProcessor>(); }, false});
  add({"ir", "Impulse Response", BlockCategory::Cab, {},
       [] { return std::make_unique<IrBlockProcessor>(); }, false});

  add({GainBlockProcessor::kKindId, "Gain", BlockCategory::Utility,
       GainBlockProcessor::parameterSpecs(),
       [] { return std::make_unique<GainBlockProcessor>(); }, true});

  // A hosted VST3 or Audio Unit. Listed so the kind id resolves to a name
  // and a category, but not addable by id and with no parameters here: a
  // block of it is made around one particular plugin
  // (TONE3000Processor::addPluginBlock), whose parameters are the block's.
  add({HostedPluginBlockProcessor::kKindId, "Plugin", BlockCategory::Plugin, {},
       []() -> std::unique_ptr<BlockProcessor> { return nullptr; }, false});

  // The built-in effects, one hook per file under plugin/src/effects. The
  // hooks get this registry by reference: instance() is still constructing
  // it, so they must not go through that.
  hookReports.reserve(builtinEffectHooks().size());
  for (const BuiltinEffectHook& hook : builtinEffectHooks()) {
    hookReports.push_back({hook.file, {}});
    runningHook = &hookReports.back();
    hook.registerKinds(*this);
  }
  runningHook = nullptr;
  registeringBuiltins = false;
}

bool BlockKindRegistry::add(BlockKindInfo info) {
  // A built-in that is refused would be a kind silently missing from the
  // product; the constructor keeps the id so a test can say so.
  const auto refuse = [this, &info] {
    if (registeringBuiltins)
      refusedBuiltins.add(info.id.isEmpty() ? juce::String("(empty id)") : info.id);
    return false;
  };

  if (info.id.isEmpty() || info.factory == nullptr)
    return refuse();
  info.spillover = false;
  for (size_t i = 0; i < info.params.size(); ++i) {
    if (findBlockParam(commonBlockParams(), info.params[i].id) >= 0)
      return refuse();
    for (size_t j = 0; j < i; ++j)
      if (info.params[j].id == info.params[i].id)
        return refuse();
    // The reserved id is the capability (see spilloverParam): anything but
    // the switch under it would be read as one by the audio loop.
    if (info.params[i].id == kSpilloverParamId) {
      if (info.params[i].type != BlockParamType::Bool)
        return refuse();
      info.spillover = true;
    }
  }
  // Readings: ids unique, and a range a meter can draw.
  for (size_t i = 0; i < info.readings.size(); ++i) {
    if (info.readings[i].id.isEmpty() || !(info.readings[i].max > info.readings[i].min))
      return refuse();
    for (size_t j = 0; j < i; ++j)
      if (info.readings[j].id == info.readings[i].id)
        return refuse();
  }
  info.defaultMix = juce::jlimit(0.0f, 1.0f, info.defaultMix);

  juce::ScopedLock scoped(lock);
  if (kinds.count(info.id) != 0)
    return refuse();
  const juce::String id = info.id;
  if (runningHook != nullptr)
    runningHook->kindIds.add(id);
  kinds[id] = std::make_unique<BlockKindInfo>(std::move(info));
  return true;
}

const BlockKindInfo* BlockKindRegistry::find(const juce::String& id) const {
  juce::ScopedLock scoped(lock);
  // An id an older build saved a kind under finds the kind it became.
  auto it = kinds.find(id);
  if (it == kinds.end())
    it = kinds.find(t3k::legacy_ids::currentKindId(id));
  return it != kinds.end() ? it->second.get() : nullptr;
}

std::vector<const BlockKindInfo*> BlockKindRegistry::all() const {
  juce::ScopedLock scoped(lock);
  std::vector<const BlockKindInfo*> out;
  out.reserve(kinds.size());
  for (const auto& entry : kinds)
    out.push_back(entry.second.get());
  return out;
}

std::unique_ptr<BlockProcessor> BlockKindRegistry::create(const juce::String& id) const {
  const BlockKindInfo* info = find(id);
  if (info == nullptr || !info->addable)
    return nullptr;
  return info->factory();
}

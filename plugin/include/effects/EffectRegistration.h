#pragma once
#include <memory>
#include <utility>
#include <vector>
#include "BlockKinds.h"

// How a built-in effect gets into the kind registry without a shared list.
//
// Every .cpp under plugin/src/effects (outside the dsp/ and detail/ helper
// folders) defines ONE hook named after the file, and registers its kinds in
// it:
//
//   // plugin/src/effects/delay/SimpleDelay.cpp
//   PEDALEIRA_EFFECT_KINDS(SimpleDelay) {
//     addEffectKind<SimpleDelayProcessor>(registry, "Simple Delay", BlockCategory::Delay, 0.35f,
//                                         BlockMixLaw::DryPreserving);
//   }
//
// The build globs the folder and writes BuiltinEffectKinds.gen.cpp, which
// lists the hooks by name (cmake/EffectSources.cmake); BlockKindRegistry
// runs them once, when it is first used. No file is edited to add an effect,
// and a file without its hook does not link (undefined
// registerKindsIn_<FileStem>).

/** Opens the registration hook of the effect file called `FileStem`.cpp. The
    body gets `registry`; use that, never BlockKindRegistry::instance(), which
    is still being constructed when the hook runs. */
#define PEDALEIRA_EFFECT_KINDS(FileStem)                    \
  void registerKindsIn_##FileStem(BlockKindRegistry&); \
  void registerKindsIn_##FileStem(BlockKindRegistry& registry)

struct BuiltinEffectHook {
  // The file the hook lives in, relative to plugin/src/effects.
  const char* file;
  void (*registerKinds)(BlockKindRegistry&);
};

// Every hook, in file order. Defined in the generated source.
const std::vector<BuiltinEffectHook>& builtinEffectHooks();

/** Registers the kind implemented by `Processor`, which follows the shape of
    GainBlockProcessor: a `static constexpr const char* kKindId`, a
    `static const BlockParamTable& parameterSpecs()` and a default
    constructor, plus `static const BlockReadingTable& readingSpecs()` when
    it reports readings. `defaultMix` is where the common mix knob of a new block
    starts and `mixLaw` how it blends (see BlockKindInfo and BlockMixLaw:
    delays and reverbs pass a mix below 1 and BlockMixLaw::DryPreserving).
    False when the registry refuses
    it (id taken, a parameter id repeated or shadowing a common one); the
    registration test reports every refusal. */
template <typename Processor>
bool addEffectKind(BlockKindRegistry& registry, const juce::String& name, BlockCategory category,
                   float defaultMix = 1.0f, BlockMixLaw mixLaw = BlockMixLaw::Crossfade) {
  BlockKindInfo info;
  info.id = Processor::kKindId;
  info.name = name;
  info.category = category;
  info.params = Processor::parameterSpecs();
  // A kind that reports readings declares them as it does its parameters.
  if constexpr (requires { Processor::readingSpecs(); })
    info.readings = Processor::readingSpecs();
  info.factory = []() -> std::unique_ptr<BlockProcessor> { return std::make_unique<Processor>(); };
  info.addable = true;
  info.defaultMix = defaultMix;
  info.mixLaw = mixLaw;
  return registry.add(std::move(info));
}

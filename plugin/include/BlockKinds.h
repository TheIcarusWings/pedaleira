#pragma once
#include <juce_core/juce_core.h>
#include <functional>
#include <map>
#include <memory>
#include <vector>
#include "BlockParams.h"
#include "BlockProcessor.h"

// What a block is for, as a user would sort it. Drives nothing in the audio
// path: it is for browsing, colour and the command layer's listings.
enum class BlockCategory {
  Amp,
  Drive,
  Cab,
  Dynamics,
  Eq,
  Delay,
  Reverb,
  Modulation,
  Pitch,
  Filter,
  Creative,
  Utility,
  Plugin
};

juce::String blockCategoryToString(BlockCategory category);
// Every category, in the order of the enum (kinds.list reports them).
const std::vector<BlockCategory>& allBlockCategories();

// One registered block kind: everything needed to list it, build it and
// describe its parameters without holding an instance.
struct BlockKindInfo {
  // Stable id, saved with every block of the kind ("utility.gain").
  juce::String id;
  juce::String name;
  BlockCategory category{BlockCategory::Utility};
  // The kind's own parameters (the processor reports the same table).
  BlockParamTable params;
  // Builds an unprepared processor at its parameter defaults. May be slow
  // and may allocate; never called on the audio thread or under chainMutex
  // by the add path (see TONE3000Processor::addKindBlock).
  std::function<std::unique_ptr<BlockProcessor>()> factory;
  // False for the kinds that only exist around a model file (nam, ir): they
  // are listed, but a block of them comes from a tone, not from the registry.
  bool addable{true};
  // Where the common mix knob of a freshly added block starts (0 = dry,
  // 1 = wet). 1 suits a kind that replaces its input; a kind that is heard
  // next to the dry signal (delay, reverb) starts lower.
  float defaultMix{1.0f};
  // How the common mix knob blends dry and wet for this kind (see
  // BlockMixLaw). Crossfade unless the kind is heard beside the dry signal.
  BlockMixLaw mixLaw{BlockMixLaw::Crossfade};
  // The kind's tail rings out after a bypass (BlockProcessor::
  // supportsSpillover). Not for the caller to set: add() derives it from the
  // table, where spilloverParam() declares the capability.
  bool spillover{false};
  // A first guess at what a block of this kind costs, as a fraction of one
  // core at 48 kHz (ChainBlock::cpuCost starts from it and is then
  // measured).
  float costHint{0.002f};
  // What the kind reports while it runs (the processor reports the same
  // table). Empty for most kinds. Last, so the positional initialisers of
  // the built-in kinds stay as they are.
  BlockReadingTable readings;
  // What a browser files the kind under within its category, and a line
  // about it: the amps carry "High gain", "Clean" or "Crunch" and their
  // character line. Empty for the kinds that have neither.
  juce::String family;
  juce::String blurb;
};

// The open set of block kinds. "A tone block is NAM or IR" used to be a
// closed assumption; a kind now is whatever is registered here, and the
// chain only ever talks to the BlockProcessor its factory returns.
//
// Built-in kinds register in the constructor, the kind that hosts a plugin
// among them: every hosted VST3 or Audio Unit is a block of the ONE kind
// "plugin", told apart by the plugin identity in its state
// (HostedPluginBlockProcessor.h). The built-in effects register there too,
// each from its own file under plugin/src/effects through a hook the build
// lists (effects/EffectRegistration.h). Anything else (the test-only kinds)
// calls add() at startup, before a state that names the kind is restored.
// Lookups may come from any non-audio thread.
class BlockKindRegistry {
public:
  static BlockKindRegistry& instance();

  // False (and nothing registered) when the id is empty or taken, or when a
  // parameter id repeats or shadows a common block parameter: block.setParam
  // addresses both sets through one name.
  bool add(BlockKindInfo info);

  // Null when no kind of that id is registered. The pointer stays valid for
  // the life of the process.
  const BlockKindInfo* find(const juce::String& id) const;

  // Every kind, sorted by id.
  std::vector<const BlockKindInfo*> all() const;

  // A fresh processor of that kind, or null (unknown id, or a kind that
  // cannot be built without a model).
  std::unique_ptr<BlockProcessor> create(const juce::String& id) const;

  // What the built-in effect hooks did when the registry was built, for the
  // test that keeps registration honest: the kinds each effect file added,
  // and every id add() refused on the way (a clash, a bad table).
  struct EffectHookReport {
    juce::String file;
    juce::StringArray kindIds;
  };
  const std::vector<EffectHookReport>& effectHookReports() const noexcept { return hookReports; }
  const juce::StringArray& refusedBuiltinIds() const noexcept { return refusedBuiltins; }

private:
  BlockKindRegistry();

  mutable juce::CriticalSection lock;
  std::map<juce::String, std::unique_ptr<BlockKindInfo>> kinds;
  // Written by the constructor only.
  std::vector<EffectHookReport> hookReports;
  juce::StringArray refusedBuiltins;
  bool registeringBuiltins{false};
  EffectHookReport* runningHook{nullptr};
};

// The five values every tone block carries outside its kind (see
// ChainBlock): inputGain, outputGain and mix as knob positions from 0 to 1,
// enabled and normalize as switches. Described in the same shape as kind
// parameters so a client sees one list per block.
const BlockParamTable& commonBlockParams();

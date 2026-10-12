#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <atomic>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "BlockDelayLine.h"
#include "BlockEq.h"
#include "BlockProcessor.h"
#include "BlockSpectrum.h"
#include "GridTopology.h"
#include "IrBlockProcessor.h"
#include "NamBlockProcessor.h"

// ChainBlockType itself lives in BlockProcessor.h (a processor names the
// type it implements); the string forms used by saved state are here.
inline juce::String chainBlockTypeToString(ChainBlockType type) {
  switch (type) {
    case ChainBlockType::NAM: return "nam";
    case ChainBlockType::INSERT: return "insert";
    case ChainBlockType::KIND: return "kind";
    case ChainBlockType::IR: break;
  }
  return "ir";
}

inline ChainBlockType chainBlockTypeFromString(const juce::String& s) {
  if (s == "nam") return ChainBlockType::NAM;
  if (s == "insert") return ChainBlockType::INSERT;
  if (s == "kind") return ChainBlockType::KIND;
  return ChainBlockType::IR;
}

// Which chain is being processed/edited in stereo mode. Left and Right are
// the legacy names of rows 0 and 1; the grid's rows 2 and 3 have no side.
enum class ChainSide { Left, Right };

// A lane is a grid row (GridTopology.h). The legacy rig uses the first two.
constexpr int kNumLanes = kGridRows;
constexpr int kNumLegacyLanes = 2;
inline int laneIndex(ChainSide side) { return side == ChainSide::Right ? 1 : 0; }

// Wet-path fade time (see ChainBlock::wetFadeGain): every discontinuous
// per-block transition (engine swap, power toggle, block add/removal)
// glides the block's wet mix through bypass over this ramp instead of
// splicing the waveform (audible click). Also the ramp for the global
// chain-edit fade (reorder/cross-lane moves mute-splice the chain output).
constexpr double kWetFadeSeconds = 0.025;

// Minimum tiles per lane. A lane always presents at least this many blocks
// (tones + insert placeholders), and always at least one insert placeholder,
// so an empty lane shows kMinLaneSlots empty slots, and once the user has
// filled them all there is still one trailing empty slot to add into. The
// invariant (insertCount == max(kMinLaneSlots - toneCount, 1)) is enforced by
// TONE3000Processor::normalizeLaneInserts after every structural change, with
// one relaxation: while a stereo branch is active, the branch lane's surplus
// trailing inserts are trimmed below this baseline so its indented rail ends
// level with the trunk lane (see rebuildTopology).
constexpr int kMinLaneSlots = 5;

// Chain block data structure
struct ChainBlock {
  std::string id;  // Chain block UUID
  ChainBlockType type;

  // Tone metadata (full tone JSON stored for complete state persistence)
  int toneId;
  juce::String toneJson;  // Complete tone JSON from TONE3000 API
  int activeModelId;      // Currently active model ID (single source of truth)

  // Parsed-once copy of toneJson (full API payload; model switching needs
  // the model URLs) and the slim projection getChainState ships to the UI
  // (title/images/user/model names only). Both are ref-counted vars, so
  // serializing chain state is O(1) per block instead of a JSON re-parse.
  // Set together wherever toneJson is set; see setToneOnBlock.
  juce::var toneVar;
  juce::var toneSummary;

  // Model cache: stores downloaded model data by model ID
  std::map<int, std::vector<uint8_t>> modelCache;

  // True when the block's stored tone can still name this model: it is the
  // active model, or the toneVar models array lists it (local tones keep
  // their full list; catalog tones collapse to the active model on every
  // switch, see switchModel). This is the persistence boundary for
  // modelCache: saves embed bytes and restores re-seed them only for
  // referenced models (serializeChainToTree / reconcileChainFromTree).
  // Anything else in the cache is an in-memory audition convenience;
  // persisting those bytes is what bloated DAW projects by hundreds of MB
  // (issue #127).
  bool referencesModel(int modelId) const {
    if (modelId == activeModelId)
      return true;
    if (const auto* models = toneVar["models"].getArray())
      for (const auto& model : *models)
        if (static_cast<int>(model["id"]) == modelId)
          return true;
    return false;
  }

  // State flags
  bool loaded;   // True when active model is loaded and ready
  bool enabled;  // True when block is enabled in processing chain

  // True when the last download/prepare of the active model failed (network
  // down, tone3000.com unreachable, bad model data). The UI swaps its loading
  // dots for a retry affordance targeting retryModelLoad. Runtime-only,
  // never persisted; cleared whenever a new load is queued.
  bool loadFailed{false};

  // True while a background download/prepare of the active model is in
  // flight. Split from `loaded` so a model switch/tone swap keeps the
  // previous engine processing (`loaded` stays true) while the replacement
  // downloads; the UI keys its loading affordances off this flag.
  // Runtime-only, never persisted.
  bool modelLoading{false};

  // One-shot: armed by loadTone (Select-flow) so the block's first
  // successful load sets the default mix from the actual model (long IR =
  // half wet, only known once the file arrives). Cleared on first apply;
  // never set by swaps/switches/restores, which keep the user's mix.
  // Runtime-only, never persisted.
  bool applyDefaultMixOnLoad{false};

  // Click-free wet-path fade (audio thread) + swap handshake.
  // `wetFadeGain` multiplies the block's wet mix and is the smoothing path
  // for every transition whose end state is bypass: the audio thread targets
  // it at 1 while the block wants to be heard (`enabled` and no swap
  // pending) and 0 otherwise, so power toggles glide through bypass, fresh
  // blocks fade in from bypass, and removals fade out before detaching.
  //
  // The handshake: another thread raises `swapFadePending` (engine swap,
  // failure drop, removal), the audio thread fades to silence and raises
  // `swapFadeDone`, and the requester then applies its change under the
  // chain lock (see requestSwapFadeAndWait: bounded wait; when no
  // callbacks are running the change applies directly, nothing is audible).
  //
  // Two fade shapes, picked by `swapMuteWet`:
  //  - false (bypass fade): wetFadeGain rides the mix and glides the
  //    post-mix Out Gain to unity in step, so the output crossfades toward
  //    the block's dry input at pass-through level. Right for transitions
  //    that END at bypass (power off, removal, failure drop, fresh-block
  //    fade-in; unity dry is what plays afterwards anyway).
  //  - true (wet mute): engine swaps end back at wet, and their dry input
  //    was never audible; at 100% mix crossfading through it blasts ~50 ms
  //    of the un-cabbed/un-ampped signal (a raw amp head into no cab is a
  //    loud bright burst). Instead `swapWetMuteGain` mutes just the wet
  //    term while the dry share of the user's mix holds steady: the old
  //    engine dips to silence, engines swap, the new one fades in from
  //    silence. wetFadeGain stays at 1 throughout.
  // Both gains are plain multipliers in the mix loop, so the shapes compose
  // (a power toggle mid-swap still glides to bypass through wetFadeGain).
  std::atomic<bool> swapFadePending{false};
  std::atomic<bool> swapFadeDone{false};
  std::atomic<bool> swapMuteWet{false};
  juce::LinearSmoothedValue<float> wetFadeGain;
  juce::LinearSmoothedValue<float> swapWetMuteGain{1.0f};

  // Tail spillover, for kinds that declare it
  // (BlockProcessor::supportsSpillover). Such a block does not go straight
  // to bypass when it is switched off: its INPUT fades to silence
  // (`spillInputGain`), its engine keeps running, and its wet output stays
  // in the mix on top of the dry signal at unity (`spillTailGain` holds the
  // wet term up while wetFadeGain takes the dry share back to pass-through)
  // until the reported tail has passed. Then spillTailGain closes, the block
  // is skipped like any bypassed one and its engine is reset. Switching it
  // back on mid-tail fades the input back in. With the block's `spillover`
  // switch off, or for a kind without the capability, none of this moves
  // and the bypass is the plain wet fade above.
  //
  // `released` is the same request from outside the saved state
  // (TONE3000Processor::setBlockReleased): the block rings out, or cuts,
  // exactly as it does for the power switch, without `enabled` and the undo
  // history being touched. Scenes do not use it: a scene's block state IS
  // the power switch (the blocks carry the active scene, see Scenes.h), so
  // that everything keyed on `enabled` follows a scene too (which amp is
  // the lane's last and keeps loudness normalization, what the chain state
  // reports, what a footswitch toggles). A scene that turns a block off
  // therefore takes the same release path through `enabled`.
  //
  // All audio-thread state except `released`.
  static constexpr juce::int64 kSpillDone = (std::numeric_limits<juce::int64>::max)();
  std::atomic<bool> released{false};
  juce::LinearSmoothedValue<float> spillInputGain{1.0f};
  juce::LinearSmoothedValue<float> spillTailGain{0.0f};
  // Chain-rate frames since the block was switched off; kSpillDone once the
  // tail is over (and for a block that was never on).
  juce::int64 spillElapsedSamples{kSpillDone};
  // The engine has processed audio since its last reset, so it may be
  // holding some (see BlockProcessor::reset).
  bool engineHoldsAudio{false};

  // Set by the audio thread when the block's processor reports
  // BlockProcessResult::Failed (the block is taken out of processing in the
  // same breath). The message thread drains it in getChainState and writes
  // the log line there; string building/logging is not RT-safe.
  std::atomic<bool> rtProcessingFailed{false};
  // The drained flag, kept so the chain state can go on saying why the block
  // stopped (`processingFailed`). Runtime-only; cleared when a processor is
  // next installed on the block. Guarded by chainMutex.
  bool processingFailed{false};

  // Registry kinds (type == KIND). `kindId` names the kind and `kindName` is
  // what to call the block; both are stamped when a processor is installed.
  // Empty for the model-based types, whose kind is their type.
  juce::String kindId;
  juce::String kindName;
  // How the mix knob weighs dry and wet for the block's kind
  // (BlockKindInfo::mixLaw), stamped with the two above. Crossfade for
  // everything that is not a registry kind asking for another law.
  BlockMixLaw mixLaw{BlockMixLaw::Crossfade};

  // A registry kind's processor is built off the chain lock, also on the
  // restore paths (see TONE3000Processor::applyKindState). While that build
  // is in flight the block has no processor yet, `modelLoading` is set, and
  // this holds the KindState node it is being built from, so a save in that
  // window still writes the block whole. `kindBuildSerial` names the build
  // the block is waiting for: a later restore bumps it, and a build that
  // comes back with an older number is dropped. Guarded by chainMutex.
  juce::ValueTree pendingKindState;
  int kindBuildSerial{0};

  // A tone is being loaded onto this registry-kind block (swapTone): the
  // block keeps running its kind until the model lands and the install
  // restamps `type`. Everything that asks "is there a model to load, retry
  // or save" keys on this rather than on the type alone.
  bool hasToneOnKind() const noexcept {
    return type == ChainBlockType::KIND && activeModelId != 0;
  }

  // Valid on a placeholder for a kind this build does not have (a preset
  // from a newer build, a plugin that is not installed): the block node
  // exactly as it was loaded. The block holds no processor, passes audio
  // through and cannot be edited, and saving writes this node back
  // untouched, so opening and saving a project never destroys what another
  // build put there.
  juce::ValueTree missingKindNode;
  bool isMissingKind() const noexcept { return missingKindNode.isValid(); }

  // Delays the dry share of the mix (and the bypassed signal) by the
  // processor's latency so it stays in phase with the wet path. Sized under
  // chainMutex by TONE3000Processor::refreshLatencyCompensation; zero for a
  // block without latency, where the audio path never touches it.
  BlockDelayLine dryDelay;

  // The block's engine: everything that differs by kind (see
  // BlockProcessor.h). NAM and IR blocks always hold one matching `type`, an
  // engine-less one until the first model lands; a registry-kind block holds
  // the one its kind's factory built; insert placeholders and missing-kind
  // placeholders hold none. A load builds a complete replacement off-thread and the install
  // swaps this pointer under chainMutex once the swap fade has the wet path
  // silent (applyPreparedModelToChainBlock); a tone swap that changes the
  // block's type is the same swap with a processor of the other kind.
  std::unique_ptr<BlockProcessor> processor;

  // The type-specific settings, authoritative here rather than in the
  // processor: they exist before any engine does (a block whose model is
  // still downloading is already editable and saveable), an insert
  // placeholder has no processor at all, and they must survive a swap to
  // another kind and back. Saved state writes both fields for every block
  // type. Whoever changes them pushes the result into the processor
  // (pushProcessorState), which keeps the copy its audio path reads.
  BlockProcessorState processorState;

  void pushProcessorState() {
    if (processor != nullptr)
      processor->setState(processorState);
  }

  // Typed views of the processor for the callers that are inherently about
  // one kind (calibration metadata, voice counts, the cab/reverb verdict).
  // Null when the block is of another kind. New kinds should not need one:
  // the shared audio path only ever talks to BlockProcessor.
  NamBlockProcessor* nam() const noexcept {
    return processor != nullptr && processor->kind() == ChainBlockType::NAM
               ? static_cast<NamBlockProcessor*>(processor.get())
               : nullptr;
  }
  IrBlockProcessor* ir() const noexcept {
    return processor != nullptr && processor->kind() == ChainBlockType::IR
               ? static_cast<IrBlockProcessor*>(processor.get())
               : nullptr;
  }

  // Per-block controls (normalized 0..1)
  float inputGainNormalized{0.5f};  // 0.5 = unity gain; drives the block harder/softer
  juce::LinearSmoothedValue<float> inputGainSmoother;
  float outputGainNormalized{0.5f};  // 0.5 = unity gain
  juce::LinearSmoothedValue<float> outputGainSmoother;
  float mixNormalized{1.0f};  // 0 = dry, 1 = wet
  juce::LinearSmoothedValue<float> mixSmoother;

  // Per-block meter levels (dB, -60 floor). Written by the audio thread every
  // block, read by the UI via getMeterLevels(). Input is measured post
  // input-gain (what the model actually receives), output post mix + Out
  // Gain.
  std::atomic<float> inputMeterDb{-60.0f};
  std::atomic<float> outputMeterDb{-60.0f};

  // What this block costs: the fraction of one core it takes at the current
  // chain rate. Seeded when its engine is
  // installed, refined by the audio thread while it runs (a moving average
  // of the time around its processing), and left alone while it is
  // bypassed, so switching a block on does not move the plan's figure.
  std::atomic<float> cpuCost{0.0f};
  // Audio thread: the average has had its first measurement.
  bool cpuCostMeasured{false};

  // Per-block 6-band EQ: on the wet signal after the model by default
  // (before Out Gain and the mix), or between the input gain and the model
  // when its pre flag is on. Flat by default, in which case processing is
  // skipped entirely (single branch per audio block).
  BlockEq eq;

  // Spectrum analyzer for the EQ editor backdrop. Only fed by the audio thread
  // while the UI has this block's EQ view open (atomic enabled flag).
  BlockSpectrum spectrum;

  ChainBlock(const std::string& blockId, ChainBlockType blockType)
      : id(blockId), type(blockType), toneId(0), activeModelId(0), loaded(false),
        enabled(true), processor(makeBlockProcessor(blockType)) {}
};

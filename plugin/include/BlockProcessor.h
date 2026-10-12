#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include "BlockParams.h"

class ChainOversampler;
class RtWorkerPool;

// Chain block types: the coarse class saved state and the routing code
// branch on. NAM and IR are the two model-based kinds, each with its own
// loader and its own saved node shape. KIND is every block whose engine
// comes from the kind registry (BlockKinds.h): built-in effects and hosted
// plugins; which one is the block's kind id (ChainBlock::kindId).
// INSERT is an empty slot.
enum class ChainBlockType { NAM, IR, INSERT, KIND };

// How a block's common mix knob (0 = dry, 1 = wet) weighs the two signals.
// A property of the kind (BlockKindInfo::mixLaw), stamped on the block when
// its processor is installed.
//  - Crossfade: dry x (1 - mix) + wet x mix. Right for a kind whose wet
//    signal replaces the dry one (an amp, a cab, a compressor, an EQ): half
//    way is half of each, and the two never add up to more than one signal.
//  - DryPreserving: dry x min(1, 2(1 - mix)) + wet x min(1, 2 mix). The dry
//    signal stays at unity up to 50%, where the wet one reaches unity too,
//    and only fades above that, down to wet alone at 100%. Right for a kind
//    heard NEXT to the dry signal (a delay, a reverb): bringing the echoes in
//    must not turn the guitar down.
enum class BlockMixLaw { Crossfade, DryPreserving };

inline const char* blockMixLawToString(BlockMixLaw law) noexcept {
  return law == BlockMixLaw::DryPreserving ? "dryPreserving" : "crossfade";
}

// The dry and wet shares of a mix position under a law. Audio thread.
inline float blockMixDryShare(BlockMixLaw law, float mix) noexcept {
  return law == BlockMixLaw::DryPreserving ? juce::jmin(1.0f, 2.0f * (1.0f - mix)) : 1.0f - mix;
}
inline float blockMixWetShare(BlockMixLaw law, float mix) noexcept {
  return law == BlockMixLaw::DryPreserving ? juce::jmin(1.0f, 2.0f * mix) : mix;
}

// The longest tail a block is taken to have, whatever its kind says. A
// feedback path can ring for minutes (a delay at 95% and 2 s takes 272 s to
// fall 60 dB), and the reported tail is both what the host keeps rendering
// after the end and how long a bypassed block with spillover keeps running.
// Everything in the chain reads a kind's tail through
// BlockProcessor::reportedTailSeconds, which stops here.
inline constexpr double kMaxBlockTailSeconds = 30.0;

// Everything a block engine needs that lives on the processor, captured at
// one moment under chainMutex (TONE3000Processor::blockPrepareSpec). Engines
// never reach back into the processor: whatever sizes their buffers or picks
// their layout has to arrive through here, so a block kind that is built
// off-thread can be checked against the live values when it is installed.
struct BlockPrepareSpec {
  // The chain rate: kChainBaseSampleRate × oversampleFactor.
  double chainSampleRate{48000.0};
  // The chain oversampling factor (1, 2, 4 or 8). Kinds that run at the base
  // rate (IR convolution) wrap themselves in an island sized from it.
  int oversampleFactor{1};
  // Largest frame count per callback at the base rate
  // (TONE3000Processor::chainBaseBlockSize).
  int baseBlockSize{0};
  // Largest frame count process() is ever handed: baseBlockSize ×
  // oversampleFactor (TONE3000Processor::chainDomainBlockSize).
  int maxBlockSize{0};
  // Channels in the lane buffer. A lane is mono or stereo; engines prepare
  // for the wider case.
  int numChannels{2};
  // Independent voices a mono model must carry (2 in the dual-mono input
  // mode, else 1; see NamEngine.h). Fixed at engine construction, so a
  // change means a rebuild, not a prepare().
  int voiceCount{1};

  // The rate and block size process() sees for a kind that does or does not
  // run at the base rate (BlockProcessor::runsAtBaseRate), so a kind can
  // size itself without knowing the chain is oversampled.
  double runSampleRate(bool atBaseRate) const noexcept {
    return atBaseRate ? chainSampleRate / juce::jmax(1, oversampleFactor) : chainSampleRate;
  }
  int runBlockSize(bool atBaseRate) const noexcept {
    return atBaseRate ? baseBlockSize : maxBlockSize;
  }
};

// Musical time for tempo-synced kinds, one copy per audio callback. Filled
// from the host playhead when the host reports one, else from the rig's own
// tempo (TONE3000Processor::setTempo), which has no transport.
struct BlockTransport {
  double bpm{120.0};
  int timeSigNumerator{4};
  int timeSigDenominator{4};
  // The host timeline is rolling. Always false without a host playhead: the
  // rig then has a tempo but no song position, and synced kinds free-run.
  bool playing{false};
  // Position of the callback's first frame in host-rate samples: the host
  // timeline's when it reports one, else a count of the frames processed so
  // far. A block inside a sliced or resampled callback sees the callback's
  // value, not its own offset.
  juce::int64 samplePosition{0};
  // bpm, signature and playing came from the host playhead.
  bool fromHost{false};
  // The same position as a time and as a musical position, for kinds that
  // hand a playhead on (a hosted plugin syncing its delay or LFO to the
  // song): the host's own values when it reports them, else derived from
  // samplePosition and bpm, so they always advance together. ppqPosition is
  // in quarter notes; ppqLastBarStart is the start of the current bar.
  double timeSeconds{0.0};
  double ppqPosition{0.0};
  double ppqLastBarStart{0.0};
};

// What only the NAM kind reads (the IR kind borrows lastNamIndex). Grouped
// so every other kind can leave it alone.
struct NamBlockContext {
  // Position of the lane's last enabled and loaded NAM block (-1 when there
  // is none). With BlockProcessContext::blockIndex it tells a block whether
  // an amp model still follows it: NAM picks calibrated hand-off over
  // loudness normalization, a stereo IR falls back to mono.
  int lastNamIndex{-1};
  // Input calibration (the user's interface level in dBu) and the loudness
  // target, as cached for this callback.
  bool calibrateInput{false};
  float inputCalibrationLevel{12.0f};
  float targetLoudness{-18.0f};
  // The realtime worker pool when multi-core is on for this callback, else
  // null (run serially on the calling thread).
  RtWorkerPool* workerPool{nullptr};
};

// What a block reads from the lane and the processor on every callback.
// Built on the audio thread from values already cached for the callback
// (plain copies, no allocation), one per block processed.
struct BlockProcessContext {
  // This block's position in its lane.
  int blockIndex{0};
  // The mono chain is carrying two independent mono signals (the dual-mono
  // input mode), so nothing may mix or mirror the channels. A two-channel
  // buffer WITHOUT this flag is a true stereo signal: a kind may treat its
  // sides differently there (a stereo IR, a ping-pong), and only there. A
  // one-channel buffer is a mono bus or one lane of the two-lane mode.
  bool dualMono{false};
  // Tempo and transport for this callback.
  BlockTransport transport;
  // The NAM passengers; see NamBlockContext.
  NamBlockContext nam;
};

/** Whether a kind may treat its two sides differently in this call (a
    ping-pong, a decorrelated reverb, an LFO phase offset): the buffer has two
    channels, they are one stereo signal rather than two independent ones
    (dual mono), and no amp model follows in the lane. A NAM block runs
    channel 0 through its model and copies the result to channel 1, so a
    stereo image made before it is thrown away, and worse, whatever a kind
    put on the right side only would be lost: before an amp, and whenever
    this is false, a kind runs its mono version on every channel alike. The
    IR kind picks its stereo convolver by the same three conditions. */
inline bool blockHasStereoImage(int numChannels, const BlockProcessContext& ctx) noexcept {
  return numChannels == 2 && !ctx.dualMono && ctx.blockIndex > ctx.nam.lastNamIndex;
}

enum class BlockProcessResult {
  // The buffer now holds the block's wet signal; the shared stages (post EQ,
  // mix, fades, meters) carry on.
  Processed,
  // Nothing to run (no engine yet). The caller leaves the buffer as it
  // stands and moves on to the next block.
  Idle,
  // The engine failed in a way that will repeat. The caller restores the dry
  // signal, takes the block out of processing and flags it for the message
  // thread, which is where the failure gets logged.
  Failed
};

// The type-specific settings of the two model-based kinds, in the shape saved
// state has always carried them: every block node writes "normalize" and "slimSize"
// whatever its type, and there is no per-block version field to branch on.
// The block holds the authoritative copy (ChainBlock::processorState) because
// settings exist before any engine does and survive an engine swap, including
// one to another kind and back; a processor keeps the fields it understands
// and ignores the rest. Registry kinds do not use it: their state is the
// KindState node (see BlockProcessor::getKindState).
struct BlockProcessorState {
  // NAM: loudness normalization on the last amp in the lane ("normalize").
  bool normalize{true};
  // NAM: A2 size in NAM's slimmable-size domain, 0.0 = lite .. 1.0 = full
  // ("slimSize").
  double slimSize{0.0};
};

/**
 * One block's audio engine: the part of a chain block that differs by kind.
 * ChainBlock keeps what every kind shares (gains, mix, EQ, meters, the fade
 * handshake) and TONE3000Processor::processChainOnBuffer runs those stages
 * around a single processInChain() call, so a kind only ever sees its own
 * wet path.
 *
 * A kind implements the virtuals. The chain calls the three non-virtual
 * entry points (prepareForChain, prepareForChainInstall, processInChain),
 * which add what a kind may ask for without building it: running at the base
 * rate inside an oversampled chain (runsAtBaseRate).
 *
 * Threading follows the engines this was lifted from: process() runs on the
 * audio thread (or a lane worker) with chainMutex held; everything else runs
 * on a message or loader thread, either with chainMutex held or on a
 * processor no block owns yet (the off-thread build). Parameter sets are the
 * exception: setParameter is a lock-free store any thread may make.
 * Destruction can be heavy and must never happen under chainMutex; the
 * install path hands the outgoing processor back to its caller for that
 * reason.
 */
class BlockProcessor {
public:
  BlockProcessor();
  virtual ~BlockProcessor();

  /** The block type this engine implements. A block's processor always
      matches ChainBlock::type; the two are stamped together at install. */
  virtual ChainBlockType kind() const noexcept = 0;

  /** The engine's id in the kind registry (BlockKinds.h): "nam", "ir",
      "utility.gain". Saved with the block, so it never changes. */
  virtual const char* kindId() const noexcept = 0;

  /** True once there is something to run. False on the engine-less
      processor a tone block holds while its first model loads. */
  virtual bool hasEngine() const noexcept = 0;

  /** A first guess at what this processor costs, as a fraction of one core
      at the rate it was prepared for, or 0 when it has none (the chain then
      uses its kind's costHint). Only ever a seed: the chain measures the
      real cost while the block runs (ChainBlock::cpuCost). */
  virtual float costSeed() const noexcept { return 0.0f; }

  /** Ask to be run at the base rate (kChainBaseSampleRate) whatever the
      chain's oversampling factor: process() then gets base-rate frames and
      the chain does the rate change around it. Right for linear and
      time-based processing, which gains nothing above the base rate and
      whose cost and tuning would otherwise move with the factor. A fixed
      property of the kind. */
  virtual bool runsAtBaseRate() const noexcept { return false; }

  /** (Re)prepare for the chain domain described by `spec`. Called for every
      block when the domain moves (prepareToPlay, an oversampling change), so
      it resets engine and smoother state. Never called on the audio thread.
      A base-rate kind sizes itself from spec.runSampleRate(true) and
      spec.runBlockSize(true). */
  virtual void prepare(const BlockPrepareSpec& spec) = 0;

  /** Last step of an off-thread build, called under chainMutex right before
      the processor goes live. The build prepared its engine against a spec
      read earlier; this repairs whatever drifted since and arms the state
      that depends on the live domain. Unlike prepare() it leaves an engine
      that already fits alone: re-preparing can be heavy and would throw away
      the warm-up the build did. */
  virtual void prepareForInstall(const BlockPrepareSpec& spec) { prepare(spec); }

  /** Process the block's wet path in place. `buffer` is one lane's buffer
      (1 or 2 channels), already through the block's input gain and pre EQ,
      at the chain rate or, for a base-rate kind, decimated to the base rate.
      No allocation, locking or logging. Must not throw: a failure is
      reported through the result. */
  virtual BlockProcessResult process(juce::AudioBuffer<float>& buffer,
                                     const BlockProcessContext& ctx) = 0;

  /** For a base-rate kind only: a stage that has to run on the chain-rate
      buffer after the base-rate section (the IR kind's level smoothing, kept
      where it always ran). Called after a process() that returned Processed.
      Chain-rate kinds do everything in process(). */
  virtual void postProcessAtChainRate(juce::AudioBuffer<float>& buffer,
                                      const BlockProcessContext& ctx) {
    juce::ignoreUnused(buffer, ctx);
  }

  /** Delay this block adds to its wet path, in samples at the rate process()
      runs at. The chain delays the dry share of the block's mix and the
      other lane by the same amount and adds it to the latency the plugin
      reports (TONE3000Processor::refreshLatencyCompensation).

      The value is latched: a kind whose latency follows a parameter
      (BlockParamSpec::affectsLatency) keeps running, and reporting, the
      latency it had until commitLatency() tells it to adopt the new one.
      The wet path and the chain's compensation then always change in the
      same instant, under the chain lock and the chain-edit fade, never a
      parameter set apart. */
  virtual int latencySamples() const noexcept { return 0; }

  /** The parameters ask for a latency other than the latched one. */
  virtual bool latencyChangePending() const noexcept { return false; }

  /** Adopt the latency the parameters ask for. Called under chainMutex with
      the chain output muted (TONE3000Processor::flushDeferredBlockRefresh,
      and a restore that moved the parameter); the chain re-reads
      latencySamples() and resizes its compensation before audio resumes.
      prepare() latches too. Not on the audio thread. */
  virtual void commitLatency() {}

  /** How long the block keeps sounding after its input stops. The processor
      reports the longest one to the host (getTailLengthSeconds). Parameters
      that move it are flagged BlockParamSpec::affectsTail. */
  virtual double tailSeconds() const noexcept { return 0.0; }

  /** tailSeconds() as the chain uses it: never negative, never beyond
      kMaxBlockTailSeconds (a tail that is not a number counts as the
      longest). This is the tail the host is told, the one the command layer
      shows, and how long a spillover release lasts; a tail still ringing at
      the cap is faded out there. */
  double reportedTailSeconds() const noexcept {
    const double seconds = tailSeconds();
    if (!(seconds < kMaxBlockTailSeconds))
      return kMaxBlockTailSeconds;
    return seconds > 0.0 ? seconds : 0.0;
  }

  /** The kind can ring out after it is switched off (tail spillover): a
      delay or a reverb whose echoes should finish rather than be cut. The
      chain then keeps running a bypassed block with its input faded to
      silence and its wet output still added to the dry signal until
      reportedTailSeconds() has passed
      (TONE3000Processor::processChainOnBuffer). A
      fixed property of the kind. A parameterised kind declares it by
      putting spilloverParam() in its table (see BlockKinds.h). Such a kind's
      tailSeconds() is read on the audio thread, so it must not allocate or
      lock. */
  virtual bool supportsSpillover() const noexcept { return false; }

  /** The block's own `spillover` switch: on, a bypass releases the tail; off,
      it cuts it like any other kind. Audio thread. */
  virtual bool spilloverEnabled() const noexcept { return false; }

  /** Drop whatever the engine is still holding (delay lines, filter and
      envelope state) without touching its parameters. The chain calls it on
      the audio thread when a block stops being processed (bypassed, its tail
      rung out), so switching it back on later starts from silence instead of
      replaying audio from before. No allocation, locking or logging. */
  virtual void reset() noexcept {}

  /** Copy the settings this kind owns into `state`, leaving the other
      fields alone. */
  virtual void getState(BlockProcessorState& state) const = 0;

  /** Adopt the fields this kind understands and assert them on the engine.
      May be heavy (a NAM retier swaps weights); callers hold chainMutex and,
      where the change is audible, the chain-edit fade. */
  virtual void setState(const BlockProcessorState& state) = 0;

  /** Fixed trim the shared mix loop applies to the wet term only, in dB.
      Gain staging that belongs to the kind rather than to a knob: the dry
      share of the mix passes at its natural level. */
  virtual float wetPadDb() const noexcept { return 0.0f; }

  // ---- Parameters ---------------------------------------------------------
  // A kind's own parameters, on top of the five every block has (see
  // commonBlockParams). The model-based kinds declare none.

  /** The kind's parameter table. The same object for the processor's whole
      life, so indices are stable. */
  virtual const BlockParamTable& parameterTable() const noexcept;

  /** Current value of parameter `index`, in its real unit. Any thread. */
  virtual float getParameter(int index) const noexcept {
    juce::ignoreUnused(index);
    return 0.0f;
  }

  /** Set parameter `index` to `value` (real unit, clamped to the spec). A
      lock-free store: any thread, including a host's automation callback;
      the audio thread picks it up on its next block and does any smoothing
      itself. An index outside the table is ignored. */
  virtual void setParameter(int index, float value) noexcept {
    juce::ignoreUnused(index, value);
  }

  /** The value of parameter `index` as the kind would display it ("-6.0 dB",
      "Hall"), for kinds whose values are not self-explanatory numbers (a
      hosted plugin's normalised 0..1). Empty when the number says it all.
      Message thread. */
  virtual juce::String parameterText(int index) const {
    juce::ignoreUnused(index);
    return {};
  }

  /** Index of the parameter with that id, or -1. Not for the audio thread
      (string compare); resolve once and keep the index. */
  int findParameter(const juce::String& id) const {
    return findBlockParam(parameterTable(), id);
  }

  // ---- Readings -----------------------------------------------------------
  // Read-only values the block reports while it runs (BlockReadingSpec). Most
  // kinds have none.

  /** The kind's reading table. The same object for the processor's whole
      life. */
  virtual const BlockReadingTable& readingTable() const noexcept;

  /** Latest value of reading `index`, in its unit. Any thread; the audio
      thread writes it once per processed block, and a block that is not
      being processed reads as its resting value (0 for a gain reduction). */
  virtual float getReading(int index) const noexcept {
    juce::ignoreUnused(index);
    return 0.0f;
  }

  // ---- Saved state --------------------------------------------------------
  // A registry kind persists as ONE child of its block node:
  //
  //   KindState kind="utility.gain" version=1 name="Gain" [chunk=<binary>]
  //     Param id="gainDb" value=6.0
  //     ...
  //     [what the kind appends: a hosted plugin's Plugin identity]
  //
  // The node is all a build needs to carry a kind it does not know (a preset
  // from a newer build, a plugin that is not installed): it is kept and
  // written back as it came.

  /** What to call a block of this kind when the name belongs to the
      instance rather than the kind (a hosted plugin's own name). Empty: the
      registry's name for the kind. */
  virtual juce::String displayName() const { return {}; }

  /** False for a kind whose chunk already holds every parameter (a hosted
      plugin): its node then carries no Param children, which would only
      repeat the chunk in every undo snapshot. */
  virtual bool savesParameterValues() const noexcept { return true; }

  /** Children a kind adds to its node beyond the parameters and the chunk
      (a hosted plugin's identity). */
  virtual void appendKindState(juce::ValueTree& kindState) const {
    juce::ignoreUnused(kindState);
  }

  /** Version of this kind's state layout, written to the node. Bump it when
      a parameter id or the chunk format changes and translate older nodes in
      setKindState. */
  virtual int kindStateVersion() const noexcept { return 1; }

  /** Opaque state beyond the parameters (a hosted plugin's own chunk).
      Written as the node's `chunk` when not empty. */
  virtual void getStateChunk(juce::MemoryBlock& chunk) const { juce::ignoreUnused(chunk); }
  virtual void setStateChunk(const juce::MemoryBlock& chunk, int version) {
    juce::ignoreUnused(chunk, version);
  }

  /** The KindState node for this processor. `displayName` is stored so a
      build without the kind can still label the block. */
  juce::ValueTree getKindState(const juce::String& displayName) const;

  /** Adopt a KindState node: every parameter back to its default, then the
      node's values by id (ids the kind lacks are ignored, values are
      clamped), then the chunk. Starting from the defaults makes the result
      depend on the node alone: a processor reused across a restore ends up
      exactly where a freshly built one would, also for a node that lacks an
      id (one saved before the parameter existed). On a live processor call
      it under chainMutex, so the audio thread never runs on the defaults in
      between. */
  virtual void setKindState(const juce::ValueTree& kindState);

  // ---- What the chain calls -----------------------------------------------

  /** prepare() plus the base-rate island when the kind asks for one. */
  void prepareForChain(const BlockPrepareSpec& spec);
  /** prepareForInstall() plus the base-rate island. */
  void prepareForChainInstall(const BlockPrepareSpec& spec);

  /** process() at the rate the kind runs at: directly on the chain-rate
      buffer, or inside the island (decimate, process, interpolate, then
      postProcessAtChainRate). A direct pass when the chain is not
      oversampled. An engine-less processor is never put through the island:
      a pass-through has to stay bit-exact. */
  BlockProcessResult processInChain(juce::AudioBuffer<float>& buffer,
                                    const BlockProcessContext& ctx);

  /** latencySamples() in chain-rate samples. */
  int chainLatencySamples() const noexcept {
    return latencySamples() * (runsAtBaseRate() ? preparedOversampleFactor : 1);
  }

private:
  void prepareIsland(const BlockPrepareSpec& spec);

  // The base-rate island (see ChainOversampler::processBaseRateIsland), only
  // built for kinds that run at the base rate. Zero-cost at factor 1.
  std::unique_ptr<ChainOversampler> baseRateIsland;
  int preparedOversampleFactor{1};
};

/** An engine-less processor for a block of `kind`, so a tone block always
    has one to hold its settings and answer queries while its model loads.
    Null for insert placeholders, which never process, and for registry
    kinds, whose processor is built by the registry (see BlockKinds.h). */
std::unique_ptr<BlockProcessor> makeBlockProcessor(ChainBlockType kind);

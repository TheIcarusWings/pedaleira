#pragma once
#include <atomic>
#include <memory>
#include <thread>
#include <vector>
#include "BlockProcessor.h"
#include "PluginScanner.h"

/**
 * One hosted plugin instance and everything that has to be said to it: the
 * part of a hosted block that outlives any one caller. The block processor
 * below holds it through a shared_ptr, and so does whatever message-thread
 * work is in flight for it (a state refresh, a re-prepare), so the instance
 * can never die under a call. The last reference always hands the object to
 * the message thread to destroy (see HostedPluginBlockProcessor::create).
 *
 * Threads, which is the whole point of this class:
 *  - MESSAGE THREAD ONLY: creation, bus layout, prepareToPlay, reset,
 *    setStateInformation, getStateInformation, parameter text, destruction.
 *    JUCE's VST3 and AU wrappers require it, and several of those calls take
 *    the message-manager lock inside the wrapper, so calling them elsewhere
 *    can deadlock a process whose message thread is busy.
 *  - AUDIO THREAD: process(), which also applies parameter sets that were
 *    made off the message thread.
 *  - ANY THREAD, lock-free: setParameter, getParameter, the latency and tail
 *    reads, and the plugin's own change notifications, which only flip
 *    atomics here and leave the work to serviceOnMessageThread().
 */
class HostedPluginInstance final : private juce::AudioProcessorListener {
public:
  ~HostedPluginInstance() override;

  const HostedPluginIdentity& identity() const noexcept { return pluginIdentity; }
  // The negotiated widths of the plugin's main input and output bus.
  int inputChannels() const noexcept { return inChannels; }
  int outputChannels() const noexcept { return outChannels; }
  const BlockParamTable& parameterTable() const noexcept { return table; }

  // ---- Message thread -----------------------------------------------------

  // Prepare for `sampleRate` with blocks of at most `maxBlockSize`, or reset
  // when that is what the plugin already runs at; latches the latency. Off
  // the message thread nothing is called on the plugin: a domain that moved
  // marks the instance unprepared (process() passes the signal through) and
  // asks for serviceOnMessageThread's owner to prepare it again.
  void prepare(double sampleRate, int maxBlockSize);
  bool isPreparedFor(double sampleRate, int maxBlockSize) const noexcept;

  // Hand the plugin a saved chunk. The cached chunk becomes exactly `chunk`,
  // so a state that is loaded and saved again without an edit in between
  // comes back byte for byte whatever the plugin would have written.
  void applyState(const juce::MemoryBlock& chunk);
  // The chunk as last cached. Safe under the chain lock: it never calls the
  // plugin (see serviceOnMessageThread for where the cache comes from).
  juce::MemoryBlock cachedState() const;
  // False while the plugin has reported a change (or a parameter was set)
  // that the cache has not caught up with.
  bool cacheIsCurrent() const noexcept {
    return !stateDirty.load(std::memory_order_relaxed) &&
           !anyDirty.load(std::memory_order_relaxed);
  }
  bool prepareIsWanted() const noexcept { return prepareWanted.load(); }

  struct ServiceResult {
    // The plugin reports a latency other than the one the chain runs with.
    bool latencyPending{false};
    bool tailChanged{false};
    // prepare() was asked for off the message thread.
    bool needsPrepare{false};
  };
  // Everything the plugin's notifications left for later: parameter sets
  // made while no audio was running, and the cached chunk, re-read from the
  // plugin when it reported a change (or always, with `forceStateRefresh`,
  // for a save). Never called with the chain lock held.
  ServiceResult serviceOnMessageThread(bool forceStateRefresh);

  juce::String parameterText(int index) const;

  // The plugin's own editor, or null when it has none, one is already open,
  // or this is not the message thread. Whoever shows it holds this instance
  // (its shared_ptr) for as long as the editor lives and deletes the editor
  // first: the editor points at the plugin.
  std::unique_ptr<juce::AudioProcessorEditor> createEditor();

  // ---- Any thread ---------------------------------------------------------

  float getParameter(int index) const noexcept;
  void setParameter(int index, float value) noexcept;

  int latchedLatency() const noexcept { return latched.load(std::memory_order_relaxed); }
  bool latencyChangePending() const noexcept {
    return reported.load(std::memory_order_relaxed) != latched.load(std::memory_order_relaxed);
  }
  // Under the chain lock, with the chain muted (BlockProcessor::commitLatency).
  void commitLatency() noexcept { latched.store(reported.load()); }
  double tailSeconds() const noexcept { return tail.load(std::memory_order_relaxed); }

  // ---- Audio thread -------------------------------------------------------

  BlockProcessResult process(juce::AudioBuffer<float>& buffer, const BlockTransport& transport);

  // Goes up whenever any instance in the process has something for the
  // message thread. A processor polls it instead of every instance.
  static juce::uint32 attentionCount() noexcept;

private:
  friend class HostedPluginBlockProcessor;

  HostedPluginInstance(std::unique_ptr<juce::AudioPluginInstance> instance,
                       const HostedPluginIdentity& identity);

  // Stereo in and out, then mono in / stereo out, then mono both ways.
  bool negotiateLayout(juce::String& error);
  void buildParameterTable();
  void captureState();
  void refreshMirrors();
  void refreshTail();
  void flushPendingParameters();
  bool onMessageThread() const noexcept {
    return std::this_thread::get_id() == messageThread;
  }
  static void raiseAttention() noexcept;

  // juce::AudioProcessorListener: called by the plugin on whatever thread it
  // likes, the audio thread included. Atomics only.
  void audioProcessorParameterChanged(juce::AudioProcessor*, int parameterIndex,
                                      float newValue) override;
  void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& details) override;
  void audioProcessorParameterChangeGestureBegin(juce::AudioProcessor*, int) override {}
  void audioProcessorParameterChangeGestureEnd(juce::AudioProcessor*, int) override;

  // The play head the plugin reads inside processBlock, filled from the
  // callback's BlockTransport just before.
  struct PlayHead final : juce::AudioPlayHead {
    juce::Optional<PositionInfo> getPosition() const override { return position; }
    PositionInfo position;
  };

  std::unique_ptr<juce::AudioPluginInstance> plugin;
  const HostedPluginIdentity pluginIdentity;
  // The thread this instance was made on. MessageManager's own check takes a
  // mutex, which setParameter (callable from the audio thread) must not.
  const std::thread::id messageThread;

  // Parameters: the exposed table, the plugin parameter behind each row, and
  // per row the last known value (`mirror`) plus a set waiting to be applied
  // (`pending` / `dirty`).
  BlockParamTable table;
  std::vector<juce::AudioProcessorParameter*> exposed;
  std::vector<int> rowOfPluginParameter;
  std::unique_ptr<std::atomic<float>[]> mirror;
  std::unique_ptr<std::atomic<float>[]> pending;
  std::unique_ptr<std::atomic<bool>[]> dirty;
  std::atomic<bool> anyDirty{false};

  // The negotiated main-bus widths and the width of the buffer processBlock
  // takes (every bus of the plugin, at least a stereo pair).
  int inChannels{2};
  int outChannels{2};
  int bufferChannels{2};
  juce::AudioBuffer<float> scratch;
  juce::MidiBuffer midi;
  PlayHead playHead;

  double preparedRate{0.0};
  int preparedBlock{0};
  std::atomic<bool> prepared{false};
  std::atomic<bool> prepareWanted{false};
  std::atomic<bool> failed{false};

  // Latency in samples at the rate the plugin runs at: what the plugin last
  // reported, and what the chain is compensating for.
  std::atomic<int> reported{0};
  std::atomic<int> latched{0};
  std::atomic<double> tail{0.0};

  std::atomic<bool> stateDirty{false};
  juce::CriticalSection stateLock;
  juce::MemoryBlock chunkCache;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HostedPluginInstance)
};

/**
 * plugin: a chain block that is one of the user's own VST3 or Audio Unit
 * effects. The kind is registered like any other, but a block of it is made
 * around a plugin the scanner knows (TONE3000Processor::addPluginBlock, or a
 * saved node naming one), never from the kind id alone.
 *
 * What the framework gets from it:
 *  - It runs at the base rate (48 kHz) whatever the chain's oversampling, in
 *    blocks of at most the base block size: plugins are prepared once and do
 *    not see the factor change.
 *  - Layout: the plugin is asked for stereo in and out, then mono in / stereo
 *    out, then mono in and out. The lane buffer has one or two channels and
 *    is adapted on the way in and out: a mono lane duplicates into a stereo
 *    input, a stereo output folds to (L + R) / 2 when the lane has one
 *    channel, and a mono output is written to every lane channel.
 *    LIMITATION: in the dual-mono input mode the two lane channels are two
 *    independent guitars, and the plugin still runs as ONE ordinary instance:
 *    a stereo plugin treats them as left and right (fine for most effects,
 *    wrong for anything that mixes its channels), and a mono-input plugin
 *    hears their sum.
 *  - Latency goes through the framework's latch: latencySamples() is what
 *    the chain compensates for, the plugin's own report is adopted in
 *    commitLatency(), under the chain mute. A plugin that changes its latency
 *    while running is out of step with the dry path from the moment it
 *    changes its delay until that commit, as in any host.
 *  - Tail: the plugin's, capped at kMaxTailSeconds (an "infinite" tail would
 *    make a host render forever).
 *  - Transport: a play head filled from BlockTransport. MIDI: none, an empty
 *    buffer every block.
 *  - Parameters: the plugin's automatable parameters (without its bypass
 *    switch: the block's own `enabled` is the bypass), at most
 *    kMaxParameters of them, as normalised 0..1 floats under the plugin's
 *    own parameter id (the index where the format has none). Values beyond
 *    the cap still travel in the state chunk.
 *  - State: the plugin's identity plus its opaque chunk. The chunk in the
 *    node is a cache (HostedPluginInstance::cachedState): building the node
 *    never calls into the plugin, so it is safe under the chain lock and on
 *    every undo snapshot.
 *  - A plugin that throws from processBlock reports Failed. A plugin that
 *    crashes takes the process with it; nothing in-process can stop that.
 */
class HostedPluginBlockProcessor final : public BlockProcessor {
public:
  static constexpr const char* kKindId = "plugin";
  // Rows a block lists and saves by id. Large enough for real effects, small
  // enough that a plugin exposing thousands of MIDI CC parameters does not
  // swamp every block listing.
  static constexpr int kMaxParameters = 256;
  static constexpr double kMaxTailSeconds = kMaxBlockTailSeconds;

  /** Wrap a freshly created instance: negotiate the layout, apply `chunk`
      (when the block was saved with one), read the parameters. Not prepared
      yet; prepareForChain does that. Null with `error` set when no usable
      layout exists. Message thread. */
  static std::unique_ptr<HostedPluginBlockProcessor>
  create(std::unique_ptr<juce::AudioPluginInstance> instance,
         const juce::PluginDescription& description, const juce::MemoryBlock* chunk,
         juce::String& error);

  /** The hosted processor behind `processor`, or null for any other kind. */
  static HostedPluginBlockProcessor* from(BlockProcessor* processor) noexcept;
  static const HostedPluginBlockProcessor* from(const BlockProcessor* processor) noexcept {
    return from(const_cast<BlockProcessor*>(processor));
  }

  /** The KindState node of a block that hosts `identity` and has no state of
      its own yet (a plugin just added). */
  static juce::ValueTree makeKindState(const HostedPluginIdentity& identity);
  /** The identity a KindState node names. */
  static HostedPluginIdentity identityOf(const juce::ValueTree& kindState);

  const std::shared_ptr<HostedPluginInstance>& instance() const noexcept { return hosted; }
  const HostedPluginIdentity& identity() const noexcept { return hosted->identity(); }

  ChainBlockType kind() const noexcept override { return ChainBlockType::KIND; }
  const char* kindId() const noexcept override { return kKindId; }
  bool hasEngine() const noexcept override { return true; }
  bool runsAtBaseRate() const noexcept override { return true; }

  void prepare(const BlockPrepareSpec& spec) override;
  void prepareForInstall(const BlockPrepareSpec& spec) override;
  BlockProcessResult process(juce::AudioBuffer<float>& buffer,
                             const BlockProcessContext& ctx) override;

  int latencySamples() const noexcept override { return hosted->latchedLatency(); }
  bool latencyChangePending() const noexcept override { return hosted->latencyChangePending(); }
  void commitLatency() override { hosted->commitLatency(); }
  double tailSeconds() const noexcept override { return hosted->tailSeconds(); }

  // The model-based kinds' typed state means nothing here.
  void getState(BlockProcessorState&) const override {}
  void setState(const BlockProcessorState&) override {}

  const BlockParamTable& parameterTable() const noexcept override {
    return hosted->parameterTable();
  }
  float getParameter(int index) const noexcept override { return hosted->getParameter(index); }
  void setParameter(int index, float value) noexcept override {
    hosted->setParameter(index, value);
  }
  juce::String parameterText(int index) const override { return hosted->parameterText(index); }

  juce::String displayName() const override { return hosted->identity().name; }
  bool savesParameterValues() const noexcept override { return false; }
  void appendKindState(juce::ValueTree& kindState) const override;
  void getStateChunk(juce::MemoryBlock& chunk) const override { chunk = hosted->cachedState(); }
  /** Adopt a saved node on the live instance: the chunk goes to the plugin.
      Message thread only (elsewhere it does nothing; the caller rebuilds the
      block instead, see TONE3000Processor::applyKindState). */
  void setKindState(const juce::ValueTree& kindState) override;

private:
  explicit HostedPluginBlockProcessor(std::shared_ptr<HostedPluginInstance> instance);

  std::shared_ptr<HostedPluginInstance> hosted;
};

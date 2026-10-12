#pragma once
#include <atomic>
#include <memory>
#include "BlockProcessor.h"

/**
 * Base for a registry kind that is "DSP plus a parameter table": it holds the
 * values, answers the parameter and state API, and hands the subclass the
 * rate it actually runs at, so an effect is its prepare, its process and a
 * table.
 *
 * Values live in one atomic per parameter. setParameter (any thread) clamps
 * and stores; the audio thread reads with param(). There is no smoothing
 * here: what needs a ramp is the effect's business (it knows which changes
 * are audible), and it smooths toward param() inside its own process.
 *
 * Runs at the base rate unless the subclass says otherwise: effects are
 * tuned in Hz and milliseconds and should not cost more because the amp is
 * oversampled. A kind that is exact at any rate (a plain gain) opts out and
 * skips the rate change.
 */
class ParameterisedBlockProcessor : public BlockProcessor {
public:
  /** `kindId`, `table` and `readings` must outlive the processor (a string
      literal and the kind's static tables). */
  ParameterisedBlockProcessor(const char* kindId, const BlockParamTable& table);
  ParameterisedBlockProcessor(const char* kindId, const BlockParamTable& table,
                              const BlockReadingTable& readings);

  ChainBlockType kind() const noexcept final { return ChainBlockType::KIND; }
  const char* kindId() const noexcept final { return id; }
  // Built ready to run: there is no model to wait for.
  bool hasEngine() const noexcept override { return true; }
  bool runsAtBaseRate() const noexcept override { return true; }

  const BlockParamTable& parameterTable() const noexcept final { return table; }
  float getParameter(int index) const noexcept final;
  void setParameter(int index, float value) noexcept final;

  // The model-based kinds' typed state means nothing here.
  void getState(BlockProcessorState&) const final {}
  void setState(const BlockProcessorState&) final {}

  void prepare(const BlockPrepareSpec& spec) final;
  // The off-lock build already prepared the effect; under the chain lock
  // only a domain that moved since is worth preparing again.
  void prepareForInstall(const BlockPrepareSpec& spec) final;
  BlockProcessResult process(juce::AudioBuffer<float>& buffer,
                             const BlockProcessContext& ctx) final;

  // Tail spillover is declared by the table: a kind has the capability when
  // its table carries spilloverParam() (BlockKinds.h), and that parameter is
  // the block's switch.
  bool supportsSpillover() const noexcept final { return spilloverIndex >= 0; }
  bool spilloverEnabled() const noexcept final {
    return spilloverIndex >= 0 && param(spilloverIndex) >= 0.5f;
  }
  void reset() noexcept final;

  const BlockReadingTable& readingTable() const noexcept final { return readings; }
  float getReading(int index) const noexcept final;

protected:
  /** What the effect runs at: the base rate and base block size, or the
      chain's when it opted out of the base rate. */
  struct EffectSpec {
    double sampleRate{48000.0};
    int maxBlockSize{0};
    int numChannels{2};
  };

  /** Size buffers and reset state. Not on the audio thread. */
  virtual void prepareEffect(const EffectSpec& spec) = 0;

  /** The effect, in place on 1 or 2 channels. No allocation, locking or
      logging. An exception takes the block out of processing (see
      BlockProcessResult::Failed). */
  virtual void processEffect(juce::AudioBuffer<float>& buffer,
                             const BlockProcessContext& ctx) = 0;

  /** Clear the audio state prepareEffect set up (delay lines, filters,
      envelopes, LFO phase), keeping buffers and parameters. Called on the
      audio thread when the block stops being processed, and worth calling at
      the end of prepareEffect too. No allocation. Stateless effects need not
      override it. */
  virtual void resetEffect() noexcept {}

  /** Audio-thread read of a parameter by table index. */
  float param(int index) const noexcept {
    return values[static_cast<size_t>(index)].load(std::memory_order_relaxed);
  }

  /** Audio thread: publish reading `index` (clamped to its spec). Once per
      processEffect is plenty; a meter polls far slower than blocks come.
      Readings go back to their minimum when the block is reset. */
  void setReading(int index, float value) noexcept;

private:
  EffectSpec effectSpecFor(const BlockPrepareSpec& spec) const noexcept;

  const char* id;
  const BlockParamTable& table;
  // Index of the `spillover` parameter, -1 for a kind without the capability.
  int spilloverIndex{-1};
  // What prepareEffect last ran with; maxBlockSize 0 = never prepared.
  EffectSpec preparedSpec;
  std::unique_ptr<std::atomic<float>[]> values;
  const BlockReadingTable& readings;
  std::unique_ptr<std::atomic<float>[]> readingValues;
};

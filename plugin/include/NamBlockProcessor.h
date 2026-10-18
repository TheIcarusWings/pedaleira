#pragma once
#include <memory>
#include "BlockProcessor.h"
#include "NamEngine.h"

/**
 * A NAM capture as a chain block: the engine (fed at the chain rate; it runs
 * the model at the model's own rate, see NamEngine.h), input calibration, and the
 * post-model gain that is either a calibrated hand-off to the next amp or
 * loudness normalization.
 */
class NamBlockProcessor final : public BlockProcessor {
public:
  /** No engine: the block's model is still loading. */
  NamBlockProcessor() = default;

  /** Takes an engine the loader has already prepared for
      `preparedBlockSize` chain-rate frames. */
  NamBlockProcessor(std::unique_ptr<NamEngine> engine, int preparedBlockSize);

  ChainBlockType kind() const noexcept override { return ChainBlockType::NAM; }
  const char* kindId() const noexcept override { return "nam"; }
  bool hasEngine() const noexcept override { return namEngine != nullptr; }
  float costSeed() const noexcept override;
  /** The engine's rate bridge: 0 for a capture that runs at the chain rate
      or a power-of-two step from it, the resampler's delay otherwise (see
      NamEngine.h). Fixed for an engine, so there is nothing to latch. */
  int latencySamples() const noexcept override {
    return namEngine != nullptr ? namEngine->latencySamples() : 0;
  }
  void prepare(const BlockPrepareSpec& spec) override;
  void prepareForInstall(const BlockPrepareSpec& spec) override;
  BlockProcessResult process(juce::AudioBuffer<float>& buffer,
                             const BlockProcessContext& ctx) override;
  void getState(BlockProcessorState& state) const override;
  void setState(const BlockProcessorState& state) override;

  /** The loaded engine, or null while the model is still loading. Its phase
      and voice counts are baked in at construction (the processor rebuilds
      the block when either requirement moves), and it carries the capture's
      calibration metadata for the chain-state JSON. */
  NamEngine* engine() const noexcept { return namEngine.get(); }

  /** The normalize toggle on its own: setState would also re-assert the
      size on the engine, and a retier is not something a toggle should
      trigger. */
  void setNormalizeEnabled(bool enabled) noexcept { normalizeEnabled = enabled; }

private:
  std::unique_ptr<NamEngine> namEngine;
  // The chain-domain block size the engine was last prepared for. A build
  // can race prepareToPlay at startup (restore-time loads run first), so
  // prepareForInstall re-prepares when this is smaller than the live size.
  int preparedBlockSize{0};
  juce::LinearSmoothedValue<float> normalizationSmoother;

  // Per-block loudness normalization toggle (off = the capture's true level,
  // which is real information). On by default; part of the chain state so
  // presets carry their own gain staging. The UI exposes it as an optional
  // (=) header control behind an advanced preference.
  bool normalizeEnabled{true};

  // Per-block A2 size, stored in NAM's own slimmable-size domain (0..1;
  // 0.0 = lite, 1.0 = full, and the tier boundary belongs to the tier above,
  // so 0.5 already selects full). The value feeds
  // NamEngine::setSlimmableSize verbatim. New blocks start at the
  // machine-wide default (TONE3000Processor::setNamSlimSizeDefault) and
  // setBlockSlimSize retiers the loaded engine in place.
  double slimSize{0.0};
};

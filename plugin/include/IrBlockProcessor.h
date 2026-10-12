#pragma once
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <memory>
#include "BlockProcessor.h"
#include "ChainDomain.h"

// IR convolver block size cap. juce::dsp::Convolution's zero-latency engines
// size their FFT partition from ProcessSpec::maximumBlockSize and run a full
// forward + inverse FFT of that size on *every* process() call, however few
// samples it carries. Preparing from the host's promised maximum ties IR CPU
// to a number unrelated to the real callback size: Ardour advertises 8192 to
// every LV2 plugin whatever buffer it actually runs, and one cab IR at
// 64-sample callbacks then costs ~90% of a core instead of ~2%. So the
// convolver's prepared block size is the host's base block *capped* at this
// (irConvolverBlockSizeFor), and the RT path never feeds it more per call
// (processConvolverInChunks). Hosts at or below the cap get exactly the
// partition they always did, so nothing changes for them; hosts above it
// (or over-promising ones) get a 256 partition and chunked input instead
// of an oversized FFT per callback.
constexpr int kIrConvolverMaxBlockSize = 256;

// The block size convolvers are prepared with for a chain whose base-rate
// block is `chainBaseBlockSize` frames (TONE3000Processor::chainBaseBlockSize).
inline int irConvolverBlockSizeFor(int chainBaseBlockSize) noexcept {
  return std::min(std::max(chainBaseBlockSize, 1), kIrConvolverMaxBlockSize);
}

// Feed `block` to a convolver prepared via irConvolverBlockSizeFor in pieces
// of at most kIrConvolverMaxBlockSize frames. When the base block is at or
// below the cap this is a single call (the island hands at most the base
// block per callback); above it, the convolver was prepared at the cap and
// the loop keeps every call within what it was prepared for.
inline void processConvolverInChunks(juce::dsp::Convolution& convolver,
                                     const juce::dsp::AudioBlock<float>& block) {
  const size_t numSamples = block.getNumSamples();
  constexpr size_t chunkSize = static_cast<size_t>(kIrConvolverMaxBlockSize);
  for (size_t start = 0; start < numSamples; start += chunkSize) {
    auto chunk = block.getSubBlock(start, std::min(chunkSize, numSamples - start));
    convolver.process(juce::dsp::ProcessContextReplacing<float>(chunk));
  }
}

/**
 * An impulse response as a chain block: zero-latency convolution at the base
 * rate (the shared island, see BlockProcessor::runsAtBaseRate), plus
 * unit-energy normalization at the chain rate.
 */
class IrBlockProcessor final : public BlockProcessor {
public:
  /** What the loader builds off-thread for one IR file. */
  struct Engine {
    // mono: IR channel 0 loaded with Stereo::no; applies the same (left)
    //   kernel to every audio channel. Always present for a loaded IR; used
    //   as the mono fallback.
    // stereo: IR loaded with Stereo::yes; audio ch0 ⊗ IR ch0, audio ch1 ⊗
    //   IR ch1. Only created when the IR file actually has >= 2 channels
    //   (true stereo IR).
    // The convolution engine is picked at load time by IR length: cab IRs
    // use JUCE's uniform zero-latency engine, reverb-length IRs the
    // two-stage non-uniform engine (also zero latency); see
    // prepareBlockModelOffThread.
    std::unique_ptr<juce::dsp::Convolution> mono;
    std::unique_ptr<juce::dsp::Convolution> stereo;
    int numChannels{1};  // channels in the loaded IR file (1 or 2)
    // Loaded IR length in base-rate samples (post trim + resample, read off
    // the built engine). Feeds tailSeconds so hosts render real reverb
    // tails.
    int lengthBaseSamples{0};
    // Kernel length vs the short/long cutoff. The block's classification
    // (isLong) is settled at install, where the tone's gear can override
    // this; see irIsLongFor in ProcessorModelLoader.cpp.
    bool isLongByLength{false};
    float normalizationGainLinear{1.0f};
    // The block size the convolvers were prepared with
    // (irConvolverBlockSizeFor at build time). prepareForInstall re-prepares
    // them when the live value differs: a restore-time load sees the 4096
    // default before prepareToPlay reports the real host block, and would
    // otherwise sit at the cap on a host that really runs 64.
    int preparedBlockSize{0};
  };

  /** No convolver: the block's IR is still loading. */
  IrBlockProcessor() = default;
  explicit IrBlockProcessor(Engine engine);

  ChainBlockType kind() const noexcept override { return ChainBlockType::IR; }
  const char* kindId() const noexcept override { return "ir"; }
  bool hasEngine() const noexcept override { return ir.mono != nullptr; }
  // Convolution always runs at kChainBaseSampleRate: linear processing gains
  // nothing from oversampling, and its CPU scales ~quadratically with the
  // rate.
  bool runsAtBaseRate() const noexcept override { return true; }
  void prepare(const BlockPrepareSpec& spec) override;
  void prepareForInstall(const BlockPrepareSpec& spec) override;
  BlockProcessResult process(juce::AudioBuffer<float>& buffer,
                             const BlockProcessContext& ctx) override;
  void postProcessAtChainRate(juce::AudioBuffer<float>& buffer,
                              const BlockProcessContext& ctx) override;
  double tailSeconds() const noexcept override;
  // Nothing of its own to persist: an IR file's level is always normalized
  // and its classification is recomputed on every load.
  void getState(BlockProcessorState&) const override {}
  void setState(const BlockProcessorState&) override {}
  float wetPadDb() const noexcept override { return irIsLong ? 0.0f : -18.0f; }

  int numChannels() const noexcept { return ir.numChannels; }
  int lengthBaseSamples() const noexcept { return ir.lengthBaseSamples; }
  // A partitioned convolution costs a little per block plus a share that
  // grows with the kernel: about 0.3% of a core for a cab, a few percent
  // for a long reverb.
  float costSeed() const noexcept override {
    return 0.003f + 0.02f * static_cast<float>(ir.lengthBaseSamples) / 48000.0f;
  }
  bool isLongByLength() const noexcept { return ir.isLongByLength; }

  // The single cab-like / reverb-like classification. Short = cab-like:
  // -18 dB output pad (spectrally concentrated kernels play back hot at
  // unit energy), 100% default mix. Long = reverb-like: no pad (diffuse
  // kernels sit at ≈ dry level at unit energy), 50% default mix. Decided by
  // the tone's gear when it is unambiguous ("cab" / "space"), else by the
  // kernel length against the cutoff in ProcessorModelLoader.cpp (see
  // irIsLongFor there). Runtime-only: recomputed on every load. Shipped to
  // the UI as `irLong`.
  bool isLong() const noexcept { return irIsLong; }
  void setIsLong(bool isLong) noexcept { irIsLong = isLong; }

private:
  Engine ir;
  bool irIsLong{false};
  juce::LinearSmoothedValue<float> normalizationSmoother;
};

#include "IrBlockProcessor.h"

IrBlockProcessor::IrBlockProcessor(Engine engine) : ir(std::move(engine)) {}

void IrBlockProcessor::prepare(const BlockPrepareSpec& spec) {
  if (ir.mono != nullptr) {
    // Convolvers always run at the base rate behind the block's island, so
    // their spec only tracks the base block size (capped, see
    // irConvolverBlockSizeFor), never the oversampling factor.
    ir.preparedBlockSize = irConvolverBlockSizeFor(spec.baseBlockSize);
    const juce::dsp::ProcessSpec convolverSpec{kChainBaseSampleRate,
                                               static_cast<juce::uint32>(ir.preparedBlockSize),
                                               static_cast<juce::uint32>(spec.numChannels)};
    ir.mono->prepare(convolverSpec);
    if (ir.stereo != nullptr)
      ir.stereo->prepare(convolverSpec);

    // Reset normalization smoother to current gain to prevent jumps on re-prepare
    normalizationSmoother.reset(spec.chainSampleRate, 0.05f);
    normalizationSmoother.setCurrentAndTargetValue(ir.normalizationGainLinear);
  }
}

void IrBlockProcessor::prepareForInstall(const BlockPrepareSpec& spec) {
  // Convolvers track the capped base block (irConvolverBlockSizeFor). Unlike
  // NAM, a *shrink* matters too: a load that ran before prepareToPlay saw the
  // 4096 default and built a 256-partition engine, which on a host that then
  // reports 64 would cost more per callback than it needs to. Re-prepare on
  // any difference so the convolver always matches the live host block.
  // (prepare() rebuilds the FFT engine under chainMutex, heavier for
  // reverb-length IRs, but this only fires on that startup race, when audio
  // has barely started.)
  const int requiredBlockSize = irConvolverBlockSizeFor(spec.baseBlockSize);
  if (ir.mono != nullptr && ir.preparedBlockSize != requiredBlockSize) {
    juce::Logger::writeToLog("[ModelLoader] Host block drifted during IR prepare (convolver " +
                             juce::String(ir.preparedBlockSize) + " -> " +
                             juce::String(requiredBlockSize) + "); re-preparing");
    const juce::dsp::ProcessSpec convolverSpec{kChainBaseSampleRate,
                                               static_cast<juce::uint32>(requiredBlockSize),
                                               static_cast<juce::uint32>(spec.numChannels)};
    ir.mono->prepare(convolverSpec);  // keeps the loaded impulse
    if (ir.stereo != nullptr)
      ir.stereo->prepare(convolverSpec);
    ir.preparedBlockSize = requiredBlockSize;
  }

  normalizationSmoother.reset(spec.chainSampleRate, 0.05f);
  normalizationSmoother.setCurrentAndTargetValue(ir.normalizationGainLinear);
}

double IrBlockProcessor::tailSeconds() const noexcept {
  // IRs always convolve at the base rate, so the kernel length is over
  // kChainBaseSampleRate regardless of oversampling.
  return ir.mono != nullptr ? ir.lengthBaseSamples / kChainBaseSampleRate : 0.0;
}

BlockProcessResult IrBlockProcessor::process(juce::AudioBuffer<float>& buffer,
                                             const BlockProcessContext& ctx) {
  // No convolver yet: the wet path is the input as it stands.
  if (ir.mono == nullptr)
    return BlockProcessResult::Processed;

  // `buffer` holds base-rate frames (runsAtBaseRate): when the chain is
  // oversampled the shared island decimates the wet path, hands the
  // convolver these frames, and interpolates back (a direct pass at ×1).
  // Linear processing gains nothing above the base rate; this keeps IR CPU
  // flat across oversampling factors and the IR sound bit-identical to the
  // non-oversampled chain.
  const int numChannels = buffer.getNumChannels();

  try {
    // True-stereo only when: the IR file is stereo, the working buffer is stereo, and no
    // NAM block downstream would collapse the image back to mono. Otherwise apply the IR's
    // left channel to every audio channel (the mono convolver, Stereo::no). Dual mono always
    // takes the mono path: each channel is its own mono chain, and inside a stereo-mode
    // lane (the definition of dual mono) a stereo IR convolves its left channel too.
    const bool noNamAfter = (ctx.blockIndex > ctx.nam.lastNamIndex);
    const bool useStereoIr = ir.numChannels > 1 && numChannels > 1 && noNamAfter &&
                             !ctx.dualMono && ir.stereo != nullptr;
    auto& convolver = useStereoIr ? *ir.stereo : *ir.mono;

    juce::dsp::AudioBlock<float> irBlock(buffer.getArrayOfWritePointers(),
                                         static_cast<size_t>(numChannels),
                                         static_cast<size_t>(buffer.getNumSamples()));
    processConvolverInChunks(convolver, irBlock);
  } catch (const std::exception& e) {
    // Not a reason to drop the block: whatever reached the buffer goes on
    // through the shared stages, and the next callback tries again.
    juce::ignoreUnused(e);
    DBG("Error in IR processing: " << e.what());
  }

  return BlockProcessResult::Processed;
}

void IrBlockProcessor::postProcessAtChainRate(juce::AudioBuffer<float>& buffer,
                                              const BlockProcessContext&) {
  const int numSamples = buffer.getNumSamples();
  const int numChannels = buffer.getNumChannels();

  // Unit-energy normalization, always on: an IR file's absolute level
  // is an accident of capture/export (unlike a NAM capture's, which is
  // real information, hence NAM's normalize toggle). Attenuation-only;
  // smoother is prepared in prepare / install, only the target moves on
  // the RT path. On the chain-rate buffer, after the island, where it has
  // always run.
  normalizationSmoother.setTargetValue(juce::jlimit(0.0f, 1.0f, ir.normalizationGainLinear));
  for (int i = 0; i < numSamples; ++i) {
    const float g = normalizationSmoother.getNextValue();
    for (int ch = 0; ch < numChannels; ++ch) {
      buffer.getWritePointer(ch)[i] *= g;
    }
  }
}

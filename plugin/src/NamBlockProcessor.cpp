#include "NamBlockProcessor.h"
#include <cmath>

NamBlockProcessor::NamBlockProcessor(std::unique_ptr<NamEngine> engine, int preparedBlockSizeIn)
    : namEngine(std::move(engine)), preparedBlockSize(preparedBlockSizeIn) {}

void NamBlockProcessor::prepare(const BlockPrepareSpec& spec) {
  if (namEngine != nullptr) {
    namEngine->prepare(spec.maxBlockSize);
    preparedBlockSize = spec.maxBlockSize;
  }

  // The RT path only ever calls setTargetValue on the smoother; reset()
  // belongs here and in the install path, never per block.
  normalizationSmoother.reset(spec.chainSampleRate, 0.05f);
  normalizationSmoother.setCurrentAndTargetValue(1.0f);
}

void NamBlockProcessor::prepareForInstall(const BlockPrepareSpec& spec) {
  // A restore-time prepare can race prepareToPlay: the engine may have been
  // sized off a stale (or defaulted) host config, and prepareChain can't have
  // covered it (the engine wasn't on the block yet). Re-prepare before it
  // goes live; feeding an engine more frames than it was prepared for is what
  // used to silently kill blocks on relaunch ("Processing failed").
  if (namEngine != nullptr && preparedBlockSize < spec.maxBlockSize) {
    juce::Logger::writeToLog("[ModelLoader] Chain domain drifted during prepare (block " +
                             juce::String(preparedBlockSize) + " -> " +
                             juce::String(spec.maxBlockSize) + "); re-preparing");
    namEngine->prepare(spec.maxBlockSize);
    preparedBlockSize = spec.maxBlockSize;
  }

  normalizationSmoother.reset(spec.chainSampleRate, 0.05f);
  normalizationSmoother.setCurrentAndTargetValue(1.0f);
}

void NamBlockProcessor::getState(BlockProcessorState& state) const {
  state.normalize = normalizeEnabled;
  state.slimSize = slimSize;
}

void NamBlockProcessor::setState(const BlockProcessorState& state) {
  normalizeEnabled = state.normalize;
  slimSize = state.slimSize;
  // Asserted on the engine every time, changed or not: the callers are the
  // restore, size-edit and install paths, and an engine that was built or
  // staged against another size must end up on this one.
  if (namEngine != nullptr)
    namEngine->setSlimmableSize(slimSize);
}

BlockProcessResult NamBlockProcessor::process(juce::AudioBuffer<float>& buffer,
                                              const BlockProcessContext& ctx) {
  const int numSamples = buffer.getNumSamples();
  const int numChannels = buffer.getNumChannels();

  try {
    if (namEngine == nullptr)
      return BlockProcessResult::Idle;

    // Calculate additional calibration gain for this specific NAM block
    float calibrationGain = 1.0f;
    if (ctx.nam.calibrateInput && namEngine->hasInputLevel()) {
      const double modelInputLevel = namEngine->getInputLevel();
      const double calibrationAdjustmentDb = ctx.nam.inputCalibrationLevel - modelInputLevel;
      calibrationGain = juce::Decibels::decibelsToGain(static_cast<float>(calibrationAdjustmentDb));
    }

    // Apply calibration gain to the buffer
    if (calibrationGain != 1.0f) {
      buffer.applyGain(0, 0, numSamples, calibrationGain);
      if (numChannels > 1) {
        buffer.applyGain(1, 0, numSamples, calibrationGain);
      }
    }

    // Process with the NAM engine: channel 0 through the model, fanned
    // out to channel 1, or in dual mono (a two-voice engine) each channel
    // through its own voice. With multi-core on, the engine forks its
    // voice × phase instances across the worker pool (resolved per
    // callback); nested inside a lane fork this is the pool's supported
    // one-deep nesting (dual mono never nests: it only runs in mono chain
    // mode, which has no lane fork). Null = instances run serially on this
    // thread.
    namEngine->process(buffer, ctx.nam.workerPool, ctx.dualMono);

    // Post-model gain: calibrated hand-off OR loudness normalization,
    // never both; they have contradictory goals (reproduce the capture
    // rig's true level vs. make every capture equally loud).
    //
    // Calibrated hand-off applies only mid-chain (another NAM downstream)
    // when calibration is on and the model carries output_level_dbu.
    // Gain = model output dBu - user's calibration dBu converts the
    // model's output back into the user's analog reference frame; the
    // downstream NAM's input calibration then converts from that frame
    // into its own model's, so the user's setting cancels and the
    // hand-off carries exactly the level of physically plugging device A
    // into device B. Normalizing mid-chain instead would wreck the drive
    // level into the next model that calibration exists to preserve.
    //
    // The last NAM block deliberately stays on normalization: calibrated
    // output at the chain's end would swing overall volume with each
    // capture's metadata (a cranked-amp model can sit 20+ dB hot). Net
    // effect: calibration governs drive/character, normalization governs
    // listening level. No clamp on the hand-off gain (it's a physical
    // level difference, not a guess), only a metadata sanity check that
    // falls back to normalization when the value is junk.
    // The smoother was prepared off the RT path (prepare / install);
    // here we only ever move its target.
    const float targetLufs = ctx.nam.targetLoudness;  // use live target
    float blockGain = 1.0f;
    bool calibratedHandOff = false;
    if (ctx.nam.calibrateInput && ctx.blockIndex < ctx.nam.lastNamIndex && namEngine->hasOutputLevel()) {
      const float modelOutputLevel = static_cast<float>(namEngine->getOutputLevel());
      if (std::isfinite(modelOutputLevel) && modelOutputLevel >= -60.0f &&
          modelOutputLevel <= 60.0f) {
        blockGain = juce::Decibels::decibelsToGain(modelOutputLevel - ctx.nam.inputCalibrationLevel);
        calibratedHandOff = true;
      }
    }
    if (!calibratedHandOff && normalizeEnabled) {
      float modelLoudnessDb = targetLufs;  // Default fallback
      if (namEngine->hasLoudness()) {
        modelLoudnessDb = static_cast<float>(namEngine->getLoudness());
      }
      if (!std::isfinite(modelLoudnessDb) || modelLoudnessDb < -100.0f || modelLoudnessDb > 0.0f) {
        modelLoudnessDb = targetLufs;
      }
      const float gainAdjustmentDb = juce::jlimit(-12.0f, 12.0f, targetLufs - modelLoudnessDb);
      blockGain = juce::Decibels::decibelsToGain(gainAdjustmentDb);
    }
    normalizationSmoother.setTargetValue(blockGain);

    // Apply per-block normalization / hand-off gain to the buffer
    auto* left = buffer.getWritePointer(0);
    auto* right = numChannels > 1 ? buffer.getWritePointer(1) : nullptr;

    for (int i = 0; i < numSamples; ++i) {
      const float g = normalizationSmoother.getNextValue();
      left[i] *= g;
      if (right) right[i] *= g;
    }
  } catch (const std::exception&) {
    // RT-safe failure path: string building/logging can't run here, so the
    // caller disables the block (stops it re-throwing every callback) and
    // flags it for the message thread.
    return BlockProcessResult::Failed;
  }

  return BlockProcessResult::Processed;
}

// What the engine's own prewarm cost per second of audio (see
// NamEngine::prepare), or a flat guess for a model that does not prewarm.
float NamBlockProcessor::costSeed() const noexcept {
  if (namEngine == nullptr)
    return 0.0f;
  const double measured = namEngine->prewarmCostSeed();
  return measured > 0.0 ? static_cast<float>(measured) : 0.05f;
}

#include "GainBlockProcessor.h"

const BlockParamTable& GainBlockProcessor::parameterSpecs() {
  static const BlockParamTable table = {
      BlockParamSpec::floatParam("gainDb", "Gain", "dB", -24.0f, 24.0f, 0.0f),
      BlockParamSpec::floatParam("pan", "Pan", "", -1.0f, 1.0f, 0.0f),
      BlockParamSpec::boolParam("phaseInvert", "Phase Invert", false),
  };
  return table;
}

GainBlockProcessor::GainBlockProcessor()
    : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

void GainBlockProcessor::targetGains(bool stereo, float& left, float& right) const noexcept {
  float gain = juce::Decibels::decibelsToGain(param(kGainDb), -100.0f);
  if (param(kPhaseInvert) >= 0.5f)
    gain = -gain;
  // A single channel has nothing to balance against (a stereo-mode lane):
  // pan only acts on a two-channel buffer.
  const float pan = stereo ? param(kPan) : 0.0f;
  left = gain * (pan > 0.0f ? 1.0f - pan : 1.0f);
  right = gain * (pan < 0.0f ? 1.0f + pan : 1.0f);
}

void GainBlockProcessor::prepareEffect(const EffectSpec& spec) {
  // 20 ms: long enough that a knob move or a polarity flip does not click,
  // short enough to feel immediate. The first block snaps to the current
  // values (see snapOnNextBlock), so a restored block does not glide in
  // from unity, nor a mono lane from the gains a stereo one would have.
  gainLeft.reset(spec.sampleRate, 0.02);
  gainRight.reset(spec.sampleRate, 0.02);
  snapOnNextBlock = true;
}

void GainBlockProcessor::processEffect(juce::AudioBuffer<float>& buffer,
                                       const BlockProcessContext&) {
  const int numSamples = buffer.getNumSamples();
  const bool stereo = buffer.getNumChannels() > 1;

  float left = 1.0f, right = 1.0f;
  targetGains(stereo, left, right);
  if (snapOnNextBlock || stereo != lastStereo) {
    gainLeft.setCurrentAndTargetValue(left);
    gainRight.setCurrentAndTargetValue(right);
    snapOnNextBlock = false;
    lastStereo = stereo;
  } else {
    gainLeft.setTargetValue(left);
    gainRight.setTargetValue(right);
  }

  // Settled at unity: leave the samples alone, so the default block is a
  // bit-exact pass-through with nothing to compute.
  const bool leftIdle = !gainLeft.isSmoothing() && gainLeft.getCurrentValue() == 1.0f;
  const bool rightIdle = !gainRight.isSmoothing() && gainRight.getCurrentValue() == 1.0f;

  if (!leftIdle) {
    auto* x = buffer.getWritePointer(0);
    for (int i = 0; i < numSamples; ++i)
      x[i] *= gainLeft.getNextValue();
  }
  if (stereo) {
    if (!rightIdle) {
      auto* x = buffer.getWritePointer(1);
      for (int i = 0; i < numSamples; ++i)
        x[i] *= gainRight.getNextValue();
    }
  } else {
    // Keep the unused side in step, so a later change of channel count does
    // not start from a stale ramp.
    gainRight.skip(numSamples);
  }
}

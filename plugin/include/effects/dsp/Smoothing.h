#pragma once
#include <juce_audio_basics/juce_audio_basics.h>

namespace fx {

/**
 * A parameter value ramped on the audio thread, for anything whose jump
 * would click (gains, mixes, cutoffs, delay times).
 *
 * ParameterisedBlockProcessor holds the values and deliberately does no
 * smoothing: param(i) is the raw target. This is the one pattern effects use
 * on top of it, a juce::LinearSmoothedValue plus the rule utility.gain
 * established: the first target after a prepare or a reset is taken at once,
 * so a restored or re-enabled block starts where its parameters are instead
 * of gliding in from a default.
 *
 *   prepareEffect:  cutoff.prepare(spec.sampleRate, 0.02);
 *   processEffect:  cutoff.setTarget(param(kCutoffHz));   // once per block
 *                   for each sample: use cutoff.next();
 *
 * Audio thread only after prepare. No allocation anywhere.
 */
class SmoothedParam {
public:
  /** Ramp length in seconds at `sampleRate`; the next setTarget snaps. */
  void prepare(double sampleRate, double rampSeconds = 0.02) noexcept {
    value.reset(sampleRate, rampSeconds);
    snapNext = true;
  }

  /** The next setTarget jumps to its value instead of ramping. Call from
      resetEffect, so a block switched back on does not glide from where it
      was left. */
  void snapOnNextTarget() noexcept { snapNext = true; }

  /** Where to ramp to. Cheap when the target has not moved, so call it once
      per block with param(i). */
  void setTarget(float target) noexcept {
    if (snapNext) {
      value.setCurrentAndTargetValue(target);
      snapNext = false;
    } else {
      value.setTargetValue(target);
    }
  }

  /** The value for this sample; advances the ramp. */
  float next() noexcept { return value.getNextValue(); }
  /** Advance `numSamples` without reading (a channel that is not there). */
  void skip(int numSamples) noexcept { value.skip(numSamples); }

  float current() const noexcept { return value.getCurrentValue(); }
  float target() const noexcept { return value.getTargetValue(); }
  bool isSmoothing() const noexcept { return value.isSmoothing(); }

private:
  juce::LinearSmoothedValue<float> value;
  bool snapNext{true};
};

}  // namespace fx

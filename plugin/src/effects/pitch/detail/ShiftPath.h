#pragma once
// Private to the pitch family: one shifted signal path, the part that the
// shifter, the transpose and the pitch bend (whammy) kinds have in common.
//
//  - one PitchVoice (one engine per independent signal, see PitchVoice.h);
//  - the interval is glided (a straight ramp in semitones over a few
//    milliseconds) so a stepped change bends instead of jumping, and reaches
//    the engine on the fixed 16-sample grid so the output does not depend on
//    the host block size;
//  - the input is delayed by the block's reported latency on its way past the
//    engine. That delayed copy is what the block plays at exactly 0 semitones
//    (a pure delay, bit for bit, crossfaded to and from the engine, which
//    keeps running either way): the engine's constant attack latency says
//    nothing about a sustained note it is not shifting, and a pure delay is
//    the one setting where the whole signal, not just the attacks, is exactly
//    that late. It is also what a "detune" blend adds to the shifted voice,
//    already lined up with it.
#include "effects/EffectDsp.h"
#include "PitchVoice.h"

namespace pitchfx {

class ShiftPath {
public:
  /** Not on the audio thread. `maxBlockSize` sizes the engine's work buffers,
      `glideSeconds` is how long an interval change takes to bend, and
      `unisonSeconds` the crossfade to and from the delayed dry at 0 st. */
  void prepare(double sampleRate, int maxBlockSize, PitchShift::Window window, int onsetDelay,
               double glideSeconds, double unisonSeconds) {
    voice.prepare(sampleRate, maxBlockSize, window, onsetDelay);
    glide.prepare(sampleRate, glideSeconds);
    unison.prepare(sampleRate, unisonSeconds);
    share.prepare(sampleRate, kShareSeconds);
    // Room for the longest window's latency (the latency never exceeds the
    // window).
    dryLine.prepare(2, PitchShift::windowSamples(PitchShift::Window::ms60, sampleRate) + 16);
    aligned.setSize(2, juce::jmax(1, maxBlockSize), false, true, false);
    // The voice is already run in by its prepare; the rest starts from rest.
    clearState();
  }

  /** The engine holds stale audio: it starts from rest on the next block that
      plays. Cheap. */
  void reset() noexcept {
    voice.markCold();
    clearState();
  }

  /** Shifts `buffer` in place by `semitones` (the target; the glide takes it
      from there). `delay` is the block's reported latency. With
      `dryShare` above 0 the delayed dry signal is blended into the result by
      that share (0.5: half and half); the share is smoothed. The glided interval in use now is
      available from currentSemitones(). */
  void process(juce::AudioBuffer<float>& buffer, bool dualMono, float semitones,
               PitchShift::Window window, int delay, float dryShare = 0.0f) {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), 2);
    if (numChannels < 1 || numSamples < 1)
      return;
    const bool dual = dualMono && numChannels == 2;

    glide.setTarget(semitones);
    unison.setTarget(std::abs(semitones) < kUnisonEpsilon ? 1.0f : 0.0f);
    share.setTarget(dryShare);
    float* channels[2] = {buffer.getWritePointer(0), numChannels > 1 ? buffer.getWritePointer(1) : nullptr};

    // The dry signal as late as the block reports, before the engine
    // overwrites the buffer.
    for (int ch = 0; ch < numChannels; ++ch) {
      float* late = aligned.getWritePointer(ch);
      for (int i = 0; i < numSamples; ++i) {
        late[i] = dryLine.read(ch, delay);
        dryLine.write(ch, channels[ch][i]);
      }
    }

    grid.run(numSamples, [&](int start, int length, bool boundary) {
      if (boundary)
        glide.tick();
      const float laneSemitones[kMaxLanes] = {glide.value(), glide.value()};
      voice.process(channels, numChannels, dual, start, length, laneSemitones, window, delay);
    });

    for (int i = 0; i < numSamples; ++i) {
      const float toDry = unison.next();
      const float blendIn = share.next();
      // This form is exact at both ends (the delayed dry signal, bit for bit,
      // at unison); the other, x + u * (a - x), rounds.
      for (int ch = 0; ch < numChannels; ++ch) {
        const float late = aligned.getReadPointer(ch)[i];
        float out = channels[ch][i] * (1.0f - toDry) + late * toDry;
        if (blendIn > 0.0f)
          out = out * (1.0f - blendIn) + late * blendIn;
        channels[ch][i] = out;
      }
    }
  }

  /** The interval the engine is at now, after the glide. */
  float currentSemitones() const noexcept { return glide.value(); }

private:
  void clearState() noexcept {
    glide.snap();
    unison.snapOnNextTarget();
    share.snapOnNextTarget();
    dryLine.clear();
    grid.reset();
  }

  // Exactly zero is the only unison: a fine setting of a cent is far above it.
  static constexpr float kUnisonEpsilon = 1.0e-4f;
  static constexpr double kShareSeconds = 0.02;

  PitchVoice voice;
  Glide glide;
  ControlGrid grid;
  fx::SmoothedParam unison, share;
  fx::DelayLine dryLine;
  juce::AudioBuffer<float> aligned;
};

}  // namespace pitchfx

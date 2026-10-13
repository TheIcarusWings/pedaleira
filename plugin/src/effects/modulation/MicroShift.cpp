// modulation.doubler "Micro Shift": a doubler made of two pitch-shifted
// voices, one a few cents sharp and one a few cents flat, each behind a short
// delay of its own. Played with the dry signal they thicken a line the way a
// second take would, without the sweep of a chorus.
//
// A voice is a delay line read at a delay that changes at a steady rate: a
// delay that shrinks by e samples per sample raises the pitch by the ratio
// 1 + e, and one that grows lowers it by 1 - e. A steady ramp runs out of
// line, so each voice reads two taps half a window apart, wraps a tap back to
// the far end of the window while its weight is zero, and fades the taps in
// and out with complementary sin^2 weights (they always add to one). At a few
// cents the ramp is slow (12 cents is a wrap every 1.2 s), which is why this
// needs no pitch tracking: it is a self-contained engine, with no relation to
// the pitch family's.
//
// The up voice leans to the right and the down voice to the left as far as
// `spread` says, by gains that always add to one (the sides add back to the
// same two voices, as for the dual chorus).
#include <algorithm>
#include <cmath>
#include "effects/EffectDsp.h"

namespace {

class MicroShiftProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "modulation.doubler";

  enum Param { kDetuneCents = 0, kDelayMs, kSpread, kToneHz };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::floatParam("detuneCents", "Detune", "cents", 0.0f, 50.0f, 12.0f));
      t.push_back(BlockParamSpec::floatParam("delayMs", "Delay", "ms", kMinDelayMs, kMaxDelayMs,
                                             20.0f)
                      .withSkew(0.7f));
      t.push_back(BlockParamSpec::floatParam("spread", "Spread", "%", 0.0f, 100.0f, 100.0f));
      t.push_back(BlockParamSpec::floatParam("toneHz", "Tone", "Hz", 1000.0f, 20000.0f, 12000.0f)
                      .withSkew(0.4f));
      return t;
    }();
    return table;
  }

  MicroShiftProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // Fixed: the longest delay of a voice plus half a window, and the high
  // cut settling.
  double tailSeconds() const noexcept override { return kLongestReadMs * 0.001 + 0.01; }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    channelsPrepared = juce::jlimit(1, kMaxChannels, spec.numChannels);
    windowSamples = static_cast<float>(kWindowMs * 0.001 * sampleRate);
    line.prepare(channelsPrepared, static_cast<int>(std::ceil(kLongestReadMs * 0.001 * sampleRate)) + 4);
    detune.prepare(sampleRate, 0.05);
    delay.prepare(sampleRate, 0.12);
    spread.prepare(sampleRate, 0.05);
    tone.prepare(sampleRate, 0.03);
    resetEffect();
  }

  void resetEffect() noexcept override {
    line.clear();
    for (auto& filter : highCut)
      filter.reset();
    detune.snapOnNextTarget();
    delay.snapOnNextTarget();
    spread.snapOnNextTarget();
    tone.snapOnNextTarget();
    // Each voice starts on one tap, mid-window: at exactly its own delay,
    // with the first wrap a half window away.
    ramp = 0.5f;
    downRamp = 0.5f;
    gridCounter = 0;
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), channelsPrepared);
    const bool stereo = numChannels > 1 && blockHasStereoImage(buffer.getNumChannels(), ctx);

    detune.setTarget(param(kDetuneCents));
    delay.setTarget(param(kDelayMs));
    spread.setTarget(stereo ? param(kSpread) * 0.01f : 0.0f);
    tone.setTarget(param(kToneHz));

    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    const float samplesPerMs = static_cast<float>(sampleRate * 0.001);
    for (int i = 0; i < numSamples; ++i) {
      const float cents = detune.next();
      const float centreSamples = delay.next() * samplesPerMs;
      const float lean = spread.next();
      const float cutoff = tone.next();
      if (gridCounter == 0)
        for (int ch = 0; ch < numChannels; ++ch)
          highCut[ch].setCutoff(cutoff, sampleRate);
      gridCounter = (gridCounter + 1) & (kControlGrid - 1);

      // The pitch ratio of the up voice is 2^(c/1200); a delay that moves
      // by e samples per sample bends the pitch by e. The ramp runs 0 to 1
      // over a window, so it advances by e / window. The down voice moves
      // the other way at 1 - 2^(-c/1200), about the same rate.
      const float upRatio = std::exp2(cents * (1.0f / 1200.0f)) - 1.0f;
      const float downRatio = 1.0f - std::exp2(-cents * (1.0f / 1200.0f));
      const float upStep = upRatio / windowSamples;
      const float downStep = downRatio / windowSamples;

      // Two taps half a window apart, for each voice: their weights and
      // delays are the same on every channel.
      const float phaseUp[2] = {ramp, wrap(ramp + 0.5f)};
      const float phaseDown[2] = {downRamp, wrap(downRamp + 0.5f)};
      const float upCentre = centreSamples * kUpDelayShare;
      const float downCentre = centreSamples * kDownDelayShare;
      const float halfWindow = 0.5f * windowSamples;
      float upWeight[2], upDelay[2], downWeight[2], downDelay[2];
      for (int k = 0; k < 2; ++k) {
        // Up: the delay shrinks across the window. Down: it grows.
        upWeight[k] = tapWeight(phaseUp[k]);
        upDelay[k] = upCentre + halfWindow - windowSamples * phaseUp[k];
        downWeight[k] = tapWeight(phaseDown[k]);
        downDelay[k] = downCentre - halfWindow + windowSamples * phaseDown[k];
      }

      float up[kMaxChannels] = {0.0f, 0.0f};
      float down[kMaxChannels] = {0.0f, 0.0f};
      for (int ch = 0; ch < numChannels; ++ch) {
        for (int k = 0; k < 2; ++k) {
          up[ch] += upWeight[k] * line.readHermite(ch, upDelay[k]);
          down[ch] += downWeight[k] * line.readHermite(ch, downDelay[k]);
        }
        line.write(ch, channels[ch][i]);
      }
      ramp = wrap(ramp + upStep);
      downRamp = wrap(downRamp + downStep);

      const float toRight = 0.5f + 0.5f * lean;
      const float toLeft = 1.0f - toRight;
      const float level = 1.0f / std::sqrt(toRight * toRight + toLeft * toLeft);
      if (stereo) {
        // Left: mostly the down voice. Right: mostly the up voice.
        channels[0][i] = highCut[0].lowpass(level * (toLeft * up[0] + toRight * down[0]));
        channels[1][i] = highCut[1].lowpass(level * (toRight * up[1] + toLeft * down[1]));
      } else {
        for (int ch = 0; ch < numChannels; ++ch)
          channels[ch][i] = highCut[ch].lowpass(0.70710678f * (up[ch] + down[ch]));
      }
    }
  }

private:
  static constexpr int kMaxChannels = 2;
  static constexpr int kControlGrid = 16;
  static constexpr float kMinDelayMs = 10.0f;
  static constexpr float kMaxDelayMs = 40.0f;
  // The window one tap travels before it wraps: the delay of a tap spans
  // this much, centred on the voice's delay.
  static constexpr double kWindowMs = 8.0;
  // The two voices sit at different delays, so they never cancel each other.
  static constexpr float kUpDelayShare = 0.85f;
  static constexpr float kDownDelayShare = 1.15f;
  // The deepest a tap can read: the down voice's longest delay plus half a
  // window.
  static constexpr double kLongestReadMs = 40.0 * 1.15 + 0.5 * kWindowMs;

  static float wrap(float x) noexcept { return x - std::floor(x); }
  // Zero where a tap wraps (phase 0), one half a window later; the two taps
  // of a voice are half a cycle apart, so the weights add to one.
  static float tapWeight(float phase) noexcept {
    const float s = std::sin(juce::MathConstants<float>::pi * phase);
    return s * s;
  }

  fx::DelayLine line;
  fx::OnePole highCut[kMaxChannels];
  fx::SmoothedParam detune;
  fx::SmoothedParam delay;
  fx::SmoothedParam spread;
  fx::SmoothedParam tone;
  float windowSamples{384.0f};
  float ramp{0.5f};
  float downRamp{0.5f};
  double sampleRate{48000.0};
  int channelsPrepared{2};
  int gridCounter{0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(MicroShift) {
  // The doubler sits beside the dry signal: a crossfade, 50% by default.
  addEffectKind<MicroShiftProcessor>(registry, "Micro Shift", BlockCategory::Modulation, 0.5f,
                                     BlockMixLaw::Crossfade);
}

// modulation.dualchorus: two chorus voices, each with its own rate, depth and
// delay, so the two never settle into one repeating pattern. Voice A leans
// to the left and voice B to the right as far as `spread` says. The spread
// is made with gains alone (a voice's two gains always add to one), never
// with a delay or a polarity flip between the sides, so the left plus the
// right is the same two voices whatever the spread: folded to mono nothing
// cancels that was not already in the voices.
#include <algorithm>
#include <cmath>
#include "effects/EffectDsp.h"

namespace {

class DualChorusProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "modulation.dualchorus";

  // Per voice: the standard rate parameters (fx::appendRateParams, prefix a
  // or b), then depth and delay.
  enum Param {
    kASync = 0, kADivision, kARateHz, kADepth, kADelayMs,
    kBSync, kBDivision, kBRateHz, kBDepth, kBDelayMs,
    kSpread, kToneHz
  };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      for (int voice = 0; voice < 2; ++voice) {
        const bool a = voice == 0;
        fx::RateParamOptions rate;
        rate.minHz = 0.05f;
        rate.maxHz = 10.0f;
        // Not a simple ratio, so the two drift against each other.
        rate.defaultHz = a ? 0.6f : 0.85f;
        rate.name = a ? "Rate A" : "Rate B";
        rate.idPrefix = a ? "a" : "b";
        fx::appendRateParams(t, rate);
        t.push_back(BlockParamSpec::floatParam(a ? "aDepth" : "bDepth", a ? "Depth A" : "Depth B",
                                               "%", 0.0f, 100.0f, 50.0f));
        t.push_back(BlockParamSpec::floatParam(a ? "aDelayMs" : "bDelayMs",
                                               a ? "Delay A" : "Delay B", "ms", kMinDelayMs,
                                               kMaxDelayMs, a ? 11.0f : 17.0f)
                        .withSkew(0.6f));
      }
      t.push_back(BlockParamSpec::floatParam("spread", "Spread", "%", 0.0f, 100.0f, 70.0f));
      t.push_back(BlockParamSpec::floatParam("toneHz", "Tone", "Hz", 1000.0f, 20000.0f, 9000.0f)
                      .withSkew(0.4f));
      return t;
    }();
    return table;
  }

  DualChorusProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The wet is the two voices alone: over after the longest delay plus the
  // sweep, and the high cut settling. Fixed.
  double tailSeconds() const noexcept override {
    return (kMaxDelayMs + kMaxSwingMs) * 0.001 + 0.01;
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    channelsPrepared = juce::jlimit(1, kMaxChannels, spec.numChannels);
    line.prepare(channelsPrepared,
                 static_cast<int>(std::ceil((kMaxDelayMs + kMaxSwingMs) * 0.001 * sampleRate)) + 4);
    for (auto& lfo : lfos)
      lfo.prepare(sampleRate);
    for (auto& voice : voices) {
      voice.depth.prepare(sampleRate, 0.03);
      // A delay that moves bends the pitch while it moves; glide slowly.
      voice.delay.prepare(sampleRate, 0.12);
    }
    spread.prepare(sampleRate, 0.05);
    tone.prepare(sampleRate, 0.03);
    resetEffect();
  }

  void resetEffect() noexcept override {
    line.clear();
    for (auto& filter : highCut)
      filter.reset();
    lfos[0].reset(0.0);
    lfos[1].reset(0.0);
    for (auto& voice : voices) {
      voice.depth.snapOnNextTarget();
      voice.delay.snapOnNextTarget();
    }
    spread.snapOnNextTarget();
    tone.snapOnNextTarget();
    gridCounter = 0;
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), channelsPrepared);
    const bool stereo = numChannels > 1 && blockHasStereoImage(buffer.getNumChannels(), ctx);

    for (int v = 0; v < 2; ++v) {
      const int base = v == 0 ? kASync : kBSync;
      lfos[v].setRateHz(fx::resolveRateHz(param(base), param(base + 1), param(base + 2), ctx.transport));
      if (param(base) >= 0.5f)
        lfos[v].syncPhaseToTransport(
            ctx.transport, fx::noteDivisionBeats(static_cast<int>(std::lround(param(base + 1)))));
      voices[v].depth.setTarget(param(base + 3) * 0.01f);
      voices[v].delay.setTarget(param(base + 4));
    }
    // Only a true stereo image gets a spread: elsewhere both sides hear
    // the same centred pair.
    spread.setTarget(stereo ? param(kSpread) * 0.01f : 0.0f);
    tone.setTarget(param(kToneHz));

    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    const float samplesPerMs = static_cast<float>(sampleRate * 0.001);
    for (int i = 0; i < numSamples; ++i) {
      float delaySamples[2];
      for (int v = 0; v < 2; ++v) {
        const float swingMs = voices[v].depth.next() * kMaxSwingMs;
        const float centreMs = voices[v].delay.next();
        // Voice B is a quarter turn behind A at the same rate.
        const float wave = lfos[v].valueAt(v == 0 ? 0.0f : 0.25f);
        delaySamples[v] = (centreMs + swingMs * wave) * samplesPerMs;
      }
      const float lean = spread.next();
      const float cutoff = tone.next();
      if (gridCounter == 0)
        for (int ch = 0; ch < numChannels; ++ch)
          highCut[ch].setCutoff(cutoff, sampleRate);
      gridCounter = (gridCounter + 1) & (kControlGrid - 1);

      // Voice A's gain on the left, B's on the right is `toA`; the other
      // side hears the rest of each, so a voice's two gains add to one.
      const float toA = 0.5f + 0.5f * lean;
      const float toB = 1.0f - toA;
      // Per-side power stays put as the spread moves (the voices are
      // decorrelated): 1 / sqrt(toA^2 + toB^2).
      const float level = 1.0f / std::sqrt(toA * toA + toB * toB);

      float voiceA[kMaxChannels] = {0.0f, 0.0f};
      float voiceB[kMaxChannels] = {0.0f, 0.0f};
      for (int ch = 0; ch < numChannels; ++ch) {
        voiceA[ch] = line.readHermite(ch, delaySamples[0]);
        voiceB[ch] = line.readHermite(ch, delaySamples[1]);
        line.write(ch, channels[ch][i]);
      }
      if (stereo) {
        channels[0][i] = highCut[0].lowpass(level * (toA * voiceA[0] + toB * voiceB[0]));
        channels[1][i] = highCut[1].lowpass(level * (toB * voiceA[1] + toA * voiceB[1]));
      } else {
        // A centred pair; toA = toB = 1/2 gives level sqrt(2).
        for (int ch = 0; ch < numChannels; ++ch)
          channels[ch][i] = highCut[ch].lowpass(0.70710678f * (voiceA[ch] + voiceB[ch]));
      }
      lfos[0].advance();
      lfos[1].advance();
    }
  }

private:
  static constexpr int kMaxChannels = 2;
  static constexpr int kControlGrid = 16;
  static constexpr float kMinDelayMs = 4.0f;
  static constexpr float kMaxDelayMs = 30.0f;
  // Peak sweep of a voice's delay at 100% depth: 3 ms at 1 Hz bends the
  // pitch by up to 2 pi * 0.003 = 1.9%, 32 cents. Never more than the
  // shortest delay leaves room for.
  static constexpr float kMaxSwingMs = 3.0f;

  struct Voice {
    fx::SmoothedParam depth;
    fx::SmoothedParam delay;
  };

  fx::DelayLine line;
  fx::OnePole highCut[kMaxChannels];
  fx::Lfo lfos[2];
  Voice voices[2];
  fx::SmoothedParam spread;
  fx::SmoothedParam tone;
  double sampleRate{48000.0};
  int channelsPrepared{2};
  int gridCounter{0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(DualChorus) {
  // The wet is the two voices alone, as for the chorus: a plain crossfade
  // against the dry.
  addEffectKind<DualChorusProcessor>(registry, "Dual Chorus", BlockCategory::Modulation, 0.5f,
                                     BlockMixLaw::Crossfade);
}

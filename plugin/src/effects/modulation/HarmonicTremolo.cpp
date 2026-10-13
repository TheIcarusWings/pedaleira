// modulation.harmonictrem: the signal is split at a crossover into lows and
// highs, and the two bands are amplitude-modulated by the same sine in
// opposite phase: while the lows swell the highs duck, and the other way
// round. The split is a fourth-order Linkwitz-Riley pair, which adds back to
// a flat response (all-pass) with no modulation, so depth 0 changes the
// sound by phase alone.
#include <algorithm>
#include <cmath>
#include "effects/EffectDsp.h"

namespace {

class HarmonicTremoloProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "modulation.harmonictrem";

  // The first three are the standard rate parameters (fx::appendRateParams).
  enum Param { kSync = 0, kDivision, kRateHz, kDepth, kCrossoverHz, kToneHz, kWidth };

  static const BlockReadingTable& readingSpecs() {
    static const BlockReadingTable table = {
        {"lowGain", "Low Band", "", 0.0f, 1.0f},
        {"highGain", "High Band", "", 0.0f, 1.0f},
    };
    return table;
  }

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      fx::RateParamOptions rate;
      rate.minHz = 0.1f;
      rate.maxHz = 20.0f;
      rate.defaultHz = 4.5f;
      fx::appendRateParams(t, rate);
      t.push_back(BlockParamSpec::floatParam("depth", "Depth", "%", 0.0f, 100.0f, 70.0f));
      t.push_back(BlockParamSpec::floatParam("crossoverHz", "Crossover", "Hz", 200.0f, 3000.0f,
                                             800.0f)
                      .withSkew(0.5f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("toneHz", "Tone", "Hz", 1000.0f, 20000.0f, 12000.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("width", "Width", "%", 0.0f, 100.0f, 0.0f));
      return t;
    }();
    return table;
  }

  HarmonicTremoloProcessor()
      : ParameterisedBlockProcessor(kKindId, parameterSpecs(), readingSpecs()) {}

  // Only the crossover rings once the input stops: a fourth-order
  // Linkwitz-Riley fades below -60 dB in about 1.6 / f seconds. Twice that
  // and a floor is a close upper bound.
  double tailSeconds() const noexcept override {
    return 3.2 / static_cast<double>(juce::jmax(param(kCrossoverHz), 50.0f)) + 0.005;
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    channelsPrepared = juce::jlimit(1, kMaxChannels, spec.numChannels);
    lfo.prepare(sampleRate);
    depth.prepare(sampleRate, 0.03);
    crossover.prepare(sampleRate, 0.05);
    tone.prepare(sampleRate, 0.03);
    width.prepare(sampleRate, 0.1);
    resetEffect();
  }

  void resetEffect() noexcept override {
    for (auto& channel : state)
      channel = ChannelState{};
    lfo.reset();
    depth.snapOnNextTarget();
    crossover.snapOnNextTarget();
    tone.snapOnNextTarget();
    width.snapOnNextTarget();
    gridCounter = 0;
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), channelsPrepared);
    const bool stereo = blockHasStereoImage(buffer.getNumChannels(), ctx);

    lfo.setRateHz(fx::resolveRateHz(param(kSync), param(kDivision), param(kRateHz), ctx.transport));
    if (param(kSync) >= 0.5f)
      lfo.syncPhaseToTransport(ctx.transport,
                               fx::noteDivisionBeats(static_cast<int>(std::lround(param(kDivision)))));
    depth.setTarget(param(kDepth) * 0.01f);
    crossover.setTarget(param(kCrossoverHz));
    tone.setTarget(param(kToneHz));
    // At 100% the right side runs half a cycle behind the left: where the
    // left lows swell, the right lows duck.
    width.setTarget(stereo ? param(kWidth) * 0.005f : 0.0f);

    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    float lowGainSeen = 1.0f;
    float highGainSeen = 1.0f;
    for (int i = 0; i < numSamples; ++i) {
      const float swing = depth.next() * 0.5f;
      const float offset = width.next();
      const float cutoff = crossover.next();
      const float toneHz = tone.next();
      if (gridCounter == 0) {
        const fx::BiquadCoeffs low = fx::BiquadCoeffs::lowpass(cutoff, 0.70710678, sampleRate);
        const fx::BiquadCoeffs high = fx::BiquadCoeffs::highpass(cutoff, 0.70710678, sampleRate);
        for (int ch = 0; ch < numChannels; ++ch) {
          state[ch].lowA.setCoeffs(low);
          state[ch].lowB.setCoeffs(low);
          state[ch].highA.setCoeffs(high);
          state[ch].highB.setCoeffs(high);
          state[ch].highCut.setCutoff(toneHz, sampleRate);
        }
      }
      gridCounter = (gridCounter + 1) & (kControlGrid - 1);

      for (int ch = 0; ch < numChannels; ++ch) {
        const float wave = lfo.valueAt(ch == 1 ? offset : 0.0f);
        // Lows follow the wave, highs its mirror image: 1 at their top,
        // 1 - depth at their bottom.
        const float lowGain = 1.0f - swing * (1.0f - wave);
        const float highGain = 1.0f - swing * (1.0f + wave);
        ChannelState& s = state[ch];
        const float in = channels[ch][i];
        const float lows = s.lowB.process(s.lowA.process(in));
        const float highs = s.highB.process(s.highA.process(in));
        channels[ch][i] = s.highCut.lowpass(lows * lowGain + highs * highGain);
        if (ch == 0) {
          lowGainSeen = lowGain;
          highGainSeen = highGain;
        }
      }
      lfo.advance();
    }
    setReading(0, lowGainSeen);
    setReading(1, highGainSeen);
  }

private:
  static constexpr int kMaxChannels = 2;
  static constexpr int kControlGrid = 16;

  struct ChannelState {
    // Two sections each make the fourth-order halves of the crossover.
    fx::Biquad lowA, lowB, highA, highB;
    fx::OnePole highCut;
  };

  ChannelState state[kMaxChannels];
  fx::Lfo lfo;
  fx::SmoothedParam depth;
  fx::SmoothedParam crossover;
  fx::SmoothedParam tone;
  fx::SmoothedParam width;
  double sampleRate{48000.0};
  int channelsPrepared{2};
  int gridCounter{0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(HarmonicTremolo) {
  // The wet is the whole signal, modulated: fully wet, like the tremolo.
  addEffectKind<HarmonicTremoloProcessor>(registry, "Harmonic Tremolo", BlockCategory::Modulation,
                                          1.0f, BlockMixLaw::Crossfade);
}

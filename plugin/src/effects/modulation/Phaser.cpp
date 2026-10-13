// modulation.phaser: a chain of first-order allpass stages whose break
// frequency is swept by a sine LFO (exponentially, so the notches climb in
// even musical steps), with feedback of either sign around the chain. The
// wet is the input plus the allpassed signal: every pair of stages gives one
// notch where the two are in opposite phase.
//
// The voicing picks how the wet is made. Classic (the default) is the above.
// Lifted is the response measured on a reference unit, which is lifted by
// +1.5 to +5.7 dB across the band rather than dipping: the wet is the input
// plus the allpassed copy at 1.8 times the loop-normalised level, so at the
// 50% mix that response is for (a crossfade of one dry share and one wet
// share) the output is the dry signal plus 0.9 of the allpass chain, which
// lifts by up to 5.6 dB where the two add and dips only a little where the
// notches dwell. It is made for a mix of 50%; at 100% it is louder.
#include <algorithm>
#include <cmath>
#include "effects/EffectDsp.h"

namespace {

class PhaserProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "modulation.phaser";

  // The first three are the standard rate parameters (fx::appendRateParams).
  enum Param {
    kSync = 0, kDivision, kRateHz, kDepth, kCenterHz, kStages, kFeedback, kWidth, kVoicing
  };
  enum Voicing { kClassic = 0, kLifted };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      fx::RateParamOptions rate;
      rate.minHz = 0.05f;
      rate.maxHz = 10.0f;
      rate.defaultHz = 0.4f;
      fx::appendRateParams(t, rate);
      // Two octaves of sweep either side of the centre at 100%.
      t.push_back(BlockParamSpec::floatParam("depth", "Depth", "%", 0.0f, 100.0f, 70.0f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("centerHz", "Center", "Hz", 100.0f, 4000.0f, 700.0f)
                      .withSkew(0.4f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::choiceParam("stages", "Stages",
                                              juce::StringArray{"4", "6", "8"}, 0)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("feedback", "Feedback", "%", -90.0f, 90.0f, 30.0f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("width", "Width", "%", 0.0f, 100.0f, 50.0f));
      // Appended after the first release.
      t.push_back(BlockParamSpec::choiceParam("voicing", "Voicing",
                                              juce::StringArray{"Classic", "Lifted"}, kClassic)
                      .notAutomatable());
      return t;
    }();
    return table;
  }

  PhaserProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The chain rings at its lowest break frequency: the response of N stages
  // there fades in about (N + 5) / t samples (t = tan(pi f / fs), measured
  // for 4 to 8 stages), and each trip round the feedback loop takes at most
  // N / t more and leaves `feedback` of the level. A loose upper bound at
  // the extremes of the sweep. Atomics and arithmetic only.
  double tailSeconds() const noexcept override {
    const int stages = stageCount(param(kStages));
    const double lowest = std::max(
        kLowestHz, static_cast<double>(param(kCenterHz)) *
                       std::exp2(-kSweepOctaves * static_cast<double>(param(kDepth)) * 0.01));
    const double t = std::tan(juce::MathConstants<double>::pi * lowest / sampleRate);
    const double feedback = std::abs(static_cast<double>(param(kFeedback))) * 0.01;
    double passes = 0.0;
    if (feedback > 0.0005)
      passes = std::ceil(std::log(0.001) / std::log(feedback));
    const double samples = (stages + 5.0) / t + 8.0 + passes * (stages / t + 1.0);
    return samples / sampleRate;
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    channelsPrepared = juce::jlimit(1, kMaxChannels, spec.numChannels);
    lfo.prepare(sampleRate);
    depth.prepare(sampleRate, 0.03);
    center.prepare(sampleRate, 0.05);
    feedback.prepare(sampleRate, 0.03);
    width.prepare(sampleRate, 0.1);
    for (auto& weight : tapWeight)
      weight.prepare(sampleRate, 0.03);
    resetEffect();
  }

  void resetEffect() noexcept override {
    for (auto& channel : state)
      channel = ChannelState{};
    lfo.reset();
    depth.snapOnNextTarget();
    center.snapOnNextTarget();
    feedback.snapOnNextTarget();
    width.snapOnNextTarget();
    for (auto& weight : tapWeight)
      weight.snapOnNextTarget();
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
    center.setTarget(param(kCenterHz));
    feedback.setTarget(param(kFeedback) * 0.01f);
    width.setTarget(stereo ? param(kWidth) * 0.005f : 0.0f);
    // The output is taken after 4, 6 or 8 stages; all eight always run, and
    // a change of choice crossfades between the taps instead of jumping.
    const int chosen = static_cast<int>(std::lround(param(kStages)));
    for (int k = 0; k < kNumTaps; ++k)
      tapWeight[k].setTarget(k == chosen ? 1.0f : 0.0f);

    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    const bool lifted = param(kVoicing) >= 0.5f;
    const float pi = juce::MathConstants<float>::pi;
    const float fs = static_cast<float>(sampleRate);
    for (int i = 0; i < numSamples; ++i) {
      const float reach = depth.next() * kSweepOctaves;
      const float centreHz = center.next();
      const float fb = feedback.next();
      const float offset = width.next();
      float weights[kNumTaps];
      for (int k = 0; k < kNumTaps; ++k)
        weights[k] = tapWeight[k].next();
      // The chain with its loop adds a resonance of power 1 / (1 - fb^2) to
      // the allpassed copy; the same normalisation as the flanger's keeps
      // the level of the wet near the input's.
      const float norm = 1.0f / std::sqrt(1.0f + 1.0f / (1.0f - fb * fb));

      for (int ch = 0; ch < numChannels; ++ch) {
        const float lfoValue = lfo.valueAt(ch == 1 ? offset : 0.0f);
        const float hz = juce::jlimit(static_cast<float>(kLowestHz), 0.45f * fs,
                                      centreHz * std::exp2(reach * lfoValue));
        // Break frequency hz: the phase through one stage is -90 degrees
        // there. a = (t - 1) / (t + 1) puts the pole at (1 - t) / (1 + t).
        const float t = std::tan(pi * hz / fs);
        const float a = (t - 1.0f) / (t + 1.0f);

        ChannelState& s = state[ch];
        const float in = channels[ch][i];
        float v = in + fx::softClip(fb * s.returned);
        float tapped = 0.0f;
        for (int stage = 0; stage < kMaxStages; ++stage) {
          const float y = a * v + s.z[stage];
          s.z[stage] = v - a * y;
          v = y;
          if (stage == 3)
            tapped += weights[0] * v;
          else if (stage == 5)
            tapped += weights[1] * v;
          else if (stage == 7)
            tapped += weights[2] * v;
        }
        s.returned = tapped;
        if (lifted)
          channels[ch][i] = in + kLiftedWet * (1.0f - std::abs(fb)) * tapped;
        else
          channels[ch][i] = (in + tapped) * norm;
      }
      lfo.advance();
    }
  }

private:
  static constexpr int kMaxChannels = 2;
  static constexpr int kMaxStages = 8;
  static constexpr int kNumTaps = 3;
  static constexpr float kSweepOctaves = 2.0f;
  static constexpr double kLowestHz = 40.0;
  // The lifted voicing's allpass level: twice 0.9, for a crossfade at 50%.
  static constexpr float kLiftedWet = 1.8f;

  struct ChannelState {
    float z[kMaxStages] = {};
    // The chain's output a sample ago, fed back to its input.
    float returned{0.0f};
  };

  static int stageCount(float choice) noexcept { return 4 + 2 * static_cast<int>(std::lround(choice)); }

  ChannelState state[kMaxChannels];
  fx::Lfo lfo;
  fx::SmoothedParam depth;
  fx::SmoothedParam center;
  fx::SmoothedParam feedback;
  fx::SmoothedParam width;
  fx::SmoothedParam tapWeight[kNumTaps];
  double sampleRate{48000.0};
  int channelsPrepared{2};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(Phaser) {
  // The wet is the input plus its allpassed copy (that is what makes the
  // notches), so the mix is a crossfade from the clean signal to the full
  // phaser.
  addEffectKind<PhaserProcessor>(registry, "Phaser", BlockCategory::Modulation, 1.0f,
                                 BlockMixLaw::Crossfade);
}

// filter.autowah: the wah voicing with an LFO on the pedal. The LFO sweeps
// the cutoff up and down around `centreHz` by up to 1.5 octaves each way
// (at full depth), at a free rate or locked to the tempo with the standard
// rate parameters.
#include <cmath>
#include "effects/EffectDsp.h"
#include "effects/filter/SweepFilter.h"

namespace {

class AutoWahProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "filter.autowah";

  // The first three are the standard rate parameters
  // (fx::appendRateParams adds them in this order).
  enum Param {
    kSync = 0,
    kDivision,
    kRateHz,
    kDepthPct,
    kCentreHz,
    kResonancePct,
    kShape,
    kLevelDb
  };

  // Octaves the cutoff travels either side of the centre at 100 % depth.
  static constexpr double kMaxSweepOctaves = 1.5;

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      fx::RateParamOptions rate;
      rate.minHz = 0.05f;
      rate.maxHz = 10.0f;
      rate.defaultHz = 0.8f;
      rate.defaultDivision = fx::kDivQuarter;
      fx::appendRateParams(t, rate);
      t.push_back(BlockParamSpec::floatParam("depthPct", "Depth", "%", 0.0f, 100.0f, 70.0f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("centreHz", "Centre", "Hz", 200.0f, 3000.0f, 800.0f)
                      .withSkew(0.45f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("resonancePct", "Resonance", "%", 0.0f, 100.0f, 50.0f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::choiceParam("shape", "Shape", {"Sine", "Triangle"}, 0));
      t.push_back(BlockParamSpec::floatParam("levelDb", "Level", "dB", -12.0f, 12.0f, 0.0f));
      return t;
    }();
    return table;
  }

  AutoWahProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // Ringing at the lowest cutoff the LFO reaches, the longest it can last.
  double tailSeconds() const noexcept override {
    const double lowest = lowestCutoffHz();
    return fxfilter::resonanceDecaySeconds(lowest, fxfilter::resonanceToQ(param(kResonancePct))) +
           kSmoothSeconds;
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    lfo.prepare(sampleRate);
    depth.prepare(sampleRate, kSmoothSeconds);
    centreLog2.prepare(sampleRate, kSmoothSeconds);
    quality.prepare(sampleRate, kSmoothSeconds);
    level.prepare(sampleRate, kSmoothSeconds);
    resetEffect();
  }

  void resetEffect() noexcept override {
    for (auto& filter : filters)
      filter.reset();
    lfo.reset();
    depth.snapOnNextTarget();
    centreLog2.snapOnNextTarget();
    quality.snapOnNextTarget();
    level.snapOnNextTarget();
    ticksLeft = 0;
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxChannels);

    lfo.setShape(param(kShape) >= 0.5f ? fx::Lfo::Shape::Triangle : fx::Lfo::Shape::Sine);
    lfo.setRateHz(fx::resolveRateHz(param(kSync), param(kDivision), param(kRateHz), ctx.transport));
    // Locked to the song position while the host plays, so a synced sweep
    // lands on the beat; free-running at the tempo otherwise.
    if (param(kSync) >= 0.5f)
      lfo.syncPhaseToTransport(ctx.transport,
                               fx::noteDivisionBeats(static_cast<int>(std::lround(param(kDivision)))));

    depth.setTarget(param(kDepthPct) * 0.01f);
    centreLog2.setTarget(std::log2(param(kCentreHz)));
    quality.setTarget(fxfilter::resonanceToQ(param(kResonancePct)));
    level.setTarget(fx::dbToGain(param(kLevelDb)));

    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    for (int i = 0; i < numSamples; ++i) {
      const float swing = lfo.next();
      const float amount = depth.next();
      const float centre = centreLog2.next();
      const float q = quality.next();
      const float gain = level.next();

      if (ticksLeft == 0) {
        // The sweep is in octaves around the centre. Both sides see the same
        // LFO value: the sweep is mono, so a dual-mono pair stays apart and a
        // stereo buffer keeps its image.
        const double hz = std::exp2(static_cast<double>(centre) +
                                    kMaxSweepOctaves * static_cast<double>(amount * swing));
        for (int ch = 0; ch < numChannels; ++ch)
          filters[ch].set(hz, q, sampleRate, fxfilter::SweepFilter::Voice::Wah);
        ticksLeft = kControlGrid;
      }
      --ticksLeft;

      for (int ch = 0; ch < numChannels; ++ch)
        channels[ch][i] = gain * filters[ch].process(channels[ch][i]);
    }
  }

private:
  static constexpr int kMaxChannels = 2;
  static constexpr int kControlGrid = 16;
  static constexpr double kSmoothSeconds = 0.02;

  double lowestCutoffHz() const noexcept {
    return static_cast<double>(param(kCentreHz)) *
           std::exp2(-kMaxSweepOctaves * static_cast<double>(param(kDepthPct)) * 0.01);
  }

  fxfilter::SweepFilter filters[kMaxChannels];
  fx::Lfo lfo;
  fx::SmoothedParam depth, centreLog2, quality, level;
  double sampleRate{48000.0};
  int ticksLeft{0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(AutoWahEffect) {
  addEffectKind<AutoWahProcessor>(registry, "Auto Wah", BlockCategory::Filter);
}

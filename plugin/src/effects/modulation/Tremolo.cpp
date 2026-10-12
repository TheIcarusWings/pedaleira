// modulation.tremolo: amplitude modulation by a sine, triangle or square
// LFO. The gain swings between 1 and 1 - depth. The square shape is
// slew-limited so its edges are ramps of a few milliseconds rather than
// clicks. In a true stereo image `pan` puts the right side's LFO out of
// phase with the left (at 100% in opposite phase: harmonic tremolo or
// auto-pan).
#include <cmath>
#include "effects/EffectDsp.h"

namespace {

class TremoloProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "modulation.tremolo";

  // The first three are the standard rate parameters (fx::appendRateParams).
  enum Param { kSync = 0, kDivision, kRateHz, kDepth, kShape, kPan };
  enum Shape { kSine = 0, kTriangle, kSquare };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      fx::RateParamOptions rate;
      rate.minHz = 0.1f;
      rate.maxHz = 20.0f;
      rate.defaultHz = 5.0f;
      fx::appendRateParams(t, rate);
      t.push_back(BlockParamSpec::floatParam("depth", "Depth", "%", 0.0f, 100.0f, 60.0f));
      t.push_back(BlockParamSpec::choiceParam(
          "shape", "Shape", juce::StringArray{"Sine", "Triangle", "Square"}, kSine));
      t.push_back(BlockParamSpec::floatParam("pan", "Pan", "%", 0.0f, 100.0f, 0.0f));
      return t;
    }();
    return table;
  }

  TremoloProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The output is the input times a gain: nothing rings after the input
  // stops, so the default tail of zero is the honest one.

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    lfo.prepare(sampleRate);
    depth.prepare(sampleRate, 0.02);
    // The right side's offset moves its gain through the cycle; glide it.
    pan.prepare(sampleRate, 0.1);
    smoothCoeffSoft = onePoleCoefficient(kSoftSeconds);
    smoothCoeffSquare = onePoleCoefficient(kSquareEdgeSeconds);
    resetEffect();
  }

  void resetEffect() noexcept override {
    lfo.reset();
    depth.snapOnNextTarget();
    pan.snapOnNextTarget();
    // The first sample after a reset starts the slew at its own target, so a
    // block that comes back on does not ramp in from unity; and a depth of
    // 0, whose target is exactly 1, passes the signal on bit for bit.
    for (auto& channel : slew)
      channel = Slew{};
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxChannels);
    const bool stereo = blockHasStereoImage(buffer.getNumChannels(), ctx);

    const int shape = static_cast<int>(std::lround(param(kShape)));
    lfo.setShape(shape == kTriangle ? fx::Lfo::Shape::Triangle : fx::Lfo::Shape::Sine);
    lfo.setRateHz(fx::resolveRateHz(param(kSync), param(kDivision), param(kRateHz), ctx.transport));
    if (param(kSync) >= 0.5f)
      lfo.syncPhaseToTransport(ctx.transport,
                               fx::noteDivisionBeats(static_cast<int>(std::lround(param(kDivision)))));
    depth.setTarget(param(kDepth) * 0.01f);
    pan.setTarget(stereo ? param(kPan) * 0.005f : 0.0f);
    const float coeff = shape == kSquare ? smoothCoeffSquare : smoothCoeffSoft;

    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    for (int i = 0; i < numSamples; ++i) {
      const float swing = depth.next() * 0.5f;
      const float offset = pan.next();
      for (int ch = 0; ch < numChannels; ++ch) {
        float wave = lfo.valueAt(ch == 1 ? offset : 0.0f);
        if (shape == kSquare)
          wave = wave >= 0.0f ? 1.0f : -1.0f;
        // 1 at the top of the wave, 1 - depth at the bottom.
        const float target = 1.0f - swing * (1.0f - wave);
        // Two one-pole stages in series: a jump of the target becomes a
        // smooth S-shaped ramp, with no edge steeper than a few per cent of
        // the swing per sample. For the soft shapes the stages are so quick
        // they only take the corners off a depth or shape change.
        Slew& s = slew[ch];
        if (!s.primed) {
          s.first = s.second = target;
          s.primed = true;
        }
        s.first += coeff * (target - s.first);
        s.second += coeff * (s.first - s.second);
        channels[ch][i] *= s.second;
      }
      lfo.advance();
    }
  }

private:
  static constexpr int kMaxChannels = 2;
  // Time constant of each of the two stages. Square: an edge of 1 ms per
  // stage settles in about 5 ms. Others: 0.1 ms, a lag of 10 microseconds at
  // the fastest rate.
  static constexpr double kSquareEdgeSeconds = 0.001;
  static constexpr double kSoftSeconds = 0.0001;

  struct Slew {
    float first{1.0f};
    float second{1.0f};
    bool primed{false};
  };

  float onePoleCoefficient(double seconds) const noexcept {
    return static_cast<float>(1.0 - std::exp(-1.0 / (seconds * sampleRate)));
  }

  fx::Lfo lfo;
  fx::SmoothedParam depth;
  fx::SmoothedParam pan;
  Slew slew[kMaxChannels];
  float smoothCoeffSoft{1.0f};
  float smoothCoeffSquare{1.0f};
  double sampleRate{48000.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(Tremolo) {
  // The wet is the whole signal, modulated: a new block starts fully wet.
  // Crossfading it against the dry only shallows the depth.
  addEffectKind<TremoloProcessor>(registry, "Tremolo", BlockCategory::Modulation, 1.0f,
                                  BlockMixLaw::Crossfade);
}

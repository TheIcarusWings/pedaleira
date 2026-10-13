// modulation.patterntrem: a step tremolo. A clock runs through 4 to 16 steps
// and every step has its own level. The gain moves from one step's level to
// the next over `shape` of a step: at 0% an edge of about a millisecond (a
// hard step, free of clicks), at 100% a raised-cosine glide that takes the
// whole step. Free, the clock runs at `rateHz` steps per second; synced, one
// step is one note value (`division`) of the tempo, and while the host rolls
// the clock sits where the song position says. The levels come from the 16
// step knobs ("Custom") or from a preset pattern drawn over however many
// steps are in use.
#include <algorithm>
#include <cmath>
#include "effects/EffectDsp.h"

namespace {

class PatternTremoloProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "modulation.patterntrem";

  static constexpr int kMaxSteps = 16;
  static constexpr int kMinSteps = 4;

  // The first three are the standard rate parameters (fx::appendRateParams);
  // the step levels follow the other controls.
  enum Param { kSync = 0, kDivision, kRateHz, kSteps, kPattern, kShape, kDepth, kLevel1 };
  enum Pattern {
    kCustom = 0, kPulse, kGallop, kRampUp, kRampDown, kSwell, kStutter, kSyncopated, kNumPatterns
  };

  static const BlockReadingTable& readingSpecs() {
    static const BlockReadingTable table = {
        {"step", "Step", "", 0.0f, static_cast<float>(kMaxSteps - 1)},
    };
    return table;
  }

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      // Here a "cycle" of the rate is one step.
      fx::RateParamOptions rate;
      rate.name = "Step Rate";
      rate.minHz = 0.5f;
      rate.maxHz = 32.0f;
      rate.defaultHz = 8.0f;
      rate.defaultDivision = fx::kDivSixteenth;
      fx::appendRateParams(t, rate);
      juce::StringArray stepLabels;
      for (int n = kMinSteps; n <= kMaxSteps; ++n)
        stepLabels.add(juce::String(n));
      t.push_back(BlockParamSpec::choiceParam("steps", "Steps", stepLabels, 8 - kMinSteps));
      t.push_back(BlockParamSpec::choiceParam(
          "pattern", "Pattern",
          juce::StringArray{"Custom", "Pulse", "Gallop", "Ramp Up", "Ramp Down", "Swell",
                            "Stutter", "Syncopated"},
          kPulse));
      t.push_back(BlockParamSpec::floatParam("shape", "Shape", "%", 0.0f, 100.0f, 0.0f));
      t.push_back(BlockParamSpec::floatParam("depth", "Depth", "%", 0.0f, 100.0f, 100.0f));
      for (int s = 0; s < kMaxSteps; ++s)
        t.push_back(BlockParamSpec::floatParam("level" + juce::String(s + 1),
                                               "Level " + juce::String(s + 1), "%", 0.0f, 100.0f,
                                               100.0f));
      return t;
    }();
    return table;
  }

  PatternTremoloProcessor()
      : ParameterisedBlockProcessor(kKindId, parameterSpecs(), readingSpecs()) {}

  // The output is the input times a gain: nothing rings.

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    depth.prepare(sampleRate, 0.02);
    smoothCoeff = static_cast<float>(1.0 - std::exp(-1.0 / (kSmoothSeconds * sampleRate)));
    resetEffect();
  }

  void resetEffect() noexcept override {
    depth.snapOnNextTarget();
    phase = 0.0;
    stepIndex = 0;
    primed = false;
    // The first step has nothing before it to glide from.
    fresh = true;
    gain = 1.0f;
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxChannels);

    const int steps = juce::jlimit(kMinSteps, kMaxSteps,
                                   kMinSteps + static_cast<int>(std::lround(param(kSteps))));
    float levels[kMaxSteps];
    fillLevels(levels, steps);

    const bool synced = param(kSync) >= 0.5f;
    const int division = static_cast<int>(std::lround(param(kDivision)));
    const double stepsPerSecond = fx::resolveRateHz(param(kSync), param(kDivision), param(kRateHz),
                                                    ctx.transport);
    const double increment = juce::jmax(0.0, stepsPerSecond) / sampleRate;
    if (synced && ctx.transport.playing) {
      // The song position, in steps: a render from the same position
      // repeats exactly.
      const double position = ctx.transport.ppqPosition / fx::noteDivisionBeats(division);
      const double whole = std::floor(position);
      phase = position - whole;
      stepIndex = static_cast<juce::int64>(whole);
      fresh = false;
    }

    depth.setTarget(param(kDepth) * 0.01f);
    // The edge between two levels, as a share of a step: about 0.7 ms for a
    // hard step, growing to the whole step at 100% shape.
    const double edgeFloor = juce::jmin(1.0, kHardEdgeSeconds * sampleRate * increment);
    const double edge = edgeFloor + (1.0 - edgeFloor) * static_cast<double>(param(kShape)) * 0.01;
    const double invEdge = 1.0 / juce::jmax(edge, 1.0e-9);

    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    for (int i = 0; i < numSamples; ++i) {
      const int current = static_cast<int>(((stepIndex % steps) + steps) % steps);
      const int previous = (current + steps - 1) % steps;
      float level = levels[current];
      if (phase < edge && !fresh) {
        const float t = static_cast<float>(phase * invEdge);
        const float eased = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::pi * t);
        level = levels[previous] + (levels[current] - levels[previous]) * eased;
      }
      const float target = 1.0f - depth.next() * (1.0f - level);
      if (!primed) {
        gain = target;
        primed = true;
      }
      // Takes the corner off the target, and carries a knob move.
      gain += smoothCoeff * (target - gain);
      for (int ch = 0; ch < numChannels; ++ch)
        channels[ch][i] *= gain;

      phase += increment;
      if (phase >= 1.0) {
        phase -= std::floor(phase);
        ++stepIndex;
        fresh = false;
      }
    }
    setReading(0, static_cast<float>(((stepIndex % steps) + steps) % steps));
  }

private:
  static constexpr int kMaxChannels = 2;
  // Time constant of the smoother on the gain (a corner of 0.2 ms).
  static constexpr double kSmoothSeconds = 0.0002;
  // The edge of a hard step.
  static constexpr double kHardEdgeSeconds = 0.0007;

  // The level of every step in use, 0 to 1: the knobs, or a preset drawn
  // over `steps` steps.
  void fillLevels(float* levels, int steps) const noexcept {
    const int pattern = static_cast<int>(std::lround(param(kPattern)));
    static constexpr float kSyncopation[8] = {1.0f, 0.0f, 0.6f, 0.0f, 1.0f, 0.3f, 0.6f, 0.0f};
    for (int s = 0; s < steps; ++s) {
      float v = 1.0f;
      switch (pattern) {
        case kPulse: v = (s % 2 == 0) ? 1.0f : 0.0f; break;
        case kGallop: v = (s % 3 == 2) ? 0.0f : 1.0f; break;
        case kRampUp: v = static_cast<float>(s + 1) / static_cast<float>(steps); break;
        case kRampDown: v = 1.0f - static_cast<float>(s) / static_cast<float>(steps); break;
        case kSwell:
          v = std::sin(juce::MathConstants<float>::pi * (static_cast<float>(s) + 0.5f) /
                       static_cast<float>(steps));
          break;
        case kStutter: v = (s % 4 == 0) ? 1.0f : 0.25f; break;
        case kSyncopated: v = kSyncopation[s % 8]; break;
        case kCustom:
        default: v = param(kLevel1 + s) * 0.01f; break;
      }
      levels[s] = juce::jlimit(0.0f, 1.0f, v);
    }
  }

  fx::SmoothedParam depth;
  double sampleRate{48000.0};
  double phase{0.0};
  juce::int64 stepIndex{0};
  float smoothCoeff{1.0f};
  float gain{1.0f};
  bool primed{false};
  bool fresh{true};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(PatternTremolo) {
  // The wet is the whole signal, gated: fully wet. A crossfade against the
  // dry only shallows the depth.
  addEffectKind<PatternTremoloProcessor>(registry, "Pattern Tremolo", BlockCategory::Modulation,
                                         1.0f, BlockMixLaw::Crossfade);
}

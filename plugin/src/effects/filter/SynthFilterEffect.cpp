// filter.synth: a four-pole transistor-ladder low pass in the classic synth
// style, with an envelope follower on the input that sweeps the cutoff.
//
// The ladder is four trapezoidal (TPT) one-pole stages in a row with the
// output fed back to the input. The feedback loop is solved in the same
// sample (zero delay): with the stage states known, the four-stage output is
// a linear function of the loop input, so the loop closes in one division
// instead of through a unit delay, and the cutoff and resonance stay true
// right up to the top of the range. The transistor character comes from a
// tanh at the loop input and after the first three stages: it rounds the
// drive, bounds the signal whatever the resonance (so full resonance and
// full drive cannot run away) and lets the filter self-oscillate as a clean,
// limited sine near 100 %.
//
// Modes tap the stages: low pass 24 dB is the fourth, low pass 12 dB the
// second, band pass is the second minus the fourth (12 dB per octave each
// side) and high pass is the binomial sum of the stages (24 dB per octave).
//
// It runs at the chain rate, not oversampled: the cutoff is held below
// 0.45 of the sample rate and the tanh stages are gentle, so at the
// extremes of drive some distortion products fold back, which is part of how
// a hard-driven ladder sounds; the chain's own oversampling covers the rest.
#include <cmath>
#include "effects/EffectDsp.h"

namespace {

class SynthFilterProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "filter.synth";

  enum Param {
    kCutoffHz = 0,
    kResonancePct,
    kDriveDb,
    kEnvAmountPct,
    kEnvAttackMs,
    kEnvReleaseMs,
    kMode,
  };
  enum Mode { kLowPass24 = 0, kLowPass12, kBandPass, kHighPass };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::floatParam("cutoffHz", "Cutoff", "Hz", 20.0f, 20000.0f, 1000.0f)
                      .withSkew(0.3f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("resonancePct", "Resonance", "%", 0.0f, 100.0f, 30.0f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("driveDb", "Drive", "dB", 0.0f, 24.0f, 0.0f));
      // Up to four octaves each way; negative closes the filter as you play.
      t.push_back(BlockParamSpec::floatParam("envAmountPct", "Env Amount", "%", -100.0f, 100.0f, 0.0f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("envAttackMs", "Env Attack", "ms", 1.0f, 500.0f, 10.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("envReleaseMs", "Env Release", "ms", 10.0f, 2000.0f, 200.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::choiceParam(
                      "mode", "Mode", {"Low Pass 24 dB", "Low Pass 12 dB", "Band Pass", "High Pass"},
                      kLowPass24)
                      .notAutomatable());
      return t;
    }();
    return table;
  }

  SynthFilterProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The ring of the resonance at the lowest cutoff the envelope can reach,
  // capped: near 100 % the filter oscillates, which is no tail to wait for.
  double tailSeconds() const noexcept override {
    const double amount = static_cast<double>(param(kEnvAmountPct)) * 0.01;
    const double lowestHz = juce::jmax(20.0, static_cast<double>(param(kCutoffHz)) *
                                                 std::exp2(-kEnvOctaves * juce::jmax(0.0, -amount)));
    const double res = static_cast<double>(param(kResonancePct)) * 0.01;
    // An equivalent quality of the dominant pole pair: 0.7 open, 40 at the edge.
    const double q = 0.7 + 40.0 * res * res * res;
    const double ring = 6.9078 * q / (3.14159265358979 * lowestHz);
    return juce::jmin(ring, 8.0) + kSmoothSeconds;
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    for (auto& follower : followers)
      follower.prepare(sampleRate);
    cutoff.prepare(sampleRate, kSmoothSeconds);
    resonance.prepare(sampleRate, kSmoothSeconds);
    drive.prepare(sampleRate, kSmoothSeconds);
    resetEffect();
  }

  void resetEffect() noexcept override {
    for (auto& ladder : ladders)
      ladder = Ladder{};
    for (auto& follower : followers)
      follower.reset();
    cutoff.snapOnNextTarget();
    resonance.snapOnNextTarget();
    drive.snapOnNextTarget();
    ticksLeft = 0;
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxChannels);
    // One stereo signal shares one envelope (and so one cutoff), so the
    // image holds; in dual mono each guitar has its own.
    const bool linked = numChannels == 2 && !ctx.dualMono;
    const int numLanes = linked ? 1 : numChannels;

    for (int lane = 0; lane < numLanes; ++lane)
      followers[lane].setTimes(param(kEnvAttackMs), param(kEnvReleaseMs));
    cutoff.setTarget(param(kCutoffHz));
    resonance.setTarget(param(kResonancePct));
    drive.setTarget(param(kDriveDb));
    const float amount = param(kEnvAmountPct) * 0.01f;
    const int mode = static_cast<int>(std::lround(param(kMode)));
    const double ceilingHz = juce::jmin(20000.0, 0.45 * sampleRate);

    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    for (int i = 0; i < numSamples; ++i) {
      float envelope[kMaxChannels] = {0.0f, 0.0f};
      if (linked) {
        envelope[0] = followers[0].process(
            juce::jmax(std::abs(channels[0][i]), std::abs(channels[1][i])));
      } else {
        for (int lane = 0; lane < numLanes; ++lane)
          envelope[lane] = followers[lane].process(channels[lane][i]);
      }
      const float baseHz = cutoff.next();
      const float res = resonance.next();
      const float driveGain = fx::dbToGain(drive.next());

      // The cutoff moves on a fixed grid of samples that runs across blocks.
      if (ticksLeft == 0) {
        for (int lane = 0; lane < numLanes; ++lane) {
          // The envelope in dB onto 0..1 over a 34 dB span below -6 dB: the
          // loudness scale, so equal steps in playing are equal steps here.
          const float envDb = envelope[lane] > 1.0e-6f ? 20.0f * std::log10(envelope[lane]) : -120.0f;
          const float x = juce::jlimit(0.0f, 1.0f, (envDb - kEnvFloorDb) / (kEnvCeilingDb - kEnvFloorDb));
          const double hz = static_cast<double>(baseHz) *
                            std::exp2(kEnvOctaves * static_cast<double>(amount) * static_cast<double>(x));
          cutoffHz[lane] = juce::jlimit(20.0, ceilingHz, hz);
        }
        const float k = feedbackFor(res);
        for (int ch = 0; ch < numChannels; ++ch)
          ladders[ch].set(cutoffHz[linked ? 0 : ch], k, sampleRate);
        ticksLeft = kControlGrid;
      }
      --ticksLeft;

      for (int ch = 0; ch < numChannels; ++ch)
        channels[ch][i] = ladders[ch].process(channels[ch][i] * driveGain, mode);
    }
  }

private:
  static constexpr int kMaxChannels = 2;
  static constexpr int kControlGrid = 8;
  static constexpr double kEnvOctaves = 4.0;
  static constexpr float kEnvFloorDb = -40.0f;
  static constexpr float kEnvCeilingDb = -6.0f;
  static constexpr double kSmoothSeconds = 0.02;

  /** Loop gain for a resonance in percent: the linear ladder oscillates at
      4, so the top of the range sits a little past it and the tanh holds
      the oscillation to a bounded sine. */
  static float feedbackFor(float resonancePercent) noexcept {
    const float r = juce::jlimit(0.0f, 1.0f, resonancePercent * 0.01f);
    return 4.15f * r;
  }

  struct Ladder {
    void set(double hz, float newFeedback, double rate) noexcept {
      const double g = std::tan(3.14159265358979 * hz / rate);
      stage = static_cast<float>(g / (1.0 + g));
      feedback = newFeedback;
    }

    float process(float in, int mode) noexcept {
      const float G = stage;
      const float oneMinusG = 1.0f - G;
      const float G2 = G * G;
      const float G3 = G2 * G;
      const float G4 = G2 * G2;
      // What the four stages would output with no input: their state terms.
      const float history = oneMinusG * (G3 * s[0] + G2 * s[1] + G * s[2] + s[3]);
      // The loop closed in the linear case gives the output the feedback
      // sees; the tanh then rounds the signal entering the ladder.
      const float estimate = (G4 * in + history) / (1.0f + feedback * G4);
      const float x = std::tanh(in - feedback * estimate);

      float y[4];
      float input = x;
      for (int n = 0; n < 4; ++n) {
        const float v = (input - s[n]) * G;
        y[n] = v + s[n];
        s[n] = y[n] + v;
        // Soft saturation between stages, not after the last.
        input = n < 2 ? std::tanh(y[n]) : y[n];
      }

      switch (mode) {
        case kLowPass12: return y[1];
        case kBandPass: return y[1] - y[3];
        case kHighPass: return x - 4.0f * y[0] + 6.0f * y[1] - 4.0f * y[2] + y[3];
        default: return y[3];
      }
    }

    float s[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float stage{0.1f};
    float feedback{0.0f};
  };

  Ladder ladders[kMaxChannels];
  fx::EnvelopeFollower followers[kMaxChannels];
  fx::SmoothedParam cutoff, resonance, drive;
  double cutoffHz[kMaxChannels] = {1000.0, 1000.0};
  double sampleRate{48000.0};
  int ticksLeft{0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(SynthFilterEffect) {
  addEffectKind<SynthFilterProcessor>(registry, "Synth Filter", BlockCategory::Filter);
}

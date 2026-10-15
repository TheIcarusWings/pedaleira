// filter.synth: a four-pole transistor-ladder low pass in the classic synth
// style, with an envelope follower on the input that sweeps the cutoff.
//
// The ladder is four trapezoidal (TPT) one-pole stages in a row with the
// output fed back to the input. The transistor character comes from a tanh
// at the loop input (where the drive pushes the signal) and after the first
// two stages: it rounds the drive, bounds the signal whatever the resonance
// (so full resonance and full drive cannot run away) and lets the filter
// self-oscillate as a clean, limited sine at the cutoff near 100 %. A tanh
// after the third stage as well measured the same for aliasing and slightly
// worse for the pitch of the oscillation near the top, at more cost, so the
// last two stages stay linear.
//
// The feedback loop is solved in the same sample (zero delay, in the manner
// of Zavalishin's TPT ladder): with the stage states known and each tanh
// replaced by its secant gain (tanh(u) / u) at the value it saw last sample,
// the four-stage output is a linear function of the loop input, so the loop
// closes in one division. The tanh are then evaluated for real from that
// solution. The secants make the solution account for how much the
// saturating stages compress, which a plain linear estimate ignores: with
// them the self-oscillation sits within 0.01 % of the cutoff up to 20 kHz,
// where the plain estimate drifted to 0.05 % (and 0.12 % at the chain rate).
//
// Modes tap the stages: low pass 24 dB is the fourth, low pass 12 dB the
// second, band pass is the second minus the fourth (12 dB per octave each
// side) and high pass is the binomial sum of the stages (24 dB per octave).
//
//   in -> envelope follower -> [4x: drive -> ladder] -> drive makeup
//
// The ladder runs at four times the chain rate in the drive family's
// oversampler (drive/DriveCommon.h): driven tanh stages make harmonics far
// above the base Nyquist, which at the chain rate would fold back as
// inharmonic tones. The envelope follower and the cutoff grid run inside the
// oversampled loop too, so the cutoff moves on the same samples as the
// ladder. The half-band filters delay the signal by kLatencySamples, which
// the kind reports.
//
// Drive is the gain into the ladder; the output is brought down by most of
// it again (kDriveMakeup), so at playing level turning Drive up changes the
// tone (more compression and harmonics, a resonance that gets swamped) and
// not much the level.
#include <cmath>
#include "effects/EffectDsp.h"
#include "effects/drive/DriveCommon.h"

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

  // The half-band filters of the 4x oversampler (drive/DriveCommon.h).
  int latencySamples() const noexcept override { return drivefx::kLatencySamples; }

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
    osRate = sampleRate * drivefx::kOversampleFactor;
    oversampler.prepare(spec.maxBlockSize);
    for (auto& follower : followers)
      follower.prepare(osRate);
    cutoff.prepare(osRate, kSmoothSeconds);
    resonance.prepare(osRate, kSmoothSeconds);
    drive.prepare(osRate, kSmoothSeconds);
    makeup.prepare(sampleRate, kSmoothSeconds);
    resetEffect();
  }

  void resetEffect() noexcept override {
    oversampler.reset();
    for (auto& ladder : ladders)
      ladder.reset();
    for (auto& follower : followers)
      follower.reset();
    cutoff.snapOnNextTarget();
    resonance.snapOnNextTarget();
    drive.snapOnNextTarget();
    makeup.snapOnNextTarget();
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
    const float driveDb = param(kDriveDb);
    drive.setTarget(fx::dbToGain(driveDb));
    makeup.setTarget(fx::dbToGain(-kDriveMakeup * driveDb));
    const float amount = param(kEnvAmountPct) * 0.01f;
    const int mode = static_cast<int>(std::lround(param(kMode)));
    const double ceilingHz = juce::jmin(20000.0, 0.45 * sampleRate);

    oversampler.run(buffer, [&](float* const* io, int frames, int numReal) {
      for (int i = 0; i < frames; ++i) {
        float envelope[kMaxChannels] = {0.0f, 0.0f};
        if (linked) {
          envelope[0] = followers[0].process(juce::jmax(std::abs(io[0][i]), std::abs(io[1][i])));
        } else {
          for (int lane = 0; lane < numLanes; ++lane)
            envelope[lane] = followers[lane].process(io[lane][i]);
        }
        const float baseHz = cutoff.next();
        const float res = resonance.next();
        const float driveGain = drive.next();

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
          for (int ch = 0; ch < numReal; ++ch)
            ladders[ch].set(cutoffHz[linked ? 0 : ch], k, osRate);
          ticksLeft = kControlGridOs;
        }
        --ticksLeft;

        for (int ch = 0; ch < numReal; ++ch)
          io[ch][i] = ladders[ch].process(io[ch][i] * driveGain, mode);
      }
    });

    for (int i = 0; i < numSamples; ++i) {
      const float g = makeup.next();
      for (int ch = 0; ch < numChannels; ++ch)
        buffer.getWritePointer(ch)[i] *= g;
    }
  }

private:
  static constexpr int kMaxChannels = drivefx::kMaxLanes;
  // Control grid: 8 chain-rate samples, counted at the oversampled rate.
  static constexpr int kControlGridOs = 8 * drivefx::kOversampleFactor;
  static constexpr double kEnvOctaves = 4.0;
  static constexpr float kEnvFloorDb = -40.0f;
  static constexpr float kEnvCeilingDb = -6.0f;
  static constexpr double kSmoothSeconds = 0.02;
  // The share of the drive taken back off the output. Measured on a chord
  // at guitar level (0.2 peak) through the default cutoff: the tanh stages
  // swallow about 5 of the 24 dB, so 0.8 holds the level within about 2 dB
  // across the whole Drive range. A quiet note still gets up to 5 dB louder
  // at full drive, as a driven ladder should.
  static constexpr float kDriveMakeup = 0.8f;

  /** Loop gain for a resonance in percent: the linear ladder oscillates at
      4, so the top of the range sits a little past it and the tanh holds
      the oscillation to a bounded sine. */
  static float feedbackFor(float resonancePercent) noexcept {
    const float r = juce::jlimit(0.0f, 1.0f, resonancePercent * 0.01f);
    return 4.15f * r;
  }

  /** tanh(u) by its [7/6] Pade approximant: within 1e-4 of tanh, monotonic,
      and clamped where it has reached 1 (the slope there is under 1e-6, so
      the clamp adds no corner to hear or to alias). Also gives the secant
      tanh(u) / u, the stage's gain at u, from the same division. A fraction
      of the cost of std::tanh, which matters at four times the rate. */
  static float saturate(float u, float& secant) noexcept {
    constexpr float kLimit = 4.97f;
    const float c = juce::jlimit(-kLimit, kLimit, u);
    const float c2 = c * c;
    // Split in halves rather than nested: the ladder is one long chain of
    // dependent steps per sample, so a shorter chain here is what costs less.
    const float c4 = c2 * c2;
    const float odd = (135135.0f + 17325.0f * c2) + c4 * (378.0f + c2);
    const float den = (135135.0f + 62370.0f * c2) + c4 * (3150.0f + 28.0f * c2);
    const float ratio = odd / den;  // tanh(c) / c
    const float y = c * ratio;
    // Past the clamp the output is flat, so the gain falls as 1 / |u|.
    // Both sides are computed and one picked: driven, the clamp comes and
    // goes every few samples, which a branch would keep guessing wrong.
    const bool clamped = c != u;
    const float divisor = clamped ? u : 1.0f;
    const float pastClamp = y / divisor;
    secant = clamped ? pastClamp : ratio;
    return y;
  }

  struct Ladder {
    void reset() noexcept {
      for (int n = 0; n < 4; ++n) {
        s[n] = 0.0f;
        gain[n] = 1.0f;
      }
    }

    void set(double hz, float newFeedback, double rate) noexcept {
      const double g = prewarp(3.14159265358979 * hz / rate);
      stage = static_cast<float>(g / (1.0 + g));
      feedback = newFeedback;
    }

    /** tan(w) by its series to the w^9 term. The cutoff is held under 0.45
        of the chain rate and the ladder runs at four times it, so w stays
        under 0.36, where the next term is below 1e-6 of the result: exact
        for the purpose, at a fraction of what std::tan costs on the grid. */
    static double prewarp(double w) noexcept {
      const double w2 = w * w;
      return w * (1.0 + w2 * (1.0 / 3.0 + w2 * (2.0 / 15.0 + w2 * (17.0 / 315.0 + w2 * (62.0 / 2835.0)))));
    }

    float process(float in, int mode) noexcept {
      const float G = stage;
      const float oneMinusG = 1.0f - G;
      // The fourth stage's output as a * x + b in the loop input x, each
      // tanh standing in as its secant from the last sample: a is the
      // instantaneous gain through the four stages, b what their states
      // alone give.
      float a = G;
      float b = oneMinusG * s[0];
      for (int n = 1; n < 4; ++n) {
        const float t = n <= kSaturatedStages ? gain[n] : 1.0f;
        a = G * t * a;
        b = G * t * b + oneMinusG * s[n];
      }
      // x = t0 * (in - k * (a * x + b)), solved for x, gives the output the
      // feedback sees; the input tanh is then taken for real.
      const float t0 = gain[0];
      // (The division waits on the gains only, not on this sample's states.)
      const float inverse = 1.0f / (1.0f + t0 * feedback * a);
      const float solved = t0 * inverse * (in - feedback * b);
      const float u = in - feedback * (a * solved + b);
      const float x = saturate(u, gain[0]);

      float y[4];
      float input = x;
      for (int n = 0; n < 4; ++n) {
        const float v = (input - s[n]) * G;
        y[n] = v + s[n];
        s[n] = y[n] + v;
        if (n < kSaturatedStages) {
          input = saturate(y[n], gain[n + 1]);
        } else {
          input = y[n];
        }
      }

      switch (mode) {
        case kLowPass12: return y[1];
        case kBandPass: return y[1] - y[3];
        case kHighPass: return x - 4.0f * y[0] + 6.0f * y[1] - 4.0f * y[2] + y[3];
        default: return y[3];
      }
    }

    // Stages followed by a tanh (see the header comment).
    static constexpr int kSaturatedStages = 2;

    float s[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // Secant gains of the input tanh and of the tanh after each saturated stage.
    float gain[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float stage{0.1f};
    float feedback{0.0f};
  };

  Ladder ladders[kMaxChannels];
  fx::EnvelopeFollower followers[kMaxChannels];
  fx::SmoothedParam cutoff, resonance, drive, makeup;
  drivefx::Oversampler4x oversampler;
  double cutoffHz[kMaxChannels] = {1000.0, 1000.0};
  double sampleRate{48000.0};
  double osRate{192000.0};
  int ticksLeft{0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(SynthFilterEffect) {
  addEffectKind<SynthFilterProcessor>(registry, "Synth Filter", BlockCategory::Filter);
}

// reverb.spring: a guitar amp's spring tank, after the parametric model of
// Valimaki, Parker and Abel ("Parametric Spring Reverberation Effect", JAES
// 2010). Each spring is a delay loop with an inverting reflection and a
// chain of first-order allpass filters in it. The chain delays high
// frequencies far longer than low ones, so every trip round the loop
// stretches a pluck further into an upward chirp, the "drip" of a spring,
// and the two or three springs differ a little in length and dispersion, so
// their echo trains drift apart.
//
// The dispersion of a real spring stops at a transition frequency of a few
// kHz, above which little gets through. The model gets both from running
// the springs at a fifth of the sample rate (9.6 kHz): the allpass chain's
// delay then climbs to its maximum at 4.8 kHz, the band limit is the
// resampling filter, and sixty allpasses per spring cost a fifth of what
// they would at the full rate.
//
// Decay: every unit delay in a loop, the ones inside the allpasses included,
// is scaled by gamma = 10^(-3 / (RT60 * rate)), which makes the decay
// parameter the RT60 at every frequency although the loop is longer for the
// high ones; a shelf in the loop then shortens it above 2 kHz, toward half.
#include <array>
#include <cmath>

#include "effects/EffectDsp.h"
#include "effects/reverb/ReverbParts.h"

namespace {

constexpr int kSprings = 3;
constexpr int kSections = 60;
// The springs run at 1/kDecimation of the sample rate.
constexpr int kDecimation = 5;
// Control values move every kControlTicks spring samples (40 samples at the
// full rate), on a grid counted from the last reset.
constexpr int kControlTicks = 8;
constexpr int kControlInterval = kControlTicks * kDecimation;

// Round trip of each spring at low frequencies, and its allpass coefficient.
constexpr double kLoopSeconds[kSprings] = {0.0571, 0.0683, 0.0797};
constexpr float kDispersion[kSprings] = {0.62f, 0.60f, 0.64f};
// High frequencies ring half as long as low ones.
constexpr double kHighRatio = 0.5;
constexpr double kDampingCornerHz = 2000.0;
// Delay of one allpass of the chain in the low and middle of the band, in
// spring-rate samples (it rises to over 4 at the top).
constexpr double kChainDelayInBand = 0.4;
// Springs pass nothing useful below the low strings' fundamentals.
constexpr double kLowCutHz = 110.0;

// A value that lands on its target at each control tick and moves linearly
// in between, in `steps` steps.
struct Slide {
  float value{0.0f}, target{0.0f}, delta{0.0f};
  void snap(float v) noexcept {
    value = target = v;
    delta = 0.0f;
  }
  void aim(float next, int steps) noexcept {
    value = target;
    target = next;
    delta = (next - value) / static_cast<float>(steps);
  }
  float step() noexcept {
    const float v = value;
    value += delta;
    return v;
  }
};

/** An eighth-order Chebyshev low-pass (0.5 dB ripple) as four biquads: the
    anti-alias filter in front of the springs and the reconstruction filter
    behind them. Chebyshev because the springs' band has to reach close to
    their Nyquist frequency (that is where the drip is): it is 26 dB down
    where the first fold lands back in band, a Butterworth of the same order
    12 dB. */
class BandLimiter {
public:
  void prepare(double cutoffHz, double sampleRate) {
    constexpr int order = 8;
    const double ripple = std::sqrt(std::pow(10.0, 0.05) - 1.0);
    const double a = std::asinh(1.0 / ripple) / order;
    const double warped = std::tan(juce::MathConstants<double>::pi * cutoffHz / sampleRate);
    for (int k = 0; k < order / 2; ++k) {
      const double theta = juce::MathConstants<double>::pi * (2.0 * k + 1.0) / (2.0 * order);
      const double real = -std::sinh(a) * std::sin(theta);
      const double imag = std::cosh(a) * std::cos(theta);
      const double radius = std::sqrt(real * real + imag * imag);
      // The cookbook low-pass is the bilinear transform of one analogue
      // pole pair, exact at its own corner.
      const double hz = std::atan(warped * radius) * sampleRate / juce::MathConstants<double>::pi;
      sections[static_cast<size_t>(k)].setCoeffs(
          fx::BiquadCoeffs::lowpass(hz, radius / (-2.0 * real), sampleRate));
    }
    // Centre the pass-band ripple on unity.
    gain = static_cast<float>(std::pow(1.0 + ripple * ripple, -0.25));
  }
  void reset() noexcept {
    for (auto& section : sections)
      section.reset();
  }
  float process(float x) noexcept {
    for (auto& section : sections)
      x = section.process(x);
    return gain * x;
  }

private:
  std::array<fx::Biquad, 4> sections{};
  float gain{1.0f};
};

/** The springs, at their own (decimated) rate. */
class SpringTank {
public:
  void prepare(double tankRate) {
    rate = tankRate;
    int longest = 2;
    for (int j = 0; j < kSprings; ++j) {
      const double a = kDispersion[j];
      // The chain's own delay at low frequencies comes off the line, so the
      // round trip is the design value.
      const double chainDelay = kSections * (1.0 - a) / (1.0 + a);
      length[static_cast<size_t>(j)] =
          std::max(8, static_cast<int>(std::lround(kLoopSeconds[j] * rate - chainDelay)));
      longest = std::max(longest, length[static_cast<size_t>(j)]);
      // In the band a guitar fills (under 3 kHz) an allpass of the chain
      // delays by about 0.4 samples; the level and the shelf are worked out
      // for that trip length.
      chainSamples = kChainDelayInBand * kSections;
      loopSamples[static_cast<size_t>(j)] = length[static_cast<size_t>(j)] + chainSamples;
    }
    line.prepare(kSprings, longest + 2);
    corner = fx::reverb::shelfCorner(kDampingCornerHz, rate);
    reset();
  }

  void reset() noexcept {
    line.clear();
    for (auto& chain : state)
      chain.fill(0.0f);
    for (auto& shelf : shelves)
      shelf.reset();
    snapPending = true;
  }

  void clearSpring(int j) noexcept {
    // The delay line is flushed by tick() while the spring is off.
    state[static_cast<size_t>(j)].fill(0.0f);
    shelves[static_cast<size_t>(j)].reset();
  }

  /** Once per control interval: the loop gains for `rt60`, and the gain
      that brings each spring's tap to unit energy per unit of input. */
  void control(double rt60) noexcept {
    const float g = static_cast<float>(fx::reverb::decayGain(1.0, rt60, rate));
    if (snapPending)
      gamma.snap(g);
    else
      gamma.aim(g, kControlTicks);
    for (int j = 0; j < kSprings; ++j) {
      const size_t n = static_cast<size_t>(j);
      const double perTrip = fx::reverb::decayGain(loopSamples[n], rt60, rate);
      const float lineG = static_cast<float>(fx::reverb::decayGain(length[n], rt60, rate));
      const float high = static_cast<float>(
          fx::reverb::decayGain(loopSamples[n], rt60 * kHighRatio, rate) / perTrip);
      // The tap sees every trip once, after the chain (the line's own gain
      // is applied at its end, past the tap): its energy is the geometric
      // series over the trips.
      const double chain = fx::reverb::decayGain(chainSamples, rt60, rate);
      const double tapEnergy = chain * chain / (1.0 - perTrip * perTrip);
      const float norm = static_cast<float>(1.0 / std::sqrt(tapEnergy));
      if (snapPending) {
        lineGain[n].snap(lineG);
        shelfGain[n].snap(high);
        tapGain[n].snap(norm);
      } else {
        lineGain[n].aim(lineG, kControlTicks);
        shelfGain[n].aim(high, kControlTicks);
        tapGain[n].aim(norm, kControlTicks);
      }
    }
    snapPending = false;
  }

  /** One spring-rate sample in, each spring's (normalised) output in `out`. */
  void tick(float in, int activeSprings, std::array<float, kSprings>& out) noexcept {
    const float g = gamma.step();
    for (int j = 0; j < kSprings; ++j) {
      const size_t n = static_cast<size_t>(j);
      const float lineG = lineGain[n].step();
      const float high = shelfGain[n].step();
      const float norm = tapGain[n].step();
      if (j >= activeSprings) {
        // A spring that is switched off is flushed with silence, so it has
        // nothing old to replay when it comes back.
        line.write(j, 0.0f);
        out[n] = 0.0f;
        continue;
      }
      const float back = shelves[n].process(lineG * line.read(j, length[n]), high, corner);
      // The far end of a spring reflects inverted.
      float v = in - back;
      const float a = kDispersion[j];
      auto& chain = state[n];
      for (int m = 0; m < kSections; ++m) {
        const float stored = g * chain[static_cast<size_t>(m)];
        const float w = v - a * stored;
        chain[static_cast<size_t>(m)] = w;
        v = stored + a * w;
      }
      line.write(j, v);
      // The pickup is at the other end from the driver: half a trip away.
      out[n] = norm * line.read(j, length[n] / 2);
    }
  }

private:
  double rate{9600.0};
  fx::DelayLine line;
  std::array<int, kSprings> length{};
  std::array<double, kSprings> loopSamples{};
  double chainSamples{0.0};
  std::array<std::array<float, kSections>, kSprings> state{};
  std::array<fx::reverb::DampingShelf, kSprings> shelves{};
  Slide gamma;
  std::array<Slide, kSprings> lineGain{}, shelfGain{}, tapGain{};
  float corner{1.0f};
  bool snapPending{true};
};

class SpringReverbProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "reverb.spring";

  enum Param { kDecayS = 0, kTone, kDwell, kSpringsChoice, kSpillover };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::floatParam("decayS", "Decay", "s", 0.5f, 5.0f, 2.2f)
                      .withSkew(0.5f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("tone", "Tone", "%", 0.0f, 100.0f, 60.0f));
      t.push_back(BlockParamSpec::floatParam("dwell", "Dwell", "%", 0.0f, 100.0f, 50.0f));
      // The third spring is faded in and out, so the switch is click-free.
      t.push_back(BlockParamSpec::choiceParam("springs", "Springs",
                                              juce::StringArray{"2 springs", "3 springs"}, 1));
      t.push_back(spilloverParam());
      return t;
    }();
    return table;
  }

  SpringReverbProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The decay time is the RT60, plus one trip round the longest spring with
  // its dispersion: the echoes come in bursts a trip apart, and the last
  // burst over -60 dB can be that much late.
  double tailSeconds() const noexcept override {
    return static_cast<double>(param(kDecayS)) + kStructureSeconds;
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    const double tankRate = sampleRate / kDecimation;
    for (auto& core : cores) {
      core.down.prepare(0.45 * tankRate, sampleRate);
      for (auto& up : core.up)
        up.prepare(0.45 * tankRate, sampleRate);
      core.tank.prepare(tankRate);
    }
    // ControlSmoother counts ticks of the family's 32 samples; this kind's
    // are 40 long, which is the same as a lower rate.
    const double smootherRate =
        sampleRate * fx::reverb::kControlInterval / static_cast<double>(kControlInterval);
    rt60.prepare(smootherRate, 0.06);
    toneHz.prepare(smootherRate, 0.03);
    drive.prepare(smootherRate, 0.03);
    thirdSpring.prepare(smootherRate, 0.05);
    resetEffect();
  }

  void resetEffect() noexcept override {
    for (auto& core : cores)
      core.reset();
    rt60.snapOnNextTarget();
    toneHz.snapOnNextTarget();
    drive.snapOnNextTarget();
    thirdSpring.snapOnNextTarget();
    untilControl = 0;
    lastCores = 0;
    thirdRunning = true;
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxChannels);
    if (numChannels <= 0)
      return;
    // True stereo: one tank fed the sum of both inputs, its springs spread
    // over the two sides with opposite signs. Otherwise one whole mono tank
    // per channel and nothing crossing.
    const bool stereo = blockHasStereoImage(buffer.getNumChannels(), ctx);
    const int activeCores = stereo ? 1 : numChannels;
    if (activeCores > lastCores && lastCores != 0)
      cores[1].reset();
    lastCores = activeCores;

    float* left = buffer.getWritePointer(0);
    float* right = numChannels > 1 ? buffer.getWritePointer(1) : nullptr;
    int done = 0;
    while (done < numSamples) {
      if (untilControl == 0) {
        updateControls(activeCores);
        untilControl = kControlInterval;
      }
      const int n = juce::jmin(untilControl, numSamples - done);
      const int springs = thirdRunning ? 3 : 2;
      if (stereo) {
        for (int i = done; i < done + n; ++i)
          cores[0].process(0.5f * (left[i] + right[i]), springs, left[i], &right[i]);
      } else {
        for (int i = done; i < done + n; ++i)
          cores[0].process(left[i], springs, left[i], nullptr);
        if (right != nullptr)
          for (int i = done; i < done + n; ++i)
            cores[1].process(right[i], springs, right[i], nullptr);
      }
      done += n;
      untilControl -= n;
    }
  }

private:
  static constexpr int kMaxChannels = 2;
  static constexpr double kStructureSeconds = 0.1;

  struct Core {
    BandLimiter down;
    std::array<BandLimiter, 2> up{};
    SpringTank tank;
    std::array<fx::reverb::WetFilter, 2> wet{};
    // Per-sample ramps of the drive into the tank and its make-up.
    Slide driveGain, makeUp;
    // Per spring-sample ramps of the output weights of each spring per side.
    std::array<Slide, kSprings> leftWeight{}, rightWeight{};
    int phase{0};
    bool snapPending{true};

    void reset() noexcept {
      down.reset();
      for (auto& filter : up)
        filter.reset();
      tank.reset();
      for (auto& filter : wet)
        filter.reset();
      phase = 0;
      snapPending = true;
    }

    // `right` null: the mono tank (the left mix only).
    void process(float x, int springs, float& left, float* right) noexcept {
      // Dwell: how hard the driver pushes the springs. The transducer
      // saturates, so more dwell is a denser, more compressed splash rather
      // than only a louder one.
      const float driven = makeUp.step() * fx::fastTanh(driveGain.step() * x);
      const float limited = down.process(driven);
      float l = 0.0f, r = 0.0f;
      if (phase == 0) {
        std::array<float, kSprings> out{};
        tank.tick(limited, springs, out);
        for (int j = 0; j < kSprings; ++j) {
          l += leftWeight[static_cast<size_t>(j)].step() * out[static_cast<size_t>(j)];
          r += rightWeight[static_cast<size_t>(j)].step() * out[static_cast<size_t>(j)];
        }
        // One sample in kDecimation carries the signal; the reconstruction
        // filter spreads it, so it goes in kDecimation times as large.
        l *= static_cast<float>(kDecimation);
        r *= static_cast<float>(kDecimation);
      }
      phase = phase + 1 == kDecimation ? 0 : phase + 1;
      left = wet[0].process(up[0].process(l));
      if (right != nullptr)
        *right = wet[1].process(up[1].process(r));
    }
  };

  void updateControls(int activeCores) noexcept {
    // Slewed here (30 to 60 ms) and ramped between ticks in the cores: a
    // moving knob changes the loop gains and the output weights smoothly.
    const double decay = rt60.advance(param(kDecayS));
    // 0.5 to 8 times, exponentially; 2 at the default.
    const float d = drive.advance(0.5f * std::pow(16.0f, param(kDwell) * 0.01f));
    const float toneBefore = toneHz.current();
    const float tone = toneHz.advance(1200.0f * std::pow(4.5f, param(kTone) * 0.01f));
    const float third = thirdSpring.advance(param(kSpringsChoice) >= 0.5f ? 1.0f : 0.0f);

    // The third spring costs nothing while it is switched off. It starts
    // again from rest.
    const bool wantThird = third > 0.0f || param(kSpringsChoice) >= 0.5f;
    if (wantThird && !thirdRunning)
      for (auto& core : cores)
        core.tank.clearSpring(2);
    thirdRunning = wantThird;

    // The tank's taps arrive at unit energy each; the weights place them
    // (all in phase on the left, which is also the mono tank; the second
    // spring inverted on the right, so the two sides are uncorrelated and
    // their sum loses nothing) and set the level.
    const float level = static_cast<float>(std::sqrt(fx::reverb::wetEnergyTarget(decay)));
    const float leftNorm = level / std::sqrt(2.0f + third * third);
    const float rightNorm = level / std::sqrt(1.0f + (1.0f + third) * (1.0f + third) + third * third);
    const float leftW[kSprings] = {leftNorm, leftNorm, leftNorm * third};
    const float rightW[kSprings] = {rightNorm, -rightNorm * (1.0f + third), rightNorm * third};
    // Unity for small signals at the default dwell, rising 3 dB for each
    // doubling of the drive; the saturation holds the loud end back.
    const float makeUp = std::sqrt(0.5f / d);

    for (int i = 0; i < activeCores; ++i) {
      Core& core = cores[static_cast<size_t>(i)];
      core.tank.control(decay);
      for (int j = 0; j < kSprings; ++j) {
        const size_t n = static_cast<size_t>(j);
        if (core.snapPending) {
          core.leftWeight[n].snap(leftW[j]);
          core.rightWeight[n].snap(rightW[j]);
        } else {
          core.leftWeight[n].aim(leftW[j], kControlTicks);
          core.rightWeight[n].aim(rightW[j], kControlTicks);
        }
      }
      if (core.snapPending) {
        core.driveGain.snap(d);
        core.makeUp.snap(makeUp);
      } else {
        core.driveGain.aim(d, kControlInterval);
        core.makeUp.aim(makeUp, kControlInterval);
      }
      if (core.snapPending || tone != toneBefore)
        for (auto& filter : core.wet)
          filter.set(kLowCutHz, tone, sampleRate);
      core.snapPending = false;
    }
  }

  std::array<Core, kMaxChannels> cores;
  fx::reverb::ControlSmoother rt60, toneHz, drive, thirdSpring;
  double sampleRate{48000.0};
  int untilControl{0};
  int lastCores{0};
  bool thirdRunning{true};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(SpringReverb) {
  // Heard beside the dry signal: the dry stays at unity and a new block
  // brings the springs in at 27% under it.
  addEffectKind<SpringReverbProcessor>(registry, "Spring", BlockCategory::Reverb, 0.27f,
                                       BlockMixLaw::DryPreserving);
}

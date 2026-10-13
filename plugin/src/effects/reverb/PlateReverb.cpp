// reverb.plate: the plate topology Jon Dattorro published ("Effect Design,
// Part 1: Reverberator and Other Filters", JAES 1997): four allpass
// diffusers in series feed a tank of two halves that feed each other, each
// half a modulated allpass, a delay, a second allpass and a second delay;
// the output is fourteen taps along the tank's delays, seven per side.
// Lengths and tap positions are the paper's, scaled from its 29761 Hz.
//
// What differs from the paper is the decay. There one gain is applied twice
// per half and a low-pass sits after the first delay, which sets the decay
// time only indirectly. Here every delay in the tank, allpass delays
// included, is scaled by gamma^length (ReverbParts.h): the decay parameter
// is the RT60 exactly, and the damping is a shelf after each plain delay that
// takes the same law to a shorter RT60 above its corner.
#include <array>
#include <cmath>

#include "effects/EffectDsp.h"
#include "effects/dsp/Ducker.h"
#include "effects/reverb/ReverbDuck.h"
#include "effects/reverb/ReverbParts.h"

namespace {

using fx::reverb::kControlInterval;

// The tank's eight delays in loop order, in samples at the paper's rate.
enum Element { kApA = 0, kDelayA, kApB, kDelayB, kApC, kDelayC, kApD, kDelayD, kElements };
constexpr double kPaperRate = 29761.0;
constexpr int kPaperLength[kElements] = {672, 4453, 1800, 3720, 908, 4217, 2656, 3163};

// Output taps: which delay, how far into it, and the sign.
struct Tap {
  Element element;
  int position;
  float sign;
};
constexpr int kTapsPerSide = 7;
constexpr Tap kLeftTaps[kTapsPerSide] = {{kDelayC, 266, 1.0f},  {kDelayC, 2974, 1.0f},
                                         {kApD, 1913, -1.0f},   {kDelayD, 1996, 1.0f},
                                         {kDelayA, 1990, -1.0f}, {kApB, 187, -1.0f},
                                         {kDelayB, 1066, -1.0f}};
constexpr Tap kRightTaps[kTapsPerSide] = {{kDelayA, 353, 1.0f},  {kDelayA, 3627, 1.0f},
                                          {kApB, 1228, -1.0f},   {kDelayB, 2673, 1.0f},
                                          {kDelayC, 2111, -1.0f}, {kApD, 335, -1.0f},
                                          {kDelayD, 121, -1.0f}};

// "Decay diffusion" 1 and 2 of the paper, which suggests 0.70 and 0.50. An
// allpass passes k of its input at once and 1 - k^2 one delay later. With
// the paper's values the strongest single path once round the tank (straight
// through both first allpasses, delayed by both second ones) carries 0.28 of
// the amplitude, and the tail repeats audibly at that period, 0.67 s. At
// k = 0.62 the two shares are equal (k = 1 - k^2) and no path round the loop
// stands out: the strongest repeat left in the tail measures 0.20 against
// 0.29 (normalised autocorrelation, see the tests).
constexpr float kDiffusion1 = 0.62f;
constexpr float kDiffusion2 = 0.62f;

constexpr double kMaxModSeconds = 0.0008;
constexpr double kDampingCornerHz = 5000.0;

class PlateTank {
public:
  struct Control {
    double rt60{2.0};
    double highRatio{1.0};
    float modDepthSamples{0.0f};
  };

  void prepare(double newSampleRate) {
    sampleRate = newSampleRate;
    const double scale = sampleRate / kPaperRate;
    maxModSamples = static_cast<int>(std::ceil(kMaxModSeconds * sampleRate));
    fx::reverb::PrimeTable primes;
    primes.prepare(static_cast<int>(std::ceil(kPaperLength[kDelayA] * scale)) + 256);
    int longest = 0;
    totalSamples = 0.0;
    for (int e = 0; e < kElements; ++e) {
      // Primes, so the eight delays share no period.
      length[static_cast<size_t>(e)] =
          primes.atLeast(static_cast<int>(std::lround(kPaperLength[e] * scale)));
      longest = std::max(longest, length[static_cast<size_t>(e)]);
      totalSamples += length[static_cast<size_t>(e)];
    }
    line.prepare(kElements, longest + maxModSamples + 4);
    for (int t = 0; t < kTapsPerSide; ++t) {
      leftTap[static_cast<size_t>(t)] = tapPosition(kLeftTaps[t], scale);
      rightTap[static_cast<size_t>(t)] = tapPosition(kRightTaps[t], scale);
    }
    corner = fx::reverb::shelfCorner(kDampingCornerHz, sampleRate);
    lfo[0].setRate(0.83, sampleRate);
    lfo[1].setRate(1.07, sampleRate);
    reset();
  }

  void reset() noexcept {
    line.clear();
    for (auto& shelf : shelves)
      shelf.reset();
    lfo[0].reset(0.0);
    lfo[1].reset(0.3);
    snapPending = true;
    settled = false;
  }

  /** Once per control interval, before its samples. */
  void control(const Control& c) noexcept {
    const bool moved = c.rt60 != appliedRt60 || c.highRatio != appliedHighRatio;
    if (snapPending || moved || !settled) {
      const double highRt60 = c.rt60 * juce::jlimit(0.01, 1.0, c.highRatio);
      for (int e = 0; e < kElements; ++e) {
        const float g =
            static_cast<float>(fx::reverb::decayGain(length[static_cast<size_t>(e)], c.rt60, sampleRate));
        if (snapPending)
          gain[static_cast<size_t>(e)].snap(g);
        else
          gain[static_cast<size_t>(e)].aim(g);
      }
      // One shelf per plain delay, answering for that delay and the allpass
      // before it: above the corner the pair decays at the high RT60.
      for (int s = 0; s < 4; ++s) {
        const double pair = length[static_cast<size_t>(2 * s)] + length[static_cast<size_t>(2 * s + 1)];
        const float h = static_cast<float>(fx::reverb::decayGain(pair, highRt60, sampleRate) /
                                           fx::reverb::decayGain(pair, c.rt60, sampleRate));
        if (snapPending)
          shelfGain[static_cast<size_t>(s)].snap(h);
        else
          shelfGain[static_cast<size_t>(s)].aim(h);
      }
      measureOutputs(c.rt60);
      settled = !snapPending && !moved && rampsIdle();
      appliedRt60 = c.rt60;
      appliedHighRatio = c.highRatio;
    }
    const float depth = juce::jlimit(0.0f, static_cast<float>(maxModSamples), c.modDepthSamples);
    if (snapPending)
      modDepth.snap(depth);
    else
      modDepth.aim(depth);
    lfo[0].renormalise();
    lfo[1].renormalise();
    snapPending = false;
  }

  void process(float x, float& left, float& right) noexcept {
    lfo[0].advance();
    lfo[1].advance();
    const float depth = modDepth.step();

    // What each half hands the other: the end of its second delay.
    const float fromD = shelves[3].process(g(kDelayD) * line.read(kDelayD, len(kDelayD)),
                                           shelfGain[3].value, corner);
    const float fromB = shelves[1].process(g(kDelayB) * line.read(kDelayB, len(kDelayB)),
                                           shelfGain[1].value, corner);
    half(x + fromD, kApA, depth * lfo[0].sine, shelves[0], shelfGain[0].value);
    half(x + fromB, kApC, depth * lfo[1].sine, shelves[2], shelfGain[2].value);

    if (!settled) {
      for (auto& ramp : gain)
        ramp.step();
      for (auto& ramp : shelfGain)
        ramp.step();
    }

    float l = 0.0f, r = 0.0f;
    for (int t = 0; t < kTapsPerSide; ++t) {
      l += kLeftTaps[t].sign * line.read(kLeftTaps[t].element, leftTap[static_cast<size_t>(t)]);
      r += kRightTaps[t].sign * line.read(kRightTaps[t].element, rightTap[static_cast<size_t>(t)]);
    }
    left = l;
    right = r;
  }

  /**
   * Gains that bring the left and the right output to `energy` (the energy
   * of the impulse response) each.
   *
   * The tank is one loop. The input enters it at two points (the head of
   * each half), and what a tap picks up of an entry is what is left of it
   * on arrival, gamma^(2 x the delay in between), once per trip round the
   * loop for ever after. A tap inside an allpass reads its internal signal,
   * 1 / (1 - k^2) hotter in energy than what flows through. Counted per
   * side, because the two tap sets sit at different distances from the
   * entries: at a short decay one would otherwise be louder.
   */
  void outputGainsFor(double energy, float& left, float& right) const noexcept {
    left = static_cast<float>(std::sqrt(energy / std::max(1.0e-12, sideEnergy[0])));
    right = static_cast<float>(std::sqrt(energy / std::max(1.0e-12, sideEnergy[1])));
  }

private:
  static int tapPosition(const Tap& tap, double scale) noexcept {
    // 1 is the sample just written (the taps are read after the writes).
    return 1 + static_cast<int>(std::lround(tap.position * scale));
  }

  void measureOutputs(double rt60) noexcept {
    // Where each element starts along the loop.
    std::array<double, kElements> start{};
    for (int e = 1; e < kElements; ++e)
      start[static_cast<size_t>(e)] =
          start[static_cast<size_t>(e - 1)] + length[static_cast<size_t>(e - 1)];
    const double perTrip = fx::reverb::decayGain(totalSamples, rt60, sampleRate);
    const double trips = 1.0 / (1.0 - perTrip * perTrip);
    const double inAllpass = 1.0 / (1.0 - static_cast<double>(kDiffusion2) * kDiffusion2);
    const auto sideOf = [&](const Tap* taps) {
      double sum = 0.0;
      for (int t = 0; t < kTapsPerSide; ++t) {
        const Element e = taps[t].element;
        const bool allpass = e == kApB || e == kApD;
        for (const Element entry : {kApA, kApC}) {
          double between = start[static_cast<size_t>(e)] - start[static_cast<size_t>(entry)];
          if (between < 0.0)
            between += totalSamples;
          const double arrived = fx::reverb::decayGain(between, rt60, sampleRate);
          sum += (allpass ? inAllpass : 1.0) * arrived * arrived * trips;
        }
      }
      return sum;
    };
    sideEnergy = {sideOf(kLeftTaps), sideOf(kRightTaps)};
  }

  float g(Element e) const noexcept { return gain[static_cast<size_t>(e)].value; }
  int len(Element e) const noexcept { return length[static_cast<size_t>(e)]; }

  // One half of the tank from its first allpass on: modulated allpass,
  // delay, shelf, allpass, into the second delay.
  void half(float in, Element firstAllpass, float modOffset, fx::reverb::DampingShelf& shelf,
            float shelfHigh) noexcept {
    const Element delay = static_cast<Element>(firstAllpass + 1);
    const Element secondAllpass = static_cast<Element>(firstAllpass + 2);
    const Element secondDelay = static_cast<Element>(firstAllpass + 3);

    // readLagrange, because the moving read sits inside the loop (see
    // ReverbParts.h).
    float stored = g(firstAllpass) *
                   (modOffset != 0.0f
                        ? fx::reverb::readLagrange(line, firstAllpass,
                                                   static_cast<float>(len(firstAllpass)) + modOffset)
                        : line.read(firstAllpass, len(firstAllpass)));
    float v = in + kDiffusion1 * stored;
    line.write(firstAllpass, v);
    float y = stored - kDiffusion1 * v;

    const float delayed = shelf.process(g(delay) * line.read(delay, len(delay)), shelfHigh, corner);
    line.write(delay, y);

    stored = g(secondAllpass) * line.read(secondAllpass, len(secondAllpass));
    v = delayed - kDiffusion2 * stored;
    line.write(secondAllpass, v);
    y = stored + kDiffusion2 * v;
    line.write(secondDelay, y);
  }

  bool rampsIdle() const noexcept {
    for (const auto& ramp : gain)
      if (ramp.delta != 0.0f)
        return false;
    for (const auto& ramp : shelfGain)
      if (ramp.delta != 0.0f)
        return false;
    return true;
  }

  double sampleRate{48000.0};
  fx::DelayLine line;
  std::array<int, kElements> length{};
  std::array<int, kTapsPerSide> leftTap{};
  std::array<int, kTapsPerSide> rightTap{};
  std::array<fx::reverb::Ramped, kElements> gain{};
  std::array<fx::reverb::Ramped, 4> shelfGain{};
  std::array<fx::reverb::DampingShelf, 4> shelves{};
  std::array<fx::reverb::QuadratureLfo, 2> lfo{};
  fx::reverb::Ramped modDepth;
  float corner{1.0f};
  double totalSamples{1.0};
  // Energy each output collects per unit of input, before its gain.
  std::array<double, 2> sideEnergy{{1.0, 1.0}};
  int maxModSamples{0};
  double appliedRt60{0.0};
  double appliedHighRatio{0.0};
  bool snapPending{true};
  bool settled{false};
};

class PlateReverbProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "reverb.plate";

  enum Param { kDecayS = 0, kPreDelayMs, kDamping, kLowCutHz, kHighCutHz, kModulation, kDuckDb, kDuckReleaseMs,
         kSpillover };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      // The RT60 below the damping corner.
      t.push_back(BlockParamSpec::floatParam("decayS", "Decay", "s", 0.5f, 10.0f, 2.0f)
                      .withSkew(0.5f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("preDelayMs", "Pre-delay", "ms", 0.0f, 200.0f, 0.0f)
                      .withSkew(0.5f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("damping", "Damping", "%", 0.0f, 100.0f, 25.0f));
      t.push_back(BlockParamSpec::floatParam("lowCutHz", "Low cut", "Hz", 20.0f, 1000.0f, 120.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("highCutHz", "High cut", "Hz", 1000.0f, 20000.0f, 12000.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("modulation", "Modulation", "%", 0.0f, 100.0f, 30.0f));
      fx::appendDuckParams(t);
      t.push_back(spilloverParam());
      return t;
    }();
    return table;
  }

  PlateReverbProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The decay time is the RT60, after the pre-delay (plus its glide). The
  // tank is one loop of 0.73 s whose taps catch the sound in lumps as it
  // comes round, so the envelope wanders around the exponential: a quarter
  // of a loop covers that.
  double tailSeconds() const noexcept override {
    return static_cast<double>(param(kDecayS)) +
           static_cast<double>(param(kPreDelayMs)) * 0.001 + kPreDelayGlideSeconds +
           kStructureSeconds;
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    // The paper's input diffusers: 142, 107, 379 and 277 samples at its rate.
    const double seconds[4] = {142.0 / kPaperRate, 107.0 / kPaperRate, 379.0 / kPaperRate,
                               277.0 / kPaperRate};
    const float coeffs[4] = {0.75f, 0.75f, 0.625f, 0.625f};
    for (auto& core : cores) {
      core.preDelay.prepare(1, static_cast<int>(std::ceil(kMaxPreDelaySeconds * sampleRate)) + 4);
      core.preDelaySamples.prepare(sampleRate, kPreDelayGlideSeconds);
      core.diffuser.prepare(seconds, coeffs, 4, sampleRate);
      core.tank.prepare(sampleRate);
    }
    rt60.prepare(sampleRate, 0.06);
    highRatio.prepare(sampleRate, 0.06);
    modDepth.prepare(sampleRate, 0.1);
    lowCut.prepare(sampleRate, 0.03);
    highCut.prepare(sampleRate, 0.03);
    ducking.prepare(sampleRate);
    resetEffect();
  }

  void resetEffect() noexcept override {
    for (auto& core : cores)
      core.reset();
    rt60.snapOnNextTarget();
    highRatio.snapOnNextTarget();
    modDepth.snapOnNextTarget();
    lowCut.snapOnNextTarget();
    highCut.snapOnNextTarget();
    untilControl = 0;
    lastCores = 0;
    ducking.reset();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const bool stereo = blockHasStereoImage(buffer.getNumChannels(), ctx);
    ducking.run(buffer, stereo, param(kDuckDb), param(kDuckReleaseMs),
                [&](juce::AudioBuffer<float>& wet) { processWet(wet, stereo); });
  }

  void processWet(juce::AudioBuffer<float>& buffer, bool stereo) {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxChannels);
    if (numChannels <= 0)
      return;
    // True stereo: one tank fed the sum of both inputs, its two tap sets
    // left and right. Otherwise (mono, dual mono, in front of an amp model)
    // one whole mono plate per channel and nothing crossing.
    const int activeCores = stereo ? 1 : numChannels;
    if (activeCores > lastCores && lastCores != 0)
      cores[1].reset();
    lastCores = activeCores;

    float* left = buffer.getWritePointer(0);
    float* right = numChannels > 1 ? buffer.getWritePointer(1) : nullptr;
    int done = 0;
    while (done < numSamples) {
      // Coefficients move on a grid of their own, counted from the last
      // reset, so the output does not depend on the host's block size.
      if (untilControl == 0) {
        updateControls(activeCores);
        untilControl = kControlInterval;
      }
      const int n = juce::jmin(untilControl, numSamples - done);
      if (stereo) {
        for (int i = done; i < done + n; ++i)
          cores[0].processStereo(0.5f * (left[i] + right[i]), left[i], right[i]);
      } else {
        for (int i = done; i < done + n; ++i)
          left[i] = cores[0].processMono(left[i]);
        if (right != nullptr)
          for (int i = done; i < done + n; ++i)
            right[i] = cores[1].processMono(right[i]);
      }
      done += n;
      untilControl -= n;
    }
  }

private:
  static constexpr int kMaxChannels = 2;
  static constexpr double kMaxPreDelaySeconds = 0.2;
  // The pre-delay's read position glides to a new time instead of jumping.
  static constexpr double kPreDelayGlideSeconds = 0.05;
  static constexpr double kStructureSeconds = 0.18;

  struct Core {
    fx::DelayLine preDelay;
    fx::SmoothedParam preDelaySamples;
    fx::reverb::AllpassChain diffuser;
    PlateTank tank;
    std::array<fx::reverb::WetFilter, 2> wet{};
    fx::reverb::Ramped leftLevel, rightLevel;
    bool snapPending{true};

    void reset() noexcept {
      preDelay.clear();
      preDelaySamples.snapOnNextTarget();
      diffuser.clear();
      tank.reset();
      for (auto& filter : wet)
        filter.reset();
      snapPending = true;
    }

    void run(float x, float& l, float& r) noexcept {
      preDelay.write(0, x);
      // Whole samples at rest, so the linear read is exact there, and it may
      // sit at zero delay.
      const float delayed = preDelay.readLinear(0, 1.0f + preDelaySamples.next());
      tank.process(diffuser.process(delayed), l, r);
    }

    float processMono(float x) noexcept {
      float l = 0.0f, r = 0.0f;
      run(x, l, r);
      // Both tap sets: they share no tap, so their sum is the denser mono
      // plate at the level of either side.
      return wet[0].process(0.70710678f * (leftLevel.step() * l + rightLevel.step() * r));
    }

    void processStereo(float x, float& left, float& right) noexcept {
      float l = 0.0f, r = 0.0f;
      run(x, l, r);
      left = wet[0].process(leftLevel.step() * l);
      right = wet[1].process(rightLevel.step() * r);
    }
  };

  void updateControls(int activeCores) noexcept {
    PlateTank::Control c;
    // Decay and damping are slewed (60 ms) and then ramped per sample inside
    // the tank: a moving knob changes the loop gains smoothly.
    c.rt60 = rt60.advance(param(kDecayS));
    c.highRatio = highRatio.advance(static_cast<float>(fx::reverb::dampingToHighRatio(param(kDamping))));
    c.modDepthSamples = modDepth.advance(param(kModulation) * 0.01f) *
                        static_cast<float>(kMaxModSeconds * sampleRate);
    const float preDelay = std::round(param(kPreDelayMs) * 0.001f * static_cast<float>(sampleRate));
    const float lowBefore = lowCut.current(), highBefore = highCut.current();
    const float low = lowCut.advance(param(kLowCutHz));
    const float high = highCut.advance(param(kHighCutHz));
    const bool filtersMoved = low != lowBefore || high != highBefore;

    for (int i = 0; i < activeCores; ++i) {
      Core& core = cores[static_cast<size_t>(i)];
      core.preDelaySamples.setTarget(preDelay);
      core.tank.control(c);
      float leftGain = 0.0f, rightGain = 0.0f;
      core.tank.outputGainsFor(fx::reverb::wetEnergyTarget(c.rt60), leftGain, rightGain);
      if (core.snapPending) {
        core.leftLevel.snap(leftGain);
        core.rightLevel.snap(rightGain);
      } else {
        core.leftLevel.aim(leftGain);
        core.rightLevel.aim(rightGain);
      }
      if (core.snapPending || filtersMoved)
        for (auto& filter : core.wet)
          filter.set(low, high, sampleRate);
      core.snapPending = false;
    }
  }

  std::array<Core, kMaxChannels> cores;
  fx::reverb::WetDucking ducking;
  fx::reverb::ControlSmoother rt60, highRatio, modDepth, lowCut, highCut;
  double sampleRate{48000.0};
  int untilControl{0};
  int lastCores{0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(PlateReverb) {
  // Heard beside the dry signal: the dry stays at unity and a new block
  // brings the plate in at 27% under it.
  addEffectKind<PlateReverbProcessor>(registry, "Plate", BlockCategory::Reverb, 0.27f,
                                      BlockMixLaw::DryPreserving);
}

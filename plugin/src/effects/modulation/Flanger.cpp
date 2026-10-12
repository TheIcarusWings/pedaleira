// modulation.flanger: a short delay swept by a sine LFO (exponentially, so
// the notches climb in even musical steps) with feedback of either sign. The
// wet is the comb itself, input plus delayed tap, which is how a flanger
// pedal sums its dry and delayed paths; the common mix then fades between
// the clean signal and the full comb.
#include <algorithm>
#include <cmath>
#include "effects/EffectDsp.h"

namespace {

class FlangerProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "modulation.flanger";

  // The first three are the standard rate parameters (fx::appendRateParams).
  enum Param { kSync = 0, kDivision, kRateHz, kDepth, kManualMs, kFeedback, kWidth };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      fx::RateParamOptions rate;
      rate.minHz = 0.05f;
      rate.maxHz = 10.0f;
      rate.defaultHz = 0.3f;
      fx::appendRateParams(t, rate);
      // How far the sweep reaches, from nothing (a fixed comb) to 1.5 octaves
      // of delay either side of the manual setting.
      t.push_back(BlockParamSpec::floatParam("depth", "Depth", "%", 0.0f, 100.0f, 70.0f)
                      .withAffectsTail());
      // The centre of the sweep: the notches sit at odd multiples of
      // 1 / (2 * delay), so a short delay is a high, thin flange.
      t.push_back(BlockParamSpec::floatParam("manualMs", "Manual", "ms", 0.5f, 10.0f, 2.5f)
                      .withSkew(0.4f)
                      .withAffectsTail());
      // Negative feedback moves the resonances to the odd multiples: the
      // hollow, jet-like sound.
      t.push_back(BlockParamSpec::floatParam("feedback", "Feedback", "%", -95.0f, 95.0f, 50.0f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("width", "Width", "%", 0.0f, 100.0f, 50.0f));
      return t;
    }();
    return table;
  }

  FlangerProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The input is gone after the longest delay the sweep can reach, and what
  // feedback keeps in the loop is 60 dB down after as many more passes as it
  // takes; the damping in the loop only shortens it. Atomics and arithmetic.
  double tailSeconds() const noexcept override {
    const double feedback = std::abs(static_cast<double>(param(kFeedback))) * 0.01;
    const double manual = static_cast<double>(param(kManualMs));
    const double reach = manual * std::exp2(kSweepOctaves * static_cast<double>(param(kDepth)) * 0.01);
    const double longest = std::min(kMaxDelayMs, reach) * 0.001;
    double passes = 1.0;
    if (feedback > 0.0005)
      passes += std::ceil(std::log(0.001) / std::log(feedback));
    return longest * passes + 0.01;
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    line.prepare(juce::jmax(1, spec.numChannels),
                 static_cast<int>(std::ceil(kMaxDelayMs * 0.001 * sampleRate)) + 4);
    lfo.prepare(sampleRate);
    depth.prepare(sampleRate, 0.03);
    // A new manual setting bends the pitch for a moment, as turning the
    // knob on a real flanger does.
    manualMs.prepare(sampleRate, 0.06);
    feedback.prepare(sampleRate, 0.03);
    width.prepare(sampleRate, 0.1);
    for (auto& filter : damping)
      filter.setCutoff(kDampingHz, sampleRate);
    resetEffect();
  }

  void resetEffect() noexcept override {
    line.clear();
    for (auto& filter : damping)
      filter.reset();
    lfo.reset();
    depth.snapOnNextTarget();
    manualMs.snapOnNextTarget();
    feedback.snapOnNextTarget();
    width.snapOnNextTarget();
    gridCounter = 0;
    scaleUp = scaleDown = 1.0f;
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), line.numChannels(), kMaxChannels);
    const bool stereo = blockHasStereoImage(buffer.getNumChannels(), ctx);

    lfo.setRateHz(fx::resolveRateHz(param(kSync), param(kDivision), param(kRateHz), ctx.transport));
    if (param(kSync) >= 0.5f)
      lfo.syncPhaseToTransport(ctx.transport,
                               fx::noteDivisionBeats(static_cast<int>(std::lround(param(kDivision)))));
    depth.setTarget(param(kDepth) * 0.01f);
    manualMs.setTarget(param(kManualMs));
    feedback.setTarget(param(kFeedback) * 0.01f);
    width.setTarget(stereo ? param(kWidth) * 0.005f : 0.0f);

    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    const float samplesPerMs = static_cast<float>(sampleRate * 0.001);
    for (int i = 0; i < numSamples; ++i) {
      const float reach = depth.next() * kSweepOctaves;
      const float manual = manualMs.next();
      const float fb = feedback.next();
      const float offset = width.next();

      // The sweep is cut to what the line and the minimum delay allow, a
      // little at a time: the logarithms are not needed every sample. The
      // two halves are scaled separately, so a manual setting near an end
      // of the range still sweeps the other way.
      if (gridCounter == 0) {
        const float safeReach = juce::jmax(reach, 1.0e-6f);
        scaleUp = juce::jmin(1.0f, std::log2(static_cast<float>(kMaxDelayMs) / manual) / safeReach);
        scaleDown = juce::jmin(1.0f, std::log2(manual / kMinDelayMs) / safeReach);
      }
      gridCounter = (gridCounter + 1) & (kControlGrid - 1);

      // Keeps the level of the comb near the level of the input for noise,
      // whatever the feedback: the loop's impulse response has power
      // 1 + 1 / (1 - fb^2) for either sign.
      const float norm = 1.0f / std::sqrt(1.0f + 1.0f / (1.0f - fb * fb));

      for (int ch = 0; ch < numChannels; ++ch) {
        const float lfoValue = lfo.valueAt(ch == 1 ? offset : 0.0f);
        const float octaves =
            reach * lfoValue * (lfoValue >= 0.0f ? scaleUp : scaleDown);
        const float delay = juce::jlimit(2.0f, static_cast<float>(line.maxDelaySamples()),
                                         manual * samplesPerMs * std::exp2(octaves));
        const float in = channels[ch][i];
        const float tap = line.readHermite(ch, delay);
        // The feedback is darkened (a bucket-brigade flanger's loop is) and
        // soft-limited: the loop cannot run away at 95%, whatever comes in.
        const float returned = fx::softClip(fb * damping[ch].lowpass(tap));
        line.write(ch, in + returned);
        channels[ch][i] = (in + tap) * norm;
      }
      lfo.advance();
    }
  }

private:
  static constexpr int kMaxChannels = 2;
  static constexpr int kControlGrid = 16;
  static constexpr float kSweepOctaves = 1.5f;
  static constexpr float kMinDelayMs = 0.3f;
  static constexpr double kMaxDelayMs = 12.0;
  static constexpr double kDampingHz = 12000.0;

  fx::DelayLine line;
  fx::OnePole damping[kMaxChannels];
  fx::Lfo lfo;
  fx::SmoothedParam depth;
  fx::SmoothedParam manualMs;
  fx::SmoothedParam feedback;
  fx::SmoothedParam width;
  double sampleRate{48000.0};
  int gridCounter{0};
  // Share of the sweep that fits above and below the manual setting;
  // refreshed on the control grid.
  float scaleUp{1.0f};
  float scaleDown{1.0f};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(Flanger) {
  // The wet already contains the input (that is what makes the comb), so the
  // mix is a crossfade, and a full wet is a flanger at the hardware's
  // dry-plus-delayed setting.
  addEffectKind<FlangerProcessor>(registry, "Flanger", BlockCategory::Modulation, 1.0f,
                                  BlockMixLaw::Crossfade);
}

// modulation.vibe: a four-stage phase shifter whose stages are not swept by an
// LFO directly but through a lamp and a light-dependent resistor, the way the
// photocell phasers of the early 1970s were. The chain is
//
//   sine LFO -> lamp drive -> filament (thermal lag) -> light (square law)
//            -> photocell conductance (fast to brighten, slow to darken)
//            -> corner frequency of each stage (linear in the conductance)
//
// and every step of it shapes the sweep: the filament smooths the sine, the
// cell lags more on the way down than on the way up (so the sweep is
// lopsided: a quick rise, a lingering fall, more so the faster it runs), and
// the resistance curve is not a straight line of the light. The four stages
// are staggered (different capacitors on the real thing), so their corners
// sit an octave or so apart and the notches are wide and uneven rather
// than evenly spaced like a phaser's.
//
// Chorus mode adds the dry signal to the shifted one (the notches);
// vibrato mode plays the shifted signal alone, which bends the pitch with
// no notches. `intensity` is how far the lamp is driven; `width` puts the
// right side's LFO behind the left in a true stereo image.
#include <algorithm>
#include <cmath>
#include "effects/EffectDsp.h"

namespace {

class VibeProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "modulation.vibe";

  // The first three are the standard rate parameters (fx::appendRateParams).
  enum Param { kSync = 0, kDivision, kRateHz, kIntensity, kMode, kWidth };
  enum Mode { kChorus = 0, kVibrato };

  static const BlockReadingTable& readingSpecs() {
    // Where the first stage's corner is at the end of the block.
    static const BlockReadingTable table = {{"cornerHz", "Corner", "Hz", 20.0f, 20000.0f}};
    return table;
  }

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      fx::RateParamOptions rate;
      rate.name = "Speed";
      rate.minHz = 0.1f;
      rate.maxHz = 10.0f;
      rate.defaultHz = 1.2f;
      fx::appendRateParams(t, rate);
      t.push_back(BlockParamSpec::floatParam("intensity", "Intensity", "%", 0.0f, 100.0f, 70.0f));
      t.push_back(BlockParamSpec::choiceParam("mode", "Mode", juce::StringArray{"Chorus", "Vibrato"},
                                              kChorus));
      t.push_back(BlockParamSpec::floatParam("width", "Width", "%", 0.0f, 100.0f, 30.0f));
      return t;
    }();
    return table;
  }

  VibeProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs(), readingSpecs()) {}

  // The chain rings at its lowest corner (about 70 Hz): the response of four
  // first-order stages there fades in about (4 + 5) / tan(pi f / fs) samples,
  // 40 ms. A close upper bound, fixed.
  double tailSeconds() const noexcept override { return 0.06; }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    channelsPrepared = juce::jlimit(1, kMaxChannels, spec.numChannels);
    lfo.prepare(sampleRate);
    intensity.prepare(sampleRate, 0.05);
    width.prepare(sampleRate, 0.1);
    chorusShare.prepare(sampleRate, 0.03);
    lampCoeff = onePoleCoefficient(kFilamentSeconds);
    attackCoeff = onePoleCoefficient(kCellAttackSeconds);
    decayCoeff = onePoleCoefficient(kCellDecaySeconds);
    resetEffect();
  }

  void resetEffect() noexcept override {
    for (auto& channel : state)
      channel = ChannelState{};
    lfo.reset();
    intensity.snapOnNextTarget();
    width.snapOnNextTarget();
    chorusShare.snapOnNextTarget();
    gridCounter = 0;
    firstCornerHz = kStageDarkHz[0];
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), channelsPrepared);
    const bool stereo = blockHasStereoImage(buffer.getNumChannels(), ctx);

    lfo.setRateHz(fx::resolveRateHz(param(kSync), param(kDivision), param(kRateHz), ctx.transport));
    if (param(kSync) >= 0.5f)
      lfo.syncPhaseToTransport(ctx.transport,
                               fx::noteDivisionBeats(static_cast<int>(std::lround(param(kDivision)))));
    intensity.setTarget(param(kIntensity) * 0.01f);
    width.setTarget(stereo ? param(kWidth) * 0.005f : 0.0f);
    chorusShare.setTarget(param(kMode) < 0.5f ? 1.0f : 0.0f);

    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    const float pi = juce::MathConstants<float>::pi;
    const float fs = static_cast<float>(sampleRate);
    for (int i = 0; i < numSamples; ++i) {
      const float amount = intensity.next();
      const float offset = width.next();
      const float share = chorusShare.next();
      // Vibrato: the shifted signal alone. Chorus: the sum of both, brought
      // down by 3 dB so the level stays near the input's.
      const float shiftedGain = 1.0f - share + 0.70710678f * share;
      const float dryGain = 0.70710678f * share;
      const bool updateCoefficients = gridCounter == 0;
      gridCounter = (gridCounter + 1) & (kControlGrid - 1);

      for (int ch = 0; ch < numChannels; ++ch) {
        ChannelState& s = state[ch];
        const float wave = lfo.valueAt(ch == 1 ? offset : 0.0f);
        // The lamp is driven about its middle by `amount` of the full swing.
        const float drive = 0.5f + 0.5f * amount * wave;
        if (!s.primed) {
          s.filament = drive;
          s.target = s.conductance = lightToConductance(s.filament * s.filament);
          s.primed = true;
        }
        s.filament += lampCoeff * (drive - s.filament);
        // The resistance curve (a power) is worth evaluating on the control
        // grid only: the cell's own lag is many grid steps long.
        if (updateCoefficients)
          s.target = lightToConductance(s.filament * s.filament);
        // Faster to brighten than to darken.
        s.conductance +=
            (s.target > s.conductance ? attackCoeff : decayCoeff) * (s.target - s.conductance);

        if (updateCoefficients) {
          const float scale = 1.0f + (kSweepRatio - 1.0f) * s.conductance;
          for (int stage = 0; stage < kStages; ++stage) {
            const float hz = juce::jlimit(20.0f, 0.45f * fs, kStageDarkHz[stage] * scale);
            const float t = std::tan(pi * hz / fs);
            s.coefficient[stage] = (t - 1.0f) / (t + 1.0f);
            if (ch == 0 && stage == 0)
              firstCornerHz = hz;
          }
        }

        const float in = channels[ch][i];
        float v = in;
        for (int stage = 0; stage < kStages; ++stage) {
          const float y = s.coefficient[stage] * v + s.z[stage];
          s.z[stage] = v - s.coefficient[stage] * y;
          v = y;
        }
        channels[ch][i] = shiftedGain * v + dryGain * in;
      }
      lfo.advance();
    }
    // Where the first stage sits (the last grid update).
    setReading(0, firstCornerHz);
  }

private:
  static constexpr int kMaxChannels = 2;
  static constexpr int kStages = 4;
  static constexpr int kControlGrid = 16;
  // Time constants: the filament heats and cools alike, the cell brightens
  // in a few milliseconds and takes several times longer to let go.
  static constexpr double kFilamentSeconds = 0.015;
  static constexpr double kCellAttackSeconds = 0.006;
  static constexpr double kCellDecaySeconds = 0.05;
  // Corner of each stage with the cell dark, in Hz, and how far a fully lit
  // cell lifts them (the conductance is 1 then).
  static constexpr float kStageDarkHz[kStages] = {450.0f, 250.0f, 135.0f, 72.0f};
  static constexpr float kSweepRatio = 10.0f;
  // The cell's conductance against the light it sees: sub-linear (about the
  // 0.7 power), not a straight line.
  static constexpr float kCellExponent = 0.7f;

  struct ChannelState {
    float z[kStages] = {};
    float coefficient[kStages] = {-0.97f, -0.97f, -0.97f, -0.97f};
    float filament{0.5f};
    float conductance{0.0f};
    float target{0.0f};
    bool primed{false};
  };

  static float lightToConductance(float light) noexcept {
    return std::pow(juce::jlimit(0.0f, 1.0f, light), kCellExponent);
  }

  float onePoleCoefficient(double seconds) const noexcept {
    return static_cast<float>(1.0 - std::exp(-1.0 / (seconds * sampleRate)));
  }

  ChannelState state[kMaxChannels];
  fx::Lfo lfo;
  fx::SmoothedParam intensity;
  fx::SmoothedParam width;
  fx::SmoothedParam chorusShare;
  float lampCoeff{1.0f};
  float attackCoeff{1.0f};
  float decayCoeff{1.0f};
  float firstCornerHz{kStageDarkHz[0]};
  double sampleRate{48000.0};
  int channelsPrepared{2};
  int gridCounter{0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(Vibe) {
  // Chorus mode already holds the dry signal in the wet (that is where the
  // notches come from), so, as for the phaser, a crossfade from the clean
  // signal; fully wet by default.
  addEffectKind<VibeProcessor>(registry, "Vibe", BlockCategory::Modulation, 1.0f,
                               BlockMixLaw::Crossfade);
}

// modulation.chorus: two or three delay voices around 7 to 26 ms, each swept
// by the same sine LFO at its own phase and read with Hermite interpolation,
// then a high cut on the wet. In a true stereo image the right side reads
// the LFO at a phase offset (width), which is where the ensemble spread
// comes from.
#include <cmath>
#include "effects/EffectDsp.h"

namespace {

class ChorusProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "modulation.chorus";

  // The first three are the standard rate parameters (fx::appendRateParams).
  enum Param { kSync = 0, kDivision, kRateHz, kDepth, kVoices, kToneHz, kWidth };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      fx::RateParamOptions rate;
      rate.minHz = 0.05f;
      rate.maxHz = 10.0f;
      rate.defaultHz = 0.8f;
      fx::appendRateParams(t, rate);
      t.push_back(BlockParamSpec::floatParam("depth", "Depth", "%", 0.0f, 100.0f, 50.0f));
      t.push_back(BlockParamSpec::choiceParam("voices", "Voices", juce::StringArray{"2", "3"}, 0));
      t.push_back(BlockParamSpec::floatParam("toneHz", "Tone", "Hz", 1000.0f, 20000.0f, 8000.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("width", "Width", "%", 0.0f, 100.0f, 100.0f));
      return t;
    }();
    return table;
  }

  ChorusProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The wet is the voices alone, so it is over a voice's longest delay after
  // the input stops, plus the high cut settling. Fixed: no parameter moves
  // it by more than the 26 ms the line is sized for.
  double tailSeconds() const noexcept override { return kLongestDelayMs * 0.001 + 0.01; }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    line.prepare(juce::jmax(1, spec.numChannels),
                 static_cast<int>(std::ceil(kLongestDelayMs * 0.001 * sampleRate)) + 4);
    lfo.prepare(sampleRate);
    depth.prepare(sampleRate, 0.03);
    // The right side's LFO offset moves a delay, so it glides like a knob on
    // a tape chorus instead of jumping.
    width.prepare(sampleRate, 0.1);
    tone.prepare(sampleRate, 0.03);
    thirdVoice.prepare(sampleRate, 0.05);
    resetEffect();
  }

  void resetEffect() noexcept override {
    line.clear();
    for (auto& filter : highCut)
      filter.reset();
    lfo.reset();
    depth.snapOnNextTarget();
    width.snapOnNextTarget();
    tone.snapOnNextTarget();
    thirdVoice.snapOnNextTarget();
    gridCounter = 0;
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), line.numChannels(), kMaxChannels);
    // Spread only where there is a stereo image to put it in; everywhere
    // else both sides run the same mono chorus.
    const bool stereo = blockHasStereoImage(buffer.getNumChannels(), ctx);

    lfo.setRateHz(fx::resolveRateHz(param(kSync), param(kDivision), param(kRateHz), ctx.transport));
    if (param(kSync) >= 0.5f)
      lfo.syncPhaseToTransport(ctx.transport,
                               fx::noteDivisionBeats(static_cast<int>(std::lround(param(kDivision)))));
    depth.setTarget(param(kDepth) * 0.01f);
    // Offset of the right side in turns: 100% is opposite phase.
    width.setTarget(stereo ? param(kWidth) * 0.005f : 0.0f);
    tone.setTarget(param(kToneHz));
    thirdVoice.setTarget(param(kVoices) >= 0.5f ? 1.0f : 0.0f);

    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    const float samplesPerMs = static_cast<float>(sampleRate * 0.001);
    for (int i = 0; i < numSamples; ++i) {
      const float swing = depth.next() * kSwingOfCentre;
      const float offset = width.next();
      const float third = thirdVoice.next();
      const float cutoff = tone.next();
      if (gridCounter == 0)
        for (int ch = 0; ch < numChannels; ++ch)
          highCut[ch].setCutoff(cutoff, sampleRate);
      gridCounter = (gridCounter + 1) & (kControlGrid - 1);

      // Equal power over the voices: they are decorrelated by 10 ms or more.
      const float gain = 1.0f / std::sqrt(2.0f + third);
      for (int ch = 0; ch < numChannels; ++ch) {
        const float side = ch == 1 ? offset : 0.0f;
        float sum = 0.0f;
        for (int v = 0; v < kVoices3; ++v) {
          const float weight = v < 2 ? 1.0f : third;
          if (weight <= 0.0f)
            continue;
          const float centre = kCentreMs[v];
          const float delayMs = centre + centre * swing * lfo.valueAt(kVoicePhase[v] + side);
          sum += weight * line.readHermite(ch, delayMs * samplesPerMs);
        }
        line.write(ch, channels[ch][i]);
        channels[ch][i] = highCut[ch].lowpass(sum * gain);
      }
      lfo.advance();
    }
  }

private:
  static constexpr int kMaxChannels = 2;
  static constexpr int kVoices3 = 3;
  static constexpr int kControlGrid = 16;
  // Voice delays in ms and the LFO phase of each voice in turns. The phases
  // are not evenly spread on purpose: three voices a third of a turn apart
  // (or two opposite) would cancel each other's pitch wobble and leave the
  // sound stiff.
  static constexpr float kCentreMs[kVoices3] = {10.0f, 15.0f, 21.0f};
  static constexpr float kVoicePhase[kVoices3] = {0.0f, 0.25f, 0.5f};
  // The sweep at 100% depth, as a share of a voice's own delay: voices reach
  // from 7.5 to 26.25 ms and the deeper ones bend more.
  static constexpr float kSwingOfCentre = 0.25f;
  static constexpr double kLongestDelayMs = 26.25;

  fx::DelayLine line;
  fx::OnePole highCut[kMaxChannels];
  fx::Lfo lfo;
  fx::SmoothedParam depth;
  fx::SmoothedParam width;
  fx::SmoothedParam tone;
  fx::SmoothedParam thirdVoice;
  double sampleRate{48000.0};
  int gridCounter{0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(Chorus) {
  // Crossfade, default 50%: the voices are the same guitar at the same
  // level as the dry signal, so the classic 50/50 blend is a plain crossfade
  // and the level stays put as the knob moves. DryPreserving would play dry
  // and voices both at unity at 50% (3 to 6 dB louder than the input) and
  // spend the top half of the knob fading the dry out; a chorus replaces the
  // timbre of what goes in rather than adding something beside it, which is
  // what that law is for (delays, reverbs).
  addEffectKind<ChorusProcessor>(registry, "Chorus", BlockCategory::Modulation, 0.5f,
                                 BlockMixLaw::Crossfade);
}

// pitch.whammy "Pitch Bend": a pitch shift swept by a pedal. The interval at
// the heel (position 0 %) and at the toe (position 100 %) are set, and the
// position moves the shift between them: in a straight line in semitones, so
// halfway is halfway in pitch. The position is a parameter like any other, so
// it is what an expression pedal, a MIDI controller, a scene or host
// automation drives; it is smoothed here (a straight ramp of a few
// milliseconds on the interval) so a stepped controller, which moves a block at
// a time, does not make zipper noise, and a fast sweep still follows.
//
// Two modes:
//  - Classic: the shifted signal alone, as on the pedals this is modelled on
//    in spirit (a heel at 0 st passes the guitar through untouched).
//  - Detune-drop: the shifted signal blended half and half with the dry guitar,
//    for a detune (a few cents or semitones off the played note, thickening
//    it) and for a drop (an octave under the played note, beside it). The
//    blend is level neutral at 0 st.
//
// The engine is the pitch family's shifter (detail/ShiftPath.h) and its
// latency is its constant attack latency: an attack or an impulse arrives
// exactly `latencySamples()` after it went in, at every position of the pedal.
// A sustained note is, as in every shifter, delayed by a wandering amount; at
// exactly 0 st the whole signal is that late (the heel position of a pedal
// whose heel is 0 st is then an exact delay, bit for bit).
#include <cmath>

#include "effects/EffectDsp.h"
#include "detail/ShiftPath.h"

namespace {

class PitchBendProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "pitch.whammy";

  enum Param { kToe = 0, kHeel, kPosition, kMode };
  enum Reading { kShift = 0 };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::floatParam("toeSt", "Toe", "st", -24.0f, 24.0f, 12.0f).withStep(1.0f));
      t.push_back(BlockParamSpec::floatParam("heelSt", "Heel", "st", -24.0f, 24.0f, 0.0f).withStep(1.0f));
      // Meant for an expression pedal: 0 is heel down, 100 is toe down.
      t.push_back(BlockParamSpec::floatParam("position", "Position", "%", 0.0f, 100.0f, 0.0f));
      t.push_back(BlockParamSpec::choiceParam("mode", "Mode", {"Classic", "Detune-drop"}, 0));
      return t;
    }();
    return table;
  }

  static const BlockReadingTable& readingSpecs() {
    static const BlockReadingTable table = {
        {"shiftSt", "Shift", "st", -24.0f, 24.0f},
    };
    return table;
  }

  PitchBendProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs(), readingSpecs()) {}

  // The window is the engine's default and does not move, so neither does
  // the latency.
  int latencySamples() const noexcept override { return latch.latencySamples(); }
  double tailSeconds() const noexcept override { return latch.tailSeconds(); }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    latch.prepare(spec.sampleRate, kWindowIndex, kMaxSemitones);
    path.prepare(spec.sampleRate, spec.maxBlockSize, latch.current(), latch.latencySamples(),
                 kGlideSeconds, kUnisonSeconds);
  }

  void resetEffect() noexcept override { path.reset(); }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const float position = juce::jlimit(0.0f, 100.0f, param(kPosition)) * 0.01f;
    const float semitones = param(kHeel) + (param(kToe) - param(kHeel)) * position;
    // The mode only sets how much of the delayed dry is added; the path
    // smooths it, so the switch does not click.
    path.process(buffer, ctx.dualMono, semitones, latch.current(), latch.latencySamples(),
                 param(kMode) >= 0.5f ? 0.5f : 0.0f);
    setReading(kShift, path.currentSemitones());
  }

private:
  static constexpr float kWindowIndex = static_cast<float>(PitchShift::kDefaultWindow);
  // The position is smoothed by a ramp of the interval this long.
  static constexpr double kGlideSeconds = 0.005;
  // At unison the engine gives way to the delayed dry this quickly: a pedal
  // leaving its heel should not wait for it.
  static constexpr double kUnisonSeconds = 0.008;
  static constexpr double kMaxSemitones = 24.0;

  pitchfx::ShiftPath path;
  pitchfx::WindowLatch latch;
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(PitchBend) {
  // The shifted signal replaces the dry one by default, like the pedal.
  addEffectKind<PitchBendProcessor>(registry, "Pitch Bend", BlockCategory::Pitch, 1.0f,
                                    BlockMixLaw::Crossfade);
}

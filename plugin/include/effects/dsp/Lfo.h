#pragma once
#include <cmath>
#include <cstdint>
#include "BlockProcessor.h"
#include "effects/dsp/Noise.h"

namespace fx {

/**
 * A low-frequency oscillator for modulation effects: sine, triangle, or a
 * random sample-and-hold step, one value per sample in [-1, 1].
 *
 * One Lfo drives every channel of an effect. In a true stereo buffer (two
 * channels, ctx.dualMono off) the right channel reads the same oscillator at
 * a phase offset (valueAt), so the stereo spread is a parameter rather than
 * two oscillators drifting apart; in mono and in dual mono every channel
 * reads offset 0:
 *
 *   lfo.setRateHz(rateHz);                    // once per block
 *   for each sample:
 *     const float left = lfo.valueAt(0.0f);
 *     const float right = lfo.valueAt(stereoPhase);   // in turns, 0.25 = 90 degrees
 *     lfo.advance();
 *
 * Tempo: setRateFromTempo gives one cycle per `beatsPerCycle` quarter notes
 * (see TempoSync.h for note values), and syncPhaseToTransport pins the phase
 * to the song position while the host is playing, so a synced tremolo lands
 * on the beat and a render from the same position repeats exactly.
 *
 * Sine and triangle start at zero, rising. The sample-and-hold value is a
 * hash of the cycle number, so it needs no state, repeats between renders
 * and steps on both channels at their own phase. Audio thread; no
 * allocation.
 */
class Lfo {
public:
  enum class Shape { Sine, Triangle, SampleAndHold };

  void prepare(double newSampleRate) noexcept {
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    reset();
  }

  /** Back to the start of cycle 0, at `phaseTurns` (0 to 1). */
  void reset(double phaseTurns = 0.0) noexcept {
    phase = phaseTurns - std::floor(phaseTurns);
    cycle = 0;
  }

  void setShape(Shape newShape) noexcept { shape = newShape; }

  void setRateHz(double hz) noexcept { increment = (hz > 0.0 ? hz : 0.0) / sampleRate; }

  /** One cycle every `beatsPerCycle` quarter notes at `bpm`. */
  void setRateFromTempo(double bpm, double beatsPerCycle) noexcept {
    setRateHz(beatsPerCycle > 0.0 ? bpm / (60.0 * beatsPerCycle) : 0.0);
  }

  /** While the host timeline rolls, put the phase where the song position
      says it is (ppq 0 = phase 0). Call once per block before the samples;
      a no-op when there is no rolling transport, so the LFO free-runs at
      the tempo. */
  void syncPhaseToTransport(const BlockTransport& transport, double beatsPerCycle) noexcept {
    if (!transport.playing || beatsPerCycle <= 0.0)
      return;
    const double cycles = transport.ppqPosition / beatsPerCycle;
    const double whole = std::floor(cycles);
    phase = cycles - whole;
    cycle = static_cast<std::uint32_t>(static_cast<std::int64_t>(whole));
  }

  /** Seed of the sample-and-hold sequence (see effectNoiseSeed). */
  void setSeed(std::uint32_t newSeed) noexcept { seed = newSeed; }

  /** The value at the current phase plus `offsetTurns` (0 to 1), without
      advancing. */
  float valueAt(float offsetTurns = 0.0f) const noexcept {
    double p = phase + static_cast<double>(offsetTurns);
    std::uint32_t c = cycle;
    if (p >= 1.0) {
      p -= 1.0;
      ++c;
    }
    switch (shape) {
      case Shape::Triangle: {
        double q = p + 0.25;
        q -= std::floor(q);
        return static_cast<float>(1.0 - 4.0 * std::abs(q - 0.5));
      }
      case Shape::SampleAndHold: return hashToBipolar(seed ^ (c * 0x9e3779b9u));
      case Shape::Sine: break;
    }
    return static_cast<float>(std::sin(6.283185307179586 * p));
  }

  /** Step one sample. */
  void advance() noexcept {
    phase += increment;
    if (phase >= 1.0) {
      phase -= 1.0;
      ++cycle;
    }
  }

  /** valueAt(0) and advance, for the single-channel case. */
  float next() noexcept {
    const float value = valueAt(0.0f);
    advance();
    return value;
  }

  /** Current phase in turns, 0 to 1. */
  double phaseTurns() const noexcept { return phase; }

private:
  double sampleRate{48000.0};
  double phase{0.0};
  double increment{0.0};
  std::uint32_t cycle{0};
  std::uint32_t seed{0x1234abcdu};
  Shape shape{Shape::Sine};
};

}  // namespace fx

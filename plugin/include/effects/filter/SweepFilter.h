#pragma once
// The resonant filter voice the wah, the auto wah and the envelope filter
// share (their three .cpp files only differ in what moves the cutoff). One
// instance is one channel. Header only and private to the filter family.
#include <cmath>
#include "effects/dsp/Filters.h"

namespace fxfilter {

// How much of the low-pass output the wah voicing keeps next to the
// band-pass. A bare band-pass has no bass at all, which is not what a wah
// pedal does; a third of the low-pass leaves the low end under the peak at
// a believable level and moves the peak by under 1% of its frequency.
inline constexpr float kWahLowpassMix = 0.3f;

/** Q for a 0 to 100 % resonance control: 1.5 (gentle) to 9 (a sharp,
    vocal peak). */
inline float resonanceToQ(float resonancePercent) noexcept {
  return 1.5f + 0.075f * resonancePercent;
}

/** Seconds a ringing resonance takes to fall 60 dB at cutoff `hz` and
    quality `q` (a 12 dB per octave section decays with time constant
    q / (pi * hz)). An upper bound for a tail, taken at the lowest cutoff
    the sweep can reach. */
inline double resonanceDecaySeconds(double hz, double q) noexcept {
  return 6.9078 * q / (3.14159265358979 * (hz > 1.0 ? hz : 1.0));
}

class SweepFilter {
public:
  enum class Voice { Wah, Lowpass, Bandpass };

  void reset() noexcept { svf.reset(); }

  /** Cutoff / centre in Hz and quality factor. Cheap enough for a control
      tick every 16 samples (one tan()). */
  void set(double hz, double q, double sampleRate, Voice newVoice) noexcept {
    svf.set(hz, q, sampleRate);
    const float qf = static_cast<float>(q < 0.3 ? 0.3 : q);
    voice = newVoice;
    inverseQ = 1.0f / qf;
    switch (voice) {
      case Voice::Wah: {
        // The peak of (mix * low-pass + band-pass) is sqrt(1 + (mix Q)^2);
        // scaling by its inverse puts it at +6 dB whatever the resonance, so
        // turning the resonance up narrows the peak instead of making it
        // louder.
        const float lowMixQ = kWahLowpassMix * qf;
        gain = 2.0f / std::sqrt(1.0f + lowMixQ * lowMixQ);
        break;
      }
      case Voice::Bandpass: gain = 2.0f; break;  // +6 dB at the centre
      case Voice::Lowpass:
        // Unity at DC, a resonant peak of about q/sqrt(q/2): the peak is
        // tamed so a high resonance does not jump out of the mix.
        gain = 1.0f / std::sqrt(0.5f + 0.5f * qf);
        break;
    }
  }

  float process(float x) noexcept {
    const auto out = svf.process(x);
    switch (voice) {
      case Voice::Wah: return gain * (kWahLowpassMix * out.low + inverseQ * out.band);
      case Voice::Bandpass: return gain * inverseQ * out.band;
      case Voice::Lowpass: break;
    }
    return gain * out.low;
  }

private:
  fx::StateVariableFilter svf;
  Voice voice{Voice::Wah};
  float inverseQ{0.2f};
  float gain{1.0f};
};

}  // namespace fxfilter

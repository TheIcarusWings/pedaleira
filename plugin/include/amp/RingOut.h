#pragma once
// amp/RingOut.h: how long an amp keeps sounding after its input stops, read
// from its description.
//
// What rings. After a loud passage the amp is not at rest: the coupling
// capacitors hold charge (and, once a grid conducts, a bias shift that leaks
// away through the grid leak), the cathode bypass and the supply recover, the
// output transformer's low corner and the speaker-resonance branch ring, and the
// tone stack, a passive RC network, relaxes. All of that is linear and slow
// compared with the notes, so every one of them is an exponential, with a time
// constant tau that the description fixes:
//
//   input network, interstage links   C x (series + grid leak), in the slowest
//                                     voicing (the gain rise only speeds it up)
//   cathode bypass                    R x C
//   supply sag                        the recovery of each voicing's supply
//                                     (it scales the headroom, so it rings
//                                     nothing by itself: counted on its own,
//                                     see below)
//   output transformer                1 / (2 pi corner)
//   speaker resonance (depth)         Q / (pi f0)
//   tone cut                          C x (the resistances in the branch)
//   tone stack                        a1 of the stack's denominator, with
//                                     the bass and middle pots at the end that
//                                     is slowest. The stack is a passive RC
//                                     ladder, so its poles are real and
//                                     negative and a1 is the SUM of their time
//                                     constants: a bound on the slowest, with
//                                     no root finding.
//
// Why a multiple of the slowest. A hard-driven amp holds a very large offset
// (volts, against the millivolts that are -60 dB of its output), and the power
// stage stays saturated until the slowest of these has brought that offset
// down: the output is within -60 dB of the burst after about ln(offset /
// threshold) time constants. Measured on every shipped amp at settings from
// clean to everything at maximum, that is 6 to 11 time constants of the
// slowest; kDecayConstants (ln of a million, 13.8) is the bound above that. The
// fitted amps are the ones that need it: their tone stacks have a time
// constant of 0.2 to 0.3 s, which is 2 to 4 seconds of ring, where the
// half second every amp used to report was enough only for the textbook ones.
//
// The sag recovery is held to a shorter multiple, kRingOutSagConstants (ln of a
// thousand): it only scales what is still sounding, a headroom that has
// recovered to within a thousandth is a gain that has stopped mattering.
//
// The value is a bound, not a prediction: it does not depend on the knobs or
// the voicing (the longest of all of them), so the block can report it without
// recomputing as they move. An amp whose power stage oscillates by itself at
// extreme settings has no tail to report; that is the amp's, not this bound's.
//
// Plain C++ and std only. Not for the audio thread (it is a few hundred
// multiplications; call it when the description is set, and cache it).
#include <algorithm>
#include <cmath>

#include "amp/AmpDescription.h"

namespace amp {

/** Time constants of exponential decay the bound allows for: ln(1e6). */
constexpr double kRingOutDecayConstants = 13.8155;
/** The shortest tail an amp reports, seconds (the oversampler and the output
    filters, and the tail every amp had before it was derived). */
constexpr double kRingOutFloorSeconds = 0.5;
/** The same for the supply sag's recovery: ln(1e3). */
constexpr double kRingOutSagConstants = 6.9078;
/** The longest, seconds (the chain's own cap, kMaxBlockTailSeconds). */
constexpr double kRingOutCeilingSeconds = 30.0;

/** The slowest time constant of the described amp's signal path, seconds
    (the sag recovery is apart: sagRecoverySeconds). */
inline double slowestTimeConstant(const AmpDescription& d) noexcept {
  constexpr double kPi = 3.14159265358979323846;
  double slowest = 0.0;
  const auto consider = [&slowest](double tau) {
    if (std::isfinite(tau) && tau > slowest)
      slowest = tau;
  };

  consider(d.input.network.leakSeconds());

  for (int k = 0; k + 1 < d.numStages; ++k) {
    const GainStageSpec& spec = d.stages[k].spec;
    if (spec.bypass == CathodeBypass::Capacitor)
      consider(static_cast<double>(spec.triode.cathodeResistor) * spec.bypassFarads);
    for (int v = 0; v < std::max(1, d.numVoicings); ++v) {
      CouplingNetwork net = spec.coupling;
      if (v < d.numVoicings && d.voicings[v].couplingFarads[k] > 0.0f)
        net.capacitance = d.voicings[v].couplingFarads[k];
      consider(net.leakSeconds());
    }
  }
  if (d.numStages > 0) {
    const GainStageSpec& last = d.stages[d.numStages - 1].spec;
    if (last.bypass == CathodeBypass::Capacitor)
      consider(static_cast<double>(last.triode.cathodeResistor) * last.bypassFarads);
  }

  if (d.power.transformerLowCornerHz > 0.0f)
    consider(1.0 / (2.0 * kPi * d.power.transformerLowCornerHz));
  const PowerAmpParams& p = d.power.params;
  const double depthHz = p.depthHz();
  if (depthHz > 0.0)
    consider(static_cast<double>(p.depthQ()) / (kPi * depthHz));
  if (d.power.hasToneCut) {
    const ToneCutParams& cut = d.power.toneCut;
    consider(static_cast<double>(cut.capacitance) *
             (cut.sourceResistance + cut.potResistance + cut.minResistance));
  }

  // The tone stack: the sum of its time constants at the slowest pot positions
  // (a1 grows with the bass and the middle; the treble pot is not in it).
  consider(designToneStack(d.tone.parts, 0.0, 1.0, 1.0, 48000.0).analogDen[1]);
  return slowest;
}

/** The slowest supply recovery of the described amp (the default supply and
    every voicing's), seconds. */
inline double sagRecoverySeconds(const AmpDescription& d) noexcept {
  double slowest = sagParamsFor(d.power.supply).releaseSeconds;
  for (int v = 0; v < d.numVoicings; ++v)
    slowest = std::max(slowest, static_cast<double>(sagParamsFor(d.voicings[v].supply).releaseSeconds));
  return std::isfinite(slowest) ? slowest : 0.0;
}

/** The tail the amp reports, seconds: kRingOutDecayConstants of the slowest
    time constant, or kRingOutSagConstants of the slowest sag recovery if that
    is longer, between kRingOutFloorSeconds and kRingOutCeilingSeconds. */
inline double ringOutSeconds(const AmpDescription& description) noexcept {
  const double seconds = std::max(kRingOutDecayConstants * slowestTimeConstant(description),
                                  kRingOutSagConstants * sagRecoverySeconds(description));
  return std::clamp(seconds, kRingOutFloorSeconds, kRingOutCeilingSeconds);
}

}  // namespace amp

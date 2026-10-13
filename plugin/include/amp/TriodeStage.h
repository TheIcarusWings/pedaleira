#pragma once
// amp/TriodeStage.h: one common-cathode triode gain stage as a fast static
// waveshaper with a drive-dependent bias shift, described by its circuit.
//
// What it models: a 12AX7-class voltage amplifier stage, reduced to the
// behaviours that matter for guitar tone:
//   1. a smooth, asymmetric transfer curve: grid conduction on the positive
//      grid swing saturates at a lower level (posLimit, the bias voltage),
//      cutoff on the negative swing later (negLimit) (triodeCurve), both with
//      the soft x / sqrt(1 + x^2) shape;
//   2. a grid-leak bias shift: when the grid is driven into conduction the
//      coupling capacitor charges through the source and the conducting grid,
//      which moves the operating point toward cutoff, and it leaks away
//      through the grid leak ("blocking", and the way the asymmetry follows
//      the drive). It is a small DC-tracking state, not a static curve;
//   3. the stage gain, mu * Ra / (Ra + rp).
// It is deliberately not a circuit solver. The curve is an algebraic stand-in
// whose shape follows the plate characteristics described in N. Koren,
// "Improved vacuum tube models for SPICE simulations" (2003); the grid
// conduction and bias-shift behaviour follows the survey in J. Pakarinen and
// D. T. Yeh, "A review of digital techniques for modeling vacuum-tube guitar
// amplifiers", Computer Music Journal 33(2), 2009. A Koren or wave digital
// filter triode can replace this class: the interface is prepare, setCoupling,
// reset and `float process(float gridVolts)`, with the circuit described by
// TriodeCircuit (tube, plate load, cathode resistor, bias current).
//
// Aliasing: the curve is clipped hard by a high-gain preamp, so the stage
// uses first-order antiderivative anti-aliasing (J. D. Parker, V. Zavalishin,
// E. Le Bivic, "Reducing the aliasing of nonlinear waveshaping using
// continuous-time convolution", DAFx-16, 2016; and S. Bilbao, F. Esqueda,
// J. D. Parker, V. Valimaki, "Antiderivative antialiasing for stateful
// systems", 2017): the output is the slope of the curve's antiderivative
// between the previous and the current input, which has a closed form for
// this shape. That is on top of running the stage oversampled.
//
// Everything is in volts. The input is the signal at the grid (after the
// coupling network and any divider), the output is the plate voltage swing.
//
// TriodeCircuit (the physical description):
//   tube                    mu and rp of the tube type (k12AX7 below)
//   plateLoad               plate resistor Ra, ohms
//   cathodeResistor, biasCurrent   the bias is their product (a cathode bias)
//   cutoffGridVolts         magnitude of the grid voltage that cuts the plate
//                           current off (a catalogue value; about 3.5 V for a
//                           12AX7 at a 250 V supply)
//   contactPotential        grid conduction starts this far below Vgk = 0
//   gridConductionResistance  resistance of the conducting grid-cathode diode
//   maxBiasShiftVolts       numerical ceiling of the bias shift
// From these: posLimit = the bias voltage (the grid swing up to Vgk = 0),
// negLimit = cutoff minus bias, gridOnset = bias minus the contact potential.
//
// Coupling (setCoupling): the time constants of the network feeding the grid.
//   leakSeconds    the coupling capacitor times the resistance it discharges
//                  through (series plus grid leak): how fast the shift leaks
//   chargeSeconds  the capacitor times the resistance it charges through when
//                  the grid conducts (source + series + conducting grid)
//
// Sample rate: prepare(sampleRate) fixes the per-sample coefficients; call
// setCoupling after it (and again when the capacitor changes). The curve is
// memoryless: run it oversampled to keep its harmonics from folding back.
//
// Plain C++ and std only. No allocation; safe on the audio thread.
#include <algorithm>
#include <cmath>

#include "amp/FastMath.h"

namespace amp {

struct TubeType {
  float mu;  // amplification factor
  float rp;  // plate resistance at the operating point, ohms
};
/** Small-signal constants of a 12AX7 / ECC83 at a typical operating point
    (manufacturer datasheet values). */
inline constexpr TubeType k12AX7{100.0f, 62.5e3f};

struct TriodeCircuit {
  TubeType tube{k12AX7};
  float plateLoad{100.0e3f};
  float cathodeResistor{1.5e3f};
  float biasCurrent{1.0e-3f};
  float cutoffGridVolts{3.5f};
  float contactPotential{0.3f};
  float gridConductionResistance{2.0e3f};
  float maxBiasShiftVolts{10.0f};

  /** Voltage gain of the stage with the cathode bypassed. */
  float amplification() const noexcept { return tube.mu * plateLoad / (plateLoad + tube.rp); }
  /** Gain with the cathode resistor unbypassed relative to bypassed. */
  float unbypassedRatio() const noexcept {
    return (plateLoad + tube.rp) / (plateLoad + tube.rp + (tube.mu + 1.0f) * cathodeResistor);
  }
  float biasVolts() const noexcept { return biasCurrent * cathodeResistor; }
  float posLimit() const noexcept { return std::max(0.1f, biasVolts()); }
  float negLimit() const noexcept { return std::max(0.1f, cutoffGridVolts - biasVolts()); }
  float gridOnset() const noexcept { return std::max(0.05f, biasVolts() - contactPotential); }
  /** The impedance the next network sees: Ra in parallel with rp. */
  float sourceImpedance() const noexcept { return plateLoad * tube.rp / (plateLoad + tube.rp); }
};

/** Asymmetric soft clipper with unity slope at 0: Lp s(x / Lp) for x >= 0 and
    Ln s(x / Ln) for x < 0, with s(t) = t / sqrt(1 + t^2). Bounded by the two
    limits; both even and odd harmonics appear once the signal reaches the
    knees. */
inline float triodeCurve(float x, float posLimit, float negLimit) noexcept {
  if (x >= 0.0f) {
    const float t = std::min(x * recip(posLimit), 1.0e3f);
    return posLimit * t * invSqrt(1.0f + t * t);
  }
  const float t = std::max(x * recip(negLimit), -1.0e3f);
  return negLimit * t * invSqrt(1.0f + t * t);
}

/** The antiderivative of triodeCurve from 0: Lp^2 (sqrt(1 + t^2) - 1) on the
    positive side and Ln^2 (sqrt(1 + t^2) - 1) on the negative side (the two
    meet at 0 with value 0). */
inline float triodeCurveIntegral(float x, float posLimit, float negLimit, float invPosLimit,
                                 float invNegLimit) noexcept {
  const float limit = x >= 0.0f ? posLimit : negLimit;
  const float t = std::clamp(x * (x >= 0.0f ? invPosLimit : invNegLimit), -1.0e3f, 1.0e3f);
  const float a = 1.0f + t * t;
  return limit * limit * (a * invSqrt(a) - 1.0f);
}

class TriodeStage {
public:
  void prepare(double newSampleRate, const TriodeCircuit& newCircuit) noexcept {
    sampleRate = newSampleRate;
    circuit = newCircuit;
    amplification = circuit.amplification();
    posLimit = circuit.posLimit();
    negLimit = circuit.negLimit();
    invPosLimit = 1.0f / posLimit;
    invNegLimit = 1.0f / negLimit;
    gridOnset = circuit.gridOnset();
    smallestStep = 1.0e-3f * std::min(posLimit, negLimit);
    setCoupling(0.01, 0.001);
    reset();
  }

  /** Time constants of the coupling network (see the top of the file). */
  void setCoupling(double leakSeconds, double chargeSeconds) noexcept {
    release = static_cast<float>(1.0 - std::exp(-1.0 / (std::max(1.0e-5, leakSeconds) * sampleRate)));
    attack = static_cast<float>(1.0 / (std::max(1.0e-6, chargeSeconds) * sampleRate));
    attack = std::min(attack, 0.5f);
  }
  void reset() noexcept {
    shift = 0.0f;
    lastGrid = 0.0f;
    lastIntegral = 0.0f;
  }

  /** `gridVolts` in, plate swing in volts out. */
  float process(float gridVolts) noexcept {
    const float grid = gridVolts - shift;
    // First-order antiderivative anti-aliasing; for a step too small to
    // divide by safely the curve is read at the midpoint.
    const float integral = triodeCurveIntegral(grid, posLimit, negLimit, invPosLimit, invNegLimit);
    const float step = grid - lastGrid;
    const float shaped = std::abs(step) > smallestStep
                             ? (integral - lastIntegral) * recip(step)
                             : triodeCurve(0.5f * (grid + lastGrid), posLimit, negLimit);
    lastGrid = grid;
    lastIntegral = integral;
    const float y = amplification * shaped;
    const float over = grid > gridOnset ? grid - gridOnset : 0.0f;
    shift += attack * over - release * shift;
    shift = std::min(shift, circuit.maxBiasShiftVolts);
    if (shift < 1.0e-20f)
      shift = 0.0f;
    return y;
  }

  /** The current bias shift (0 at rest), volts at the grid. */
  float biasShift() const noexcept { return shift; }

private:
  TriodeCircuit circuit;
  double sampleRate{48000.0};
  float amplification{1.0f}, posLimit{1.0f}, negLimit{1.0f}, gridOnset{0.5f};
  float invPosLimit{1.0f}, invNegLimit{1.0f};
  float attack{0.0f}, release{0.0f};
  float shift{0.0f};
  float smallestStep{1.0e-3f};
  float lastGrid{0.0f}, lastIntegral{0.0f};
};

}  // namespace amp

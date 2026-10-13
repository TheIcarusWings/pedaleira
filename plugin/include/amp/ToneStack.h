#pragma once
// amp/ToneStack.h: the classic passive three-knob tone stack (treble, middle,
// bass) as a third-order filter, with the interaction between the knobs.
//
// What it models: the passive RC network of a "TMB" tone stack: a treble
// capacitor C1 and pot R1, a bass pot R2 with capacitor C2, a middle pot R3
// with capacitor C3, and a slope resistor R4. It has an insertion loss and a
// mid dip that moves with the knobs; the controls interact (the bass and
// middle settings change the treble response and so on), which is why it is
// a single third-order transfer function and not three independent EQ bands.
//
// Reference: the closed-form analysis of this network is by D. T. Yeh and
// J. O. Smith, "Discretization of the '59 Fender Bassman Tone Stack",
// Proc. DAFx-06, 2006 (and D. T. Yeh's 2009 Stanford thesis, "Digital
// Implementation of Musical Distortion Circuits by Analysis and
// Simulation"). The transfer function is
//
//   H(s) = (b1 s + b2 s^2 + b3 s^3) / (1 + a1 s + a2 s^2 + a3 s^3)
//
// with the coefficients in terms of the component values and the pot
// positions t (treble), m (middle), l (bass), each 0..1. It is discretised
// here with the bilinear transform (no prewarp; at the oversampled rates the
// amp runs at the warp is negligible below 10 kHz).
//
// The third-order coefficients (the s^3 terms of numerator and denominator)
// of an earlier version of this file were wrong: they did not match the
// network, gave complex poles at about two knob settings in a hundred (which a
// passive RC network cannot have) and were up to 1 dB off the true response.
// They were re-derived from the netlist and are now checked against
// WdfToneStack.h, which solves the same network by wave digital filtering
// (amp_wdf_tests.cpp: within 0.5 dB at several knob settings, and the poles of
// random stacks are real and negative).
//
// Parameters (ToneStackParts): R1 treble pot, R2 bass pot, R3 middle pot,
// R4 slope resistor in ohms; C1, C2, C3 in farads. The controls are the pot
// positions; a log taper on the bass pot is applied by the caller.
//
// Sample rate: design() takes the rate the filter will run at and computes in
// double precision; the filter keeps double state because the low poles are
// very close to z = 1 at oversampled rates. Update the coefficients as often
// as the knobs move (a few hundred times a second is plenty; a smoothed knob
// should be re-designed per control tick). One design can be shared by all
// channels; each channel owns a ToneStackFilter.
//
// Plain C++ and std only. design() does no allocation.
#include <cmath>
#include <complex>

namespace amp {

struct ToneStackParts {
  double r1{220.0e3};   // treble pot
  double r2{1.0e6};     // bass pot
  double r3{22.0e3};    // middle pot
  double r4{39.0e3};    // slope resistor
  double c1{400.0e-12};
  double c2{22.0e-9};
  double c3{22.0e-9};
};

/** The analogue polynomials in s and the digital (z^-1) polynomials. */
struct ToneStackDesign {
  double analogNum[4]{0.0, 0.0, 0.0, 0.0};  // b0..b3 (b0 is 0)
  double analogDen[4]{1.0, 0.0, 0.0, 0.0};  // a0..a3
  double b[4]{0.0, 0.0, 0.0, 0.0};          // digital numerator, z^0..z^-3
  double a[4]{1.0, 0.0, 0.0, 0.0};          // digital denominator, a[0] == 1

  /** Magnitude of the analogue prototype at `hz`. */
  double analogMagnitude(double hz) const noexcept {
    const std::complex<double> s(0.0, 2.0 * 3.141592653589793238 * hz);
    std::complex<double> n = 0.0, d = 0.0, power = 1.0;
    for (int k = 0; k < 4; ++k) {
      n += analogNum[k] * power;
      d += analogDen[k] * power;
      power *= s;
    }
    return std::abs(n / d);
  }

  /** Magnitude of the discretised filter at `hz` for rate `sampleRate`. */
  double digitalMagnitude(double hz, double sampleRate) const noexcept {
    const std::complex<double> zinv = std::polar(1.0, -2.0 * 3.141592653589793238 * hz / sampleRate);
    std::complex<double> n = 0.0, d = 0.0, power = 1.0;
    for (int k = 0; k < 4; ++k) {
      n += b[k] * power;
      d += a[k] * power;
      power *= zinv;
    }
    return std::abs(n / d);
  }
};

/** Designs the stack for pot positions `t`, `m`, `l` (0..1) at `sampleRate`. */
inline ToneStackDesign designToneStack(const ToneStackParts& parts, double t, double m, double l,
                                       double sampleRate) noexcept {
  const double R1 = parts.r1, R2 = parts.r2, R3 = parts.r3, R4 = parts.r4;
  const double C1 = parts.c1, C2 = parts.c2, C3 = parts.c3;
  ToneStackDesign out;
  double* nb = out.analogNum;
  double* da = out.analogDen;

  nb[1] = t * C1 * R1 + m * C3 * R3 + l * (C1 * R2 + C2 * R2) + (C1 * R3 + C2 * R3);
  nb[2] = t * (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4) - m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
          m * (C1 * C3 * R1 * R3 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
          l * (C1 * C2 * R1 * R2 + C1 * C2 * R2 * R4 + C1 * C3 * R2 * R4) +
          l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3) +
          (C1 * C2 * R1 * R3 + C1 * C2 * R3 * R4 + C1 * C3 * R3 * R4);
  nb[3] = l * m * (C1 * C2 * C3 * R1 * R2 * R3 + C1 * C2 * C3 * R2 * R3 * R4) -
          m * m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4) +
          m * (C1 * C2 * C3 * R3 * R3 * R4 + C1 * C2 * C3 * R1 * R3 * R3) +
          t * C1 * C2 * C3 * R1 * R3 * R4 - t * m * C1 * C2 * C3 * R1 * R3 * R4 +
          t * l * C1 * C2 * C3 * R1 * R2 * R4;

  da[0] = 1.0;
  da[1] = (C1 * R1 + C1 * R3 + C2 * R3 + C2 * R4 + C3 * R4) + m * C3 * R3 + l * (C1 * R2 + C2 * R2);
  da[2] = m * (C1 * C3 * R1 * R3 - C2 * C3 * R3 * R4 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
          l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3) -
          m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
          l * (C1 * C2 * R2 * R4 + C1 * C2 * R1 * R2 + C1 * C3 * R2 * R4 + C2 * C3 * R2 * R4) +
          (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4 + C1 * C2 * R3 * R4 + C1 * C2 * R1 * R3 +
           C1 * C3 * R3 * R4 + C2 * C3 * R3 * R4);
  da[3] = l * m * (C1 * C2 * C3 * R1 * R2 * R3 + C1 * C2 * C3 * R2 * R3 * R4) -
          m * m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4) +
          m * (C1 * C2 * C3 * R3 * R3 * R4 + C1 * C2 * C3 * R1 * R3 * R3 - C1 * C2 * C3 * R1 * R3 * R4) +
          l * C1 * C2 * C3 * R1 * R2 * R4 + C1 * C2 * C3 * R1 * R3 * R4;

  // Bilinear transform s = K (1 - z^-1) / (1 + z^-1). s^k becomes
  // K^k (1 - z)^k (1 + z)^(3 - k) over (1 + z)^3, with z standing for z^-1.
  static constexpr double kBasis[4][4] = {
      {1.0, 3.0, 3.0, 1.0}, {1.0, 1.0, -1.0, -1.0}, {1.0, -1.0, -1.0, 1.0}, {1.0, -3.0, 3.0, -1.0}};
  const double K = 2.0 * sampleRate;
  double bz[4] = {0.0, 0.0, 0.0, 0.0}, az[4] = {0.0, 0.0, 0.0, 0.0};
  double Kk = 1.0;
  for (int k = 0; k < 4; ++k) {
    for (int i = 0; i < 4; ++i) {
      bz[i] += nb[k] * Kk * kBasis[k][i];
      az[i] += da[k] * Kk * kBasis[k][i];
    }
    Kk *= K;
  }
  for (int i = 0; i < 4; ++i) {
    out.b[i] = bz[i] / az[0];
    out.a[i] = az[i] / az[0];
  }
  return out;
}

/** One channel of the stack: a transposed direct form II section in double
    precision. The coefficients can change at any time. */
class ToneStackFilter {
public:
  void setDesign(const ToneStackDesign& design) noexcept {
    for (int i = 0; i < 4; ++i) {
      b[i] = design.b[i];
      a[i] = design.a[i];
    }
  }
  void reset() noexcept { z1 = z2 = z3 = 0.0; }

  float process(float x) noexcept {
    const double in = x;
    const double y = b[0] * in + z1;
    z1 = b[1] * in - a[1] * y + z2;
    z2 = b[2] * in - a[2] * y + z3;
    z3 = b[3] * in - a[3] * y;
    // Flush the tiny residue of a decayed state so silence stays silent.
    if (std::abs(z1) < 1.0e-30) z1 = 0.0;
    if (std::abs(z2) < 1.0e-30) z2 = 0.0;
    if (std::abs(z3) < 1.0e-30) z3 = 0.0;
    return static_cast<float>(y);
  }

private:
  double b[4]{1.0, 0.0, 0.0, 0.0};
  double a[4]{1.0, 0.0, 0.0, 0.0};
  double z1{0.0}, z2{0.0}, z3{0.0};
};

}  // namespace amp

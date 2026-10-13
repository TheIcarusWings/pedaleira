#pragma once
// amp/WdfToneStack.h: the passive three-knob tone stack (treble, middle, bass)
// as a wave digital filter, built straight from the netlist.
//
// What it is for: an independent reference for ToneStack.h. ToneStack.h
// evaluates a closed-form third-order transfer function; this class solves the
// same network by wave digital filtering, so the two share no algebra. A
// disagreement between them is a bug in one of them. It is also a complete
// alternative implementation: slower (an R-type adaptor with ten ports), but
// with no polynomial to get wrong, so it can model a variant of the circuit by
// changing the netlist.
//
// The network (nodes: 0 ground, 1 input, 2 top of the treble pot, 3 its wiper =
// the output, 4 the bottom of the treble pot, 5 after the slope resistor, 6 and
// 7 the two ends of the middle pot at the bass pot side and the ground side):
//
//     C1   input - 2            treble capacitor
//     R1a  2 - 3                (1 - t) R1, the treble pot above its wiper
//     R1b  3 - 4                t R1, below it
//     R4   input - 5            slope resistor
//     C2   5 - 4                bass capacitor
//     R2'  4 - 6                l R2, the bass pot used as a variable resistor
//     R3a  6 - 7                (1 - m) R3, the middle pot above its wiper
//     R3b  7 - ground           m R3, below it
//     C3   5 - 7                middle capacitor (to the middle pot's wiper)
//
// This is the Fender / Marshall "TMB" stack as analysed in D. T. Yeh and
// J. O. Smith, "Discretization of the '59 Fender Bassman Tone Stack",
// Proc. DAFx-06, 2006. The pot positions t, m, l (0..1) and the parts are the
// same as ToneStackParts / designToneStack use.
//
// Method: ten one-port elements (the capacitors are bilinear, as in
// chowdsp_wdf's CapacitorT) joined by one root R-type adaptor. The adaptor's
// scattering matrix comes from the nodal analysis of the netlist: for a port
// set with port resistances R_j, conductances G = diag(1 / R), and the reduced
// incidence matrix A (nodes by ports), S = 2 A^T (A G A^T)^-1 A G - I
// (A. Fettweis, "Wave digital filters: theory and practice", 1986; R-type
// adaptors for arbitrary topologies: K. J. Werner, J. O. Smith and J. S.
// Abel, "Wave digital filter adaptors for arbitrary topologies and multiport
// linear elements", DAFx-15, 2015). It is
// recomputed whenever a part or a pot changes, so control changes are not
// meant for the audio thread.
//
// Sample rate: prepare(sampleRate) before use. Double precision by default.
// Needs chowdsp_wdf (BSD-3, header only). Not copyable.
#include <chowdsp_wdf/chowdsp_wdf.h>

#include <algorithm>
#include <array>
#include <cmath>

#include "amp/ToneStack.h"

namespace amp {

namespace wdft = chowdsp::wdft;

namespace detail {

/** S = 2 A^T (A G A^T)^-1 A G - I for ports between nodes plus[j] and
    minus[j] (node 0 is ground and left out of the equations). */
template <typename T, int Ports, int Nodes>
void scatteringMatrix(const std::array<T, Ports>& resistances, const int (&plus)[Ports], const int (&minus)[Ports],
                      T (&out)[Ports][Ports]) {
  // M = A G A^T over nodes 1..Nodes.
  T m[Nodes][Nodes] = {};
  for (int j = 0; j < Ports; ++j) {
    const T g = T(1) / resistances[static_cast<size_t>(j)];
    const int p = plus[j] - 1, n = minus[j] - 1;
    if (p >= 0)
      m[p][p] += g;
    if (n >= 0)
      m[n][n] += g;
    if (p >= 0 && n >= 0) {
      m[p][n] -= g;
      m[n][p] -= g;
    }
  }
  // Invert by Gauss-Jordan with partial pivoting.
  T inv[Nodes][Nodes] = {};
  for (int i = 0; i < Nodes; ++i)
    inv[i][i] = T(1);
  for (int c = 0; c < Nodes; ++c) {
    int pivot = c;
    for (int r = c + 1; r < Nodes; ++r)
      if (std::abs(m[r][c]) > std::abs(m[pivot][c]))
        pivot = r;
    for (int k = 0; k < Nodes; ++k) {
      std::swap(m[c][k], m[pivot][k]);
      std::swap(inv[c][k], inv[pivot][k]);
    }
    const T d = T(1) / m[c][c];
    for (int k = 0; k < Nodes; ++k) {
      m[c][k] *= d;
      inv[c][k] *= d;
    }
    for (int r = 0; r < Nodes; ++r) {
      if (r == c)
        continue;
      const T f = m[r][c];
      if (f == T(0))
        continue;
      for (int k = 0; k < Nodes; ++k) {
        m[r][k] -= f * m[c][k];
        inv[r][k] -= f * inv[c][k];
      }
    }
  }
  // S_ij = 2 sum_uv A[u][i] inv[u][v] A[v][j] G_j - delta_ij.
  auto a = [&](int node, int port) -> T {
    return (plus[port] == node + 1 ? T(1) : T(0)) - (minus[port] == node + 1 ? T(1) : T(0));
  };
  for (int i = 0; i < Ports; ++i) {
    for (int j = 0; j < Ports; ++j) {
      T sum = 0;
      for (int u = 0; u < Nodes; ++u) {
        const T au = a(u, i);
        if (au == T(0))
          continue;
        for (int v = 0; v < Nodes; ++v)
          sum += au * inv[u][v] * a(v, j);
      }
      out[i][j] = T(2) * sum / resistances[static_cast<size_t>(j)] - (i == j ? T(1) : T(0));
    }
  }
}

}  // namespace detail

template <typename T = double>
class WdfToneStack {
public:
  WdfToneStack() = default;
  WdfToneStack(const WdfToneStack&) = delete;
  WdfToneStack& operator=(const WdfToneStack&) = delete;

  void prepare(T sampleRate) {
    c1.prepare(sampleRate);
    c2.prepare(sampleRate);
    c3.prepare(sampleRate);
    update();
  }
  void reset() {
    c1.reset();
    c2.reset();
    c3.reset();
  }

  void setParts(const ToneStackParts& newParts) {
    parts = newParts;
    update();
  }
  /** Pot positions, each 0..1: treble, middle, bass (as designToneStack). */
  void setControls(T treble, T middle, T bass) {
    t = std::clamp(treble, T(0), T(1));
    m = std::clamp(middle, T(0), T(1));
    l = std::clamp(bass, T(0), T(1));
    update();
  }

  /** One sample. The output is the wiper node, read as the sum of the voltages
      of the four resistors from the wiper down to ground (R1b, R2', R3a, R3b);
      resistors only, because a capacitor's reflected wave has already moved on
      to the next sample when compute() returns. The R-type adaptor reflects the
      source after it scatters, so the output lags the input by one sample. */
  T process(T x) noexcept {
    source.setVoltage(x);
    root.compute();
    return wdft::voltage<T>(r1b) + wdft::voltage<T>(r2) + wdft::voltage<T>(r3a) + wdft::voltage<T>(r3b);
  }

private:
  static constexpr int kPorts = 10;
  static constexpr int kNodes = 7;
  static constexpr T kMinOhms = T(1.0e-3);

  // Ports, in the order of the node table below.
  wdft::ResistiveVoltageSourceT<T> source{T(0.1)};
  wdft::CapacitorT<T> c1{T(400.0e-12)};
  wdft::ResistorT<T> r1a{T(1.0e3)};
  wdft::ResistorT<T> r1b{T(1.0e3)};
  wdft::ResistorT<T> r4{T(39.0e3)};
  wdft::CapacitorT<T> c2{T(22.0e-9)};
  wdft::ResistorT<T> r2{T(1.0e3)};
  wdft::ResistorT<T> r3a{T(1.0e3)};
  wdft::ResistorT<T> r3b{T(1.0e3)};
  wdft::CapacitorT<T> c3{T(22.0e-9)};

  static constexpr int kPlus[kPorts] = {1, 1, 2, 3, 1, 5, 4, 6, 7, 5};
  static constexpr int kMinus[kPorts] = {0, 2, 3, 4, 5, 4, 6, 7, 0, 7};

  struct Calc {
    template <typename RType>
    static void calcImpedance(RType& r) {
      const auto z = r.getPortImpedances();
      T s[kPorts][kPorts];
      detail::scatteringMatrix<T, kPorts, kNodes>(z, kPlus, kMinus, s);
      r.setSMatrixData(s);
    }
  };

  using Root = wdft::RootRtypeAdaptor<T, Calc, decltype(source), decltype(c1), decltype(r1a), decltype(r1b),
                                      decltype(r4), decltype(c2), decltype(r2), decltype(r3a), decltype(r3b),
                                      decltype(c3)>;
  Root root{source, c1, r1a, r1b, r4, c2, r2, r3a, r3b, c3};

  void update() {
    c1.setCapacitanceValue(static_cast<T>(parts.c1));
    c2.setCapacitanceValue(static_cast<T>(parts.c2));
    c3.setCapacitanceValue(static_cast<T>(parts.c3));
    r1a.setResistanceValue(std::max(static_cast<T>((1.0 - t) * parts.r1), kMinOhms));
    r1b.setResistanceValue(std::max(static_cast<T>(t * parts.r1), kMinOhms));
    r4.setResistanceValue(static_cast<T>(parts.r4));
    r2.setResistanceValue(std::max(static_cast<T>(l * parts.r2), kMinOhms));
    r3a.setResistanceValue(std::max(static_cast<T>((1.0 - m) * parts.r3), kMinOhms));
    r3b.setResistanceValue(std::max(static_cast<T>(m * parts.r3), kMinOhms));
    root.calcImpedance();
  }

  ToneStackParts parts;
  T t{T(0.5)}, m{T(0.5)}, l{T(0.5)};
};

}  // namespace amp

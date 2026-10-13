#pragma once
// amp/WdfTriodeStage.h: a triode gain stage built from wave digital filters.
//
// What it models: the same stage as ClassicGainStage (GainStages.h), but with
// the linear networks around the tube solved as wave digital filter trees
// built straight from the component values, and the tube as the nonlinear
// root they meet in.
//
//   plate network   Ra (to the supply, an AC ground)  ||  Cp (plate wiring and
//                   the next Miller capacitance)  ||  [Cc in series with Rs and
//                   the next grid leak Rg]. The voltage across Rg is the next
//                   grid. The plate resistance rp of the tube is NOT in this
//                   tree: it is inside the tube equation below.
//   cathode network Rk || Ck (or a short for an ideally bypassed cathode, or Rk
//                   alone without a capacitor).
//   pot network     the gain pot between two stages: the source resistance, the
//                   top section of the pot with the bright capacitor across it,
//                   and at the wiper the lower section in parallel with the
//                   coupling capacitor, series resistor and next grid leak.
//
// The tube is a two-port root joining the plate tree and the cathode tree. With
// i the plate current (out of the plate network, through the tube, into the
// cathode network) and vgk, vpk the grid-cathode and plate-cathode voltages,
// the model is
//
//     rp i = mu c(vgk) + vpk                (1)
//
// where c(x) is the SAME asymmetric soft curve as the classic stage
// (triodeCurve: unity slope at 0, grid conduction limit on the positive side,
// cutoff on the negative side). For a linear c this is the textbook small
// signal triode: plate current (mu vgk + vpk) / rp, which gives the stage gain
// mu Ra / (Ra + rp + (mu + 1) Zk) with Zk the cathode impedance. Writing the
// two port voltages with the waves a (from the tree), b (back to it) and the
// port resistances R:
//
//     plate:    vp = ap - Rp i,    bp = ap - 2 Rp i
//     cathode:  vk = ak + Rk i,    bk = ak + 2 Rk i
//     vgk = g - vk,   vpk = vp - vk
//
// turns (1) into one scalar equation in i,
//
//     F(i) = (rp + Rp + Rk) i - mu c(g - ak - Rk i) - ap + ak = 0,
//     F'(i) = (rp + Rp + Rk) + mu Rk c'(vgk)  > 0,
//
// which is monotone, so Newton's method from the previous sample's current
// converges in two or three steps (the sample rate is high against the signal).
// This is "a root nonlinearity solved per sample"; the alternative of driving
// the existing curve from the port voltage would leave the cathode feedback
// out, because the curve is only a function of the grid voltage, while here
// the cathode resistor degenerates the gain through the same equation that
// clips it, and the plate load loads the tube through the tree.
//
// What stays our own, not wave digital: the grid-conduction bias shift (the
// coupling capacitor charging through the conducting grid). A grid diode at
// the next grid would put a second nonlinearity inside the plate tree, which a
// one-root tree cannot hold, so the existing behavioural element is kept (the
// same attack / release state as TriodeStage, driven by the grid voltage the
// tree delivers).
//
// Method: wave digital filters, A. Fettweis, "Wave digital filters: theory
// and practice", Proc. IEEE 74(2), 1986, with the series and parallel
// adaptors and the bilinear (trapezoidal) capacitor of the library
// chowdsp_wdf (J. Chowdhury, arXiv:2210.12554). Wave digital models of triode
// stages go back to M. Karjalainen and J. Pakarinen, "Wave digital simulation
// of a vacuum-tube amplifier", ICASSP 2006. The tube equation here is our own
// algebraic stand-in (the curve of TriodeStage.h in place of the Koren plate
// current function, N. Koren 2003), solved at the root by Newton's method.
//
// Differences from the classic stage, all on purpose:
//   - the next grid network LOADS the plate (the classic stage drives it from
//     an ideal voltage), so mid-band gain is a fraction of a dB lower
//     (Ra || (Rs + Rg) in place of Ra);
//   - the cathode resistor degenerates the gain through the tube equation
//     (the classic stage applies a gain shelf before the curve);
//   - there is no antiderivative anti-aliasing (it does not carry over to an
//     implicit solve); run it oversampled, as the amp does.
//
// Sample rate: prepare(sampleRate) with the rate the stage runs at (the
// oversampled rate in the amp). Capacitors are bilinear, so the response is
// warped near Nyquist; at four times 48 kHz the error below 10 kHz is under
// 0.02 dB.
//
// Needs chowdsp_wdf (BSD-3, header only). One instance is ONE channel and is
// not copyable (the trees hold references to their own parts).
#include <chowdsp_wdf/chowdsp_wdf.h>

#include <algorithm>
#include <cmath>

#include "amp/FastMath.h"
#include "amp/GainStages.h"

namespace amp {

namespace wdft = chowdsp::wdft;

/** Ra || Cp || (Cc - Rs - Rg): the plate network of a stage. */
template <typename T>
class WdfPlateTree {
public:
  /** The elements are public so a test can probe voltages. */
  wdft::ResistorT<T> ra{(T)100.0e3};
  wdft::CapacitorT<T> cp{(T)1.0e-12};
  wdft::CapacitorT<T> cc{(T)1.0e-9};
  wdft::ResistorT<T> rs{(T)1.0e3};
  wdft::ResistorT<T> rg{(T)1.0e6};
  wdft::WDFSeriesT<T, decltype(rs), decltype(rg)> seriesLeak{rs, rg};
  wdft::WDFSeriesT<T, decltype(cc), decltype(seriesLeak)> branch{cc, seriesLeak};
  wdft::WDFParallelT<T, decltype(ra), decltype(cp)> load{ra, cp};
  wdft::WDFParallelT<T, decltype(load), decltype(branch)> top{load, branch};

  WdfPlateTree() = default;
  WdfPlateTree(const WdfPlateTree&) = delete;
  WdfPlateTree& operator=(const WdfPlateTree&) = delete;

  void prepare(T sampleRate) noexcept {
    cp.prepare(sampleRate);
    cc.prepare(sampleRate);
  }
  void reset() noexcept {
    cp.reset();
    cc.reset();
  }
  /** Voltage across the next grid leak (the next grid). Valid after the tree
      has been given its incident wave. */
  T gridVoltage() const noexcept { return wdft::voltage<T>(rg); }
};

/** Rk || Ck: the cathode network of a stage. */
template <typename T>
class WdfCathodeTree {
public:
  wdft::ResistorT<T> rk{(T)1.5e3};
  wdft::CapacitorT<T> ck{(T)1.0e-6};
  wdft::WDFParallelT<T, decltype(rk), decltype(ck)> top{rk, ck};

  WdfCathodeTree() = default;
  WdfCathodeTree(const WdfCathodeTree&) = delete;
  WdfCathodeTree& operator=(const WdfCathodeTree&) = delete;

  void prepare(T sampleRate) noexcept { ck.prepare(sampleRate); }
  void reset() noexcept { ck.reset(); }
};

/** The gain pot with its bright capacitor, loaded by the coupling network to
    the next grid. Driven by an ideal voltage (the plate of the stage before). */
template <typename T>
class WdfPotTree {
public:
  wdft::ResistorT<T> source{(T)100.0e3};
  wdft::ResistorT<T> upper{(T)1.0e3};
  wdft::CapacitorT<T> bright{(T)1.0e-15};
  wdft::ResistorT<T> lower{(T)22.0e3};
  wdft::CapacitorT<T> cc{(T)1.0e-9};
  wdft::ResistorT<T> rs{(T)1.0};
  wdft::ResistorT<T> rg{(T)470.0e3};
  wdft::WDFParallelT<T, decltype(upper), decltype(bright)> upperSection{upper, bright};
  wdft::WDFSeriesT<T, decltype(source), decltype(upperSection)> top{source, upperSection};
  wdft::WDFSeriesT<T, decltype(rs), decltype(rg)> seriesLeak{rs, rg};
  wdft::WDFSeriesT<T, decltype(cc), decltype(seriesLeak)> branch{cc, seriesLeak};
  wdft::WDFParallelT<T, decltype(lower), decltype(branch)> bottom{lower, branch};
  wdft::WDFSeriesT<T, decltype(top), decltype(bottom)> loop{top, bottom};
  wdft::IdealVoltageSourceT<T, decltype(loop)> vs{loop};

private:
  GainPotParams params;

public:
  static constexpr float kOpenFarads = 1.0e-15f;
  static constexpr float kMinOhms = 1.0e-3f;

  WdfPotTree() = default;
  WdfPotTree(const WdfPotTree&) = delete;
  WdfPotTree& operator=(const WdfPotTree&) = delete;

  /** Sets the fixed parts: the pot, the coupling to the next grid and the
      source it is driven from. The coupling capacitor is `coupling.capacitance`
      (change it later with setCouplingFarads). */
  void setParts(const GainPotParams& pot, const CouplingNetwork& coupling) noexcept {
    params = pot;
    source.setResistanceValue((T)pot.sourceResistance);
    cc.setCapacitanceValue((T)std::max(coupling.capacitance, kOpenFarads));
    rs.setResistanceValue((T)std::max(coupling.seriesResistance, kMinOhms));
    rg.setResistanceValue((T)coupling.gridLeak);
  }
  void setCouplingFarads(T farads) noexcept { cc.setCapacitanceValue(std::max(farads, (T)kOpenFarads)); }

  /** The wiper position 0..1 and the bright switch, with the same law as
      GainPot (audio taper, a floor resistor below the pot, the bright
      capacitor across the upper section). */
  void setPot(T position, bool brightOn) noexcept {
    const T p = std::clamp(position, (T)0, (T)1);
    const T below = (T)params.floorResistance + (T)params.potResistance * std::pow(p, (T)params.taperExponent);
    const T top = std::max((T)0, (T)params.potResistance + (T)params.floorResistance - below);
    lower.setResistanceValue(std::max(below, (T)kMinOhms));
    upper.setResistanceValue(std::max(top, (T)kMinOhms));
    bright.setCapacitanceValue(brightOn && params.brightCapacitance > 0.0f ? (T)params.brightCapacitance : (T)kOpenFarads);
  }

  void prepare(T sampleRate) noexcept {
    bright.prepare(sampleRate);
    cc.prepare(sampleRate);
  }
  void reset() noexcept {
    bright.reset();
    cc.reset();
  }
  /** One sample: `x` volts in (the plate), the next grid voltage out. */
  T process(T x) noexcept {
    vs.setVoltage(x);
    vs.incident(loop.reflected());
    loop.incident(vs.reflected());
    return wdft::voltage<T>(rg);
  }
};

class WdfGainStage {
public:
  WdfGainStage() = default;
  WdfGainStage(const WdfGainStage&) = delete;
  WdfGainStage& operator=(const WdfGainStage&) = delete;

  void prepare(double sampleRate, const GainStageSpec& newSpec) noexcept {
    spec = newSpec;
    rate = sampleRate;
    const float fs = static_cast<float>(sampleRate);

    idealCathode = spec.bypass == CathodeBypass::Ideal;
    mu = spec.triode.tube.mu;
    rp = spec.triode.tube.rp;
    posLimit = spec.triode.posLimit();
    negLimit = spec.triode.negLimit();
    gridOnset = spec.triode.gridOnset();

    // Plate network. A stage without a next grid (or whose coupling hangs
    // after a pot) gets an open branch: a femtofarad in series with a megohm
    // loads nothing.
    plate.ra.setResistanceValue(spec.triode.plateLoad);
    plate.cp.setCapacitanceValue(std::max(spec.plateFarads, kOpenFarads));
    const bool coupledHere = spec.hasNext && !spec.hasPot;
    plate.cc.setCapacitanceValue(coupledHere ? std::max(spec.coupling.capacitance, kOpenFarads) : kOpenFarads);
    plate.rs.setResistanceValue(coupledHere ? std::max(spec.coupling.seriesResistance, kMinOhms) : 1.0e6f);
    plate.rg.setResistanceValue(coupledHere ? spec.coupling.gridLeak : 1.0e6f);
    plate.prepare(fs);

    // Cathode network.
    switch (spec.bypass) {
      case CathodeBypass::Ideal:
        cathode.rk.setResistanceValue(kMinOhms);
        cathode.ck.setCapacitanceValue(1.0f);
        break;
      case CathodeBypass::None:
        cathode.rk.setResistanceValue(spec.triode.cathodeResistor);
        cathode.ck.setCapacitanceValue(kOpenFarads);
        break;
      case CathodeBypass::Capacitor:
        cathode.rk.setResistanceValue(spec.triode.cathodeResistor);
        cathode.ck.setCapacitanceValue(std::max(spec.bypassFarads, kOpenFarads));
        break;
    }
    cathode.prepare(fs);

    // Pot network, when this stage has one.
    if (spec.hasPot) {
      potTree.setParts(spec.pot, spec.coupling);
      potTree.prepare(fs);
      potTree.setPot(1.0f, false);
    }
    setGridTimes(0.01, 0.001);
    reset();
  }

  void reset() noexcept {
    plate.reset();
    cathode.reset();
    potTree.reset();
    shift = 0.0f;
    current = 0.0f;
  }

  /** Grid-conduction time constants of this stage's own grid network (see
      TriodeStage::setCoupling). */
  void setGridTimes(double leakSeconds, double chargeSeconds) noexcept {
    release = static_cast<float>(1.0 - std::exp(-1.0 / (std::max(1.0e-5, leakSeconds) * rate)));
    attack = std::min(static_cast<float>(1.0 / (std::max(1.0e-6, chargeSeconds) * rate)), 0.5f);
  }

  /** Corner of the coupling to the next grid, in Hz: sets the coupling
      capacitor so that 1 / (2 pi C (Rs + Rg)) is `hz`. */
  void setCouplingCorner(double hz) noexcept {
    if (!spec.hasNext)
      return;
    const double total = static_cast<double>(spec.coupling.totalResistance());
    const float farads = static_cast<float>(1.0 / (kTwoPi * std::max(hz, 0.1) * total));
    const float value = std::max(farads, kOpenFarads);
    if (spec.hasPot)
      potTree.setCouplingFarads(value);
    else
      plate.cc.setCapacitanceValue(value);
  }

  /** The wiper position 0..1 and the bright switch (see WdfPotTree::setPot). */
  void setPot(float position, bool bright) noexcept {
    if (spec.hasPot)
      potTree.setPot(position, bright);
  }

  float process(float gridVolts) noexcept {
    const float grid = gridVolts - shift;
    const float ap = plate.top.reflected();
    const float rpl = plate.top.wdf.R;
    float i;
    if (idealCathode) {
      // With the cathode shorted for audio the grid-cathode voltage is the
      // grid voltage whatever the current is, so F(i) is linear in i and the
      // tube equation has a closed form: no iteration.
      i = (mu * triodeCurve(grid, posLimit, negLimit) + ap) * recip(rp + rpl);
    } else {
      const float ak = cathode.top.reflected();
      const float rca = cathode.top.wdf.R;
      const float total = rp + rpl + rca;
      // Newton's method on F(i), from the last sample's current.
      i = current;
      for (int n = 0; n < newtonSteps; ++n) {
        const float x = grid - ak - rca * i;
        float slope;
        const float c = curveAndSlope(x, slope);
        const float f = total * i - mu * c - ap + ak;
        const float df = total + mu * rca * slope;
        i -= f * recip(df);
      }
      cathode.top.incident(ak + 2.0f * rca * i);
    }
    current = i;
    plate.top.incident(ap - 2.0f * rpl * i);

    // Grid conduction: the same attack / release element as TriodeStage.
    const float over = grid > gridOnset ? grid - gridOnset : 0.0f;
    shift += attack * over - release * shift;
    shift = std::min(shift, spec.triode.maxBiasShiftVolts);
    if (shift < 1.0e-20f)
      shift = 0.0f;

    // The stage inverts; the amp's polarity convention does not (see the top
    // of GainStages.h), so the sign is taken back here.
    float out;
    if (spec.hasPot) {
      out = potTree.process(-(ap - rpl * i));
    } else if (spec.hasNext) {
      out = -plate.gridVoltage();
    } else {
      out = -(ap - rpl * i);
    }
    return out;
  }

  float biasShift() const noexcept { return shift; }
  /** Newton steps per sample for a stage with a cathode network (2 is enough;
      more is for tests). */
  void setNewtonSteps(int steps) noexcept { newtonSteps = std::clamp(steps, 1, 64); }

  WdfPlateTree<float> plate;
  WdfCathodeTree<float> cathode;
  WdfPotTree<float> potTree;

private:
  static constexpr float kOpenFarads = 1.0e-15f;
  static constexpr float kMinOhms = 1.0e-3f;

  // c(x) and c'(x) for the asymmetric soft curve of triodeCurve.
  float curveAndSlope(float x, float& slope) const noexcept {
    const bool positive = x >= 0.0f;
    const float limit = positive ? posLimit : negLimit;
    const float t = std::clamp(x * recip(limit), -1.0e3f, 1.0e3f);
    const float r = invSqrt(1.0f + t * t);
    slope = r * r * r;
    return limit * t * r;
  }

  GainStageSpec spec;
  double rate{48000.0};
  float mu{100.0f}, rp{62.5e3f};
  float posLimit{1.0f}, negLimit{1.0f}, gridOnset{0.5f};
  float attack{0.0f}, release{0.0f}, shift{0.0f};
  float current{0.0f};
  int newtonSteps{2};
  bool idealCathode{true};
};

}  // namespace amp

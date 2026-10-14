#pragma once
// amp/CouplingFilters.h: the passive pieces between the gain stages of a
// preamp: the coupling network, the cathode bypass shelf and the gain pot with
// its bright capacitor. All are described by component values (ohms, farads)
// and their corners are derived here, so a schematic can be typed in.
//
// CouplingNetwork + CouplingHighpass
//   What it models: a coupling capacitor C into a series resistor Rs and a
//   grid leak resistor Rg to ground. It is a first-order high-pass at
//   1 / (2 pi C (Rs + Rg)) with a voltage divider Rg / (Rs + Rg) in the pass
//   band (the source impedance of the previous plate is not included; add it
//   to Rs if it matters). The same capacitor and resistance set how fast a
//   grid-conduction bias shift leaks away (leakSeconds, used by TriodeStage).
//   A smaller capacitor "tightens" the low end before the next clipping
//   stage, which is how a high-gain preamp keeps low chords from turning to
//   mud.
//
// CathodeShelf
//   What it models: the cathode resistor Rk and its bypass capacitor Ck. Below
//   the corner 1 / (2 pi Rk Ck) the cathode is unbypassed and the stage gain
//   is lower by negative feedback (a ratio derived from the tube in
//   TriodeCircuit::unbypassedRatio); above it the capacitor shorts Rk and the
//   stage has its full gain. A first-order low shelf, 1 at high frequencies.
//   Reference: the textbook cathode-bypass analysis (any valve amplifier
//   text, "common-cathode stage with cathode bypass").
//
// GainPot
//   What it models: the gain control between two stages, a potentiometer
//   Rpot used as a voltage divider from the source resistance Rsrc (the
//   previous plate load), with a floor resistor Rfloor in series at the
//   bottom (so the control does not reach silence), loaded by the next stage's
//   grid leak Rload, and a bright capacitor Cb across the upper part of the
//   pot. The wiper position follows an audio taper: the resistance below the
//   wiper is Rpot * position^taperExponent (3.3 gives about 10 percent at
//   half rotation). The bright cap passes highs around the upper section, so
//   with the pot turned down the highs are lifted; the lift is exactly the
//   network's transfer function
//     H(s) = H0 (1 + s tz) / (1 + s tp)
//   with tz = Rtop Cb and tp = tz (Rsrc + Rb) / (Rsrc + Rtop + Rb), and it
//   fades to nothing as the pot goes up (Rtop goes to zero), as in the real
//   circuit. Rb is the lower section in parallel with Rload. Setting Cb to 0
//   removes the bright cap.
//
// ToneCut
//   What it models: the tone cut control of a power amp without global
//   feedback: a pot Rpot (plus a fixed Rmin) in series with a capacitor C that
//   shunts the signal driving the output tubes, which comes from a source
//   resistance Rsrc (the phase inverter's plate). Turning the cut up takes
//   resistance out of the branch, so more of the highs go to ground:
//     H(s) = (1 + s Rc C) / (1 + s (Rsrc + Rc) C),  Rc = Rmin + Rpot (1 - position)
//   unity at low frequencies, a shelf down to Rc / (Rsrc + Rc) above the
//   corner 1 / (2 pi (Rsrc + Rc) C); with the cut fully up and Rmin = 0 it is
//   a plain first-order low-pass.
//
// Sample rate: every setter takes the rate the filter runs at. Plain C++ and
// std only (amp/Filters.h). One instance is ONE channel.
#include <algorithm>
#include <cmath>

#include "amp/Filters.h"

namespace amp {

struct CouplingNetwork {
  float capacitance{22.0e-9f};
  float seriesResistance{0.0f};
  float gridLeak{1.0e6f};

  float totalResistance() const noexcept { return seriesResistance + gridLeak; }
  /** Time constant C (Rs + Rg), which is also the bias-shift leak time. */
  float leakSeconds() const noexcept { return capacitance * totalResistance(); }
  /** High-pass corner in Hz. */
  float cornerHz() const noexcept { return 1.0f / (static_cast<float>(kTwoPi) * leakSeconds()); }
  /** Pass-band voltage division Rg / (Rs + Rg). */
  float divider() const noexcept { return gridLeak / totalResistance(); }
};

class CouplingHighpass {
public:
  void setCorner(double hz, double sampleRate) noexcept { pole.setCutoff(hz, sampleRate); }
  void reset() noexcept { pole.reset(); }
  float process(float x) noexcept { return pole.highpass(x); }

private:
  OnePole pole;
};

class CathodeShelf {
public:
  /** `cornerHz` = 1 / (2 pi Rk Ck); `lowRatio` from TriodeCircuit. */
  void setup(double cornerHz, float newLowRatio, double sampleRate) noexcept {
    pole.setCutoff(cornerHz, sampleRate);
    lowRatio = newLowRatio;
  }
  static double cornerFor(double cathodeResistor, double bypassCapacitor) noexcept {
    return 1.0 / (kTwoPi * cathodeResistor * bypassCapacitor);
  }
  void reset() noexcept { pole.reset(); }
  float process(float x) noexcept { return x + (lowRatio - 1.0f) * pole.lowpass(x); }

private:
  OnePole pole;
  float lowRatio{1.0f};
};

struct GainPotParams {
  float sourceResistance{100.0e3f};  // plate load of the stage before
  float potResistance{1.0e6f};
  float floorResistance{22.0e3f};  // fixed resistor below the pot
  float loadResistance{470.0e3f};  // grid leak of the stage after
  float taperExponent{3.3f};
  float brightCapacitance{100.0e-12f};  // 0 for none
};

class GainPot {
public:
  void prepare(double newSampleRate, const GainPotParams& newParams) noexcept {
    params = newParams;
    sampleRate = newSampleRate;
    setPosition(1.0f, false);
    reset();
  }
  void reset() noexcept { lift.reset(); }

  /** `position` 0..1 (1 = pot fully up). Call at control rate. */
  void setPosition(float position, bool brightOn) noexcept {
    const float p = std::clamp(position, 0.0f, 1.0f);
    const float below = params.floorResistance + params.potResistance * std::pow(p, params.taperExponent);
    const float top = std::max(0.0f, params.potResistance + params.floorResistance - below);
    const float bottom = below * params.loadResistance / (below + params.loadResistance);
    const float total = params.sourceResistance + top + bottom;
    midGain = bottom / total;
    if (brightOn && params.brightCapacitance > 0.0f && top > 1.0f) {
      const float zero = top * params.brightCapacitance;
      const float pole = zero * (params.sourceResistance + bottom) / total;
      highRatio = zero / pole;  // = total / (Rsrc + Rb)
      lift.setCutoff(1.0 / (kTwoPi * pole), sampleRate);
    } else {
      highRatio = 1.0f;
    }
  }

  /** The linear mid-band gain of the pot at the current position. */
  float gain() const noexcept { return midGain; }
  /** Gain at high frequencies relative to the mid band (1 without the cap). */
  float brightRatio() const noexcept { return highRatio; }

  float process(float x) noexcept {
    if (highRatio == 1.0f)
      return midGain * x;
    return midGain * (highRatio * x + (1.0f - highRatio) * lift.lowpass(x));
  }

private:
  GainPotParams params;
  double sampleRate{48000.0};
  OnePole lift;
  float midGain{1.0f};
  float highRatio{1.0f};
};

struct ToneCutParams {
  float sourceResistance{100.0e3f};  // what drives the network: the phase inverter's plate
  float potResistance{100.0e3f};
  float minResistance{0.0f};  // resistance left in the branch with the cut fully up
  float capacitance{4.7e-9f};
};

class ToneCut {
public:
  void prepare(double newSampleRate, const ToneCutParams& newParams) noexcept {
    params = newParams;
    sampleRate = newSampleRate;
    setPosition(0.0f);
    reset();
  }
  void reset() noexcept { pole.reset(); }

  /** `position` 0..1 (1 = the most cut). Call at control rate. */
  void setPosition(float position) noexcept {
    const float p = std::clamp(position, 0.0f, 1.0f);
    const float branch = params.minResistance + params.potResistance * (1.0f - p);
    const float total = params.sourceResistance + branch;
    shelf = branch / total;
    pole.setCutoff(1.0 / (kTwoPi * static_cast<double>(total) * params.capacitance), sampleRate);
  }

  /** Gain at high frequencies relative to the low ones. */
  float highRatio() const noexcept { return shelf; }

  float process(float x) noexcept {
    const float low = pole.lowpass(x);
    return low + shelf * (x - low);
  }

private:
  ToneCutParams params;
  double sampleRate{48000.0};
  OnePole pole;
  float shelf{1.0f};
};

}  // namespace amp

#pragma once
// amp/GainStages.h: the description of one gain stage (a triode with
// everything that hangs on it) and the classic, non-wave-digital realisation
// of it.
//
// A gain stage, in the sense of the amp framework, is
//
//   grid -> [cathode network] -> triode -> plate load -> [gain pot] ->
//   coupling network -> the NEXT stage's grid
//
// GainStageSpec lists the component values of all of that. Two classes
// realise it with the same interface, so an amp can run either and a test can
// compare them:
//
//   ClassicGainStage   (this file)         the triode as a static waveshaper
//                                          with one-pole filters for the
//                                          networks around it (TriodeStage.h,
//                                          CouplingFilters.h)
//   WdfGainStage       (WdfTriodeStage.h)  the networks as wave digital
//                                          filter trees, the triode as a
//                                          nonlinear root solved per sample
//
// Interface of both (one instance is ONE channel):
//   prepare(sampleRate, spec)        build from the component values
//   reset()                          clear all audio state
//   setGridTimes(leak, charge)       grid-conduction time constants of THIS
//                                    stage's grid network (as TriodeStage)
//   setCouplingCorner(hz)            corner of the coupling to the next grid
//   setPot(position, bright)         gain pot position 0..1 and bright switch
//                                    (only for a stage with a pot)
//   float process(float gridVolts)   the voltage at the next grid (after the
//                                    last stage: the plate voltage)
//
// Polarity: neither class inverts. A real common-cathode stage does; the amp
// as a whole does not care, and both realisations agree on this.
//
// Plain C++ and std only.
#include "amp/CouplingFilters.h"
#include "amp/Filters.h"
#include "amp/TriodeStage.h"

namespace amp {

/** What the cathode resistor is bypassed with, for the signal. */
enum class CathodeBypass {
  Ideal,      // an ideal short for audio: the full stage gain at every frequency
  None,       // no capacitor: the cathode resistor degenerates the stage everywhere
  Capacitor,  // a capacitor of bypassFarads across the cathode resistor
};

struct GainStageSpec {
  TriodeCircuit triode;
  CathodeBypass bypass{CathodeBypass::Ideal};
  float bypassFarads{0.0f};
  /** Capacitance on the plate: wiring plus the next grid's Miller capacitance. */
  float plateFarads{0.0f};
  /** False for the last stage, whose plate goes to the tone stack. */
  bool hasNext{true};
  /** Coupling capacitor, series resistor and grid leak of the next grid. */
  CouplingNetwork coupling;
  /** The gain pot sits between this plate and the coupling capacitor. */
  bool hasPot{false};
  GainPotParams pot;
};

class ClassicGainStage {
public:
  void prepare(double sampleRate, const GainStageSpec& newSpec) noexcept {
    spec = newSpec;
    rate = sampleRate;
    triode.prepare(rate, spec.triode);
    if (spec.plateFarads > 0.0f)
      plate.setCutoff(1.0 / (kTwoPi * spec.triode.sourceImpedance() * spec.plateFarads), rate);
    else
      plate.setCutoff(0.49 * rate, rate);
    shelfMode = spec.bypass;
    if (shelfMode == CathodeBypass::Capacitor)
      cathode.setup(CathodeShelf::cornerFor(spec.triode.cathodeResistor, spec.bypassFarads),
                    spec.triode.unbypassedRatio(), rate);
    fixedRatio = spec.triode.unbypassedRatio();
    if (spec.hasPot)
      pot.prepare(rate, spec.pot);
    divider = spec.coupling.divider();
    if (spec.coupling.capacitance > 0.0f)
      coupling.setCorner(spec.coupling.cornerHz(), rate);
    reset();
  }

  void reset() noexcept {
    triode.reset();
    plate.reset();
    cathode.reset();
    pot.reset();
    coupling.reset();
  }

  void setGridTimes(double leakSeconds, double chargeSeconds) noexcept {
    triode.setCoupling(leakSeconds, chargeSeconds);
  }
  void setCouplingCorner(double hz) noexcept { coupling.setCorner(hz, rate); }
  void setPot(float position, bool bright) noexcept { pot.setPosition(position, bright); }

  float process(float grid) noexcept {
    float s = grid;
    if (shelfMode == CathodeBypass::Capacitor)
      s = cathode.process(s);
    else if (shelfMode == CathodeBypass::None)
      s *= fixedRatio;
    s = plate.lowpass(triode.process(s));
    if (spec.hasPot)
      s = pot.process(s);
    if (spec.hasNext)
      s = coupling.process(s) * divider;
    return s;
  }

  float biasShift() const noexcept { return triode.biasShift(); }

private:
  GainStageSpec spec;
  double rate{48000.0};
  TriodeStage triode;
  OnePole plate;
  CathodeShelf cathode;
  CathodeBypass shelfMode{CathodeBypass::Ideal};
  float fixedRatio{1.0f};
  GainPot pot;
  CouplingHighpass coupling;
  float divider{1.0f};
};

}  // namespace amp

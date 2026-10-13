#pragma once
// amp/AmpCircuit.h: one channel of an amp, built from an AmpDescription.
//
//   in -> [input coupling high-pass -> grid stopper low-pass -> gain stages
//         (each: cathode, triode, plate load, [gain pot], coupling) -> passive
//         tone stack -> master -> power amp with presence / depth feedback and
//         supply sag]
//
// The audio engine in plugin/src/effects/amp/dsp/AmpProcessor.h runs this
// four times oversampled and owns the knobs; this class owns the circuit.
//
// Two phases:
//   prepare(rate, description)   builds every component from the description's
//                                numbers; call off the audio thread (the wave
//                                digital filter model sets up its trees here)
//   apply(frame)                 pushes one set of control values into the
//                                components; no allocation, safe at control rate
//   preamp / powerSample         the audio
//
// A ControlFrame is the set of control values for one control tick, computed by
// makeControlFrame from the smoothed panel positions: it holds the derived
// values (tone stack polynomials, interstage corners, grid time constants) so
// the two channels share one computation.
//
// Plain C++ and std only (plus chowdsp_wdf through WdfTriodeStage.h). One
// instance is ONE channel.
#include <algorithm>
#include <cmath>

#include "amp/AmpDescription.h"
#include "amp/CouplingFilters.h"
#include "amp/Filters.h"
#include "amp/GainStages.h"
#include "amp/PowerAmp.h"
#include "amp/SagModel.h"
#include "amp/ToneStack.h"
#include "amp/WdfTriodeStage.h"

namespace amp {

/** The smoothed knob positions (0..1) and the voicing, in the units of the
    panel: what the audio engine hands to makeControlFrame. */
struct PanelValues {
  float gain{0.5f}, bass{0.5f}, middle{0.5f}, treble{0.5f};
  float presence{0.5f}, depth{0.5f}, master{0.5f};
  bool bright{false};
  /** Interstage corner of each link for the chosen voicing, Hz. */
  float couplingCornerHz[kMaxLinks]{};
  float sagDepth{0.2f}, sagReleaseSeconds{0.07f};
};

/** position = low + (high - low) * knob ^ taper (the identity, exactly, for a
    plain full-travel linear control). */
inline float panelPosition(const PanelControl& control, float knob) noexcept {
  const float k = std::clamp(knob, 0.0f, 1.0f);
  if (control.taper == 1.0f && control.low == 0.0f && control.high == 1.0f)
    return k;
  return control.low + (control.high - control.low) * std::pow(k, control.taper);
}

struct ControlFrame {
  float potPosition{0.5f};
  bool bright{false};
  float cornerHz[kMaxLinks]{};
  float leakSeconds[kMaxLinks]{};
  float chargeSeconds[kMaxLinks]{};
  ToneStackDesign tone;
  float presence{0.0f}, depth{0.0f};
  float sagDepth{0.2f}, sagReleaseSeconds{0.07f};
  float masterGain{0.0f};
};

/** The ratio between the charge path (source + series + conducting grid) and
    the discharge path (series + grid leak) of a coupling network. */
inline float chargeRatio(float sourceOhms, const CouplingNetwork& net, const TriodeCircuit& circuit) noexcept {
  return (sourceOhms + net.seriesResistance + circuit.gridConductionResistance) / net.totalResistance();
}

inline ControlFrame makeControlFrame(const AmpDescription& d, const PanelValues& v, double rate) noexcept {
  ControlFrame f;
  const CalibrationDescription& c = d.calibration;
  const float gainPosition = panelPosition(d.control(PanelId::Gain), v.gain);
  const float rise = 1.0f + c.couplingGainRise * gainPosition;
  f.potPosition = gainPosition;
  f.bright = v.bright;
  f.tone = designToneStack(d.tone.parts, panelPosition(d.control(PanelId::Treble), v.treble),
                           panelPosition(d.control(PanelId::Middle), v.middle),
                           panelPosition(d.control(PanelId::Bass), v.bass), rate);
  f.masterGain = panelPosition(d.control(PanelId::Master), v.master) / c.powerDriveVolts;
  // Per link: the corner after the gain rise, the time the grid charge leaks
  // away in, and the time it takes to build.
  for (int k = 0; k + 1 < d.numStages; ++k) {
    const GainStageSpec& sender = d.stages[k].spec;
    const GainStageSpec& receiver = d.stages[k + 1].spec;
    f.cornerHz[k] = v.couplingCornerHz[k] * rise;
    f.leakSeconds[k] = 1.0f / (static_cast<float>(kTwoPi) * f.cornerHz[k]);
    const float sourceOhms = sender.hasPot ? sender.pot.sourceResistance : sender.triode.sourceImpedance();
    f.chargeSeconds[k] = f.leakSeconds[k] * chargeRatio(sourceOhms, sender.coupling, receiver.triode);
  }
  f.presence = panelPosition(d.control(PanelId::Presence), v.presence);
  f.depth = panelPosition(d.control(PanelId::Depth), v.depth);
  f.sagDepth = v.sagDepth;
  f.sagReleaseSeconds = v.sagReleaseSeconds;
  return f;
}

/** The copyable part of a channel: everything but the wave digital trees. */
struct AmpCircuitCore {
  CouplingHighpass inputHighpass;
  OnePole gridStopper;
  ClassicGainStage classic[kMaxStages];
  ToneStackFilter tone;
  PowerAmp power;
  SagModel sag;
  float masterGain{0.0f};
  float inputVolts{2.0f};
  int numStages{0};
  StageModel model{StageModel::Classic};
};

class AmpCircuit : public AmpCircuitCore {
public:
  AmpCircuit() = default;
  AmpCircuit(const AmpCircuit&) = delete;
  AmpCircuit& operator=(const AmpCircuit&) = delete;

  /** Copies the audio state of `other` (the same description, prepared at the
      same rate) into this channel. Only for the classic model: the wave digital
      trees hold references to their own parts and are not copyable. */
  void copyStateFrom(const AmpCircuit& other) noexcept { AmpCircuitCore::operator=(other); }

  bool copyable() const noexcept { return model == StageModel::Classic; }

  void prepare(double rate, const AmpDescription& d) {
    const CalibrationDescription& c = d.calibration;
    numStages = d.numStages;
    model = d.model;
    inputVolts = c.inputFullScaleVolts;
    inputHighpass.setCorner(d.input.network.cornerHz(), rate);
    gridStopper.setCutoff(1.0 / (kTwoPi * d.input.gridStopperOhms * d.input.millerFarads), rate);
    for (int k = 0; k < numStages; ++k) {
      if (model == StageModel::Wdf)
        wdf[k].prepare(rate, d.stages[k].spec);
      else
        classic[k].prepare(rate, d.stages[k].spec);
    }
    // The input stage's grid network is fixed.
    const double leak = d.input.network.leakSeconds();
    const double charge =
        leak * chargeRatio(d.input.sourceOhms + d.input.gridStopperOhms, d.input.network, d.stages[0].spec.triode);
    setGridTimes(0, leak, charge);
    sag.prepare(rate, sagParamsFor(d.voicings[d.defaultVoicing].supply));
    power.prepare(rate, d.power.params);
    reset();
  }

  void reset() noexcept {
    inputHighpass.reset();
    gridStopper.reset();
    for (int k = 0; k < numStages; ++k) {
      if (model == StageModel::Wdf)
        wdf[k].reset();
      else
        classic[k].reset();
    }
    tone.reset();
    power.reset();
    sag.reset();
  }

  void apply(const ControlFrame& f, const AmpDescription& d) noexcept {
    const int pot = d.potStage;
    if (pot >= 0)
      setPot(pot, f.potPosition, f.bright);
    for (int k = 0; k + 1 < numStages; ++k) {
      setCornerOf(k, f.cornerHz[k]);
      setGridTimes(k + 1, f.leakSeconds[k], f.chargeSeconds[k]);
    }
    tone.setDesign(f.tone);
    power.setPresence(f.presence);
    power.setDepth(f.depth);
    sag.setDepth(f.sagDepth);
    sag.setReleaseSeconds(f.sagReleaseSeconds);
    masterGain = f.masterGain;
  }

  /** The preamp and tone stack for `n` samples: `io` in, `v` out. Each stage
      is its own loop over the chunk, so the filters and curves of consecutive
      samples overlap in the processor instead of waiting for one sample to
      travel the whole circuit. */
  void preamp(const float* io, float* v, int n) noexcept {
    if (model == StageModel::Wdf) {
      for (int i = 0; i < n; ++i)
        v[i] = wdf[0].process(gridStopper.lowpass(inputHighpass.process(io[i] * inputVolts)));
      for (int k = 1; k < numStages; ++k)
        for (int i = 0; i < n; ++i)
          v[i] = wdf[k].process(v[i]);
    } else {
      for (int i = 0; i < n; ++i)
        v[i] = classic[0].process(gridStopper.lowpass(inputHighpass.process(io[i] * inputVolts)));
      for (int k = 1; k < numStages; ++k)
        for (int i = 0; i < n; ++i)
          v[i] = classic[k].process(v[i]);
    }
    for (int i = 0; i < n; ++i)
      v[i] = tone.process(v[i]) * masterGain;
  }

  float powerSample(float v) noexcept {
    const float y = power.process(v, sag.headroom());
    sag.update(std::abs(y));
    return y;
  }

  float droop() const noexcept { return sag.droop(); }

private:
  void setPot(int stage, float position, bool bright) noexcept {
    if (model == StageModel::Wdf)
      wdf[stage].setPot(position, bright);
    else
      classic[stage].setPot(position, bright);
  }
  void setCornerOf(int stage, double hz) noexcept {
    if (model == StageModel::Wdf)
      wdf[stage].setCouplingCorner(hz);
    else
      classic[stage].setCouplingCorner(hz);
  }
  void setGridTimes(int stage, double leak, double charge) noexcept {
    if (model == StageModel::Wdf)
      wdf[stage].setGridTimes(leak, charge);
    else
      classic[stage].setGridTimes(leak, charge);
  }

  WdfGainStage wdf[kMaxStages];
};

}  // namespace amp

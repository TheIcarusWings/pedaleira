#pragma once
// amp/SagModel.h: power supply sag.
//
// What it models: the high-voltage rail of a tube amp droops when the output
// stage draws heavy current, because the rectifier and filter capacitors
// cannot hold it up. Less rail means less headroom, so loud passages
// compress and bloom, and the rail recovers over tens of milliseconds
// afterwards. The model is a slow envelope of the output stage's load (the
// rectified output) with a faster attack and a slower recovery, and the
// headroom it hands to the power stage is `1 - depth * load`.
// This is the usual lumped description of rectifier and supply sag; see
// Pakarinen and Yeh, "A review of digital techniques for modeling
// vacuum-tube guitar amplifiers", Computer Music Journal 33(2), 2009. The
// time constants are tuning values, to be set from measurements.
//
// Parameters (SagParams), which can be derived from the supply's components
// with sagParamsFor(SagSupply):
//   attackSeconds    how fast the rail droops when the load rises
//   releaseSeconds   how fast it recovers when the load goes away
//   depth            how much headroom a full load takes (0 = none, 0.3 =
//                    30 percent); can change at any time (a voicing)
// SagSupply (the physical description):
//   plateVolts          the unloaded high-voltage rail, B+
//   sourceResistance    rectifier plus transformer winding resistance, ohms
//   fullLoadAmps        supply current at full output
//   filterCapacitance   the reservoir capacitor, farads
//   attackSeconds       how fast the load builds (an empirical value: it
//                       depends on the program material, not the supply)
// From these: depth = sourceResistance * fullLoadAmps / plateVolts (the
// fraction of the rail lost at full load), releaseSeconds = sourceResistance
// * filterCapacitance (the reservoir recharging through the source).
//
// Interface: update(load) once per sample with the rectified output of the
// power stage, headroom() for the next sample. load() and droop() are for
// meters.
//
// Sample rate: prepare(sampleRate) converts the time constants to per-sample
// coefficients; call update at that rate. Plain C++ and std only. One
// instance per channel.
#include <algorithm>
#include <cmath>

namespace amp {

struct SagParams {
  float attackSeconds{0.008f};
  float releaseSeconds{0.07f};
  float depth{0.2f};
};

struct SagSupply {
  float plateVolts{450.0f};
  float sourceResistance{250.0f};
  float fullLoadAmps{0.3f};
  float filterCapacitance{220.0e-6f};
  float attackSeconds{0.008f};
};

inline SagParams sagParamsFor(const SagSupply& supply) noexcept {
  SagParams out;
  out.depth = std::min(0.9f, supply.sourceResistance * supply.fullLoadAmps / supply.plateVolts);
  out.releaseSeconds = supply.sourceResistance * supply.filterCapacitance;
  out.attackSeconds = supply.attackSeconds;
  return out;
}

class SagModel {
public:
  void prepare(double sampleRate_, const SagParams& newParams) noexcept {
    params = newParams;
    sampleRate = sampleRate_;
    attack = static_cast<float>(1.0 - std::exp(-1.0 / (std::max(1.0e-4, double(params.attackSeconds)) * sampleRate)));
    setReleaseSeconds(params.releaseSeconds);
    reset();
  }
  void setDepth(float depth) noexcept { params.depth = depth; }
  /** Changes the recovery time (for example when the supply changes). */
  void setReleaseSeconds(float seconds) noexcept {
    params.releaseSeconds = seconds;
    release = static_cast<float>(1.0 - std::exp(-1.0 / (std::max(1.0e-4, double(seconds)) * sampleRate)));
  }
  void reset() noexcept { envelope = 0.0f; }

  /** `load` is the rectified output of the power stage (about 0..1 at full
      power). Returns the headroom for the next sample. */
  float update(float load) noexcept {
    const float target = std::min(load, 1.0f);
    envelope += (target > envelope ? attack : release) * (target - envelope);
    if (envelope < 1.0e-20f)
      envelope = 0.0f;
    return headroom();
  }

  /** 1 at rest, falling toward `1 - depth` under full load. */
  float headroom() const noexcept { return 1.0f - params.depth * envelope; }
  /** The smoothed load, 0..1. */
  float load() const noexcept { return envelope; }
  /** How much headroom is lost right now (depth times load). */
  float droop() const noexcept { return params.depth * envelope; }

private:
  SagParams params;
  double sampleRate{48000.0};
  float attack{0.01f}, release{0.001f};
  float envelope{0.0f};
};

}  // namespace amp

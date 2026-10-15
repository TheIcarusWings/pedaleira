#pragma once
// The parameter set a parametric band has in eq.parametric3 and
// eq.parametric8, and the shared ranges of the eq.* kinds.
#include <cmath>
#include "BlockParams.h"
#include "effects/dsp/EqBank.h"

namespace fx::eq {

constexpr float kFreqMinHz = 20.0f;
constexpr float kFreqMaxHz = 20000.0f;
constexpr float kQMin = 0.1f;
constexpr float kQMax = 10.0f;
constexpr float kGainRangeDb = 18.0f;

/** A frequency control; the taper puts its half-way point at the geometric
    mean of its ends. Moving it can change the ring, so it affects the tail. */
inline BlockParamSpec frequencyParam(const juce::String& id, const juce::String& name, float lo,
                                     float hi, float defaultHz) {
  return BlockParamSpec::floatParam(id, name, "Hz", lo, hi, defaultHz)
      .withSkew(skewForMidpoint(lo, hi, std::sqrt(static_cast<double>(lo) * hi)))
      .withAffectsTail();
}

/** Appends one parametric band: [type,] frequency, gain, Q, bypass.
    `typeChoices` empty means a peak-only band without a type control. */
inline void addParametricBand(BlockParamTable& t, int number, const juce::StringArray& typeChoices,
                              float defaultHz) {
  const juce::String n(number);
  const juce::String label = "Band " + n + " ";
  if (typeChoices.size() > 0)
    t.push_back(BlockParamSpec::choiceParam("b" + n + "Type", label + "type", typeChoices, 0)
                    .withAffectsTail());
  t.push_back(
      frequencyParam("b" + n + "Freq", label + "frequency", kFreqMinHz, kFreqMaxHz, defaultHz));
  t.push_back(BlockParamSpec::floatParam("b" + n + "Gain", label + "gain", "dB", -kGainRangeDb,
                                         kGainRangeDb, 0.0f)
                  .withAffectsTail());
  t.push_back(BlockParamSpec::floatParam("b" + n + "Q", label + "Q", "Q", kQMin, kQMax, 1.0f)
                  .withSkew(skewForMidpoint(kQMin, kQMax, 1.0))
                  .withAffectsTail());
  t.push_back(
      BlockParamSpec::boolParam("b" + n + "Bypass", label + "bypass", false).withAffectsTail());
}

/** The per-band "shape" control of eq.parametric3 and eq.parametric8, appended
    after the older parameters. "Default" (0) leaves the band as its type
    control makes it, which is how every band behaved before this control
    existed; the others pick the band's filter outright, as a unit does that
    lets every band be any of the five. The two second-order filters take no
    gain (their Q is the resonance at the corner). */
inline juce::StringArray shapeChoices() {
  return juce::StringArray{"Default", "Peak", "High-pass", "Low-pass", "High shelf", "Low shelf"};
}

inline void addShapeParams(BlockParamTable& t, int bands) {
  for (int b = 1; b <= bands; ++b)
    t.push_back(BlockParamSpec::choiceParam("b" + juce::String(b) + "Shape",
                                            "Band " + juce::String(b) + " shape", shapeChoices(), 0)
                    .withAffectsTail());
}

/** Applies a shape choice to a band; returns false (type untouched) for Default. */
inline bool applyShape(int choice, BandType& type) noexcept {
  switch (choice) {
    case 1: type = BandType::Peak; return true;
    case 2: type = BandType::HighPass2; return true;
    case 3: type = BandType::LowPass2; return true;
    case 4: type = BandType::HighShelf; return true;
    case 5: type = BandType::LowShelf; return true;
    default: return false;
  }
}

}  // namespace fx::eq

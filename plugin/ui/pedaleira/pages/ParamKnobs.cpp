#include "ParamKnobs.h"

#include <cmath>

namespace pedaleira::ui {

juce::String signedDb(double db) {
  if (std::abs(db) < 0.05)
    return "0.0 dB";
  return (db > 0.0 ? "+" : "") + juce::String(db, 1) + " dB";
}

KnobParam KnobParam::fromVar(const juce::var& spec) {
  KnobParam p;
  p.id = spec["id"].toString();
  p.name = spec["name"].toString();
  p.unit = spec["unit"].toString();
  p.type = spec["type"].toString();
  p.minValue = spec["min"];
  p.maxValue = spec["max"];
  p.skew = spec.hasProperty("skew") ? static_cast<double>(spec["skew"]) : 1.0;
  p.defaultValue = spec["default"];
  p.value = spec["value"];
  p.common = spec["scope"].toString() == "common";
  p.affectsLatency = static_cast<bool>(spec["affectsLatency"]);
  p.tempoSync = static_cast<bool>(spec["tempoSync"]);
  if (const auto* labels = spec["choices"].getArray())
    for (const auto& label : *labels)
      p.choices.add(label.toString());
  // The common gains read as the player knows them.
  if (p.id == "inputGain")
    p.name = "Input";
  else if (p.id == "outputGain")
    p.name = "Output";
  return p;
}

double KnobParam::toPosition(double v) const {
  if (maxValue <= minValue)
    return 0.0;
  const double proportion = juce::jlimit(0.0, 1.0, (v - minValue) / (maxValue - minValue));
  // The table's skew, as juce::NormalisableRange applies it.
  return skew == 1.0 ? proportion : std::pow(proportion, skew);
}

double KnobParam::fromPosition(double position) const {
  const double proportion = skew == 1.0 ? position : std::pow(position, 1.0 / skew);
  return minValue + proportion * (maxValue - minValue);
}

juce::String KnobParam::text(double v) const {
  // The common knobs are positions: the gains are 0.5 at unity and span
  // 24 dB either way, mix is a share.
  if (id == "inputGain" || id == "outputGain")
    return signedDb((v - 0.5) * 48.0);
  if (id == "mix")
    return juce::String(juce::roundToInt(v * 100.0)) + "%";
  if (unit == "dB")
    return signedDb(v);
  if (unit == "%")
    return juce::String(juce::roundToInt(v)) + "%";
  if (unit == "Hz" && v >= 1000.0)
    return juce::String(v / 1000.0, 1) + " kHz";
  if (id.containsIgnoreCase("balance") && maxValue > minValue) {
    // How much goes each way, path A first: "50 / 50", "70 / 30".
    const int towardB = juce::roundToInt((v - minValue) / (maxValue - minValue) * 100.0);
    return juce::String(100 - towardB) + " / " + juce::String(towardB);
  }
  if (unit == "pan" || id.containsIgnoreCase("pan")) {
    // Left to right around the middle of the range: "C", "L 30", "R 30".
    const double centre = (minValue + maxValue) * 0.5;
    const double half = (maxValue - minValue) * 0.5;
    const int amount = half > 0.0 ? juce::roundToInt((v - centre) / half * 100.0) : 0;
    return amount == 0 ? juce::String("C") : (amount < 0 ? "L " : "R ") + juce::String(std::abs(amount));
  }
  const juce::String number = std::abs(v) >= 100.0 ? juce::String(juce::roundToInt(v)) : juce::String(v, 1);
  return unit.isEmpty() || unit == "ratio" || unit == "normalized" ? number : number + " " + unit;
}

}  // namespace pedaleira::ui

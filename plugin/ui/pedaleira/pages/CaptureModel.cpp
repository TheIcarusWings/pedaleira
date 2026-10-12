#include "CaptureModel.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace pedaleira::ui::capture {

namespace {
constexpr std::array<const char*, kStepCount> kIds{"source", "wiring", "levels", "record", "check", "train", "review"};
constexpr std::array<const char*, kStepCount> kTitles{"Source", "Wiring", "Levels", "Record", "Check", "Train", "Review"};
}  // namespace

const char* stepTitle(Step step) { return kTitles[static_cast<size_t>(step)]; }
const char* stepId(Step step) { return kIds[static_cast<size_t>(step)]; }

Step stepFromId(const juce::String& id) {
  for (size_t i = 0; i < kIds.size(); ++i)
    if (id == kIds[i])
      return static_cast<Step>(i);
  return Step::source;
}

juce::String sourceName(const juce::String& source) {
  if (source == "pedal") return "Pedal";
  if (source == "ampHead") return "Amp Head";
  if (source == "ampCab") return "Amp + Cab";
  if (source == "modeller") return "Another modeller";
  return source;
}

juce::String gearTypeName(const juce::String& gearType) {
  if (gearType == "ampHead") return "Amp Head";
  if (gearType == "ampCab") return "Amp + Cab";
  if (gearType == "pedal") return "Pedal";
  if (gearType == "cabinet") return "Cabinet";
  if (gearType == "outboard") return "Outboard";
  if (gearType == "spaces") return "Spaces";
  if (gearType == "experimental") return "Experimental";
  return gearType;
}

juce::String dbText(double db, int decimals) {
  if (db <= -90.0)
    return "-inf";
  return juce::String(db, decimals);
}

juce::String clockText(double seconds) {
  const int total = juce::jmax(0, juce::roundToInt(seconds));
  return juce::String(total / 60) + ":" + juce::String(total % 60).paddedLeft('0', 2);
}

double meterFraction(double db) { return juce::jlimit(0.0, 1.0, (db + 60.0) / 60.0); }

juce::String esrText(double esr) { return juce::String(esr, 3); }

LevelState levelStateOf(const juce::String& state) {
  if (state == "low") return LevelState::low;
  if (state == "high") return LevelState::high;
  if (state == "clipping") return LevelState::clipping;
  if (state == "inRange") return LevelState::inRange;
  return LevelState::silent;
}

juce::StringArray gearTypeChoices(const juce::String& current) {
  juce::StringArray choices{"Amp Head", "Pedal", "Amp + Cab"};
  const juce::String name = gearTypeName(current);
  if (current.isNotEmpty() && !choices.contains(name))
    choices.add(name);
  return choices;
}

juce::String gearTypeId(const juce::String& label) {
  if (label == "Amp Head") return "ampHead";
  if (label == "Pedal") return "pedal";
  if (label == "Amp + Cab") return "ampCab";
  if (label == "Cabinet") return "cabinet";
  if (label == "Outboard") return "outboard";
  if (label == "Spaces") return "spaces";
  if (label == "Experimental") return "experimental";
  return label;
}

}  // namespace pedaleira::ui::capture

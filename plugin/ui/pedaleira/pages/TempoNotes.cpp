#include "TempoNotes.h"

namespace pedaleira::ui {

const juce::StringArray& noteBases() {
  static const juce::StringArray bases{"1/1", "1/2", "1/4", "1/8", "1/16", "1/32"};
  return bases;
}

double noteMs(int division, double bpm) {
  const int base = division / 3;
  const int variant = division % 3;
  const double whole = 4.0 * 60000.0 / juce::jmax(20.0, bpm);
  const double straight = whole / static_cast<double>(1 << base);
  return variant == 1 ? straight * 1.5 : variant == 2 ? straight * 2.0 / 3.0 : straight;
}

juce::String noteName(int division) {
  const int base = juce::jlimit(0, noteBases().size() - 1, division / 3);
  const int variant = division % 3;
  return noteBases()[base] + (variant == 1 ? " dotted" : variant == 2 ? " triplet" : "");
}

bool tempoNeighbours(const juce::String& id, juce::String& syncId, juce::String& divisionId) {
  for (const char* suffix : {"timeMs", "TimeMs", "rateHz", "RateHz"}) {
    if (!id.endsWith(suffix))
      continue;
    juce::String prefix = id.dropLastCharacters(juce::String(suffix).length());
    if (prefix.isEmpty()) {
      syncId = "sync";
      divisionId = "division";
    } else {
      syncId = prefix + "Sync";
      divisionId = prefix + "Division";
    }
    return true;
  }
  return false;
}

}  // namespace pedaleira::ui

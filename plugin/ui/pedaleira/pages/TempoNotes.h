// The note values a tempo-synced parameter can take, in the order the engine
// numbers its divisions (3 per base: 1/1, 1/1 dotted, 1/1 triplet, 1/2, ...),
// and what they last at a tempo. Shared by the block editor and the Live page.
#pragma once

#include <juce_core/juce_core.h>

namespace pedaleira::ui {

// "1/1" to "1/32".
const juce::StringArray& noteBases();
// How long a note lasts at a tempo, in milliseconds.
double noteMs(int division, double bpm);
// "1/8", "1/8 dotted", "1/8 triplet".
juce::String noteName(int division);
// The id of the sync switch and the division that go with a time or rate
// parameter (fx::appendTimeParams and appendRateParams name them from the
// same prefix), or false when the id is not one of those.
bool tempoNeighbours(const juce::String& id, juce::String& syncId, juce::String& divisionId);

}  // namespace pedaleira::ui

// The capture wizard's testbed scenarios. A scenario's script is a list of
// steps separated by ";" that bring the engine into a state with commands,
// a simulated audio device (a loopback through a soft clipper, a delay and a
// little noise, as the engine's own capture tests use) and a stand-in for the
// trainer (a shell script that answers like `python`), and then open the
// Capture page on it. Nothing here touches the engine's code.
#pragma once

#include "EngineClient.h"
#include "pages/CapturePage.h"

#include <juce_core/juce_core.h>

class TONE3000Processor;

namespace pedaleira::ui::testbed {

// Plays `script` over `page`; `scratch` is the scenario's data folder.
// Returns an error text, empty when every step ran.
juce::String driveCapture(EngineClient& client, TONE3000Processor& processor, const juce::File& scratch, CapturePage& page,
                          const juce::String& script);

// The script steps, for --list.
const char* captureStepsHelp();

}  // namespace pedaleira::ui::testbed

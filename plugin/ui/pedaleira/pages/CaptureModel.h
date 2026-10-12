// What the capture wizard's screens work from, kept as plain values: the
// steps, the wiring variants, how a level or a time reads, and the wording
// of the engine's ids. No drawing and no engine calls in here.
#pragma once

#include <juce_core/juce_core.h>

namespace pedaleira::ui::capture {

enum class Step { source = 0, wiring, levels, record, check, train, review };
inline constexpr int kStepCount = 7;

// The stepper's names ("Source" to "Review") and the engine's ids ("source").
const char* stepTitle(Step step);
const char* stepId(Step step);
Step stepFromId(const juce::String& id);

// The three ways of wiring the board shows. The first two are both a
// re-amp (the session's method); the box is a choice of the wizard.
enum class Wiring { reampBox, noBox, dryWet };

// The wording of the engine's ids.
juce::String sourceName(const juce::String& source);
juce::String gearTypeName(const juce::String& gearType);

// A level as the screens write it: one decimal, "-inf" under -90.
juce::String dbText(double db, int decimals = 1);
// Minutes and seconds: 72 gives "1:12".
juce::String clockText(double seconds);
// Where a level sits on the -60 to 0 dB meters, 0 to 1.
double meterFraction(double db);
// 0.012 gives "0.012".
juce::String esrText(double esr);

// What the levels run says about the return (capture.levelStatus "state").
enum class LevelState { silent, low, high, clipping, inRange };
LevelState levelStateOf(const juce::String& state);

// The review step offers three kinds of gear; any other the session holds
// (a modeller is an "outboard") is added as a fourth.
juce::StringArray gearTypeChoices(const juce::String& current);
juce::String gearTypeId(const juce::String& label);

}  // namespace pedaleira::ui::capture

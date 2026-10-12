#pragma once
#include <cmath>
#include "BlockParams.h"
#include "BlockProcessor.h"

namespace fx {

// ---------------------------------------------------------------------------
// Note values
// ---------------------------------------------------------------------------
// Eighteen divisions: 1/1, 1/2, 1/4, 1/8, 1/16, 1/32, each straight, dotted
// (x1.5) and triplet (x2/3). The index is what a `division` parameter stores,
// so the order is saved state and never changes:
//
//   index = base * 3 + variant     base 0 = 1/1 .. 5 = 1/32
//                                  variant 0 = straight, 1 = dotted, 2 = triplet

constexpr int kNumNoteDivisions = 18;

enum NoteDivision : int {
  kDivWhole = 0, kDivWholeDotted, kDivWholeTriplet,
  kDivHalf, kDivHalfDotted, kDivHalfTriplet,
  kDivQuarter, kDivQuarterDotted, kDivQuarterTriplet,
  kDivEighth, kDivEighthDotted, kDivEighthTriplet,
  kDivSixteenth, kDivSixteenthDotted, kDivSixteenthTriplet,
  kDivThirtySecond, kDivThirtySecondDotted, kDivThirtySecondTriplet
};

/** The choice labels, in index order: "1/4", "1/4D", "1/4T". */
const juce::StringArray& noteDivisionNames();

/** Length of a division in quarter notes (1/4 = 1, 1/8D = 0.75). An index
    outside the list is clamped. No allocation. */
inline double noteDivisionBeats(int division) noexcept {
  const int index = division < 0 ? 0 : division >= kNumNoteDivisions ? kNumNoteDivisions - 1 : division;
  const int base = index / 3;
  const int variant = index % 3;
  const double straight = 4.0 / static_cast<double>(1 << base);
  return variant == 1 ? straight * 1.5 : variant == 2 ? straight * 2.0 / 3.0 : straight;
}

/** Length of a division in seconds at `bpm`. */
inline double noteDivisionSeconds(int division, double bpm) noexcept {
  return noteDivisionBeats(division) * 60.0 / (bpm > 1.0 ? bpm : 1.0);
}

// ---------------------------------------------------------------------------
// The standard time parameters
// ---------------------------------------------------------------------------
// Every effect with a time (a delay time, a pre-delay, a gate length) exposes
// the same three parameters with the same meaning, so a client that knows
// one knows them all:
//
//   sync      Bool    off: the time is timeMs; on: it is `division` at the tempo
//   division  Choice  a note value (noteDivisionNames)
//   timeMs    Float   the free time in milliseconds
//
// appendTimeParams adds them to a table, in that order, and resolveTimeSeconds
// turns their values and the callback's transport into seconds. A second time
// in the same effect takes a prefix ("left" gives leftSync, leftDivision,
// leftTimeMs).

struct TimeParamOptions {
  float minMs{1.0f};
  float maxMs{2000.0f};
  float defaultMs{350.0f};
  int defaultDivision{kDivQuarter};
  bool defaultSync{false};
  // The time decides how long the effect rings (a delay): flags all three
  // parameters affectsTail, so the reported tail follows them.
  bool affectsTail{false};
  // Display name of the free time ("Time", "Pre-Delay").
  juce::String name{"Time"};
  // Empty for the effect's main time.
  juce::String idPrefix;
};

// Where the three landed in the table; they are always consecutive, so an
// effect's index enum can simply list kSync, kDivision, kTimeMs in a row.
struct TimeParamIndices {
  int sync{-1};
  int division{-1};
  int timeMs{-1};
};

TimeParamIndices appendTimeParams(BlockParamTable& table, const TimeParamOptions& options = {});

/** The time in seconds: `timeMs` when sync is off, else the division at the
    transport's tempo (the host's when it has one, else the rig's own; see
    BlockTransport). Parameter values go in as param() returns them. Audio
    thread; no allocation. */
inline double resolveTimeSeconds(float sync, float division, float timeMs,
                                 const BlockTransport& transport) noexcept {
  if (sync >= 0.5f)
    return noteDivisionSeconds(static_cast<int>(std::lround(division)), transport.bpm);
  return static_cast<double>(timeMs) * 0.001;
}

// ---------------------------------------------------------------------------
// The standard rate parameters
// ---------------------------------------------------------------------------
// The same idea for anything with a speed (an LFO): sync, division and
// rateHz. Synced, the rate is one cycle per division.

struct RateParamOptions {
  float minHz{0.05f};
  float maxHz{20.0f};
  float defaultHz{1.0f};
  int defaultDivision{kDivQuarter};
  bool defaultSync{false};
  juce::String name{"Rate"};
  juce::String idPrefix;
};

struct RateParamIndices {
  int sync{-1};
  int division{-1};
  int rateHz{-1};
};

RateParamIndices appendRateParams(BlockParamTable& table, const RateParamOptions& options = {});

/** The rate in Hz: `rateHz` when sync is off, else one cycle per division at
    the transport's tempo. */
inline double resolveRateHz(float sync, float division, float rateHz,
                            const BlockTransport& transport) noexcept {
  if (sync >= 0.5f)
    return 1.0 / noteDivisionSeconds(static_cast<int>(std::lround(division)), transport.bpm);
  return static_cast<double>(rateHz);
}

}  // namespace fx

#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include <memory>
#include <vector>
#include "BlockDelayLine.h"
#include "SplitJunction.h"

// The grid's model types.
//
// The rig is a 4 x 8 grid of slots. A row is one of the processor's lanes
// (TONE3000Processor::lanes), so everything that already loops the lanes
// covers the grid for free. Two routing modes share the rows:
//
//  - Legacy: today's rig. Rows 0 and 1 are the Left and Right chains, with
//    the optional branch between them; rows 2 and 3 stay empty. The
//    processor's own fields (stereo mode, branch, input mode, the pan and
//    balance parameters) are authoritative and the per-row values here are
//    derived from them. Saved state is unchanged.
//  - Grid: the topology below is authoritative. Rows hold exactly kGridCols
//    slots (an empty slot is an insert placeholder), each row names its own
//    input and output, and saved state carries a Grid node.
//
// Both compile to one ProcessingPlan (ProcessingPlan.h), which is what the
// audio thread runs.

constexpr int kGridRows = 4;
constexpr int kGridCols = 8;
constexpr int kMaxIoPairs = 4;
constexpr int kMaxIoChannels = 8;

enum class RoutingMode { Legacy, Grid };

// What a row's buffer carries: one channel, a true stereo pair, or two
// independent mono signals (two buffer channels, with the block context's
// dualMono flag set and two voices in every NAM engine).
enum class RowFormat { Mono, Stereo, DualMono };

struct RowInput {
  // None = fed by a split or by another row's output, or unused.
  enum class Type { None, Device };
  Type type = Type::None;
  int channel = 0;      // physical device channel, 0-based
  bool stereo = false;  // the pair channel, channel + 1
  // SumToMono is today's mono-chain fold: half of (L + R) onto both.
  enum class Fold { None, SumToMono };
  Fold fold = Fold::None;
  // The row's own trim on top of the rig's input level. Applied where the
  // strip's signal enters the row (GridRow::trimDb is the audio thread's
  // copy), so rows that share a source can trim apart.
  float trimDb = 0.0f;
  bool gate = true;
  bool pitch = false;
};

// The ranges the row mix and input stage accept.
constexpr float kRowVolumeMinDb = -60.0f;  // and below: off
constexpr float kRowVolumeMaxDb = 12.0f;
constexpr float kRowTrimMinDb = -24.0f;
constexpr float kRowTrimMaxDb = 24.0f;

struct RowOutput {
  enum class Type { None, Device, Row };
  Type type = Type::None;
  int pair = 0;    // output pair, 0-based
  int toRow = -1;  // Type::Row: the row this one feeds
  // Read by the audio thread, written by the message thread.
  std::atomic<float> volumeDb{0.0f};
  std::atomic<float> pan{0.5f};
  std::atomic<bool> mute{false};
  std::atomic<bool> solo{false};
  std::atomic<bool> invert{false};
};

struct GridRow {
  RowFormat format = RowFormat::Stereo;
  RowInput input;
  RowOutput output;

  // Audio-thread state, sized under chainMutex.
  // Pads this row to the longest of the rows it is heard with. In Legacy
  // mode that is the other lane, or with a branch the other parallel
  // section (see TONE3000Processor::refreshLatencyCompensation).
  BlockDelayLine outAlign;
  juce::SmoothedValue<float> gainL, gainR;
  // The input trim as the audio thread reads it (input.trimDb is the saved
  // value, under chainMutex), and its smoother.
  std::atomic<float> trimDb{0.0f};
  juce::SmoothedValue<float> trimGain;
  // Peaks of the last chain-rate block in dB (-60 floor): what entered the
  // row after its trim, and what it sent to its bus after volume and pan.
  // (The processor's constructor sets them to the floor.)
  std::array<std::atomic<float>, 2> meterIn{};
  std::array<std::atomic<float>, 2> meterOut{};
};

// A split of one row into the row directly below it, and the optional mix
// back, is a SplitJunction (SplitJunction.h): which row it leaves from, the
// two column boundaries, the splitter's law and the parameters of both ends.
// The lower row of a split has no input of its own; a split nested inside
// another (row 0 into 1, and row 1 into 2 within that range) makes a
// three-way parallel path.
//
// A row feed is the other way rows meet: a row whose output is another row
// (RowOutput::Type::Row) is summed into that row's input, with the rows that
// feed one target kept in step.
struct GridTopology {
  std::array<GridRow, kGridRows> rows;
  // At most one per upper row. Owned here; the audio thread reaches them
  // through TONE3000Processor::rtSplits, a table by upper row that is
  // rebuilt under chainMutex whenever this list changes.
  std::vector<std::unique_ptr<SplitJunction>> splits;
};

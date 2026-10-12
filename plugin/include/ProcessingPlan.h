#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>
#include "GridTopology.h"

// The processing plan: what the chain stage runs, compiled from the rig's
// topology.
//
// The plan is a flat list of operations grouped into sequences. Sequence 0
// is the root; a Fork operation names child sequences that may run on other
// cores. It is compiled on the message thread (or a loader thread) under
// chainMutex and read by the audio thread under the same lock, so a swap is
// a plain assignment: the plan is fixed-capacity and trivially copyable, and
// it holds indices, never pointers.
//
// Two front-ends share the vocabulary. The legacy one emits RunBlocks, Copy,
// Fork and Delay over the stage's own channel pointers. The grid one gives
// every active row a buffer of its own: RowIn fills it from the row's input
// strip, RunBlocks runs the row's slots, RowOut pads, pans and sums it into
// its output bus. Split forks a row into the row below it, AlignMix brings
// the two paths back into the upper row, and FeedRow sums a row's output
// into another row's buffer.

// A view on one or two channels of a working buffer.
struct BufRef {
  enum Kind : std::uint8_t {
    Row,   // a row's own working buffer (grid mode)
    Bus,   // an output bus, by down-port (grid mode)
    Port   // the stage's channel pointers: in place in legacy mode, an input
           // strip's up-port (read only) in grid mode
  };
  std::uint8_t kind = Port;
  std::uint8_t index = 0;
  std::uint8_t firstCh = 0;
  std::uint8_t numCh = 0;
  // The view is narrowed to the host buffer's channel count for the
  // callback: a mono-mode lane runs one channel on a mono host buffer and
  // two on a stereo one.
  bool followHostChannels = false;
};

struct PlanOp {
  enum Type : std::uint8_t {
    RunBlocks,
    Copy,
    Split,
    AlignMix,
    FeedRow,
    RowOut,
    Delay,
    Fork,
    RowIn
  };
  std::uint8_t type = RunBlocks;

  // RunBlocks: the row whose blocks [begin, end) run in place on `a`.
  // Delay: the row whose alignment line (GridRow::outAlign) delays `a`.
  // Split, AlignMix: `row` is the upper row and `row2` the lower one.
  // FeedRow: `row` is the row that feeds and `row2` the row it feeds.
  std::uint8_t row = 0;
  std::uint8_t row2 = 0;
  std::int16_t begin = 0;
  std::int16_t end = 0;

  // RunBlocks, Delay: the buffer worked on. Copy: `a` is copied onto `b`.
  // RowIn: `a` is the strip's port (numCh 0 when the row's input channel is
  // missing: the row runs from silence) and `b` the row's buffer. RowOut:
  // `a` is the row's buffer and `b` its bus. Split, AlignMix: `a` is the
  // upper row's buffer and `b` the lower row's. FeedRow: `a` is the feeding
  // row's buffer and `b` the fed row's.
  BufRef a, b;

  // RowOut: the first operation that writes its bus in this plan. It
  // replaces the bus's contents; the others add to them. FeedRow: likewise
  // the first feed into its target row.
  bool firstOnBus = false;

  // Split, AlignMix: the junction, named by its upper row (the processor
  // keeps a table by upper row; a plan holds no pointers).
  std::int16_t junction = -1;

  // RunBlocks: a RowFormat. A legacy segment also sets followRigDualMono:
  // whether dual mono is in effect is then the rig's answer for the
  // callback (it needs two channels in and out), not the plan's.
  // Split, AlignMix, FeedRow: the RowFormat of `row`, with `format2` that
  // of `row2`.
  std::uint8_t format = 0;
  std::uint8_t format2 = 0;
  bool followRigDualMono = false;

  // RunBlocks: what follows this segment's blocks, for the "is there an amp
  // after me" question (calibrated hand-off, stereo IRs). [scanBegin,
  // scanEnd) is the segment's own row; `cont` continues in the rows it
  // joins or feeds. A legacy plan scans the whole lane and continues
  // nowhere.
  std::int16_t scanBegin = 0;
  std::int16_t scanEnd = 0;
  struct Range {
    std::uint8_t row = 0;
    std::int16_t begin = 0;
    std::int16_t end = 0;
  };
  std::array<Range, 3> cont{};
  std::uint8_t numCont = 0;

  // Fork: the sequences that run side by side. child[0] stays on the
  // calling thread.
  std::array<std::uint8_t, 4> child{};
  std::uint8_t numChildren = 0;

  // Filled by the latency pass, in chain-rate samples. RunBlocks: the sum
  // of its blocks' latencies. Delay, RowOut, FeedRow: the pad. AlignMix:
  // what the upper path waits (`latency`) and what the lower one does
  // (`latency2`); the later of the two waits nothing.
  std::int32_t latency = 0;
  std::int32_t latency2 = 0;
};

struct PlanSeq {
  std::uint16_t firstOp = 0;
  std::uint16_t numOps = 0;
};

struct ProcessingPlan {
  static constexpr int kMaxOps = 96;
  static constexpr int kMaxSeqs = 24;

  std::array<PlanOp, kMaxOps> ops{};
  std::array<PlanSeq, kMaxSeqs> seqs{};
  int numOps = 0;
  int numSeqs = 0;

  // Input strips and output buses: the host-rate ends of the chain stage
  // (TONE3000Processor::runInputStrip / runOutputStrip). Strip `port` is the
  // boundary up-port its two channels enter the chain rate through, bus
  // `port` the down-port they leave by; the ports of each side are numbered
  // 0 to count - 1 without gaps. A legacy plan has one of each, the main
  // pair.
  struct Strip {
    // Where the strip's signal comes from and which input stages it runs.
    // The channel is a physical device channel; the processor finds the
    // buffer channel through its device channel map.
    RowInput in;
    std::uint8_t port = 0;
    // Unused: a strip that does not run the pitch shifter is padded by the
    // shifter's latency while it is powered, which follows a parameter and
    // not the plan (TONE3000Processor::runInputStrip).
    int padHostSamples = 0;
    // Legacy: the strip is the host buffer's first two channels, in place,
    // whichever device channels those are, and what is folded onto them is
    // the rig's input mode for the callback, not `in.fold`.
    bool hostMainPair = false;
  };
  struct Bus {
    std::uint8_t pair = 0;  // output pair, 0-based: device channels 2 * pair and 2 * pair + 1
    std::uint8_t port = 0;
    // The main output strip: tone stack, the auto-align mute, the master
    // output level and the faceplate meters, and in a legacy plan the image
    // stage. Every bus runs the DC blocker and the chain-edit fade.
    bool mainStrip = false;
    // Legacy: the bus is the host buffer's first two channels, in place.
    bool hostMainPair = false;
    // Grid: some RowOut writes this bus. A bus nobody writes is cleared by
    // the chain stage.
    bool hasWriter = false;
  };
  std::array<Strip, kMaxIoPairs> strips{};
  int numStrips = 0;
  std::array<Bus, kMaxIoPairs> buses{};
  int numBuses = 0;

  // Filled by the latency pass: what the chain's blocks add to the
  // reported latency, in chain-rate samples.
  int reportedChainLatency = 0;
  // The plan CPU value, as fractions of
  // one core: every compiled block plus the port overheads, and the largest
  // share that has to run on one thread (what the callback sees with
  // multi-core on). From the block costs at compile time; computePlanCpu
  // gives the live figures.
  float cpuEstimate = 0.0f;
  float cpuCritical = 0.0f;

  // Grid: what became of each row.
  struct RowInfo {
    // Where the row's signal comes from and where it goes. A row runs when
    // it has both.
    enum Source : std::uint8_t { NoSource, FromDevice, FromSplit, FromFeed };
    enum Sink : std::uint8_t { NoSink, ToDevice, ToRow, ToMix };

    bool active = false;          // it is run: something reaches it and it reaches an output
    bool inputMissing = false;    // its input channel is not on this device
    bool outputFallback = false;  // its output pair is not: sent to Out 1/2
    std::uint8_t source = NoSource;
    std::uint8_t sink = NoSink;
    // The slots that run, [begin, end): all eight, or for the lower row of
    // a split the split's range (to the end of the row without a rejoin).
    // The slots outside it are kept and not run.
    std::int8_t begin = 0;
    std::int8_t end = 0;
    // The RowFormat the row runs in: its own, except that the lower row of
    // a split from a dual-mono row is dual mono too.
    std::uint8_t format = 0;
    std::int8_t strip = -1;       // index into strips, -1 without one
    std::int8_t bus = -1;         // index into buses, -1 when it sends to none
    std::int8_t group = -1;       // rows that are processed together
    std::int8_t feedsRow = -1;    // ToRow: the row it feeds
    // Bit n: row n feeds this row, directly or through other rows (the row
    // itself included). Solo on any of them keeps this row's send open.
    std::uint8_t upstream = 0;
    // How late the row's signal is where it leaves the row (its send, its
    // feed or the mix point), before the pad; chain-rate samples.
    std::int32_t latency = 0;
    std::int32_t pad = 0;         // what it waits there for the paths it meets
  };
  std::array<RowInfo, kGridRows> rowInfo{};
  int numGroups = 0;

  // Grid: what became of each split, by upper row.
  struct SplitInfo {
    bool present = false;         // the row has a split
    bool lowerActive = false;     // the lower path is run (it reaches an output)
    std::int8_t splitCol = 0;
    std::int8_t mixCol = -1;
    // From the latency pass: the mixer's two alignment delays.
    std::int32_t alignA = 0;
    std::int32_t alignB = 0;
  };
  std::array<SplitInfo, kGridRows> splitInfo{};

  // The topology this plan was compiled from, and how many slots each row
  // had then. The audio thread checks both against the live rig.
  std::uint32_t topologyRevision = 0;
  std::array<std::int16_t, kGridRows> rowSlots{};

  bool legacy = true;
  // False when the input could not be compiled; such a plan is never
  // installed.
  bool valid = false;
};

static_assert(std::is_trivially_copyable<ProcessingPlan>::value,
              "the plan is swapped by assignment under chainMutex");

// One slot of a row as the compiler sees it.
struct PlanSlot {
  bool isBlock = false;        // false for an insert placeholder
  int latency = 0;             // chain-rate samples
  float costEstimate = 0.0f;   // fraction of one core (ChainBlock::cpuCost)
};

// Everything compilePlan reads: a snapshot with no reference to the
// processor, so the compiler can be tested on its own. The slot lists are
// vectors because a legacy lane has no fixed length; the snapshot is built
// off the audio thread.
struct PlanInput {
  RoutingMode mode = RoutingMode::Legacy;
  std::uint32_t topologyRevision = 0;

  // Legacy mode: the processor's own routing fields.
  struct Legacy {
    bool stereoEnabled = false;
    // The trunk lane's row and the index of the tapped block in it, or -1
    // without a live branch (none, or dormant in mono mode).
    int trunkRow = 0;
    int branchTapIndex = -1;
  } legacy;

  struct Row {
    RowFormat format = RowFormat::Stereo;
    RowInput input;
    RowOutput::Type outputType = RowOutput::Type::None;
    int outputPair = 0;
    int outputToRow = -1;
    std::vector<PlanSlot> slots;
  };
  std::array<Row, kGridRows> rows;

  // Grid mode: the splits. A split forks
  // `upperRow` at the boundary `splitCol` (0 to 8, between slot splitCol - 1
  // and slot splitCol) into the row below, and brings the two paths back
  // into the upper row at `mixCol`, or never when that is -1.
  struct Split {
    int upperRow = 0;
    int splitCol = 0;
    int mixCol = -1;
  };
  std::array<Split, kGridRows> splits{};
  int numSplits = 0;

  // Grid mode: what the device offers.
  // Bit n of a mask is physical channel n. Until a device is known every
  // channel counts as present.
  struct Io {
    bool known = false;
    std::uint32_t inputChannels = 0;
    std::uint32_t outputChannels = 0;
    // Pad every bus to the latest one, so that a single reported latency is
    // right for all outputs (a plugin). Off, rows are aligned only with the
    // rows they share an output with (the standalone app).
    bool padAcrossBuses = false;
    // What one port costs, as a fraction of one core (strip or bus, its
    // resampler and oversampler included).
    float portCost = 0.0f;
  } io;
};

// What one row costs, and the plan's two totals (see ProcessingPlan).
struct PlanCpu {
  float total = 0.0f;
  float critical = 0.0f;
  std::array<float, kGridRows> rows{};
};

// Compile the plan's structure and run the latency pass over it. Pure. A
// plan that comes back with `valid` false must not be installed; `whyNot`
// then receives the reason.
ProcessingPlan compilePlan(const PlanInput& input, std::string* whyNot = nullptr);

// The grid's routing rules on their own (compilePlan applies them too):
//  - a split goes from a row into the row directly below it, one per upper
//    row, and the lower row has no input of its own;
//  - it forks at a boundary 0 to 8 and rejoins at a later one, or not at all;
//  - a split of a lower row lies inside its parent's range;
//  - a row feed goes to another row that has no input of its own and is not
//    a split's lower row, and the feeds and splits together form no loop.
// Returns an empty string when the routing holds, else the reason in words
// a user can act on. `badSplitRow` then receives the upper row of the split
// at fault and `badFeedRow` the feeding row at fault (-1 when it is not
// that kind of fault). Pure.
std::string validateGridRouting(const PlanInput& input, int* badSplitRow = nullptr,
                                int* badFeedRow = nullptr);

// The format a grid row runs in (see ProcessingPlan::RowInfo::format). Pure.
RowFormat effectiveRowFormat(const PlanInput& input, int row);

// The plan CPU value from the block costs in `input`. Pure.
PlanCpu computePlanCpu(const ProcessingPlan& plan, const PlanInput& input);

// The latency pass on its own: block latencies from `input` onto the plan's
// segments, the pads that keep parallel paths in step, and the total. The
// structure is not touched, so this is all that has to run when a block's
// latency moved but no block did.
void computePlanLatency(ProcessingPlan& plan, const PlanInput& input);

// Check the plan's invariants against the input it was compiled from:
// every slot of an active row is run exactly once and in order, every
// sequence is reached exactly once, fork children write disjoint buffers,
// the paths that meet at a group of Delay operations are equally late
// after their pads, and in a grid plan every active row is filled once
// before it runs (from its strip, by its split or by the rows that feed it)
// and handed on once after (to its bus, its mixer or the row it feeds),
// only the slots of its range are run, and the paths that meet at a mixer,
// in a fed row or on a bus are equally late. Returns an empty string when
// the plan holds, else the first violation. Not for the audio thread.
std::string validatePlan(const ProcessingPlan& plan, const PlanInput& input);

// The plan as text, one operation per line (reports, test failures). Not
// for the audio thread.
std::string describePlan(const ProcessingPlan& plan);

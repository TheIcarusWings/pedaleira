#include "ProcessingPlan.h"

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <vector>

// The plan compiler (see ProcessingPlan.h). Pure functions over plain data:
// nothing here knows the processor, and nothing here runs on the audio
// thread.

namespace {

// Appends operations and sequences with the capacity checks in one place.
// Running out marks the plan invalid; the caller keeps the plan it had.
struct PlanBuilder {
  ProcessingPlan& plan;
  bool overflow = false;
  PlanOp spill;  // takes the writes after an overflow; never read

  // Opens a sequence that starts at the next operation. Sequences are
  // contiguous, so one is filled completely before the next is opened.
  int beginSeq() {
    if (plan.numSeqs >= ProcessingPlan::kMaxSeqs) {
      overflow = true;
      return 0;
    }
    plan.seqs[static_cast<std::size_t>(plan.numSeqs)] = {static_cast<std::uint16_t>(plan.numOps),
                                                         0};
    return plan.numSeqs++;
  }

  PlanOp& add(int seq, PlanOp::Type type) {
    if (plan.numOps >= ProcessingPlan::kMaxOps) {
      overflow = true;
      spill = {};
      return spill;
    }
    PlanOp& op = plan.ops[static_cast<std::size_t>(plan.numOps++)];
    op = {};
    op.type = type;
    ++plan.seqs[static_cast<std::size_t>(seq)].numOps;
    return op;
  }
};

BufRef portRef(int firstCh, int numCh, bool followHostChannels = false) {
  BufRef ref;
  ref.kind = BufRef::Port;
  ref.index = 0;
  ref.firstCh = static_cast<std::uint8_t>(firstCh);
  ref.numCh = static_cast<std::uint8_t>(numCh);
  ref.followHostChannels = followHostChannels;
  return ref;
}

void addLegacyRun(PlanBuilder& b, int seq, int row, int begin, int end, int rowSlots,
                  const BufRef& buffer, RowFormat format) {
  PlanOp& op = b.add(seq, PlanOp::RunBlocks);
  op.row = static_cast<std::uint8_t>(row);
  op.begin = static_cast<std::int16_t>(begin);
  op.end = static_cast<std::int16_t>(end);
  op.a = buffer;
  op.format = static_cast<std::uint8_t>(format);
  // Dual mono needs two channels in and two out, which the rig only
  // answers per callback.
  op.followRigDualMono = true;
  // The whole lane, also for a segment of it: a branched trunk is still one
  // chain split around the tap, not two chains.
  op.scanBegin = 0;
  op.scanEnd = static_cast<std::int16_t>(rowSlots);
}

void addDelay(PlanBuilder& b, int seq, int row, const BufRef& buffer) {
  PlanOp& op = b.add(seq, PlanOp::Delay);
  op.row = static_cast<std::uint8_t>(row);
  op.a = buffer;
}

// Today's three shapes. The order of the
// operations is the order processChainStage always ran them in.
void compileLegacyPlan(const PlanInput& input, ProcessingPlan& plan) {
  PlanBuilder b{plan, false, {}};
  const int slots0 = static_cast<int>(input.rows[0].slots.size());
  const int slots1 = static_cast<int>(input.rows[1].slots.size());
  if (slots0 > INT16_MAX || slots1 > INT16_MAX)
    return;  // not a rig anyone can build; refuse rather than wrap

  const int root = b.beginSeq();

  if (!input.legacy.stereoEnabled) {
    // Mono mode: the one lane on both channels of the main pair, or on the
    // single channel of a mono host buffer. The right lane and a branch lie
    // dormant and are not compiled.
    addLegacyRun(b, root, 0, 0, slots0, slots0, portRef(0, 2, true), RowFormat::Stereo);
  } else if (input.legacy.branchTapIndex < 0) {
    // Stereo mode: each channel is its own mono lane, then whichever lane
    // has less block latency waits for the other.
    PlanOp& fork = b.add(root, PlanOp::Fork);
    addDelay(b, root, 0, portRef(0, 1));
    addDelay(b, root, 1, portRef(1, 1));
    const int left = b.beginSeq();
    addLegacyRun(b, left, 0, 0, slots0, slots0, portRef(0, 1), RowFormat::Mono);
    const int right = b.beginSeq();
    addLegacyRun(b, right, 1, 0, slots1, slots1, portRef(1, 1), RowFormat::Mono);
    fork.child = {static_cast<std::uint8_t>(left), static_cast<std::uint8_t>(right), 0, 0};
    fork.numChildren = 2;
  } else {
    // Branched: the trunk up to and including the tapped block, the tap
    // copied onto the branch lane's channel, then the trunk's remainder
    // beside the branch lane, then the shorter of those two waits.
    const int trunk = input.legacy.trunkRow == 1 ? 1 : 0;
    const int branch = 1 - trunk;
    const int trunkSlots = trunk == 0 ? slots0 : slots1;
    const int branchSlots = trunk == 0 ? slots1 : slots0;
    const int tapEnd = input.legacy.branchTapIndex + 1;
    if (tapEnd > trunkSlots)
      return;  // the tap is not in the trunk lane

    addLegacyRun(b, root, trunk, 0, tapEnd, trunkSlots, portRef(trunk, 1), RowFormat::Mono);
    PlanOp& copy = b.add(root, PlanOp::Copy);
    copy.a = portRef(trunk, 1);
    copy.b = portRef(branch, 1);
    PlanOp& fork = b.add(root, PlanOp::Fork);
    addDelay(b, root, trunk, portRef(trunk, 1));
    addDelay(b, root, branch, portRef(branch, 1));
    const int trunkRest = b.beginSeq();
    addLegacyRun(b, trunkRest, trunk, tapEnd, trunkSlots, trunkSlots, portRef(trunk, 1),
                 RowFormat::Mono);
    const int branchSeq = b.beginSeq();
    addLegacyRun(b, branchSeq, branch, 0, branchSlots, branchSlots, portRef(branch, 1),
                 RowFormat::Mono);
    fork.child = {static_cast<std::uint8_t>(trunkRest), static_cast<std::uint8_t>(branchSeq), 0,
                  0};
    fork.numChildren = 2;
  }

  // The main pair in, the main pair out, both in place on the host buffer's
  // first two channels. The strip runs every input stage; which channel is
  // folded onto which is the input mode's answer per callback.
  plan.numStrips = 1;
  ProcessingPlan::Strip& strip = plan.strips[0];
  strip.in.type = RowInput::Type::Device;
  strip.in.channel = 0;
  strip.in.stereo = true;
  strip.in.gate = true;
  strip.in.pitch = true;
  strip.port = 0;
  strip.hostMainPair = true;
  plan.numBuses = 1;
  plan.buses[0].pair = 0;
  plan.buses[0].port = 0;
  plan.buses[0].mainStrip = true;
  plan.buses[0].hostMainPair = true;

  plan.valid = !b.overflow;
}


BufRef rowRef(int row, int numCh) {
  BufRef ref;
  ref.kind = BufRef::Row;
  ref.index = static_cast<std::uint8_t>(row);
  ref.firstCh = 0;
  ref.numCh = static_cast<std::uint8_t>(numCh);
  return ref;
}

BufRef busRef(int bus) {
  BufRef ref;
  ref.kind = BufRef::Bus;
  ref.index = static_cast<std::uint8_t>(bus);
  ref.firstCh = 0;
  ref.numCh = 2;
  return ref;
}

int rowChannels(RowFormat format) { return format == RowFormat::Mono ? 1 : 2; }

bool inputChannelPresent(const PlanInput& input, int channel) {
  return !input.io.known || (channel >= 0 && channel < 32 && ((input.io.inputChannels >> channel) & 1u) != 0);
}

// A pair is there when its first channel is; a device with an odd channel
// count folds the last pair to mono on the way out.
bool outputPairPresent(const PlanInput& input, int pair) {
  return !input.io.known ||
         (pair >= 0 && pair < 16 && ((input.io.outputChannels >> (2 * pair)) & 1u) != 0);
}

// Rows that share a device source share a strip, as long as they also agree
// on the stages a strip runs.
bool sameStripSource(const RowInput& a, const RowInput& b) {
  return a.channel == b.channel && a.stereo == b.stereo && a.fold == b.fold && a.gate == b.gate &&
         a.pitch == b.pitch;
}

using RowInfo = ProcessingPlan::RowInfo;

// What the grid's routing amounts to, row by row: the one reading of the
// rules that the compiler, the validator and the routing check share.
//
// A row runs when something reaches it and it reaches an output. What
// reaches it is its device input, the split of the row above (when that row
// runs) or the rows that feed it (when one of them runs). What it reaches
// is a device output, the row it feeds (when that row reaches an output) or,
// for the lower row of a split that rejoins, the mixer (when the upper row
// reaches an output). The output setting of such a lower row is not used.
struct GridAnalysis {
  std::string error;
  int badSplit = -1;  // upper row of the split at fault
  int badFeed = -1;   // feeding row at fault

  struct Row {
    bool active = false;
    bool sourceLive = false;
    bool sinkLive = false;
    int begin = 0;
    int end = 0;
    RowInfo::Source source = RowInfo::NoSource;
    RowInfo::Sink sink = RowInfo::NoSink;
    int feedsRow = -1;  // the declared feed target, live or not
    RowFormat format = RowFormat::Stereo;
    int group = -1;
    unsigned upstream = 0;
  };
  std::array<Row, kGridRows> rows{};

  // By upper row.
  struct Split {
    bool present = false;
    int splitCol = 0;
    int mixCol = -1;
    bool lowerActive = false;
    bool rejoins() const { return mixCol >= 0; }
  };
  std::array<Split, kGridRows> splits{};
  int numGroups = 0;

  bool ok() const { return error.empty(); }
  // The row is the lower row of a split.
  bool isLower(int row) const { return row > 0 && splits[static_cast<std::size_t>(row - 1)].present; }
  const Split& splitAbove(int row) const { return splits[static_cast<std::size_t>(row - 1)]; }
};

std::string rowText(int row) { return "row " + std::to_string(row); }

GridAnalysis analyzeGrid(const PlanInput& input) {
  GridAnalysis an;
  const auto failSplit = [&an](int upper, const std::string& why) {
    if (an.error.empty()) {
      an.error = why;
      an.badSplit = upper;
    }
  };
  const auto failFeed = [&an](int row, const std::string& why) {
    if (an.error.empty()) {
      an.error = why;
      an.badFeed = row;
    }
  };

  // ---- Splits: one per upper row, into the row directly below.
  if (input.numSplits < 0 || input.numSplits > kGridRows) {
    an.error = "the grid names more splits than it has rows";
    return an;
  }
  for (int i = 0; i < input.numSplits; ++i) {
    const PlanInput::Split& split = input.splits[static_cast<std::size_t>(i)];
    const int upper = split.upperRow;
    if (upper < 0 || upper >= kGridRows) {
      an.error = "a split names a row the grid does not have";
      return an;
    }
    if (upper == kGridRows - 1) {
      failSplit(upper, rowText(upper) + " is the last row: there is no row below it to split into");
      continue;
    }
    GridAnalysis::Split& entry = an.splits[static_cast<std::size_t>(upper)];
    if (entry.present) {
      failSplit(upper, rowText(upper) + " already has a split (one per row)");
      continue;
    }
    entry.present = true;
    entry.splitCol = split.splitCol;
    entry.mixCol = split.mixCol < 0 ? -1 : split.mixCol;
    if (split.splitCol < 0 || split.splitCol > kGridCols)
      failSplit(upper, "the split of " + rowText(upper) + " forks at boundary " +
                           std::to_string(split.splitCol) + "; boundaries run from 0 to " +
                           std::to_string(kGridCols));
    else if (entry.rejoins() && (entry.mixCol <= entry.splitCol || entry.mixCol > kGridCols))
      failSplit(upper, "the split of " + rowText(upper) + " forks at boundary " +
                           std::to_string(entry.splitCol) + " and has to rejoin at a later one, up to " +
                           std::to_string(kGridCols) + ", not at " + std::to_string(entry.mixCol));
    if (input.rows[static_cast<std::size_t>(upper + 1)].input.type != RowInput::Type::None)
      failSplit(upper, rowText(upper + 1) + " has an input of its own, so " + rowText(upper) +
                           " cannot split into it");
  }
  if (!an.ok())
    return an;

  // ---- Ranges. A split of a lower row lies inside the range that row runs.
  for (int row = 0; row < kGridRows; ++row) {
    GridAnalysis::Row& r = an.rows[static_cast<std::size_t>(row)];
    r.begin = 0;
    r.end = kGridCols;
    if (an.isLower(row)) {
      const GridAnalysis::Split& parent = an.splitAbove(row);
      r.begin = parent.splitCol;
      r.end = parent.rejoins() ? parent.mixCol : kGridCols;
    }
    const GridAnalysis::Split& own = an.splits[static_cast<std::size_t>(row)];
    if (own.present && an.isLower(row) &&
        (own.splitCol < r.begin || own.splitCol > r.end || (own.rejoins() && own.mixCol > r.end)))
      failSplit(row, "the split of " + rowText(row) + " has to lie inside the split above it, which " +
                         "runs from boundary " + std::to_string(r.begin) + " to " +
                         std::to_string(r.end));
  }
  if (!an.ok())
    return an;

  // ---- Feeds. The output of the lower row of a rejoining split is not
  // used, so it declares no feed.
  const auto dormantOutput = [&an](int row) {
    return an.isLower(row) && an.splitAbove(row).rejoins();
  };
  for (int row = 0; row < kGridRows; ++row) {
    const PlanInput::Row& in = input.rows[static_cast<std::size_t>(row)];
    if (in.outputType != RowOutput::Type::Row || dormantOutput(row))
      continue;
    const int target = in.outputToRow;
    if (target < 0 || target >= kGridRows)
      failFeed(row, rowText(row) + " feeds a row the grid does not have");
    else if (target == row)
      failFeed(row, rowText(row) + " cannot feed itself");
    else if (input.rows[static_cast<std::size_t>(target)].input.type != RowInput::Type::None)
      failFeed(row, rowText(target) + " has an input of its own, so " + rowText(row) +
                        " cannot feed it");
    else if (an.isLower(target))
      failFeed(row, rowText(target) + " is the lower row of a split, so " + rowText(row) +
                        " cannot feed it");
    else
      an.rows[static_cast<std::size_t>(row)].feedsRow = target;
  }
  if (!an.ok())
    return an;

  // No loop through feeds and splits: follow what each row hands its signal
  // to (the row it feeds, the row it splits into).
  {
    std::array<int, kGridRows> state{};  // 0 = unseen, 1 = on the path, 2 = done
    int loopRow = -1;
    const auto visit = [&](const auto& self, int row) -> void {
      if (state[static_cast<std::size_t>(row)] == 2 || loopRow >= 0)
        return;
      if (state[static_cast<std::size_t>(row)] == 1) {
        loopRow = row;
        return;
      }
      state[static_cast<std::size_t>(row)] = 1;
      if (const int target = an.rows[static_cast<std::size_t>(row)].feedsRow; target >= 0)
        self(self, target);
      if (an.splits[static_cast<std::size_t>(row)].present)
        self(self, row + 1);
      state[static_cast<std::size_t>(row)] = 2;
    };
    for (int row = 0; row < kGridRows && loopRow < 0; ++row)
      visit(visit, row);
    if (loopRow >= 0) {
      // Name a feed on the loop: that is what the user can take away.
      int feeder = -1;
      for (int row = 0; row < kGridRows; ++row)
        if (an.rows[static_cast<std::size_t>(row)].feedsRow == loopRow)
          feeder = row;
      failFeed(feeder, "the row feeds form a loop through " + rowText(loopRow));
      return an;
    }
  }

  // ---- Formats: a row's own, but two independent signals stay two: the
  // lower row of a split from a dual-mono row is dual mono.
  for (int row = 0; row < kGridRows; ++row) {
    GridAnalysis::Row& r = an.rows[static_cast<std::size_t>(row)];
    r.format = input.rows[static_cast<std::size_t>(row)].format;
    if (an.isLower(row) && an.rows[static_cast<std::size_t>(row - 1)].format == RowFormat::DualMono)
      r.format = RowFormat::DualMono;
  }

  // ---- What each row reaches. Never through the rows it forks into, so
  // this needs only the feed chain, which has no loop.
  const auto reaches = [&](const auto& self, int row, int depth) -> bool {
    if (depth > kGridRows)
      return false;
    if (dormantOutput(row))
      return self(self, row - 1, depth + 1);
    const PlanInput::Row& in = input.rows[static_cast<std::size_t>(row)];
    if (in.outputType == RowOutput::Type::Device)
      return true;
    if (const int target = an.rows[static_cast<std::size_t>(row)].feedsRow; target >= 0)
      return self(self, target, depth + 1);
    return false;
  };
  for (int row = 0; row < kGridRows; ++row) {
    GridAnalysis::Row& r = an.rows[static_cast<std::size_t>(row)];
    r.sinkLive = reaches(reaches, row, 0);
    const PlanInput::Row& in = input.rows[static_cast<std::size_t>(row)];
    if (dormantOutput(row))
      r.sink = RowInfo::ToMix;
    else if (in.outputType == RowOutput::Type::Device)
      r.sink = RowInfo::ToDevice;
    else if (r.feedsRow >= 0)
      r.sink = RowInfo::ToRow;
  }

  // ---- What reaches each row. A row waits for the rows it takes its
  // signal from, so settle them in rounds (four rows: four rounds at most).
  for (int round = 0; round < kGridRows; ++round)
    for (int row = 0; row < kGridRows; ++row) {
      GridAnalysis::Row& r = an.rows[static_cast<std::size_t>(row)];
      const PlanInput::Row& in = input.rows[static_cast<std::size_t>(row)];
      if (in.input.type == RowInput::Type::Device) {
        r.source = RowInfo::FromDevice;
        r.sourceLive = true;
      } else if (an.isLower(row)) {
        r.source = RowInfo::FromSplit;
        r.sourceLive = an.rows[static_cast<std::size_t>(row - 1)].active;
      } else {
        r.sourceLive = false;
        for (int feeder = 0; feeder < kGridRows; ++feeder)
          if (an.rows[static_cast<std::size_t>(feeder)].feedsRow == row) {
            r.source = RowInfo::FromFeed;
            r.sourceLive = r.sourceLive || an.rows[static_cast<std::size_t>(feeder)].active;
          }
      }
      r.active = r.sourceLive && r.sinkLive;
    }
  for (int upper = 0; upper + 1 < kGridRows; ++upper)
    an.splits[static_cast<std::size_t>(upper)].lowerActive =
        an.splits[static_cast<std::size_t>(upper)].present &&
        an.rows[static_cast<std::size_t>(upper + 1)].active;

  // ---- Groups: the rows a split or a feed ties together are processed as
  // one. Numbered in the order of their first row.
  std::array<int, kGridRows> parent{};
  for (int row = 0; row < kGridRows; ++row)
    parent[static_cast<std::size_t>(row)] = row;
  const auto find = [&parent](int row) {
    while (parent[static_cast<std::size_t>(row)] != row)
      row = parent[static_cast<std::size_t>(row)];
    return row;
  };
  const auto unite = [&](int a, int b) {
    const int ra = find(a), rb = find(b);
    parent[static_cast<std::size_t>(std::max(ra, rb))] = std::min(ra, rb);
  };
  for (int row = 0; row < kGridRows; ++row) {
    const GridAnalysis::Row& r = an.rows[static_cast<std::size_t>(row)];
    if (!r.active)
      continue;
    if (an.splits[static_cast<std::size_t>(row)].lowerActive)
      unite(row, row + 1);
    if (r.sink == RowInfo::ToRow)
      unite(row, r.feedsRow);  // the target runs: this row reaches an output through it
  }
  for (int row = 0; row < kGridRows; ++row) {
    GridAnalysis::Row& r = an.rows[static_cast<std::size_t>(row)];
    if (!r.active)
      continue;
    const int root = find(row);
    r.group = root == row ? an.numGroups++ : an.rows[static_cast<std::size_t>(root)].group;
  }

  // ---- Which rows a row's send carries: itself, the rows that feed it and
  // theirs, and for a lower row whatever feeds the row it was forked from.
  const auto feedAncestors = [&](const auto& self, int row, int depth) -> unsigned {
    unsigned mask = 0;
    if (depth > 2 * kGridRows)
      return mask;
    for (int feeder = 0; feeder < kGridRows; ++feeder) {
      const GridAnalysis::Row& f = an.rows[static_cast<std::size_t>(feeder)];
      if (f.active && f.sink == RowInfo::ToRow && f.feedsRow == row)
        mask |= (1u << feeder) | self(self, feeder, depth + 1);
    }
    if (an.isLower(row))
      mask |= self(self, row - 1, depth + 1);
    return mask;
  };
  for (int row = 0; row < kGridRows; ++row)
    if (an.rows[static_cast<std::size_t>(row)].active)
      an.rows[static_cast<std::size_t>(row)].upstream =
          (1u << row) | feedAncestors(feedAncestors, row, 0);
  return an;
}

// Sequences under construction. A fork's children are filled before the
// fork itself is appended, so the plan's contiguous layout (PlanSeq) is only
// produced at the end, by flatten.
struct PlanDraft {
  std::vector<std::vector<PlanOp>> seqs;

  int newSeq() {
    seqs.emplace_back();
    return static_cast<int>(seqs.size()) - 1;
  }
  // The reference is good until the next add to the same sequence.
  PlanOp& add(int seq, PlanOp::Type type) {
    auto& ops = seqs[static_cast<std::size_t>(seq)];
    ops.emplace_back();
    ops.back().type = type;
    return ops.back();
  }
  void addFork(int seq, const std::vector<int>& children) {
    PlanOp& fork = add(seq, PlanOp::Fork);
    fork.numChildren = static_cast<std::uint8_t>(std::min<std::size_t>(children.size(), fork.child.size()));
    for (std::size_t c = 0; c < fork.numChildren; ++c)
      fork.child[c] = static_cast<std::uint8_t>(children[c]);
  }
  bool flatten(ProcessingPlan& plan) const {
    std::size_t total = 0;
    for (const auto& ops : seqs)
      total += ops.size();
    if (seqs.size() > static_cast<std::size_t>(ProcessingPlan::kMaxSeqs) ||
        total > static_cast<std::size_t>(ProcessingPlan::kMaxOps))
      return false;
    plan.numOps = 0;
    plan.numSeqs = 0;
    for (const auto& ops : seqs) {
      plan.seqs[static_cast<std::size_t>(plan.numSeqs++)] = {static_cast<std::uint16_t>(plan.numOps),
                                                             static_cast<std::uint16_t>(ops.size())};
      for (const PlanOp& op : ops)
        plan.ops[static_cast<std::size_t>(plan.numOps++)] = op;
    }
    return true;
  }
};

// Grid mode. The rows a split or a feed
// ties together are a group; the root forks over the groups, then sends
// every row that has a device output to its bus, in row order, on the
// calling thread. Inside a group the rows run in feed order, and a split
// forks its two paths:
//
//   RunBlocks(row, 0, s)  Split  Fork{ row s..m | lower row s..m }
//   AlignMix  RunBlocks(row, m, 8)
//
// with the lower path compiled the same way, so a split of the lower row
// nests. Without a rejoin the lower path runs to the end of its row and on
// to its own output or feed.
void compileGridPlan(const PlanInput& input, ProcessingPlan& plan, std::string& whyNot) {
  for (int row = 0; row < kGridRows; ++row) {
    const PlanInput::Row& in = input.rows[static_cast<std::size_t>(row)];
    if (static_cast<int>(in.slots.size()) != kGridCols) {
      whyNot = "row " + std::to_string(row) + " holds " + std::to_string(in.slots.size()) +
               " slots; a grid row holds " + std::to_string(kGridCols);
      return;
    }
  }
  const GridAnalysis an = analyzeGrid(input);
  if (!an.ok()) {
    whyNot = an.error;
    return;
  }
  for (int row = 0; row < kGridRows; ++row) {
    const PlanInput::Row& in = input.rows[static_cast<std::size_t>(row)];
    const GridAnalysis::Row& r = an.rows[static_cast<std::size_t>(row)];
    if (!r.active)
      continue;
    const int lastChannel = in.input.channel + (in.input.stereo ? 1 : 0);
    if (r.source == RowInfo::FromDevice && (in.input.channel < 0 || lastChannel >= kMaxIoChannels)) {
      whyNot = "row " + std::to_string(row) + " names an input channel outside 1 to " +
               std::to_string(kMaxIoChannels);
      return;
    }
    if (r.sink == RowInfo::ToDevice && (in.outputPair < 0 || in.outputPair >= kMaxIoPairs)) {
      whyNot = "row " + std::to_string(row) + " names an output pair outside 1 to " +
               std::to_string(kMaxIoPairs);
      return;
    }
  }

  // Strips, in the order the rows first use them: the first strip is the
  // faceplate's (meters, tuner).
  std::array<int, kGridRows> pairOfRow{};
  std::array<bool, kMaxIoPairs> pairUsed{};
  pairUsed[0] = true;  // the main output strip always exists
  for (int row = 0; row < kGridRows; ++row) {
    const PlanInput::Row& in = input.rows[static_cast<std::size_t>(row)];
    const GridAnalysis::Row& r = an.rows[static_cast<std::size_t>(row)];
    RowInfo& info = plan.rowInfo[static_cast<std::size_t>(row)];
    info.source = r.source;
    info.sink = r.sink;
    info.begin = static_cast<std::int8_t>(r.begin);
    info.end = static_cast<std::int8_t>(r.end);
    info.format = static_cast<std::uint8_t>(r.format);
    info.feedsRow = static_cast<std::int8_t>(r.sink == RowInfo::ToRow ? r.feedsRow : -1);
    if (!r.active)
      continue;
    info.active = true;
    info.group = static_cast<std::int8_t>(r.group);
    info.upstream = static_cast<std::uint8_t>(r.upstream);

    if (r.source == RowInfo::FromDevice) {
      if (!inputChannelPresent(input, in.input.channel)) {
        info.inputMissing = true;
      } else {
        int strip = -1;
        for (int s = 0; s < plan.numStrips; ++s)
          if (sameStripSource(plan.strips[static_cast<std::size_t>(s)].in, in.input))
            strip = s;
        if (strip < 0 && plan.numStrips < kMaxIoPairs) {
          strip = plan.numStrips++;
          ProcessingPlan::Strip& made = plan.strips[static_cast<std::size_t>(strip)];
          made.in = in.input;
          made.in.trimDb = 0.0f;  // the trim is the row's, applied at RowIn
          made.port = static_cast<std::uint8_t>(strip);
        }
        info.strip = static_cast<std::int8_t>(strip);
      }
    }

    if (r.sink == RowInfo::ToDevice) {
      int pair = in.outputPair;
      if (!outputPairPresent(input, pair)) {
        info.outputFallback = pair != 0;
        pair = 0;
      }
      pairOfRow[static_cast<std::size_t>(row)] = pair;
      pairUsed[static_cast<std::size_t>(pair)] = true;
    }
  }
  plan.numGroups = an.numGroups;
  for (int upper = 0; upper < kGridRows; ++upper) {
    const GridAnalysis::Split& split = an.splits[static_cast<std::size_t>(upper)];
    ProcessingPlan::SplitInfo& info = plan.splitInfo[static_cast<std::size_t>(upper)];
    info.present = split.present;
    info.lowerActive = split.lowerActive;
    info.splitCol = static_cast<std::int8_t>(split.splitCol);
    info.mixCol = static_cast<std::int8_t>(split.mixCol);
  }

  // The boundary always has a port each way, so a rig without a live input
  // keeps a strip on the main pair: the faceplate meters and the tuner go on
  // working with an empty grid.
  if (plan.numStrips == 0) {
    ProcessingPlan::Strip& strip = plan.strips[0];
    strip.in.type = RowInput::Type::Device;
    strip.in.channel = 0;
    strip.in.stereo = true;
    strip.in.fold = RowInput::Fold::SumToMono;
    strip.in.gate = true;
    strip.in.pitch = true;
    strip.port = 0;
    plan.numStrips = 1;
  }

  std::array<int, kMaxIoPairs> busOfPair{};
  for (int pair = 0; pair < kMaxIoPairs; ++pair) {
    if (!pairUsed[static_cast<std::size_t>(pair)])
      continue;
    ProcessingPlan::Bus& bus = plan.buses[static_cast<std::size_t>(plan.numBuses)];
    bus.pair = static_cast<std::uint8_t>(pair);
    bus.port = static_cast<std::uint8_t>(plan.numBuses);
    bus.mainStrip = pair == 0;
    busOfPair[static_cast<std::size_t>(pair)] = plan.numBuses++;
  }

  PlanDraft draft;
  const auto formatOf = [&an](int row) { return an.rows[static_cast<std::size_t>(row)].format; };
  const auto bufferOf = [&](int row) { return rowRef(row, rowChannels(formatOf(row))); };

  // A segment of a row. What follows it, for "is there an amp after me":
  // the rest of the row's own range, then the row it rejoins from the mix
  // point on or the row it feeds, and so on down the path. The upper row of
  // a split never looks into the lower one.
  const auto emitRun = [&](int seq, int row, int begin, int end) {
    if (begin >= end)
      return;
    const GridAnalysis::Row& r = an.rows[static_cast<std::size_t>(row)];
    PlanOp& run = draft.add(seq, PlanOp::RunBlocks);
    run.row = static_cast<std::uint8_t>(row);
    run.begin = static_cast<std::int16_t>(begin);
    run.end = static_cast<std::int16_t>(end);
    run.a = bufferOf(row);
    run.format = static_cast<std::uint8_t>(r.format);
    run.scanBegin = static_cast<std::int16_t>(r.begin);
    run.scanEnd = static_cast<std::int16_t>(r.end);
    int at = row;
    while (run.numCont < run.cont.size()) {
      const GridAnalysis::Row& here = an.rows[static_cast<std::size_t>(at)];
      PlanOp::Range next;
      if (here.sink == RowInfo::ToMix) {
        next.row = static_cast<std::uint8_t>(at - 1);
        next.begin = static_cast<std::int16_t>(an.splitAbove(at).mixCol);
        next.end = static_cast<std::int16_t>(an.rows[static_cast<std::size_t>(at - 1)].end);
        at = at - 1;
      } else if (here.sink == RowInfo::ToRow) {
        const GridAnalysis::Row& target = an.rows[static_cast<std::size_t>(here.feedsRow)];
        next.row = static_cast<std::uint8_t>(here.feedsRow);
        next.begin = static_cast<std::int16_t>(target.begin);
        next.end = static_cast<std::int16_t>(target.end);
        at = here.feedsRow;
      } else {
        break;
      }
      run.cont[run.numCont++] = next;
    }
  };

  const auto junctionOp = [&](int seq, PlanOp::Type type, int upper) {
    PlanOp& op = draft.add(seq, type);
    op.row = static_cast<std::uint8_t>(upper);
    op.row2 = static_cast<std::uint8_t>(upper + 1);
    op.a = bufferOf(upper);
    op.b = bufferOf(upper + 1);
    op.junction = static_cast<std::int16_t>(upper);
    op.format = static_cast<std::uint8_t>(formatOf(upper));
    op.format2 = static_cast<std::uint8_t>(formatOf(upper + 1));
  };

  // A row over [from, to), its split (and the splits below it) included.
  const auto emitRow = [&](const auto& self, int seq, int row, int from, int to) -> void {
    const GridAnalysis::Split& split = an.splits[static_cast<std::size_t>(row)];
    if (!split.present) {
      emitRun(seq, row, from, to);
      return;
    }
    const int lower = row + 1;
    emitRun(seq, row, from, split.splitCol);
    // The splitter shapes the upper path too (its level, its band), so it
    // runs whether or not the lower path leads anywhere.
    junctionOp(seq, PlanOp::Split, row);
    if (split.rejoins()) {
      const int upperSeq = draft.newSeq();
      emitRun(upperSeq, row, split.splitCol, split.mixCol);
      const int lowerSeq = draft.newSeq();
      self(self, lowerSeq, lower, split.splitCol, split.mixCol);
      draft.addFork(seq, {upperSeq, lowerSeq});
      junctionOp(seq, PlanOp::AlignMix, row);
      emitRun(seq, row, split.mixCol, to);
      return;
    }
    const bool upperHasRest = split.splitCol < to;
    const bool lowerHasWork = split.lowerActive &&
                              (split.splitCol < kGridCols || an.splits[static_cast<std::size_t>(lower)].present);
    if (upperHasRest && lowerHasWork) {
      const int upperSeq = draft.newSeq();
      emitRun(upperSeq, row, split.splitCol, to);
      const int lowerSeq = draft.newSeq();
      self(self, lowerSeq, lower, split.splitCol, kGridCols);
      draft.addFork(seq, {upperSeq, lowerSeq});
    } else if (upperHasRest) {
      emitRun(seq, row, split.splitCol, to);
    } else if (lowerHasWork) {
      self(self, seq, lower, split.splitCol, kGridCols);
    }
  };

  // A unit is a row that starts a path (a device input, or the rows that
  // feed it) with everything its splits hang below it.
  const auto isUnitTop = [&an](int row) {
    const GridAnalysis::Row& r = an.rows[static_cast<std::size_t>(row)];
    return r.active && (r.source == RowInfo::FromDevice || r.source == RowInfo::FromFeed);
  };
  const auto unitTopOf = [&an](int row) {
    while (an.isLower(row))
      --row;
    return row;
  };
  const auto emitUnit = [&](int seq, int top) {
    const GridAnalysis::Row& r = an.rows[static_cast<std::size_t>(top)];
    const RowInfo& info = plan.rowInfo[static_cast<std::size_t>(top)];
    if (r.source == RowInfo::FromDevice) {
      const int channels = rowChannels(r.format);
      PlanOp& fill = draft.add(seq, PlanOp::RowIn);
      fill.row = static_cast<std::uint8_t>(top);
      fill.a.kind = BufRef::Port;
      fill.a.index = static_cast<std::uint8_t>(info.strip >= 0 ? info.strip : 0);
      fill.a.firstCh = 0;
      fill.a.numCh = static_cast<std::uint8_t>(info.strip >= 0 ? channels : 0);
      fill.b = rowRef(top, channels);
    } else {
      bool first = true;
      for (int feeder = 0; feeder < kGridRows; ++feeder) {
        const GridAnalysis::Row& f = an.rows[static_cast<std::size_t>(feeder)];
        if (!f.active || f.sink != RowInfo::ToRow || f.feedsRow != top)
          continue;
        PlanOp& feed = draft.add(seq, PlanOp::FeedRow);
        feed.row = static_cast<std::uint8_t>(feeder);
        feed.row2 = static_cast<std::uint8_t>(top);
        feed.a = bufferOf(feeder);
        feed.b = bufferOf(top);
        feed.format = static_cast<std::uint8_t>(f.format);
        feed.format2 = static_cast<std::uint8_t>(r.format);
        feed.firstOnBus = first;
        first = false;
      }
    }
    emitRow(emitRow, seq, top, 0, kGridCols);
  };

  // The level of a unit: one more than the latest unit that feeds it. Units
  // of one level do not depend on each other and are forked.
  std::array<int, kGridRows> level{};
  level.fill(-1);
  for (int round = 0; round < kGridRows; ++round)
    for (int top = 0; top < kGridRows; ++top) {
      if (!isUnitTop(top))
        continue;
      int wanted = 0;
      for (int feeder = 0; feeder < kGridRows; ++feeder) {
        const GridAnalysis::Row& f = an.rows[static_cast<std::size_t>(feeder)];
        if (f.active && f.sink == RowInfo::ToRow && f.feedsRow == top)
          wanted = std::max(wanted, level[static_cast<std::size_t>(unitTopOf(feeder))] + 1);
      }
      level[static_cast<std::size_t>(top)] = wanted;
    }

  const auto emitGroup = [&](int seq, int group) {
    for (int at = 0; at < kGridRows; ++at) {
      std::vector<int> units;
      for (int top = 0; top < kGridRows; ++top)
        if (isUnitTop(top) && an.rows[static_cast<std::size_t>(top)].group == group &&
            level[static_cast<std::size_t>(top)] == at)
          units.push_back(top);
      if (units.size() == 1) {
        emitUnit(seq, units[0]);
      } else if (units.size() >= 2) {
        std::vector<int> children;
        for (const int top : units) {
          children.push_back(draft.newSeq());
          emitUnit(children.back(), top);
        }
        draft.addFork(seq, children);
      }
    }
  };

  const int root = draft.newSeq();
  if (an.numGroups == 1) {
    emitGroup(root, 0);
  } else if (an.numGroups >= 2) {
    std::vector<int> children;
    for (int group = 0; group < an.numGroups; ++group) {
      children.push_back(draft.newSeq());
      emitGroup(children.back(), group);
    }
    draft.addFork(root, children);
  }

  // The sends, after the join: one thread, row order, so a bus is summed in
  // the same order whether or not the rows ran side by side.
  std::array<bool, kMaxIoPairs> busWritten{};
  for (int row = 0; row < kGridRows; ++row) {
    RowInfo& info = plan.rowInfo[static_cast<std::size_t>(row)];
    if (!info.active || info.sink != RowInfo::ToDevice)
      continue;
    const int bus = busOfPair[static_cast<std::size_t>(pairOfRow[static_cast<std::size_t>(row)])];
    info.bus = static_cast<std::int8_t>(bus);
    PlanOp& send = draft.add(root, PlanOp::RowOut);
    send.row = static_cast<std::uint8_t>(row);
    send.format = static_cast<std::uint8_t>(formatOf(row));
    send.a = bufferOf(row);
    send.b = busRef(bus);
    send.firstOnBus = !busWritten[static_cast<std::size_t>(bus)];
    busWritten[static_cast<std::size_t>(bus)] = true;
    plan.buses[static_cast<std::size_t>(bus)].hasWriter = true;
  }

  plan.valid = draft.flatten(plan);
  if (!plan.valid)
    whyNot = "the rig needs more operations than a plan holds";
}

// One entry per buffer channel a plan can name.
constexpr int kChannelsPerBuffer = 2;
constexpr int kBuffersPerKind = 4;  // kGridRows rows, kMaxIoPairs buses or ports
constexpr int kNumChannelKeys = 3 * kBuffersPerKind * kChannelsPerBuffer;

int channelKey(const BufRef& ref, int ch) {
  return (static_cast<int>(ref.kind) * kBuffersPerKind + ref.index) * kChannelsPerBuffer +
         ref.firstCh + ch;
}

bool refInRange(const BufRef& ref) {
  return ref.kind <= BufRef::Port && ref.index < kBuffersPerKind && ref.numCh >= 1 &&
         ref.firstCh + ref.numCh <= kChannelsPerBuffer;
}

int segmentLatency(const PlanInput& input, const PlanOp& op) {
  if (op.row >= kGridRows)
    return 0;
  const auto& slots = input.rows[op.row].slots;
  const int last = std::min(static_cast<int>(op.end), static_cast<int>(slots.size()));
  int total = 0;
  for (int i = std::max(0, static_cast<int>(op.begin)); i < last; ++i)
    total += std::max(0, slots[static_cast<std::size_t>(i)].latency);
  return total;
}

// Walks the plan in execution order, carrying how late each buffer channel
// is. With `write` set it stores the segment latencies and the pads;
// without, it checks the stored ones and reports the first join whose sides
// would not arrive together.
struct LatencyWalk {
  ProcessingPlan* writable = nullptr;
  const ProcessingPlan& plan;
  const PlanInput& input;
  std::array<int, kNumChannelKeys> late{};
  std::string error;

  void runSeq(int seqIndex, int depth) {
    if (seqIndex < 0 || seqIndex >= plan.numSeqs || depth > ProcessingPlan::kMaxSeqs)
      return;
    const PlanSeq seq = plan.seqs[static_cast<std::size_t>(seqIndex)];
    const int last = std::min<int>(seq.firstOp + seq.numOps, plan.numOps);
    for (int i = seq.firstOp; i < last; ++i) {
      const PlanOp& op = plan.ops[static_cast<std::size_t>(i)];
      switch (op.type) {
        case PlanOp::RunBlocks: {
          const int latency = segmentLatency(input, op);
          if (writable != nullptr)
            writable->ops[static_cast<std::size_t>(i)].latency = latency;
          else if (op.latency != latency && error.empty())
            error = "segment latency is stale at op " + std::to_string(i);
          if (refInRange(op.a))
            for (int ch = 0; ch < op.a.numCh; ++ch)
              late[static_cast<std::size_t>(channelKey(op.a, ch))] += latency;
          break;
        }
        case PlanOp::Copy:
          if (refInRange(op.a) && refInRange(op.b))
            for (int ch = 0; ch < op.b.numCh; ++ch)
              late[static_cast<std::size_t>(channelKey(op.b, ch))] =
                  late[static_cast<std::size_t>(channelKey(op.a, std::min<int>(ch, op.a.numCh - 1)))];
          break;
        case PlanOp::Fork:
          for (int c = 0; c < op.numChildren; ++c)
            runSeq(op.child[static_cast<std::size_t>(c)], depth + 1);
          break;
        case PlanOp::Delay: {
          // A run of Delay operations is one join: every path in it is
          // padded to the latest.
          int groupEnd = i;
          while (groupEnd < last && plan.ops[static_cast<std::size_t>(groupEnd)].type == PlanOp::Delay)
            ++groupEnd;
          int latest = 0;
          for (int k = i; k < groupEnd; ++k) {
            const PlanOp& d = plan.ops[static_cast<std::size_t>(k)];
            if (refInRange(d.a))
              latest = std::max(latest, late[static_cast<std::size_t>(channelKey(d.a, 0))]);
          }
          for (int k = i; k < groupEnd; ++k) {
            const PlanOp& d = plan.ops[static_cast<std::size_t>(k)];
            if (!refInRange(d.a))
              continue;
            const int arrives = late[static_cast<std::size_t>(channelKey(d.a, 0))];
            if (writable != nullptr)
              writable->ops[static_cast<std::size_t>(k)].latency = latest - arrives;
            else if (arrives + d.latency != latest && error.empty())
              error = "paths meet unequally late at op " + std::to_string(k) + ": " +
                      std::to_string(arrives + d.latency) + " against " + std::to_string(latest);
            for (int ch = 0; ch < d.a.numCh; ++ch)
              late[static_cast<std::size_t>(channelKey(d.a, ch))] = latest;
          }
          i = groupEnd - 1;
          break;
        }
        default:
          break;  // the grid's operations: none is compiled yet
      }
    }
  }

  // What the main pair ends up with: the number the host has to know.
  int reported() const {
    int total = 0;
    for (int ch = 0; ch < kChannelsPerBuffer; ++ch)
      total = std::max(total, late[static_cast<std::size_t>(channelKey(portRef(0, 2), ch))]);
    return total;
  }
};


// The grid's latency pass, over the plan
// in execution order. A row starts at zero from its device input, at its
// upper row's latency from a split, and at the latest of its feeders from a
// feed; its blocks add theirs; a mixer makes both paths as late as the
// later one. The rows that send to one bus wait for the latest of them, as
// do the sends of one group whatever their buses (two outputs of one split
// stay in step) and, with padAcrossBuses, every send of the rig.
//
// With `writable` set the segment latencies and the pads are stored;
// without, the stored ones are checked and the first that is stale or
// leaves two paths apart is reported.
struct GridLatency {
  ProcessingPlan* writable = nullptr;
  const ProcessingPlan& plan;
  const PlanInput& input;
  std::array<int, kGridRows> late{};        // while walking: how late each row's buffer is
  std::array<int, kGridRows> rowLatency{};  // where each row hands its signal on
  std::array<int, kGridRows> rowPad{};
  std::array<int, kGridRows> alignA{};      // by upper row
  std::array<int, kGridRows> alignB{};
  // How late the two paths of each mixer are when they reach it, by upper
  // row.
  std::array<int, kGridRows> mixUpper{};
  std::array<int, kGridRows> mixLower{};
  int reported = 0;
  std::string error;

  GridLatency(ProcessingPlan* writeTo, const ProcessingPlan& p, const PlanInput& in)
      : writable(writeTo), plan(p), input(in) {
    runSeq(0, 0);
    sends();
  }

  void fail(const std::string& message) {
    if (error.empty())
      error = message;
  }

  void store(int opIndex, int latency, int latency2, const char* what) {
    const PlanOp& op = plan.ops[static_cast<std::size_t>(opIndex)];
    if (writable != nullptr) {
      writable->ops[static_cast<std::size_t>(opIndex)].latency = latency;
      writable->ops[static_cast<std::size_t>(opIndex)].latency2 = latency2;
    } else if (op.latency != latency || op.latency2 != latency2) {
      fail(std::string(what) + " at op " + std::to_string(opIndex) + ": " +
           std::to_string(op.latency) + " against " + std::to_string(latency));
    }
  }

  void runSeq(int seqIndex, int depth) {
    if (seqIndex < 0 || seqIndex >= plan.numSeqs || depth > ProcessingPlan::kMaxSeqs)
      return;
    const PlanSeq seq = plan.seqs[static_cast<std::size_t>(seqIndex)];
    const int last = std::min<int>(seq.firstOp + seq.numOps, plan.numOps);
    for (int i = seq.firstOp; i < last; ++i) {
      const PlanOp& op = plan.ops[static_cast<std::size_t>(i)];
      if (op.type == PlanOp::Fork) {
        for (int c = 0; c < op.numChildren && c < static_cast<int>(op.child.size()); ++c)
          runSeq(op.child[static_cast<std::size_t>(c)], depth + 1);
        continue;
      }
      if (op.row >= kGridRows || op.row2 >= kGridRows)
        continue;
      switch (op.type) {
        case PlanOp::RowIn:
          late[op.row] = 0;
          break;
        case PlanOp::RunBlocks: {
          const int latency = segmentLatency(input, op);
          store(i, latency, 0, "segment latency is stale");
          late[op.row] += latency;
          rowLatency[op.row] = late[op.row];
          break;
        }
        case PlanOp::Split:
          late[op.row2] = late[op.row];
          rowLatency[op.row2] = late[op.row2];
          break;
        case PlanOp::AlignMix: {
          const int latest = std::max(late[op.row], late[op.row2]);
          mixUpper[op.row] = late[op.row];
          mixLower[op.row] = late[op.row2];
          rowLatency[op.row2] = late[op.row2];
          rowPad[op.row2] = latest - late[op.row2];
          alignA[op.row] = latest - late[op.row];
          alignB[op.row] = latest - late[op.row2];
          store(i, alignA[op.row], alignB[op.row], "the paths of a split meet unequally late");
          late[op.row] = latest;
          rowLatency[op.row] = latest;
          break;
        }
        case PlanOp::FeedRow: {
          // The feeds into one row stand together: every feeder is padded
          // to the latest.
          int groupEnd = i;
          while (groupEnd < last &&
                 plan.ops[static_cast<std::size_t>(groupEnd)].type == PlanOp::FeedRow &&
                 plan.ops[static_cast<std::size_t>(groupEnd)].row2 == op.row2)
            ++groupEnd;
          int latest = 0;
          for (int k = i; k < groupEnd; ++k)
            if (const int feeder = plan.ops[static_cast<std::size_t>(k)].row; feeder < kGridRows)
              latest = std::max(latest, late[static_cast<std::size_t>(feeder)]);
          for (int k = i; k < groupEnd; ++k) {
            const int feeder = plan.ops[static_cast<std::size_t>(k)].row;
            if (feeder >= kGridRows)
              continue;
            rowLatency[static_cast<std::size_t>(feeder)] = late[static_cast<std::size_t>(feeder)];
            rowPad[static_cast<std::size_t>(feeder)] = latest - late[static_cast<std::size_t>(feeder)];
            store(k, rowPad[static_cast<std::size_t>(feeder)], 0, "feeds meet unequally late");
          }
          late[op.row2] = latest;
          rowLatency[op.row2] = latest;
          i = groupEnd - 1;
          break;
        }
        default:
          break;
      }
    }
  }

  // The sends of the root sequence: which of them have to arrive together,
  // and what each waits.
  void sends() {
    std::array<int, kGridRows> parent{};
    std::array<bool, kGridRows> sending{};
    for (int row = 0; row < kGridRows; ++row)
      parent[static_cast<std::size_t>(row)] = row;
    const auto find = [&parent](int row) {
      while (parent[static_cast<std::size_t>(row)] != row)
        row = parent[static_cast<std::size_t>(row)];
      return row;
    };
    for (int row = 0; row < kGridRows; ++row) {
      const ProcessingPlan::RowInfo& info = plan.rowInfo[static_cast<std::size_t>(row)];
      sending[static_cast<std::size_t>(row)] = info.active && info.sink == RowInfo::ToDevice &&
                                               info.bus >= 0 && info.bus < kMaxIoPairs;
    }
    for (int a = 0; a < kGridRows; ++a)
      for (int b = a + 1; b < kGridRows; ++b) {
        if (!sending[static_cast<std::size_t>(a)] || !sending[static_cast<std::size_t>(b)])
          continue;
        const ProcessingPlan::RowInfo& ia = plan.rowInfo[static_cast<std::size_t>(a)];
        const ProcessingPlan::RowInfo& ib = plan.rowInfo[static_cast<std::size_t>(b)];
        if (input.io.padAcrossBuses || ia.bus == ib.bus || ia.group == ib.group)
          parent[static_cast<std::size_t>(find(b))] = find(a);
      }
    std::array<int, kGridRows> latestOf{};
    for (int row = 0; row < kGridRows; ++row)
      if (sending[static_cast<std::size_t>(row)]) {
        int& latest = latestOf[static_cast<std::size_t>(find(row))];
        latest = std::max(latest, late[static_cast<std::size_t>(row)]);
        reported = std::max(reported, late[static_cast<std::size_t>(row)]);
      }
    for (int row = 0; row < kGridRows; ++row)
      if (sending[static_cast<std::size_t>(row)]) {
        rowLatency[static_cast<std::size_t>(row)] = late[static_cast<std::size_t>(row)];
        rowPad[static_cast<std::size_t>(row)] =
            latestOf[static_cast<std::size_t>(find(row))] - late[static_cast<std::size_t>(row)];
      }
    for (int i = 0; i < plan.numOps; ++i) {
      const PlanOp& op = plan.ops[static_cast<std::size_t>(i)];
      if (op.type != PlanOp::RowOut || op.row >= kGridRows)
        continue;
      if (writable != nullptr)
        writable->ops[static_cast<std::size_t>(i)].latency = rowPad[op.row];
      else if (op.latency != rowPad[op.row])
        fail("row " + std::to_string(op.row) + " has a stale pad");
    }
  }
};

const char* formatName(int format) {
  switch (static_cast<RowFormat>(format)) {
    case RowFormat::Mono: return "mono";
    case RowFormat::DualMono: return "dual mono";
    case RowFormat::Stereo: break;
  }
  return "stereo";
}

std::string describeRef(const BufRef& ref) {
  static const char* const kinds[] = {"row", "bus", "port"};
  std::string text = std::string(ref.kind <= BufRef::Port ? kinds[ref.kind] : "?") +
                     std::to_string(ref.index) + " ch" + std::to_string(ref.firstCh);
  if (ref.numCh > 1)
    text += "-" + std::to_string(ref.firstCh + ref.numCh - 1);
  if (ref.followHostChannels)
    text += " (as many as the host buffer has)";
  return text;
}

void describeSeq(const ProcessingPlan& plan, int seqIndex, int indent, std::ostringstream& out) {
  if (seqIndex < 0 || seqIndex >= plan.numSeqs || indent > ProcessingPlan::kMaxSeqs)
    return;
  const PlanSeq seq = plan.seqs[static_cast<std::size_t>(seqIndex)];
  const std::string pad(static_cast<std::size_t>(indent) * 2, ' ');
  const int last = std::min<int>(seq.firstOp + seq.numOps, plan.numOps);
  for (int i = seq.firstOp; i < last; ++i) {
    const PlanOp& op = plan.ops[static_cast<std::size_t>(i)];
    out << pad;
    switch (op.type) {
      case PlanOp::RunBlocks:
        out << "RunBlocks row " << int(op.row) << " [" << op.begin << ", " << op.end << ") on "
            << describeRef(op.a) << ", " << formatName(op.format)
            << (op.followRigDualMono ? ", dual mono as the rig allows" : "") << ", scan ["
            << op.scanBegin << ", " << op.scanEnd << ")";
        for (int c = 0; c < op.numCont && c < static_cast<int>(op.cont.size()); ++c)
          out << " then row " << int(op.cont[static_cast<std::size_t>(c)].row) << " ["
              << op.cont[static_cast<std::size_t>(c)].begin << ", "
              << op.cont[static_cast<std::size_t>(c)].end << ")";
        out << ", latency " << op.latency << "\n";
        break;
      case PlanOp::Copy:
        out << "Copy " << describeRef(op.a) << " to " << describeRef(op.b) << "\n";
        break;
      case PlanOp::Delay:
        out << "Delay row " << int(op.row) << " on " << describeRef(op.a) << " by " << op.latency
            << "\n";
        break;
      case PlanOp::Fork:
        out << "Fork\n";
        for (int c = 0; c < op.numChildren; ++c) {
          out << pad << "  child " << c << (c == 0 ? " (calling thread):\n" : ":\n");
          describeSeq(plan, op.child[static_cast<std::size_t>(c)], indent + 2, out);
        }
        break;
      case PlanOp::RowIn:
        out << "RowIn row " << int(op.row) << " from ";
        if (op.a.numCh == 0)
          out << "silence (its input channel is missing)";
        else
          out << "strip " << describeRef(op.a);
        out << " to " << describeRef(op.b) << "\n";
        break;
      case PlanOp::RowOut:
        out << "RowOut row " << int(op.row) << " (" << describeRef(op.a) << ", "
            << formatName(op.format) << ") to " << describeRef(op.b) << ", pad " << op.latency
            << (op.firstOnBus ? ", first on its bus" : ", added") << "\n";
        break;
      case PlanOp::Split:
        out << "Split row " << int(op.row) << " (" << describeRef(op.a) << ", "
            << formatName(op.format) << ") into row " << int(op.row2) << " (" << describeRef(op.b)
            << ", " << formatName(op.format2) << ")\n";
        break;
      case PlanOp::AlignMix:
        out << "AlignMix row " << int(op.row2) << " back into row " << int(op.row) << ", delay A "
            << op.latency << ", delay B " << op.latency2 << "\n";
        break;
      case PlanOp::FeedRow:
        out << "FeedRow row " << int(op.row) << " (" << describeRef(op.a) << ", "
            << formatName(op.format) << ") into row " << int(op.row2) << " (" << describeRef(op.b)
            << ", " << formatName(op.format2) << "), pad " << op.latency
            << (op.firstOnBus ? ", first into it" : ", added") << "\n";
        break;
      default:
        out << "(unknown op type " << int(op.type) << ")\n";
        break;
    }
  }
}

// validatePlan's structural walk: slot coverage in order, each sequence
// reached once, and the set of buffer channels each sequence writes.
struct StructureWalk {
  const ProcessingPlan& plan;
  const PlanInput& input;
  std::array<int, kGridRows> nextSlot{};
  std::array<int, ProcessingPlan::kMaxSeqs> visits{};
  std::string error;
  // Grid: how often each row is filled and sent, and which buses were
  // written.
  std::array<int, kGridRows> fills{};
  std::array<int, kGridRows> sends{};
  std::array<bool, kMaxIoPairs> busWritten{};
  // Grid: the routing the plan has to match (null for a legacy plan), how
  // often each split forks and mixes (by upper row) and how many rows were
  // fed into each row.
  const GridAnalysis* an = nullptr;
  std::array<int, kGridRows> splitRuns{};
  std::array<int, kGridRows> mixRuns{};
  std::array<int, kGridRows> feedsIn{};

  bool rowBuffer(const BufRef& ref, int row) const {
    return ref.kind == BufRef::Row && ref.index == row;
  }

  void fail(const std::string& message) {
    if (error.empty())
      error = message;
  }

  // Returns the bit set of channel keys the sequence writes.
  std::uint32_t runSeq(int seqIndex, int depth) {
    if (seqIndex < 0 || seqIndex >= plan.numSeqs) {
      fail("sequence index out of range: " + std::to_string(seqIndex));
      return 0;
    }
    if (depth > ProcessingPlan::kMaxSeqs || ++visits[static_cast<std::size_t>(seqIndex)] > 1) {
      fail("sequence " + std::to_string(seqIndex) + " is reached more than once");
      return 0;
    }
    const PlanSeq seq = plan.seqs[static_cast<std::size_t>(seqIndex)];
    if (seq.firstOp + seq.numOps > plan.numOps) {
      fail("sequence " + std::to_string(seqIndex) + " runs past the last operation");
      return 0;
    }

    std::uint32_t written = 0;
    const auto bits = [&](const BufRef& ref, int opIndex) -> std::uint32_t {
      if (!refInRange(ref)) {
        fail("buffer reference out of range at op " + std::to_string(opIndex));
        return 0;
      }
      std::uint32_t mask = 0;
      for (int ch = 0; ch < ref.numCh; ++ch)
        mask |= 1u << channelKey(ref, ch);
      return mask;
    };

    for (int i = seq.firstOp; i < seq.firstOp + seq.numOps; ++i) {
      const PlanOp& op = plan.ops[static_cast<std::size_t>(i)];
      switch (op.type) {
        case PlanOp::RunBlocks: {
          if (op.row >= kGridRows) {
            fail("row out of range at op " + std::to_string(i));
            break;
          }
          const int slots = static_cast<int>(input.rows[op.row].slots.size());
          if (op.begin < 0 || op.begin > op.end || op.end > slots)
            fail("block range out of bounds at op " + std::to_string(i));
          // In order and without gaps: a segment starts where the one
          // before it on the same row ended.
          if (!plan.legacy && fills[op.row] != 1)
            fail("row " + std::to_string(op.row) + " runs before it is filled, at op " +
                 std::to_string(i));
          if (!plan.legacy && sends[op.row] != 0)
            fail("row " + std::to_string(op.row) + " runs after it was sent, at op " +
                 std::to_string(i));
          if (op.begin != nextSlot[op.row])
            fail("row " + std::to_string(op.row) + " runs out of order at op " +
                 std::to_string(i) + ": expected slot " + std::to_string(nextSlot[op.row]) +
                 ", got " + std::to_string(op.begin));
          nextSlot[op.row] = op.end;
          if (op.numCont > op.cont.size())
            fail("too many continuation ranges at op " + std::to_string(i));
          written |= bits(op.a, i);
          break;
        }
        case PlanOp::Copy:
          bits(op.a, i);
          written |= bits(op.b, i);
          break;
        case PlanOp::Delay:
          if (op.row >= kGridRows)
            fail("row out of range at op " + std::to_string(i));
          if (op.latency < 0)
            fail("negative pad at op " + std::to_string(i));
          written |= bits(op.a, i);
          break;
        case PlanOp::Fork: {
          if (op.numChildren < 2 || op.numChildren > op.child.size()) {
            fail("a fork needs two to four children, at op " + std::to_string(i));
            break;
          }
          std::uint32_t seen = 0;
          for (int c = 0; c < op.numChildren; ++c) {
            const int childSeq = op.child[static_cast<std::size_t>(c)];
            if (childSeq <= seqIndex) {
              fail("fork child does not come after its parent at op " + std::to_string(i));
              continue;
            }
            const std::uint32_t childWrites = runSeq(childSeq, depth + 1);
            if ((seen & childWrites) != 0)
              fail("fork children write the same buffer at op " + std::to_string(i));
            seen |= childWrites;
          }
          written |= seen;
          break;
        }
        case PlanOp::RowIn: {
          if (plan.legacy || op.row >= kGridRows) {
            fail("misplaced RowIn at op " + std::to_string(i));
            break;
          }
          if (++fills[op.row] != 1 || nextSlot[op.row] != 0)
            fail("row " + std::to_string(op.row) + " is filled twice or late, at op " +
                 std::to_string(i));
          if (an != nullptr && an->rows[op.row].source != RowInfo::FromDevice)
            fail("row " + std::to_string(op.row) + " is filled from a strip and has no device input, at op " +
                 std::to_string(i));
          // The source is a strip's port, or nothing when the channel is
          // missing; the row reads it and never writes it.
          if (op.a.numCh != 0 &&
              (op.a.kind != BufRef::Port || op.a.index >= plan.numStrips || !refInRange(op.a)))
            fail("RowIn reads no strip at op " + std::to_string(i));
          if (op.b.kind != BufRef::Row || op.b.index != op.row)
            fail("RowIn fills another row's buffer at op " + std::to_string(i));
          written |= bits(op.b, i);
          break;
        }
        case PlanOp::RowOut: {
          if (plan.legacy || op.row >= kGridRows) {
            fail("misplaced RowOut at op " + std::to_string(i));
            break;
          }
          if (seqIndex != 0)
            fail("RowOut outside the root sequence at op " + std::to_string(i));
          if (++sends[op.row] != 1)
            fail("row " + std::to_string(op.row) + " is sent twice, at op " + std::to_string(i));
          if (nextSlot[op.row] != (an != nullptr ? an->rows[op.row].end
                                                 : static_cast<int>(input.rows[op.row].slots.size())))
            fail("row " + std::to_string(op.row) + " is sent before it has run, at op " +
                 std::to_string(i));
          if (an != nullptr && an->rows[op.row].sink != RowInfo::ToDevice)
            fail("row " + std::to_string(op.row) + " is sent to a bus and has no device output, at op " +
                 std::to_string(i));
          if (op.a.kind != BufRef::Row || op.a.index != op.row)
            fail("RowOut sends another row's buffer at op " + std::to_string(i));
          if (op.b.kind != BufRef::Bus || op.b.index >= plan.numBuses) {
            fail("RowOut names no bus at op " + std::to_string(i));
            break;
          }
          if (op.firstOnBus == busWritten[op.b.index])
            fail("bus " + std::to_string(op.b.index) + " is not started exactly once, at op " +
                 std::to_string(i));
          busWritten[op.b.index] = true;
          if (op.latency < 0)
            fail("negative pad at op " + std::to_string(i));
          bits(op.a, i);
          written |= bits(op.b, i);
          break;
        }
        case PlanOp::Split:
        case PlanOp::AlignMix: {
          const bool isSplit = op.type == PlanOp::Split;
          const char* name = isSplit ? "Split" : "AlignMix";
          if (an == nullptr || op.row >= kGridRows - 1 || op.row2 != op.row + 1) {
            fail(std::string("misplaced ") + name + " at op " + std::to_string(i));
            break;
          }
          const int upper = op.row, lower = op.row2;
          const GridAnalysis::Split& split = an->splits[static_cast<std::size_t>(upper)];
          if (!split.present || !an->rows[static_cast<std::size_t>(upper)].active ||
              (!isSplit && !split.rejoins())) {
            fail(std::string(name) + " for a split the rig does not have, at op " + std::to_string(i));
            break;
          }
          if (op.junction != upper || !rowBuffer(op.a, upper) || !rowBuffer(op.b, lower))
            fail(std::string(name) + " names the wrong junction or buffers at op " + std::to_string(i));
          const int at = isSplit ? split.splitCol : split.mixCol;
          if (fills[static_cast<std::size_t>(upper)] != 1 || sends[static_cast<std::size_t>(upper)] != 0 ||
              nextSlot[static_cast<std::size_t>(upper)] != at)
            fail(std::string(name) + " of row " + std::to_string(upper) + " is not at boundary " +
                 std::to_string(at) + ", at op " + std::to_string(i));
          if (isSplit) {
            if (++splitRuns[static_cast<std::size_t>(upper)] != 1)
              fail("row " + std::to_string(upper) + " is split twice, at op " + std::to_string(i));
            // The fork is what fills the lower row.
            if (an->rows[static_cast<std::size_t>(lower)].active &&
                ++fills[static_cast<std::size_t>(lower)] != 1)
              fail("row " + std::to_string(lower) + " is filled twice, at op " + std::to_string(i));
          } else {
            if (splitRuns[static_cast<std::size_t>(upper)] != 1 ||
                ++mixRuns[static_cast<std::size_t>(upper)] != 1)
              fail("row " + std::to_string(upper) + " is mixed before it is split, or twice, at op " +
                   std::to_string(i));
            if (nextSlot[static_cast<std::size_t>(lower)] != split.mixCol ||
                ++sends[static_cast<std::size_t>(lower)] != 1)
              fail("row " + std::to_string(lower) + " reaches its mixer before it has run, or twice, at op " +
                   std::to_string(i));
            if (op.latency < 0 || op.latency2 < 0)
              fail("negative alignment at op " + std::to_string(i));
          }
          written |= bits(op.a, i);
          written |= bits(op.b, i);
          break;
        }
        case PlanOp::FeedRow: {
          if (an == nullptr || op.row >= kGridRows || op.row2 >= kGridRows) {
            fail("misplaced FeedRow at op " + std::to_string(i));
            break;
          }
          const int feeder = op.row, target = op.row2;
          const GridAnalysis::Row& from = an->rows[static_cast<std::size_t>(feeder)];
          if (!from.active || from.sink != RowInfo::ToRow || from.feedsRow != target) {
            fail("row " + std::to_string(feeder) + " is fed into a row it does not feed, at op " +
                 std::to_string(i));
            break;
          }
          if (!rowBuffer(op.a, feeder) || !rowBuffer(op.b, target))
            fail("FeedRow names the wrong buffers at op " + std::to_string(i));
          if (nextSlot[static_cast<std::size_t>(feeder)] != from.end ||
              ++sends[static_cast<std::size_t>(feeder)] != 1)
            fail("row " + std::to_string(feeder) + " is fed on before it has run, or twice, at op " +
                 std::to_string(i));
          // The first feed starts the target's buffer, the others add; all
          // of them come before the target runs.
          const bool first = feedsIn[static_cast<std::size_t>(target)]++ == 0;
          if (op.firstOnBus != first)
            fail("row " + std::to_string(target) + " is not started by its first feed, at op " +
                 std::to_string(i));
          if (first)
            ++fills[static_cast<std::size_t>(target)];
          if (nextSlot[static_cast<std::size_t>(target)] !=
              an->rows[static_cast<std::size_t>(target)].begin)
            fail("row " + std::to_string(target) + " is fed after it has run, at op " +
                 std::to_string(i));
          if (op.latency < 0)
            fail("negative pad at op " + std::to_string(i));
          written |= bits(op.a, i);
          written |= bits(op.b, i);
          break;
        }
        default:
          fail("unknown operation type " + std::to_string(op.type) + " at op " + std::to_string(i));
          break;
      }
    }
    return written;
  }
};

// The cost of one sequence: everything in it, and the most that one thread
// has to carry when its forks run side by side.
struct SeqCost {
  float total = 0.0f;
  float critical = 0.0f;
};

SeqCost seqCost(const ProcessingPlan& plan, const PlanInput& input, int seqIndex, int depth,
                std::array<float, kGridRows>& rows) {
  SeqCost cost;
  if (seqIndex < 0 || seqIndex >= plan.numSeqs || depth > ProcessingPlan::kMaxSeqs)
    return cost;
  const PlanSeq seq = plan.seqs[static_cast<std::size_t>(seqIndex)];
  const int last = std::min<int>(seq.firstOp + seq.numOps, plan.numOps);
  for (int i = seq.firstOp; i < last; ++i) {
    const PlanOp& op = plan.ops[static_cast<std::size_t>(i)];
    if (op.type == PlanOp::RunBlocks && op.row < kGridRows) {
      const auto& slots = input.rows[op.row].slots;
      const int end = std::min(static_cast<int>(op.end), static_cast<int>(slots.size()));
      float segment = 0.0f;
      for (int k = std::max(0, static_cast<int>(op.begin)); k < end; ++k)
        if (slots[static_cast<std::size_t>(k)].isBlock)
          segment += std::max(0.0f, slots[static_cast<std::size_t>(k)].costEstimate);
      cost.total += segment;
      cost.critical += segment;
      rows[op.row] += segment;
    } else if (op.type == PlanOp::Fork) {
      float widest = 0.0f;
      for (int c = 0; c < op.numChildren && c < static_cast<int>(op.child.size()); ++c) {
        const SeqCost child =
            seqCost(plan, input, op.child[static_cast<std::size_t>(c)], depth + 1, rows);
        cost.total += child.total;
        widest = std::max(widest, child.critical);
      }
      cost.critical += widest;
    }
  }
  return cost;
}

}  // namespace

ProcessingPlan compilePlan(const PlanInput& input, std::string* whyNot) {
  ProcessingPlan plan;
  std::string reason;
  plan.topologyRevision = input.topologyRevision;
  plan.legacy = input.mode == RoutingMode::Legacy;
  for (int row = 0; row < kGridRows; ++row)
    plan.rowSlots[static_cast<std::size_t>(row)] = static_cast<std::int16_t>(
        std::min<std::size_t>(input.rows[static_cast<std::size_t>(row)].slots.size(), INT16_MAX));

  if (input.mode == RoutingMode::Legacy)
    compileLegacyPlan(input, plan);
  else
    compileGridPlan(input, plan, reason);

  if (plan.valid) {
    computePlanLatency(plan, input);
    const PlanCpu cpu = computePlanCpu(plan, input);
    plan.cpuEstimate = cpu.total;
    plan.cpuCritical = cpu.critical;
  } else if (whyNot != nullptr) {
    *whyNot = reason.empty() ? std::string("the rig does not compile") : reason;
  }
  return plan;
}

PlanCpu computePlanCpu(const ProcessingPlan& plan, const PlanInput& input) {
  PlanCpu cpu;
  const SeqCost cost = seqCost(plan, input, 0, 0, cpu.rows);
  // Strips and buses run one after the other on the callback's own thread.
  const float ports =
      static_cast<float>(plan.numStrips + plan.numBuses) * std::max(0.0f, input.io.portCost);
  cpu.total = cost.total + ports;
  cpu.critical = cost.critical + ports;
  return cpu;
}

void computePlanLatency(ProcessingPlan& plan, const PlanInput& input) {
  if (!plan.legacy) {
    const GridLatency latency(&plan, plan, input);
    for (int row = 0; row < kGridRows; ++row) {
      plan.rowInfo[static_cast<std::size_t>(row)].latency =
          latency.rowLatency[static_cast<std::size_t>(row)];
      plan.rowInfo[static_cast<std::size_t>(row)].pad = latency.rowPad[static_cast<std::size_t>(row)];
      plan.splitInfo[static_cast<std::size_t>(row)].alignA =
          latency.alignA[static_cast<std::size_t>(row)];
      plan.splitInfo[static_cast<std::size_t>(row)].alignB =
          latency.alignB[static_cast<std::size_t>(row)];
    }
    plan.reportedChainLatency = latency.reported;
    return;
  }
  LatencyWalk walk{&plan, plan, input, {}, {}};
  walk.runSeq(0, 0);
  plan.reportedChainLatency = walk.reported();
}

std::string validateGridRouting(const PlanInput& input, int* badSplitRow, int* badFeedRow) {
  const GridAnalysis an = analyzeGrid(input);
  if (badSplitRow != nullptr)
    *badSplitRow = an.badSplit;
  if (badFeedRow != nullptr)
    *badFeedRow = an.badFeed;
  return an.error;
}

RowFormat effectiveRowFormat(const PlanInput& input, int row) {
  row = std::max(0, std::min(kGridRows - 1, row));
  // Down from the first row of the stack of splits this row hangs in.
  int top = row;
  const auto hasSplit = [&input](int upper) {
    for (int i = 0; i < input.numSplits && i < kGridRows; ++i)
      if (input.splits[static_cast<std::size_t>(i)].upperRow == upper)
        return true;
    return false;
  };
  while (top > 0 && hasSplit(top - 1))
    --top;
  for (int r = top; r <= row; ++r)
    if (input.rows[static_cast<std::size_t>(r)].format == RowFormat::DualMono)
      return RowFormat::DualMono;
  return input.rows[static_cast<std::size_t>(row)].format;
}

std::string validatePlan(const ProcessingPlan& plan, const PlanInput& input) {
  if (!plan.valid)
    return "the plan did not compile";
  if (plan.numOps < 0 || plan.numOps > ProcessingPlan::kMaxOps || plan.numSeqs < 1 ||
      plan.numSeqs > ProcessingPlan::kMaxSeqs)
    return "operation or sequence count out of range";
  if (plan.topologyRevision != input.topologyRevision)
    return "the plan belongs to another topology revision";

  // Strips and buses: at least the main pair, and on each side the ports
  // numbered 0 to count - 1 (the boundary is opened with exactly that many).
  if (plan.numStrips < 1 || plan.numStrips > kMaxIoPairs || plan.numBuses < 1 ||
      plan.numBuses > kMaxIoPairs)
    return "strip or bus count out of range";
  {
    unsigned stripPorts = 0, busPorts = 0;
    int mainStrips = 0;
    for (int s = 0; s < plan.numStrips; ++s)
      stripPorts |= 1u << std::min<int>(plan.strips[static_cast<std::size_t>(s)].port, 31);
    for (int b = 0; b < plan.numBuses; ++b) {
      busPorts |= 1u << std::min<int>(plan.buses[static_cast<std::size_t>(b)].port, 31);
      mainStrips += plan.buses[static_cast<std::size_t>(b)].mainStrip ? 1 : 0;
    }
    if (stripPorts != (1u << plan.numStrips) - 1u)
      return "the input strips do not use ports 0 to count - 1 once each";
    if (busPorts != (1u << plan.numBuses) - 1u)
      return "the output buses do not use ports 0 to count - 1 once each";
    if (mainStrips != 1)
      return "a plan needs exactly one main output strip";
  }

  // The routing the plan was compiled from has to hold still, and the plan
  // is checked against the same reading of it the compiler used.
  GridAnalysis an;
  if (!plan.legacy) {
    for (int row = 0; row < kGridRows; ++row)
      if (static_cast<int>(input.rows[static_cast<std::size_t>(row)].slots.size()) != kGridCols)
        return "row " + std::to_string(row) + " changed length since the plan was compiled";
    an = analyzeGrid(input);
    if (!an.ok())
      return "the rig's routing does not hold: " + an.error;
  }
  const auto rowIsActive = [&](int row) {
    if (!plan.legacy)
      return an.rows[static_cast<std::size_t>(row)].active;
    // Legacy: the left lane always, the right lane in stereo mode, rows 2
    // and 3 never.
    return row == 0 || (row == 1 && input.legacy.stereoEnabled);
  };

  StructureWalk structure{plan, input, {}, {}, {}, {}, {}, {}, plan.legacy ? nullptr : &an, {}, {}, {}};
  if (!plan.legacy)
    for (int row = 0; row < kGridRows; ++row)
      structure.nextSlot[static_cast<std::size_t>(row)] = an.rows[static_cast<std::size_t>(row)].begin;
  structure.runSeq(0, 0);
  if (!structure.error.empty())
    return structure.error;

  for (int s = 0; s < plan.numSeqs; ++s)
    if (structure.visits[static_cast<std::size_t>(s)] != 1)
      return "sequence " + std::to_string(s) + " is never reached";

  int opsInSeqs = 0;
  for (int s = 0; s < plan.numSeqs; ++s)
    opsInSeqs += plan.seqs[static_cast<std::size_t>(s)].numOps;
  if (opsInSeqs != plan.numOps)
    return "an operation belongs to no sequence";

  // Every slot of an active row's range exactly once (the ordered walk
  // above already rules out overlaps and gaps), no slot of any other row.
  for (int row = 0; row < kGridRows; ++row) {
    const int slots = static_cast<int>(input.rows[static_cast<std::size_t>(row)].slots.size());
    const int first = plan.legacy ? 0 : an.rows[static_cast<std::size_t>(row)].begin;
    const int last = plan.legacy ? slots : an.rows[static_cast<std::size_t>(row)].end;
    const int covered = structure.nextSlot[static_cast<std::size_t>(row)];
    if (plan.rowSlots[static_cast<std::size_t>(row)] != slots)
      return "row " + std::to_string(row) + " changed length since the plan was compiled";
    if (rowIsActive(row) && covered != last)
      return "row " + std::to_string(row) + " runs " + std::to_string(covered - first) + " of " +
             std::to_string(last - first) + " slots";
    if (!rowIsActive(row) && covered != first)
      return "inactive row " + std::to_string(row) + " is run";
  }

  if (!plan.legacy) {
    // Every active row is filled once and handed on once, as the routing
    // says, and no other row is touched.
    for (int row = 0; row < kGridRows; ++row) {
      const ProcessingPlan::RowInfo& info = plan.rowInfo[static_cast<std::size_t>(row)];
      const GridAnalysis::Row& r = an.rows[static_cast<std::size_t>(row)];
      if (info.active != r.active)
        return "row " + std::to_string(row) + " is active in the plan and not in the rig, or the reverse";
      const int wanted = r.active ? 1 : 0;
      if (structure.fills[static_cast<std::size_t>(row)] != wanted ||
          structure.sends[static_cast<std::size_t>(row)] != wanted)
        return "row " + std::to_string(row) + " is not filled and sent exactly once";
      if (info.source != r.source || info.sink != r.sink || info.begin != r.begin ||
          info.end != r.end || info.format != static_cast<std::uint8_t>(r.format))
        return "row " + std::to_string(row) + " is described otherwise than the rig routes it";
      if (!r.active)
        continue;
      if (info.group != r.group || info.upstream != r.upstream)
        return "row " + std::to_string(row) + " is in another group than the rig puts it in";
      if (info.strip >= plan.numStrips)
        return "row " + std::to_string(row) + " names a strip or bus the plan does not have";
      if (r.source == RowInfo::FromDevice && info.inputMissing != (info.strip < 0))
        return "row " + std::to_string(row) + " has a strip and a missing input, or neither";
      if ((r.sink == RowInfo::ToDevice) != (info.bus >= 0) || info.bus >= plan.numBuses)
        return "row " + std::to_string(row) + " names a strip or bus the plan does not have";
      int feeders = 0;
      for (int feeder = 0; feeder < kGridRows; ++feeder) {
        const GridAnalysis::Row& f = an.rows[static_cast<std::size_t>(feeder)];
        feeders += f.active && f.sink == RowInfo::ToRow && f.feedsRow == row ? 1 : 0;
      }
      if (structure.feedsIn[static_cast<std::size_t>(row)] != feeders)
        return "row " + std::to_string(row) + " is not fed by every row that feeds it";
    }
    for (int upper = 0; upper < kGridRows; ++upper) {
      const GridAnalysis::Split& split = an.splits[static_cast<std::size_t>(upper)];
      const ProcessingPlan::SplitInfo& info = plan.splitInfo[static_cast<std::size_t>(upper)];
      const bool runs = split.present && an.rows[static_cast<std::size_t>(upper)].active;
      if (structure.splitRuns[static_cast<std::size_t>(upper)] != (runs ? 1 : 0) ||
          structure.mixRuns[static_cast<std::size_t>(upper)] != (runs && split.rejoins() ? 1 : 0))
        return "the split of row " + std::to_string(upper) + " is not run exactly once";
      if (info.present != split.present || info.lowerActive != split.lowerActive ||
          (split.present && (info.splitCol != split.splitCol || info.mixCol != split.mixCol)))
        return "the split of row " + std::to_string(upper) + " is described otherwise than the rig has it";
    }
    if (an.numGroups != plan.numGroups)
      return "group count is stale";
    for (int bus = 0; bus < plan.numBuses; ++bus)
      if (plan.buses[static_cast<std::size_t>(bus)].hasWriter !=
          structure.busWritten[static_cast<std::size_t>(bus)])
        return "bus " + std::to_string(bus) + " is marked written and is not, or the reverse";

    // The latency pass again, in checking mode: stored segment latencies,
    // alignments and pads are the ones the rig asks for now.
    const GridLatency latency(nullptr, plan, input);
    if (!latency.error.empty())
      return latency.error;
    // And what they are for, checked on its own: the two paths of a mixer,
    // the rows fed into one row, the rows of a bus (of every bus, with
    // padAcrossBuses) and the sends of one group arrive together.
    std::array<int, kMaxIoPairs> arrival{};
    arrival.fill(-1);
    std::array<int, kGridRows> groupArrival{};
    groupArrival.fill(-1);
    std::array<int, kGridRows> feedArrival{};
    feedArrival.fill(-1);
    int anyArrival = -1;
    for (int i = 0; i < plan.numOps; ++i) {
      const PlanOp& op = plan.ops[static_cast<std::size_t>(i)];
      if (op.row >= kGridRows || op.row2 >= kGridRows)
        continue;
      if (op.type == PlanOp::AlignMix &&
          std::min(op.latency, op.latency2) != 0)
        return "both paths of the split of row " + std::to_string(op.row) + " wait";
      if (op.type == PlanOp::AlignMix &&
          latency.mixLower[op.row] + op.latency2 != latency.mixUpper[op.row] + op.latency)
        return "the paths of the split of row " + std::to_string(op.row) + " meet unequally late";
      if (op.type == PlanOp::FeedRow) {
        const int arrives = latency.rowLatency[op.row] + op.latency;
        int& fed = feedArrival[op.row2];
        if (fed >= 0 && fed != arrives)
          return "rows meet unequally late in row " + std::to_string(op.row2);
        fed = arrives;
      }
      if (op.type != PlanOp::RowOut)
        continue;
      const int arrives = latency.rowLatency[op.row] + op.latency;
      int& busArrival = arrival[op.b.index];
      if (busArrival >= 0 && busArrival != arrives)
        return "rows meet unequally late on bus " + std::to_string(op.b.index) + ": " +
               std::to_string(arrives) + " against " + std::to_string(busArrival);
      busArrival = arrives;
      const int group = plan.rowInfo[op.row].group;
      if (group >= 0 && group < kGridRows) {
        int& together = groupArrival[static_cast<std::size_t>(group)];
        if (together >= 0 && together != arrives)
          return "the outputs of one group are unequally late";
        together = arrives;
      }
      if (input.io.padAcrossBuses && anyArrival >= 0 && anyArrival != arrives)
        return "buses are unequally late with cross-bus padding";
      anyArrival = arrives;
    }
    for (int row = 0; row < kGridRows; ++row)
      if (plan.rowInfo[static_cast<std::size_t>(row)].latency !=
              latency.rowLatency[static_cast<std::size_t>(row)] ||
          plan.rowInfo[static_cast<std::size_t>(row)].pad != latency.rowPad[static_cast<std::size_t>(row)])
        return "row " + std::to_string(row) + " reports a stale latency";
    if (latency.reported != plan.reportedChainLatency)
      return "reported latency is stale";
    return {};
  }

  LatencyWalk latency{nullptr, plan, input, {}, {}};
  latency.runSeq(0, 0);
  if (!latency.error.empty())
    return latency.error;
  if (latency.reported() != plan.reportedChainLatency)
    return "reported latency is stale";
  return {};
}

std::string describePlan(const ProcessingPlan& plan) {
  std::ostringstream out;
  out << (plan.legacy ? "legacy" : "grid") << " plan, revision " << plan.topologyRevision << ", "
      << plan.numOps << " ops in " << plan.numSeqs << " sequences, chain latency "
      << plan.reportedChainLatency << ", slots per row (";
  for (int row = 0; row < kGridRows; ++row)
    out << (row > 0 ? ", " : "") << plan.rowSlots[static_cast<std::size_t>(row)];
  out << ")" << (plan.valid ? "" : " (INVALID)") << "\n";
  describeSeq(plan, 0, 1, out);
  for (int s = 0; s < plan.numStrips && s < kMaxIoPairs; ++s) {
    const ProcessingPlan::Strip& strip = plan.strips[static_cast<std::size_t>(s)];
    out << "  strip " << s << ": ";
    if (strip.hostMainPair)
      out << "the host buffer's main pair in place, fold by input mode";
    else if (strip.in.type == RowInput::Type::Device)
      out << "device channel " << strip.in.channel << (strip.in.stereo ? " and the next" : "")
          << (strip.in.fold == RowInput::Fold::SumToMono ? ", summed to mono" : "");
    else
      out << "no source";
    out << (strip.in.gate ? ", gate" : "") << (strip.in.pitch ? ", pitch" : "") << ", up-port "
        << int(strip.port) << "\n";
  }
  for (int b = 0; b < plan.numBuses && b < kMaxIoPairs; ++b) {
    const ProcessingPlan::Bus& bus = plan.buses[static_cast<std::size_t>(b)];
    out << "  bus " << b << ": ";
    if (bus.hostMainPair)
      out << "the host buffer's main pair in place";
    else
      out << "output pair " << int(bus.pair);
    out << (bus.mainStrip ? ", main output strip" : "") << ", down-port " << int(bus.port)
        << "\n";
  }
  if (!plan.legacy)
    for (int row = 0; row < kGridRows; ++row) {
      const ProcessingPlan::RowInfo& info = plan.rowInfo[static_cast<std::size_t>(row)];
      out << "  row " << row << ": ";
      if (!info.active) {
        out << "inactive\n";
        continue;
      }
      out << "group " << int(info.group) << ", " << formatName(info.format) << ", slots ["
          << int(info.begin) << ", " << int(info.end) << "), ";
      if (info.source == ProcessingPlan::RowInfo::FromSplit)
        out << "from the split of row " << row - 1;
      else if (info.source == ProcessingPlan::RowInfo::FromFeed)
        out << "fed by rows";
      else if (info.strip >= 0)
        out << "strip " << int(info.strip);
      else
        out << "input missing";
      if (info.sink == ProcessingPlan::RowInfo::ToMix)
        out << ", to the mixer of row " << row - 1;
      else if (info.sink == ProcessingPlan::RowInfo::ToRow)
        out << ", feeds row " << int(info.feedsRow);
      else
        out << ", bus " << int(info.bus) << (info.outputFallback ? " (fallback to Out 1/2)" : "");
      out << ", latency " << info.latency << ", pad " << info.pad << "\n";
    }
  return out.str();
}

// Plan compiler tests
//
// compilePlan (ProcessingPlan.h) turns a snapshot of the rig's topology into
// the list of operations the chain stage runs. It is a pure function, so
// these tests need no processor: they build the snapshot by hand.
//
//   - Each legacy rig shape (mono, stereo lanes, branched with the trunk on
//     either side, and the dormant states around them) compiles to the
//     operations processChainStage always ran, in the same order.
//   - validatePlan accepts every compiled plan and names the fault in a
//     plan that breaks an invariant: a slot run twice or not at all, a row
//     run out of order, fork children sharing a buffer, a join whose sides
//     are not equally late, a sequence nothing reaches.
//   - The latency pass gives exactly the pads and the total the chain used
//     before there was a plan, over randomised lanes and latencies.
#include "ProcessingPlan.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

// A lane of `slots` entries with the given latencies at the given indices;
// every other entry is an insert placeholder.
std::vector<PlanSlot> makeLane(int slots, const std::vector<std::pair<int, int>>& latencyAt = {},
                               const std::vector<int>& blocksAt = {}) {
  std::vector<PlanSlot> lane(static_cast<size_t>(slots));
  for (const int i : blocksAt)
    lane[static_cast<size_t>(i)].isBlock = true;
  for (const auto& [i, latency] : latencyAt) {
    lane[static_cast<size_t>(i)].isBlock = true;
    lane[static_cast<size_t>(i)].latency = latency;
  }
  return lane;
}

PlanInput monoInput(int slots = 5) {
  PlanInput input;
  input.topologyRevision = 7;
  input.rows[0].slots = makeLane(slots, {}, {0, 1});
  // The right lane exists in mono mode too; it just is not heard.
  input.rows[1].slots = makeLane(5);
  return input;
}

PlanInput stereoInput(int leftSlots = 5, int rightSlots = 5) {
  PlanInput input = monoInput(leftSlots);
  input.legacy.stereoEnabled = true;
  input.rows[0].format = RowFormat::Mono;
  input.rows[1].format = RowFormat::Mono;
  input.rows[1].slots = makeLane(rightSlots, {}, {0});
  return input;
}

PlanInput branchedInput(int trunkRow, int tapIndex, int trunkSlots = 6, int branchSlots = 4) {
  PlanInput input = stereoInput();
  input.legacy.trunkRow = trunkRow;
  input.legacy.branchTapIndex = tapIndex;
  input.rows[static_cast<size_t>(trunkRow)].slots = makeLane(trunkSlots, {}, {0, 1, 2});
  input.rows[static_cast<size_t>(1 - trunkRow)].slots = makeLane(branchSlots, {}, {0});
  return input;
}

const PlanOp& opAt(const ProcessingPlan& plan, int seq, int index) {
  const PlanSeq& s = plan.seqs[static_cast<size_t>(seq)];
  EXPECT_LT(index, static_cast<int>(s.numOps));
  return plan.ops[static_cast<size_t>(s.firstOp + index)];
}

// The pad of the row's Delay operation, 0 when the plan has none for it.
int padOf(const ProcessingPlan& plan, int row) {
  for (int i = 0; i < plan.numOps; ++i) {
    const PlanOp& op = plan.ops[static_cast<size_t>(i)];
    if (op.type == PlanOp::Delay && op.row == row)
      return op.latency;
  }
  return 0;
}

// How many RunBlocks operations cover each slot of each row, counted over
// the operations themselves (not through validatePlan's own walk).
std::array<std::vector<int>, kGridRows> slotCoverage(const ProcessingPlan& plan,
                                                     const PlanInput& input) {
  std::array<std::vector<int>, kGridRows> seen;
  for (int row = 0; row < kGridRows; ++row)
    seen[static_cast<size_t>(row)].assign(input.rows[static_cast<size_t>(row)].slots.size(), 0);
  for (int i = 0; i < plan.numOps; ++i) {
    const PlanOp& op = plan.ops[static_cast<size_t>(i)];
    if (op.type != PlanOp::RunBlocks)
      continue;
    for (int slot = op.begin; slot < op.end; ++slot)
      ++seen[op.row][static_cast<size_t>(slot)];
  }
  return seen;
}

void expectCoverage(const ProcessingPlan& plan, const PlanInput& input,
                    const std::array<bool, kGridRows>& activeRows) {
  const auto seen = slotCoverage(plan, input);
  for (int row = 0; row < kGridRows; ++row)
    for (size_t slot = 0; slot < seen[static_cast<size_t>(row)].size(); ++slot)
      EXPECT_EQ(seen[static_cast<size_t>(row)][slot], activeRows[static_cast<size_t>(row)] ? 1 : 0)
          << "row " << row << " slot " << slot;
}

// ---------------------------------------------------------------------------
// The legacy shapes
// ---------------------------------------------------------------------------

TEST(PlanCompileTest, MonoIsOneSegmentOnTheMainPair) {
  const PlanInput input = monoInput();
  const ProcessingPlan plan = compilePlan(input);
  ASSERT_TRUE(plan.valid);
  EXPECT_TRUE(plan.legacy);
  EXPECT_EQ(plan.topologyRevision, 7u);
  EXPECT_EQ(validatePlan(plan, input), "");
  std::cout << "[plan] mono\n" << describePlan(plan);

  ASSERT_EQ(plan.numSeqs, 1);
  ASSERT_EQ(plan.numOps, 1);
  const PlanOp& run = opAt(plan, 0, 0);
  EXPECT_EQ(run.type, PlanOp::RunBlocks);
  EXPECT_EQ(run.row, 0);
  EXPECT_EQ(run.begin, 0);
  EXPECT_EQ(run.end, 5);
  // Both channels of the main pair, or the one channel a mono host buffer
  // has: the view follows the host buffer.
  EXPECT_EQ(run.a.kind, BufRef::Port);
  EXPECT_EQ(run.a.index, 0);
  EXPECT_EQ(run.a.firstCh, 0);
  EXPECT_EQ(run.a.numCh, 2);
  EXPECT_TRUE(run.a.followHostChannels);
  EXPECT_EQ(static_cast<RowFormat>(run.format), RowFormat::Stereo);
  EXPECT_TRUE(run.followRigDualMono);
  // The amp question is asked of the whole lane and nowhere else.
  EXPECT_EQ(run.scanBegin, 0);
  EXPECT_EQ(run.scanEnd, 5);
  EXPECT_EQ(run.numCont, 0);

  EXPECT_EQ(plan.numStrips, 1);
  EXPECT_EQ(plan.numBuses, 1);
  EXPECT_TRUE(plan.buses[0].mainStrip);
  for (int row = 0; row < kGridRows; ++row)
    EXPECT_EQ(plan.rowSlots[static_cast<size_t>(row)], row < 2 ? 5 : 0);
  expectCoverage(plan, input, {true, false, false, false});
}

// Every legacy shape has the same two ends: one input strip and one output
// bus, both the host buffer's main pair in place, on port 0. The strip runs
// every input stage and takes its fold from the input mode; the bus is the
// main output strip.
TEST(PlanCompileTest, LegacyHasTheMainPairAsItsOnlyStripAndBus) {
  for (const PlanInput& input : {monoInput(), stereoInput(), branchedInput(0, 1)}) {
    const ProcessingPlan plan = compilePlan(input);
    ASSERT_TRUE(plan.valid);
    ASSERT_EQ(plan.numStrips, 1);
    const ProcessingPlan::Strip& strip = plan.strips[0];
    EXPECT_TRUE(strip.hostMainPair);
    EXPECT_EQ(strip.port, 0);
    EXPECT_EQ(strip.in.type, RowInput::Type::Device);
    EXPECT_EQ(strip.in.channel, 0);
    EXPECT_TRUE(strip.in.stereo);
    EXPECT_TRUE(strip.in.gate);
    EXPECT_TRUE(strip.in.pitch);
    EXPECT_EQ(strip.in.trimDb, 0.0f);
    EXPECT_EQ(strip.padHostSamples, 0);

    ASSERT_EQ(plan.numBuses, 1);
    const ProcessingPlan::Bus& bus = plan.buses[0];
    EXPECT_TRUE(bus.hostMainPair);
    EXPECT_TRUE(bus.mainStrip);
    EXPECT_EQ(bus.pair, 0);
    EXPECT_EQ(bus.port, 0);
  }
}

// The boundary is opened with one port per strip and per bus, so the ports
// of each side have to be 0 to count - 1, and one bus is the main output.
TEST(PlanCompileTest, ValidateChecksStripAndBusPorts) {
  const PlanInput input = stereoInput();
  const ProcessingPlan good = compilePlan(input);
  ASSERT_EQ(validatePlan(good, input), "");

  ProcessingPlan plan = good;
  plan.numStrips = 0;
  EXPECT_NE(validatePlan(plan, input), "");

  plan = good;
  plan.numBuses = kMaxIoPairs + 1;
  EXPECT_NE(validatePlan(plan, input), "");

  plan = good;
  plan.strips[0].port = 1;
  EXPECT_NE(validatePlan(plan, input), "");

  plan = good;
  plan.numStrips = 2;
  plan.strips[1].port = 0;  // twice port 0, never port 1
  EXPECT_NE(validatePlan(plan, input), "");
  plan.strips[1].port = 1;
  EXPECT_EQ(validatePlan(plan, input), "");

  plan = good;
  plan.numBuses = 2;
  plan.buses[1].port = 1;
  EXPECT_EQ(validatePlan(plan, input), "");
  plan.buses[1].mainStrip = true;  // two main outputs
  EXPECT_NE(validatePlan(plan, input), "");
  plan.buses[0].mainStrip = false;
  plan.buses[1].mainStrip = false;  // none
  EXPECT_NE(validatePlan(plan, input), "");
}

// Dual mono is the same plan: whether the lane's two channels are two
// signals is the rig's answer for each callback, not the topology's.
TEST(PlanCompileTest, DualMonoCompilesLikeMono) {
  PlanInput dual = monoInput();
  dual.rows[0].format = RowFormat::DualMono;
  const ProcessingPlan plan = compilePlan(dual);
  ASSERT_TRUE(plan.valid);
  EXPECT_EQ(validatePlan(plan, dual), "");
  EXPECT_EQ(describePlan(plan), describePlan(compilePlan(monoInput())));
  EXPECT_TRUE(opAt(plan, 0, 0).followRigDualMono);
}

// A right lane full of latent blocks and a branch that lies dormant in mono
// mode are not compiled and add nothing.
TEST(PlanCompileTest, MonoIgnoresTheDormantRightLaneAndBranch) {
  PlanInput input = monoInput();
  input.rows[0].slots = makeLane(6, {{1, 32}, {3, 100}});
  input.rows[1].slots = makeLane(5, {{0, 999}});
  input.legacy.trunkRow = 1;      // the branch fields persist while mono...
  input.legacy.branchTapIndex = -1;  // ...but the tap is unresolved
  const ProcessingPlan plan = compilePlan(input);
  ASSERT_TRUE(plan.valid);
  EXPECT_EQ(validatePlan(plan, input), "");
  EXPECT_EQ(plan.numOps, 1);
  EXPECT_EQ(opAt(plan, 0, 0).latency, 132);
  EXPECT_EQ(plan.reportedChainLatency, 132);
  EXPECT_EQ(padOf(plan, 0), 0);
  EXPECT_EQ(padOf(plan, 1), 0);
  expectCoverage(plan, input, {true, false, false, false});
}

TEST(PlanCompileTest, StereoForksTheTwoLanesThenAlignsThem) {
  PlanInput input = stereoInput(6, 5);
  input.rows[0].slots[1].latency = 48;
  const ProcessingPlan plan = compilePlan(input);
  ASSERT_TRUE(plan.valid);
  EXPECT_EQ(validatePlan(plan, input), "");
  std::cout << "[plan] stereo\n" << describePlan(plan);

  ASSERT_EQ(plan.numSeqs, 3);
  ASSERT_EQ(plan.numOps, 5);
  const PlanOp& fork = opAt(plan, 0, 0);
  ASSERT_EQ(fork.type, PlanOp::Fork);
  ASSERT_EQ(fork.numChildren, 2);

  // The left lane is the fork's first child: it stays on the audio thread
  // and runs first when the callback does not fork.
  const PlanOp& left = opAt(plan, fork.child[0], 0);
  EXPECT_EQ(left.type, PlanOp::RunBlocks);
  EXPECT_EQ(left.row, 0);
  EXPECT_EQ(left.begin, 0);
  EXPECT_EQ(left.end, 6);
  EXPECT_EQ(left.a.firstCh, 0);
  EXPECT_EQ(left.a.numCh, 1);
  EXPECT_FALSE(left.a.followHostChannels);
  EXPECT_EQ(static_cast<RowFormat>(left.format), RowFormat::Mono);

  const PlanOp& right = opAt(plan, fork.child[1], 0);
  EXPECT_EQ(right.row, 1);
  EXPECT_EQ(right.begin, 0);
  EXPECT_EQ(right.end, 5);
  EXPECT_EQ(right.a.firstCh, 1);
  EXPECT_EQ(right.a.numCh, 1);

  // After the join: left then right, each on its own channel.
  const PlanOp& delayL = opAt(plan, 0, 1);
  const PlanOp& delayR = opAt(plan, 0, 2);
  EXPECT_EQ(delayL.type, PlanOp::Delay);
  EXPECT_EQ(delayL.row, 0);
  EXPECT_EQ(delayL.a.firstCh, 0);
  EXPECT_EQ(delayL.latency, 0);
  EXPECT_EQ(delayR.type, PlanOp::Delay);
  EXPECT_EQ(delayR.row, 1);
  EXPECT_EQ(delayR.a.firstCh, 1);
  EXPECT_EQ(delayR.latency, 48) << "the lane without latency waits for the other";
  EXPECT_EQ(plan.reportedChainLatency, 48);
  expectCoverage(plan, input, {true, true, false, false});
}

TEST(PlanCompileTest, BranchedSplitsTheTrunkAroundTheTap) {
  for (const int trunk : {0, 1}) {
    SCOPED_TRACE(trunk == 0 ? "trunk left" : "trunk right");
    const int branch = 1 - trunk;
    PlanInput input = branchedInput(trunk, /*tapIndex=*/1);
    input.rows[static_cast<size_t>(trunk)].slots[0].latency = 10;   // before the tap
    input.rows[static_cast<size_t>(trunk)].slots[2].latency = 64;   // after it
    input.rows[static_cast<size_t>(branch)].slots[0].latency = 24;  // in the branch lane
    const ProcessingPlan plan = compilePlan(input);
    ASSERT_TRUE(plan.valid);
    EXPECT_EQ(validatePlan(plan, input), "");
    std::cout << "[plan] branched, trunk " << (trunk == 0 ? "left" : "right") << "\n"
              << describePlan(plan);

    ASSERT_EQ(plan.numSeqs, 3);
    ASSERT_EQ(plan.numOps, 7);

    // The trunk up to and including the tapped block, on the trunk's
    // channel.
    const PlanOp& prefix = opAt(plan, 0, 0);
    EXPECT_EQ(prefix.type, PlanOp::RunBlocks);
    EXPECT_EQ(prefix.row, trunk);
    EXPECT_EQ(prefix.begin, 0);
    EXPECT_EQ(prefix.end, 2);
    EXPECT_EQ(prefix.a.firstCh, trunk);
    EXPECT_EQ(prefix.a.numCh, 1);
    EXPECT_EQ(prefix.latency, 10);
    // A segment still answers for its whole lane.
    EXPECT_EQ(prefix.scanBegin, 0);
    EXPECT_EQ(prefix.scanEnd, 6);

    // The tap, copied onto the branch lane's channel.
    const PlanOp& copy = opAt(plan, 0, 1);
    EXPECT_EQ(copy.type, PlanOp::Copy);
    EXPECT_EQ(copy.a.firstCh, trunk);
    EXPECT_EQ(copy.b.firstCh, branch);

    // The trunk's remainder stays on the audio thread, the branch lane is
    // the one handed to a worker.
    const PlanOp& fork = opAt(plan, 0, 2);
    ASSERT_EQ(fork.type, PlanOp::Fork);
    ASSERT_EQ(fork.numChildren, 2);
    const PlanOp& rest = opAt(plan, fork.child[0], 0);
    EXPECT_EQ(rest.row, trunk);
    EXPECT_EQ(rest.begin, 2);
    EXPECT_EQ(rest.end, 6);
    EXPECT_EQ(rest.a.firstCh, trunk);
    EXPECT_EQ(rest.scanBegin, 0);
    EXPECT_EQ(rest.scanEnd, 6);
    const PlanOp& branchRun = opAt(plan, fork.child[1], 0);
    EXPECT_EQ(branchRun.row, branch);
    EXPECT_EQ(branchRun.begin, 0);
    EXPECT_EQ(branchRun.end, 4);
    EXPECT_EQ(branchRun.a.firstCh, branch);

    // The shorter of the two parallel sections waits: the trunk's remainder
    // has 64, the branch lane 24. The prefix is common to both.
    const PlanOp& delayTrunk = opAt(plan, 0, 3);
    const PlanOp& delayBranch = opAt(plan, 0, 4);
    EXPECT_EQ(delayTrunk.type, PlanOp::Delay);
    EXPECT_EQ(delayTrunk.row, trunk);
    EXPECT_EQ(delayTrunk.a.firstCh, trunk);
    EXPECT_EQ(delayTrunk.latency, 0);
    EXPECT_EQ(delayBranch.type, PlanOp::Delay);
    EXPECT_EQ(delayBranch.row, branch);
    EXPECT_EQ(delayBranch.a.firstCh, branch);
    EXPECT_EQ(delayBranch.latency, 40);
    EXPECT_EQ(plan.reportedChainLatency, 74);
    expectCoverage(plan, input, {true, true, false, false});
  }
}

// The tap can be the trunk's last entry: the remainder is then an empty
// range, still compiled (its sequence runs nothing).
TEST(PlanCompileTest, BranchedAtTheEndOfTheTrunkLeavesAnEmptyRemainder) {
  PlanInput input = branchedInput(0, /*tapIndex=*/5);
  const ProcessingPlan plan = compilePlan(input);
  ASSERT_TRUE(plan.valid);
  EXPECT_EQ(validatePlan(plan, input), "");
  const PlanOp& fork = opAt(plan, 0, 2);
  const PlanOp& rest = opAt(plan, fork.child[0], 0);
  EXPECT_EQ(rest.begin, 6);
  EXPECT_EQ(rest.end, 6);
}

// Rows 2 and 3 are never run by a legacy plan, whatever they hold.
TEST(PlanCompileTest, LegacyNeverRunsRowsTwoAndThree) {
  for (PlanInput input : {monoInput(), stereoInput(), branchedInput(0, 0)}) {
    // The operations, without the summary line (which counts the slots).
    const auto operations = [](const ProcessingPlan& p) {
      const std::string text = describePlan(p);
      return text.substr(text.find('\n') + 1);
    };
    const std::string without = operations(compilePlan(input));
    input.rows[2].slots = makeLane(8, {{0, 77}});
    input.rows[3].slots = makeLane(8, {{3, 5}});
    const ProcessingPlan plan = compilePlan(input);
    ASSERT_TRUE(plan.valid);
    EXPECT_EQ(validatePlan(plan, input), "");
    EXPECT_EQ(operations(plan), without);
    EXPECT_EQ(plan.reportedChainLatency, 0);
    EXPECT_EQ(plan.rowSlots[2], 8);
    expectCoverage(plan, input,
                   {true, input.legacy.stereoEnabled, false, false});
  }
}

TEST(PlanCompileTest, RefusesWhatItCannotCompile) {
  // A tap that is not in the trunk lane.
  EXPECT_FALSE(compilePlan(branchedInput(0, /*tapIndex=*/6)).valid);
  // Grid mode has no front-end yet.
  PlanInput gridInput = monoInput();
  gridInput.mode = RoutingMode::Grid;
  const ProcessingPlan gridPlan = compilePlan(gridInput);
  EXPECT_FALSE(gridPlan.valid);
  EXPECT_FALSE(gridPlan.legacy);
  EXPECT_NE(validatePlan(gridPlan, gridInput), "");
  // A lane too long for the plan's indices.
  PlanInput huge = monoInput();
  huge.rows[0].slots.resize(40000);
  EXPECT_FALSE(compilePlan(huge).valid);
}

// ---------------------------------------------------------------------------
// validatePlan
// ---------------------------------------------------------------------------

TEST(PlanValidateTest, NamesASlotThatIsNotRun) {
  const PlanInput input = monoInput();
  ProcessingPlan plan = compilePlan(input);
  plan.ops[0].end = 4;
  EXPECT_NE(validatePlan(plan, input).find("runs 4 of 5"), std::string::npos)
      << validatePlan(plan, input);
}

TEST(PlanValidateTest, NamesASlotRunTwiceOrOutOfOrder) {
  const PlanInput input = branchedInput(0, 1);
  ProcessingPlan plan = compilePlan(input);
  ASSERT_EQ(validatePlan(plan, input), "");

  // The remainder starts one slot early: slot 1 would run twice.
  ProcessingPlan twice = plan;
  twice.ops[static_cast<size_t>(twice.seqs[1].firstOp)].begin = 1;
  EXPECT_NE(validatePlan(twice, input).find("out of order"), std::string::npos)
      << validatePlan(twice, input);

  // The prefix moved behind the fork: the remainder would run first.
  ProcessingPlan reordered = plan;
  std::swap(reordered.ops[0], reordered.ops[2]);
  EXPECT_NE(validatePlan(reordered, input).find("out of order"), std::string::npos)
      << validatePlan(reordered, input);
}

TEST(PlanValidateTest, NamesARangePastTheEndOfItsRow) {
  const PlanInput input = monoInput();
  ProcessingPlan plan = compilePlan(input);
  plan.ops[0].end = 6;
  EXPECT_NE(validatePlan(plan, input).find("out of bounds"), std::string::npos);
}

TEST(PlanValidateTest, NamesAnInactiveRowThatIsRun) {
  // A stereo plan checked against a mono rig: the right lane must not run.
  const ProcessingPlan plan = compilePlan(stereoInput());
  PlanInput mono = stereoInput();
  mono.legacy.stereoEnabled = false;
  EXPECT_NE(validatePlan(plan, mono).find("inactive row 1"), std::string::npos)
      << validatePlan(plan, mono);
}

TEST(PlanValidateTest, NamesForkChildrenThatShareABuffer) {
  const PlanInput input = stereoInput();
  ProcessingPlan plan = compilePlan(input);
  // The right lane pointed at the left lane's channel.
  plan.ops[static_cast<size_t>(plan.seqs[2].firstOp)].a.firstCh = 0;
  EXPECT_NE(validatePlan(plan, input).find("write the same buffer"), std::string::npos)
      << validatePlan(plan, input);
}

TEST(PlanValidateTest, NamesAJoinWhoseSidesArriveApart) {
  PlanInput input = stereoInput();
  input.rows[1].slots[0].latency = 100;
  ProcessingPlan plan = compilePlan(input);
  ASSERT_EQ(validatePlan(plan, input), "");
  ASSERT_EQ(padOf(plan, 0), 100);

  // A stale pad: the left lane would arrive 30 samples early.
  ProcessingPlan stalePad = plan;
  stalePad.ops[1].latency = 70;
  EXPECT_NE(validatePlan(stalePad, input).find("unequally late"), std::string::npos)
      << validatePlan(stalePad, input);

  // A block's latency moved and the latency pass did not run.
  PlanInput moved = input;
  moved.rows[1].slots[0].latency = 120;
  EXPECT_NE(validatePlan(plan, moved), "");
  computePlanLatency(plan, moved);
  EXPECT_EQ(validatePlan(plan, moved), "");
  EXPECT_EQ(padOf(plan, 0), 120);
  EXPECT_EQ(plan.reportedChainLatency, 120);
}

TEST(PlanValidateTest, NamesBrokenStructure) {
  const PlanInput input = stereoInput();
  const ProcessingPlan plan = compilePlan(input);

  // A sequence nothing forks to.
  ProcessingPlan orphan = plan;
  orphan.ops[0].numChildren = 1;
  EXPECT_NE(validatePlan(orphan, input), "");

  // A child reached twice.
  ProcessingPlan twice = plan;
  twice.ops[0].child[1] = twice.ops[0].child[0];
  EXPECT_NE(validatePlan(twice, input).find("more than once"), std::string::npos)
      << validatePlan(twice, input);

  // A fork that points back at its own sequence.
  ProcessingPlan loop = plan;
  loop.ops[0].child[0] = 0;
  EXPECT_NE(validatePlan(loop, input), "");

  // A grid operation has no place in a legacy plan.
  ProcessingPlan reserved = plan;
  reserved.ops[1].type = PlanOp::Split;
  EXPECT_NE(validatePlan(reserved, input).find("misplaced Split"), std::string::npos);
  reserved.ops[1].type = PlanOp::FeedRow;
  EXPECT_NE(validatePlan(reserved, input).find("misplaced FeedRow"), std::string::npos);
  reserved.ops[1].type = static_cast<PlanOp::Type>(200);
  EXPECT_NE(validatePlan(reserved, input).find("unknown operation"), std::string::npos);
  reserved.ops[1].type = PlanOp::RowOut;
  EXPECT_NE(validatePlan(reserved, input).find("misplaced RowOut"), std::string::npos);

  // Another topology revision, or a lane that changed length since.
  PlanInput later = input;
  later.topologyRevision = input.topologyRevision + 1;
  EXPECT_NE(validatePlan(plan, later).find("revision"), std::string::npos);
  PlanInput longer = input;
  longer.rows[2].slots.resize(1);
  EXPECT_NE(validatePlan(plan, longer).find("changed length"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Latency
// ---------------------------------------------------------------------------

// The lane pads and the reported total as TONE3000Processor::
// refreshLatencyCompensation computed them before the plan existed, written
// out again here so the plan's latency pass is checked against it and not
// against itself.
struct LegacyLatency {
  int pad[2] = {0, 0};
  int total = 0;
};

LegacyLatency previousFormula(const PlanInput& input) {
  int laneLatency[2] = {0, 0};
  int trunkPrefixLatency = 0;
  const int trunkLane = input.legacy.trunkRow;
  const int tap = input.legacy.branchTapIndex;
  for (int li = 0; li < 2; ++li) {
    const auto& lane = input.rows[static_cast<size_t>(li)].slots;
    for (int i = 0; i < static_cast<int>(lane.size()); ++i) {
      const int latency = std::max(0, lane[static_cast<size_t>(i)].latency);
      laneLatency[li] += latency;
      if (tap >= 0 && li == trunkLane && i <= tap)
        trunkPrefixLatency += latency;
    }
  }

  LegacyLatency result;
  result.total = laneLatency[0];
  if (input.legacy.stereoEnabled) {
    int section[2] = {laneLatency[0], laneLatency[1]};
    if (tap >= 0)
      section[trunkLane] -= trunkPrefixLatency;
    const int longest = std::max(section[0], section[1]);
    result.pad[0] = longest - section[0];
    result.pad[1] = longest - section[1];
    result.total = (tap >= 0 ? trunkPrefixLatency : 0) + longest;
  }
  return result;
}

TEST(PlanLatencyTest, LegacyPadsEqualThePreviousFormulaOverRandomRigs) {
  std::mt19937 rng(20261010);
  const auto pick = [&rng](int low, int high) {
    return std::uniform_int_distribution<int>(low, high)(rng);
  };

  const auto randomLane = [&](int slots) {
    std::vector<PlanSlot> lane(static_cast<size_t>(slots));
    for (auto& slot : lane) {
      slot.isBlock = pick(0, 3) != 0;
      // Most blocks have no latency, a few have a lot, as in a real rig.
      if (slot.isBlock && pick(0, 2) == 0)
        slot.latency = pick(0, 4) == 0 ? pick(1, 20000) : pick(1, 512);
    }
    return lane;
  };

  int shapes[3] = {0, 0, 0};
  for (int trial = 0; trial < 5000; ++trial) {
    PlanInput input;
    input.topologyRevision = static_cast<std::uint32_t>(trial);
    input.rows[0].slots = randomLane(pick(1, 14));
    input.rows[1].slots = randomLane(pick(1, 14));
    input.legacy.stereoEnabled = pick(0, 3) != 0;
    input.legacy.trunkRow = pick(0, 1);
    // A branch is only ever live in stereo mode, on a block of the trunk.
    if (input.legacy.stereoEnabled && pick(0, 1) == 1) {
      const auto& trunk = input.rows[static_cast<size_t>(input.legacy.trunkRow)].slots;
      input.legacy.branchTapIndex = pick(0, static_cast<int>(trunk.size()) - 1);
    }
    ++shapes[!input.legacy.stereoEnabled ? 0 : input.legacy.branchTapIndex < 0 ? 1 : 2];

    const ProcessingPlan plan = compilePlan(input);
    ASSERT_TRUE(plan.valid) << "trial " << trial;
    ASSERT_EQ(validatePlan(plan, input), "") << "trial " << trial << "\n" << describePlan(plan);

    const LegacyLatency expected = previousFormula(input);
    ASSERT_EQ(padOf(plan, 0), expected.pad[0]) << "trial " << trial << "\n" << describePlan(plan);
    ASSERT_EQ(padOf(plan, 1), expected.pad[1]) << "trial " << trial << "\n" << describePlan(plan);
    ASSERT_EQ(plan.reportedChainLatency, expected.total)
        << "trial " << trial << "\n" << describePlan(plan);

    // The latency pass alone, after every latency moved, lands where a
    // fresh compile does: an install never needs the structure recompiled.
    PlanInput moved = input;
    for (auto& row : moved.rows)
      for (auto& slot : row.slots)
        if (slot.isBlock && pick(0, 1) == 1)
          slot.latency = pick(0, 300);
    ProcessingPlan repassed = plan;
    computePlanLatency(repassed, moved);
    const LegacyLatency movedExpected = previousFormula(moved);
    ASSERT_EQ(padOf(repassed, 0), movedExpected.pad[0]) << "trial " << trial;
    ASSERT_EQ(padOf(repassed, 1), movedExpected.pad[1]) << "trial " << trial;
    ASSERT_EQ(repassed.reportedChainLatency, movedExpected.total) << "trial " << trial;
    ASSERT_EQ(describePlan(repassed), describePlan(compilePlan(moved))) << "trial " << trial;
  }
  // The generator must have reached every shape for the loop to mean much.
  EXPECT_GT(shapes[0], 500);
  EXPECT_GT(shapes[1], 500);
  EXPECT_GT(shapes[2], 500);
}

// A negative latency from a misbehaving block counts as none, as before.
TEST(PlanLatencyTest, NegativeBlockLatencyCountsAsZero) {
  PlanInput input = stereoInput();
  input.rows[0].slots[0].latency = -50;
  input.rows[1].slots[0].latency = 20;
  const ProcessingPlan plan = compilePlan(input);
  EXPECT_EQ(padOf(plan, 0), 20);
  EXPECT_EQ(padOf(plan, 1), 0);
  EXPECT_EQ(plan.reportedChainLatency, 20);
}

}  // namespace

// ---------------------------------------------------------------------------
// The grid front-end (independent rows)
// ---------------------------------------------------------------------------

namespace {

void routeRow(PlanInput& input, int row, int channel, int pair, RowFormat format = RowFormat::Stereo,
              bool stereoIn = false) {
  PlanInput::Row& r = input.rows[static_cast<size_t>(row)];
  r.format = format;
  r.input.type = RowInput::Type::Device;
  r.input.channel = channel;
  r.input.stereo = stereoIn;
  r.outputType = RowOutput::Type::Device;
  r.outputPair = pair;
}

PlanInput gridInput() {
  PlanInput input;
  input.mode = RoutingMode::Grid;
  input.topologyRevision = 11;
  for (auto& row : input.rows)
    row.slots = makeLane(kGridCols);
  return input;
}

int countOps(const ProcessingPlan& plan, PlanOp::Type type) {
  int count = 0;
  for (int i = 0; i < plan.numOps; ++i)
    count += plan.ops[static_cast<size_t>(i)].type == type ? 1 : 0;
  return count;
}

const PlanOp* findOp(const ProcessingPlan& plan, PlanOp::Type type, int row) {
  for (int i = 0; i < plan.numOps; ++i)
    if (plan.ops[static_cast<size_t>(i)].type == type && plan.ops[static_cast<size_t>(i)].row == row)
      return &plan.ops[static_cast<size_t>(i)];
  return nullptr;
}

}  // namespace

TEST(GridPlanCompileTest, OneRowIsFillRunSendWithoutAFork) {
  PlanInput input = gridInput();
  routeRow(input, 0, 0, 0, RowFormat::Stereo, true);
  input.rows[0].input.fold = RowInput::Fold::SumToMono;
  input.rows[0].input.pitch = true;
  input.rows[0].slots = makeLane(kGridCols, {}, {0, 1});
  // Row 1 holds blocks but has no input: kept, not compiled.
  input.rows[1].slots = makeLane(kGridCols, {}, {0});

  const ProcessingPlan plan = compilePlan(input);
  ASSERT_TRUE(plan.valid);
  EXPECT_FALSE(plan.legacy);
  EXPECT_EQ(validatePlan(plan, input), "");
  EXPECT_EQ(plan.numSeqs, 1);
  ASSERT_EQ(plan.numOps, 3);
  EXPECT_EQ(plan.ops[0].type, PlanOp::RowIn);
  EXPECT_EQ(plan.ops[1].type, PlanOp::RunBlocks);
  EXPECT_EQ(plan.ops[1].begin, 0);
  EXPECT_EQ(plan.ops[1].end, kGridCols);
  EXPECT_EQ(plan.ops[1].a.kind, BufRef::Row);
  EXPECT_EQ(plan.ops[1].a.numCh, 2);
  EXPECT_FALSE(plan.ops[1].followRigDualMono);
  EXPECT_EQ(plan.ops[2].type, PlanOp::RowOut);
  EXPECT_TRUE(plan.ops[2].firstOnBus);
  EXPECT_EQ(plan.ops[2].b.kind, BufRef::Bus);
  ASSERT_EQ(plan.numStrips, 1);
  EXPECT_FALSE(plan.strips[0].hostMainPair);
  EXPECT_EQ(plan.strips[0].in.fold, RowInput::Fold::SumToMono);
  ASSERT_EQ(plan.numBuses, 1);
  EXPECT_TRUE(plan.buses[0].mainStrip);
  EXPECT_TRUE(plan.buses[0].hasWriter);
  EXPECT_FALSE(plan.buses[0].hostMainPair);
  EXPECT_TRUE(plan.rowInfo[0].active);
  EXPECT_FALSE(plan.rowInfo[1].active);
  EXPECT_EQ(plan.numGroups, 1);
}

TEST(GridPlanCompileTest, IndependentRowsForkAndSendInRowOrder) {
  PlanInput input = gridInput();
  routeRow(input, 0, 0, 0);
  routeRow(input, 1, 1, 1, RowFormat::Mono);
  routeRow(input, 3, 0, 0, RowFormat::DualMono, true);
  input.rows[0].slots = makeLane(kGridCols, {{2, 64}}, {0});
  input.rows[3].slots = makeLane(kGridCols, {{5, 10}});

  const ProcessingPlan plan = compilePlan(input);
  ASSERT_TRUE(plan.valid);
  EXPECT_EQ(validatePlan(plan, input), "") << describePlan(plan);
  std::cout << "[plan] three independent rows\n" << describePlan(plan);

  // The root: one fork over the three groups, then the sends on the calling
  // thread in row order.
  EXPECT_EQ(plan.numSeqs, 4);
  ASSERT_EQ(plan.seqs[0].numOps, 4);
  EXPECT_EQ(plan.ops[0].type, PlanOp::Fork);
  EXPECT_EQ(plan.ops[0].numChildren, 3);
  for (int i = 1; i <= 3; ++i)
    EXPECT_EQ(plan.ops[static_cast<size_t>(i)].type, PlanOp::RowOut);
  EXPECT_EQ(plan.ops[1].row, 0);
  EXPECT_EQ(plan.ops[2].row, 1);
  EXPECT_EQ(plan.ops[3].row, 3);
  EXPECT_EQ(plan.numGroups, 3);

  // Rows 0 and 3 share input 1 but not the format of the source (mono
  // against the pair), so they are two strips; the buses are the two pairs.
  EXPECT_EQ(plan.numStrips, 3);
  ASSERT_EQ(plan.numBuses, 2);
  EXPECT_EQ(plan.buses[0].pair, 0);
  EXPECT_EQ(plan.buses[1].pair, 1);
  EXPECT_TRUE(plan.ops[1].firstOnBus);
  EXPECT_TRUE(plan.ops[2].firstOnBus);
  EXPECT_FALSE(plan.ops[3].firstOnBus) << "row 3 adds to what row 0 put on Out 1/2";
  EXPECT_EQ(findOp(plan, PlanOp::RunBlocks, 1)->a.numCh, 1) << "a mono row is one channel";
  EXPECT_EQ(findOp(plan, PlanOp::RunBlocks, 3)->format, static_cast<int>(RowFormat::DualMono));

  // Rows 0 and 3 meet on one bus: the earlier one waits. Row 1 is alone on
  // its bus and waits for nobody (the standalone rule).
  EXPECT_EQ(plan.rowInfo[0].latency, 64);
  EXPECT_EQ(plan.rowInfo[0].pad, 0);
  EXPECT_EQ(plan.rowInfo[3].latency, 10);
  EXPECT_EQ(plan.rowInfo[3].pad, 54);
  EXPECT_EQ(plan.rowInfo[1].pad, 0);
  EXPECT_EQ(plan.reportedChainLatency, 64);

  // The plugin rule: every bus as late as the latest.
  input.io.padAcrossBuses = true;
  const ProcessingPlan padded = compilePlan(input);
  EXPECT_EQ(validatePlan(padded, input), "");
  EXPECT_EQ(padded.rowInfo[1].pad, 64);
  EXPECT_EQ(padded.reportedChainLatency, 64);
  EXPECT_NE(validatePlan(plan, input), "") << "a per-bus plan does not hold under the plugin rule";
}

TEST(GridPlanCompileTest, RowsThatShareASourceShareAStrip) {
  PlanInput input = gridInput();
  routeRow(input, 0, 1, 0);
  routeRow(input, 1, 1, 1);
  routeRow(input, 2, 1, 2);
  input.rows[2].input.gate = false;  // another strip: the gate is the strip's
  const ProcessingPlan plan = compilePlan(input);
  ASSERT_TRUE(plan.valid);
  EXPECT_EQ(validatePlan(plan, input), "");
  EXPECT_EQ(plan.numStrips, 2);
  EXPECT_EQ(plan.rowInfo[0].strip, 0);
  EXPECT_EQ(plan.rowInfo[1].strip, 0);
  EXPECT_EQ(plan.rowInfo[2].strip, 1);
  EXPECT_EQ(plan.numBuses, 3);
  // The trim is the row's, never the strip's: two rows on one source can
  // trim apart, and a trim move does not change the plan.
  input.rows[0].input.trimDb = 6.0f;
  const ProcessingPlan trimmed = compilePlan(input);
  EXPECT_EQ(trimmed.numStrips, 2);
  EXPECT_EQ(trimmed.strips[0].in.trimDb, 0.0f);
}

TEST(GridPlanCompileTest, MissingChannelsAreFlaggedNotRewritten) {
  PlanInput input = gridInput();
  routeRow(input, 0, 0, 0);
  routeRow(input, 1, 4, 2);  // input 5, Out 5/6
  input.io.known = true;
  input.io.inputChannels = 0b11;
  input.io.outputChannels = 0b11;

  const ProcessingPlan plan = compilePlan(input);
  ASSERT_TRUE(plan.valid);
  EXPECT_EQ(validatePlan(plan, input), "");
  EXPECT_FALSE(plan.rowInfo[0].inputMissing);
  EXPECT_TRUE(plan.rowInfo[1].inputMissing);
  EXPECT_EQ(plan.rowInfo[1].strip, -1);
  EXPECT_TRUE(plan.rowInfo[1].outputFallback);
  EXPECT_FALSE(plan.rowInfo[0].outputFallback);
  EXPECT_EQ(plan.numStrips, 1);
  ASSERT_EQ(plan.numBuses, 1);
  EXPECT_EQ(plan.rowInfo[1].bus, 0) << "sent to Out 1/2";
  EXPECT_EQ(findOp(plan, PlanOp::RowIn, 1)->a.numCh, 0) << "filled with silence";
  const std::string text = describePlan(plan);
  EXPECT_NE(text.find("input missing"), std::string::npos) << text;
  EXPECT_NE(text.find("fallback"), std::string::npos) << text;

  // On a device with the channels the same input compiles to its own ports.
  input.io.inputChannels = 0xFF;
  input.io.outputChannels = 0xFF;
  const ProcessingPlan whole = compilePlan(input);
  EXPECT_EQ(whole.numStrips, 2);
  EXPECT_EQ(whole.numBuses, 2);
  EXPECT_FALSE(whole.rowInfo[1].inputMissing);
  EXPECT_FALSE(whole.rowInfo[1].outputFallback);
}

TEST(GridPlanCompileTest, AnEmptyGridStillHasAStripAndTheMainBus) {
  const PlanInput input = gridInput();
  const ProcessingPlan plan = compilePlan(input);
  ASSERT_TRUE(plan.valid);
  EXPECT_EQ(validatePlan(plan, input), "");
  EXPECT_EQ(plan.numOps, 0);
  EXPECT_EQ(plan.numStrips, 1);
  ASSERT_EQ(plan.numBuses, 1);
  EXPECT_TRUE(plan.buses[0].mainStrip);
  EXPECT_FALSE(plan.buses[0].hasWriter) << "the chain stage clears it";
}

TEST(GridPlanCompileTest, RefusesWhatItCannotRun) {
  std::string why;
  PlanInput input = gridInput();
  input.rows[2].slots = makeLane(9);
  EXPECT_FALSE(compilePlan(input, &why).valid);
  EXPECT_NE(why.find("row 2 holds 9 slots"), std::string::npos) << why;

  input = gridInput();
  routeRow(input, 0, kMaxIoChannels, 0);
  EXPECT_FALSE(compilePlan(input, &why).valid);
  EXPECT_NE(why.find("input channel"), std::string::npos) << why;

  input = gridInput();
  routeRow(input, 0, 0, kMaxIoPairs);
  EXPECT_FALSE(compilePlan(input, &why).valid);
  EXPECT_NE(why.find("output pair"), std::string::npos) << why;

  // A row that feeds a row which leads nowhere is not run: inactive, not an
  // error.
  input = gridInput();
  routeRow(input, 0, 0, 0);
  input.rows[0].outputType = RowOutput::Type::Row;
  input.rows[0].outputToRow = 1;
  const ProcessingPlan plan = compilePlan(input, &why);
  ASSERT_TRUE(plan.valid);
  EXPECT_FALSE(plan.rowInfo[0].active);
  EXPECT_EQ(validatePlan(plan, input), "");
}

TEST(GridPlanCompileTest, ValidateNamesBrokenGridPlans) {
  PlanInput input = gridInput();
  routeRow(input, 0, 0, 0);
  routeRow(input, 1, 1, 0);
  input.rows[0].slots = makeLane(kGridCols, {{0, 32}});
  const ProcessingPlan good = compilePlan(input);
  ASSERT_EQ(validatePlan(good, input), "");

  // A pad that no longer evens the bus out.
  ProcessingPlan broken = good;
  for (int i = 0; i < broken.numOps; ++i)
    if (broken.ops[static_cast<size_t>(i)].type == PlanOp::RowOut && broken.ops[static_cast<size_t>(i)].row == 1)
      broken.ops[static_cast<size_t>(i)].latency = 0;
  EXPECT_NE(validatePlan(broken, input).find("stale pad"), std::string::npos);

  // Two fork children writing one row's buffer.
  broken = good;
  for (int i = 0; i < broken.numOps; ++i)
    if (broken.ops[static_cast<size_t>(i)].type == PlanOp::RunBlocks && broken.ops[static_cast<size_t>(i)].row == 1)
      broken.ops[static_cast<size_t>(i)].a.index = 0;
  EXPECT_NE(validatePlan(broken, input).find("write the same buffer"), std::string::npos);

  // A row sent twice, a bus started twice.
  broken = good;
  for (int i = 0; i < broken.numOps; ++i)
    if (broken.ops[static_cast<size_t>(i)].type == PlanOp::RowOut)
      broken.ops[static_cast<size_t>(i)].firstOnBus = true;
  EXPECT_NE(validatePlan(broken, input).find("not started exactly once"), std::string::npos);

  // A row the rig no longer routes.
  PlanInput changed = input;
  changed.rows[1].outputType = RowOutput::Type::None;
  EXPECT_NE(validatePlan(good, changed), "");

  // A block whose latency moved since the pass.
  changed = input;
  changed.rows[0].slots[0].latency = 48;
  EXPECT_NE(validatePlan(good, changed).find("stale"), std::string::npos);
  ProcessingPlan refreshed = good;
  computePlanLatency(refreshed, changed);
  EXPECT_EQ(validatePlan(refreshed, changed), "");
  EXPECT_EQ(refreshed.rowInfo[1].pad, 48);
}

TEST(GridPlanCompileTest, CpuIsTheSumAndTheWidestGroup) {
  PlanInput input = gridInput();
  routeRow(input, 0, 0, 0);
  routeRow(input, 1, 1, 1);
  routeRow(input, 2, 2, 2);
  const auto cost = [&](int row, int slot, float value) {
    PlanSlot& s = input.rows[static_cast<size_t>(row)].slots[static_cast<size_t>(slot)];
    s.isBlock = true;
    s.costEstimate = value;
  };
  cost(0, 0, 0.10f);
  cost(0, 3, 0.05f);
  cost(1, 1, 0.30f);
  cost(2, 7, 0.02f);
  // A block in a row that is not routed costs nothing: it does not run.
  input.rows[3].slots[0].isBlock = true;
  input.rows[3].slots[0].costEstimate = 9.0f;
  // A cost on an empty slot is not counted either.
  input.rows[2].slots[1].costEstimate = 5.0f;

  const ProcessingPlan plan = compilePlan(input);
  ASSERT_TRUE(plan.valid);
  EXPECT_NEAR(plan.cpuEstimate, 0.47f, 1e-6f);
  EXPECT_NEAR(plan.cpuCritical, 0.30f, 1e-6f) << "the dearest of the three groups";
  const PlanCpu cpu = computePlanCpu(plan, input);
  EXPECT_NEAR(cpu.rows[0], 0.15f, 1e-6f);
  EXPECT_NEAR(cpu.rows[1], 0.30f, 1e-6f);
  EXPECT_NEAR(cpu.rows[2], 0.02f, 1e-6f);
  EXPECT_EQ(cpu.rows[3], 0.0f);

  // Ports cost a fixed allowance each, on the callback's own thread.
  input.io.portCost = 0.01f;
  const PlanCpu withPorts = computePlanCpu(plan, input);
  EXPECT_NEAR(withPorts.total, 0.47f + 6 * 0.01f, 1e-6f);
  EXPECT_NEAR(withPorts.critical, 0.30f + 6 * 0.01f, 1e-6f);

  // A legacy plan has the same two figures: the two lanes are its fork.
  PlanInput legacy = stereoInput();
  legacy.rows[0].slots[0].costEstimate = 0.2f;
  legacy.rows[0].slots[1].costEstimate = 0.1f;
  legacy.rows[1].slots[0].costEstimate = 0.25f;
  const ProcessingPlan legacyPlan = compilePlan(legacy);
  EXPECT_NEAR(legacyPlan.cpuEstimate, 0.55f, 1e-6f);
  EXPECT_NEAR(legacyPlan.cpuCritical, 0.30f, 1e-6f);
}

// Random valid grids without splits or feeds: every one compiles to a plan
// that holds, and the invariants the executor relies on are checked
// directly as well.
TEST(GridPlanCompileTest, FuzzedGridsCompileToPlansThatHold) {
  std::mt19937 rng(20261010);
  const auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); };
  int forked = 0, flagged = 0;
  for (int trial = 0; trial < 3000; ++trial) {
    PlanInput input = gridInput();
    input.topologyRevision = static_cast<std::uint32_t>(trial);
    input.io.padAcrossBuses = pick(2) == 0;
    input.io.known = pick(3) != 0;
    input.io.inputChannels = static_cast<std::uint32_t>(rng() & 0xFFu) | 1u;
    input.io.outputChannels = static_cast<std::uint32_t>(rng() & 0xFFu) | 1u;
    for (int row = 0; row < kGridRows; ++row) {
      PlanInput::Row& r = input.rows[static_cast<size_t>(row)];
      r.format = static_cast<RowFormat>(pick(3));
      for (auto& slot : r.slots) {
        slot.isBlock = pick(3) == 0;
        slot.latency = slot.isBlock && pick(4) == 0 ? pick(300) : 0;
        slot.costEstimate = slot.isBlock ? 0.01f * static_cast<float>(pick(20)) : 0.0f;
      }
      r.input.type = pick(4) == 0 ? RowInput::Type::None : RowInput::Type::Device;
      r.input.stereo = pick(2) == 0;
      r.input.channel = pick(kMaxIoChannels - (r.input.stereo ? 1 : 0));
      r.input.fold = r.input.stereo && pick(2) == 0 ? RowInput::Fold::SumToMono : RowInput::Fold::None;
      r.input.gate = pick(4) != 0;
      r.input.pitch = pick(3) == 0;
      r.input.trimDb = static_cast<float>(pick(13) - 6);
      r.outputType = pick(5) == 0 ? RowOutput::Type::None : RowOutput::Type::Device;
      r.outputPair = pick(kMaxIoPairs);
    }

    std::string why;
    const ProcessingPlan plan = compilePlan(input, &why);
    ASSERT_TRUE(plan.valid) << "trial " << trial << ": " << why;
    ASSERT_EQ(validatePlan(plan, input), "") << "trial " << trial << "\n" << describePlan(plan);

    // Every block of an active row sits in exactly one RunBlocks, no block
    // of an inactive row in any.
    std::array<std::array<int, kGridCols>, kGridRows> runs{};
    for (int i = 0; i < plan.numOps; ++i) {
      const PlanOp& op = plan.ops[static_cast<size_t>(i)];
      if (op.type == PlanOp::RunBlocks)
        for (int slot = op.begin; slot < op.end; ++slot)
          ++runs[op.row][static_cast<size_t>(slot)];
    }
    int active = 0;
    std::array<int, kMaxIoPairs> busArrival{};
    busArrival.fill(-1);
    int anyArrival = -1;
    for (int row = 0; row < kGridRows; ++row) {
      const PlanInput::Row& r = input.rows[static_cast<size_t>(row)];
      const bool routed =
          r.input.type == RowInput::Type::Device && r.outputType == RowOutput::Type::Device;
      const ProcessingPlan::RowInfo& info = plan.rowInfo[static_cast<size_t>(row)];
      ASSERT_EQ(info.active, routed);
      active += routed ? 1 : 0;
      int latency = 0;
      for (int slot = 0; slot < kGridCols; ++slot) {
        ASSERT_EQ(runs[static_cast<size_t>(row)][static_cast<size_t>(slot)], routed ? 1 : 0);
        latency += r.slots[static_cast<size_t>(slot)].latency;
      }
      if (!routed)
        continue;
      // Rows of one bus arrive together, after their pads; with the plugin
      // rule so do all rows.
      ASSERT_GE(info.pad, 0);
      ASSERT_EQ(info.latency, latency);
      int& arrival = busArrival[static_cast<size_t>(info.bus)];
      if (arrival >= 0) {
        ASSERT_EQ(arrival, latency + info.pad) << "trial " << trial;
      }
      arrival = latency + info.pad;
      if (input.io.padAcrossBuses) {
        if (anyArrival >= 0) {
          ASSERT_EQ(anyArrival, arrival);
        }
        anyArrival = arrival;
      }
      ASSERT_LE(arrival, plan.reportedChainLatency);
      flagged += info.inputMissing || info.outputFallback ? 1 : 0;
      // The bus is the row's pair, or the main pair when the device lacks it.
      const int pair = plan.buses[static_cast<size_t>(info.bus)].pair;
      ASSERT_EQ(pair, info.outputFallback ? 0 : r.outputPair);
    }

    // Fork children write disjoint row buffers: one sequence per row.
    ASSERT_EQ(countOps(plan, PlanOp::Fork), active >= 2 ? 1 : 0);
    ASSERT_EQ(countOps(plan, PlanOp::RowIn), active);
    ASSERT_EQ(countOps(plan, PlanOp::RowOut), active);
    ASSERT_EQ(plan.numSeqs, active >= 2 ? active + 1 : 1);
    forked += active >= 2 ? 1 : 0;

    // Ports are dense on both sides, and there is exactly one main strip.
    ASSERT_GE(plan.numStrips, 1);
    ASSERT_GE(plan.numBuses, 1);
    ASSERT_LE(plan.numStrips, kMaxIoPairs);
    ASSERT_LE(plan.numBuses, kMaxIoPairs);
    ASSERT_GE(plan.cpuEstimate, plan.cpuCritical);

    // The latency pass alone is idempotent, and compilation deterministic.
    ProcessingPlan again = plan;
    computePlanLatency(again, input);
    ASSERT_EQ(describePlan(again), describePlan(plan));
    ASSERT_EQ(describePlan(compilePlan(input)), describePlan(plan));
  }
  EXPECT_GT(forked, 1000);
  EXPECT_GT(flagged, 100);
}

// ---------------------------------------------------------------------------
// The grid front-end: splits and row feeds
// ---------------------------------------------------------------------------

namespace {

void addSplit(PlanInput& input, int upperRow, int splitCol, int mixCol) {
  input.splits[static_cast<size_t>(input.numSplits++)] = {upperRow, splitCol, mixCol};
}

void feedRow(PlanInput& input, int from, int to) {
  input.rows[static_cast<size_t>(from)].outputType = RowOutput::Type::Row;
  input.rows[static_cast<size_t>(from)].outputToRow = to;
}

void sendRow(PlanInput& input, int row, int pair) {
  input.rows[static_cast<size_t>(row)].outputType = RowOutput::Type::Device;
  input.rows[static_cast<size_t>(row)].outputPair = pair;
}

// The types of a sequence's operations, as letters: R run, S split, M mix,
// F fork, I row in, O row out, E feed.
std::string seqShape(const ProcessingPlan& plan, int seq) {
  static const char letters[] = {'R', 'C', 'S', 'M', 'E', 'O', 'D', 'F', 'I'};
  std::string shape;
  const PlanSeq& s = plan.seqs[static_cast<size_t>(seq)];
  for (int i = s.firstOp; i < s.firstOp + s.numOps; ++i)
    shape += letters[plan.ops[static_cast<size_t>(i)].type];
  return shape;
}

// How late each row's signal is where it leaves the row, worked out from
// the routing alone (never from a plan): the model the latency pass is held
// against.
struct RoutedLatency {
  const PlanInput& input;
  const ProcessingPlan& plan;  // only for which rows run

  const PlanInput::Split* splitOf(int upper) const {
    for (int i = 0; i < input.numSplits; ++i)
      if (input.splits[static_cast<size_t>(i)].upperRow == upper)
        return &input.splits[static_cast<size_t>(i)];
    return nullptr;
  }
  bool active(int row) const { return plan.rowInfo[static_cast<size_t>(row)].active; }
  int sum(int row, int from, int to) const {
    int total = 0;
    for (int i = from; i < to; ++i)
      total += input.rows[static_cast<size_t>(row)].slots[static_cast<size_t>(i)].latency;
    return total;
  }
  int start(int row) const {
    if (input.rows[static_cast<size_t>(row)].input.type == RowInput::Type::Device)
      return 0;
    if (row > 0 && splitOf(row - 1) != nullptr)
      return at(row - 1, splitOf(row - 1)->splitCol);
    int latest = 0;
    for (int feeder = 0; feeder < kGridRows; ++feeder)
      if (active(feeder) && plan.rowInfo[static_cast<size_t>(feeder)].sink ==
                                ProcessingPlan::RowInfo::ToRow &&
          input.rows[static_cast<size_t>(feeder)].outputToRow == row)
        latest = std::max(latest, end(feeder));
    return latest;
  }
  int firstSlot(int row) const {
    return row > 0 && splitOf(row - 1) != nullptr ? splitOf(row - 1)->splitCol : 0;
  }
  int lastSlot(int row) const {
    if (row > 0 && splitOf(row - 1) != nullptr && splitOf(row - 1)->mixCol >= 0)
      return splitOf(row - 1)->mixCol;
    return kGridCols;
  }
  // How late the row is at boundary `col`, the mixer at that boundary
  // included when `afterMix`.
  int at(int row, int col, bool afterMix = false) const {
    const PlanInput::Split* split = splitOf(row);
    const int first = firstSlot(row);
    if (split == nullptr || split->mixCol < 0 || col < split->mixCol ||
        (col == split->mixCol && !afterMix))
      return start(row) + sum(row, first, col);
    const int upperSide = start(row) + sum(row, first, split->mixCol);
    return std::max(upperSide, end(row + 1)) + sum(row, split->mixCol, col);
  }
  int end(int row) const { return at(row, lastSlot(row), true); }
};

}  // namespace

TEST(GridSplitPlanTest, ARejoinIsSplitForkAlignMix) {
  PlanInput input = gridInput();
  routeRow(input, 0, 0, 0);
  addSplit(input, 0, 2, 6);
  input.rows[0].slots = makeLane(kGridCols, {{0, 10}, {3, 48}, {7, 5}});
  input.rows[1].slots = makeLane(kGridCols, {{1, 999}, {4, 16}, {6, 999}});
  std::string why;
  const ProcessingPlan plan = compilePlan(input, &why);
  ASSERT_TRUE(plan.valid) << why;
  ASSERT_EQ(validatePlan(plan, input), "") << describePlan(plan);

  // RowIn, row 0 up to the fork, Split, Fork, AlignMix, the rest, the send.
  EXPECT_EQ(seqShape(plan, 0), "IRSFMRO") << describePlan(plan);
  ASSERT_EQ(plan.numSeqs, 3);
  EXPECT_EQ(seqShape(plan, 1), "R");
  EXPECT_EQ(seqShape(plan, 2), "R");
  const PlanOp& upper = opAt(plan, 1, 0);
  const PlanOp& lower = opAt(plan, 2, 0);
  EXPECT_EQ(upper.row, 0);
  EXPECT_EQ(upper.begin, 2);
  EXPECT_EQ(upper.end, 6);
  EXPECT_EQ(lower.row, 1);
  EXPECT_EQ(lower.begin, 2);
  EXPECT_EQ(lower.end, 6);
  // The lower row's blocks look on into row 0 from the mix point; the
  // upper row's only along their own row.
  ASSERT_EQ(lower.numCont, 1);
  EXPECT_EQ(lower.cont[0].row, 0);
  EXPECT_EQ(lower.cont[0].begin, 6);
  EXPECT_EQ(lower.cont[0].end, 8);
  EXPECT_EQ(lower.scanBegin, 2);
  EXPECT_EQ(lower.scanEnd, 6);
  EXPECT_EQ(upper.numCont, 0);
  EXPECT_EQ(upper.scanBegin, 0);
  EXPECT_EQ(upper.scanEnd, 8);

  // The slots of row 1 outside the range are not run, whatever they hold.
  EXPECT_TRUE(plan.rowInfo[1].active);
  EXPECT_EQ(plan.rowInfo[1].begin, 2);
  EXPECT_EQ(plan.rowInfo[1].end, 6);
  EXPECT_EQ(plan.rowInfo[1].source, ProcessingPlan::RowInfo::FromSplit);
  EXPECT_EQ(plan.rowInfo[1].sink, ProcessingPlan::RowInfo::ToMix);
  const auto seen = slotCoverage(plan, input);
  for (int slot = 0; slot < kGridCols; ++slot)
    EXPECT_EQ(seen[1][static_cast<size_t>(slot)], slot >= 2 && slot < 6 ? 1 : 0) << slot;

  // A is 48 late and B 16: B waits 32. Then 10 before, 5 after.
  const PlanOp* mix = findOp(plan, PlanOp::AlignMix, 0);
  ASSERT_NE(mix, nullptr);
  EXPECT_EQ(mix->latency, 0);
  EXPECT_EQ(mix->latency2, 32);
  EXPECT_EQ(mix->junction, 0);
  EXPECT_EQ(plan.splitInfo[0].alignB, 32);
  EXPECT_EQ(plan.reportedChainLatency, 63);
  EXPECT_EQ(plan.rowInfo[0].latency, 63);
  EXPECT_EQ(plan.numGroups, 1);
  EXPECT_EQ(plan.rowInfo[0].group, plan.rowInfo[1].group);

  // The other way round, the upper path waits.
  input.rows[1].slots[4].latency = 100;
  const ProcessingPlan later = compilePlan(input);
  EXPECT_EQ(findOp(later, PlanOp::AlignMix, 0)->latency, 52);
  EXPECT_EQ(findOp(later, PlanOp::AlignMix, 0)->latency2, 0);
  EXPECT_EQ(later.reportedChainLatency, 115);
  // The latency pass alone brings a compiled plan up to date.
  ProcessingPlan refreshed = plan;
  EXPECT_NE(validatePlan(refreshed, input), "");
  computePlanLatency(refreshed, input);
  EXPECT_EQ(validatePlan(refreshed, input), "");
  EXPECT_EQ(describePlan(refreshed), describePlan(later));
}

TEST(GridSplitPlanTest, WithoutARejoinTheLowerRowRunsToItsOwnOutput) {
  PlanInput input = gridInput();
  routeRow(input, 0, 0, 0);
  addSplit(input, 0, 3, -1);
  input.rows[0].slots = makeLane(kGridCols, {{5, 40}});
  // No output yet: the lower path leads nowhere. The splitter still runs,
  // because it shapes the upper path too.
  ProcessingPlan plan = compilePlan(input);
  ASSERT_EQ(validatePlan(plan, input), "") << describePlan(plan);
  EXPECT_FALSE(plan.rowInfo[1].active);
  EXPECT_FALSE(plan.splitInfo[0].lowerActive);
  EXPECT_EQ(seqShape(plan, 0), "IRSRO");
  EXPECT_EQ(countOps(plan, PlanOp::Fork), 0);

  sendRow(input, 1, 1);
  plan = compilePlan(input);
  ASSERT_EQ(validatePlan(plan, input), "") << describePlan(plan);
  EXPECT_EQ(seqShape(plan, 0), "IRSFOO");
  EXPECT_EQ(countOps(plan, PlanOp::AlignMix), 0);
  EXPECT_EQ(plan.rowInfo[1].begin, 3);
  EXPECT_EQ(plan.rowInfo[1].end, 8);
  EXPECT_EQ(plan.rowInfo[1].sink, ProcessingPlan::RowInfo::ToDevice);
  EXPECT_EQ(plan.numBuses, 2);
  // Two outputs of one split are kept in step whatever their buses.
  EXPECT_FALSE(input.io.padAcrossBuses);
  EXPECT_EQ(plan.rowInfo[1].pad, 40);
  EXPECT_EQ(plan.rowInfo[0].pad, 0);

  // At the very end of the row there is nothing left to fork: no fork op,
  // and the lower row runs no slot.
  input.splits[0].splitCol = 8;
  plan = compilePlan(input);
  ASSERT_EQ(validatePlan(plan, input), "") << describePlan(plan);
  EXPECT_EQ(seqShape(plan, 0), "IRSOO");
  EXPECT_TRUE(plan.rowInfo[1].active);
  EXPECT_EQ(plan.rowInfo[1].begin, 8);
}

TEST(GridSplitPlanTest, ASplitOfTheLowerRowNestsThreeWays) {
  PlanInput input = gridInput();
  routeRow(input, 0, 0, 0);
  addSplit(input, 0, 1, 7);
  addSplit(input, 1, 2, 6);
  input.rows[0].slots = makeLane(kGridCols, {{4, 64}});
  input.rows[1].slots = makeLane(kGridCols, {{3, 100}});
  input.rows[2].slots = makeLane(kGridCols, {{4, 7}});
  const ProcessingPlan plan = compilePlan(input);
  ASSERT_EQ(validatePlan(plan, input), "") << describePlan(plan);
  EXPECT_EQ(countOps(plan, PlanOp::Fork), 2);
  EXPECT_EQ(countOps(plan, PlanOp::Split), 2);
  EXPECT_EQ(countOps(plan, PlanOp::AlignMix), 2);
  EXPECT_EQ(seqShape(plan, 0), "IRSFMRO");
  // The lower path holds the inner split whole.
  EXPECT_EQ(seqShape(plan, 2), "RSFMR");
  EXPECT_EQ(plan.numGroups, 1);
  // Inner: 100 against 7. Outer: 64 against 100.
  EXPECT_EQ(findOp(plan, PlanOp::AlignMix, 1)->latency, 0);
  EXPECT_EQ(findOp(plan, PlanOp::AlignMix, 1)->latency2, 93);
  EXPECT_EQ(findOp(plan, PlanOp::AlignMix, 0)->latency, 36);
  EXPECT_EQ(findOp(plan, PlanOp::AlignMix, 0)->latency2, 0);
  EXPECT_EQ(plan.reportedChainLatency, 100);
  // The lowest row's blocks look on through both mix points.
  const PlanOp* lowest = findOp(plan, PlanOp::RunBlocks, 2);
  ASSERT_NE(lowest, nullptr);
  ASSERT_EQ(lowest->numCont, 2);
  EXPECT_EQ(lowest->cont[0].row, 1);
  EXPECT_EQ(lowest->cont[0].begin, 6);
  EXPECT_EQ(lowest->cont[0].end, 7);
  EXPECT_EQ(lowest->cont[1].row, 0);
  EXPECT_EQ(lowest->cont[1].begin, 7);
  // The critical path is the dearest of the three, not their sum.
  input.rows[0].slots[4].costEstimate = 0.1f;
  input.rows[1].slots[3].costEstimate = 0.3f;
  input.rows[2].slots[4].costEstimate = 0.2f;
  const PlanCpu cpu = computePlanCpu(plan, input);
  EXPECT_NEAR(cpu.total, 0.6f, 1e-6f);
  EXPECT_NEAR(cpu.critical, 0.3f, 1e-6f);
}

TEST(GridSplitPlanTest, FeedersAreAlignedAndRunBeforeTheRowTheyFeed) {
  PlanInput input = gridInput();
  routeRow(input, 0, 0, 0);
  routeRow(input, 1, 1, 0);
  feedRow(input, 0, 2);
  feedRow(input, 1, 2);
  sendRow(input, 2, 0);
  input.rows[0].slots = makeLane(kGridCols, {{2, 30}});
  input.rows[1].slots = makeLane(kGridCols, {{2, 80}});
  input.rows[2].slots = makeLane(kGridCols, {{0, 5}});
  const ProcessingPlan plan = compilePlan(input);
  ASSERT_EQ(validatePlan(plan, input), "") << describePlan(plan);
  // The two feeders side by side, then both fed in, then the fed row.
  EXPECT_EQ(seqShape(plan, 0), "FEERO") << describePlan(plan);
  EXPECT_EQ(plan.numGroups, 1);
  const PlanOp* first = findOp(plan, PlanOp::FeedRow, 0);
  const PlanOp* second = findOp(plan, PlanOp::FeedRow, 1);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_TRUE(first->firstOnBus);
  EXPECT_FALSE(second->firstOnBus);
  EXPECT_EQ(first->latency, 50);
  EXPECT_EQ(second->latency, 0);
  EXPECT_EQ(plan.reportedChainLatency, 85);
  EXPECT_EQ(plan.rowInfo[2].source, ProcessingPlan::RowInfo::FromFeed);
  EXPECT_EQ(plan.rowInfo[0].sink, ProcessingPlan::RowInfo::ToRow);
  EXPECT_EQ(plan.rowInfo[2].upstream, 0b111);
  EXPECT_EQ(plan.rowInfo[0].upstream, 0b001);
  // A feeder's blocks look on into the row it feeds.
  const PlanOp* run = findOp(plan, PlanOp::RunBlocks, 0);
  ASSERT_EQ(run->numCont, 1);
  EXPECT_EQ(run->cont[0].row, 2);
  EXPECT_EQ(run->cont[0].begin, 0);
  EXPECT_EQ(run->cont[0].end, 8);

  // A chain of feeds, 0 into 1 into 2: no fork, three levels.
  PlanInput chain = gridInput();
  routeRow(chain, 0, 0, 0);
  feedRow(chain, 0, 1);
  feedRow(chain, 1, 2);
  sendRow(chain, 2, 0);
  const ProcessingPlan chained = compilePlan(chain);
  ASSERT_EQ(validatePlan(chained, chain), "") << describePlan(chained);
  EXPECT_EQ(seqShape(chained, 0), "IRERERO");
  EXPECT_EQ(findOp(chained, PlanOp::RunBlocks, 0)->numCont, 2);

  // A row that feeds a row which leads nowhere is not run, nor is that row.
  chain.rows[2].outputType = RowOutput::Type::None;
  const ProcessingPlan nowhere = compilePlan(chain);
  ASSERT_EQ(validatePlan(nowhere, chain), "");
  EXPECT_FALSE(nowhere.rowInfo[0].active);
  EXPECT_FALSE(nowhere.rowInfo[1].active);
  EXPECT_EQ(nowhere.numGroups, 0);
}

TEST(GridSplitPlanTest, ADualMonoRowForcesTheLowerRowsFormat) {
  PlanInput input = gridInput();
  routeRow(input, 0, 0, 0, RowFormat::DualMono, true);
  input.rows[1].format = RowFormat::Mono;
  input.rows[2].format = RowFormat::Stereo;
  addSplit(input, 0, 1, 7);
  addSplit(input, 1, 2, 6);
  const ProcessingPlan plan = compilePlan(input);
  ASSERT_EQ(validatePlan(plan, input), "");
  for (int row = 0; row < 3; ++row) {
    EXPECT_EQ(static_cast<RowFormat>(plan.rowInfo[static_cast<size_t>(row)].format),
              RowFormat::DualMono);
    EXPECT_EQ(effectiveRowFormat(input, row), RowFormat::DualMono);
  }
  EXPECT_EQ(effectiveRowFormat(input, 3), RowFormat::Stereo);
  // Otherwise the lower row keeps its own: a mono row under a stereo one.
  input.rows[0].format = RowFormat::Stereo;
  const ProcessingPlan mixed = compilePlan(input);
  EXPECT_EQ(static_cast<RowFormat>(mixed.rowInfo[1].format), RowFormat::Mono);
  EXPECT_EQ(findOp(mixed, PlanOp::Split, 0)->b.numCh, 1);
  EXPECT_EQ(findOp(mixed, PlanOp::Split, 0)->a.numCh, 2);
  EXPECT_EQ(effectiveRowFormat(input, 1), RowFormat::Mono);
}

// Every shape the routing rules forbid is refused, by the compiler and by
// the check the mutators run first, with a reason that names the rows.
TEST(GridSplitPlanTest, RefusesEveryShapeTheRulesForbid) {
  struct Case {
    const char* name;
    std::function<void(PlanInput&)> build;
    const char* reason;
    int badSplit, badFeed;
  };
  const Case cases[] = {
      {"a split of the last row", [](PlanInput& in) { addSplit(in, 3, 2, 6); }, "no row below", 3, -1},
      {"two splits of one row",
       [](PlanInput& in) {
         addSplit(in, 0, 1, 3);
         addSplit(in, 0, 4, 6);
       },
       "already has a split", 0, -1},
      {"a lower row with an input of its own",
       [](PlanInput& in) {
         routeRow(in, 1, 1, 0);
         addSplit(in, 0, 2, 6);
       },
       "row 1 has an input of its own", 0, -1},
      {"a fork before the row", [](PlanInput& in) { addSplit(in, 0, -1, 4); }, "boundaries run from 0 to 8", 0, -1},
      {"a fork past the row", [](PlanInput& in) { addSplit(in, 0, 9, -1); }, "boundaries run from 0 to 8", 0, -1},
      {"a rejoin at the fork", [](PlanInput& in) { addSplit(in, 0, 4, 4); }, "rejoin at a later one", 0, -1},
      {"a rejoin before the fork", [](PlanInput& in) { addSplit(in, 0, 4, 2); }, "rejoin at a later one", 0, -1},
      {"a rejoin past the row", [](PlanInput& in) { addSplit(in, 0, 4, 9); }, "rejoin at a later one", 0, -1},
      {"a nested split that forks before its parent",
       [](PlanInput& in) {
         addSplit(in, 0, 3, 6);
         addSplit(in, 1, 2, 5);
       },
       "inside the split above it", 1, -1},
      {"a nested split that rejoins after its parent",
       [](PlanInput& in) {
         addSplit(in, 0, 3, 6);
         addSplit(in, 1, 4, 7);
       },
       "inside the split above it", 1, -1},
      {"a nested split that forks after its parent has rejoined",
       [](PlanInput& in) {
         addSplit(in, 0, 1, 3);
         addSplit(in, 1, 5, -1);
       },
       "inside the split above it", 1, -1},
      {"a nested split before a parent that does not rejoin",
       [](PlanInput& in) {
         addSplit(in, 0, 4, -1);
         addSplit(in, 1, 2, 6);
       },
       "inside the split above it", 1, -1},
      {"a row that feeds itself", [](PlanInput& in) { feedRow(in, 0, 0); }, "cannot feed itself", -1, 0},
      {"a feed to no row", [](PlanInput& in) { feedRow(in, 0, 7); }, "does not have", -1, 0},
      {"a feed to a row with an input",
       [](PlanInput& in) {
         routeRow(in, 2, 1, 0);
         feedRow(in, 0, 2);
       },
       "row 2 has an input of its own", -1, 0},
      {"a feed to the lower row of a split",
       [](PlanInput& in) {
         addSplit(in, 0, 2, 6);
         feedRow(in, 2, 1);
       },
       "lower row of a split", -1, 2},
      {"two rows that feed each other",
       [](PlanInput& in) {
         in.rows[0].input.type = RowInput::Type::None;
         feedRow(in, 0, 1);
         feedRow(in, 1, 0);
       },
       "loop", -1, 1},
      {"a loop of three",
       [](PlanInput& in) {
         in.rows[0].input.type = RowInput::Type::None;
         feedRow(in, 0, 1);
         feedRow(in, 1, 2);
         feedRow(in, 2, 0);
       },
       "loop", -1, 2},
      {"a loop through a split",
       [](PlanInput& in) {
         in.rows[0].input.type = RowInput::Type::None;
         addSplit(in, 0, 2, -1);
         feedRow(in, 1, 0);
       },
       "loop", -1, 1},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    PlanInput input = gridInput();
    routeRow(input, 0, 0, 0);
    c.build(input);
    std::string why;
    const ProcessingPlan plan = compilePlan(input, &why);
    EXPECT_FALSE(plan.valid);
    EXPECT_NE(why.find(c.reason), std::string::npos) << why;
    int badSplit = -2, badFeed = -2;
    EXPECT_EQ(validateGridRouting(input, &badSplit, &badFeed), why);
    EXPECT_EQ(badSplit, c.badSplit);
    EXPECT_EQ(badFeed, c.badFeed);
    // A plan compiled before the rig took this shape no longer holds.
    PlanInput before = gridInput();
    routeRow(before, 0, 0, 0);
    EXPECT_NE(validatePlan(compilePlan(before), input), "");
  }

  // What the rules allow, for contrast: the output of a row that ends in a
  // mixer is not used, so it declares no feed; a nested split may fork where
  // its parent rejoins; and a fed row may itself be split.
  PlanInput input = gridInput();
  routeRow(input, 0, 0, 0);
  addSplit(input, 0, 2, 6);
  feedRow(input, 1, 1);  // dormant: row 1 ends in the mixer
  EXPECT_EQ(validateGridRouting(input), "");
  addSplit(input, 1, 6, -1);
  EXPECT_EQ(validateGridRouting(input), "");
  input = gridInput();
  routeRow(input, 0, 0, 0);
  feedRow(input, 0, 1);
  sendRow(input, 1, 0);
  addSplit(input, 1, 3, 5);
  EXPECT_EQ(validateGridRouting(input), "");
  EXPECT_EQ(validatePlan(compilePlan(input), input), "");
}

TEST(GridSplitPlanTest, ValidateNamesBrokenSplitAndFeedPlans) {
  PlanInput input = gridInput();
  routeRow(input, 0, 0, 0);
  addSplit(input, 0, 2, 6);
  feedRow(input, 0, 2);
  sendRow(input, 2, 0);
  input.rows[0].slots = makeLane(kGridCols, {{3, 20}});
  const ProcessingPlan good = compilePlan(input);
  ASSERT_EQ(validatePlan(good, input), "") << describePlan(good);
  const auto indexOf = [&](PlanOp::Type type) {
    for (int i = 0; i < good.numOps; ++i)
      if (good.ops[static_cast<size_t>(i)].type == type)
        return i;
    return -1;
  };

  // An alignment that no longer evens the two paths out.
  ProcessingPlan broken = good;
  broken.ops[static_cast<size_t>(indexOf(PlanOp::AlignMix))].latency2 = 0;
  EXPECT_NE(validatePlan(broken, input).find("meet unequally late"), std::string::npos);
  // A mixer that never runs, or runs for a split that is not there.
  broken = good;
  broken.ops[static_cast<size_t>(indexOf(PlanOp::AlignMix))].type = PlanOp::Split;
  EXPECT_NE(validatePlan(broken, input), "");
  broken = good;
  broken.ops[static_cast<size_t>(indexOf(PlanOp::AlignMix))].row = 1;
  broken.ops[static_cast<size_t>(indexOf(PlanOp::AlignMix))].row2 = 2;
  EXPECT_NE(validatePlan(broken, input).find("does not have"), std::string::npos);
  // The wrong junction.
  broken = good;
  broken.ops[static_cast<size_t>(indexOf(PlanOp::Split))].junction = 1;
  EXPECT_NE(validatePlan(broken, input).find("wrong junction"), std::string::npos);
  // Both sides of the fork writing the upper row's buffer.
  broken = good;
  for (int i = 0; i < broken.numOps; ++i)
    if (broken.ops[static_cast<size_t>(i)].type == PlanOp::RunBlocks && broken.ops[static_cast<size_t>(i)].row == 1)
      broken.ops[static_cast<size_t>(i)].a.index = 0;
  EXPECT_NE(validatePlan(broken, input).find("write the same buffer"), std::string::npos);
  // A lower row run past its range.
  broken = good;
  for (int i = 0; i < broken.numOps; ++i)
    if (broken.ops[static_cast<size_t>(i)].type == PlanOp::RunBlocks && broken.ops[static_cast<size_t>(i)].row == 1)
      broken.ops[static_cast<size_t>(i)].end = 8;
  EXPECT_NE(validatePlan(broken, input), "");
  // A feed that adds to a buffer nobody started, or into the wrong row.
  broken = good;
  broken.ops[static_cast<size_t>(indexOf(PlanOp::FeedRow))].firstOnBus = false;
  EXPECT_NE(validatePlan(broken, input).find("not started by its first feed"), std::string::npos);
  broken = good;
  broken.ops[static_cast<size_t>(indexOf(PlanOp::FeedRow))].row2 = 3;
  EXPECT_NE(validatePlan(broken, input).find("does not feed"), std::string::npos);
  // A split the rig has since moved.
  PlanInput moved = input;
  moved.splits[0].mixCol = 5;
  EXPECT_NE(validatePlan(good, moved), "");
  moved = input;
  moved.numSplits = 0;
  EXPECT_NE(validatePlan(good, moved), "");
}

// Random grids with splits and feeds. Those the rules forbid are refused
// with a reason; every other one compiles to a plan that holds, runs exactly
// the slots of each row's range, and puts every path where the routing says
// it has to be, by a model of the latency that never looks at the plan.
TEST(GridSplitPlanTest, FuzzedSplitsAndFeedsCompileToPlansThatHold) {
  std::mt19937 rng(20261011);
  const auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); };
  int valid = 0, refused = 0, nested = 0, fed = 0, rejoined = 0, forkedGroups = 0, threeWay = 0;
  for (int trial = 0; trial < 20000; ++trial) {
    PlanInput input = gridInput();
    input.topologyRevision = static_cast<std::uint32_t>(trial);
    input.io.padAcrossBuses = pick(2) == 0;
    for (int row = 0; row < kGridRows; ++row) {
      PlanInput::Row& r = input.rows[static_cast<size_t>(row)];
      r.format = static_cast<RowFormat>(pick(3));
      for (auto& slot : r.slots) {
        slot.isBlock = pick(3) == 0;
        slot.latency = slot.isBlock && pick(3) == 0 ? pick(300) : 0;
        slot.costEstimate = slot.isBlock ? 0.01f * static_cast<float>(pick(20)) : 0.0f;
      }
      r.input.type = pick(3) == 0 ? RowInput::Type::Device : RowInput::Type::None;
      r.input.channel = pick(kMaxIoChannels);
      const int out = pick(6);
      r.outputType = out == 0 ? RowOutput::Type::None
                         : out <= 3 ? RowOutput::Type::Device : RowOutput::Type::Row;
      r.outputPair = pick(kMaxIoPairs);
      r.outputToRow = pick(kGridRows);
    }
    // Half the trials draw their splits and feeds blind (most of those are
    // refused); the other half draw them the way a user would, so that the
    // deep shapes come up often: a lower row has no input, a rejoin lies
    // after its fork, a nested split inside its parent, a feed goes to a
    // row without an input.
    const bool guided = trial % 2 == 1;
    int parentFrom = 0, parentTo = kGridCols;
    bool parentSplit = false;
    for (int upper = 0; upper < kGridRows - 1; ++upper) {
      if (pick(2) != 0) {
        parentSplit = false;
        continue;
      }
      int splitCol = pick(kGridCols + 1);
      int mixCol = pick(3) == 0 ? -1 : splitCol + 1 + pick(kGridCols);
      if (guided) {
        const int from = parentSplit ? parentFrom : 0;
        const int to = parentSplit ? parentTo : kGridCols;
        splitCol = from + pick(to - from + 1);
        mixCol = splitCol < to && pick(4) != 0 ? splitCol + 1 + pick(to - splitCol) : -1;
        input.rows[static_cast<size_t>(upper + 1)].input.type = RowInput::Type::None;
      }
      addSplit(input, upper, splitCol, mixCol);
      parentSplit = true;
      parentFrom = splitCol;
      parentTo = mixCol < 0 ? kGridCols : mixCol;
    }
    if (guided)
      for (int row = 0; row < kGridRows; ++row) {
        PlanInput::Row& r = input.rows[static_cast<size_t>(row)];
        if (r.outputType == RowOutput::Type::Row && pick(4) != 0)
          for (int tries = 0; tries < 4; ++tries) {
            const int target = pick(kGridRows);
            if (target != row && input.rows[static_cast<size_t>(target)].input.type == RowInput::Type::None) {
              r.outputToRow = target;
              break;
            }
          }
      }

    std::string why;
    const ProcessingPlan plan = compilePlan(input, &why);
    const std::string fault = validateGridRouting(input);
    if (!plan.valid) {
      ++refused;
      ASSERT_FALSE(why.empty());
      ASSERT_EQ(fault, why) << "trial " << trial;
      continue;
    }
    ++valid;
    ASSERT_EQ(fault, "") << "trial " << trial;
    ASSERT_EQ(validatePlan(plan, input), "") << "trial " << trial << "\n" << describePlan(plan);

    // Slots: exactly the range of every active row, nothing else.
    std::array<std::array<int, kGridCols>, kGridRows> runs{};
    for (int i = 0; i < plan.numOps; ++i) {
      const PlanOp& op = plan.ops[static_cast<size_t>(i)];
      if (op.type == PlanOp::RunBlocks)
        for (int slot = op.begin; slot < op.end; ++slot)
          ++runs[op.row][static_cast<size_t>(slot)];
    }
    const RoutedLatency model{input, plan};
    std::array<int, kMaxIoPairs> busArrival{};
    busArrival.fill(-1);
    std::array<int, kGridRows> groupArrival{};
    groupArrival.fill(-1);
    int latest = 0, activeRows = 0;
    for (int row = 0; row < kGridRows; ++row) {
      const ProcessingPlan::RowInfo& info = plan.rowInfo[static_cast<size_t>(row)];
      const PlanInput::Row& r = input.rows[static_cast<size_t>(row)];
      for (int slot = 0; slot < kGridCols; ++slot)
        ASSERT_EQ(runs[static_cast<size_t>(row)][static_cast<size_t>(slot)],
                  info.active && slot >= model.firstSlot(row) && slot < model.lastSlot(row) ? 1 : 0)
            << "trial " << trial << " row " << row << " slot " << slot << "\n" << describePlan(plan);
      if (!info.active)
        continue;
      ++activeRows;
      // A row runs only when something reaches it and it reaches an output.
      const bool lower = row > 0 && model.splitOf(row - 1) != nullptr;
      ASSERT_TRUE(r.input.type == RowInput::Type::Device || lower ||
                  info.source == ProcessingPlan::RowInfo::FromFeed);
      if (lower) {
        ASSERT_TRUE(plan.rowInfo[static_cast<size_t>(row - 1)].active);
        ASSERT_EQ(r.input.type, RowInput::Type::None);
      }
      ASSERT_EQ(info.latency, model.end(row)) << "trial " << trial << " row " << row << "\n"
                                              << describePlan(plan);
      ASSERT_GE(info.pad, 0);
      if (info.sink == ProcessingPlan::RowInfo::ToDevice) {
        const int arrives = info.latency + info.pad;
        int& bus = busArrival[static_cast<size_t>(info.bus)];
        if (bus >= 0) {
          ASSERT_EQ(bus, arrives) << "trial " << trial;
        }
        bus = arrives;
        int& group = groupArrival[static_cast<size_t>(info.group)];
        if (group >= 0) {
          ASSERT_EQ(group, arrives) << "trial " << trial;
        }
        group = arrives;
        latest = std::max(latest, info.latency);
        ASSERT_LE(arrives, plan.reportedChainLatency);
      } else if (info.sink == ProcessingPlan::RowInfo::ToMix) {
        // Both paths of the mixer are as late as the later one.
        const ProcessingPlan::SplitInfo& split = plan.splitInfo[static_cast<size_t>(row - 1)];
        const int upperSide = model.at(row - 1, split.mixCol, false);
        ASSERT_EQ(upperSide + split.alignA, info.latency + split.alignB);
        ASSERT_EQ(std::min(split.alignA, split.alignB), 0);
        ++rejoined;
      } else {
        ASSERT_EQ(info.sink, ProcessingPlan::RowInfo::ToRow);
        // Every feeder of a row arrives when the latest does.
        ASSERT_EQ(info.latency + info.pad, model.start(info.feedsRow)) << "trial " << trial;
        ++fed;
      }
      if (lower && row > 1 && model.splitOf(row - 2) != nullptr)
        ++nested;
    }
    ASSERT_EQ(plan.reportedChainLatency, latest);
    if (input.io.padAcrossBuses) {
      int any = -1;
      for (const int arrival : busArrival)
        if (arrival >= 0) {
          if (any >= 0) {
            ASSERT_EQ(any, arrival);
          }
          any = arrival;
        }
    }
    ASSERT_GE(plan.cpuEstimate, plan.cpuCritical);
    ASSERT_LE(plan.numGroups, activeRows);
    forkedGroups += plan.numGroups >= 2 ? 1 : 0;
    threeWay += countOps(plan, PlanOp::AlignMix) >= 2 ? 1 : 0;

    // The latency pass alone is idempotent, and compilation deterministic.
    ProcessingPlan again = plan;
    computePlanLatency(again, input);
    ASSERT_EQ(describePlan(again), describePlan(plan));
    ASSERT_EQ(describePlan(compilePlan(input)), describePlan(plan));
  }
  std::cout << "  fuzzed grids: " << valid << " valid, " << refused << " refused; rows that rejoin "
            << rejoined << ", feed " << fed << ", nest " << nested << "; plans with two mixers "
            << threeWay << ", with several groups " << forkedGroups << "\n";
  EXPECT_GT(valid, 3000);
  EXPECT_GT(refused, 3000);
  EXPECT_GT(rejoined, 1000);
  EXPECT_GT(fed, 300);
  EXPECT_GT(nested, 300);
  EXPECT_GT(threeWay, 150);
  EXPECT_GT(forkedGroups, 300);
}


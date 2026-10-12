#include "Processor.h"

#include "FiniteValue.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// ##############################
// GRID MODE
// ##############################
// The rig as a 4 x 8 grid: conversion to and from the legacy rig, the row
// and split mutators, the saved Grid node, the state the commands report
// and what a client that only knows the legacy rig is shown. Which rows run,
// over which slots and in which order is the plan compiler's answer
// (ProcessingPlan.cpp); nothing here second-guesses it.
//
// Threading is that of every chain edit: message thread, chainMutex for
// anything that touches the lanes or the topology, and a ChainEditFade
// around whatever changes which audio goes where. The row mix and the input
// trim are the exception: lock-free stores the audio thread smooths.

namespace {

// The newest Grid node this build reads. 1 is independent rows; 2 adds
// splits and row feeds. A node is written at the lowest format that can
// hold it, so a rig that uses neither is still readable by a build that
// only knows 1.
constexpr int kGridFormatVersion = 2;
constexpr int kGridFormatWithJoins = 2;

// The parameters of the legacy image stage that a grid either binds to rows
// 0 and 1 or holds at neutral (see Processor.h, LegacyControl), with the
// value each rests at.
struct LegacyControlParam {
  const char* id;
  float neutral;
};
constexpr LegacyControlParam kLegacyControlParams[] = {
    {"chainPanLeft", 0.0f},   {"chainPanRight", 1.0f},    {"chainSoloLeft", 0.0f},
    {"chainSoloRight", 0.0f}, {"chainInvertLeft", 0.0f},  {"chainInvertRight", 0.0f},
    {"outputBalance", 0.5f},  {"spreadEnabled", 0.0f},    {"alignEnabled", 0.0f},
};

juce::Result failure(const juce::String& message) { return juce::Result::fail(message); }

bool isInsert(const std::unique_ptr<ChainBlock>& block) {
  return block != nullptr && block->type == ChainBlockType::INSERT;
}

std::unique_ptr<ChainBlock> makeInsert() {
  return std::make_unique<ChainBlock>(juce::Uuid().toString().toStdString(),
                                      ChainBlockType::INSERT);
}

int toneCount(const std::vector<std::unique_ptr<ChainBlock>>& lane) {
  return static_cast<int>(lane.size()) -
         static_cast<int>(std::count_if(lane.begin(), lane.end(), isInsert));
}

const char* formatToString(RowFormat format) {
  switch (format) {
    case RowFormat::Mono: return "mono";
    case RowFormat::DualMono: return "dual";
    case RowFormat::Stereo: break;
  }
  return "stereo";
}

void resetRow(GridRow& row) {
  row.format = RowFormat::Stereo;
  row.input = RowInput{};
  row.output.type = RowOutput::Type::None;
  row.output.pair = 0;
  row.output.toRow = -1;
  row.output.volumeDb.store(0.0f);
  row.output.pan.store(0.5f);
  row.output.mute.store(false);
  row.output.solo.store(false);
  row.output.invert.store(false);
  row.trimDb.store(0.0f);
}

bool rowIsRouted(const GridRow& row) {
  return row.input.type == RowInput::Type::Device && row.output.type == RowOutput::Type::Device;
}

juce::String rowName(int row) { return "row " + juce::String(row + 1); }

// Numbers out of saved state. A file can hold anything: a NaN passes
// straight through juce::jlimit (every comparison with it is false) and
// would reach a gain, and a NaN or an infinity cast to int is undefined. So
// every number of the Grid node is read through these: what is not a finite
// number is the default.
// The mark on a split or a feed that a load found in breach of the routing
// rules and kept without running it. A marked node stays as it is until the
// user replaces it (see reconcileGridFromTree).
const juce::Identifier kSetAsideProperty("setAside");

juce::ValueTree setAsideCopy(const juce::ValueTree& node) {
  juce::ValueTree copy = node.createCopy();
  copy.setProperty(kSetAsideProperty, true, nullptr);
  return copy;
}

// (FiniteValue.h, shared with the legacy restore.)
using t3k::finiteFloat;
using t3k::finiteInt;

// Set a parameter from a real-unit value, leaving it alone when it is
// already there: a round trip through the grid must not move a value by a
// rounding step (the saved bytes are compared).
void setParameterIfMoved(juce::AudioProcessorValueTreeState& parameters, const char* id,
                         float value) {
  if (!std::isfinite(value))
    return;
  if (auto* param = parameters.getParameter(id)) {
    const float normalized = param->convertTo0to1(value);
    if (std::abs(param->getValue() - normalized) > 1.0e-6f)
      param->setValueNotifyingHost(normalized);
  }
}

}  // namespace

// ####################
// BOOKKEEPING
// ####################

void TONE3000Processor::syncGridAtomics() noexcept {
  const bool gridMode = routingMode == RoutingMode::Grid;
  gridModeActive.store(gridMode, std::memory_order_relaxed);
  // The legacy measurements have nothing to measure in a grid; one that was
  // running when the rig became one ends here (and its output mute lifts).
  if (gridMode)
    cancelLegacyMeasurements();
  // The format a row runs in, which a split from a dual-mono row can make
  // dual mono whatever the row's own setting says.
  for (int row = 0; row < kGridRows; ++row)
    gridRowVoices[static_cast<size_t>(row)].store(
        gridMode && rowFormat(row) == RowFormat::DualMono ? 2 : 1, std::memory_order_relaxed);
}

SplitJunction* TONE3000Processor::splitOfRow(int upperRow) const noexcept {
  for (const auto& junction : grid.splits)
    if (junction->upperRow == upperRow)
      return junction.get();
  return nullptr;
}

void TONE3000Processor::refreshSplitTable() noexcept {
  for (int row = 0; row < kGridRows; ++row)
    rtSplits[static_cast<size_t>(row)] = routingMode == RoutingMode::Grid ? splitOfRow(row) : nullptr;
}

void TONE3000Processor::prepareGridSplits() {
  for (const auto& junction : grid.splits)
    junction->prepare(chainSampleRate(), juce::jmax(1, chainDomainBlockSize()));
}

juce::String TONE3000Processor::gridRoutingFault() const {
  if (routingMode != RoutingMode::Grid)
    return {};
  return juce::String(validateGridRouting(makePlanInput()));
}

void TONE3000Processor::padGridRows() {
  for (auto& l : lanes) {
    // Surplus placeholders go first, from the end (a row can only be over
    // its eight slots by placeholders: the mutators refuse a ninth block).
    for (int i = static_cast<int>(l.size()) - 1; i >= 0 && static_cast<int>(l.size()) > kGridCols;
         --i)
      if (isInsert(l[static_cast<size_t>(i)]))
        l.erase(l.begin() + i);
    while (static_cast<int>(l.size()) < kGridCols)
      l.push_back(makeInsert());
  }
}

void TONE3000Processor::sizeGridBuffers() {
  const int frames = juce::jmax(1, chainDomainBlockSize());
  for (auto& work : rowWork) {
    work.setSize(2, frames, false, false, true);
    work.clear();
  }
  for (auto& work : busWork) {
    work.setSize(2, frames, false, false, true);
    work.clear();
  }
}

void TONE3000Processor::prepareGridRows() {
  const double rate = chainSampleRate();
  for (auto& row : grid.rows) {
    // 20 ms, like the legacy image gains.
    row.gainL.reset(rate, 0.02);
    row.gainR.reset(rate, 0.02);
    row.trimGain.reset(rate, 0.02);
  }
  snapGridRowGains();
}

void TONE3000Processor::snapGridRowGains() noexcept {
  const unsigned soloMask = gridSoloMask();
  for (int row = 0; row < kGridRows; ++row) {
    GridRow& gridRow = grid.rows[static_cast<size_t>(row)];
    auto [left, right] = rowSendGains(row, rowSilencedBySolo(row, soloMask));
    // A row that feeds another row has the one gain.
    if (activePlan.rowInfo[static_cast<size_t>(row)].sink == ProcessingPlan::RowInfo::ToRow)
      left = right = rowFeedGain(row);
    gridRow.gainL.setCurrentAndTargetValue(left);
    gridRow.gainR.setCurrentAndTargetValue(right);
    const float trimDb = gridRow.trimDb.load();
    gridRow.trimGain.setCurrentAndTargetValue(
        std::isfinite(trimDb) ? juce::Decibels::decibelsToGain(trimDb) : 1.0f);
  }
}

bool TONE3000Processor::gridPadAcrossBuses() const noexcept {
  if (gridCrossBusPaddingOverride >= 0)
    return gridCrossBusPaddingOverride > 0;
  // A plugin reports one latency for everything it outputs, so every output
  // has to be that late. The standalone app (and a processor outside any
  // wrapper) reports to nobody: there a mic row is not held back by the
  // guitar row's latent blocks unless the two share an output.
  return wrapperType != wrapperType_Standalone && wrapperType != wrapperType_Undefined;
}

void TONE3000Processor::setGridCrossBusPaddingForTesting(int mode) {
  ChainEditFade fade(*this);
  juce::ScopedLock lock(chainMutex);
  gridCrossBusPaddingOverride = mode;
  rebuildTopology();
}

void TONE3000Processor::syncPlanPorts() {
  if (activePlan.numStrips == lastPlanStrips && activePlan.numBuses == lastPlanBuses)
    return;
  lastPlanStrips = activePlan.numStrips;
  lastPlanBuses = activePlan.numBuses;

  // The boundary runs every port on one frame schedule and has to be opened
  // for the counts it will be run with (the audio thread refuses any
  // other). Its latency is the same for every port count.
  if (chainBoundary != nullptr) {
    const bool opened = chainBoundary->reset(
        hostSampleRate, kChainBaseSampleRate, juce::jmax(1, maxBlockSize),
        juce::jlimit(1, ChainBoundaryMulti::kMaxPorts, activePlan.numStrips),
        juce::jlimit(1, ChainBoundaryMulti::kMaxPorts, activePlan.numBuses));
    jassert(opened);
    if (!opened)
      chainBoundary.reset();
  }
  // A port that now carries another strip or bus starts from clean filters.
  for (auto& oversampler : chainOversamplers)
    oversampler.reset();
}

bool TONE3000Processor::boundaryMatchesPlanForTesting() const {
  juce::ScopedLock lock(chainMutex);
  return chainBoundary == nullptr ||
         (chainBoundary->numUpPorts() ==
              juce::jlimit(1, ChainBoundaryMulti::kMaxPorts, activePlan.numStrips) &&
          chainBoundary->numDownPorts() ==
              juce::jlimit(1, ChainBoundaryMulti::kMaxPorts, activePlan.numBuses));
}

void TONE3000Processor::refreshStripPad() {
  const int wanted =
      paramRefs.pitchEnabled->load() >= 0.5f
          ? PitchShift::latencySamples(PitchShift::windowFromIndex(static_cast<int>(
                                           std::lround(paramRefs.pitchWindow->load()))),
                                       hostSampleRate)
          : 0;
  if (wanted == stripPadSamples.load())
    return;
  // Every strip that does not run the shifter waits this long
  // (runInputStrip). A rig whose only strip runs it has nobody waiting (the
  // legacy rig, exactly as it was); otherwise the pads jump: inside the
  // chain mute.
  bool someStripWaits = false;
  {
    juce::ScopedLock lock(chainMutex);
    int pitchStrip = -1;
    for (int s = activePlan.numStrips - 1; s >= 0; --s)
      if (activePlan.strips[static_cast<size_t>(s)].in.pitch)
        pitchStrip = s;
    someStripWaits = activePlan.numStrips > 1 || pitchStrip < 0;
  }
  std::optional<ChainEditFade> fade;
  if (someStripWaits)
    fade.emplace(*this);
  stripPadSamples.store(wanted);
}

juce::var TONE3000Processor::getPlanCpu() const {
  PlanCpu cpu;
  {
    juce::ScopedLock lock(chainMutex);
    cpu = computePlanCpu(activePlan, makePlanInput());
  }
  juce::DynamicObject::Ptr result = new juce::DynamicObject();
  result->setProperty("total", static_cast<double>(cpu.total));
  result->setProperty("critical", static_cast<double>(cpu.critical));
  juce::Array<juce::var> rows;
  for (const float row : cpu.rows)
    rows.add(static_cast<double>(row));
  result->setProperty("rows", rows);
  return juce::var(result.get());
}

double TONE3000Processor::blockCpuCostForTesting(const std::string& blockId) const {
  juce::ScopedLock lock(chainMutex);
  for (const auto& l : lanes)
    for (const auto& block : l)
      if (block->id == blockId)
        return static_cast<double>(block->cpuCost.load());
  return -1.0;
}

// ####################
// LEGACY TO GRID
// ####################

namespace {

// Where the tapped block of a branched trunk lands in a row of eight: its
// lane position, less the empty slots before it that have to go when the
// lane is longer than a row (the surplus ones go from the end first, as in
// padGridRows). -1 when the lane does not hold the block.
int fittedTapColumn(const std::vector<std::unique_ptr<ChainBlock>>& lane, const std::string& tapId) {
  std::vector<int> kept;  // lane indices that survive
  for (int i = 0; i < static_cast<int>(lane.size()); ++i)
    kept.push_back(i);
  for (int i = static_cast<int>(kept.size()) - 1; i >= 0 && static_cast<int>(kept.size()) > kGridCols;
       --i)
    if (isInsert(lane[static_cast<size_t>(kept[static_cast<size_t>(i)])]))
      kept.erase(kept.begin() + i);
  for (int column = 0; column < static_cast<int>(kept.size()); ++column)
    if (lane[static_cast<size_t>(kept[static_cast<size_t>(column)])]->id == tapId)
      return column;
  return -1;
}

}  // namespace

juce::String TONE3000Processor::promoteRefusal() const {
  for (int row = 0; row < kNumLegacyLanes; ++row)
    if (const int tones = toneCount(lanes[static_cast<size_t>(row)]); tones > kGridCols)
      return juce::String(row == 0 ? "the left" : "the right") + " lane holds " +
             juce::String(tones) + " blocks and a grid row holds " + juce::String(kGridCols);
  if (stereoEnabled.load() && !branchAfterBlockId.empty()) {
    // The branch lane becomes the lower row of a split and starts at the
    // split's column: it has the slots from there to the end of the row.
    const Lane& trunk = lane(branchSourceSide);
    const Lane& branchLane =
        lane(branchSourceSide == ChainSide::Left ? ChainSide::Right : ChainSide::Left);
    const int tapColumn = fittedTapColumn(trunk, branchAfterBlockId);
    const int room = kGridCols - (tapColumn + 1);
    if (tapColumn >= 0 && toneCount(branchLane) > room)
      return "the branch lane holds " + juce::String(toneCount(branchLane)) +
             " blocks and only " + juce::String(room) + " slots of a grid row follow the tap";
  }
  return {};
}

// The mapping table of section 3.2.
void TONE3000Processor::promoteToGridLocked(juce::StringArray& lossy) {
  const bool wasStereo = stereoEnabled.load();
  const bool branched = wasStereo && !branchAfterBlockId.empty();
  const InputMode mode = getInputMode();
  const bool stereoIn = stereoInputDetected.load();
  const bool dual = dualMonoEngaged();  // asked while the rig is still legacy
  const float balance = paramRefs.outputBalance->load();
  const bool pitchOn = paramRefs.pitchEnabled->load() >= 0.5f;

  if (!branchAfterBlockId.empty() && !branched)
    lossy.add("the dormant branch is dropped");
  if (!wasStereo && paramRefs.spreadEnabled->load() >= 0.5f)
    lossy.add("Spread does nothing in grid mode and is switched off");
  if (wasStereo && paramRefs.alignEnabled->load() >= 0.5f)
    lossy.add("Align does nothing in grid mode and is switched off");
  if (wasStereo)
    lossy.add("auto-align and auto-balance are not available in grid mode");
  if (dual && std::abs(balance - 0.5f) > 1.0e-4f)
    lossy.add("the output balance of the dual-mono rig is not carried over");

  // What the legacy image controls are now: put back when the rig is a
  // legacy rig again (see holdLegacyParam).
  gridHeldLegacyParams.clear();
  for (const LegacyControlParam& control : kLegacyControlParams)
    gridHeldLegacyParams[control.id] = legacyParamValue(control.id);

  for (auto& row : grid.rows)
    resetRow(row);
  for (int row = 0; row < kGridRows; ++row) {
    gridRawInput[static_cast<size_t>(row)] = {};
    gridRawOutput[static_cast<size_t>(row)] = {};
    gridRawFormat[static_cast<size_t>(row)].clear();
  }
  gridUnknownNodes.clear();
  grid.splits.clear();

  GridRow& first = grid.rows[0];
  GridRow& second = grid.rows[1];
  if (!wasStereo) {
    // One chain. The right lane's blocks stay in row 1, which has neither
    // input nor output: kept, not heard, as they were.
    first.input.type = RowInput::Type::Device;
    first.input.gate = true;
    first.input.pitch = true;
    first.output.type = RowOutput::Type::Device;
    if (!stereoIn || mode == InputMode::Left) {
      first.input.channel = 0;
    } else if (mode == InputMode::Right) {
      first.input.channel = 1;
    } else if (dual) {
      first.input.channel = 0;
      first.input.stereo = true;
      first.format = RowFormat::DualMono;
    } else {
      // Stereo, or dual mono on a rig where it cannot engage: the sum.
      first.input.channel = 0;
      first.input.stereo = true;
      first.input.fold = RowInput::Fold::SumToMono;
    }
  } else {
    // Two mono chains, each panned onto Out 1/2. The balance trim becomes
    // the two row volumes, opposed. With a branch the trunk has to be the
    // upper row, so a trunk on the right swaps the lanes; everything a lane
    // carries (its pan, its share of the balance, its polarity and solo)
    // travels with it, and the audio is the same.
    const bool swapLanes = branched && branchSourceSide == ChainSide::Right;
    if (swapLanes)
      std::swap(lanes[0], lanes[1]);
    const int laneOfRow[2] = {swapLanes ? 1 : 0, swapLanes ? 0 : 1};

    const float trimDb = (balance - 0.5f) * 24.0f;
    // A branched rig has the one source, the trunk's; the input mode says
    // which channel that is.
    const int single = (!stereoIn || mode == InputMode::Left) ? 0 : mode == InputMode::Right ? 1 : 0;
    const int channels[2] = {single, branched || !stereoIn || mode == InputMode::Left ||
                                             mode == InputMode::Right
                                         ? single
                                         : 1};
    const float pans[2] = {paramRefs.chainPanLeft->load(), paramRefs.chainPanRight->load()};
    const bool inverts[2] = {paramRefs.chainInvertLeft->load() >= 0.5f,
                             paramRefs.chainInvertRight->load() >= 0.5f};
    const bool solos[2] = {paramRefs.chainSoloLeft->load() >= 0.5f,
                           paramRefs.chainSoloRight->load() >= 0.5f};
    for (int row = 0; row < 2; ++row) {
      GridRow& gridRow = row == 0 ? first : second;
      const int from = laneOfRow[row];
      gridRow.format = RowFormat::Mono;
      gridRow.input.type = RowInput::Type::Device;
      gridRow.input.channel = channels[row];
      gridRow.input.gate = true;
      // One strip runs the shifter. Two chains on one channel share a
      // strip and are both shifted, as before; on two channels the second
      // one is only kept in step.
      gridRow.input.pitch = row == 0 || channels[0] == channels[1];
      gridRow.output.type = RowOutput::Type::Device;
      gridRow.output.pan.store(juce::jlimit(0.0f, 1.0f, pans[from]));
      gridRow.output.volumeDb.store(from == 0 ? -trimDb : trimDb);
      gridRow.output.invert.store(inverts[from]);
      // A solo on both sides leaves both audible, in either mode.
      gridRow.output.solo.store(solos[from]);
    }
    if (pitchOn && channels[0] != channels[1])
      lossy.add("the pitch shifter runs on the first input only");

    if (branched) {
      // The branch is a split that never rejoins: A/B at unity, forking
      // after the tapped block, the branch lane below it from that column
      // on (where the current UI already draws it).
      Lane& trunk = lanes[0];
      Lane& branchLane = lanes[1];
      for (int i = static_cast<int>(trunk.size()) - 1;
           i >= 0 && static_cast<int>(trunk.size()) > kGridCols; --i)
        if (isInsert(trunk[static_cast<size_t>(i)]))
          trunk.erase(trunk.begin() + i);
      int splitCol = 0;
      for (int i = 0; i < static_cast<int>(trunk.size()); ++i)
        if (trunk[static_cast<size_t>(i)]->id == branchAfterBlockId)
          splitCol = i + 1;
      for (int i = static_cast<int>(branchLane.size()) - 1;
           i >= 0 && static_cast<int>(branchLane.size()) + splitCol > kGridCols; --i)
        if (isInsert(branchLane[static_cast<size_t>(i)]))
          branchLane.erase(branchLane.begin() + i);
      for (int i = 0; i < splitCol; ++i)
        branchLane.insert(branchLane.begin(), makeInsert());

      second.input = RowInput{};  // fed by the split
      auto junction = std::make_unique<SplitJunction>();
      junction->id = juce::Uuid().toString().toStdString();
      junction->upperRow = 0;
      junction->splitCol = splitCol;
      junction->mixCol = -1;
      junction->type = SplitterType::AB;
      junction->prepare(chainSampleRate(), juce::jmax(1, chainDomainBlockSize()));
      grid.splits.push_back(std::move(junction));
      if (swapLanes)
        lossy.add("the trunk was the right lane and is now the upper row: the lanes changed "
                  "places, with their pans");
    }
  }

  // Lane index i is column i. A lane longer than a row sheds placeholders
  // (promoteRefusal has made sure the blocks themselves fit).
  routingMode = RoutingMode::Grid;
  stereoEnabled.store(false);
  branchAfterBlockId.clear();
  branchSourceSide = ChainSide::Left;
  pendingAddSide = ChainSide::Left;
  padGridRows();
  syncGridAtomics();
}

// ####################
// GRID TO LEGACY
// ####################

namespace {

// The one split of a grid that is a legacy branch: row 0 into row 1, A/B at
// unity, mono in and out, never rejoined.
bool isBranchSplit(const SplitJunction& junction) {
  return junction.upperRow == 0 && !junction.hasRejoin() && junction.type == SplitterType::AB &&
         std::abs(junction.getParameter(SplitJunction::kSplitLevelA)) < 1.0e-3f &&
         std::abs(junction.getParameter(SplitJunction::kSplitLevelB)) < 1.0e-3f &&
         junction.getParameter(SplitJunction::kSplitStereo) < 0.5f;
}

// The block a legacy branch would tap for a split at `splitCol`: the last
// block before it (empty slots pass the signal on unchanged).
const ChainBlock* tapBlockFor(const std::vector<std::unique_ptr<ChainBlock>>& row, int splitCol) {
  for (int i = juce::jmin(splitCol, static_cast<int>(row.size())) - 1; i >= 0; --i)
    if (!isInsert(row[static_cast<size_t>(i)]))
      return row[static_cast<size_t>(i)].get();
  return nullptr;
}

}  // namespace

// The legacy input mode a projectable grid amounts to. The one the rig had
// before it became a grid is kept whenever it still means the same thing on
// this device (a mono device makes several modes equivalent), so a round
// trip gives the rig back unchanged.
TONE3000Processor::InputMode TONE3000Processor::projectedInputModeLocked() const {
  const InputMode stored = getInputMode();
  const GridRow& first = grid.rows[0];
  const GridRow& second = grid.rows[1];
  const bool stereoIn = stereoInputDetected.load();
  const bool dualCanEngage = stereoIn && stereoOutputDetected.load();
  const auto keepIf = [stored](std::initializer_list<InputMode> same, InputMode otherwise) {
    for (const InputMode candidate : same)
      if (candidate == stored)
        return stored;
    return otherwise;
  };

  if (splitOfRow(0) != nullptr) {
    // A branch: the one source, never a stereo feed.
    if (first.input.channel == 1)
      return InputMode::Right;
    return stereoIn ? InputMode::Left : keepIf({InputMode::Left, InputMode::Right}, InputMode::Left);
  }
  if (rowIsRouted(second)) {
    if (first.input.channel == 1)
      return InputMode::Right;
    if (second.input.channel == 1)
      return keepIf({InputMode::Stereo, InputMode::DualMono}, InputMode::Stereo);
    return stereoIn ? InputMode::Left
                    : keepIf({InputMode::Stereo, InputMode::Left, InputMode::DualMono},
                             InputMode::Left);
  }
  if (first.format == RowFormat::DualMono)
    return InputMode::DualMono;
  if (first.input.stereo)
    return dualCanEngage ? InputMode::Stereo
                         : keepIf({InputMode::Stereo, InputMode::DualMono}, InputMode::Stereo);
  if (first.input.channel == 1)
    return InputMode::Right;
  return stereoIn ? InputMode::Left
                  : keepIf({InputMode::Stereo, InputMode::Left, InputMode::DualMono},
                           InputMode::Left);
}

// Section 3.3: the grid is a legacy rig when it is one chain (M: row 0
// alone, stereo or dual mono), two mono chains (S: rows 0 and 1, each with
// its input) or a branched pair (B: row 1 fed by a split of row 0 that is
// A/B at unity and never rejoins), all on the first two inputs and Out 1/2,
// with nothing set that the legacy rig has no place for.
juce::StringArray TONE3000Processor::demoteBlockersLocked() const {
  juce::StringArray blockers;
  if (routingMode != RoutingMode::Grid)
    return blockers;

  if (!gridUnknownNodes.empty())
    blockers.add("the grid holds routing this build does not know");
  for (int row = 0; row < kGridRows; ++row) {
    if (gridRawInput[static_cast<size_t>(row)].isValid() ||
        gridRawOutput[static_cast<size_t>(row)].isValid() ||
        gridRawFormat[static_cast<size_t>(row)].isNotEmpty())
      blockers.add(rowName(row) + " has settings this build does not know");
    if (grid.rows[static_cast<size_t>(row)].output.type == RowOutput::Type::Row)
      blockers.add(rowName(row) + " feeds another row");
  }
  for (int row = kNumLegacyLanes; row < kGridRows; ++row) {
    const GridRow& gridRow = grid.rows[static_cast<size_t>(row)];
    if (toneCount(lanes[static_cast<size_t>(row)]) > 0)
      blockers.add(rowName(row) + " holds blocks");
    if (gridRow.input.type != RowInput::Type::None || gridRow.output.type != RowOutput::Type::None)
      blockers.add(rowName(row) + " has an input or an output");
  }

  const GridRow& first = grid.rows[0];
  const GridRow& second = grid.rows[1];
  // The branch shape, when the one split is the kind a legacy branch is.
  const SplitJunction* branch = nullptr;
  for (const auto& junction : grid.splits) {
    if (junction->upperRow != 0)
      blockers.add(rowName(junction->upperRow) + " has a split");
    else if (junction->hasRejoin())
      blockers.add("the split of row 1 rejoins (a legacy branch runs to its own output)");
    else if (!isBranchSplit(*junction))
      blockers.add("the split of row 1 is not an A/B split with both levels at 0 dB");
    else
      branch = junction.get();
  }
  const bool hasSplit = splitOfRow(0) != nullptr;

  if (!rowIsRouted(first))
    blockers.add("row 1 needs a device input and a device output");
  const bool two = rowIsRouted(second) ||
                   (hasSplit && second.output.type == RowOutput::Type::Device);
  if (!two && (second.input.type != RowInput::Type::None ||
               second.output.type != RowOutput::Type::None))
    blockers.add("row 2 has an input without an output, or the reverse");
  if (hasSplit && !two)
    blockers.add("row 2 is fed by a split and has no device output");

  for (int row = 0; row < (two ? 2 : 1); ++row) {
    const GridRow& gridRow = grid.rows[static_cast<size_t>(row)];
    const bool fed = row == 1 && hasSplit;
    if (!rowIsRouted(gridRow) && !fed)
      continue;
    if (gridRow.output.pair != 0)
      blockers.add(rowName(row) + " goes to output pair " + juce::String(gridRow.output.pair + 1));
    if (gridRow.output.mute.load())
      blockers.add(rowName(row) + " is muted");
    if (fed)
      continue;  // the rest is about a row's own input
    if (gridRow.input.channel + (gridRow.input.stereo ? 1 : 0) > 1)
      blockers.add(rowName(row) + " takes an input channel past the first two");
    if (std::abs(gridRow.input.trimDb) > 1.0e-4f)
      blockers.add(rowName(row) + " has an input trim");
    if (!gridRow.input.gate)
      blockers.add(rowName(row) + " has its noise gate switched off");
  }
  if (rowIsRouted(first) && !first.input.pitch)
    blockers.add("row 1 does not run the pitch shifter");

  if (!two && rowIsRouted(first)) {
    if (first.format == RowFormat::Mono)
      blockers.add("a single mono row has no legacy form (the legacy chain is stereo)");
    if (first.format == RowFormat::Stereo && first.input.stereo &&
        first.input.fold != RowInput::Fold::SumToMono)
      blockers.add("a stereo row fed by an unsummed pair has no legacy form");
    if (first.format == RowFormat::DualMono &&
        (!first.input.stereo || first.input.fold != RowInput::Fold::None ||
         first.input.channel != 0))
      blockers.add("a dual-mono row needs the unsummed pair of inputs 1 and 2");
    if (std::abs(first.output.volumeDb.load()) > 1.0e-3f ||
        std::abs(first.output.pan.load() - 0.5f) > 1.0e-4f || first.output.invert.load())
      blockers.add("row 1's volume, pan or polarity is off its default, and a single legacy "
                   "chain has none of them");
  }
  if (two) {
    for (int row = 0; row < 2; ++row) {
      const GridRow& gridRow = grid.rows[static_cast<size_t>(row)];
      if (gridRow.format != RowFormat::Mono)
        blockers.add(rowName(row) + " is not mono (two legacy chains are two mono lanes)");
      if (gridRow.input.stereo && !(row == 1 && hasSplit))
        blockers.add(rowName(row) + " takes a pair of inputs");
    }
    if (!hasSplit && first.input.channel == 1 && second.input.channel == 0)
      blockers.add("the rows take the two inputs crossed");
    const float volumeA = first.output.volumeDb.load();
    const float volumeB = second.output.volumeDb.load();
    if (std::abs(volumeA + volumeB) > 1.0e-3f || std::abs(volumeB) > 12.0f + 1.0e-3f)
      blockers.add("the row volumes are not an opposed pair within 12 dB (the legacy balance)");
    if (!hasSplit && second.input.pitch != (first.input.channel == second.input.channel))
      blockers.add("row 2's pitch switch differs from what two legacy chains do");
  }
  if (branch != nullptr) {
    // A legacy branch taps a block of the trunk, and the branch lane starts
    // there: nothing of it can sit before the tap.
    if (tapBlockFor(lanes[0], branch->splitCol) == nullptr)
      blockers.add("the split forks before the first block of row 1 (a legacy branch taps a "
                   "block)");
    for (int i = 0; i < branch->splitCol && i < static_cast<int>(lanes[1].size()); ++i)
      if (!isInsert(lanes[1][static_cast<size_t>(i)])) {
        blockers.add("row 2 holds blocks before the split");
        break;
      }
  }
  return blockers;
}

juce::StringArray TONE3000Processor::legacyProjectionBlockers() const {
  juce::ScopedLock lock(chainMutex);
  return demoteBlockersLocked();
}

void TONE3000Processor::demoteToLegacyLocked() {
  const InputMode mode = projectedInputModeLocked();
  const GridRow& first = grid.rows[0];
  const GridRow& second = grid.rows[1];
  const SplitJunction* branch = splitOfRow(0);
  const bool two = rowIsRouted(second) || branch != nullptr;

  // What the legacy image controls were before the rig became a grid comes
  // back first; the two rows' own values then go on top of it.
  releaseHeldLegacyParams();
  if (two) {
    // What the two rows carried goes back onto the parameters the legacy
    // image stage reads (written once the lock is released, see
    // flushLegacyParamWrites).
    queueLegacyParamWrite("chainPanLeft", first.output.pan.load());
    queueLegacyParamWrite("chainPanRight", second.output.pan.load());
    queueLegacyParamWrite("outputBalance", 0.5f + second.output.volumeDb.load() / 24.0f);
    queueLegacyParamWrite("chainInvertLeft", first.output.invert.load() ? 1.0f : 0.0f);
    queueLegacyParamWrite("chainInvertRight", second.output.invert.load() ? 1.0f : 0.0f);
    queueLegacyParamWrite("chainSoloLeft", first.output.solo.load() ? 1.0f : 0.0f);
    queueLegacyParamWrite("chainSoloRight", second.output.solo.load() ? 1.0f : 0.0f);
  }

  // The branch: the trunk is row 0, the left lane; the tap is the last
  // block before the split; the branch lane is row 1 from the split's
  // column on (the slots before it are empty, a blocker otherwise).
  std::string tapId;
  if (branch != nullptr) {
    if (const ChainBlock* tap = tapBlockFor(lanes[0], branch->splitCol))
      tapId = tap->id;
    Lane& branchLane = lanes[1];
    const int leading = juce::jmin(branch->splitCol, static_cast<int>(branchLane.size()));
    branchLane.erase(branchLane.begin(), branchLane.begin() + leading);
  }

  // Rows 2 and 3 hold placeholders only (a blocker otherwise) and do not
  // exist in a legacy rig.
  for (int row = kNumLegacyLanes; row < kGridRows; ++row)
    lanes[static_cast<size_t>(row)].clear();

  routingMode = RoutingMode::Legacy;
  stereoEnabled.store(two);
  inputMode.store(static_cast<int>(mode));
  branchAfterBlockId = tapId;
  branchSourceSide = ChainSide::Left;
  pendingAddSide = ChainSide::Left;
  for (auto& row : grid.rows)
    resetRow(row);
  grid.splits.clear();
  refreshSplitTable();
  gridLegacyControls.store(0);
  syncGridAtomics();
}

// ####################
// WHAT A LEGACY CLIENT IS SHOWN
// ####################

// The lanes of a legacy rig hold their blocks, then just enough empty slots
// to make kMinLaneSlots, and always one (normalizeLaneInserts). A row holds
// eight slots. So the lane a row projects to is the row with its surplus
// empty slots taken from the end, exactly as a conversion would leave it;
// the slots that stay are real ones, with their ids. (A row with no empty
// slot at all projects without one: the conversion would add it.)
static std::vector<ChainBlock*> projectLane(
    const std::vector<std::unique_ptr<ChainBlock>>& row, int firstSlot, int minSlots) {
  std::vector<ChainBlock*> lane;
  int tones = 0;
  for (int i = juce::jmax(0, firstSlot); i < static_cast<int>(row.size()); ++i) {
    lane.push_back(row[static_cast<size_t>(i)].get());
    tones += row[static_cast<size_t>(i)]->type != ChainBlockType::INSERT ? 1 : 0;
  }
  const int wantedInserts = juce::jmax(minSlots - tones, 1);
  int inserts = static_cast<int>(lane.size()) - tones;
  for (int i = static_cast<int>(lane.size()) - 1; i >= 0 && inserts > wantedInserts; --i)
    if (lane[static_cast<size_t>(i)]->type == ChainBlockType::INSERT) {
      lane.erase(lane.begin() + i);
      --inserts;
    }
  return lane;
}

TONE3000Processor::LegacyProjection TONE3000Processor::projectGridToLegacy() const {
  LegacyProjection projection;
  if (routingMode != RoutingMode::Grid || !demoteBlockersLocked().isEmpty())
    return projection;
  projection.projectable = true;
  projection.inputMode = projectedInputModeLocked();

  const SplitJunction* branch = splitOfRow(0);
  projection.stereo = rowIsRouted(grid.rows[1]) || branch != nullptr;
  projection.dualMono = !projection.stereo && grid.rows[0].format == RowFormat::DualMono &&
                        stereoInputDetected.load() && stereoOutputDetected.load();
  projection.left = projectLane(lanes[0], 0, kMinLaneSlots);
  // The right lane is listed whether or not it is heard, as in a legacy
  // rig (the client shows it in stereo mode only).
  projection.right = projectLane(lanes[1], branch != nullptr ? branch->splitCol : 0, kMinLaneSlots);
  if (branch != nullptr) {
    projection.branchSide = "left";
    if (const ChainBlock* tap = tapBlockFor(lanes[0], branch->splitCol))
      projection.branchAfterBlockId = juce::String(tap->id);
    // With a branch the lanes' visible ends are kept even: the branch lane
    // sheds trailing empty slots past the trunk's end, down to one (see
    // rebuildTopology).
    int tapIndex = 0;
    for (int i = 0; i < static_cast<int>(projection.left.size()); ++i)
      if (juce::String(projection.left[static_cast<size_t>(i)]->id) == projection.branchAfterBlockId)
        tapIndex = i;
    const int targetSlots = static_cast<int>(projection.left.size()) - (tapIndex + 1);
    int inserts = 0;
    for (const ChainBlock* block : projection.right)
      inserts += block->type == ChainBlockType::INSERT ? 1 : 0;
    while (static_cast<int>(projection.right.size()) > targetSlots && inserts > 1 &&
           projection.right.back()->type == ChainBlockType::INSERT) {
      projection.right.pop_back();
      --inserts;
    }
  }
  return projection;
}

bool TONE3000Processor::leaveGridForLaneEdit() {
  if (!isGridMode())
    return true;
  return static_cast<bool>(convertRouting(false)["ok"]);
}

// ####################
// THE LEGACY IMAGE CONTROLS IN A GRID
// ####################

// What a legacy image control is, counting a write that is queued and not
// made yet. Caller holds chainMutex.
float TONE3000Processor::legacyParamValue(const juce::String& id) const {
  for (auto it = pendingLegacyParamWrites.rbegin(); it != pendingLegacyParamWrites.rend(); ++it)
    if (it->first == id)
      return it->second;
  if (auto* param = parameters.getParameter(id))
    return param->convertFrom0to1(param->getValue());
  return 0.0f;
}

int TONE3000Processor::legacyControlsBoundLocked() const noexcept {
  if (routingMode != RoutingMode::Grid)
    return 0;
  // Two mono rows on device outputs are placed by the same pan law the
  // legacy image stage uses, so rows 0 and 1 are the left and right chain
  // of those controls one to one.
  for (int row = 0; row < 2; ++row) {
    const ProcessingPlan::RowInfo& info = activePlan.rowInfo[static_cast<size_t>(row)];
    if (!info.active || info.sink != ProcessingPlan::RowInfo::ToDevice ||
        static_cast<RowFormat>(info.format) != RowFormat::Mono)
      return 0;
  }
  int bound = kLegacyPans | kLegacySolos | kLegacyInverts;
  const float volumeA = grid.rows[0].output.volumeDb.load();
  const float volumeB = grid.rows[1].output.volumeDb.load();
  if (std::abs(volumeA + volumeB) <= 1.0e-3f && std::abs(volumeB) <= 12.0f + 1.0e-3f)
    bound |= kLegacyBalance;
  return bound;
}

void TONE3000Processor::syncLegacyControlsFromGrid() {
  if (routingMode != RoutingMode::Grid)
    return;
  const int bound = legacyControlsBoundLocked();
  gridLegacyControls.store(bound);

  // A rig that became a grid without a conversion (a loaded state from
  // before these were kept) records what the controls are at this point.
  for (const LegacyControlParam& control : kLegacyControlParams)
    if (gridHeldLegacyParams.find(control.id) == gridHeldLegacyParams.end())
      gridHeldLegacyParams[control.id] = legacyParamValue(control.id);

  const RowOutput& first = grid.rows[0].output;
  const RowOutput& second = grid.rows[1].output;
  const auto value = [&](const LegacyControlParam& control) {
    const juce::String id(control.id);
    if ((bound & kLegacyPans) != 0 && id == "chainPanLeft") return first.pan.load();
    if ((bound & kLegacyPans) != 0 && id == "chainPanRight") return second.pan.load();
    if ((bound & kLegacySolos) != 0 && id == "chainSoloLeft") return first.solo.load() ? 1.0f : 0.0f;
    if ((bound & kLegacySolos) != 0 && id == "chainSoloRight") return second.solo.load() ? 1.0f : 0.0f;
    if ((bound & kLegacyInverts) != 0 && id == "chainInvertLeft")
      return first.invert.load() ? 1.0f : 0.0f;
    if ((bound & kLegacyInverts) != 0 && id == "chainInvertRight")
      return second.invert.load() ? 1.0f : 0.0f;
    if ((bound & kLegacyBalance) != 0 && id == "outputBalance")
      return 0.5f + second.volumeDb.load() / 24.0f;
    return control.neutral;  // held: Spread and Align always, the others when unbound
  };
  for (const LegacyControlParam& control : kLegacyControlParams)
    queueLegacyParamWrite(control.id, value(control));
}

// A parameter write is a host notification, and a host may take its time
// over one, or call back in. So none is made under chainMutex, where the
// audio thread would wait for it: what the rig wants the legacy image
// controls to be is queued under the lock and written by whoever took the
// lock once it has let go of it.
void TONE3000Processor::queueLegacyParamWrite(const juce::String& id, float value) {
  // Nothing to write when the control is there already (also counting what
  // is queued): most edits move none of them.
  if (!std::isfinite(value))
    return;
  // Compared as the parameter would keep it (it has a step), or a value
  // between two steps would be written again on every edit.
  if (auto* param = parameters.getParameter(id))
    value = param->getNormalisableRange().snapToLegalValue(value);
  if (std::abs(legacyParamValue(id) - value) <= 1.0e-6f)
    return;
  pendingLegacyParamWrites.emplace_back(id, value);
  legacyParamWritesQueued.store(true);
  // The safety net for a path that does not flush itself.
  triggerAsyncUpdate();
}

void TONE3000Processor::flushLegacyParamWrites() {
  // Nothing queued (every edit of a legacy rig): not even the lock is
  // taken. A restore's caller comes here between releasing the lock and
  // deciding how long to hold its mute, and must not hand the lock to a
  // loader thread in between for nothing.
  if (!legacyParamWritesQueued.exchange(false))
    return;
  std::vector<std::pair<juce::String, float>> writes;
  {
    juce::ScopedLock lock(chainMutex);
    writes.swap(pendingLegacyParamWrites);
  }
  if (writes.empty())
    return;
  // Not moves of a control: they are not to be carried back onto the rows
  // (legacyControlMoved).
  legacyControlSyncDepth.fetch_add(1);
  for (const auto& [id, value] : writes)
    setParameterIfMoved(parameters, id.toRawUTF8(), value);
  legacyControlSyncDepth.fetch_sub(1);
}

void TONE3000Processor::legacyControlMoved(const juce::String& parameterId, float newValue) {
  if (!std::isfinite(newValue))
    return;
  const int bound = gridLegacyControls.load(std::memory_order_relaxed);
  RowOutput& first = grid.rows[0].output;
  RowOutput& second = grid.rows[1].output;
  bool held = false;
  if (parameterId == "chainPanLeft" || parameterId == "chainPanRight") {
    if ((bound & kLegacyPans) != 0)
      (parameterId == "chainPanLeft" ? first : second).pan.store(juce::jlimit(0.0f, 1.0f, newValue));
    else
      held = true;
  } else if (parameterId == "chainSoloLeft" || parameterId == "chainSoloRight") {
    if ((bound & kLegacySolos) != 0)
      (parameterId == "chainSoloLeft" ? first : second).solo.store(newValue >= 0.5f);
    else
      held = true;
  } else if (parameterId == "chainInvertLeft" || parameterId == "chainInvertRight") {
    if ((bound & kLegacyInverts) != 0)
      (parameterId == "chainInvertLeft" ? first : second).invert.store(newValue >= 0.5f);
    else
      held = true;
  } else if (parameterId == "outputBalance") {
    if ((bound & kLegacyBalance) != 0) {
      // The legacy balance: up to 12 dB, opposed between the two chains.
      const float trimDb = (juce::jlimit(0.0f, 1.0f, newValue) - 0.5f) * 24.0f;
      first.volumeDb.store(-trimDb);
      second.volumeDb.store(trimDb);
    } else {
      held = true;
    }
  } else if (parameterId == "spreadEnabled" || parameterId == "alignEnabled") {
    held = newValue >= 0.5f;
  } else {
    return;
  }
  // A held control goes back to neutral on the message thread (this may be
  // the audio thread, in a host that automates the parameter).
  if (held && !legacyControlResyncPending.exchange(true))
    triggerAsyncUpdate();
}

void TONE3000Processor::releaseHeldLegacyParams() {
  for (const auto& [id, value] : gridHeldLegacyParams)
    queueLegacyParamWrite(id, value);
  gridHeldLegacyParams.clear();
}

juce::var TONE3000Processor::convertRouting(bool toGrid) {
  juce::DynamicObject::Ptr result = new juce::DynamicObject();
  juce::StringArray lossy, reasons;
  const auto finish = [&](bool ok) {
    juce::Array<juce::var> lossyVar, reasonsVar;
    for (const auto& item : lossy)
      lossyVar.add(item);
    for (const auto& item : reasons)
      reasonsVar.add(item);
    result->setProperty("ok", ok);
    result->setProperty("mode", isGridMode() ? "grid" : "legacy");
    result->setProperty("lossy", lossyVar);
    result->setProperty("reasons", reasonsVar);
    return juce::var(result.get());
  };

  // Nothing to do, or nothing that can be done: answer without dipping the
  // audio or recording an undo step.
  {
    juce::ScopedLock lock(chainMutex);
    if ((routingMode == RoutingMode::Grid) == toGrid)
      return finish(true);
    if (toGrid) {
      if (const juce::String refusal = promoteRefusal(); refusal.isNotEmpty())
        reasons.add(refusal);
    } else {
      reasons = demoteBlockersLocked();
    }
    if (!reasons.isEmpty())
      return finish(false);
  }

  serviceHostedPlugins();  // the undo snapshot reads cached plugin chunks
  ChainEditFade editFade(*this);
  // After the lock below is released, and still inside the fade.
  const LegacyParamFlush flushOnExit{*this};
  {
    juce::ScopedLock lock(chainMutex);
    if ((routingMode == RoutingMode::Grid) == toGrid)
      return finish(true);
    if (toGrid) {
      if (const juce::String refusal = promoteRefusal(); refusal.isNotEmpty())
        reasons.add(refusal);
    } else {
      reasons = demoteBlockersLocked();
    }
    if (!reasons.isEmpty())
      return finish(false);

    pushChainHistory();
    if (toGrid)
      promoteToGridLocked(lossy);
    else
      demoteToLegacyLocked();
    rebuildTopology();
    snapGridRowGains();
    syncLegacyControlsFromGrid();
    // A dual-mono row carries two voices exactly like the dual-mono legacy
    // rig, so a conversion normally rebuilds nothing; when it does, the
    // mute holds until the engines are back.
    if (requeueNamEnginesForVoiceCount())
      editFade.releaseWhenChainLoadsSettle();
    refreshBlockTailLength();
    bumpChainRevision();
  }
  return finish(true);
}

// ####################
// ROW AND SPLIT MUTATORS
// ####################

juce::Result TONE3000Processor::editGrid(bool structural, const juce::String& coalesceKey,
                                         const std::function<juce::Result()>& edit) {
  bool legacy = false;
  {
    juce::ScopedLock lock(chainMutex);
    legacy = routingMode == RoutingMode::Legacy;
    if (legacy)
      if (const juce::String refusal = promoteRefusal(); refusal.isNotEmpty())
        return failure("the rig cannot become a grid: " + refusal);
  }

  // A conversion or a routing change moves audio between buffers and may
  // reopen the boundary: inside the chain mute. A mix move is a smoothed
  // gain and needs none.
  std::optional<ChainEditFade> editFade;
  if (structural || legacy) {
    serviceHostedPlugins();
    editFade.emplace(*this);
  }

  // Both after the lock below is released: the parameter writes the edit
  // queued, and the blocks of a rolled-back edit.
  const LegacyParamFlush flushOnExit{*this};
  Lane retired;
  juce::ScopedLock lock(chainMutex);
  const bool promote = routingMode == RoutingMode::Legacy;
  if (promote) {
    if (const juce::String refusal = promoteRefusal(); refusal.isNotEmpty())
      return failure("the rig cannot become a grid: " + refusal);
    if (!editFade.has_value())
      return failure("the rig changed while the edit was starting; try again");
  }

  if (!structural && !promote) {
    // A value move: nothing to validate, nothing to rebuild.
    pushChainHistory(coalesceKey);
    const juce::Result result = edit();
    syncLegacyControlsFromGrid();
    deferredRevisionBump();
    return result;
  }

  // The rig as it stands, which is what an undo goes back to and what the
  // edit is rolled back to should it break the routing rules: a refused
  // edit leaves the rig, its plan and the history exactly as they were.
  const juce::ValueTree before = captureChainSnapshot();
  std::array<bool, kGridRows> wasActive{};
  for (int row = 0; row < kGridRows; ++row)
    wasActive[static_cast<size_t>(row)] =
        !promote ? activePlan.rowInfo[static_cast<size_t>(row)].active
                 : row == 0 || (row == 1 && stereoEnabled.load());

  if (promote) {
    juce::StringArray lossy;
    promoteToGridLocked(lossy);
  }
  juce::Result result = edit();
  if (result.wasOk())
    if (const juce::String fault = gridRoutingFault(); fault.isNotEmpty())
      result = failure(fault);
  if (result.failed()) {
    retired = restoreChainSnapshot(before);
    return result;
  }
  chainHistory.push(before, {});

  rebuildTopology();
  // A row that starts to be heard has engines that may have sat out every
  // prepare since they were built.
  for (int row = 0; row < kGridRows; ++row)
    if (activePlan.rowInfo[static_cast<size_t>(row)].active && !wasActive[static_cast<size_t>(row)])
      prepareChain(row);
  snapGridRowGains();
  syncLegacyControlsFromGrid();
  if (requeueNamEnginesForVoiceCount() && editFade.has_value())
    editFade->releaseWhenChainLoadsSettle();
  refreshBlockTailLength();
  bumpChainRevision();
  return result;
}

juce::Result TONE3000Processor::setRowInput(int row, RowInput::Type type, int channel, bool stereo,
                                            RowInput::Fold fold) {
  if (row < 0 || row >= kGridRows)
    return failure("no row " + juce::String(row + 1));
  if (type == RowInput::Type::Device &&
      (channel < 0 || channel + (stereo ? 1 : 0) >= kMaxIoChannels))
    return failure("input channels run from 1 to " + juce::String(kMaxIoChannels));

  return editGrid(true, {}, [&] {
    GridRow& gridRow = grid.rows[static_cast<size_t>(row)];
    gridRow.input.type = type;
    if (type == RowInput::Type::Device) {
      gridRow.input.channel = channel;
      gridRow.input.stereo = stereo;
      // Only a pair can be summed.
      gridRow.input.fold = stereo ? fold : RowInput::Fold::None;
    }
    gridRawInput[static_cast<size_t>(row)] = {};
    return juce::Result::ok();
  });
}

juce::Result TONE3000Processor::setRowOutput(int row, RowOutput::Type type, int pair, int toRow) {
  if (row < 0 || row >= kGridRows)
    return failure("no row " + juce::String(row + 1));
  if (type == RowOutput::Type::Device && (pair < 0 || pair >= kMaxIoPairs))
    return failure("output pairs run from 1 to " + juce::String(kMaxIoPairs));
  if (type == RowOutput::Type::Row && (toRow < 0 || toRow >= kGridRows))
    return failure("a row feeds one of rows 0 to " + juce::String(kGridRows - 1));

  return editGrid(true, {}, [&] {
    GridRow& gridRow = grid.rows[static_cast<size_t>(row)];
    gridRow.output.type = type;
    gridRow.output.toRow = type == RowOutput::Type::Row ? toRow : -1;
    if (type == RowOutput::Type::Device)
      gridRow.output.pair = pair;
    gridRawOutput[static_cast<size_t>(row)] = {};
    return juce::Result::ok();
  });
}

juce::Result TONE3000Processor::setRowFormat(int row, RowFormat format) {
  if (row < 0 || row >= kGridRows)
    return failure("no row " + juce::String(row + 1));
  return editGrid(true, {}, [&] {
    grid.rows[static_cast<size_t>(row)].format = format;
    gridRawFormat[static_cast<size_t>(row)].clear();
    return juce::Result::ok();
  });
}

juce::Result TONE3000Processor::setRowMix(int row, const RowMixEdit& edit) {
  if (row < 0 || row >= kGridRows)
    return failure("no row " + juce::String(row + 1));
  for (const auto& value : {edit.volumeDb, edit.pan})
    if (value.has_value() && !std::isfinite(*value))
      return failure("the value is not a number");
  // Nothing to set: not a reason to convert a legacy rig or to record a
  // step that undoes nothing.
  if (!edit.volumeDb.has_value() && !edit.pan.has_value() && !edit.mute.has_value() &&
      !edit.solo.has_value() && !edit.invert.has_value())
    return juce::Result::ok();

  return editGrid(false, "row:" + juce::String(row) + ":mix", [&] {
    RowOutput& output = grid.rows[static_cast<size_t>(row)].output;
    if (edit.volumeDb.has_value())
      output.volumeDb.store(juce::jlimit(kRowVolumeMinDb, kRowVolumeMaxDb, *edit.volumeDb));
    if (edit.pan.has_value())
      output.pan.store(juce::jlimit(0.0f, 1.0f, *edit.pan));
    if (edit.mute.has_value())
      output.mute.store(*edit.mute);
    if (edit.solo.has_value())
      output.solo.store(*edit.solo);
    if (edit.invert.has_value())
      output.invert.store(*edit.invert);
    return juce::Result::ok();
  });
}

juce::Result TONE3000Processor::setRowInputStage(int row, const RowInputStageEdit& edit) {
  if (row < 0 || row >= kGridRows)
    return failure("no row " + juce::String(row + 1));
  if (edit.trimDb.has_value() && !std::isfinite(*edit.trimDb))
    return failure("the value is not a number");
  if (!edit.trimDb.has_value() && !edit.gate.has_value() && !edit.pitch.has_value())
    return juce::Result::ok();  // nothing to set (see setRowMix)

  // The gate and pitch switches decide which strips there are; the trim is
  // a gain the row applies itself.
  const bool structural = edit.gate.has_value() || edit.pitch.has_value();
  return editGrid(structural, "row:" + juce::String(row) + ":trim", [&] {
    GridRow& gridRow = grid.rows[static_cast<size_t>(row)];
    if (edit.trimDb.has_value()) {
      gridRow.input.trimDb = juce::jlimit(kRowTrimMinDb, kRowTrimMaxDb, *edit.trimDb);
      gridRow.trimDb.store(gridRow.input.trimDb);
    }
    if (edit.gate.has_value())
      gridRow.input.gate = *edit.gate;
    if (edit.pitch.has_value())
      gridRow.input.pitch = *edit.pitch;
    return juce::Result::ok();
  });
}

juce::Result TONE3000Processor::addSplit(int row, int splitCol, int mixCol, SplitterType type) {
  if (row < 0 || row >= kGridRows)
    return failure("no row " + juce::String(row));
  return editGrid(true, {}, [&]() -> juce::Result {
    if (splitOfRow(row) != nullptr)
      return failure("row " + juce::String(row) + " already has a split (one per row); move it "
                     "with split.move or take it out with split.remove");
    auto junction = std::make_unique<SplitJunction>();
    junction->id = juce::Uuid().toString().toStdString();
    junction->upperRow = row;
    junction->splitCol = splitCol;
    junction->mixCol = mixCol < 0 ? -1 : mixCol;
    junction->type = type;
    junction->prepare(chainSampleRate(), juce::jmax(1, chainDomainBlockSize()));
    // The lower row takes the upper row's format at the fork; row.setFormat
    // gives it one of its own afterwards.
    if (row + 1 < kGridRows) {
      grid.rows[static_cast<size_t>(row + 1)].format = rowFormat(row);
      gridRawFormat[static_cast<size_t>(row + 1)].clear();
    }
    grid.splits.push_back(std::move(junction));
    return juce::Result::ok();  // editGrid checks the routing rules
  });
}

juce::Result TONE3000Processor::moveSplit(int row, std::optional<int> splitCol,
                                          std::optional<int> mixCol) {
  if (row < 0 || row >= kGridRows)
    return failure("no row " + juce::String(row));
  return editGrid(true, {}, [&]() -> juce::Result {
    SplitJunction* junction = splitOfRow(row);
    if (junction == nullptr)
      return failure("row " + juce::String(row) + " has no split");
    if (splitCol.has_value())
      junction->splitCol = *splitCol;
    if (mixCol.has_value())
      junction->mixCol = *mixCol < 0 ? -1 : *mixCol;
    // The paths it joins are other audio now: nothing of the old ones may
    // ring on in its filters or its alignment lines.
    junction->reset();
    return juce::Result::ok();
  });
}

juce::Result TONE3000Processor::removeSplit(int row) {
  if (row < 0 || row >= kGridRows)
    return failure("no row " + juce::String(row));
  return editGrid(true, {}, [&]() -> juce::Result {
    for (auto it = grid.splits.begin(); it != grid.splits.end(); ++it)
      if ((*it)->upperRow == row) {
        // The audio thread is locked out and the plan that names the
        // junction is replaced before it runs again (rebuildTopology).
        grid.splits.erase(it);
        refreshSplitTable();
        return juce::Result::ok();
      }
    return failure("row " + juce::String(row) + " has no split");
  });
}

juce::Result TONE3000Processor::setSplitType(int row, SplitterType type) {
  if (row < 0 || row >= kGridRows)
    return failure("no row " + juce::String(row));
  return editGrid(true, {}, [&]() -> juce::Result {
    SplitJunction* junction = splitOfRow(row);
    if (junction == nullptr)
      return failure("row " + juce::String(row) + " has no split");
    // One law for an unrelated one: under the mute, and the splitter starts
    // the new one clean on its next block.
    junction->type = type;
    return juce::Result::ok();
  });
}

juce::Result TONE3000Processor::setSplitParam(int row, const juce::String& param, double value) {
  if (row < 0 || row >= kGridRows)
    return failure("no row " + juce::String(row));
  if (!std::isfinite(value))
    return failure("the value is not a number");
  std::string junctionId;
  {
    juce::ScopedLock lock(chainMutex);
    // A legacy rig has no split: nothing to convert it for.
    const SplitJunction* junction = routingMode == RoutingMode::Grid ? splitOfRow(row) : nullptr;
    if (junction == nullptr)
      return failure("row " + juce::String(row) + " has no split");
    if (SplitJunction::findParameter(param) < 0)
      return failure("a split has no parameter '" + param + "'");
    junctionId = junction->id;
  }
  // A lock-free store the junction ramps to: no fade, no recompile. The
  // key is the address scenes and MIDI will use for it.
  return editGrid(false, "split:" + juce::String(junctionId) + ":" + param, [&]() -> juce::Result {
    SplitJunction* junction = splitOfRow(row);
    if (junction == nullptr || !junction->setParameter(param, static_cast<float>(value)))
      return failure("row " + juce::String(row) + " has no split");
    return juce::Result::ok();
  });
}

juce::var TONE3000Processor::getSplitParams(int row) const {
  juce::ScopedLock lock(chainMutex);
  const SplitJunction* junction = routingMode == RoutingMode::Grid ? splitOfRow(row) : nullptr;
  if (junction == nullptr)
    return {};
  juce::Array<juce::var> params;
  const BlockParamTable& table = SplitJunction::parameterSpecs();
  for (size_t i = 0; i < table.size(); ++i) {
    juce::var entry = table[i].toVar();
    if (auto* object = entry.getDynamicObject())
      object->setProperty("value", static_cast<double>(junction->getParameter(static_cast<int>(i))));
    params.add(entry);
  }
  juce::DynamicObject::Ptr result = new juce::DynamicObject();
  result->setProperty("id", juce::String(junction->id));
  result->setProperty("row", row);
  result->setProperty("type", splitterTypeName(junction->type));
  result->setProperty("params", params);
  return juce::var(result.get());
}

juce::Result TONE3000Processor::moveGridBlock(const std::string& blockId, int row, int slot,
                                              bool insertMode) {
  if (row < 0 || row >= kGridRows)
    return failure("no row " + juce::String(row + 1));
  if (slot < 0 || slot >= kGridCols)
    return failure("slots run from 1 to " + juce::String(kGridCols));

  // Checked before anything is recorded or converted. Positions are the
  // same in the legacy rig and in the grid it becomes (lane index i is
  // column i), except past a lane's own length, where the grid has empty
  // slots.
  {
    juce::ScopedLock lock(chainMutex);
    const ChainBlock* block = findBlockById(blockId);
    if (block == nullptr)
      return failure("no block with id '" + juce::String(blockId) + "'");
    if (block->type == ChainBlockType::INSERT)
      return failure("an empty slot cannot be moved");
    const int sourceRow = rowOfBlock(*block);
    if (insertMode && sourceRow != row &&
        toneCount(lanes[static_cast<size_t>(row)]) >= kGridCols)
      return failure(rowName(row) + " is full");
  }

  return editGrid(true, {}, [&]() -> juce::Result {
    int sourceRow = -1, sourceSlot = -1;
    for (int r = 0; r < kGridRows && sourceRow < 0; ++r)
      for (int i = 0; i < static_cast<int>(lanes[static_cast<size_t>(r)].size()); ++i)
        if (lanes[static_cast<size_t>(r)][static_cast<size_t>(i)]->id == blockId) {
          sourceRow = r;
          sourceSlot = i;
          break;
        }
    if (sourceRow < 0 || isInsert(lanes[static_cast<size_t>(sourceRow)][static_cast<size_t>(sourceSlot)]))
      return failure("the block is gone");
    Lane& source = lanes[static_cast<size_t>(sourceRow)];
    Lane& target = lanes[static_cast<size_t>(row)];
    if (slot >= static_cast<int>(target.size()))
      return failure("the slot is gone");

    if (!insertMode) {
      // Whatever sits there (a block or an empty slot) takes this block's
      // place.
      if (sourceRow != row || sourceSlot != slot)
        std::swap(source[static_cast<size_t>(sourceSlot)], target[static_cast<size_t>(slot)]);
      return juce::Result::ok();
    }

    // Insert: the block leaves an empty slot behind, then takes `slot`,
    // pushing what is there toward the nearest empty slot of the row.
    std::unique_ptr<ChainBlock> moving = std::move(source[static_cast<size_t>(sourceSlot)]);
    source[static_cast<size_t>(sourceSlot)] = makeInsert();
    if (isInsert(target[static_cast<size_t>(slot)])) {
      target[static_cast<size_t>(slot)] = std::move(moving);
      return juce::Result::ok();
    }
    int spare = -1;
    for (int i = slot; i < static_cast<int>(target.size()) && spare < 0; ++i)
      if (isInsert(target[static_cast<size_t>(i)]))
        spare = i;
    for (int i = slot - 1; i >= 0 && spare < 0; --i)
      if (isInsert(target[static_cast<size_t>(i)]))
        spare = i;
    if (spare < 0) {
      // Not reachable past the check above; put the block back.
      source[static_cast<size_t>(sourceSlot)] = std::move(moving);
      return failure(rowName(row) + " is full");
    }
    target.erase(target.begin() + spare);
    // With the spare slot taken from before `slot`, the blocks up to it
    // moved one to the left and the block lands on `slot` itself; taken
    // from after, the blocks from `slot` on moved one to the right.
    target.insert(target.begin() + juce::jmin(slot, static_cast<int>(target.size())),
                  std::move(moving));
    return juce::Result::ok();
  });
}

std::string TONE3000Processor::gridSlotBlockId(int row, int slot) const {
  juce::ScopedLock lock(chainMutex);
  if (routingMode != RoutingMode::Grid || row < 0 || row >= kGridRows || slot < 0 ||
      slot >= static_cast<int>(lanes[static_cast<size_t>(row)].size()))
    return {};
  return lanes[static_cast<size_t>(row)][static_cast<size_t>(slot)]->id;
}

// ####################
// REPORTED STATE
// ####################

juce::var TONE3000Processor::gridStateLocked() const {
  const bool gridMode = routingMode == RoutingMode::Grid;
  juce::DynamicObject::Ptr state = new juce::DynamicObject();
  state->setProperty("mode", gridMode ? "grid" : "legacy");
  state->setProperty("legacyProjectable", demoteBlockersLocked().isEmpty());

  const PlanCpu cpu = computePlanCpu(activePlan, makePlanInput());
  juce::Array<juce::var> rows, groups;
  std::uint32_t wantedInputs = 0, wantedPairs = 0;
  if (gridMode)
    for (int row = 0; row < kGridRows; ++row) {
      const GridRow& gridRow = grid.rows[static_cast<size_t>(row)];
      const ProcessingPlan::RowInfo& info = activePlan.rowInfo[static_cast<size_t>(row)];
      juce::DynamicObject::Ptr item = new juce::DynamicObject();
      item->setProperty("index", row);
      item->setProperty("format", gridRawFormat[static_cast<size_t>(row)].isNotEmpty()
                                      ? gridRawFormat[static_cast<size_t>(row)]
                                      : juce::String(formatToString(gridRow.format)));
      // What the row runs in: its own format, or dual mono below a split
      // from a dual-mono row.
      item->setProperty("runsAs", formatToString(static_cast<RowFormat>(info.format)));
      // Where its signal comes from and goes, as the plan has it.
      static const char* const sources[] = {"none", "device", "split", "rows"};
      static const char* const sinks[] = {"none", "device", "row", "mix"};
      item->setProperty("source", sources[juce::jlimit(0, 3, static_cast<int>(info.source))]);
      item->setProperty("sink", sinks[juce::jlimit(0, 3, static_cast<int>(info.sink))]);

      juce::DynamicObject::Ptr input = new juce::DynamicObject();
      const bool deviceIn = gridRow.input.type == RowInput::Type::Device;
      input->setProperty("type", gridRawInput[static_cast<size_t>(row)].isValid()
                                     ? gridRawInput[static_cast<size_t>(row)]
                                           .getProperty("type")
                                           .toString()
                                     : juce::String(deviceIn ? "device" : "none"));
      if (deviceIn) {
        input->setProperty("channel", gridRow.input.channel + 1);
        input->setProperty("stereo", gridRow.input.stereo);
        input->setProperty("fold",
                           gridRow.input.fold == RowInput::Fold::SumToMono ? "sum" : "none");
        wantedInputs |= 1u << gridRow.input.channel;
        if (gridRow.input.stereo)
          wantedInputs |= 1u << (gridRow.input.channel + 1);
      }
      input->setProperty("trimDb", static_cast<double>(gridRow.input.trimDb));
      input->setProperty("gate", gridRow.input.gate);
      input->setProperty("pitch", gridRow.input.pitch);
      item->setProperty("input", juce::var(input.get()));

      juce::DynamicObject::Ptr output = new juce::DynamicObject();
      const bool deviceOut = gridRow.output.type == RowOutput::Type::Device;
      output->setProperty("type",
                          gridRawOutput[static_cast<size_t>(row)].isValid()
                              ? gridRawOutput[static_cast<size_t>(row)].getProperty("type").toString()
                              : juce::String(deviceOut ? "device"
                                                 : gridRow.output.type == RowOutput::Type::Row
                                                       ? "row"
                                                       : "none"));
      if (deviceOut) {
        output->setProperty("pair", gridRow.output.pair + 1);
        wantedPairs |= 1u << gridRow.output.pair;
      }
      if (gridRow.output.type == RowOutput::Type::Row)
        output->setProperty("toRow", gridRow.output.toRow);
      output->setProperty("volumeDb", static_cast<double>(gridRow.output.volumeDb.load()));
      output->setProperty("pan", static_cast<double>(gridRow.output.pan.load()));
      output->setProperty("mute", gridRow.output.mute.load());
      output->setProperty("solo", gridRow.output.solo.load());
      output->setProperty("invert", gridRow.output.invert.load());
      item->setProperty("output", juce::var(output.get()));

      // The slots of a running row's range are run: all eight, or for the
      // lower row of a split the split's range. A row that is not routed
      // runs none. Blocks outside are kept.
      juce::Array<juce::var> slots, active;
      int slotIndex = 0;
      for (const auto& block : lanes[static_cast<size_t>(row)]) {
        slots.add(juce::String(block->id));
        active.add(info.active && slotIndex >= info.begin && slotIndex < info.end &&
                   block->type != ChainBlockType::INSERT);
        ++slotIndex;
      }
      item->setProperty("slots", slots);
      item->setProperty("active", active);
      item->setProperty("routed", info.active);
      juce::Array<juce::var> range;
      range.add(static_cast<int>(info.begin));
      range.add(static_cast<int>(info.end));
      item->setProperty("activeSlots", range);
      item->setProperty("latencySamples", info.latency);
      item->setProperty("padSamples", info.pad);
      item->setProperty("cpu", static_cast<double>(cpu.rows[static_cast<size_t>(row)]));
      item->setProperty("inputMissing", info.inputMissing);
      item->setProperty("outputFallback", info.outputFallback);
      rows.add(juce::var(item.get()));
    }
  // The rows that are processed together: tied by a split or a feed.
  if (gridMode)
    for (int group = 0; group < activePlan.numGroups; ++group) {
      juce::Array<juce::var> members;
      for (int row = 0; row < kGridRows; ++row)
        if (activePlan.rowInfo[static_cast<size_t>(row)].active &&
            activePlan.rowInfo[static_cast<size_t>(row)].group == group)
          members.add(row);
      groups.add(juce::var(members));
    }

  juce::Array<juce::var> splits, feeds;
  if (gridMode) {
    const auto pair = [](float a, float b) {
      juce::Array<juce::var> values;
      values.add(static_cast<double>(a));
      values.add(static_cast<double>(b));
      return juce::var(values);
    };
    for (const auto& junction : grid.splits) {
      const int upper = juce::jlimit(0, kGridRows - 1, junction->upperRow);
      const ProcessingPlan::SplitInfo& info = activePlan.splitInfo[static_cast<size_t>(upper)];
      juce::DynamicObject::Ptr item = new juce::DynamicObject();
      item->setProperty("id", juce::String(junction->id));
      item->setProperty("row", junction->upperRow);
      item->setProperty("lowerRow", junction->upperRow + 1);
      item->setProperty("splitAt", junction->splitCol);
      item->setProperty("mixAt", junction->hasRejoin() ? juce::var(junction->mixCol) : juce::var());
      item->setProperty("type", splitterTypeName(junction->type));
      // Whether the fork is run (the upper row is) and whether the lower
      // path leads anywhere.
      item->setProperty("active", activePlan.rowInfo[static_cast<size_t>(upper)].active);
      item->setProperty("lowerActive", info.lowerActive);
      juce::DynamicObject::Ptr params = new juce::DynamicObject();
      const BlockParamTable& table = SplitJunction::parameterSpecs();
      for (size_t i = 0; i < table.size(); ++i)
        params->setProperty(juce::Identifier(table[i].id),
                            static_cast<double>(junction->getParameter(static_cast<int>(i))));
      item->setProperty("params", juce::var(params.get()));
      // The mixer's alignment: what each path waits for the other, in
      // chain-rate samples.
      juce::DynamicObject::Ptr alignment = new juce::DynamicObject();
      alignment->setProperty("a", junction->alignmentA());
      alignment->setProperty("b", junction->alignmentB());
      item->setProperty("alignment", juce::var(alignment.get()));
      juce::DynamicObject::Ptr meters = new juce::DynamicObject();
      meters->setProperty("split", pair(junction->splitMeterDb(0), junction->splitMeterDb(1)));
      meters->setProperty("mix", pair(junction->mixMeterDb(0), junction->mixMeterDb(1)));
      item->setProperty("meters", juce::var(meters.get()));
      splits.add(juce::var(item.get()));
    }
    // Splits this build cannot run (a type from a later build, or one a
    // loaded rig placed where the rules forbid): kept, written back, inert.
    for (const auto& unknown : gridUnknownNodes)
      if (unknown.hasType("Split")) {
        juce::DynamicObject::Ptr item = new juce::DynamicObject();
        item->setProperty("id", unknown.getProperty("id").toString());
        item->setProperty("row", finiteInt(unknown.getProperty("upperRow"), -1));
        item->setProperty("type", unknown.getProperty("type").toString());
        item->setProperty("active", false);
        item->setProperty("inert", true);
        splits.add(juce::var(item.get()));
      }
    for (int row = 0; row < kGridRows; ++row) {
      const ProcessingPlan::RowInfo& info = activePlan.rowInfo[static_cast<size_t>(row)];
      if (grid.rows[static_cast<size_t>(row)].output.type != RowOutput::Type::Row)
        continue;
      juce::DynamicObject::Ptr item = new juce::DynamicObject();
      item->setProperty("from", row);
      item->setProperty("to", grid.rows[static_cast<size_t>(row)].output.toRow);
      item->setProperty("active", info.active && info.sink == ProcessingPlan::RowInfo::ToRow);
      item->setProperty("padSamples", info.pad);
      feeds.add(juce::var(item.get()));
    }
  }
  state->setProperty("rows", rows);
  state->setProperty("splits", splits);
  state->setProperty("feeds", feeds);
  state->setProperty("groups", groups);
  // What the legacy image controls are in this grid: "rows" where rows 0
  // and 1 carry them, "held" where they rest at neutral and do nothing.
  if (gridMode) {
    const int bound = gridLegacyControls.load();
    juce::DynamicObject::Ptr controls = new juce::DynamicObject();
    controls->setProperty("pan", (bound & kLegacyPans) != 0 ? "rows" : "held");
    controls->setProperty("solo", (bound & kLegacySolos) != 0 ? "rows" : "held");
    controls->setProperty("invert", (bound & kLegacyInverts) != 0 ? "rows" : "held");
    controls->setProperty("balance", (bound & kLegacyBalance) != 0 ? "rows" : "held");
    controls->setProperty("spread", "held");
    controls->setProperty("align", "held");
    state->setProperty("legacyControls", juce::var(controls.get()));
  }
  // The image stage (Spread, Align, balance and pan matrix) is a legacy
  // construct and does nothing in a grid.
  state->setProperty("imageStage", gridMode ? "none" : "legacy");
  state->setProperty("chainLatencySamples", activePlan.reportedChainLatency);
  state->setProperty("latencySamples", getLatencySamples());

  juce::DynamicObject::Ptr cpuObject = new juce::DynamicObject();
  cpuObject->setProperty("total", static_cast<double>(cpu.total));
  cpuObject->setProperty("critical", static_cast<double>(cpu.critical));
  state->setProperty("cpu", juce::var(cpuObject.get()));

  // What the device has, and what the rows ask for that it has not.
  juce::Array<juce::var> inputs, outputs;
  std::uint32_t haveInputs = 0, havePairs = 0;
  for (int i = 0; i < deviceChannels.numInputs; ++i) {
    const int physical = deviceChannels.inputPhysical[static_cast<size_t>(i)];
    if (physical < 0 || physical >= 32)
      continue;
    haveInputs |= 1u << physical;
    juce::DynamicObject::Ptr entry = new juce::DynamicObject();
    entry->setProperty("channel", physical + 1);
    entry->setProperty("name", deviceChannels.inputNames[i]);
    entry->setProperty("available", true);
    inputs.add(juce::var(entry.get()));
  }
  for (int channel = 0; channel < kMaxIoChannels; ++channel)
    if ((wantedInputs & ~haveInputs & (1u << channel)) != 0 && deviceChannels.numInputs > 0) {
      juce::DynamicObject::Ptr entry = new juce::DynamicObject();
      entry->setProperty("channel", channel + 1);
      entry->setProperty("name", juce::String());
      entry->setProperty("available", false);
      inputs.add(juce::var(entry.get()));
    }
  for (int pair = 0; pair < kMaxIoPairs; ++pair) {
    const int left = deviceChannels.outputBufferChannel(2 * pair);
    const int right = deviceChannels.outputBufferChannel(2 * pair + 1);
    if (left < 0)
      continue;
    havePairs |= 1u << pair;
    juce::DynamicObject::Ptr entry = new juce::DynamicObject();
    entry->setProperty("pair", pair + 1);
    entry->setProperty("name", deviceChannels.outputNames[left] +
                                   (right >= 0 ? " / " + deviceChannels.outputNames[right]
                                               : juce::String()));
    entry->setProperty("available", true);
    outputs.add(juce::var(entry.get()));
  }
  for (int pair = 0; pair < kMaxIoPairs; ++pair)
    if ((wantedPairs & ~havePairs & (1u << pair)) != 0 && deviceChannels.numOutputs > 0) {
      juce::DynamicObject::Ptr entry = new juce::DynamicObject();
      entry->setProperty("pair", pair + 1);
      entry->setProperty("name", juce::String());
      entry->setProperty("available", false);
      outputs.add(juce::var(entry.get()));
    }
  juce::DynamicObject::Ptr io = new juce::DynamicObject();
  io->setProperty("inputs", inputs);
  io->setProperty("outputs", outputs);
  state->setProperty("io", juce::var(io.get()));
  return juce::var(state.get());
}

juce::var TONE3000Processor::getGridState() const {
  juce::ScopedLock lock(chainMutex);
  return gridStateLocked();
}

// ####################
// SAVED STATE
// ####################

juce::ValueTree TONE3000Processor::captureGridNode(bool includeModelData) const {
  juce::ValueTree node("Grid");
  // The format the node is written in, and the oldest reader that can run
  // it: 1 for independent rows, 2 once the rig has a split or a row feed. A
  // build that only knows 1 then refuses the rig whole instead of playing
  // its rows as if nothing joined them.
  bool joins = !grid.splits.empty();
  for (const auto& gridRow : grid.rows)
    joins = joins || gridRow.output.type == RowOutput::Type::Row;
  for (const auto& unknown : gridUnknownNodes)
    joins = joins || unknown.hasType("Split");
  node.setProperty("version", joins ? kGridFormatWithJoins : 1, nullptr);
  node.setProperty("minReader", joins ? kGridFormatWithJoins : 1, nullptr);
  node.setProperty("rows", kGridRows, nullptr);
  node.setProperty("cols", kGridCols, nullptr);
  // What the legacy image controls were when the rig became a grid (see
  // Processor.h, LegacyControl): "id=value" pairs for those off neutral.
  // Always written, so that a reader can tell "all at neutral" from a node
  // that predates this.
  {
    juce::StringArray held;
    for (const LegacyControlParam& control : kLegacyControlParams)
      if (const auto it = gridHeldLegacyParams.find(control.id);
          it != gridHeldLegacyParams.end() && std::abs(it->second - control.neutral) > 1.0e-6f)
        held.add(juce::String(control.id) + "=" + juce::String(it->second, 6));
    node.setProperty("legacyControls", held.joinIntoString(";"), nullptr);
  }

  for (int row = 0; row < kGridRows; ++row) {
    const GridRow& gridRow = grid.rows[static_cast<size_t>(row)];
    juce::ValueTree rowNode("Row");
    rowNode.setProperty("index", row, nullptr);
    rowNode.setProperty("format",
                        gridRawFormat[static_cast<size_t>(row)].isNotEmpty()
                            ? gridRawFormat[static_cast<size_t>(row)]
                            : juce::String(formatToString(gridRow.format)),
                        nullptr);

    // An input or output of a type this build does not know goes back out
    // exactly as it came in.
    if (gridRawInput[static_cast<size_t>(row)].isValid()) {
      rowNode.appendChild(gridRawInput[static_cast<size_t>(row)].createCopy(), nullptr);
    } else {
      juce::ValueTree input("Input");
      input.setProperty("type",
                        gridRow.input.type == RowInput::Type::Device ? "device" : "none", nullptr);
      input.setProperty("channel", gridRow.input.channel, nullptr);
      input.setProperty("stereo", gridRow.input.stereo, nullptr);
      input.setProperty("fold", gridRow.input.fold == RowInput::Fold::SumToMono ? "sum" : "none",
                        nullptr);
      input.setProperty("trimDb", static_cast<double>(gridRow.input.trimDb), nullptr);
      input.setProperty("gate", gridRow.input.gate, nullptr);
      input.setProperty("pitch", gridRow.input.pitch, nullptr);
      rowNode.appendChild(input, nullptr);
    }
    if (gridRawOutput[static_cast<size_t>(row)].isValid()) {
      rowNode.appendChild(gridRawOutput[static_cast<size_t>(row)].createCopy(), nullptr);
    } else {
      juce::ValueTree output("Output");
      output.setProperty("type",
                         gridRow.output.type == RowOutput::Type::Device
                             ? "device"
                             : gridRow.output.type == RowOutput::Type::Row ? "row" : "none",
                         nullptr);
      output.setProperty("pair", gridRow.output.pair, nullptr);
      if (gridRow.output.type == RowOutput::Type::Row)
        output.setProperty("toRow", gridRow.output.toRow, nullptr);
      output.setProperty("volumeDb", static_cast<double>(gridRow.output.volumeDb.load()), nullptr);
      output.setProperty("pan", static_cast<double>(gridRow.output.pan.load()), nullptr);
      output.setProperty("mute", gridRow.output.mute.load(), nullptr);
      output.setProperty("solo", gridRow.output.solo.load(), nullptr);
      output.setProperty("invert", gridRow.output.invert.load(), nullptr);
      rowNode.appendChild(output, nullptr);
    }

    // Exactly `cols` block nodes, today's ChainBlock node unchanged; an
    // empty slot is one of type "insert".
    juce::ValueTree slots("Slots");
    serializeChainToTree(lanes[static_cast<size_t>(row)], slots, includeModelData);
    rowNode.appendChild(slots, nullptr);
    node.appendChild(rowNode, nullptr);
  }

  // Splits, in row order (the node of section 10: placement, type and one
  // Param child per parameter).
  for (int upper = 0; upper < kGridRows; ++upper)
    if (const SplitJunction* junction = splitOfRow(upper))
      node.appendChild(junction->getState(), nullptr);
  for (const auto& unknown : gridUnknownNodes)
    node.appendChild(unknown.createCopy(), nullptr);
  return node;
}

juce::String TONE3000Processor::gridSnapshotRefusal(const juce::ValueTree& snapshot) {
  if (snapshot.getProperty("layout").toString() != "grid")
    return {};
  const juce::ValueTree node = snapshot.getChildWithName("Grid");
  if (!node.isValid())
    return "the grid layout has no Grid node";
  const int minReader = finiteInt(node.getProperty("minReader"), 1);
  if (minReader > kGridFormatVersion)
    return "the grid needs format " + juce::String(minReader) + " and this build reads up to " +
           juce::String(kGridFormatVersion);
  if (finiteInt(node.getProperty("rows"), kGridRows) > kGridRows ||
      finiteInt(node.getProperty("cols"), kGridCols) != kGridCols)
    return "the grid is not " + juce::String(kGridRows) + " rows of " + juce::String(kGridCols) +
           " slots";
  return {};
}

void TONE3000Processor::leaveGridMode(Lane& retired) {
  for (int row = kNumLegacyLanes; row < kGridRows; ++row) {
    for (auto& block : lanes[static_cast<size_t>(row)])
      retired.push_back(std::move(block));
    lanes[static_cast<size_t>(row)].clear();
  }
  routingMode = RoutingMode::Legacy;
  for (int row = 0; row < kGridRows; ++row) {
    resetRow(grid.rows[static_cast<size_t>(row)]);
    gridRawInput[static_cast<size_t>(row)] = {};
    gridRawOutput[static_cast<size_t>(row)] = {};
    gridRawFormat[static_cast<size_t>(row)].clear();
  }
  gridUnknownNodes.clear();
  grid.splits.clear();
  refreshSplitTable();
  gridLegacyControls.store(0);
  // The legacy image controls go back to what they were before the grid
  // held or bound them.
  releaseHeldLegacyParams();
  syncGridAtomics();
}

// Rebuild all four rows from a Grid node. Every live block is parked in one
// map first, whichever row it sits in, so a block that the snapshot has in
// another row keeps its engine (an undo of a cross-row move rebuilds
// nothing, bar the voices of a NAM that changes format).
void TONE3000Processor::reconcileGridFromTree(const juce::ValueTree& gridNode, Lane& retired) {
  ParkedBlocks existing;
  for (auto& l : lanes) {
    for (auto& block : l)
      if (block)
        existing[block->id] = std::move(block);
    l.clear();
  }

  for (int row = 0; row < kGridRows; ++row) {
    resetRow(grid.rows[static_cast<size_t>(row)]);
    gridRawInput[static_cast<size_t>(row)] = {};
    gridRawOutput[static_cast<size_t>(row)] = {};
    gridRawFormat[static_cast<size_t>(row)].clear();
  }
  gridUnknownNodes.clear();

  // The junctions, parked by id like the blocks: one that the node still
  // has keeps its filters, ramps and delay lines, so an undo of a parameter
  // move disturbs nothing.
  std::map<std::string, std::unique_ptr<SplitJunction>> parkedSplits;
  for (auto& junction : grid.splits)
    parkedSplits[junction->id] = std::move(junction);
  grid.splits.clear();
  refreshSplitTable();  // nothing may reach a junction that is not kept

  // What the legacy image controls go back to. The node names those that
  // were off neutral; a node without the property predates this, and the
  // controls are then taken as they stand (syncLegacyControlsFromGrid).
  gridHeldLegacyParams.clear();
  if (gridNode.hasProperty("legacyControls")) {
    for (const LegacyControlParam& control : kLegacyControlParams)
      gridHeldLegacyParams[control.id] = control.neutral;
    for (const auto& entry :
         juce::StringArray::fromTokens(gridNode.getProperty("legacyControls").toString(), ";", ""))
      for (const LegacyControlParam& control : kLegacyControlParams)
        if (entry.upToFirstOccurrenceOf("=", false, false) == control.id)
          gridHeldLegacyParams[control.id] = finiteFloat(
              juce::var(entry.fromFirstOccurrenceOf("=", false, false).getDoubleValue()),
              control.neutral);
  }

  for (int i = 0; i < gridNode.getNumChildren(); ++i) {
    const juce::ValueTree child = gridNode.getChild(i);
    const int row = finiteInt(child.getProperty("index"), -1);
    if (child.hasType("Split") && static_cast<bool>(child.getProperty(kSetAsideProperty, false))) {
      // Set aside by an earlier load: it stays aside. Taking it up now,
      // because the split that was in its way has since been removed, would
      // make a split appear on an undo or a reload that nobody added.
      gridUnknownNodes.push_back(child.createCopy());
      continue;
    }
    if (child.hasType("Split")) {
      // Reconciled by id. A type this build does not know (setState says
      // so and changes nothing) is kept as the node it came as.
      const std::string id = child.getProperty("id").toString().toStdString();
      std::unique_ptr<SplitJunction> junction;
      if (const auto parked = parkedSplits.find(id); parked != parkedSplits.end()) {
        junction = std::move(parked->second);
        parkedSplits.erase(parked);
      } else {
        junction = std::make_unique<SplitJunction>();
        junction->prepare(chainSampleRate(), juce::jmax(1, chainDomainBlockSize()));
      }
      if (junction->setState(child)) {
        // One split per row, and only of a row that has a row below it:
        // anything else is set aside here and now, whatever the file holds
        // (a dozen of them, a thousand), so the rig never has more
        // junctions than rows.
        if (junction->upperRow < 0 || junction->upperRow >= kGridRows - 1 ||
            splitOfRow(junction->upperRow) != nullptr) {
          juce::Logger::writeToLog("[Restore] Split of row " + juce::String(junction->upperRow) +
                                   " has no place in the grid; kept inert");
          gridUnknownNodes.push_back(setAsideCopy(child));
          continue;
        }
        if (junction->id.empty())
          junction->id = juce::Uuid().toString().toStdString();
        grid.splits.push_back(std::move(junction));
      } else {
        juce::Logger::writeToLog("[Restore] Split of type '" + child.getProperty("type").toString() +
                                 "' is not known to this build; kept inert");
        gridUnknownNodes.push_back(child.createCopy());
      }
      continue;
    }
    if (!child.hasType("Row") || row < 0 || row >= kGridRows) {
      // Not something this build runs. Kept whole and written back, so a
      // newer build finds its rig as it left it.
      gridUnknownNodes.push_back(child.createCopy());
      continue;
    }
    GridRow& gridRow = grid.rows[static_cast<size_t>(row)];

    const juce::String format = child.getProperty("format").toString();
    if (format == "mono")
      gridRow.format = RowFormat::Mono;
    else if (format == "dual")
      gridRow.format = RowFormat::DualMono;
    else if (format != "stereo" && format.isNotEmpty())
      gridRawFormat[static_cast<size_t>(row)] = format;  // runs as stereo, saved as it was

    const juce::ValueTree input = child.getChildWithName("Input");
    const juce::String inputType = input.getProperty("type", "none").toString();
    if (inputType == "device" || inputType == "none") {
      gridRow.input.type =
          inputType == "device" ? RowInput::Type::Device : RowInput::Type::None;
      gridRow.input.stereo = static_cast<bool>(input.getProperty("stereo", false));
      gridRow.input.channel = juce::jlimit(0, kMaxIoChannels - (gridRow.input.stereo ? 2 : 1),
                                           finiteInt(input.getProperty("channel"), 0));
      gridRow.input.fold = gridRow.input.stereo && input.getProperty("fold").toString() == "sum"
                               ? RowInput::Fold::SumToMono
                               : RowInput::Fold::None;
      gridRow.input.trimDb = juce::jlimit(kRowTrimMinDb, kRowTrimMaxDb,
                                          finiteFloat(input.getProperty("trimDb"), 0.0f));
      gridRow.input.gate = static_cast<bool>(input.getProperty("gate", true));
      gridRow.input.pitch = static_cast<bool>(input.getProperty("pitch", false));
    } else {
      // An input type from a newer build: the row has no input here.
      gridRawInput[static_cast<size_t>(row)] = input.createCopy();
    }
    gridRow.trimDb.store(gridRow.input.trimDb);

    const juce::ValueTree output = child.getChildWithName("Output");
    const juce::String outputType = output.getProperty("type", "none").toString();
    if ((outputType == "device" || outputType == "none" || outputType == "row") &&
        !static_cast<bool>(output.getProperty(kSetAsideProperty, false))) {
      gridRow.output.type = outputType == "device" ? RowOutput::Type::Device
                                : outputType == "row" ? RowOutput::Type::Row
                                                      : RowOutput::Type::None;
      gridRow.output.pair =
          juce::jlimit(0, kMaxIoPairs - 1, finiteInt(output.getProperty("pair"), 0));
      gridRow.output.toRow = finiteInt(output.getProperty("toRow"), -1);
      gridRow.output.volumeDb.store(juce::jlimit(
          kRowVolumeMinDb, kRowVolumeMaxDb, finiteFloat(output.getProperty("volumeDb"), 0.0f)));
      gridRow.output.pan.store(
          juce::jlimit(0.0f, 1.0f, finiteFloat(output.getProperty("pan"), 0.5f)));
      gridRow.output.mute.store(static_cast<bool>(output.getProperty("mute", false)));
      gridRow.output.solo.store(static_cast<bool>(output.getProperty("solo", false)));
      gridRow.output.invert.store(static_cast<bool>(output.getProperty("invert", false)));
    } else {
      gridRawOutput[static_cast<size_t>(row)] = output.createCopy();
    }

    Lane& target = lanes[static_cast<size_t>(row)];
    const juce::ValueTree slots = child.getChildWithName("Slots");
    for (int s = 0; s < slots.getNumChildren(); ++s) {
      const juce::ValueTree blockState = slots.getChild(s);
      if (!blockState.hasType("ChainBlock"))
        continue;
      if (static_cast<int>(target.size()) >= kGridCols) {
        juce::Logger::writeToLog("[Restore] " + rowName(row) + " has more than " +
                                 juce::String(kGridCols) + " slots; the rest is dropped");
        break;
      }
      target.push_back(reconcileBlockFromTree(blockState, existing));
    }
  }

  padGridRows();
  for (auto& [id, block] : existing)
    retired.push_back(std::move(block));
  sanitizeRestoredRouting();
}

// A feed kept as the Output node it was saved as, and taken out of the
// routing. Caller holds chainMutex.
void TONE3000Processor::setFeedAside(int row) {
  GridRow& gridRow = grid.rows[static_cast<size_t>(row)];
  juce::ValueTree output("Output");
  output.setProperty("type", "row", nullptr);
  output.setProperty(kSetAsideProperty, true, nullptr);
  output.setProperty("pair", gridRow.output.pair, nullptr);
  output.setProperty("toRow", gridRow.output.toRow, nullptr);
  output.setProperty("volumeDb", static_cast<double>(gridRow.output.volumeDb.load()), nullptr);
  output.setProperty("pan", static_cast<double>(gridRow.output.pan.load()), nullptr);
  output.setProperty("mute", gridRow.output.mute.load(), nullptr);
  output.setProperty("solo", gridRow.output.solo.load(), nullptr);
  output.setProperty("invert", gridRow.output.invert.load(), nullptr);
  gridRawOutput[static_cast<size_t>(row)] = output;
  gridRow.output.type = RowOutput::Type::None;
  gridRow.output.toRow = -1;
}

// Every split and every feed out of the routing, kept as the nodes they
// were. What is left is rows with device inputs and outputs, which always
// compiles. Caller holds chainMutex.
void TONE3000Processor::setAllJoinsAside() {
  for (auto& junction : grid.splits)
    gridUnknownNodes.push_back(setAsideCopy(junction->getState()));
  grid.splits.clear();
  refreshSplitTable();
  for (int row = 0; row < kGridRows; ++row)
    if (grid.rows[static_cast<size_t>(row)].output.type == RowOutput::Type::Row)
      setFeedAside(row);
}

// See the declaration. The node is the saved truth, so nothing is thrown
// away: the split at fault goes back to being a raw node (as a split of an
// unknown type is), the feed at fault becomes a raw Output node, and both
// are written back as they came. Every pass takes one split or one feed
// out, so the passes are bounded by how many there are; should a fault
// outlive them all, every join goes aside.
void TONE3000Processor::sanitizeRestoredRouting() {
  int joins = static_cast<int>(grid.splits.size());
  for (const auto& gridRow : grid.rows)
    joins += gridRow.output.type == RowOutput::Type::Row ? 1 : 0;
  for (int pass = 0; pass <= joins; ++pass) {
    int badSplit = -1, badFeed = -1;
    const std::string fault = validateGridRouting(makePlanInput(), &badSplit, &badFeed);
    if (fault.empty())
      return;
    juce::Logger::writeToLog("[Restore] Grid routing set aside: " + juce::String(fault));
    bool repaired = false;
    if (badSplit >= 0)
      for (auto it = grid.splits.begin(); it != grid.splits.end(); ++it)
        if ((*it)->upperRow == badSplit) {
          gridUnknownNodes.push_back(setAsideCopy((*it)->getState()));
          grid.splits.erase(it);
          refreshSplitTable();
          repaired = true;
          break;
        }
    if (!repaired && badFeed >= 0 && badFeed < kGridRows &&
        grid.rows[static_cast<size_t>(badFeed)].output.type == RowOutput::Type::Row) {
      setFeedAside(badFeed);
      repaired = true;
    }
    if (!repaired)
      break;  // nothing names the fault
  }
  if (!validateGridRouting(makePlanInput()).empty()) {
    juce::Logger::writeToLog("[Restore] The grid's splits and feeds could not be repaired one by "
                             "one; all of them are set aside");
    setAllJoinsAside();
  }
}

void TONE3000Processor::restoreGridSnapshot(const juce::ValueTree& snapshot, Lane& retired) {
  // No latency total is published for the half-rebuilt rows in between:
  // rebuildTopology below refreshes once, on the finished grid.
  std::array<bool, kGridRows> wasActive{};
  for (int row = 0; row < kGridRows; ++row)
    wasActive[static_cast<size_t>(row)] = routingMode == RoutingMode::Grid
                                              ? activePlan.rowInfo[static_cast<size_t>(row)].active
                                              : row == 0 || (row == 1 && stereoEnabled.load());
  latencyRefreshSuspended = true;
  routingMode = RoutingMode::Grid;
  stereoEnabled.store(false);
  branchAfterBlockId.clear();
  branchSourceSide = ChainSide::Left;
  pendingAddSide = ChainSide::Left;
  reconcileGridFromTree(snapshot.getChildWithName("Grid"), retired);
  latencyRefreshSuspended = false;

  scenes.restoreFromValueTree(snapshot.getChildWithName("Scenes"));
  stomps.restoreFromValueTree(snapshot.getChildWithName("Stomps"));

  rebuildTopology();
  // A restore never ends on a plan that does not describe the rows it has
  // just rebuilt: the audio thread would run the old rig's ranges over the
  // new one's blocks, and every later edit would roll back onto the same
  // broken state. The routing was repaired before the compile, so this is
  // not expected; if the compiler refuses all the same, the rig is reduced
  // to what always compiles (rows with a device on both ends) and says so.
  if (!planMatchesLanes()) {
    juce::Logger::writeToLog("[Restore] The restored grid did not compile; its splits and feeds "
                             "are set aside");
    jassertfalse;
    setAllJoinsAside();
    rebuildTopology();
  }
  // A row the restore brings back into the plan may hold engines that sat
  // out the prepares since they were built. Only those: a row that was
  // running keeps its engines as they are (an undo of a knob move must not
  // prewarm every amp again).
  for (int row = 0; row < kGridRows; ++row)
    if (activePlan.rowInfo[static_cast<size_t>(row)].active && !wasActive[static_cast<size_t>(row)])
      prepareChain(row);
  snapGridRowGains();
  syncLegacyControlsFromGrid();

  // A block that changed rows may sit in one of another format now.
  requeueNamEnginesForVoiceCount();
  refreshBlockTailLength();
  bumpChainRevision();
}

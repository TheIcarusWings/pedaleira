#include "Processor.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <utility>

// ##############################
// PLAN EXECUTOR (chain rate)
// ##############################
// Runs the active ProcessingPlan (ProcessingPlan.h) over one invocation of
// the chain stage. Everything here is on the audio thread or on a pool
// worker inside the audio thread's chainMutex hold: no allocation, no
// locks, no logging, nothing that can throw. The plan is a fixed array of
// operations, a buffer view is two pointers on the stack, and a fork's job
// contexts are stack locals that outlive the join.
//
// The plan decides what runs where; the samples come from the same
// per-block loop as ever (processChainOnBuffer). A fork is scheduling only:
// its children write disjoint buffers (validatePlan checks that), so running
// them side by side or one after the other gives the same output bit for
// bit.

int TONE3000Processor::resolvePlanBuffer(const BufRef& ref, const PlanStageIo& io,
                                         float** out) const noexcept {
  // A reference that names nothing is a compiler bug or a stale plan. The
  // operation is skipped, but never silently: the count is there for the
  // tests and the debugger stops here.
  const auto unresolved = [this] {
    unresolvedPlanRefs.fetch_add(1, std::memory_order_relaxed);
    jassertfalse;
    return 0;
  };

  if (ref.kind == BufRef::Row) {
    // A row's own chain-rate buffer (grid plans).
    if (ref.index >= kGridRows)
      return unresolved();
    const juce::AudioBuffer<float>& work = rowWork[ref.index];
    const int numCh = juce::jmin<int>(ref.numCh, 2 - juce::jmin<int>(ref.firstCh, 2));
    if (numCh <= 0 || work.getNumChannels() < ref.firstCh + numCh ||
        work.getNumSamples() < io.numFrames)
      return unresolved();
    for (int ch = 0; ch < numCh; ++ch)
      out[ch] = const_cast<float*>(work.getReadPointer(ref.firstCh + ch));
    return numCh;
  }

  // A bus is written where the stage was told to put it (grid plans); a
  // port is a strip's input in a grid plan and the in-place pair of a
  // legacy one.
  const bool isBus = ref.kind == BufRef::Bus;
  if ((!isBus && ref.kind != BufRef::Port) || ref.index >= io.ports.size())
    return unresolved();
  float* const* port = isBus ? io.busPorts[ref.index] : io.ports[ref.index];
  if (port == nullptr)
    return unresolved();

  int numCh = ref.numCh;
  if (ref.followHostChannels)
    numCh = juce::jmin(numCh, rtChainChannels);
  numCh = juce::jlimit(0, 2 - juce::jmin<int>(ref.firstCh, 2), numCh);
  for (int ch = 0; ch < numCh; ++ch)
    out[ch] = port[ref.firstCh + ch];
  return numCh;
}

// The continuation rule. A block's
// context is the rest of its own row, then, where the row rejoins or feeds
// another, that row from the join on. An amp anywhere in the continuation
// means every block of this segment has an amp after it. A legacy segment
// continues nowhere and scans its whole lane, which is the value the block
// loop always computed for itself.
int TONE3000Processor::planSegmentLastNamIndex(const PlanOp& op) const noexcept {
  for (int c = 0; c < op.numCont && c < static_cast<int>(op.cont.size()); ++c) {
    const PlanOp::Range& range = op.cont[static_cast<size_t>(c)];
    if (range.row < kNumLanes &&
        lastActiveNamIndex(lanes[range.row], range.begin, range.end) >= 0)
      return INT_MAX;
  }
  return lastActiveNamIndex(lanes[op.row], op.scanBegin, op.scanEnd);
}

bool TONE3000Processor::planSeqHasWork(const ProcessingPlan& plan, int seq) const noexcept {
  if (seq < 0 || seq >= plan.numSeqs)
    return false;
  const PlanSeq& s = plan.seqs[static_cast<size_t>(seq)];
  const int last = juce::jmin<int>(s.firstOp + s.numOps, plan.numOps);
  for (int i = s.firstOp; i < last; ++i) {
    const PlanOp& op = plan.ops[static_cast<size_t>(i)];
    if (op.type == PlanOp::RunBlocks && op.row < kNumLanes &&
        laneHasWork(lanes[op.row], op.begin, op.end))
      return true;
    if (op.type == PlanOp::Fork)
      for (int c = 0; c < op.numChildren && c < static_cast<int>(op.child.size()); ++c)
        if (planSeqHasWork(plan, op.child[static_cast<size_t>(c)]))
          return true;
  }
  return false;
}

void TONE3000Processor::runPlanSeq(const ProcessingPlan& plan, int seq, const PlanStageIo& io) {
  if (seq < 0 || seq >= plan.numSeqs)
    return;
  const PlanSeq& s = plan.seqs[static_cast<size_t>(seq)];
  const int last = juce::jmin<int>(s.firstOp + s.numOps, plan.numOps);

  for (int i = s.firstOp; i < last; ++i) {
    const PlanOp& op = plan.ops[static_cast<size_t>(i)];
    switch (op.type) {
      case PlanOp::RunBlocks: {
        float* channels[2] = {nullptr, nullptr};
        const int numCh = resolvePlanBuffer(op.a, io, channels);
        if (numCh <= 0 || op.row >= kNumLanes)
          break;
        // A view on the stage's own channels (a legacy lane processes in
        // place, no copies in or out) or on the row's buffer.
        juce::AudioBuffer<float> view(channels, numCh, io.numFrames);
        SegmentCtx segment;
        segment.lastNamIndex = planSegmentLastNamIndex(op);
        segment.dualMono = op.followRigDualMono
                               ? rtDualMono
                               : static_cast<RowFormat>(op.format) == RowFormat::DualMono;
        processChainOnBuffer(lanes[op.row], view, laneDryScratch[op.row], op.begin, op.end,
                             segment);
        break;
      }

      case PlanOp::Copy: {
        float* from[2] = {nullptr, nullptr};
        float* to[2] = {nullptr, nullptr};
        const int numCh =
            juce::jmin(resolvePlanBuffer(op.a, io, from), resolvePlanBuffer(op.b, io, to));
        for (int ch = 0; ch < numCh; ++ch)
          std::memcpy(to[ch], from[ch], sizeof(float) * static_cast<size_t>(io.numFrames));
        break;
      }

      case PlanOp::Delay: {
        // Block latency: the path that arrives early waits for the others
        // it is heard with (see refreshLatencyCompensation). After the
        // join, on the thread that ran the fork. A pass-through at zero.
        float* channels[2] = {nullptr, nullptr};
        const int numCh = resolvePlanBuffer(op.a, io, channels);
        if (numCh > 0 && op.row < kGridRows)
          grid.rows[op.row].outAlign.process(channels, numCh, io.numFrames);
        break;
      }

      case PlanOp::Fork:
        runPlanFork(plan, op, io);
        break;

      case PlanOp::RowIn:
        runRowIn(op, io);
        break;

      case PlanOp::RowOut:
        runRowOut(op, io);
        break;

      case PlanOp::Split:
        runSplit(op, io, /*mix=*/false);
        break;

      case PlanOp::AlignMix:
        runSplit(op, io, /*mix=*/true);
        break;

      case PlanOp::FeedRow:
        runFeedRow(op, io);
        break;

      default:
        unresolvedPlanRefs.fetch_add(1, std::memory_order_relaxed);
        jassertfalse;
        break;
    }
  }
}

// Fork/join over the plan's parallel sections (see RtWorkerPool.h). The job
// contexts live on this stack frame and stay valid until forkJoin returns;
// the lambda decays to a plain function pointer, so forking allocates
// nothing on the RT path. The first child that carries work runs on this
// thread; the others go to pool workers, or are stolen back inline when no
// worker picks them up.
void TONE3000Processor::runPlanFork(const ProcessingPlan& plan, const PlanOp& fork,
                                    const PlanStageIo& io) {
  const int numChildren = juce::jmin<int>(fork.numChildren, static_cast<int>(fork.child.size()));

  struct SeqJob {
    TONE3000Processor* proc;
    const ProcessingPlan* plan;
    const PlanStageIo* io;
    int seq;
  };
  SeqJob jobs[4];
  void* ctxs[4];
  bool hasWork[4] = {false, false, false, false};
  int numJobs = 0;

  // The fork is worth a handoff only when at least two children carry work:
  // dispatching an idle sequence costs more than the empty loop it hides.
  // rtPhasePool is this callback's "may fork" answer (multi-core on, workers
  // up).
  if (rtPhasePool != nullptr)
    for (int c = 0; c < numChildren; ++c) {
      hasWork[c] = planSeqHasWork(plan, fork.child[static_cast<size_t>(c)]);
      if (hasWork[c]) {
        jobs[numJobs] = {this, &plan, &io, fork.child[static_cast<size_t>(c)]};
        ctxs[numJobs] = &jobs[numJobs];
        ++numJobs;
      }
    }

  if (numJobs < 2) {
    for (int c = 0; c < numChildren; ++c)
      runPlanSeq(plan, fork.child[static_cast<size_t>(c)], io);
    return;
  }

  // Idle children still run (bypassed blocks keep their delays and meters
  // going), inline and first: they are cheap and touch nothing the jobs do.
  for (int c = 0; c < numChildren; ++c)
    if (!hasWork[c])
      runPlanSeq(plan, fork.child[static_cast<size_t>(c)], io);

  rtPhasePool->forkJoin(
      [](void* ctx) {
        const auto& job = *static_cast<const SeqJob*>(ctx);
        job.proc->runPlanSeq(*job.plan, job.seq, *job.io);
      },
      ctxs, numJobs);
}

// ##############################
// ROW INPUT AND ROW OUTPUT (grid plans)
// ##############################

static float rowPeakDb(float peak) noexcept {
  return peak > 0.0f ? std::max(-60.0f, juce::Decibels::gainToDecibels(peak)) : -60.0f;
}

// The gains a row's send is heading for.
// A mono row is placed with the constant-power law the legacy pans use, so
// a hard pan is unity on its side; a stereo or dual-mono row keeps both
// channels and the pan is a balance, unity at the centre, the far side
// falling to zero. Mute, a solo elsewhere and the volume's floor all mean
// zero; polarity is the sign.
std::pair<float, float> TONE3000Processor::rowSendGains(int row,
                                                        bool silencedBySolo) const noexcept {
  const GridRow& gridRow = grid.rows[static_cast<size_t>(row)];
  const RowOutput& output = gridRow.output;
  const float volumeDb = output.volumeDb.load(std::memory_order_relaxed);
  // A value that is not a number never reaches a gain (the setters and the
  // state reader keep them out; this is the last line): the row is silent
  // rather than a NaN on its bus, which would stay in every filter after it.
  const bool audible = !output.mute.load(std::memory_order_relaxed) && !silencedBySolo &&
                       std::isfinite(volumeDb) && volumeDb > kRowVolumeMinDb;
  if (!audible)
    return {0.0f, 0.0f};

  float level = juce::Decibels::decibelsToGain(juce::jmin(volumeDb, kRowVolumeMaxDb));
  if (output.invert.load(std::memory_order_relaxed))
    level = -level;
  const float storedPan = output.pan.load(std::memory_order_relaxed);
  const float pan = std::isfinite(storedPan) ? juce::jlimit(0.0f, 1.0f, storedPan) : 0.5f;
  // The format the row runs in (a split can make it dual mono), which is
  // also how many channels its buffer carries.
  if (static_cast<RowFormat>(activePlan.rowInfo[static_cast<size_t>(row)].format) ==
      RowFormat::Mono) {
    const float angle = pan * juce::MathConstants<float>::halfPi;
    return {level * std::cos(angle), level * std::sin(angle)};
  }
  return {level * juce::jmin(1.0f, 2.0f * (1.0f - pan)), level * juce::jmin(1.0f, 2.0f * pan)};
}

void TONE3000Processor::runRowIn(const PlanOp& op, const PlanStageIo& io) noexcept {
  if (op.row >= kGridRows)
    return;
  GridRow& gridRow = grid.rows[op.row];
  float* dest[2] = {nullptr, nullptr};
  const int numCh = resolvePlanBuffer(op.b, io, dest);
  if (numCh <= 0)
    return;
  const size_t bytes = sizeof(float) * static_cast<size_t>(io.numFrames);

  // A missing input channel is silence: the row still runs, so its blocks'
  // tails and meters behave.
  float* const* source = op.a.numCh > 0 && op.a.index < io.ports.size() ? io.ports[op.a.index]
                                                                        : nullptr;
  if (source == nullptr) {
    for (int ch = 0; ch < numCh; ++ch)
      std::memset(dest[ch], 0, bytes);
  } else {
    // A mono row takes the strip's first channel (a mono source is on both,
    // a folded pair too); a stereo or dual-mono row takes the pair.
    for (int ch = 0; ch < numCh; ++ch)
      std::memcpy(dest[ch], source[juce::jmin<int>(op.a.firstCh + ch, 1)], bytes);
  }

  // The row's own trim, smoothed. At 0 dB and at rest the samples are not
  // touched: a promoted legacy rig stays bit for bit what it was.
  const float trimDb = gridRow.trimDb.load(std::memory_order_relaxed);
  const float trim = std::isfinite(trimDb) ? juce::Decibels::decibelsToGain(trimDb) : 1.0f;
  gridRow.trimGain.setTargetValue(trim);
  const bool trimMoving = gridRow.trimGain.isSmoothing();
  if (trimMoving || !juce::exactlyEqual(trim, 1.0f)) {
    for (int i = 0; i < io.numFrames; ++i) {
      const float g = gridRow.trimGain.getNextValue();
      for (int ch = 0; ch < numCh; ++ch)
        dest[ch][i] *= g;
    }
  }

  float peaks[2] = {0.0f, 0.0f};
  for (int ch = 0; ch < numCh; ++ch)
    for (int i = 0; i < io.numFrames; ++i)
      peaks[ch] = std::max(peaks[ch], std::abs(dest[ch][i]));
  gridRow.meterIn[0].store(rowPeakDb(peaks[0]), std::memory_order_relaxed);
  gridRow.meterIn[1].store(rowPeakDb(numCh > 1 ? peaks[1] : peaks[0]), std::memory_order_relaxed);
}

void TONE3000Processor::runRowOut(const PlanOp& op, const PlanStageIo& io) noexcept {
  if (op.row >= kGridRows)
    return;
  GridRow& gridRow = grid.rows[op.row];
  float* source[2] = {nullptr, nullptr};
  float* bus[2] = {nullptr, nullptr};
  const int numCh = resolvePlanBuffer(op.a, io, source);
  if (numCh <= 0 || resolvePlanBuffer(op.b, io, bus) < 2)
    return;
  const int numFrames = io.numFrames;
  const size_t bytes = sizeof(float) * static_cast<size_t>(numFrames);

  // Block latency: this row waits for the latest of the rows it is heard
  // with (see refreshLatencyCompensation). Before the gains, so a muted row
  // keeps its line filled and comes back in step. A pass-through at zero.
  gridRow.outAlign.process(source, numCh, numFrames);

  const auto [targetL, targetR] =
      rowSendGains(op.row, rowSilencedBySolo(op.row, rtGridSoloMask));
  gridRow.gainL.setTargetValue(targetL);
  gridRow.gainR.setTargetValue(targetR);
  const bool moving = gridRow.gainL.isSmoothing() || gridRow.gainR.isSmoothing();
  const float* left = source[0];
  const float* right = numCh > 1 ? source[1] : source[0];
  float peakL = 0.0f, peakR = 0.0f;

  // At rest and silent (muted, soloed out, volume at its floor): nothing to
  // send. The first writer of a bus still has to leave it clean.
  if (!moving && juce::exactlyEqual(targetL, 0.0f) && juce::exactlyEqual(targetR, 0.0f)) {
    if (op.firstOnBus) {
      std::memset(bus[0], 0, bytes);
      std::memset(bus[1], 0, bytes);
    }
  } else if (!moving && numCh > 1 && std::abs(targetL - 1.0f) < 1.0e-4f &&
             std::abs(targetR - 1.0f) < 1.0e-4f) {
    // Identity (the same 1e-4 test the legacy image stage uses): the
    // samples go to the bus as they are. This is what keeps a promoted
    // mono rig bit-exact.
    if (op.firstOnBus) {
      std::memcpy(bus[0], left, bytes);
      std::memcpy(bus[1], right, bytes);
    } else {
      juce::FloatVectorOperations::add(bus[0], left, numFrames);
      juce::FloatVectorOperations::add(bus[1], right, numFrames);
    }
    for (int i = 0; i < numFrames; ++i) {
      peakL = std::max(peakL, std::abs(left[i]));
      peakR = std::max(peakR, std::abs(right[i]));
    }
  } else if (!moving && numCh == 1 &&
             ((std::abs(targetL - 1.0f) < 1.0e-4f && std::abs(targetR) < 1.0e-4f) ||
              (std::abs(targetR - 1.0f) < 1.0e-4f && std::abs(targetL) < 1.0e-4f))) {
    // A mono row panned hard to one side at unity: the same identity, for
    // the constant-power law. The row goes to that side as it is and the
    // other side gets nothing, which is what the legacy image stage does
    // with two lanes at their default pans (it skips its matrix).
    const int side = std::abs(targetL - 1.0f) < 1.0e-4f ? 0 : 1;
    if (op.firstOnBus) {
      std::memcpy(bus[side], left, bytes);
      std::memset(bus[1 - side], 0, bytes);
    } else {
      juce::FloatVectorOperations::add(bus[side], left, numFrames);
    }
    float peak = 0.0f;
    for (int i = 0; i < numFrames; ++i)
      peak = std::max(peak, std::abs(left[i]));
    (side == 0 ? peakL : peakR) = peak;
  } else {
    for (int i = 0; i < numFrames; ++i) {
      const float l = left[i] * gridRow.gainL.getNextValue();
      const float r = right[i] * gridRow.gainR.getNextValue();
      if (op.firstOnBus) {
        bus[0][i] = l;
        bus[1][i] = r;
      } else {
        bus[0][i] += l;
        bus[1][i] += r;
      }
      peakL = std::max(peakL, std::abs(l));
      peakR = std::max(peakR, std::abs(r));
    }
  }
  gridRow.meterOut[0].store(rowPeakDb(peakL), std::memory_order_relaxed);
  gridRow.meterOut[1].store(rowPeakDb(peakR), std::memory_order_relaxed);
}

// ##############################
// SPLITS AND ROW FEEDS (grid plans)
// ##############################

unsigned TONE3000Processor::gridSoloMask() const noexcept {
  unsigned mask = 0;
  for (int row = 0; row < kGridRows; ++row) {
    const ProcessingPlan::RowInfo& info = activePlan.rowInfo[static_cast<size_t>(row)];
    // A row that ends in a mixer has no send of its own to solo.
    if (info.active && info.sink != ProcessingPlan::RowInfo::ToMix &&
        grid.rows[static_cast<size_t>(row)].output.solo.load(std::memory_order_relaxed))
      mask |= 1u << row;
  }
  return mask;
}

float TONE3000Processor::rowFeedGain(int row) const noexcept {
  const RowOutput& output = grid.rows[static_cast<size_t>(row)].output;
  const float volumeDb = output.volumeDb.load(std::memory_order_relaxed);
  if (output.mute.load(std::memory_order_relaxed) || !std::isfinite(volumeDb) ||
      volumeDb <= kRowVolumeMinDb)
    return 0.0f;
  const float level = juce::Decibels::decibelsToGain(juce::jmin(volumeDb, kRowVolumeMaxDb));
  return output.invert.load(std::memory_order_relaxed) ? -level : level;
}

// A split's fork (the upper row's buffer onto the lower row's, through the
// splitter) or its rejoin (both paths aligned, then mixed into the upper
// row's buffer). The junction does the work; this hands it the two buffers
// and says how to read them. Its filters, ramps and delay lines are only
// ever touched here, on the thread that runs the sequence the operation is
// in, between the fork that follows a Split and the join that precedes an
// AlignMix.
void TONE3000Processor::runSplit(const PlanOp& op, const PlanStageIo& io, bool mix) noexcept {
  SplitJunction* junction =
      op.junction >= 0 && op.junction < kGridRows ? rtSplits[static_cast<size_t>(op.junction)]
                                                  : nullptr;
  float* upper[2] = {nullptr, nullptr};
  float* lower[2] = {nullptr, nullptr};
  const int upperCh = resolvePlanBuffer(op.a, io, upper);
  const int lowerCh = resolvePlanBuffer(op.b, io, lower);
  if (junction == nullptr || upperCh <= 0 || lowerCh <= 0) {
    if (junction == nullptr) {
      unresolvedPlanRefs.fetch_add(1, std::memory_order_relaxed);
      jassertfalse;
    }
    return;
  }
  juce::AudioBuffer<float> upperView(upper, upperCh, io.numFrames);
  juce::AudioBuffer<float> lowerView(lower, lowerCh, io.numFrames);
  const SplitFormat format{upperCh, lowerCh,
                           static_cast<RowFormat>(op.format) == RowFormat::DualMono,
                           static_cast<RowFormat>(op.format2) == RowFormat::DualMono};
  if (mix)
    junction->mix(upperView, lowerView, io.numFrames, format);
  else
    junction->split(upperView, lowerView, io.numFrames, format);
}

// One row's output into another row's buffer: the feeding row waits for the
// latest of the rows that feed the same target (its alignment line), then
// goes in at its volume, with its mute and polarity. The first feed starts
// the target's buffer and the others add to it. At 0 dB and at rest the
// samples are copied as they are, so [X] feeding [Y] is the same audio as X
// and Y in one row.
//
// Channels follow the conventions a signal entering a row always has: a
// mono row onto both channels of a wider one, a wider row into a mono one
// as half of (L + R), and otherwise channel for channel.
void TONE3000Processor::runFeedRow(const PlanOp& op, const PlanStageIo& io) noexcept {
  if (op.row >= kGridRows || op.row2 >= kGridRows)
    return;
  GridRow& feeder = grid.rows[op.row];
  GridRow& target = grid.rows[op.row2];
  float* source[2] = {nullptr, nullptr};
  float* dest[2] = {nullptr, nullptr};
  const int sourceCh = resolvePlanBuffer(op.a, io, source);
  const int destCh = resolvePlanBuffer(op.b, io, dest);
  if (sourceCh <= 0 || destCh <= 0)
    return;
  const int numFrames = io.numFrames;
  const size_t bytes = sizeof(float) * static_cast<size_t>(numFrames);

  feeder.outAlign.process(source, sourceCh, numFrames);

  const float gain = rowFeedGain(op.row);
  feeder.gainL.setTargetValue(gain);
  // gainR is not used by a feed; it is kept at the level so that a row
  // which goes back to a device output starts from where it is.
  feeder.gainR.setCurrentAndTargetValue(gain);
  const bool moving = feeder.gainL.isSmoothing();
  const bool silent = !moving && juce::exactlyEqual(gain, 0.0f);
  const bool unity = !moving && juce::exactlyEqual(gain, 1.0f);
  const bool fold = sourceCh == 2 && destCh == 1;
  float peaks[2] = {0.0f, 0.0f};

  if (silent) {
    if (op.firstOnBus)
      for (int ch = 0; ch < destCh; ++ch)
        std::memset(dest[ch], 0, bytes);
  } else if (unity && !fold) {
    for (int ch = 0; ch < destCh; ++ch) {
      const float* from = source[juce::jmin(ch, sourceCh - 1)];
      if (op.firstOnBus)
        std::memcpy(dest[ch], from, bytes);
      else
        juce::FloatVectorOperations::add(dest[ch], from, numFrames);
    }
    for (int ch = 0; ch < sourceCh; ++ch)
      for (int i = 0; i < numFrames; ++i)
        peaks[ch] = std::max(peaks[ch], std::abs(source[ch][i]));
  } else {
    for (int i = 0; i < numFrames; ++i) {
      const float g = feeder.gainL.getNextValue();
      const float left = source[0][i] * g;
      const float right = sourceCh > 1 ? source[1][i] * g : left;
      peaks[0] = std::max(peaks[0], std::abs(left));
      peaks[1] = std::max(peaks[1], std::abs(right));
      if (destCh == 1) {
        const float value = sourceCh > 1 ? 0.5f * (left + right) : left;
        dest[0][i] = op.firstOnBus ? value : dest[0][i] + value;
      } else {
        dest[0][i] = op.firstOnBus ? left : dest[0][i] + left;
        dest[1][i] = op.firstOnBus ? right : dest[1][i] + right;
      }
    }
  }
  feeder.meterOut[0].store(rowPeakDb(peaks[0]), std::memory_order_relaxed);
  feeder.meterOut[1].store(rowPeakDb(sourceCh > 1 ? peaks[1] : peaks[0]),
                           std::memory_order_relaxed);

  // What has entered the target so far; the last feed leaves the total.
  float inPeaks[2] = {0.0f, 0.0f};
  for (int ch = 0; ch < destCh; ++ch)
    for (int i = 0; i < numFrames; ++i)
      inPeaks[ch] = std::max(inPeaks[ch], std::abs(dest[ch][i]));
  target.meterIn[0].store(rowPeakDb(inPeaks[0]), std::memory_order_relaxed);
  target.meterIn[1].store(rowPeakDb(destCh > 1 ? inPeaks[1] : inPeaks[0]),
                          std::memory_order_relaxed);
}

std::string TONE3000Processor::describeActivePlanForTesting() const {
  juce::ScopedLock lock(chainMutex);
  return describePlan(activePlan);
}

std::string TONE3000Processor::validateActivePlanForTesting() const {
  juce::ScopedLock lock(chainMutex);
  if (!planMatchesLanes())
    return "the active plan does not match the lanes";
  return validatePlan(activePlan, makePlanInput());
}

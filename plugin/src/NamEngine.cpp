#include "NamEngine.h"
#include "NAM/slimmable.h"
#include "RtWorkerPool.h"
#include <atomic>
#include <cmath>
#include <stdexcept>

namespace {

// Resampling-bridge blocks are cut to this many chain frames: the resampler's
// history ring holds 2^17 frames, and this keeps a push well inside it at any
// ratio the engine runs.
constexpr int kMaxBridgeBlock = 8192;

bool isPowerOfTwoUpTo(int value, int highest) {
  return value >= 2 && value <= highest && (value & (value - 1)) == 0;
}

}  // namespace

NamEngine::NamEngine(std::vector<std::unique_ptr<nam::DSP>> modelInstances, int factor,
                     int voiceCount)
    : instances(std::move(modelInstances)), oversampleFactor(factor), voices(voiceCount) {
  if (instances.empty())
    throw std::invalid_argument("NamEngine needs at least one model instance");
  for (const auto& instance : instances) {
    if (instance == nullptr)
      throw std::invalid_argument("NAM model cannot be null");
  }
  if (factor < 1)
    throw std::invalid_argument("NamEngine oversampling factor must be >= 1");
  if (voices < 1 || voices > kMaxVoices)
    throw std::invalid_argument("NamEngine voice count must be 1 or 2");
  const auto count = static_cast<int>(instances.size());
  if (count % voices != 0)
    throw std::invalid_argument("NamEngine instance count must be a multiple of the voice count");
  phases = count / voices;
  if (phases != 1 && phases != factor)
    throw std::invalid_argument("NamEngine phases per voice must be 1 or the oversampling factor");
  // The flat voice × phase fork publishes one job per instance.
  if (count > RtWorkerPool::kMaxJobs)
    throw std::invalid_argument("NamEngine instance count exceeds the worker pool's job limit");

  // The rate the model was trained at. NAM writes -1 for "unknown", and
  // files from before the field existed have none: both are 48 kHz.
  const double expected = primary().GetExpectedSampleRate();
  rateDeclared = std::isfinite(expected) && expected > 0.0;
  modelSampleRate = rateDeclared ? std::round(expected) : kChainBaseSampleRate;

  // The bridge between the chain rate and the stream the instances see.
  const double chainRate = kChainBaseSampleRate * oversampleFactor;
  const double streamRate = modelSampleRate * phases;
  if (std::abs(streamRate - chainRate) < 0.5) {
    bridge = RateBridge::none;
  } else if (streamRate > chainRate &&
             std::abs(streamRate / chainRate - std::round(streamRate / chainRate)) < 1e-9 &&
             isPowerOfTwoUpTo(static_cast<int>(std::round(streamRate / chainRate)),
                              ChainOversampler::kMaxFactor)) {
    bridge = RateBridge::raise;
    bridgeFactor = static_cast<int>(std::round(streamRate / chainRate));
  } else if (streamRate < chainRate &&
             std::abs(chainRate / streamRate - std::round(chainRate / streamRate)) < 1e-9 &&
             isPowerOfTwoUpTo(static_cast<int>(std::round(chainRate / streamRate)),
                              ChainOversampler::kMaxFactor) &&
             oversampleFactor % static_cast<int>(std::round(chainRate / streamRate)) == 0) {
    // Chain buffers are whole multiples of the chain's factor, so a ratio
    // that divides it always splits them evenly.
    bridge = RateBridge::lower;
    bridgeFactor = static_cast<int>(std::round(chainRate / streamRate));
  } else {
    bridge = RateBridge::resample;
  }
}

NamEngine::~NamEngine() = default;
NamEngine::NamEngine(NamEngine&&) noexcept = default;
NamEngine& NamEngine::operator=(NamEngine&&) noexcept = default;

int NamEngine::bridgeUnderruns() const {
  return resampler != nullptr ? resampler->underrunCount() : 0;
}

void NamEngine::primeIfNeeded(nam::DSP& instance, int capacity) {
  if (instance.GetPrewarmSamples() > 0 || capacity <= 0)
    return;
  std::vector<double> silence(static_cast<size_t>(capacity), 0.0);
  std::vector<double> ignored(static_cast<size_t>(capacity), 0.0);
  NAM_SAMPLE* ins[] = {silence.data()};
  NAM_SAMPLE* outs[] = {ignored.data()};
  instance.process(ins, outs, capacity);
}

void NamEngine::prepare(int newMaxBlockSize) {
  maxBlockSize = juce::jmax(1, newMaxBlockSize);
  const double chainRate = kChainBaseSampleRate * oversampleFactor;
  const double streamRate = modelSampleRate * phases;

  latency = 0;
  halfBands.reset();
  resampler.reset();
  switch (bridge) {
    case RateBridge::none:
      maxChunk = maxBlockSize;
      streamCapacity = maxChunk;
      break;
    case RateBridge::raise:
      maxChunk = maxBlockSize;
      streamCapacity = maxChunk * bridgeFactor;
      halfBands = std::make_unique<ChainOversampler>();
      halfBands->prepare(bridgeFactor, maxChunk);
      break;
    case RateBridge::lower:
      // Whole multiples of the ratio, so every pass splits evenly.
      maxChunk = juce::jmax(bridgeFactor, maxBlockSize - maxBlockSize % bridgeFactor);
      streamCapacity = maxChunk / bridgeFactor;
      halfBands = std::make_unique<ChainOversampler>();
      halfBands->prepare(bridgeFactor, streamCapacity);
      break;
    case RateBridge::resample:
      maxChunk = juce::jmin(maxBlockSize, kMaxBridgeBlock);
      resampler = std::make_unique<Resampler>();
      if (!resampler->reset(chainRate, streamRate, maxChunk, 1, 1))
        throw std::runtime_error("NamEngine could not set up its resampler");
      streamCapacity = resampler->maxChainFrames();
      latency = resampler->latencySamples();
      break;
  }
  spareChannel.assign(static_cast<size_t>(maxChunk), 0.0f);

  const auto slotCount = instances.size();
  // +1: a carried phase offset can hand one phase an extra frame when a
  // block isn't divisible by the phase count.
  const size_t perPhaseCapacity =
      static_cast<size_t>(streamCapacity) / static_cast<size_t>(phases) + 1;
  slotInputs.assign(slotCount, std::vector<double>(perPhaseCapacity));
  slotOutputs.assign(slotCount, std::vector<double>(perPhaseCapacity));
  phaseFrames.assign(static_cast<size_t>(phases), 0);
  phaseOffset = 0;

  costSeed = 0.0;
  for (auto& instance : instances) {
    // Timed: the prewarm runs the model over a known number of samples, the
    // one measure of its cost there is before any audio has passed (the
    // plan CPU value starts from it). The reset's own allocations are in
    // the figure too, so it errs high.
    const double startMs = juce::Time::getMillisecondCounterHiRes();
    instance->ResetAndPrewarm(modelSampleRate, static_cast<int>(perPhaseCapacity));
    const double seconds = (juce::Time::getMillisecondCounterHiRes() - startMs) / 1000.0;
    if (const int prewarmSamples = instance->GetPrewarmSamples(); prewarmSamples > 0)
      costSeed += seconds * modelSampleRate / static_cast<double>(prewarmSamples);

    // SlimmableContainer / SlimmableWavenet: after max buffer size is set on the
    // root DSP, SetSlimmableSize re-selects the active tier and ResetAndPrewarms
    // it (see NAM render tools and NeuralAmpModelerCore tests). Without this,
    // container models can stay on a stale or uninitialized sub-path.
    if (auto* slimmable = dynamic_cast<nam::SlimmableModel*>(instance.get()))
      slimmable->SetSlimmableSize(requestedSlimmableSize);
    primeIfNeeded(*instance, static_cast<int>(perPhaseCapacity));
  }

  isPrepared = true;
}

void NamEngine::setSlimmableSize(double val) {
  requestedSlimmableSize = juce::jlimit(0.0, 1.0, val);
  if (!isPrepared)
    return;
  for (auto& instance : instances) {
    if (auto* slimmable = dynamic_cast<nam::SlimmableModel*>(instance.get())) {
      slimmable->SetSlimmableSize(requestedSlimmableSize);
      // A tier change resets the new tier; one that does not prewarm is
      // primed here, not on its first audio call.
      primeIfNeeded(*instance, static_cast<int>(slotInputs.front().size()));
    }
  }
}

void NamEngine::processStream(const float* const* in, float* const* out, int activeVoices,
                              int frames, RtWorkerPool* pool) {
  const int jobCount = activeVoices * phases;

  // Deinterleave each active channel into its voice's per-phase streams
  // (single phase = one stream = a straight copy). This doubles as the
  // float → double conversion. Both voices see the same phase walk, so
  // phaseFrames is computed once from voice 0.
  std::fill(phaseFrames.begin(), phaseFrames.end(), 0);
  for (int v = 0; v < activeVoices; ++v) {
    const float* channel = in[v];
    int phase = phaseOffset;
    int counts[RtWorkerPool::kMaxJobs] = {};
    for (int i = 0; i < frames; ++i) {
      slotInputs[static_cast<size_t>(slotIndex(v, phase))][static_cast<size_t>(counts[phase]++)] =
          static_cast<double>(channel[i]);
      if (++phase == phases)
        phase = 0;
    }
    if (v == 0)
      for (int p = 0; p < phases; ++p)
        phaseFrames[static_cast<size_t>(p)] = counts[p];
  }

  if (pool != nullptr && jobCount > 1) {
    // Fork every (voice, phase) job across the pool (see the header
    // comment): each job touches only its own instance and buffers, so
    // any schedule produces the same bits as the serial loop below.
    // Exceptions can't cross threads, so a job traps its own and the
    // joiner rethrows once all jobs are done; the caller's RT failure
    // handling (disable the block, log off-thread) works exactly as for
    // a serial throw.
    struct SlotJob {
      nam::DSP* model;
      NAM_SAMPLE* input;
      NAM_SAMPLE* output;
      int frames;
      std::atomic<bool>* failed;
    };
    std::atomic<bool> jobFailed{false};
    jassert(jobCount <= RtWorkerPool::kMaxJobs);  // enforced by the constructor
    SlotJob jobs[RtWorkerPool::kMaxJobs];
    void* ctxs[RtWorkerPool::kMaxJobs];
    for (int v = 0; v < activeVoices; ++v) {
      for (int p = 0; p < phases; ++p) {
        const int job = slotIndex(v, p);
        const auto slot = static_cast<size_t>(job);
        jobs[job] = {instances[slot].get(), slotInputs[slot].data(), slotOutputs[slot].data(),
                     phaseFrames[static_cast<size_t>(p)], &jobFailed};
        ctxs[job] = &jobs[job];
      }
    }
    pool->forkJoin(
        [](void* ctx) {
          auto& j = *static_cast<SlotJob*>(ctx);
          if (j.frames <= 0)
            return;
          try {
            NAM_SAMPLE* inputPtrs[] = {j.input};
            NAM_SAMPLE* outputPtrs[] = {j.output};
            j.model->process(inputPtrs, outputPtrs, j.frames);
          } catch (...) {
            j.failed->store(true, std::memory_order_release);
          }
        },
        ctxs, jobCount);
    if (jobFailed.load(std::memory_order_acquire))
      throw std::runtime_error("NAM phase processing failed");
  } else {
    for (int v = 0; v < activeVoices; ++v) {
      for (int p = 0; p < phases; ++p) {
        const int count = phaseFrames[static_cast<size_t>(p)];
        if (count <= 0)
          continue;
        const auto slot = static_cast<size_t>(slotIndex(v, p));
        NAM_SAMPLE* inputPtrs[] = {slotInputs[slot].data()};
        NAM_SAMPLE* outputPtrs[] = {slotOutputs[slot].data()};
        instances[slot]->process(inputPtrs, outputPtrs, count);
      }
    }
  }

  // Reinterleave each voice back onto its channel (and convert to float).
  for (int v = 0; v < activeVoices; ++v) {
    float* channel = out[v];
    int phase = phaseOffset;
    int counts[RtWorkerPool::kMaxJobs] = {};
    for (int i = 0; i < frames; ++i) {
      channel[i] = static_cast<float>(
          slotOutputs[static_cast<size_t>(slotIndex(v, phase))][static_cast<size_t>(counts[phase]++)]);
      if (++phase == phases)
        phase = 0;
    }
  }

  phaseOffset = (phaseOffset + frames) % phases;
}

void NamEngine::process(juce::AudioBuffer<float>& buffer, RtWorkerPool* pool, bool dualMono) {
  if (!isPrepared || maxBlockSize < 1) {
    throw std::runtime_error("NamEngine must be prepared before processing");
  }

  const int numSamples = buffer.getNumSamples();
  const int numChannels = buffer.getNumChannels();

  // Which channels get their own voice this call: channel 0 always; channel
  // 1 only in dual mono, on a two-voice engine, with a stereo buffer. Every
  // other combination is the classic mono path (voice 0, then fan out), so a
  // single-voice engine is unaffected by the flag and a two-voice engine
  // never processes a mirrored channel twice for nothing.
  const int activeVoices = (dualMono && voices > 1 && numChannels > 1) ? 2 : 1;

  // NAM models are mono: each active voice processes its own channel.
  // Buffers larger than the prepared size are run in prepared-size slices
  // (models stream statefully, so slicing is exact); throwing here would
  // permanently disable the block when a startup prepare races the host's
  // actual block size.
  for (int offset = 0; offset < numSamples; offset += maxChunk) {
    const int chunk = juce::jmin(maxChunk, numSamples - offset);
    // Two channels for the bridges; channel 1 is the spare when only voice 0
    // runs (its contents are never read back).
    float* channels[2] = {buffer.getWritePointer(0) + offset,
                          activeVoices > 1 ? buffer.getWritePointer(1) + offset
                                           : spareChannel.data()};

    switch (bridge) {
      case RateBridge::none:
        processStream(channels, channels, activeVoices, chunk, pool);
        break;
      case RateBridge::raise:
        halfBands->process(channels, channels, chunk,
                           [this, activeVoices, pool](float** streamIn, float** streamOut,
                                                      int frames) {
                             processStream(streamIn, streamOut, activeVoices, frames, pool);
                           });
        break;
      case RateBridge::lower:
        jassert(chunk % bridgeFactor == 0);  // chain buffers are multiples of the factor
        halfBands->processBaseRateIsland(
            channels, activeVoices, chunk,
            [this, activeVoices, pool](float* const* stream, int frames) {
              processStream(stream, stream, activeVoices, frames, pool);
            });
        break;
      case RateBridge::resample: {
        const float* const inPort[2] = {channels[0], channels[1]};
        float* const outPort[2] = {channels[0], channels[1]};
        const float* const* const ins[1] = {inPort};
        float* const* const outs[1] = {outPort};
        resampler->process(ins, outs, chunk,
                           [this, activeVoices, pool](float* const* const* streamIn,
                                                      float* const* const* streamOut, int frames) {
                             processStream(streamIn[0], streamOut[0], activeVoices, frames, pool);
                           });
        break;
      }
    }
  }

  // Mono path: fan voice 0's result out to the other channels. In dual mono
  // channel 1 already carries voice 1's output and is left alone.
  for (int ch = activeVoices; ch < numChannels; ++ch) {
    buffer.copyFrom(ch, 0, buffer, 0, 0, numSamples);
  }
}

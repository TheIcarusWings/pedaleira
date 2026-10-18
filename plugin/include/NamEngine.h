#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <vector>
#include "NAM/dsp.h"
#include "ChainBoundaryMulti.h"
#include "ChainDomain.h"
#include "ChainOversampler.h"
#include "SincResampler.h"

/**
 * A JUCE-compatible host for a nam::DSP model, running in the chain domain
 * (kChainBaseSampleRate × oversampling factor; see ChainDomain.h). It does:
 *  - float ↔ double conversion (NAM models process NAM_SAMPLE == double),
 *  - mono processing with fan-out to stereo buffers,
 *  - slimmable (A2 container) tier selection,
 *  - phase-interleaved oversampled processing (below),
 *  - running the model at the rate it was trained at (below).
 *
 * Phase-interleaved oversampling:
 * A NAM model oversampled by N with its convolution dilations scaled by N is
 * mathematically identical to N independent copies of the *unscaled* model,
 * each processing every Nth sample of the oversampled stream at the native
 * rate: every scaled dilation lands taps exactly N samples apart (within one
 * phase), and 1×1 convolutions/activations are per-sample. So instead of
 * patching dilation scaling into NeuralAmpModelerCore, the engine holds N
 * instances of the same model and interleaves them. The receptive field stays
 * constant in seconds (the model sounds the same) while its nonlinear
 * harmonics land in the widened band where the chain's decimation filter
 * removes them instead of letting them alias.
 *
 * Recurrent architectures (LSTM) update state on consecutive samples and
 * can't be phase-split; the loader gives them a single instance, which runs
 * at its own rate like every other model (next section): in an oversampled
 * chain the engine lowers the rate around it and raises it back.
 *
 * The model's rate:
 * A model is a function of its input at the rate it was trained at, R (the
 * file's sample_rate; 48 kHz when the file names none, as NAM assumes). Run
 * at any other rate, its time constants and its whole frequency response
 * move with the ratio. So the stream the instances see runs at
 *
 *     T = R × phases            (R per instance)
 *
 * and the engine bridges the chain rate C and T around them:
 *
 *   none       T == C: the buffer goes straight in (every A2 capture, all
 *              48 kHz, in a chain at any factor);
 *   raise      T = k × C, k = 2, 4 or 8: the chain oversampler's half-band
 *              stages raise the rate and lower it back (a 96 kHz model);
 *   lower      C = k × T, k dividing the chain's factor: the same stages the
 *              other way round, as an IR island does (an LSTM in an
 *              oversampled chain);
 *   resample   any other ratio (a 44.1 kHz model): a band-limited polyphase
 *              resampler (SincResampler) on the multi-port boundary's
 *              push/pop schedule, one port, a channel per voice.
 *
 * The half-band stages are minimum phase and add no latency. The resampler
 * is linear phase and does: latencySamples() reports it (in chain-rate
 * samples, measured the way the host boundary is), and the block reports it
 * to the chain, which delays the dry share of its mix and the other paths to
 * match.
 *
 * Voices (dual mono):
 * A NAM model is mono, so by default channel 0 goes through the model and
 * the result is fanned out to channel 1. With two voices the engine holds a
 * second, independent set of phase instances for channel 1, so a stereo
 * buffer is processed as two separate mono signals through the same model
 * (the mono chain's "Dual Mono" input mode; see InputMode in Processor.h).
 * The voice count is fixed at construction like the phase count: the
 * processor rebuilds the engine when the mode's requirement changes (a
 * dormant voice would resume with stale model history, and nam::DSP has no
 * RT-safe reset). process() only *uses* voice 1 when asked (`dualMono`)
 * and the buffer is stereo; otherwise voice 0 fans out exactly as a
 * single-voice engine does, so the two schedules never mix channels.
 *
 * Multi-core:
 * Every (voice, phase) instance is fully independent (separate model,
 * disjoint I/O buffers), so process() forks them as one flat job set across
 * the processor's RtWorkerPool (the same pool that forks the stereo lanes; a
 * lane job forking its phases is the pool's supported one-deep nesting; a
 * dual-mono engine runs in mono chain mode, where no lane fork exists, so
 * the voice×phase fork never nests). Scheduling is the only thing that
 * changes: deinterleave/reinterleave and the rate bridge stay on the calling
 * thread and every job writes only its own buffers, so parallel output is
 * bit-identical to serial. Passing no pool runs the sequential loop.
 */
class RtWorkerPool;

class NamEngine {
public:
  /** Largest voice count an engine can carry (channel 0 and channel 1). */
  static constexpr int kMaxVoices = 2;

  /** How the engine bridges the chain rate and its model's rate (see the
      header comment). */
  enum class RateBridge { none, raise, lower, resample };

  /** Takes ownership of the model instances, all built from the same model
      config, laid out voice-major: `voices` groups of `phases` instances,
      where `phases` is 1 or `oversampleFactor`. The chain runs at
      kChainBaseSampleRate × `oversampleFactor`; N > 1 phases run
      phase-interleaved. Every instance runs at the model's own rate, and the
      bridge that takes at this chain rate is chosen here. The loader picks
      both counts: `oversampleFactor` phases for phase-safe architectures, one
      otherwise; two voices for the dual-mono input mode, one otherwise.
      Throws std::invalid_argument on empty/null instances, an invalid
      factor or voice count, or a size that isn't voices × {1, factor}. */
  NamEngine(std::vector<std::unique_ptr<nam::DSP>> instances, int oversampleFactor,
            int voices = 1);
  ~NamEngine();

  NamEngine(const NamEngine&) = delete;
  NamEngine& operator=(const NamEngine&) = delete;
  NamEngine(NamEngine&&) noexcept;
  NamEngine& operator=(NamEngine&&) noexcept;

  /** Prepare for processing in the chain domain. `maxBlockSize` is the
      largest per-call frame count (the chain-domain block size). Resets (and
      prewarms) every instance and the rate bridge. Allocates. */
  void prepare(int maxBlockSize);

  /** Process a chain-domain buffer in place. Default: channel 0 through
      voice 0, fanned out to channel 1 if present. With `dualMono` set on a
      two-voice engine and a stereo buffer, channel 1 goes through voice 1
      instead (two independent mono signals through the same model); any
      other combination falls back to the fan-out, so a single-voice engine
      or a mono buffer behaves identically whatever `dualMono` says. Must be
      prepared first. With a pool and more than one (voice, phase) job, the
      jobs fork across the pool's workers (bit-identical to the serial loop;
      see the header comment); a job failure rethrows as std::runtime_error
      on this thread once every job has completed. `pool` may be null.
      Realtime-safe: no allocation, no locks. */
  void process(juce::AudioBuffer<float>& buffer, RtWorkerPool* pool = nullptr,
               bool dualMono = false);

  /** The rate the model runs at: the rate its file says it was trained at,
      or 48 kHz when the file does not say (see modelRateDeclared()). */
  double getModelSampleRate() const { return modelSampleRate; }

  /** The file named the rate (false: 48 kHz is assumed). */
  bool modelRateDeclared() const { return rateDeclared; }

  /** How the chain rate reaches the model's rate. */
  RateBridge getRateBridge() const { return bridge; }

  /** Delay the rate bridge adds, in chain-rate samples: 0 except for the
      resampling bridge. Valid after prepare(). */
  int latencySamples() const { return latency; }

  /** Times the resampling bridge could not fill a block (0 in correct use;
      for tests). */
  int bridgeUnderruns() const;

  /** The oversampling factor this engine was built for. The apply path
      compares it against the live factor: a mismatch (the setting changed
      while the load was in flight) means the phase count is wrong and the
      block must be rebuilt. */
  int getOversampleFactor() const { return oversampleFactor; }

  /** The voice count this engine was built for (1, or 2 for dual mono).
      Compared by the apply path against the live requirement exactly like
      the factor above; a mismatch rebuilds the block. */
  int getVoiceCount() const { return voices; }

  /** Phase instances per voice (1, or the oversampling factor). */
  int getPhaseCount() const { return phases; }

  bool hasInputLevel() const { return primary().HasInputLevel(); }
  double getInputLevel() const { return primary().GetInputLevel(); }
  bool hasOutputLevel() const { return primary().HasOutputLevel(); }
  double getOutputLevel() const { return primary().GetOutputLevel(); }
  bool hasLoudness() const { return primary().HasLoudness(); }
  double getLoudness() const { return primary().GetLoudness(); }

  /**
   * Requested slimmable size (0.0 = lite, 1.0 = full). Clamped to [0.0, 1.0].
   * A no-op for models that aren't SlimmableModel (non-container files).
   * NAM tier mappers assign the boundary value to the tier above (a two-tier
   * container selects lite for [0, 0.5) and full for [0.5, 1.0]), so the lite
   * request must be 0.0; 0.5 would select full.
   * Applied in prepare() and immediately if already prepared. Fans out to
   * every phase instance so all phases always run the same tier.
   */
  void setSlimmableSize(double val);

  double getSlimmableSize() const noexcept { return requestedSlimmableSize; }

  /** The time the last prepare() spent prewarming, per second of the audio
      it prewarmed with, summed over the instances: a rough fraction of one
      core, there before the engine has processed anything. 0 when the
      model does not prewarm. */
  double prewarmCostSeed() const noexcept { return costSeed; }

private:
  /** Instance 0: the reference for metadata queries (all instances share
      one model config, so levels/loudness/rate are identical). */
  nam::DSP& primary() const { return *instances.front(); }

  /** Flat index of (voice, phase) in `instances` and the I/O slot arrays. */
  int slotIndex(int voice, int phase) const noexcept { return voice * phases + phase; }

  /** Run the instances over `frames` frames of the model-rate stream:
      `in[v]` into `out[v]` for each active voice (they may be the same
      pointers). At most streamCapacity frames. */
  void processStream(const float* const* in, float* const* out, int activeVoices, int frames,
                     RtWorkerPool* pool);

  /** A model that does not prewarm (Linear) sizes some of its buffers on
      its first call. One call of silence at the prepared size does that
      here, off the audio thread. Silence leaves such a model as it was: its
      state is its input history, which starts silent. */
  void primeIfNeeded(nam::DSP& instance, int capacity);

  std::vector<std::unique_ptr<nam::DSP>> instances;
  int oversampleFactor = 1;
  int voices = 1;
  int phases = 1;
  double modelSampleRate = kChainBaseSampleRate;
  bool rateDeclared = false;

  RateBridge bridge = RateBridge::none;
  // raise / lower: the power-of-two ratio between the chain and the stream.
  int bridgeFactor = 1;
  std::unique_ptr<ChainOversampler> halfBands;
  using Resampler = BasicChainBoundaryMulti<SincResampler<2>>;
  std::unique_ptr<Resampler> resampler;
  int latency = 0;
  // Chain frames per pass through the bridge.
  int maxChunk = 0;
  // Stream frames per processStream call, at most.
  int streamCapacity = 0;
  // Channel 1's stand-in when only voice 0 runs (a mono buffer, or a stereo
  // one fanned out): the bridges always carry two channels.
  std::vector<float> spareChannel;

  // Per-(voice, phase) double-precision I/O (NAM expects double); the
  // deinterleave IS the float→double conversion. A single instance is just
  // the one-voice, one-phase case of the same path. Indexed by slotIndex;
  // sized in prepare(). phaseFrames is per phase only: both voices split
  // the same chunk with the same phase offset, so their counts are equal.
  std::vector<std::vector<double>> slotInputs;
  std::vector<std::vector<double>> slotOutputs;
  std::vector<int> phaseFrames;

  // Which phase the next incoming sample belongs to. Chain buffers are
  // normally divisible by the phase count (the oversampler guarantees it),
  // but tracking the offset keeps every instance's sample stream continuous
  // when a defensive slice, or the resampling bridge (whose blocks are any
  // length), hands over a partial block.
  int phaseOffset = 0;

  bool isPrepared = false;
  int maxBlockSize = 0;

  double requestedSlimmableSize{1.0};

  double costSeed = 0.0;
};

#pragma once
#include "BlockDelayLine.h"
#include "BlockParams.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_data_structures/juce_data_structures.h>

#include <array>
#include <atomic>
#include <string>

// What a splitter does with the signal it forks.
enum class SplitterType { Balance, AB, Crossover };

// The saved spelling of a SplitterType ("balance", "ab", "crossover"), and
// back. An unknown name leaves `type` alone and returns false, so a state
// written by a newer build can be kept as a placeholder instead of being
// loaded as the wrong thing.
juce::String splitterTypeName(SplitterType type);
bool splitterTypeFromName(const juce::String& name, SplitterType& type);

// How the two buffers handed to SplitJunction::split and ::mix are to be
// read. The buffers may be allocated wider than the rows they stand for, so
// the live channel counts travel here and not in the buffers.
struct SplitFormat {
  // Live channels of the upper (A, and the result) and lower (B) paths: 1
  // or 2. Values outside that are clamped, and never exceed what the buffer
  // has.
  int upperChannels{2};
  int lowerChannels{2};
  // The path carries two independent signals on its two channels (a
  // RowFormat::DualMono row), so nothing may pan or cross-feed them: the
  // pans do nothing on that path and, when it is the upper one, the stereo
  // split option is ignored.
  bool upperDualMono{false};
  bool lowerDualMono{false};

  // The format the buffers themselves show: every channel live, no dual
  // mono.
  static SplitFormat of(const juce::AudioBuffer<float>& upper,
                        const juce::AudioBuffer<float>& lower) noexcept {
    return {upper.getNumChannels(), lower.getNumChannels(), false, false};
  }
};

/**
 * One fork of the grid and, optionally, its rejoin: the splitter that sends
 * the upper row's signal down to the row below, and the mixer that sums the
 * two paths back into the upper row.
 * A is the path that stays in the upper row, B the one in the lower row.
 *
 * It is a plan node, not a block kind: it works on two buffers and adds no
 * latency. It is linear, so it runs at the chain rate with no base-rate
 * island.
 *
 * Threads. The identity and placement fields, prepare, reset, setAlignment
 * and setState belong to the message thread under the chain mutex, like
 * every structural edit. The parameters are atomics: set from any thread,
 * read by the audio thread once per block. split and mix are the audio
 * thread's and neither allocates, locks nor logs.
 *
 * Channel rules (U = upper channels, L = lower channels):
 *
 *   split  U=2 L=2  B gets the source channel for channel.
 *          U=1 L=1  B gets the source.
 *          U=1 L=2  B gets the source on both channels (the convention of a
 *                   mono signal entering a stereo row).
 *          U=2 L=1  B gets the mono fold 0.5 * (L + R), the processor's own
 *                   fold law, so a centred signal keeps its level.
 *          Stereo split needs a stereo upper path (and not a dual mono one):
 *          then A is the left channel on both channels and B the right on
 *          both (on its one channel when L=1). On a mono upper path it does
 *          nothing: there is one signal and both paths get it.
 *
 *   mix    The result has the upper path's width.
 *          U=2 L=2  A and B each through the balance law of their pan.
 *          U=2 L=1  A through the balance law; the mono B is placed by the
 *                   constant-power law, scaled so centre is unity.
 *          U=1 L=1  plain sum; the pans do nothing (a mono result has
 *                   nothing to pan across).
 *          U=1 L=2  B's channels through the balance law of panB, then
 *                   folded 0.5 * (L + R) into the mono result. panA does
 *                   nothing.
 *
 * With every parameter at its default, split followed by mix is exactly
 * 2 * input in all four cases, given a stereo signal that is the same on
 * both channels where a fold is involved.
 */
class SplitJunction {
public:
  // Parameter indices, in table order. The ids ("split.balance" ...) are
  // what saved state, scenes and commands use; the indices are for the
  // audio path and tests.
  enum Param {
    kSplitBalance = 0,
    kSplitLevelA,
    kSplitLevelB,
    kSplitCrossoverHz,
    kSplitSwap,
    kSplitStereo,
    kMixLevelA,
    kMixPanA,
    kMixLevelB,
    kMixPanB,
    kMixInvertB,
    kMixLevel,
    kMixMute,
    kNumParams
  };

  // A level at or below this is off (gain exactly 0), for every level
  // parameter here.
  static constexpr float kLevelFloorDb = -60.0f;
  static constexpr float kLevelMaxDb = 12.0f;
  static constexpr float kCrossoverMinHz = 20.0f;
  static constexpr float kCrossoverMaxHz = 20000.0f;
  // Meters read this when the path is silent, like the block meters.
  static constexpr float kMeterFloorDb = -60.0f;
  // Every gain, the stereo-split and swap crossfades and the crossover
  // frequency take this long to reach a new value.
  static constexpr double kRampSeconds = 0.02;

  // The parameters, in real units:
  //   split.balance      -1 (all A) .. 1 (all B), 0 = both at unity
  //   split.levelA/B     dB, kLevelFloorDb = off        (A/B type only)
  //   split.crossoverHz  20 .. 20000                    (Crossover type only)
  //   split.swap         bool: high band to A, low to B (Crossover type only)
  //   split.stereo       bool: A = left, B = right
  //   mix.levelA/B       dB, kLevelFloorDb = off
  //   mix.panA/B         -1 (left) .. 1 (right)
  //   mix.invertB        bool: polarity of B
  //   mix.level          dB, kLevelFloorDb = off
  //   mix.mute           bool
  static const BlockParamTable& parameterSpecs();

  SplitJunction();

  // ---- Identity and placement: plain data, message thread, chain mutex ----
  std::string id;
  // The row the fork leaves from and the rejoin returns to; B runs in the
  // row below.
  int upperRow{0};
  // Column boundaries, 0..8. mixCol -1 = no rejoin (B runs to its own
  // output and mix is never called).
  int splitCol{0};
  int mixCol{-1};
  // Changing the type is a structural edit: do it under the chain-edit
  // fade. The splitter restarts its filters and takes the new law at once
  // on the next block instead of gliding between two unrelated laws.
  SplitterType type{SplitterType::Balance};

  bool hasRejoin() const noexcept { return mixCol >= 0; }

  // ---- Parameters: any thread ----
  static int findParameter(const juce::String& paramId);
  float getParameter(int index) const noexcept;
  // Clamped to the parameter's range (a non-finite value becomes the
  // default). An index out of range is ignored.
  void setParameter(int index, float value) noexcept;
  // By id; false (and nothing set) when there is no such parameter. Not for
  // the audio thread: the lookup compares strings.
  bool setParameter(const juce::String& paramId, float value);
  // The default when there is no such parameter.
  float getParameter(const juce::String& paramId) const;

  // ---- State: message thread ----
  // Split id upperRow splitCol mixCol type, with one Param{id, value} child
  // per parameter.
  juce::ValueTree getState() const;
  // Forward compatible: unknown properties and unknown Param ids are
  // ignored, a missing Param takes its default, values are clamped. Returns
  // false and changes nothing when the node is not a Split or its type is
  // one this build does not know, so the caller can keep the raw node.
  bool setState(const juce::ValueTree& state);

  // ---- Lifecycle: message thread, chain mutex ----
  // For the chain rate (base rate times the oversampling factor): the
  // crossover is designed for it and the ramps are timed by it. Resets.
  void prepare(double chainSampleRate, int maxBlockSize);
  // Clears the filters, the alignment delay lines and the meters; the next
  // block takes every parameter at its current value without a ramp.
  void reset();
  // The delays that bring the two paths into step at the mix point, in
  // chain-rate samples (the latency pass: the later path gets 0, the other
  // the difference). Allocates when a delay changes; an unchanged delay
  // keeps its contents.
  void setAlignment(int samplesA, int samplesB);
  int alignmentA() const noexcept { return alignA.delaySamples(); }
  int alignmentB() const noexcept { return alignB.delaySamples(); }

  // ---- Audio thread ----
  // The fork. `upper` holds the source and comes back holding A; B is
  // written to `lower` (its previous contents are ignored). The buffers
  // must not share memory. See the channel rules above.
  void split(juce::AudioBuffer<float>& upper, juce::AudioBuffer<float>& lower, int numFrames,
             const SplitFormat& format) noexcept;
  // The rejoin. Delays both paths by their alignment, then sums A (in
  // `upper`) and B (in `lower`) into `upper`. `lower` is left holding the
  // delayed B. At the default settings this is exactly A + B, with no
  // multiply.
  void mix(juce::AudioBuffer<float>& upper, juce::AudioBuffer<float>& lower, int numFrames,
           const SplitFormat& format) noexcept;

  // ---- Meters: any thread ----
  // Peak of the last block in dB (kMeterFloorDb at silence), per side
  // (0 = A, 1 = B): what the splitter sent down each path, and what
  // reached the mixer from each path (after alignment, before the mixer's
  // levels).
  float splitMeterDb(int side) const noexcept;
  float mixMeterDb(int side) const noexcept;

private:
  // A linear ramp whose first target after a reset is taken at once, so a
  // restored or re-prepared junction starts where its parameters are.
  struct Ramp {
    void prepare(double sampleRate) noexcept;
    void snapOnNextTarget() noexcept { snapNext = true; }
    void setTarget(float target) noexcept;
    float next() noexcept { return value.getNextValue(); }
    float current() const noexcept { return value.getCurrentValue(); }
    bool isSmoothing() const noexcept { return value.isSmoothing(); }
    bool settledAt(float v) const noexcept { return !isSmoothing() && current() == v; }

    juce::LinearSmoothedValue<float> value;
    bool snapNext{true};
  };

  // One 12 dB per octave Butterworth section in the trapezoidal (TPT)
  // state-variable form, with its state in double: at 20 Hz and an 8x
  // chain rate the integrator gain is near 1e-4, where float state would
  // lose the low band to rounding.
  struct Section {
    double ic1{0.0}, ic2{0.0};
    void reset() noexcept { ic1 = ic2 = 0.0; }
  };
  // Linkwitz-Riley 4th order for one channel. The first section's low and
  // high outputs feed one more section each, so each band is two cascaded
  // Butterworth sections and the two first sections (which would see the
  // same input with the same coefficients) are one.
  struct Crossover {
    Section first, low, high;
    void reset() noexcept {
      first.reset();
      low.reset();
      high.reset();
    }
  };
  struct CrossoverCoeffs {
    double a1{0.0}, a2{0.0}, a3{0.0};
  };

  void updateCrossoverCoeffs(double hz) noexcept;
  void resetSplitterState() noexcept;
  static void storePeakDb(std::atomic<float>& meter, const juce::AudioBuffer<float>& buffer,
                          int channels, int numFrames) noexcept;

  std::array<std::atomic<float>, kNumParams> values;

  double sampleRate{48000.0};
  int preparedBlockSize{0};
  int rampSamples{960};

  // Splitter.
  Ramp splitGainA, splitGainB;
  // 0 = straight, 1 = stereo split; 0 = low to A, 1 = high to A. Ramps so
  // that toggling either under audio is a short crossfade, and exact at
  // both ends.
  Ramp stereoAmount, swapAmount;
  std::array<Crossover, 2> crossover;
  CrossoverCoeffs coeffs;
  // The crossover frequency moves in the log domain (equal time per
  // octave), one coefficient update per sample while it moves.
  double logHz{0.0}, logHzTarget{0.0}, logHzStep{0.0};
  int logHzSamplesLeft{0};
  bool snapCrossover{true};
  // The type the last block ran with (-1 = none yet), to notice a change.
  int lastSplitType{-1};

  // Mixer.
  BlockDelayLine alignA, alignB;
  Ramp mixGainAL, mixGainAR, mixGainBL, mixGainBR;
  // The pan law depends on the channel layout, so a change of layout takes
  // the new gains at once (as utility.gain does on a channel count change).
  int lastMixLayout{-1};

  std::array<std::atomic<float>, 2> splitMeter;
  std::array<std::atomic<float>, 2> mixMeter;
};

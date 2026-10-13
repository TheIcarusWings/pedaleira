// filter.glitch (Stutter): a tempo-synced slice repeat. The input is always
// being written to a ring of the last two seconds. Time is cut into cells of
// `every` (a note value at the tempo); at the start of a cell the dice are
// rolled (`probability`, from a hash of the cell number, so a render repeats
// exactly) and, if they say yes and no stutter is running, the audio that
// came just before the cell, one slice long, is copied out and played again
// `repeats` times in a row, back to back, starting at that very sample. The
// event lasts repeats x slice; a cell that begins while one is running is
// passed over.
//
//   slice      the slice length: free in ms or a note value at the tempo
//   every      the cell length (a note value); 1/1 is once a bar
//   repeats    how many times the slice plays
//   reverse    each repeat plays the slice backwards
//   pitchDown  every repeat is this many semitones lower than the one before
//              (repeat k plays at a rate of 2^(-k * st / 12) and so covers
//              only that fraction of the slice: it falls like a tape slowing)
//
// The joins are short raised-cosine fades (3 ms, at most a quarter slice):
// the stutter fades in against the live signal over its first 3 ms and out
// over its last 3 ms, and inside the event each repeat fades out and the
// next one fades in, so none of them clicks. Everything between the fades is
// the slice exactly, sample for sample. Whenever no stutter is running the
// wet path is the input itself, and the block adds no latency.
//
// Timing: free running, the cells are counted in samples from the start (the
// first cell boundary is one cell in, so an event never starts on the first
// sample); while the host plays, the cell is taken from the song position
// each block, so a stutter lands on the host's beat. The same events happen
// on every channel.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>
#include "effects/EffectDsp.h"

namespace {

class StutterProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "filter.glitch";

  enum Param {
    kSliceSync = 0,
    kSliceDivision,
    kSliceTimeMs,
    kEvery,
    kRepeats,
    kProbability,
    kReverse,
    kPitchDown
  };

  static constexpr int kMaxChannels = 2;
  static constexpr double kMaxSliceSeconds = 2.0;
  static constexpr double kMinSliceSeconds = 0.01;
  static constexpr double kFadeSeconds = 0.003;

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      fx::TimeParamOptions sliceTime;
      sliceTime.minMs = 10.0f;
      sliceTime.maxMs = 2000.0f;
      sliceTime.defaultMs = 250.0f;
      sliceTime.defaultSync = true;
      sliceTime.defaultDivision = fx::kDivEighth;
      sliceTime.affectsTail = true;
      sliceTime.name = "Slice";
      sliceTime.idPrefix = "slice";
      fx::appendTimeParams(t, sliceTime);
      t.push_back(BlockParamSpec::choiceParam("every", "Every", fx::noteDivisionNames(),
                                              fx::kDivWhole));
      t.push_back(BlockParamSpec::floatParam("repeats", "Repeats", "x", 1.0f, 16.0f, 4.0f)
                      .withStep(1.0f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("probability", "Probability", "%", 0.0f, 100.0f,
                                             100.0f));
      t.push_back(BlockParamSpec::boolParam("reverse", "Reverse", false));
      t.push_back(BlockParamSpec::floatParam("pitchDown", "Pitch Down", "st", 0.0f, 12.0f, 0.0f));
      return t;
    }();
    return table;
  }

  StutterProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // A cell can start as the input ends and still hold the end of it: the
  // slice, then the repeats. A running event has its own length.
  double tailSeconds() const noexcept override {
    const double current = (std::round(static_cast<double>(param(kRepeats))) + 1.0) *
                           sliceSeconds(lastBpm.load(std::memory_order_relaxed));
    return std::max(current, static_cast<double>(eventSeconds.load(std::memory_order_relaxed))) +
           0.02;
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    maxSlice = static_cast<size_t>(std::ceil(kMaxSliceSeconds * sampleRate)) + 8;
    ringSize = static_cast<size_t>(juce::nextPowerOfTwo(static_cast<int>(maxSlice) + 8));
    ringMask = ringSize - 1;
    ring.assign(static_cast<size_t>(kMaxChannels) * ringSize, 0.0f);
    slice.assign(static_cast<size_t>(kMaxChannels) * maxSlice, 0.0f);
    resetEffect();
  }

  void resetEffect() noexcept override {
    std::fill(ring.begin(), ring.end(), 0.0f);
    std::fill(slice.begin(), slice.end(), 0.0f);
    writePos = 0;
    active = false;
    eventPos = 0;
    cellSamples = 0;
    cellIndex = 0;
    started = false;
    eventSeconds.store(0.0f, std::memory_order_relaxed);
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxChannels);
    lastBpm.store(ctx.transport.bpm, std::memory_order_relaxed);

    const double everyBeats = fx::noteDivisionBeats(static_cast<int>(std::lround(param(kEvery))));
    const std::int64_t everySamples = std::max<std::int64_t>(
        1, std::llround(everyBeats * 60.0 / std::max(1.0, ctx.transport.bpm) * sampleRate));
    const double probability = static_cast<double>(param(kProbability)) * 0.01;

    // Locked to the song while the host plays: the cell is the one the song
    // position is in, and the first sample is the boundary when it is a new
    // one.
    bool boundaryAtStart = false;
    if (ctx.transport.playing) {
      const double cells = ctx.transport.ppqPosition / everyBeats;
      const double whole = std::floor(cells);
      const auto index = static_cast<std::int64_t>(whole);
      cellSamples = static_cast<std::int64_t>(std::llround((cells - whole) * static_cast<double>(everySamples)));
      boundaryAtStart = started && index != cellIndex;
      cellIndex = index;
      started = true;
    } else if (!started) {
      started = true;
      cellSamples = 0;
      cellIndex = 0;
    }

    float* data[kMaxChannels] = {buffer.getWritePointer(0),
                                 numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    for (int i = 0; i < numSamples; ++i) {
      // An event that ends on a cell boundary has ended before the dice are
      // rolled for that cell.
      if (active && eventPos >= repeatsTotal * sliceLength)
        active = false;
      bool boundary = boundaryAtStart && i == 0;
      if (cellSamples >= everySamples) {
        cellSamples = 0;
        ++cellIndex;
        boundary = true;
      }
      if (boundary && !active && roll(cellIndex) < probability)
        startEvent(ctx.transport.bpm);

      float live[kMaxChannels] = {0.0f, 0.0f};
      for (int ch = 0; ch < numChannels; ++ch)
        live[ch] = data[ch][i];

      if (active) {
        const std::int64_t k = eventPos / sliceLength;
        {  // repeat k of the event, j frames into it
          const std::int64_t j = eventPos - k * sliceLength;
          const std::int64_t total = repeatsTotal * sliceLength;
          const float edge = std::min(fadeShape(eventPos, fadeLength),
                                      fadeShape(total - 1 - eventPos, fadeLength));
          float envelope = 1.0f;
          if (k > 0)
            envelope = std::min(envelope, fadeShape(j, fadeLength));
          if (k < repeatsTotal - 1)
            envelope = std::min(envelope, fadeShape(sliceLength - 1 - j, fadeLength));
          const double rate = std::exp2(-static_cast<double>(k) * pitchStepSemitones / 12.0);
          double position = static_cast<double>(j) * rate;
          if (reversed)
            position = static_cast<double>(sliceLength - 1) - position;
          position = juce::jlimit(0.0, static_cast<double>(sliceLength - 1), position);
          const auto whole = static_cast<size_t>(position);
          const float fraction = static_cast<float>(position - static_cast<double>(whole));
          const size_t next = std::min(whole + 1, static_cast<size_t>(sliceLength - 1));
          for (int ch = 0; ch < numChannels; ++ch) {
            const float* source = slice.data() + static_cast<size_t>(ch) * maxSlice;
            const float played = source[whole] + fraction * (source[next] - source[whole]);
            data[ch][i] = live[ch] * (1.0f - edge) + played * envelope * edge;
          }
          ++eventPos;
        }
      }

      for (int ch = 0; ch < numChannels; ++ch)
        ring[static_cast<size_t>(ch) * ringSize + writePos] = live[ch];
      writePos = (writePos + 1) & ringMask;
      ++cellSamples;
    }
  }

private:
  // 0 at the edge, 1 from `length` samples in, a raised cosine between;
  // `distance` counts samples from the edge, 0 on the edge sample itself.
  static float fadeShape(std::int64_t distance, std::int64_t length) noexcept {
    if (distance >= length)
      return 1.0f;
    if (distance < 0)
      return 0.0f;
    const float x = (static_cast<float>(distance) + 0.5f) / static_cast<float>(length);
    return 0.5f - 0.5f * std::cos(3.14159265359f * x);
  }

  double sliceSeconds(double bpm) const noexcept {
    BlockTransport transport;
    transport.bpm = bpm;
    return juce::jlimit(kMinSliceSeconds, kMaxSliceSeconds,
                        fx::resolveTimeSeconds(param(kSliceSync), param(kSliceDivision),
                                               param(kSliceTimeMs), transport));
  }

  // 0 to 1 from the cell number, the same for every render.
  static double roll(std::int64_t cell) noexcept {
    const auto bits = static_cast<std::uint32_t>(cell) * 0x9e3779b1u ^ 0x85ebca6bu;
    return static_cast<double>(fx::hash32(bits) >> 8) / 16777216.0;
  }

  void startEvent(double bpm) noexcept {
    sliceLength = std::max<std::int64_t>(
        1, std::llround(sliceSeconds(bpm) * sampleRate));
    sliceLength = std::min<std::int64_t>(sliceLength, static_cast<std::int64_t>(maxSlice));
    repeatsTotal = std::max<std::int64_t>(1, std::llround(static_cast<double>(param(kRepeats))));
    reversed = param(kReverse) >= 0.5f;
    pitchStepSemitones = static_cast<double>(param(kPitchDown));
    fadeLength = std::max<std::int64_t>(
        1, std::min<std::int64_t>(std::llround(kFadeSeconds * sampleRate), sliceLength / 4));
    // The slice is what came just before this sample.
    for (int ch = 0; ch < kMaxChannels; ++ch)
      for (std::int64_t n = 0; n < sliceLength; ++n)
        slice[static_cast<size_t>(ch) * maxSlice + static_cast<size_t>(n)] =
            ring[static_cast<size_t>(ch) * ringSize +
                 ((writePos + ringSize - static_cast<size_t>(sliceLength) + static_cast<size_t>(n)) &
                  ringMask)];
    eventPos = 0;
    active = true;
    eventSeconds.store(static_cast<float>(static_cast<double>((repeatsTotal + 1) * sliceLength) /
                                          sampleRate),
                       std::memory_order_relaxed);
  }

  std::vector<float> ring;
  std::vector<float> slice;
  size_t ringSize{0};
  size_t ringMask{0};
  size_t maxSlice{0};
  size_t writePos{0};

  bool active{false};
  std::int64_t eventPos{0};
  std::int64_t sliceLength{1};
  std::int64_t repeatsTotal{1};
  std::int64_t fadeLength{1};
  bool reversed{false};
  double pitchStepSemitones{0.0};

  std::int64_t cellSamples{0};
  std::int64_t cellIndex{0};
  bool started{false};

  double sampleRate{48000.0};
  std::atomic<double> lastBpm{120.0};
  std::atomic<float> eventSeconds{0.0f};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(Stutter) {
  // Replaces the signal while it runs and is the signal when it does not.
  addEffectKind<StutterProcessor>(registry, "Stutter", BlockCategory::Filter, 1.0f,
                                  BlockMixLaw::Crossfade);
}

// SplitJunction tests: the splitter laws, the mixer and the junction's
// parameters and state. The
// crossover type has its own file, crossover_tests.cpp.
//
//   Balance and A/B give exact gains, the floor is silence, and a moved
//     knob ramps without a step;
//   stereo split sends left to A and right to B, on both channels of each;
//   split then mix at the defaults is exactly twice the input, and exact
//     zeros with B inverted, in every channel layout;
//   the pan laws at centre and at the ends, level and mute;
//   the alignment delays null a path that arrives late;
//   state survives a round trip and tolerates unknown and missing values.
#include "BlockDelayLine.h"
#include "SplitJunction.h"
#include "test_helpers.h"

#include <gtest/gtest.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_data_structures/juce_data_structures.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

namespace {

constexpr int kBlock = 128;
using P = SplitJunction::Param;

struct Stereo {
  std::vector<float> l, r;
};

// What a run produced: A and B as the splitter left them, and the mixer's
// result (empty when the run did not mix).
struct RunResult {
  Stereo a, b, mixed;
};

struct RunOptions {
  int upperChannels{2};
  int lowerChannels{2};
  bool upperDualMono{false};
  bool lowerDualMono{false};
  bool mix{false};
  // Called before each block, to move parameters under audio.
  std::function<void(SplitJunction&, int)> beforeBlock;
  // Called between the split and the mix, to stand in for what the rows
  // would do to the two paths.
  std::function<void(juce::AudioBuffer<float>&, juce::AudioBuffer<float>&, int)> between;
};

void append(Stereo& to, const juce::AudioBuffer<float>& from, int channels, int n) {
  to.l.insert(to.l.end(), from.getReadPointer(0), from.getReadPointer(0) + n);
  const float* right = from.getReadPointer(channels > 1 ? 1 : 0);
  to.r.insert(to.r.end(), right, right + n);
}

// Streams `in` (its right channel ignored by a mono upper path) through
// the junction in blocks of kBlock.
RunResult run(SplitJunction& junction, const Stereo& in, const RunOptions& options = {}) {
  const juce::ScopedNoDenormals noDenormals;
  juce::AudioBuffer<float> upper(options.upperChannels, kBlock);
  juce::AudioBuffer<float> lower(options.lowerChannels, kBlock);
  SplitFormat format = SplitFormat::of(upper, lower);
  format.upperDualMono = options.upperDualMono;
  format.lowerDualMono = options.lowerDualMono;

  RunResult out;
  int block = 0;
  for (size_t pos = 0; pos < in.l.size(); pos += kBlock, ++block) {
    const int n = static_cast<int>(std::min<size_t>(kBlock, in.l.size() - pos));
    if (options.beforeBlock)
      options.beforeBlock(junction, block);
    upper.copyFrom(0, 0, in.l.data() + pos, n);
    if (options.upperChannels > 1)
      upper.copyFrom(1, 0, in.r.data() + pos, n);
    // Stale contents the splitter must overwrite, not add to.
    for (int ch = 0; ch < lower.getNumChannels(); ++ch)
      juce::FloatVectorOperations::fill(lower.getWritePointer(ch), 123.0f, n);

    junction.split(upper, lower, n, format);
    append(out.a, upper, options.upperChannels, n);
    append(out.b, lower, options.lowerChannels, n);
    if (options.between)
      options.between(upper, lower, n);
    if (options.mix) {
      junction.mix(upper, lower, n, format);
      append(out.mixed, upper, options.upperChannels, n);
    }
  }
  return out;
}

Stereo noiseStereo(int frames, unsigned seed = 5u, float amplitude = 0.5f) {
  return {makeNoise(frames, seed, amplitude), makeNoise(frames, seed + 100u, amplitude)};
}

// The same signal on both channels.
Stereo dualMono(const std::vector<float>& x) { return {x, x}; }

Stereo constantStereo(int frames, float left, float right) {
  return {std::vector<float>(static_cast<size_t>(frames), left),
          std::vector<float>(static_cast<size_t>(frames), right)};
}

float gainOf(float db) { return juce::Decibels::decibelsToGain(db, SplitJunction::kLevelFloorDb); }

void expectEqualScaled(const std::vector<float>& actual, const std::vector<float>& source,
                       float gain, const char* what) {
  ASSERT_EQ(actual.size(), source.size()) << what;
  for (size_t i = 0; i < actual.size(); ++i)
    ASSERT_EQ(actual[i], source[i] * gain) << what << " sample " << i;
}

void expectSilent(const std::vector<float>& actual, const char* what) {
  for (size_t i = 0; i < actual.size(); ++i)
    ASSERT_EQ(actual[i], 0.0f) << what << " sample " << i;
}

double largestStep(const std::vector<float>& x) {
  double step = 0.0;
  for (size_t i = 1; i < x.size(); ++i)
    step = std::max(step, std::abs(double(x[i]) - x[i - 1]));
  return step;
}

// The most a gain may move per sample: the whole range over the ramp.
double rampStep(double fullRange = 1.0) {
  return fullRange / (kFs * SplitJunction::kRampSeconds);
}

}  // namespace

// ---------------------------------------------------------------------------
// Splitter laws
// ---------------------------------------------------------------------------

TEST(SplitJunctionTest, BalanceLawGivesExactGains) {
  struct Setting {
    float balance, gainA, gainB;
  };
  // Centre is both at unity; toward A (negative) fades B, toward B fades A.
  const Setting settings[] = {
      {0.0f, 1.0f, 1.0f},  {-1.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 1.0f},
      {-0.5f, 1.0f, 0.5f}, {0.25f, 0.75f, 1.0f}, {0.75f, 0.25f, 1.0f},
  };
  const Stereo in = noiseStereo(kBlock * 8);
  for (const Setting& s : settings) {
    SCOPED_TRACE(testing::Message() << "balance " << s.balance);
    SplitJunction junction;
    junction.prepare(kFs, kBlock);
    junction.setParameter(P::kSplitBalance, s.balance);
    const RunResult out = run(junction, in);
    expectEqualScaled(out.a.l, in.l, s.gainA, "A left");
    expectEqualScaled(out.a.r, in.r, s.gainA, "A right");
    expectEqualScaled(out.b.l, in.l, s.gainB, "B left");
    expectEqualScaled(out.b.r, in.r, s.gainB, "B right");
  }
}

TEST(SplitJunctionTest, BalanceIgnoresTheAbLevels) {
  SplitJunction junction;
  junction.prepare(kFs, kBlock);
  junction.setParameter(P::kSplitLevelA, -20.0f);
  junction.setParameter(P::kSplitLevelB, SplitJunction::kLevelFloorDb);
  const Stereo in = noiseStereo(kBlock * 4);
  const RunResult out = run(junction, in);
  expectEqualScaled(out.a.l, in.l, 1.0f, "A");
  expectEqualScaled(out.b.l, in.l, 1.0f, "B");
}

TEST(SplitJunctionTest, AbLawGivesExactGainsAndTheFloorIsSilence) {
  struct Setting {
    float dbA, dbB;
  };
  const Setting settings[] = {
      {0.0f, 0.0f},  {-6.0f, 6.0f}, {12.0f, -24.0f}, {-59.0f, -3.0f},
      {SplitJunction::kLevelFloorDb, 0.0f}, {0.0f, SplitJunction::kLevelFloorDb},
  };
  const Stereo in = noiseStereo(kBlock * 8);
  for (const Setting& s : settings) {
    SCOPED_TRACE(testing::Message() << "A " << s.dbA << " dB, B " << s.dbB << " dB");
    SplitJunction junction;
    junction.type = SplitterType::AB;
    junction.prepare(kFs, kBlock);
    junction.setParameter(P::kSplitLevelA, s.dbA);
    junction.setParameter(P::kSplitLevelB, s.dbB);
    // The balance knob belongs to the other type and must do nothing here.
    junction.setParameter(P::kSplitBalance, 1.0f);
    const RunResult out = run(junction, in);
    expectEqualScaled(out.a.l, in.l, gainOf(s.dbA), "A left");
    expectEqualScaled(out.a.r, in.r, gainOf(s.dbA), "A right");
    expectEqualScaled(out.b.l, in.l, gainOf(s.dbB), "B left");
    expectEqualScaled(out.b.r, in.r, gainOf(s.dbB), "B right");
  }
  EXPECT_EQ(gainOf(SplitJunction::kLevelFloorDb), 0.0f);
  EXPECT_EQ(gainOf(0.0f), 1.0f);
  EXPECT_NEAR(gainOf(-6.0f), 0.501187f, 1e-6f);
}

TEST(SplitJunctionTest, SplitterGainsRampWithoutAStep) {
  // A constant input shows the gain itself.
  const Stereo in = constantStereo(kBlock * 40, 1.0f, 1.0f);
  {
    SplitJunction junction;
    junction.prepare(kFs, kBlock);
    RunOptions options;
    options.beforeBlock = [](SplitJunction& j, int block) {
      if (block == 4)
        j.setParameter(P::kSplitBalance, 1.0f);  // A fades out
      if (block == 20)
        j.setParameter(P::kSplitBalance, -1.0f);  // A back, B out
    };
    const RunResult out = run(junction, in, options);
    EXPECT_LE(largestStep(out.a.l), 1.01 * rampStep());
    EXPECT_LE(largestStep(out.b.l), 1.01 * rampStep());
    EXPECT_EQ(out.a.l[kBlock * 4 - 1], 1.0f);
    EXPECT_EQ(out.a.l[kBlock * 19], 0.0f);  // 15 blocks is twice the ramp
    EXPECT_EQ(out.a.l.back(), 1.0f);
    EXPECT_EQ(out.b.l.back(), 0.0f);
    // The ramp takes the 20 ms it says.
    EXPECT_GT(out.a.l[kBlock * 4 + 900], 0.0f);
    EXPECT_EQ(out.a.l[kBlock * 4 + 960], 0.0f);
  }
  {
    SplitJunction junction;
    junction.type = SplitterType::AB;
    junction.prepare(kFs, kBlock);
    RunOptions options;
    options.beforeBlock = [](SplitJunction& j, int block) {
      if (block == 4)
        j.setParameter(P::kSplitLevelA, SplitJunction::kLevelFloorDb);
      if (block == 20)
        j.setParameter(P::kSplitLevelB, 12.0f);
    };
    const RunResult out = run(junction, in, options);
    EXPECT_LE(largestStep(out.a.l), 1.01 * rampStep());
    EXPECT_LE(largestStep(out.b.l), 1.01 * rampStep(gainOf(12.0f) - 1.0f));
    EXPECT_EQ(out.a.l.back(), 0.0f);
    EXPECT_EQ(out.b.l.back(), gainOf(12.0f));
  }
}

TEST(SplitJunctionTest, FirstBlockStartsAtTheParameters) {
  // No glide in from the defaults after a prepare or a reset.
  SplitJunction junction;
  junction.prepare(kFs, kBlock);
  junction.setParameter(P::kSplitBalance, 1.0f);
  junction.setParameter(P::kMixLevel, -12.0f);
  const Stereo in = constantStereo(kBlock * 2, 1.0f, 1.0f);
  RunOptions options;
  options.mix = true;
  RunResult out = run(junction, in, options);
  EXPECT_EQ(out.a.l.front(), 0.0f);
  EXPECT_EQ(out.mixed.l.front(), gainOf(-12.0f));

  junction.setParameter(P::kSplitBalance, -1.0f);
  junction.reset();
  out = run(junction, in, options);
  EXPECT_EQ(out.a.l.front(), 1.0f);
  EXPECT_EQ(out.b.l.front(), 0.0f);
}

TEST(SplitJunctionTest, ChangingTheTypeTakesTheNewLawAtOnce) {
  SplitJunction junction;
  junction.prepare(kFs, kBlock);
  junction.setParameter(P::kSplitBalance, 1.0f);
  junction.setParameter(P::kSplitLevelA, -6.0f);
  const Stereo in = constantStereo(kBlock * 4, 1.0f, 1.0f);
  RunOptions options;
  options.beforeBlock = [](SplitJunction& j, int block) {
    if (block == 2)
      j.type = SplitterType::AB;
  };
  const RunResult out = run(junction, in, options);
  EXPECT_EQ(out.a.l[kBlock * 2 - 1], 0.0f);
  EXPECT_EQ(out.a.l[kBlock * 2], gainOf(-6.0f));
}

// ---------------------------------------------------------------------------
// Stereo split
// ---------------------------------------------------------------------------

TEST(SplitJunctionTest, StereoSplitSendsLeftToAAndRightToB) {
  const Stereo in = noiseStereo(kBlock * 4);
  for (const SplitterType type : {SplitterType::Balance, SplitterType::AB}) {
    SplitJunction junction;
    junction.type = type;
    junction.prepare(kFs, kBlock);
    junction.setParameter(P::kSplitStereo, 1.0f);
    const RunResult out = run(junction, in);
    EXPECT_EQ(out.a.l, in.l);
    EXPECT_EQ(out.a.r, in.l);
    EXPECT_EQ(out.b.l, in.r);
    EXPECT_EQ(out.b.r, in.r);
  }
}

TEST(SplitJunctionTest, StereoSplitOfALeftOnlyInputLeavesBSilent) {
  Stereo in = noiseStereo(kBlock * 4);
  std::fill(in.r.begin(), in.r.end(), 0.0f);
  for (const SplitterType type :
       {SplitterType::Balance, SplitterType::AB, SplitterType::Crossover}) {
    SCOPED_TRACE(splitterTypeName(type).toStdString());
    SplitJunction junction;
    junction.type = type;
    junction.prepare(kFs, kBlock);
    junction.setParameter(P::kSplitStereo, 1.0f);
    const RunResult out = run(junction, in);
    expectSilent(out.b.l, "B left");
    expectSilent(out.b.r, "B right");
    EXPECT_EQ(out.a.l, out.a.r);  // A carries left on both channels
    if (type != SplitterType::Crossover) {
      EXPECT_EQ(out.a.l, in.l);
    } else {
      // The low band of the left channel: not silent.
      EXPECT_GT(largestStep(out.a.l), 0.0);
    }
  }
}

TEST(SplitJunctionTest, StereoSplitThenTheTypesLaw) {
  const Stereo in = noiseStereo(kBlock * 4);
  SplitJunction junction;
  junction.type = SplitterType::AB;
  junction.prepare(kFs, kBlock);
  junction.setParameter(P::kSplitStereo, 1.0f);
  junction.setParameter(P::kSplitLevelA, -6.0f);
  junction.setParameter(P::kSplitLevelB, 3.0f);
  const RunResult out = run(junction, in);
  expectEqualScaled(out.a.l, in.l, gainOf(-6.0f), "A left");
  expectEqualScaled(out.a.r, in.l, gainOf(-6.0f), "A right");
  expectEqualScaled(out.b.l, in.r, gainOf(3.0f), "B left");
  expectEqualScaled(out.b.r, in.r, gainOf(3.0f), "B right");
}

TEST(SplitJunctionTest, StereoSplitIntoAMonoLowerPathIsTheRightChannel) {
  const Stereo in = noiseStereo(kBlock * 4);
  SplitJunction junction;
  junction.prepare(kFs, kBlock);
  junction.setParameter(P::kSplitStereo, 1.0f);
  RunOptions options;
  options.lowerChannels = 1;
  const RunResult out = run(junction, in, options);
  EXPECT_EQ(out.a.l, in.l);
  EXPECT_EQ(out.a.r, in.l);
  EXPECT_EQ(out.b.l, in.r);
}

TEST(SplitJunctionTest, StereoSplitDoesNothingOnMonoAndDualMonoPaths) {
  const Stereo in = noiseStereo(kBlock * 4);
  {
    SplitJunction junction;
    junction.prepare(kFs, kBlock);
    junction.setParameter(P::kSplitStereo, 1.0f);
    RunOptions options;
    options.upperChannels = 1;
    const RunResult out = run(junction, in, options);
    EXPECT_EQ(out.a.l, in.l);
    EXPECT_EQ(out.b.l, in.l);
    EXPECT_EQ(out.b.r, in.l);
  }
  {
    // Two players on two channels: each path keeps both, uncrossed.
    SplitJunction junction;
    junction.prepare(kFs, kBlock);
    junction.setParameter(P::kSplitStereo, 1.0f);
    RunOptions options;
    options.upperDualMono = options.lowerDualMono = true;
    const RunResult out = run(junction, in, options);
    EXPECT_EQ(out.a.l, in.l);
    EXPECT_EQ(out.a.r, in.r);
    EXPECT_EQ(out.b.l, in.l);
    EXPECT_EQ(out.b.r, in.r);
  }
}

TEST(SplitJunctionTest, TogglingStereoSplitUnderAudioCrossfades) {
  // Left and right at different constants: an instant switch would step by
  // their difference.
  const Stereo in = constantStereo(kBlock * 30, 1.0f, -1.0f);
  SplitJunction junction;
  junction.prepare(kFs, kBlock);
  RunOptions options;
  options.beforeBlock = [](SplitJunction& j, int block) {
    if (block == 4)
      j.setParameter(P::kSplitStereo, 1.0f);
  };
  const RunResult out = run(junction, in, options);
  EXPECT_LE(largestStep(out.a.r), 1.01 * rampStep(2.0));
  EXPECT_LE(largestStep(out.b.l), 1.01 * rampStep(2.0));
  EXPECT_EQ(out.a.r.front(), -1.0f);
  EXPECT_EQ(out.a.r.back(), 1.0f);
  EXPECT_EQ(out.b.l.front(), 1.0f);
  EXPECT_EQ(out.b.l.back(), -1.0f);
}

// ---------------------------------------------------------------------------
// Channel layouts of the splitter
// ---------------------------------------------------------------------------

TEST(SplitJunctionTest, SplitChannelRules) {
  const Stereo in = noiseStereo(kBlock * 4);
  SplitJunction junction;
  junction.prepare(kFs, kBlock);
  {
    RunOptions options;  // mono to mono
    options.upperChannels = options.lowerChannels = 1;
    const RunResult out = run(junction, in, options);
    EXPECT_EQ(out.a.l, in.l);
    EXPECT_EQ(out.b.l, in.l);
  }
  {
    RunOptions options;  // mono to stereo: the signal on both channels
    options.upperChannels = 1;
    const RunResult out = run(junction, in, options);
    EXPECT_EQ(out.a.l, in.l);
    EXPECT_EQ(out.b.l, in.l);
    EXPECT_EQ(out.b.r, in.l);
  }
  {
    RunOptions options;  // stereo to mono: the fold 0.5 (L + R)
    options.lowerChannels = 1;
    const RunResult out = run(junction, in, options);
    EXPECT_EQ(out.a.l, in.l);
    EXPECT_EQ(out.a.r, in.r);
    for (size_t i = 0; i < in.l.size(); ++i)
      ASSERT_EQ(out.b.l[i], 0.5f * (in.l[i] + in.r[i]));
    // A signal that is the same on both sides keeps its level exactly.
    const RunResult centred = run(junction, dualMono(in.l), options);
    EXPECT_EQ(centred.b.l, in.l);
  }
}

TEST(SplitJunctionTest, FormatCanNarrowAWiderBuffer) {
  // The plan may hand in two-channel work buffers for a mono row: the
  // format says one channel is live, and the second is left alone.
  const juce::ScopedNoDenormals noDenormals;
  SplitJunction junction;
  junction.prepare(kFs, kBlock);
  juce::AudioBuffer<float> upper(2, kBlock), lower(2, kBlock);
  const std::vector<float> x = makeNoise(kBlock, 9u, 0.5f);
  upper.copyFrom(0, 0, x.data(), kBlock);
  juce::FloatVectorOperations::fill(upper.getWritePointer(1), 7.0f, kBlock);
  juce::FloatVectorOperations::fill(lower.getWritePointer(0), 7.0f, kBlock);
  juce::FloatVectorOperations::fill(lower.getWritePointer(1), 7.0f, kBlock);
  const SplitFormat format{1, 1, false, false};
  junction.split(upper, lower, kBlock, format);
  junction.mix(upper, lower, kBlock, format);
  for (int i = 0; i < kBlock; ++i) {
    ASSERT_EQ(upper.getSample(0, i), 2.0f * x[static_cast<size_t>(i)]);
    ASSERT_EQ(upper.getSample(1, i), 7.0f);
    ASSERT_EQ(lower.getSample(1, i), 7.0f);
  }
}

// ---------------------------------------------------------------------------
// Mixer
// ---------------------------------------------------------------------------

namespace {

struct Layout {
  const char* name;
  int upper, lower;
};
const Layout kLayouts[] = {
    {"mono/mono", 1, 1}, {"mono/stereo", 1, 2}, {"stereo/mono", 2, 1}, {"stereo/stereo", 2, 2}};

}  // namespace

TEST(SplitJunctionTest, SplitThenMixAtDefaultsIsExactlyTwiceTheInput) {
  const std::vector<float> x = makeNoise(kBlock * 8, 31u, 0.5f);
  for (const Layout& layout : kLayouts) {
    SCOPED_TRACE(layout.name);
    SplitJunction junction;
    junction.prepare(kFs, kBlock);
    RunOptions options;
    options.upperChannels = layout.upper;
    options.lowerChannels = layout.lower;
    options.mix = true;
    // The same signal on both sides, so the folds have nothing to average.
    const RunResult out = run(junction, dualMono(x), options);
    expectEqualScaled(out.mixed.l, x, 2.0f, "left");
    expectEqualScaled(out.mixed.r, x, 2.0f, "right");
  }
  // True stereo through a stereo pair of paths: each side doubled.
  SplitJunction junction;
  junction.prepare(kFs, kBlock);
  RunOptions options;
  options.mix = true;
  const Stereo in = noiseStereo(kBlock * 8);
  const RunResult out = run(junction, in, options);
  expectEqualScaled(out.mixed.l, in.l, 2.0f, "left");
  expectEqualScaled(out.mixed.r, in.r, 2.0f, "right");
}

TEST(SplitJunctionTest, InvertBGivesExactZeros) {
  const std::vector<float> x = makeNoise(kBlock * 8, 32u, 0.5f);
  for (const Layout& layout : kLayouts) {
    SCOPED_TRACE(layout.name);
    SplitJunction junction;
    junction.prepare(kFs, kBlock);
    junction.setParameter(P::kMixInvertB, 1.0f);
    RunOptions options;
    options.upperChannels = layout.upper;
    options.lowerChannels = layout.lower;
    options.mix = true;
    const RunResult out = run(junction, dualMono(x), options);
    expectSilent(out.mixed.l, "left");
    expectSilent(out.mixed.r, "right");
  }
}

TEST(SplitJunctionTest, ReturningToTheDefaultsRestoresTheExactSum) {
  // The mixer leaves its plain-sum shortcut while a gain moves and comes
  // back to it: once the ramp back to unity is over, the samples are those
  // of a mixer that never moved.
  const Stereo in = noiseStereo(kBlock * 40);
  for (const Layout& layout : kLayouts) {
    SCOPED_TRACE(layout.name);
    RunOptions options;
    options.upperChannels = layout.upper;
    options.lowerChannels = layout.lower;
    options.mix = true;
    SplitJunction plain;
    plain.prepare(kFs, kBlock);
    const RunResult reference = run(plain, in, options);

    SplitJunction moved;
    moved.prepare(kFs, kBlock);
    options.beforeBlock = [](SplitJunction& j, int block) {
      j.setParameter(P::kMixLevel, block < 2 ? -1.0f : 0.0f);
    };
    const RunResult out = run(moved, in, options);
    // Past the ramp back to unity (block 2 plus 20 ms).
    for (size_t i = kBlock * 12; i < in.l.size(); ++i) {
      ASSERT_EQ(out.mixed.l[i], reference.mixed.l[i]) << i;
      ASSERT_EQ(out.mixed.r[i], reference.mixed.r[i]) << i;
    }
  }
}

TEST(SplitJunctionTest, MixLevelsAreExact) {
  const Stereo in = noiseStereo(kBlock * 4);
  SplitJunction junction;
  junction.type = SplitterType::AB;
  junction.prepare(kFs, kBlock);
  // B off at the splitter, so the mixer's A gain can be read alone, then
  // the reverse.
  junction.setParameter(P::kSplitLevelB, SplitJunction::kLevelFloorDb);
  junction.setParameter(P::kMixLevelA, -6.0f);
  junction.setParameter(P::kMixLevel, 3.0f);
  RunOptions options;
  options.mix = true;
  RunResult out = run(junction, in, options);
  expectEqualScaled(out.mixed.l, in.l, gainOf(-6.0f) * gainOf(3.0f), "A left");
  expectEqualScaled(out.mixed.r, in.r, gainOf(-6.0f) * gainOf(3.0f), "A right");

  junction.reset();
  junction.setParameter(P::kSplitLevelB, 0.0f);
  junction.setParameter(P::kSplitLevelA, SplitJunction::kLevelFloorDb);
  junction.setParameter(P::kMixLevelB, 6.0f);
  junction.setParameter(P::kMixInvertB, 1.0f);
  out = run(junction, in, options);
  expectEqualScaled(out.mixed.l, in.l, -(gainOf(6.0f) * gainOf(3.0f)), "B left");
  expectEqualScaled(out.mixed.r, in.r, -(gainOf(6.0f) * gainOf(3.0f)), "B right");

  // The floors are off, each of them.
  for (const P floorParam : {P::kMixLevelB, P::kMixLevel}) {
    junction.reset();
    junction.setParameter(P::kMixLevelB, 0.0f);
    junction.setParameter(P::kMixLevel, 0.0f);
    junction.setParameter(floorParam, SplitJunction::kLevelFloorDb);
    out = run(junction, in, options);
    expectSilent(out.mixed.l, "floor");
    expectSilent(out.mixed.r, "floor");
  }
}

TEST(SplitJunctionTest, MuteIsSilenceAndRampsBothWays) {
  const Stereo in = constantStereo(kBlock * 40, 0.5f, 0.5f);
  SplitJunction junction;
  junction.prepare(kFs, kBlock);
  RunOptions options;
  options.mix = true;
  options.beforeBlock = [](SplitJunction& j, int block) {
    j.setParameter(P::kMixMute, block >= 4 && block < 20 ? 1.0f : 0.0f);
  };
  const RunResult out = run(junction, in, options);
  EXPECT_EQ(out.mixed.l[kBlock * 4 - 1], 1.0f);
  EXPECT_EQ(out.mixed.l[kBlock * 19], 0.0f);
  EXPECT_EQ(out.mixed.r[kBlock * 19], 0.0f);
  EXPECT_EQ(out.mixed.l.back(), 1.0f);
  EXPECT_LE(largestStep(out.mixed.l), 1.01 * rampStep());
  EXPECT_LE(largestStep(out.mixed.r), 1.01 * rampStep());

  // Muted from the first block: silence from the first sample.
  SplitJunction muted;
  muted.prepare(kFs, kBlock);
  muted.setParameter(P::kMixMute, 1.0f);
  options.beforeBlock = nullptr;
  const RunResult silent = run(muted, in, options);
  expectSilent(silent.mixed.l, "muted left");
  expectSilent(silent.mixed.r, "muted right");
}

TEST(SplitJunctionTest, MixerGainsRampWithoutAStep) {
  const Stereo in = constantStereo(kBlock * 60, 0.5f, 0.5f);
  SplitJunction junction;
  junction.prepare(kFs, kBlock);
  RunOptions options;
  options.mix = true;
  options.beforeBlock = [](SplitJunction& j, int block) {
    if (block == 4)
      j.setParameter(P::kMixInvertB, 1.0f);  // B: +1 to -1
    if (block == 16)
      j.setParameter(P::kMixPanA, 1.0f);  // A leaves the left side
    if (block == 28)
      j.setParameter(P::kMixLevel, 12.0f);
    if (block == 40)
      j.setParameter(P::kMixLevelA, SplitJunction::kLevelFloorDb);
  };
  const RunResult out = run(junction, in, options);
  // Left: the polarity flip of B (two, times the 0.5 input), then B alone
  // rising 12 dB. Right: A and B cancel until A is taken away from +12 dB.
  const double flip = 2.0 * 0.5;
  const double rise = 0.5 * (gainOf(12.0f) - 1.0f);
  EXPECT_LE(largestStep(out.mixed.l), 1.01 * rampStep(std::max(flip, rise)));
  EXPECT_LE(largestStep(out.mixed.r), 1.01 * rampStep(0.5 * gainOf(12.0f)));
  EXPECT_EQ(out.mixed.l[kBlock * 15], 0.0f);  // A - B
  EXPECT_EQ(out.mixed.l.back(), -0.5f * gainOf(12.0f));
  EXPECT_EQ(out.mixed.r.back(), -0.5f * gainOf(12.0f));
}

TEST(SplitJunctionTest, BalancePanLawOnStereoPaths) {
  // A and B are told apart by sending only one of them (A/B splitter).
  const Stereo in = noiseStereo(kBlock * 4);
  struct Setting {
    float pan, left, right;
  };
  const Setting settings[] = {{0.0f, 1.0f, 1.0f},  {-1.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 1.0f},
                              {-0.5f, 1.0f, 0.5f}, {0.25f, 0.75f, 1.0f}};
  for (const bool sideB : {false, true}) {
    for (const Setting& s : settings) {
      SCOPED_TRACE(testing::Message() << (sideB ? "B" : "A") << " pan " << s.pan);
      SplitJunction junction;
      junction.type = SplitterType::AB;
      junction.prepare(kFs, kBlock);
      junction.setParameter(sideB ? P::kSplitLevelA : P::kSplitLevelB,
                            SplitJunction::kLevelFloorDb);
      junction.setParameter(sideB ? P::kMixPanB : P::kMixPanA, s.pan);
      RunOptions options;
      options.mix = true;
      const RunResult out = run(junction, in, options);
      // Balance, not a pan: each side keeps its own channel.
      expectEqualScaled(out.mixed.l, in.l, s.left, "left");
      expectEqualScaled(out.mixed.r, in.r, s.right, "right");
    }
  }
}

TEST(SplitJunctionTest, ConstantPowerPanLawForAMonoRowInAStereoResult) {
  const std::vector<float> x = makeNoise(kBlock * 4, 41u, 0.5f);
  const float root2 = std::sqrt(2.0f);
  const auto mixedWithPanB = [&](float pan) {
    SplitJunction junction;
    junction.type = SplitterType::AB;
    junction.prepare(kFs, kBlock);
    junction.setParameter(P::kSplitLevelA, SplitJunction::kLevelFloorDb);  // B alone
    junction.setParameter(P::kMixPanB, pan);
    RunOptions options;
    options.lowerChannels = 1;
    options.mix = true;
    return run(junction, dualMono(x), options);
  };

  // Centre is unity on both sides, the ends are one side only at +3 dB.
  RunResult out = mixedWithPanB(0.0f);
  expectEqualScaled(out.mixed.l, x, 1.0f, "centre left");
  expectEqualScaled(out.mixed.r, x, 1.0f, "centre right");
  out = mixedWithPanB(-1.0f);
  expectEqualScaled(out.mixed.l, x, root2, "hard left, left");
  expectSilent(out.mixed.r, "hard left, right");
  out = mixedWithPanB(1.0f);
  expectSilent(out.mixed.l, "hard right, left");
  expectEqualScaled(out.mixed.r, x, root2, "hard right, right");

  // In between, the two gains keep the centre's power (1 + 1) and lean the
  // way the pan points.
  for (const float pan : {-0.75f, -0.3f, 0.1f, 0.5f, 0.9f}) {
    SCOPED_TRACE(testing::Message() << "pan " << pan);
    out = mixedWithPanB(pan);
    size_t loud = 0;
    for (size_t i = 0; i < x.size(); ++i)
      if (std::abs(x[i]) > std::abs(x[loud]))
        loud = i;
    const double left = out.mixed.l[loud] / x[loud];
    const double right = out.mixed.r[loud] / x[loud];
    EXPECT_NEAR(left * left + right * right, 2.0, 1e-5);
    EXPECT_NEAR(left, std::sqrt(2.0) * std::cos((pan + 1.0) * kPi / 4.0), 1e-6);
    EXPECT_EQ(pan < 0.0f, left > right);
  }
}

TEST(SplitJunctionTest, PansDoNothingInAMonoResultOrOnDualMonoPaths) {
  const std::vector<float> x = makeNoise(kBlock * 4, 42u, 0.5f);
  {
    SplitJunction junction;
    junction.prepare(kFs, kBlock);
    junction.setParameter(P::kMixPanA, -1.0f);
    junction.setParameter(P::kMixPanB, 1.0f);
    RunOptions options;
    options.upperChannels = options.lowerChannels = 1;
    options.mix = true;
    const RunResult out = run(junction, dualMono(x), options);
    expectEqualScaled(out.mixed.l, x, 2.0f, "mono/mono");
  }
  {
    const Stereo in = noiseStereo(kBlock * 4);
    SplitJunction junction;
    junction.prepare(kFs, kBlock);
    junction.setParameter(P::kMixPanA, -1.0f);
    junction.setParameter(P::kMixPanB, 1.0f);
    RunOptions options;
    options.upperDualMono = options.lowerDualMono = true;
    options.mix = true;
    const RunResult out = run(junction, in, options);
    expectEqualScaled(out.mixed.l, in.l, 2.0f, "dual mono left");
    expectEqualScaled(out.mixed.r, in.r, 2.0f, "dual mono right");
  }
}

TEST(SplitJunctionTest, StereoBFoldsIntoAMonoResultThroughItsBalance) {
  // Upper mono, lower stereo. Give B different content per side between
  // the split and the mix, as a stereo effect in the lower row would.
  const std::vector<float> x = makeNoise(kBlock * 4, 43u, 0.5f);
  const auto mixed = [&](float panB) {
    SplitJunction junction;
    junction.type = SplitterType::AB;
    junction.prepare(kFs, kBlock);
    junction.setParameter(P::kSplitLevelA, SplitJunction::kLevelFloorDb);
    junction.setParameter(P::kMixPanB, panB);
    RunOptions options;
    options.upperChannels = 1;
    options.mix = true;
    options.between = [](juce::AudioBuffer<float>&, juce::AudioBuffer<float>& lower, int n) {
      lower.applyGain(1, 0, n, -0.5f);  // right = -0.5 left
    };
    return run(junction, dualMono(x), options);
  };
  RunResult out = mixed(0.0f);
  for (size_t i = 0; i < x.size(); ++i)
    ASSERT_EQ(out.mixed.l[i], 0.5f * (x[i] + x[i] * -0.5f));
  out = mixed(-1.0f);  // left side only
  expectEqualScaled(out.mixed.l, x, 0.5f, "pan left");
  out = mixed(1.0f);  // right side only
  expectEqualScaled(out.mixed.l, x, -0.25f, "pan right");
}

TEST(SplitJunctionTest, AlignmentDelaysNullALatePath) {
  // The upper row gains N samples of latency between the split and the
  // mix. With B delayed by the same N at the mix the two paths meet again:
  // inverted, they cancel to exact zeros; summed, they are twice the input
  // N samples late.
  const int latency = 37;
  const std::vector<float> x = makeNoise(kBlock * 16, 51u, 0.5f);
  for (const Layout& layout : kLayouts) {
    for (const bool invert : {true, false}) {
      SCOPED_TRACE(testing::Message() << layout.name << (invert ? " inverted" : " summed"));
      SplitJunction junction;
      junction.prepare(kFs, kBlock);
      junction.setAlignment(0, latency);
      EXPECT_EQ(junction.alignmentA(), 0);
      EXPECT_EQ(junction.alignmentB(), latency);
      junction.setParameter(P::kMixInvertB, invert ? 1.0f : 0.0f);

      BlockDelayLine latentBlock;
      latentBlock.configure(latency);
      RunOptions options;
      options.upperChannels = layout.upper;
      options.lowerChannels = layout.lower;
      options.mix = true;
      options.between = [&](juce::AudioBuffer<float>& upper, juce::AudioBuffer<float>&, int n) {
        latentBlock.process(upper.getArrayOfWritePointers(), upper.getNumChannels(), n);
      };
      const RunResult out = run(junction, dualMono(x), options);
      if (invert) {
        expectSilent(out.mixed.l, "left");
        expectSilent(out.mixed.r, "right");
      } else {
        for (size_t i = 0; i < x.size(); ++i) {
          const float expected = i < size_t(latency) ? 0.0f : 2.0f * x[i - size_t(latency)];
          ASSERT_EQ(out.mixed.l[i], expected) << i;
          ASSERT_EQ(out.mixed.r[i], expected) << i;
        }
      }
    }
  }
}

TEST(SplitJunctionTest, AlignmentDelaysTheOtherWayAndWithoutThemThePathsDoNotNull) {
  const int latency = 5;
  const std::vector<float> x = makeNoise(kBlock * 8, 52u, 0.5f);
  const auto mixed = [&](int alignA, int alignB) {
    SplitJunction junction;
    junction.prepare(kFs, kBlock);
    junction.setAlignment(alignA, alignB);
    junction.setParameter(P::kMixInvertB, 1.0f);
    BlockDelayLine latentBlock;
    latentBlock.configure(latency);
    RunOptions options;
    options.mix = true;
    // This time the lower row is the late one.
    options.between = [&](juce::AudioBuffer<float>&, juce::AudioBuffer<float>& lower, int n) {
      latentBlock.process(lower.getArrayOfWritePointers(), lower.getNumChannels(), n);
    };
    return run(junction, dualMono(x), options);
  };
  expectSilent(mixed(latency, 0).mixed.l, "aligned");
  const RunResult unaligned = mixed(0, 0);
  EXPECT_GT(largestStep(unaligned.mixed.l), 0.1);
}

TEST(SplitJunctionTest, ResetClearsTheAlignmentDelays) {
  const std::vector<float> x(kBlock * 2, 1.0f);
  SplitJunction junction;
  junction.prepare(kFs, kBlock);
  junction.setAlignment(16, 16);
  RunOptions options;
  options.mix = true;
  run(junction, dualMono(x), options);
  junction.reset();
  const RunResult out = run(junction, dualMono(x), options);
  for (int i = 0; i < 16; ++i)
    ASSERT_EQ(out.mixed.l[static_cast<size_t>(i)], 0.0f) << i;
  EXPECT_EQ(out.mixed.l[16], 2.0f);
  EXPECT_EQ(junction.alignmentA(), 16);  // reset keeps the configured delays
}

// ---------------------------------------------------------------------------
// Meters
// ---------------------------------------------------------------------------

TEST(SplitJunctionTest, MetersReadThePeakOfEachSide) {
  SplitJunction junction;
  junction.type = SplitterType::AB;
  junction.prepare(kFs, kBlock);
  EXPECT_EQ(junction.splitMeterDb(0), SplitJunction::kMeterFloorDb);
  junction.setParameter(P::kSplitLevelA, -6.0f);
  junction.setParameter(P::kSplitLevelB, SplitJunction::kLevelFloorDb);
  junction.setParameter(P::kMixLevelA, -20.0f);  // after the mixer's meter
  RunOptions options;
  options.mix = true;
  run(junction, constantStereo(kBlock * 2, 0.5f, -1.0f), options);
  EXPECT_NEAR(junction.splitMeterDb(0), -6.0f, 0.01f);  // peak is the -1 side
  EXPECT_EQ(junction.splitMeterDb(1), SplitJunction::kMeterFloorDb);
  EXPECT_NEAR(junction.mixMeterDb(0), -6.0f, 0.01f);
  EXPECT_EQ(junction.mixMeterDb(1), SplitJunction::kMeterFloorDb);
  junction.reset();
  EXPECT_EQ(junction.splitMeterDb(0), SplitJunction::kMeterFloorDb);
  EXPECT_EQ(junction.mixMeterDb(0), SplitJunction::kMeterFloorDb);
}

// ---------------------------------------------------------------------------
// Parameters and state
// ---------------------------------------------------------------------------

TEST(SplitJunctionTest, ParameterTableHasTheDesignIdsInOrder) {
  const char* ids[] = {"split.balance", "split.levelA", "split.levelB", "split.crossoverHz",
                       "split.swap",    "split.stereo", "mix.levelA",   "mix.panA",
                       "mix.levelB",    "mix.panB",     "mix.invertB",  "mix.level",
                       "mix.mute"};
  const BlockParamTable& table = SplitJunction::parameterSpecs();
  ASSERT_EQ(static_cast<int>(table.size()), static_cast<int>(SplitJunction::kNumParams));
  SplitJunction junction;
  for (int i = 0; i < SplitJunction::kNumParams; ++i) {
    EXPECT_EQ(table[static_cast<size_t>(i)].id, juce::String(ids[i]));
    EXPECT_EQ(SplitJunction::findParameter(ids[i]), i);
    EXPECT_EQ(junction.getParameter(i), table[static_cast<size_t>(i)].defaultValue);
  }
  EXPECT_EQ(SplitJunction::findParameter("mix.nope"), -1);
  EXPECT_EQ(table[P::kSplitSwap].type, BlockParamType::Bool);
  EXPECT_EQ(table[P::kMixLevel].unit, juce::String("dB"));
  EXPECT_EQ(table[P::kSplitCrossoverHz].unit, juce::String("Hz"));
}

TEST(SplitJunctionTest, ParametersAreClamped) {
  SplitJunction junction;
  junction.setParameter(P::kSplitBalance, 5.0f);
  EXPECT_EQ(junction.getParameter(P::kSplitBalance), 1.0f);
  junction.setParameter(P::kSplitCrossoverHz, 1.0f);
  EXPECT_EQ(junction.getParameter(P::kSplitCrossoverHz), 20.0f);
  junction.setParameter(P::kSplitCrossoverHz, 1.0e6f);
  EXPECT_EQ(junction.getParameter(P::kSplitCrossoverHz), 20000.0f);
  junction.setParameter(P::kMixLevel, -200.0f);
  EXPECT_EQ(junction.getParameter(P::kMixLevel), SplitJunction::kLevelFloorDb);
  junction.setParameter(P::kMixLevel, 99.0f);
  EXPECT_EQ(junction.getParameter(P::kMixLevel), SplitJunction::kLevelMaxDb);
  junction.setParameter(P::kMixPanB, -3.0f);
  EXPECT_EQ(junction.getParameter(P::kMixPanB), -1.0f);
  junction.setParameter(P::kMixMute, 0.7f);
  EXPECT_EQ(junction.getParameter(P::kMixMute), 1.0f);
  junction.setParameter(P::kMixPanA, std::numeric_limits<float>::quiet_NaN());
  EXPECT_EQ(junction.getParameter(P::kMixPanA), 0.0f);

  // By id, and out of range indices and unknown ids are ignored.
  EXPECT_TRUE(junction.setParameter("mix.levelA", -12.0f));
  EXPECT_EQ(junction.getParameter("mix.levelA"), -12.0f);
  EXPECT_EQ(junction.getParameter(P::kMixLevelA), -12.0f);
  EXPECT_FALSE(junction.setParameter("mix.unknown", 1.0f));
  junction.setParameter(-1, 1.0f);
  junction.setParameter(SplitJunction::kNumParams, 1.0f);
  EXPECT_EQ(junction.getParameter(-1), 0.0f);
  EXPECT_EQ(junction.getParameter(SplitJunction::kNumParams), 0.0f);
}

TEST(SplitJunctionTest, StateRoundTrip) {
  SplitJunction source;
  source.id = "split-1";
  source.upperRow = 1;
  source.splitCol = 2;
  source.mixCol = 6;
  source.type = SplitterType::Crossover;
  const float values[SplitJunction::kNumParams] = {-0.25f, -6.0f, 3.0f, 250.0f, 1.0f, 1.0f, -3.0f,
                                                   -0.5f,  2.0f,  0.75f, 1.0f,  -9.0f, 1.0f};
  for (int i = 0; i < SplitJunction::kNumParams; ++i)
    source.setParameter(i, values[i]);

  const juce::ValueTree state = source.getState();
  EXPECT_TRUE(state.hasType("Split"));
  EXPECT_EQ(state.getProperty("type").toString(), juce::String("crossover"));
  EXPECT_EQ(state.getNumChildren(), static_cast<int>(SplitJunction::kNumParams));

  // Through XML too, the way a preset travels.
  const auto xml = state.createXml();
  ASSERT_NE(xml, nullptr);
  SplitJunction loaded;
  ASSERT_TRUE(loaded.setState(juce::ValueTree::fromXml(*xml)));
  EXPECT_EQ(loaded.id, "split-1");
  EXPECT_EQ(loaded.upperRow, 1);
  EXPECT_EQ(loaded.splitCol, 2);
  EXPECT_EQ(loaded.mixCol, 6);
  EXPECT_TRUE(loaded.hasRejoin());
  EXPECT_EQ(loaded.type, SplitterType::Crossover);
  for (int i = 0; i < SplitJunction::kNumParams; ++i)
    EXPECT_EQ(loaded.getParameter(i), values[i]) << i;
  EXPECT_TRUE(loaded.getState().isEquivalentTo(state));
}

TEST(SplitJunctionTest, StateToleratesUnknownAndMissingParams) {
  juce::ValueTree state("Split");
  state.setProperty("id", "s", nullptr);
  state.setProperty("upperRow", 2, nullptr);
  state.setProperty("splitCol", 3, nullptr);
  state.setProperty("type", "ab", nullptr);
  state.setProperty("someFutureProperty", 42, nullptr);
  const auto addParam = [&state](const char* id, double value) {
    juce::ValueTree param("Param");
    param.setProperty("id", id, nullptr);
    param.setProperty("value", value, nullptr);
    state.appendChild(param, nullptr);
  };
  addParam("split.levelB", -12.0);
  addParam("split.fromTheFuture", 1.0);
  addParam("mix.panA", 9.0);  // out of range: clamped
  state.appendChild(juce::ValueTree("SomeFutureChild"), nullptr);

  SplitJunction junction;
  // Values a load must not leave behind.
  junction.setParameter(P::kMixLevel, -30.0f);
  junction.setParameter(P::kSplitSwap, 1.0f);
  junction.mixCol = 7;
  ASSERT_TRUE(junction.setState(state));
  EXPECT_EQ(junction.type, SplitterType::AB);
  EXPECT_EQ(junction.upperRow, 2);
  EXPECT_EQ(junction.splitCol, 3);
  EXPECT_EQ(junction.mixCol, -1);  // absent = no rejoin
  EXPECT_FALSE(junction.hasRejoin());
  EXPECT_EQ(junction.getParameter(P::kSplitLevelB), -12.0f);
  EXPECT_EQ(junction.getParameter(P::kMixPanA), 1.0f);
  EXPECT_EQ(junction.getParameter(P::kMixLevel), 0.0f);  // missing: default
  EXPECT_EQ(junction.getParameter(P::kSplitSwap), 0.0f);
  EXPECT_EQ(junction.getParameter(P::kSplitCrossoverHz),
            SplitJunction::parameterSpecs()[P::kSplitCrossoverHz].defaultValue);
}

TEST(SplitJunctionTest, StateOfAnUnknownTypeIsRefusedUntouched) {
  SplitJunction junction;
  junction.id = "keep";
  junction.type = SplitterType::AB;
  junction.setParameter(P::kMixLevel, -3.0f);

  juce::ValueTree future("Split");
  future.setProperty("id", "other", nullptr);
  future.setProperty("type", "multiband", nullptr);
  EXPECT_FALSE(junction.setState(future));
  EXPECT_FALSE(junction.setState(juce::ValueTree("Row")));
  EXPECT_FALSE(junction.setState(juce::ValueTree()));
  EXPECT_EQ(junction.id, "keep");
  EXPECT_EQ(junction.type, SplitterType::AB);
  EXPECT_EQ(junction.getParameter(P::kMixLevel), -3.0f);

  SplitterType type = SplitterType::AB;
  EXPECT_TRUE(splitterTypeFromName("balance", type));
  EXPECT_EQ(type, SplitterType::Balance);
  EXPECT_FALSE(splitterTypeFromName("Balance", type));
}

// ---------------------------------------------------------------------------
// Cost
// ---------------------------------------------------------------------------

namespace {

// Nanoseconds per 128-sample stereo block of split followed by mix, the way
// the effect harness measures a kind (measureKindCost): the best of three
// passes of the average and of the worst block.
struct Cost {
  double averageNs, worstNs;
};

Cost measureJunction(SplitJunction& junction, double fs, bool moving) {
  const juce::ScopedNoDenormals noDenormals;
  const int blocks = 4000;
  const std::vector<float> source = makeNoise(kBlock * 64, 7u, 0.25f);
  juce::AudioBuffer<float> upper(2, kBlock), lower(2, kBlock);
  const SplitFormat format = SplitFormat::of(upper, lower);
  Cost best{std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
  for (int pass = 0; pass < 3; ++pass) {
    junction.prepare(fs, kBlock);
    double total = 0.0, worst = 0.0;
    for (int b = 0; b < blocks; ++b) {
      const float* chunk = source.data() + static_cast<size_t>(b % 64) * kBlock;
      upper.copyFrom(0, 0, chunk, kBlock);
      upper.copyFrom(1, 0, chunk, kBlock);
      if (moving) {
        // Every ramp busy in every block: the worst the junction can cost.
        const float t = static_cast<float>(b % 2);
        junction.setParameter(P::kSplitCrossoverHz, 200.0f + 4000.0f * t);
        junction.setParameter(P::kSplitBalance, 0.5f * t);
        junction.setParameter(P::kMixLevel, -6.0f * t);
        junction.setParameter(P::kMixPanA, 0.5f * t);
      }
      const auto begin = std::chrono::steady_clock::now();
      junction.split(upper, lower, kBlock, format);
      junction.mix(upper, lower, kBlock, format);
      const auto end = std::chrono::steady_clock::now();
      const double ns = static_cast<double>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
      total += ns;
      worst = std::max(worst, ns);
    }
    best.averageNs = std::min(best.averageNs, total / blocks);
    best.worstNs = std::min(best.worstNs, worst);
  }
  return best;
}

}  // namespace

TEST(SplitJunctionTest, CpuCost) {
  struct Case {
    const char* name;
    SplitterType type;
    bool moving;
    double fs;
  };
  const Case cases[] = {
      {"balance + mix at the defaults", SplitterType::Balance, false, 48000.0},
      {"a/b + mix, every gain ramping", SplitterType::AB, true, 48000.0},
      {"crossover + mix, at rest", SplitterType::Crossover, false, 48000.0},
      {"crossover + mix, frequency and gains ramping", SplitterType::Crossover, true, 48000.0},
      {"crossover + mix, at rest, 384 kHz chain rate (8 such blocks per host block)",
       SplitterType::Crossover, false, 384000.0},
  };
  // One 128-sample block at 48 kHz lasts 2,666,667 ns.
  for (const Case& c : cases) {
    SplitJunction junction;
    junction.type = c.type;
    junction.setAlignment(0, 64);
    const Cost cost = measureJunction(junction, c.fs, c.moving);
    std::cout << "[   CPU    ] split junction, " << c.name << ": average "
              << static_cast<long long>(cost.averageNs) << " ns, worst "
              << static_cast<long long>(cost.worstNs) << " ns per 128-sample stereo block ("
              << juce::String(100.0 * cost.averageNs / 2666667.0, 3) << "% of a core at 48 kHz)"
              << std::endl;
    // A generous ceiling (1.5% of a core) that still catches an accident
    // such as an allocation or a per-sample transcendental at rest.
    EXPECT_LT(cost.averageNs, 40000.0) << c.name;
  }
}

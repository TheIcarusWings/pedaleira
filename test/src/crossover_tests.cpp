// Crossover tests: the Linkwitz-Riley 4th order splitter of SplitJunction,
// measured through the real split() call.
//
//   each band is -6.02 dB at the crossover frequency;
//   low + high is the second-order allpass of that frequency, sample for
//     sample on an impulse, and flat in magnitude from 20 Hz to 20 kHz, at
//     48 kHz and at the 8x oversampled chain rate (384 kHz);
//   the slopes are 24 dB per octave;
//   swap exchanges the bands exactly;
//   sweeping the frequency under audio stays bounded and free of clicks,
//     and leaves the filters in the state a fixed crossover would have.
#include "SplitJunction.h"
#include "test_helpers.h"

#include <gtest/gtest.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace {

constexpr int kBlock = 128;

struct Bands {
  std::vector<float> a, b;  // what the splitter sent to A and to B
};

// Mono in, mono out through a crossover junction, in blocks of kBlock.
// `beforeBlock(junction, blockIndex)` may move parameters between blocks.
template <typename BeforeBlock>
Bands runCrossover(const std::vector<float>& in, double fs, float hz, bool swap,
                   BeforeBlock&& beforeBlock) {
  const juce::ScopedNoDenormals noDenormals;
  SplitJunction junction;
  junction.type = SplitterType::Crossover;
  junction.setParameter(SplitJunction::kSplitCrossoverHz, hz);
  junction.setParameter(SplitJunction::kSplitSwap, swap ? 1.0f : 0.0f);
  junction.prepare(fs, kBlock);

  juce::AudioBuffer<float> upper(1, kBlock), lower(1, kBlock);
  Bands out;
  out.a.reserve(in.size());
  out.b.reserve(in.size());
  int block = 0;
  for (size_t pos = 0; pos < in.size(); pos += kBlock, ++block) {
    const int n = static_cast<int>(std::min<size_t>(kBlock, in.size() - pos));
    beforeBlock(junction, block);
    upper.copyFrom(0, 0, in.data() + pos, n);
    junction.split(upper, lower, n, SplitFormat::of(upper, lower));
    out.a.insert(out.a.end(), upper.getReadPointer(0), upper.getReadPointer(0) + n);
    out.b.insert(out.b.end(), lower.getReadPointer(0), lower.getReadPointer(0) + n);
  }
  return out;
}

Bands runCrossover(const std::vector<float>& in, double fs, float hz, bool swap = false) {
  return runCrossover(in, fs, hz, swap, [](SplitJunction&, int) {});
}

std::vector<float> impulse(int frames) {
  std::vector<float> x(static_cast<size_t>(frames), 0.0f);
  x[0] = 1.0f;
  return x;
}

// Long enough for the response of a crossover at `hz` to decay below
// anything a float holds (the envelope falls as exp(-2 pi hz t / sqrt 2)).
int impulseFrames(double fs, double hz) {
  const double seconds = 20.0 / (2.0 * kPi * hz / std::sqrt(2.0));
  const int frames = static_cast<int>(std::ceil(seconds * fs)) + 4096;
  return ((frames + kBlock - 1) / kBlock) * kBlock;
}

// The response at one frequency of a filter given by its impulse response.
template <typename Sample>
std::complex<double> responseAt(const std::vector<Sample>& h, double hz, double fs) {
  // A rotating phasor instead of cos/sin per sample; re-seeded now and then
  // so its rounding does not build up over a long response.
  const double w = -2.0 * kPi * hz / fs;
  const std::complex<double> rot(std::cos(w), std::sin(w));
  std::complex<double> acc(0.0, 0.0), phasor(1.0, 0.0);
  for (size_t i = 0; i < h.size(); ++i) {
    if ((i & 1023u) == 0)
      phasor = std::complex<double>(std::cos(w * static_cast<double>(i)),
                                    std::sin(w * static_cast<double>(i)));
    acc += static_cast<double>(h[i]) * phasor;
    phasor *= rot;
  }
  return acc;
}

template <typename Sample>
double magnitudeDb(const std::vector<Sample>& h, double hz, double fs) {
  return 20.0 * std::log10(std::max(std::abs(responseAt(h, hz, fs)), 1e-15));
}

std::vector<double> sumOf(const Bands& bands) {
  std::vector<double> sum(bands.a.size());
  for (size_t i = 0; i < sum.size(); ++i)
    sum[i] = static_cast<double>(bands.a[i]) + static_cast<double>(bands.b[i]);
  return sum;
}

// The impulse response of the second-order Butterworth allpass
// (s^2 - sqrt2 s + 1) / (s^2 + sqrt2 s + 1) at `hz`, through the bilinear
// transform with the frequency pre-warped: what the two Linkwitz-Riley
// bands must add up to. Written out here, independently of the code under
// test.
std::vector<double> analyticAllpass(int frames, double hz, double fs) {
  const double g = std::tan(kPi * hz / fs);
  const double root2 = std::sqrt(2.0);
  const double a0 = 1.0 + root2 * g + g * g;
  const double b0 = (1.0 - root2 * g + g * g) / a0;
  const double b1 = 2.0 * (g * g - 1.0) / a0;
  const double b2 = 1.0;
  const double a1 = b1;
  const double a2 = b0;
  std::vector<double> y(static_cast<size_t>(frames));
  double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
  for (int i = 0; i < frames; ++i) {
    const double x = i == 0 ? 1.0 : 0.0;
    const double out = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1;
    x1 = x;
    y2 = y1;
    y1 = out;
    y[static_cast<size_t>(i)] = out;
  }
  return y;
}

struct Case {
  double fs;
  float hz;
};

// Both chain rates the design names, with crossovers across the range,
// the ends of it included.
const Case kCases[] = {
    {48000.0, 20.0f},  {48000.0, 80.0f},   {48000.0, 500.0f},   {48000.0, 3000.0f},
    {48000.0, 12000.0f}, {48000.0, 20000.0f}, {384000.0, 20.0f},  {384000.0, 250.0f},
    {384000.0, 2000.0f}, {384000.0, 20000.0f},
};

}  // namespace

TEST(CrossoverTest, EachBandIsMinusSixDbAtTheCrossover) {
  double worst = 0.0;
  for (const Case& c : kCases) {
    SCOPED_TRACE(testing::Message() << c.hz << " Hz at " << c.fs);
    const Bands bands = runCrossover(impulse(impulseFrames(c.fs, c.hz)), c.fs, c.hz);
    const double expected = 20.0 * std::log10(0.5);  // -6.0206 dB
    const double low = magnitudeDb(bands.a, c.hz, c.fs);
    const double high = magnitudeDb(bands.b, c.hz, c.fs);
    EXPECT_NEAR(low, expected, 0.01);
    EXPECT_NEAR(high, expected, 0.01);
    worst = std::max({worst, std::abs(low - expected), std::abs(high - expected)});
  }
  std::cout << "[ CROSSOVER ] band level at fc: worst error " << worst << " dB from -6.0206 dB"
            << std::endl;
}

TEST(CrossoverTest, LowPlusHighIsTheAnalyticAllpass) {
  double worst = 0.0;
  for (const Case& c : kCases) {
    SCOPED_TRACE(testing::Message() << c.hz << " Hz at " << c.fs);
    const int frames = impulseFrames(c.fs, c.hz);
    const std::vector<double> sum = sumOf(runCrossover(impulse(frames), c.fs, c.hz));
    const std::vector<double> reference = analyticAllpass(frames, c.hz, c.fs);
    double maxError = 0.0;
    for (size_t i = 0; i < sum.size(); ++i)
      maxError = std::max(maxError, std::abs(sum[i] - reference[i]));
    EXPECT_LT(maxError, 1e-5);
    worst = std::max(worst, maxError);
  }
  std::cout << "[ CROSSOVER ] low + high against the analytic allpass: worst sample error "
            << worst << std::endl;
}

TEST(CrossoverTest, SummedMagnitudeIsFlat) {
  double worst = 0.0;
  for (const Case& c : kCases) {
    SCOPED_TRACE(testing::Message() << c.hz << " Hz at " << c.fs);
    const std::vector<double> sum =
        sumOf(runCrossover(impulse(impulseFrames(c.fs, c.hz)), c.fs, c.hz));
    // 20 Hz to 20 kHz, twelve points per octave.
    for (double hz = 20.0; hz <= 20000.0; hz *= std::pow(2.0, 1.0 / 12.0)) {
      const double level = magnitudeDb(sum, hz, c.fs);
      EXPECT_NEAR(level, 0.0, 0.01) << "at " << hz << " Hz";
      worst = std::max(worst, std::abs(level));
    }
  }
  std::cout << "[ CROSSOVER ] summed magnitude 20 Hz to 20 kHz: worst deviation " << worst
            << " dB" << std::endl;
}

TEST(CrossoverTest, SlopesAreTwentyFourDbPerOctave) {
  // Far enough from the crossover that the slope has reached its asymptote
  // (|H| = 1 / (1 + r^4), r the frequency ratio: 24.07 dB between r = 4 and
  // r = 8), and at the oversampled rate so the bilinear warp near Nyquist
  // does not steepen it.
  const double fs = 384000.0;
  {
    const float hz = 250.0f;
    const Bands bands = runCrossover(impulse(impulseFrames(fs, hz)), fs, hz);
    const double octave =
        magnitudeDb(bands.a, 4.0 * hz, fs) - magnitudeDb(bands.a, 8.0 * hz, fs);
    std::cout << "[ CROSSOVER ] low band slope: " << octave << " dB per octave" << std::endl;
    EXPECT_NEAR(octave, 24.07, 0.1);
    // And the pass band is untouched.
    EXPECT_NEAR(magnitudeDb(bands.a, hz / 8.0, fs), 0.0, 0.01);
  }
  {
    const float hz = 4000.0f;
    const Bands bands = runCrossover(impulse(impulseFrames(fs, hz)), fs, hz);
    const double octave =
        magnitudeDb(bands.b, hz / 4.0, fs) - magnitudeDb(bands.b, hz / 8.0, fs);
    std::cout << "[ CROSSOVER ] high band slope: " << octave << " dB per octave" << std::endl;
    EXPECT_NEAR(octave, 24.07, 0.1);
    EXPECT_NEAR(magnitudeDb(bands.b, hz * 8.0, fs), 0.0, 0.01);
  }
}

TEST(CrossoverTest, LowGoesToAAndHighToB) {
  const float hz = 1000.0f;
  const Bands bands = runCrossover(impulse(impulseFrames(kFs, hz)), kFs, hz);
  EXPECT_NEAR(magnitudeDb(bands.a, 100.0, kFs), 0.0, 0.01);
  EXPECT_LT(magnitudeDb(bands.a, 8000.0, kFs), -60.0);
  EXPECT_NEAR(magnitudeDb(bands.b, 8000.0, kFs), 0.0, 0.01);
  EXPECT_LT(magnitudeDb(bands.b, 100.0, kFs), -60.0);
}

TEST(CrossoverTest, SwapExchangesTheBandsExactly) {
  const std::vector<float> noise = makeNoise(kBlock * 64, 11u, 0.5f);
  const Bands straight = runCrossover(noise, kFs, 700.0f, false);
  const Bands swapped = runCrossover(noise, kFs, 700.0f, true);
  ASSERT_EQ(straight.a.size(), swapped.a.size());
  for (size_t i = 0; i < straight.a.size(); ++i) {
    ASSERT_EQ(straight.a[i], swapped.b[i]) << "sample " << i;
    ASSERT_EQ(straight.b[i], swapped.a[i]) << "sample " << i;
  }
}

TEST(CrossoverTest, TogglingSwapUnderAudioCrossfades) {
  // A low sine sits wholly in the low band, so a swap moves it from A to B.
  // An instant exchange would be a full-scale step; the crossfade moves at
  // most 1/ramp of the amplitude per sample on top of the sine's own step.
  const float amplitude = 0.5f;
  const double sineHz = 100.0;
  const std::vector<float> sine = makeSine(kBlock * 100, sineHz, amplitude);
  const Bands bands = runCrossover(sine, kFs, 2000.0f, false, [](SplitJunction& j, int block) {
    if (block == 50)
      j.setParameter(SplitJunction::kSplitSwap, 1.0f);
  });
  const double sineStep = amplitude * 2.0 * kPi * sineHz / kFs;
  const double rampStep = amplitude / (kFs * SplitJunction::kRampSeconds);
  double maxStep = 0.0;
  for (size_t i = 1; i < bands.a.size(); ++i)
    maxStep = std::max({maxStep, std::abs(double(bands.a[i]) - bands.a[i - 1]),
                        std::abs(double(bands.b[i]) - bands.b[i - 1])});
  EXPECT_LT(maxStep, sineStep + 1.5 * rampStep);
  // And it did move: A ends silent, B carries the sine.
  const size_t tail = bands.a.size() - 2048;
  EXPECT_LT(db(goertzelPower(bands.a.data() + tail, 2048, sineHz)), -60.0);
  EXPECT_NEAR(db(goertzelPower(bands.b.data() + tail, 2048, sineHz)),
              20.0 * std::log10(amplitude), 0.1);
}

TEST(CrossoverTest, StereoChannelsAreFilteredIndependently) {
  const juce::ScopedNoDenormals noDenormals;
  const std::vector<float> left = makeNoise(kBlock * 16, 3u, 0.5f);
  const std::vector<float> right = makeNoise(kBlock * 16, 4u, 0.5f);
  const Bands monoLeft = runCrossover(left, kFs, 900.0f);
  const Bands monoRight = runCrossover(right, kFs, 900.0f);

  SplitJunction junction;
  junction.type = SplitterType::Crossover;
  junction.setParameter(SplitJunction::kSplitCrossoverHz, 900.0f);
  junction.prepare(kFs, kBlock);
  juce::AudioBuffer<float> upper(2, kBlock), lower(2, kBlock);
  for (size_t pos = 0; pos < left.size(); pos += kBlock) {
    upper.copyFrom(0, 0, left.data() + pos, kBlock);
    upper.copyFrom(1, 0, right.data() + pos, kBlock);
    junction.split(upper, lower, kBlock, SplitFormat::of(upper, lower));
    for (int i = 0; i < kBlock; ++i) {
      ASSERT_EQ(upper.getSample(0, i), monoLeft.a[pos + i]);
      ASSERT_EQ(upper.getSample(1, i), monoRight.a[pos + i]);
      ASSERT_EQ(lower.getSample(0, i), monoLeft.b[pos + i]);
      ASSERT_EQ(lower.getSample(1, i), monoRight.b[pos + i]);
    }
  }
}

TEST(CrossoverTest, SweepUnderAudioStaysBoundedAndClickFree) {
  // A steady sine while the crossover is thrown across its whole range and
  // back, a new target every few blocks: slow glides, retargets in the
  // middle of a ramp and full-range jumps.
  const float amplitude = 0.5f;
  const double sineHz = 1000.0;
  const int sweepBlocks = 1500;  // 4 seconds
  const int settleBlocks = 400;
  const std::vector<float> sine = makeSine(kBlock * (sweepBlocks + settleBlocks), sineHz, amplitude);
  const float restHz = 640.0f;

  const auto sweep = [=](SplitJunction& j, int block) {
    if (block >= sweepBlocks) {
      j.setParameter(SplitJunction::kSplitCrossoverHz, restHz);
      return;
    }
    float hz = restHz;
    if (block < 500) {
      // A glide up and down in log frequency, retargeted every block.
      const double phase = std::abs(std::fmod(block / 125.0, 2.0) - 1.0);
      hz = static_cast<float>(20.0 * std::pow(1000.0, phase));
    } else if ((block / 3) % 2 == 0) {
      hz = 20.0f;  // end to end jumps, faster than the ramp can finish
    } else {
      hz = 20000.0f;
    }
    j.setParameter(SplitJunction::kSplitCrossoverHz, hz);
  };
  const Bands bands = runCrossover(sine, kFs, restHz, false, sweep);

  // The sine's own largest step. A click is a jump of the order of the
  // amplitude; a moving allpass can only add the slope of its phase
  // movement, which the 20 ms ramp keeps to the same order as the sine's.
  const double sineStep = amplitude * 2.0 * kPi * sineHz / kFs;
  double peak = 0.0, maxStep = 0.0, maxSumStep = 0.0, sumPeak = 0.0;
  const size_t sweepEnd = static_cast<size_t>(kBlock) * sweepBlocks;
  for (size_t i = 1; i < sweepEnd; ++i) {
    ASSERT_TRUE(std::isfinite(bands.a[i]) && std::isfinite(bands.b[i])) << "sample " << i;
    const double sum = double(bands.a[i]) + bands.b[i];
    const double previousSum = double(bands.a[i - 1]) + bands.b[i - 1];
    peak = std::max({peak, std::abs(double(bands.a[i])), std::abs(double(bands.b[i]))});
    sumPeak = std::max(sumPeak, std::abs(sum));
    maxStep = std::max({maxStep, std::abs(double(bands.a[i]) - bands.a[i - 1]),
                        std::abs(double(bands.b[i]) - bands.b[i - 1])});
    maxSumStep = std::max(maxSumStep, std::abs(sum - previousSum));
  }
  std::cout << "[ CROSSOVER ] sweep on a " << amplitude << " sine (own step " << sineStep
            << "): band peak " << peak << ", sum peak " << sumPeak << ", largest band step "
            << maxStep << ", largest sum step " << maxSumStep << std::endl;
  EXPECT_LT(peak, 2.0 * amplitude);
  EXPECT_LT(sumPeak, 2.0 * amplitude);
  EXPECT_LT(maxStep, 3.0 * sineStep);
  EXPECT_LT(maxSumStep, 3.0 * sineStep);

  // Once the frequency rests, the filters hold what a crossover that never
  // moved holds: the modulation left nothing behind in the state.
  const Bands fixed = runCrossover(sine, kFs, restHz);
  double residue = 0.0;
  for (size_t i = bands.a.size() - 4096; i < bands.a.size(); ++i)
    residue = std::max({residue, std::abs(double(bands.a[i]) - fixed.a[i]),
                        std::abs(double(bands.b[i]) - fixed.b[i])});
  EXPECT_LT(residue, 1e-5);
}

TEST(CrossoverTest, BlockSizeDoesNotChangeTheResult) {
  const juce::ScopedNoDenormals noDenormals;
  const std::vector<float> noise = makeNoise(kBlock * 32, 21u, 0.5f);
  const Bands reference = runCrossover(noise, kFs, 1200.0f);

  SplitJunction junction;
  junction.type = SplitterType::Crossover;
  junction.setParameter(SplitJunction::kSplitCrossoverHz, 1200.0f);
  junction.prepare(kFs, kBlock);
  juce::AudioBuffer<float> upper(1, kBlock), lower(1, kBlock);
  const int sizes[] = {1, 7, 128, 33, 64, 2, 100};
  size_t pos = 0;
  for (int turn = 0; pos < noise.size(); ++turn) {
    const int n = static_cast<int>(std::min<size_t>(sizes[turn % 7], noise.size() - pos));
    upper.copyFrom(0, 0, noise.data() + pos, n);
    junction.split(upper, lower, n, SplitFormat::of(upper, lower));
    for (int i = 0; i < n; ++i) {
      ASSERT_EQ(upper.getSample(0, i), reference.a[pos + i]);
      ASSERT_EQ(lower.getSample(0, i), reference.b[pos + i]);
    }
    pos += static_cast<size_t>(n);
  }
}

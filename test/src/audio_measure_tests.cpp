// AudioMeasure: each measurement against a synthetic signal whose answer is
// known in closed form, so the numbers an agent steers by mean what the
// documentation says they mean.

#include "AudioMeasure.h"
#include "test_helpers.h"

#include <gtest/gtest.h>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace {

juce::AudioBuffer<float> monoBuffer(const std::vector<float>& x) {
  juce::AudioBuffer<float> audio(1, static_cast<int>(x.size()));
  std::copy(x.begin(), x.end(), audio.getWritePointer(0));
  return audio;
}

juce::AudioBuffer<float> stereoBuffer(const std::vector<float>& l, const std::vector<float>& r) {
  juce::AudioBuffer<float> audio(2, static_cast<int>(l.size()));
  std::copy(l.begin(), l.end(), audio.getWritePointer(0));
  std::copy(r.begin(), r.end(), audio.getWritePointer(1));
  return audio;
}

// Pink noise built in the frequency domain: every bin's power is 1 / f with
// a random phase, so each octave holds exactly the same energy.
std::vector<float> pinkNoise(int order, unsigned seed) {
  const int size = 1 << order;
  juce::dsp::FFT fft(order);
  std::vector<float> data(static_cast<size_t>(size) * 2, 0.0f);
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> phase(0.0, 2.0 * kPi);
  for (int bin = 1; bin < size / 2; ++bin) {
    const double magnitude = 1.0 / std::sqrt(static_cast<double>(bin));
    const double angle = phase(rng);
    data[static_cast<size_t>(bin) * 2] = static_cast<float>(magnitude * std::cos(angle));
    data[static_cast<size_t>(bin) * 2 + 1] = static_cast<float>(magnitude * std::sin(angle));
  }
  fft.performRealOnlyInverseTransform(data.data());
  std::vector<float> x(data.begin(), data.begin() + size);
  float peak = 0.0f;
  for (const float v : x)
    peak = std::max(peak, std::abs(v));
  for (float& v : x)
    v *= 0.5f / peak;
  return x;
}

std::vector<float> hardClip(std::vector<float> x, float drive, float ceiling) {
  for (float& v : x)
    v = juce::jlimit(-ceiling, ceiling, v * drive);
  return x;
}

// The share of `y` that is not a multiple of `x`, as an amplitude
// percentage: for a sine in, exactly the harmonics the clipping added.
double residualPercent(const std::vector<float>& x, const std::vector<float>& y) {
  double xy = 0.0, xx = 0.0, yy = 0.0;
  for (size_t i = 0; i < x.size(); ++i) {
    xy += static_cast<double>(x[i]) * y[i];
    xx += static_cast<double>(x[i]) * x[i];
    yy += static_cast<double>(y[i]) * y[i];
  }
  return 100.0 * std::sqrt(std::max(0.0, 1.0 - xy * xy / (xx * yy)));
}

}  // namespace

TEST(AudioMeasure, SineLevelsAreItsAmplitude) {
  // Amplitude 0.25 is -12.04 dBFS peak and 3.01 dB less in RMS.
  const auto audio = monoBuffer(makeSine(96000, 997.0, 0.25f));
  const AudioMeasure::Levels levels = AudioMeasure::measureLevels(audio, kFs);
  EXPECT_NEAR(levels.peakDb, -12.04, 0.02);
  EXPECT_NEAR(levels.rmsDb, -15.05, 0.02);
  EXPECT_NEAR(levels.crestDb, 3.01, 0.03);
  EXPECT_NEAR(levels.truePeakDb, -12.04, 0.05);
}

TEST(AudioMeasure, LoudnessOfTheReferenceSine) {
  // BS.1770's own check: a full-scale 997 Hz sine on one channel reads
  // -3.01 LKFS, because the K-weighting gain at 997 Hz cancels the -0.691
  // offset. Level changes carry over dB for dB.
  const auto loud = monoBuffer(makeSine(4 * 48000, 997.0, 1.0f));
  EXPECT_NEAR(AudioMeasure::integratedLoudness(loud, kFs), -3.01, 0.05);
  const auto quiet = monoBuffer(makeSine(4 * 48000, 997.0, 0.1f));
  EXPECT_NEAR(AudioMeasure::integratedLoudness(quiet, kFs), -23.01, 0.05);

  // The same sine on both channels is 3 dB louder: channel powers add.
  const auto x = makeSine(4 * 48000, 997.0, 0.1f);
  EXPECT_NEAR(AudioMeasure::integratedLoudness(stereoBuffer(x, x), kFs), -20.0, 0.05);

  // The filters are derived per rate, so another rate reads the same.
  const auto at44 = monoBuffer(makeSine(4 * 44100, 997.0, 1.0f, 44100.0));
  EXPECT_NEAR(AudioMeasure::integratedLoudness(at44, 44100.0), -3.01, 0.05);
}

TEST(AudioMeasure, LoudnessIsKWeightedAndGated) {
  // K-weighting: the 38 Hz high-pass takes a 30 Hz sine down, the shelf
  // lifts a 4 kHz one by close to 4 dB.
  const double mid = AudioMeasure::integratedLoudness(monoBuffer(makeSine(192000, 997.0, 0.1f)), kFs);
  const double low = AudioMeasure::integratedLoudness(monoBuffer(makeSine(192000, 30.0, 0.1f)), kFs);
  const double high = AudioMeasure::integratedLoudness(monoBuffer(makeSine(192000, 4000.0, 0.1f)), kFs);
  EXPECT_LT(low, mid - 3.0);
  EXPECT_NEAR(high - mid, 3.0, 0.8);

  // Gating: silence around a tone does not pull the reading down.
  std::vector<float> padded(48000 * 6, 0.0f);
  const auto tone = makeSine(48000 * 2, 997.0, 0.1f);
  std::copy(tone.begin(), tone.end(), padded.begin() + 48000 * 2);
  // (Ungated, a third of the time at this level would read 4.8 dB lower.
  // The blocks that straddle the tone's edges pass the relative gate, as
  // the standard has it, and cost a little.)
  EXPECT_NEAR(AudioMeasure::integratedLoudness(monoBuffer(padded), kFs), mid, 0.8);

  // Nothing at all reads the floor, not minus infinity.
  EXPECT_EQ(AudioMeasure::integratedLoudness(monoBuffer(std::vector<float>(48000, 0.0f)), kFs),
            AudioMeasure::kFloorDb);
}

TEST(AudioMeasure, TruePeakFindsWhatSamplesMiss) {
  // A sine at a quarter of the sample rate, sampled 45 degrees off its
  // peaks: every sample is 0.707 of the amplitude, the wave itself reaches 1.
  std::vector<float> x(4800);
  for (size_t i = 0; i < x.size(); ++i)
    x[i] = static_cast<float>(std::sin(2.0 * kPi * 0.25 * static_cast<double>(i) + kPi / 4.0));
  const AudioMeasure::Levels levels = AudioMeasure::measureLevels(monoBuffer(x), kFs);
  EXPECT_NEAR(levels.peakDb, -3.01, 0.02);
  EXPECT_NEAR(levels.truePeakDb, 0.0, 0.3);
}

TEST(AudioMeasure, PinkNoiseFillsTheBandsByTheirWidthInOctaves) {
  const auto audio = monoBuffer(pinkNoise(19, 7));
  const AudioMeasure::Spectrum spectrum = AudioMeasure::measureSpectrum(audio, kFs);

  double totalOctaves = 0.0;
  for (const auto& band : AudioMeasure::kBands)
    totalOctaves += std::log2(band.highHz / band.lowHz);
  double shareSum = 0.0;
  for (size_t band = 0; band < AudioMeasure::kBands.size(); ++band) {
    const double expected =
        std::log2(AudioMeasure::kBands[band].highHz / AudioMeasure::kBands[band].lowHz) /
        totalOctaves;
    EXPECT_NEAR(spectrum.db[band], 10.0 * std::log10(expected), 0.6)
        << AudioMeasure::kBands[band].id;
    shareSum += spectrum.share[band];
  }
  EXPECT_NEAR(shareSum, 1.0, 1.0e-6);
  // sub and bass together: log2(250 / 20) of log2(20000 / 20) octaves.
  EXPECT_NEAR(spectrum.lowEndShare, std::log2(250.0 / 20.0) / totalOctaves, 0.03);
}

TEST(AudioMeasure, SpectrumOfOneAndTwoSines) {
  // One sine: all the energy in its band, the centroid on its frequency.
  const AudioMeasure::Spectrum one =
      AudioMeasure::measureSpectrum(monoBuffer(makeSine(96000, 1000.0, 0.5f)), kFs);
  EXPECT_GT(one.share[3], 0.999);  // mid
  EXPECT_NEAR(one.centroidHz, 1000.0, 5.0);

  // Two sines of equal level: half each, the centroid halfway.
  auto x = makeSine(96000, 100.0, 0.3f);
  const auto y = makeSine(96000, 5000.0, 0.3f);
  for (size_t i = 0; i < x.size(); ++i)
    x[i] += y[i];
  const AudioMeasure::Spectrum two = AudioMeasure::measureSpectrum(monoBuffer(x), kFs);
  EXPECT_NEAR(two.share[1], 0.5, 0.01);  // bass
  EXPECT_NEAR(two.share[5], 0.5, 0.01);  // presence
  EXPECT_NEAR(two.centroidHz, 2550.0, 30.0);
  EXPECT_NEAR(two.lowEndShare, 0.5, 0.01);
}

TEST(AudioMeasure, DistortionOfCleanAndClippedSines) {
  const auto x = makeSine(96000, 220.0, 0.5f);

  // Clean, at any gain: nothing a linear filter cannot explain.
  std::vector<float> scaled(x);
  for (float& v : scaled)
    v *= 0.3f;
  const AudioMeasure::Distortion clean =
      AudioMeasure::measureDistortion(x.data(), scaled.data(), static_cast<int>(x.size()), kFs);
  ASSERT_TRUE(clean.valid);
  EXPECT_LT(clean.percent, 0.5);

  // Clipped: the harmonics are what the fit cannot explain, and their share
  // is known exactly from the projection onto the input.
  for (const float drive : {1.5f, 3.0f, 10.0f}) {
    const auto clipped = hardClip(x, drive, 0.5f);
    const AudioMeasure::Distortion measured = AudioMeasure::measureDistortion(
        x.data(), clipped.data(), static_cast<int>(x.size()), kFs);
    ASSERT_TRUE(measured.valid);
    EXPECT_NEAR(measured.percent, residualPercent(x, clipped), 1.5) << "drive " << drive;
  }
  // And more drive reads as more distortion.
  const auto mild = hardClip(x, 1.5f, 0.5f), heavy = hardClip(x, 10.0f, 0.5f);
  EXPECT_LT(AudioMeasure::measureDistortion(x.data(), mild.data(), 96000, kFs).percent,
            AudioMeasure::measureDistortion(x.data(), heavy.data(), 96000, kFs).percent);
}

TEST(AudioMeasure, DistortionIgnoresFilteringAndShortEchoes) {
  // A broadband input through a linear system (a one-pole low-pass plus an
  // echo 20 ms later) is not distortion: the fit accounts for both. (The
  // input ends in silence, so the echo of its last moments is in the
  // output: a render with a tail.)
  auto x = makeNoise(144000, 11, 0.5f);
  std::fill(x.end() - 9600, x.end(), 0.0f);
  std::vector<float> y(x.size(), 0.0f);
  float state = 0.0f;
  for (size_t i = 0; i < x.size(); ++i) {
    state += 0.2f * (x[i] - state);
    y[i] = state + (i >= 960 ? 0.5f * x[i - 960] : 0.0f);
  }
  const AudioMeasure::Distortion linear =
      AudioMeasure::measureDistortion(x.data(), y.data(), static_cast<int>(x.size()), kFs);
  ASSERT_TRUE(linear.valid);
  EXPECT_LT(linear.percent, 1.0);

  // A longer echo, a quarter of a second, is still inside what the fit
  // models for a clip of this length.
  std::vector<float> echoed(x.size(), 0.0f);
  for (size_t i = 0; i < x.size(); ++i)
    echoed[i] = x[i] + (i >= 4800 ? 0.7f * x[i - 4800] : 0.0f);
  EXPECT_LT(AudioMeasure::measureDistortion(x.data(), echoed.data(), 144000, kFs).percent, 1.5);

  // The same system followed by clipping is.
  const auto clipped = hardClip(y, 4.0f, 0.5f);
  const AudioMeasure::Distortion nonlinear =
      AudioMeasure::measureDistortion(x.data(), clipped.data(), static_cast<int>(x.size()), kFs);
  EXPECT_GT(nonlinear.percent, 25.0);

  // Silence on either side is no measurement, not zero distortion.
  const std::vector<float> silence(x.size(), 0.0f);
  EXPECT_FALSE(AudioMeasure::measureDistortion(x.data(), silence.data(), 144000, kFs).valid);
}

TEST(AudioMeasure, LinearFitReadsAKnownFilter) {
  // A gain of -6.02 dB with a 2 ms delay: flat magnitude, and the fit says
  // so wherever the noise input has energy.
  auto x = makeNoise(192000, 3, 0.25f);
  std::fill(x.end() - 4800, x.end(), 0.0f);
  std::vector<float> y(x.size(), 0.0f);
  for (size_t i = 96; i < x.size(); ++i)
    y[i] = 0.5f * x[i - 96];
  const AudioMeasure::LinearFit fit(x.data(), y.data(), static_cast<int>(x.size()), kFs);
  EXPECT_LT(fit.distortion().percent, 0.5);
  for (const double hz : {50.0, 200.0, 1000.0, 5000.0, 15000.0}) {
    EXPECT_NEAR(fit.gainDbAt(hz), -6.02, 0.3) << hz;
    EXPECT_GT(fit.coherenceAt(hz), 0.98) << hz;
  }
}

TEST(AudioMeasure, StereoWidthAndCorrelation) {
  const auto a = makeNoise(48000, 1, 0.5f);
  const auto b = makeNoise(48000, 2, 0.5f);
  std::vector<float> inverted(a);
  for (float& v : inverted)
    v = -v;

  const AudioMeasure::Stereo mono = AudioMeasure::measureStereo(stereoBuffer(a, a));
  EXPECT_NEAR(mono.correlation, 1.0, 1.0e-6);
  EXPECT_NEAR(mono.width, 0.0, 1.0e-4);
  EXPECT_NEAR(mono.balanceDb, 0.0, 0.01);

  const AudioMeasure::Stereo unrelated = AudioMeasure::measureStereo(stereoBuffer(a, b));
  EXPECT_NEAR(unrelated.correlation, 0.0, 0.03);
  EXPECT_NEAR(unrelated.width, std::sqrt(0.5), 0.02);

  const AudioMeasure::Stereo outOfPhase = AudioMeasure::measureStereo(stereoBuffer(a, inverted));
  EXPECT_NEAR(outOfPhase.correlation, -1.0, 1.0e-6);
  EXPECT_NEAR(outOfPhase.width, 1.0, 1.0e-4);

  // Right 6 dB down: the balance says so.
  std::vector<float> quieter(a);
  for (float& v : quieter)
    v *= 0.5f;
  EXPECT_NEAR(AudioMeasure::measureStereo(stereoBuffer(a, quieter)).balanceDb, -6.02, 0.02);

  // One channel only: mono by definition.
  EXPECT_NEAR(AudioMeasure::measureStereo(monoBuffer(a)).correlation, 1.0, 1.0e-9);
}

TEST(AudioMeasure, TailLengthOfADecayAndOfSilence) {
  // One second of tone, then a decay of 60 dB per 0.5 s: the tail is the
  // time the decay takes to fall 60 dB below the tone's level.
  const int toneFrames = 48000, total = 48000 * 3;
  std::vector<float> x(static_cast<size_t>(total), 0.0f);
  for (int i = 0; i < total; ++i) {
    const double envelope =
        i < toneFrames ? 1.0 : std::pow(10.0, -60.0 / 20.0 * (i - toneFrames) / 24000.0);
    x[static_cast<size_t>(i)] = static_cast<float>(0.5 * envelope * std::sin(2.0 * kPi * 440.0 * i / kFs));
  }
  bool truncated = true;
  EXPECT_NEAR(AudioMeasure::tailSeconds(monoBuffer(x), toneFrames, kFs, &truncated), 0.5, 0.03);
  EXPECT_FALSE(truncated);

  // A buffer that ends while the decay is still above the threshold says so.
  std::vector<float> cut(x.begin(), x.begin() + toneFrames + 12000);
  EXPECT_NEAR(AudioMeasure::tailSeconds(monoBuffer(cut), toneFrames, kFs, &truncated), 0.25, 0.01);
  EXPECT_TRUE(truncated);

  // Nothing after the input: no tail.
  std::vector<float> dry(x.begin(), x.begin() + toneFrames);
  dry.resize(static_cast<size_t>(total), 0.0f);
  EXPECT_NEAR(AudioMeasure::tailSeconds(monoBuffer(dry), toneFrames, kFs, &truncated), 0.0, 1.0e-9);
  EXPECT_FALSE(truncated);
}

TEST(AudioMeasure, HumIsFoundAtItsFrequencyAndLevel) {
  // Notes with 60 Hz hum at -46 dBFS under them.
  std::vector<float> x(48000 * 4);
  const auto hum = makeSine(48000 * 4, 60.0, 0.005f);
  for (size_t i = 0; i < x.size(); ++i) {
    const double t = static_cast<double>(i % 24000) / kFs;
    x[i] = static_cast<float>(0.3 * std::exp(-6.0 * t) * std::sin(2.0 * kPi * 196.0 * t)) + hum[i];
  }
  const AudioMeasure::Hum found =
      AudioMeasure::measureHum(x.data(), static_cast<int>(x.size()), kFs);
  EXPECT_EQ(found.mainsHz, 60);
  EXPECT_NEAR(found.hz60Db, -46.02, 0.5);
  EXPECT_LT(found.hz50Db, found.hz60Db - 10.0);
  EXPECT_NEAR(found.levelDb, -46.02, 0.6);

  // 50 Hz mains is told apart.
  const auto hum50 = makeSine(48000 * 4, 50.0, 0.01f);
  EXPECT_EQ(AudioMeasure::measureHum(hum50.data(), static_cast<int>(hum50.size()), kFs).mainsHz, 50);

  // No hum: no mains frequency, the floor for a level.
  const auto clean = makeSine(48000 * 4, 196.0, 0.3f);
  const AudioMeasure::Hum none =
      AudioMeasure::measureHum(clean.data(), static_cast<int>(clean.size()), kFs);
  EXPECT_EQ(none.mainsHz, 0);
  EXPECT_EQ(none.levelDb, AudioMeasure::kFloorDb);

  // toneLevelDb reads a component's amplitude wherever it is asked to look.
  EXPECT_NEAR(AudioMeasure::toneLevelDb(clean.data(), static_cast<int>(clean.size()), 196.0, kFs),
              20.0 * std::log10(0.3), 0.1);
}

TEST(AudioMeasure, NoiseFloorIsTheQuietPart) {
  // Loud bursts over a steady noise bed at about -60 dBFS RMS.
  std::vector<float> x = makeNoise(48000 * 4, 5, 0.001f * std::sqrt(3.0f));
  const auto burst = makeSine(4800, 440.0, 0.5f);
  for (int n = 0; n < 4; ++n)
    for (size_t i = 0; i < burst.size(); ++i)
      x[static_cast<size_t>(n) * 48000 + i] += burst[i];
  EXPECT_NEAR(AudioMeasure::noiseFloorDb(x.data(), static_cast<int>(x.size()), kFs), -60.0, 1.5);
}

TEST(AudioMeasure, ReadingsAreRepeatableAndPrintShort) {
  const auto audio = monoBuffer(pinkNoise(16, 3));
  const juce::String first = juce::JSON::toString(AudioMeasure::toVar(AudioMeasure::measureLevels(audio, kFs))) +
                             juce::JSON::toString(AudioMeasure::toVar(AudioMeasure::measureSpectrum(audio, kFs)));
  const juce::String second = juce::JSON::toString(AudioMeasure::toVar(AudioMeasure::measureLevels(audio, kFs))) +
                              juce::JSON::toString(AudioMeasure::toVar(AudioMeasure::measureSpectrum(audio, kFs)));
  EXPECT_EQ(first, second);
  // Rounded for people and for comparisons: no binary dust on the end.
  EXPECT_EQ(AudioMeasure::tidyDb(9.1234567), 9.12);
  EXPECT_EQ(juce::JSON::toString(juce::var(AudioMeasure::tidyDb(-17.130000001)), true, 9), "-17.13");
}

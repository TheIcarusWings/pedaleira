// eq.graphic10 (plugin/src/effects/eq/GraphicEq10.cpp): the conformance
// test, then what a graphic EQ is for: flat is unity, each slider acts at its
// own centre frequency by the amount it is set to, the level slider is a
// plain gain, and moving a slider mid-note does not click.
#include "effects/effect_test_helpers.h"

#include <gtest/gtest.h>

using namespace fxtest;

namespace {

constexpr const char* kKind = "eq.graphic10";
constexpr double kRate = 48000.0;
constexpr int kNumBands = 10;
constexpr double kCentres[kNumBands] = {31.5,   63.0,   125.0,  250.0,  500.0,
                                        1000.0, 2000.0, 4000.0, 8000.0, 16000.0};
const char* const kBandIds[kNumBands] = {"band31",  "band63", "band125", "band250", "band500",
                                         "band1k",  "band2k", "band4k",  "band8k",  "band16k"};

// The block's own magnitude response at `freqs`, from its impulse response
// (the block alone: the rig's DC blocker would bend the lowest band).
std::vector<double> responseDb(const Params& params, const std::vector<double>& freqs) {
  DirectKind kind(kKind, params);
  return magnitudeResponseDb(kind.render(impulse(24000)), freqs, kRate);
}

// A grid of frequencies from 20 Hz to 20 kHz, `perOctave` points per octave.
std::vector<double> logGrid(int perOctave) {
  std::vector<double> f;
  for (double hz = 20.0; hz <= 20000.0; hz *= std::pow(2.0, 1.0 / perOctave))
    f.push_back(hz);
  return f;
}

}  // namespace

TEST(GraphicEq10EffectTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"band63", 9.0}, {"band500", -8.0}, {"band4k", 10.0}, {"levelDb", 2.0}};
  options.stateParams = {{"band31", 3.0},   {"band125", -6.0}, {"band1k", 7.5}, {"band2k", -4.0},
                         {"band8k", 12.0},  {"band16k", -12.0}, {"levelDb", -3.0}};
  // Every slider up by 12 dB and the level too: the cumulative gain on noise.
  options.sweepBound = 60.0f;
  options.cpuAverageBudgetNs = 6000.0;
  options.cpuWorstBudgetNs = 150000.0;
  expectKindConforms("eq.graphic10", options);
}

TEST(GraphicEq10EffectTest, FlatIsUnity) {
  // Alone: the input, to float rounding (the sections are exact identities
  // on paper, and rounding is all that is left of them).
  {
    DirectKind kind(kKind);
    const std::vector<float> in = musicalSignal(1.0, kRate);
    const std::vector<float> out = kind.render(in);
    float worst = 0.0f;
    for (size_t i = 0; i < in.size(); ++i)
      worst = std::max(worst, std::abs(out[i] - in[i]));
    EXPECT_LT(worst, 1.0e-6f);
  }
  // The response across the audio band is flat to within 0.1 dB, measured
  // against the empty rig so the rig's own filters cancel.
  EffectRig rig(kKind, {{"mix", 1.0}});
  EffectRig reference{juce::String()};
  const std::vector<double> freqs = logGrid(3);
  const std::vector<double> got = magnitudeResponseDb(captureImpulseResponse(rig, 1.0), freqs, kRate);
  const std::vector<double> want = magnitudeResponseDb(captureImpulseResponse(reference, 1.0), freqs, kRate);
  for (size_t i = 0; i < freqs.size(); ++i)
    EXPECT_NEAR(got[i], want[i], 0.1) << freqs[i] << " Hz";
}

TEST(GraphicEq10EffectTest, EachBandActsAtItsCentreByTheAmountSet) {
  for (int b = 0; b < kNumBands; ++b) {
    for (const double gain : {12.0, 6.0, -6.0, -12.0}) {
      SCOPED_TRACE(testing::Message() << kBandIds[b] << " " << gain << " dB");
      const std::vector<double> at = responseDb({{kBandIds[b], gain}}, {kCentres[b]});
      EXPECT_NEAR(at[0], gain, 0.5);
    }
  }
}

TEST(GraphicEq10EffectTest, EachBandPeaksAtItsCentreAndLeavesTheOthersAlone) {
  const std::vector<double> grid = logGrid(8);
  for (int b = 0; b < kNumBands; ++b) {
    for (const double gain : {12.0, -12.0}) {
      SCOPED_TRACE(testing::Message() << kBandIds[b] << " " << gain << " dB");
      // The grid, then the centre itself as the last point.
      std::vector<double> freqs = grid;
      freqs.push_back(kCentres[b]);
      const std::vector<double> response = responseDb({{kBandIds[b], gain}}, freqs);
      const double atCentre = response.back();
      EXPECT_NEAR(atCentre, gain, 0.5);
      // No frequency on the grid is moved further than the centre is.
      for (size_t i = 0; i + 1 < freqs.size(); ++i)
        EXPECT_LE(std::abs(response[i]), std::abs(atCentre) + 1.0e-6) << freqs[i] << " Hz";

      // Three octaves away the response is back within 1.5 dB of flat.
      for (const double octaves : {-3.0, 3.0}) {
        const double f = kCentres[b] * std::pow(2.0, octaves);
        if (f < 25.0 || f > 18000.0)
          continue;
        EXPECT_LT(std::abs(responseDb({{kBandIds[b], gain}}, {f})[0]), 1.5) << f << " Hz";
      }
    }
  }
}

TEST(GraphicEq10EffectTest, SeveralBandsCombine) {
  // Far-apart bands do not interfere much: each is within 1 dB of its slider.
  const Params params = {{"band125", 9.0}, {"band1k", -9.0}, {"band8k", 6.0}};
  const std::vector<double> at = responseDb(params, {125.0, 1000.0, 8000.0});
  EXPECT_NEAR(at[0], 9.0, 1.0);
  EXPECT_NEAR(at[1], -9.0, 1.0);
  EXPECT_NEAR(at[2], 6.0, 1.0);
}

TEST(GraphicEq10EffectTest, LevelIsAPlainGain) {
  for (const double level : {-12.0, -3.0, 6.0, 12.0}) {
    const std::vector<double> response = responseDb({{"levelDb", level}}, {40.0, 440.0, 3000.0, 12000.0});
    for (const double v : response)
      EXPECT_NEAR(v, level, 0.1) << level;
  }
}

TEST(GraphicEq10EffectTest, MovingASliderMidNoteDoesNotClick) {
  // A 1 kHz tone, the 1 kHz band swung from 0 to +12 dB and back during it.
  // The output tone grows by 12 dB, so its own slope grows with it; the
  // glide adds nothing beyond that, where a jump in the response would
  // splice in a step.
  EffectRig rig(kKind, {{"mix", 1.0}});
  const std::vector<float> in = sine(0.9, 1000.0, 0.2f, kRate);
  std::vector<float> out;
  const auto append = [&out](const std::vector<float>& more) { out.insert(out.end(), more.begin(), more.end()); };
  const auto slice = [&](double from, double to) {
    return std::vector<float>(in.begin() + static_cast<std::ptrdiff_t>(from * kRate),
                              in.begin() + static_cast<std::ptrdiff_t>(to * kRate));
  };
  append(rig.render(slice(0.0, 0.3)));
  rig.set("band1k", 12.0);
  append(rig.render(slice(0.3, 0.6)));
  rig.set("band1k", -12.0);
  append(rig.render(slice(0.6, 0.9)));
  expectCleanSignal(out, 1.2f, "slider move");
  // Natural step of the loudest tone (0.2 * 4 = 0.8 at 1 kHz) is 0.105.
  EXPECT_LT(maxStep(out, 0, out.size()), 0.105f * 1.15f);
}

TEST(GraphicEq10EffectTest, TailIsHonestAboutWhatRings) {
  EffectRig rig(kKind);
  EXPECT_EQ(rig.tailSeconds(), 0.0) << "a flat EQ rings for nothing";
  rig.set("band31", 12.0);
  const double low = rig.tailSeconds();
  EXPECT_GT(low, 0.1);
  rig.set("band31", 0.0);
  rig.set("band16k", 12.0);
  EXPECT_LT(rig.tailSeconds(), low) << "a high band rings for less time than a low one";
  EXPECT_EQ(rig.latencySamples(), 0);
}

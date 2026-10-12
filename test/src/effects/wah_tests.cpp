// filter.wah (plugin/src/effects/filter/WahEffect.cpp): the conformance test,
// then what a wah is for: the centre frequency of its peak follows the pedal
// position monotonically between the ends of the voicing, resonance narrows
// the peak, level is a plain gain, and a pedal that jumps does not click.
#include "effects/effect_test_helpers.h"

#include <gtest/gtest.h>

using namespace fxtest;

namespace {

constexpr const char* kKind = "filter.wah";
constexpr double kRate = 48000.0;

struct Voicing {
  const char* name;
  double heelHz;
  double toeHz;
};
const Voicing kVoicings[] = {
    {"Classic", 350.0, 2200.0}, {"Wide", 250.0, 3200.0}, {"Low", 200.0, 1400.0}, {"Bright", 500.0, 3000.0}};

// The wah alone, an impulse in, its magnitude response out.
std::vector<float> impulseResponse(const Params& params) {
  DirectKind kind(kKind, params);
  return kind.render(impulse(24000));
}

std::vector<double> frequencyGrid() {
  std::vector<double> f;
  for (double hz = 80.0; hz <= 6000.0; hz *= std::pow(2.0, 1.0 / 48.0))
    f.push_back(hz);
  return f;
}

struct Peak {
  double hz;
  double gainDb;
};

Peak peakOfResponse(const Params& params) {
  const std::vector<double> grid = frequencyGrid();
  const std::vector<double> response = magnitudeResponseDb(impulseResponse(params), grid, kRate);
  size_t best = 0;
  for (size_t i = 0; i < grid.size(); ++i)
    if (response[i] > response[best])
      best = i;
  return {grid[best], response[best]};
}

// Width in octaves of the part of the response within 3 dB of the peak.
double bandwidthOctaves(const Params& params) {
  const std::vector<double> grid = frequencyGrid();
  const std::vector<double> response = magnitudeResponseDb(impulseResponse(params), grid, kRate);
  const double top = *std::max_element(response.begin(), response.end());
  double lo = grid.back(), hi = grid.front();
  for (size_t i = 0; i < grid.size(); ++i)
    if (response[i] >= top - 3.0) {
      lo = std::min(lo, grid[i]);
      hi = std::max(hi, grid[i]);
    }
  return std::log2(hi / lo);
}

}  // namespace

TEST(WahEffectTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"positionPct", 70.0}, {"resonancePct", 60.0}};
  options.stateParams = {{"positionPct", 20.0}, {"resonancePct", 80.0}, {"voicing", 2.0}, {"levelDb", 3.0}};
  options.cpuAverageBudgetNs = 2500.0;
  options.cpuWorstBudgetNs = 150000.0;
  expectKindConforms("filter.wah", options);
}

TEST(WahEffectTest, CentreFrequencyMovesMonotonicallyBetweenTheEndsOfEachVoicing) {
  for (size_t v = 0; v < sizeof(kVoicings) / sizeof(kVoicings[0]); ++v) {
    SCOPED_TRACE(kVoicings[v].name);
    double previous = 0.0;
    for (const double position : {0.0, 20.0, 40.0, 60.0, 80.0, 100.0}) {
      const Peak peak = peakOfResponse({{"voicing", static_cast<double>(v)}, {"positionPct", position}});
      EXPECT_GT(peak.hz, previous) << "position " << position;
      previous = peak.hz;

      // An exponential sweep between the two ends.
      const double expected =
          kVoicings[v].heelHz * std::pow(kVoicings[v].toeHz / kVoicings[v].heelHz, position / 100.0);
      EXPECT_NEAR(std::log2(peak.hz / expected), 0.0, 0.06) << "position " << position;
    }
  }
}

TEST(WahEffectTest, TheClassicVoicingSweepsAbout350HzTo2200Hz) {
  EXPECT_NEAR(peakOfResponse({{"positionPct", 0.0}}).hz, 350.0, 25.0);
  EXPECT_NEAR(peakOfResponse({{"positionPct", 100.0}}).hz, 2200.0, 150.0);
}

TEST(WahEffectTest, ResonanceNarrowsThePeak) {
  const double loose = bandwidthOctaves({{"positionPct", 50.0}, {"resonancePct", 10.0}});
  const double mid = bandwidthOctaves({{"positionPct", 50.0}, {"resonancePct", 50.0}});
  const double tight = bandwidthOctaves({{"positionPct", 50.0}, {"resonancePct", 95.0}});
  EXPECT_GT(loose, mid);
  EXPECT_GT(mid, tight);
  EXPECT_LT(tight, 0.5 * loose);
  // The peak stays where the pedal puts it.
  EXPECT_NEAR(std::log2(peakOfResponse({{"positionPct", 50.0}, {"resonancePct", 95.0}}).hz /
                        peakOfResponse({{"positionPct", 50.0}, {"resonancePct", 10.0}}).hz),
              0.0, 0.08);
}

TEST(WahEffectTest, PeakSitsAtAboutPlusSixDecibelsWhateverTheResonance) {
  for (const double resonance : {0.0, 50.0, 100.0})
    EXPECT_NEAR(peakOfResponse({{"positionPct", 50.0}, {"resonancePct", resonance}}).gainDb, 6.0, 1.0)
        << resonance;
}

TEST(WahEffectTest, LevelIsAPlainGain) {
  const double base = peakOfResponse({{"positionPct", 50.0}}).gainDb;
  EXPECT_NEAR(peakOfResponse({{"positionPct", 50.0}, {"levelDb", 6.0}}).gainDb, base + 6.0, 0.1);
  EXPECT_NEAR(peakOfResponse({{"positionPct", 50.0}, {"levelDb", -9.0}}).gainDb, base - 9.0, 0.1);
}

TEST(WahEffectTest, TheLowEndIsNeverCutCompletely) {
  // A third of the low-pass output rides under the band-pass, as in the
  // pedal: a 100 Hz note is about 10 dB down at any position, not gone.
  for (const double position : {0.0, 50.0, 100.0}) {
    const std::vector<double> db =
        magnitudeResponseDb(impulseResponse({{"positionPct", position}}), {100.0}, kRate);
    EXPECT_GT(db[0], -12.0) << position;
    EXPECT_LT(db[0], -7.0) << position;
  }
}

TEST(WahEffectTest, ThePedalGlidesInsteadOfJumping) {
  // 2200 Hz, the toe's centre, is 29 dB down with the pedal at the heel. Put
  // the pedal to the toe at 0.3 s: the tone swells to its 6 dB gain along
  // the position glide, neither at once nor sluggishly.
  EffectRig rig(kKind, {{"positionPct", 0.0}, {"mix", 1.0}});
  rig.render(sine(0.3, 2200.0, 0.1f, kRate));
  rig.set("positionPct", 100.0);
  const std::vector<float> after = rig.render(sine(0.2, 2200.0, 0.1f, kRate));
  const float settled = peakOf(after, after.size() - 480);
  EXPECT_GT(settled, 0.15f);
  int halfway = -1;
  for (size_t i = 0; i + 22 < after.size(); ++i)
    if (peakOf(after, i, i + 22) >= 0.5f * settled) {
      halfway = static_cast<int>(i);
      break;
    }
  ASSERT_GE(halfway, 0);
  const double halfwayMs = 1000.0 * halfway / kRate;
  EXPECT_GT(halfwayMs, 2.0) << "the pedal jumped";
  EXPECT_LT(halfwayMs, 30.0) << "the pedal glide is too slow to follow a foot";
}

TEST(WahEffectTest, AFastPedalMoveDoesNotClick) {
  // A sustained 800 Hz tone with the pedal slammed between heel and toe
  // every 25 ms. On the way past, the sweep drives the peak (+6 dB) through
  // the tone, which neither end does: the tone's own slope at that level is
  // the yardstick for any step in the output.
  const std::vector<float> in = sine(2.0, 800.0, 0.25f, kRate);
  const float peakStep = 0.25f * 2.0f * static_cast<float>(2.0 * kPi * 800.0 / kRate);

  EffectRig rig(kKind, {{"positionPct", 0.0}, {"mix", 1.0}});
  std::vector<float> out;
  const size_t chunk = static_cast<size_t>(0.025 * kRate);
  bool toe = false;
  for (size_t at = 0; at + chunk <= in.size(); at += chunk) {
    rig.set("positionPct", toe ? 100.0 : 0.0);
    toe = !toe;
    const std::vector<float> part =
        rig.render(std::vector<float>(in.begin() + static_cast<std::ptrdiff_t>(at),
                                      in.begin() + static_cast<std::ptrdiff_t>(at + chunk)));
    out.insert(out.end(), part.begin(), part.end());
  }
  expectCleanSignal(out, 1.0f, "pedal slam");
  EXPECT_LT(maxStep(out, static_cast<size_t>(0.5 * kRate), out.size()), 1.1f * peakStep);
}

TEST(WahEffectTest, TailFollowsTheResonanceAndTheHeel) {
  EffectRig rig(kKind, {{"resonancePct", 100.0}});
  const double sharp = rig.tailSeconds();
  rig.set("resonancePct", 0.0);
  EXPECT_LT(rig.tailSeconds(), sharp);
  rig.set("resonancePct", 100.0);
  rig.set("voicing", 3.0);  // Bright: heel at 500 Hz, rings less
  EXPECT_LT(rig.tailSeconds(), sharp);
  EXPECT_EQ(rig.latencySamples(), 0);
}

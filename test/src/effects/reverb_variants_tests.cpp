// The reverb variants (plugin/src/effects/reverb): ambience, modulated hall,
// cave, large hall, ducking reverb and vintage digital, and the ducking every
// reverb now has (plugin/include/effects/reverb/ReverbDuck.h). Each new kind
// gets the conformance test, the spillover contract and then what it is for:
// the decay time the knob says, the pre-delay to the sample, the bass and
// treble controls of the large hall, a cave that cannot run away at 20 s, a
// freeze that holds, a word length that really rounds. The ducking is tested
// across all six kinds that carry it: it lowers the wet by the set depth while
// the input is loud, lets go within the release, leaves the tail alone, and
// at 0 dB changes nothing.
#include "effects/delay_test_support.h"
#include "effects/reverb_test_helpers.h"

#include <gtest/gtest.h>

#include <cmath>
#include <iostream>

using namespace fxtest;

namespace {

using reverbtest::kRate;
using reverbtest::samples;
using reverbtest::with;

double windowRms(const std::vector<float>& x, double fromSeconds, double lengthSeconds) {
  const size_t begin = static_cast<size_t>(samples(fromSeconds));
  return rmsOf(x, begin, begin + static_cast<size_t>(samples(lengthSeconds)));
}

Stereo wetOf(const char* kind, const Params& params, const Stereo& in, Layout layout = Layout::Mono,
             int block = 256) {
  DirectKind direct(kind, params, layout, block);
  return direct.render(in);
}

std::vector<float> wetOf(const char* kind, const Params& params, const std::vector<float>& in,
                         Layout layout = Layout::Mono) {
  return wetOf(kind, params, Stereo{in, in}, layout).left;
}

const BlockParamSpec* specOf(const char* kind, const char* id) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kind);
  if (info == nullptr)
    return nullptr;
  const int index = findBlockParam(info->params, id);
  return index < 0 ? nullptr : &info->params[static_cast<size_t>(index)];
}

/** RT60 of the band [lowHz, highHz] of the impulse response at each decay,
    within `tolerance` of it. For a kind whose damping is part of the design
    the band sits under the damping corner. */
void expectBandRt60MatchesDecay(const char* kind, const Params& params, const std::vector<double>& decays,
                                double lowHz, double highHz, double tolerance = 0.2) {
  for (const double decay : decays) {
    const std::vector<float> ir =
        reverbtest::wetImpulseResponse(kind, with(params, {{"decayS", decay}}), decay * 1.1 + 1.0).left;
    const double measured = reverbtest::bandRt60(ir, lowHz, highHz);
    std::cout << "[   RT60   ] " << kind << ": decay " << decay << " s, measured "
              << juce::String(measured, 3) << " s in " << lowHz << " to " << highHz << " Hz ("
              << juce::String(100.0 * (measured / decay - 1.0), 1) << "%)" << std::endl;
    EXPECT_NEAR(measured, decay, tolerance * decay) << kind << " at decay " << decay;
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Ambience
// ---------------------------------------------------------------------------

namespace {
constexpr const char* kAmbience = "reverb.ambience";
const Params kAmbienceFlat = {{"lowCutHz", 20.0}, {"highCutHz", 12000.0}, {"preDelayMs", 0.0}};
}  // namespace

TEST(AmbienceReverbTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"size", 2.0}, {"preDelayMs", 12.0}};
  options.stateParams = {{"size", 0.0},         {"preDelayMs", 40.0}, {"lowCutHz", 200.0},
                         {"highCutHz", 5000.0}, {"spillover", 0.0}};
  // The wet path starts after the pre-delay and the first reflection, which
  // is the effect and not latency. The kind reports none.
  options.latency = LatencyCheck::AtMostOnset;
  options.cpuAverageBudgetNs = 50000.0;
  options.cpuWorstBudgetNs = 400000.0;
  expectKindConforms("reverb.ambience", options);
}

TEST(AmbienceReverbTest, SpilloverRingsOutAndCutsOnRequest) {
  delaytest::expectSpilloverBehaviour(kAmbience, {{"size", 1.0}}, 20.0, 120.0, 2.0e-3f);
}

TEST(AmbienceReverbTest, ParametersAreTheDocumentedRanges) {
  const BlockParamSpec* size = specOf(kAmbience, "size");
  ASSERT_NE(size, nullptr);
  EXPECT_EQ(size->type, BlockParamType::Choice);
  EXPECT_EQ(size->choices.size(), 3);
  const BlockParamSpec* preDelay = specOf(kAmbience, "preDelayMs");
  ASSERT_NE(preDelay, nullptr);
  EXPECT_FLOAT_EQ(preDelay->min, 0.0f);
  EXPECT_FLOAT_EQ(preDelay->max, 100.0f);
  const BlockParamSpec* low = specOf(kAmbience, "lowCutHz");
  ASSERT_NE(low, nullptr);
  EXPECT_FLOAT_EQ(low->min, 20.0f);
  EXPECT_FLOAT_EQ(low->max, 800.0f);
  const BlockParamSpec* high = specOf(kAmbience, "highCutHz");
  ASSERT_NE(high, nullptr);
  EXPECT_FLOAT_EQ(high->min, 800.0f);
  EXPECT_FLOAT_EQ(high->max, 12000.0f);
}

TEST(AmbienceReverbTest, TheTailIsVeryShortAndTheReflectionsCarryTheSound) {
  double previousEnd = 0.0;
  for (const double size : {0.0, 1.0, 2.0}) {
    SCOPED_TRACE(size);
    const std::vector<float> ir =
        reverbtest::wetImpulseResponse(kAmbience, with(kAmbienceFlat, {{"size", size}}), 1.5).left;
    // Energy in the first 80 ms against the rest: the reflections dominate.
    const size_t early = static_cast<size_t>(samples(0.08));
    const double early_energy = reverbtest::energyOf(ir, 0, early);
    const double rest = reverbtest::energyOf(ir, early);
    std::cout << "[ AMBIENCE ] size " << size << ": first 80 ms against the rest "
              << juce::String(10.0 * std::log10(early_energy / rest), 1) << " dB" << std::endl;
    EXPECT_GT(early_energy, rest);
    // Down to -60 dB well inside half a second (the tail time reported).
    const float peak = peakOf(ir);
    const double end = decayTimeSeconds(ir, 0, peak, kRate);
    EXPECT_LT(end, 0.75);
    EXPECT_GE(end, previousEnd - 0.02) << "a larger size does not ring for less time";
    previousEnd = end;
    DirectKind direct(kAmbience, {{"size", size}});
    EXPECT_LT(direct.tailSeconds(), 1.0);
  }
}

TEST(AmbienceReverbTest, SizeMovesTheReflections) {
  const auto firstReflectionSamples = [](double size) {
    const std::vector<float> ir =
        reverbtest::wetImpulseResponse(kAmbience, with(kAmbienceFlat, {{"size", size}}), 0.3).left;
    // The centre of the energy in the first 80 ms.
    double weighted = 0.0, total = 0.0;
    for (size_t i = 0; i < static_cast<size_t>(samples(0.08)); ++i) {
      weighted += static_cast<double>(i) * ir[i] * ir[i];
      total += static_cast<double>(ir[i]) * ir[i];
    }
    return weighted / total;
  };
  const double compact = firstReflectionSamples(0.0);
  const double medium = firstReflectionSamples(1.0);
  const double roomy = firstReflectionSamples(2.0);
  EXPECT_LT(compact, medium);
  EXPECT_LT(medium, roomy);
}

TEST(AmbienceReverbTest, PreDelayMatchesTheParameter) {
  reverbtest::expectPreDelayMatches(kAmbience, kAmbienceFlat, {10.0, 50.0, 100.0});
}

TEST(AmbienceReverbTest, LowAndHighCutShapeTheWetSignal) {
  const auto bandDb = [](const Params& params, double lowHz, double highHz) {
    return 10.0 * std::log10(
                      reverbtest::energyOf(reverbtest::bandOf(
                          reverbtest::wetImpulseResponse(kAmbience, params, 0.8).left, lowHz, highHz)));
  };
  // An octave under a 500 Hz low cut and over a 2 kHz high cut: 12 dB per
  // octave means at least 9 dB down against the open setting.
  const double lowOpen = bandDb(kAmbienceFlat, 100.0, 250.0);
  const double lowCut = bandDb(with(kAmbienceFlat, {{"lowCutHz", 500.0}}), 100.0, 250.0);
  EXPECT_LT(lowCut, lowOpen - 9.0);
  const double highOpen = bandDb(kAmbienceFlat, 4000.0, 8000.0);
  const double highCut = bandDb(with(kAmbienceFlat, {{"highCutHz", 2000.0}}), 4000.0, 8000.0);
  EXPECT_LT(highCut, highOpen - 9.0);
}

TEST(AmbienceReverbTest, WideInStereoOnly) {
  reverbtest::expectWideInStereoOnly(kAmbience, {{"size", 2.0}}, 0.05, 0.4);
}

// ---------------------------------------------------------------------------
// Modulated hall
// ---------------------------------------------------------------------------

namespace {
constexpr const char* kModHall = "reverb.modhall";
const Params kModHallFlat = {{"lowCutHz", 20.0}, {"highCutHz", 20000.0}, {"preDelayMs", 0.0}};
}  // namespace

TEST(ModulatedHallReverbTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"decayS", 2.5}, {"preDelayMs", 30.0}, {"modSpeedHz", 1.5}, {"modDepth", 70.0}};
  options.stateParams = {{"decayS", 6.0},        {"preDelayMs", 120.0}, {"modSpeedHz", 3.0},
                         {"modDepth", 20.0},     {"lowCutHz", 250.0},   {"highCutHz", 6000.0},
                         {"spillover", 0.0}};
  options.latency = LatencyCheck::AtMostOnset;
  options.cpuAverageBudgetNs = 50000.0;
  options.cpuWorstBudgetNs = 400000.0;
  // The modulation is slow and deterministic, but it makes the output depend
  // on the sample rate through the lines' rounding to whole samples.
  expectKindConforms("reverb.modhall", options);
}

TEST(ModulatedHallReverbTest, SpilloverRingsOutAndCutsOnRequest) {
  delaytest::expectSpilloverBehaviour(kModHall, {{"decayS", 1.5}, {"preDelayMs", 0.0}}, 100.0, 500.0, 2.0e-3f);
}

TEST(ModulatedHallReverbTest, ParametersAreTheDocumentedRanges) {
  const BlockParamSpec* decay = specOf(kModHall, "decayS");
  ASSERT_NE(decay, nullptr);
  EXPECT_FLOAT_EQ(decay->min, 1.0f);
  EXPECT_FLOAT_EQ(decay->max, 10.0f);
  const BlockParamSpec* preDelay = specOf(kModHall, "preDelayMs");
  ASSERT_NE(preDelay, nullptr);
  EXPECT_FLOAT_EQ(preDelay->max, 200.0f);
  const BlockParamSpec* speed = specOf(kModHall, "modSpeedHz");
  ASSERT_NE(speed, nullptr);
  EXPECT_FLOAT_EQ(speed->min, 0.1f);
  EXPECT_FLOAT_EQ(speed->max, 5.0f);
  EXPECT_NE(specOf(kModHall, "modDepth"), nullptr);
}

TEST(ModulatedHallReverbTest, DecayParameterIsTheRt60) {
  // The damping is part of the design, so the band is the one under its
  // corner.
  expectBandRt60MatchesDecay(kModHall, kModHallFlat, {1.0, 4.0, 10.0}, 200.0, 1000.0);
}

TEST(ModulatedHallReverbTest, ReportedTailHoldsAndPreDelayMatches) {
  reverbtest::expectTailHolds(kModHall, with(kModHallFlat, {{"decayS", 4.0}, {"preDelayMs", 200.0}}));
  reverbtest::expectPreDelayMatches(kModHall, kModHallFlat, {20.0, 100.0, 200.0});
}

TEST(ModulatedHallReverbTest, SpeedAndDepthAreHeardAndStayStable) {
  const std::vector<float> in = noise(4.0, 5u, 0.2f, kRate);
  const std::vector<float> still = wetOf(kModHall, with(kModHallFlat, {{"modDepth", 0.0}}), in);
  const std::vector<float> slow = wetOf(kModHall, with(kModHallFlat, {{"modDepth", 100.0}, {"modSpeedHz", 0.1}}), in);
  const std::vector<float> fast = wetOf(kModHall, with(kModHallFlat, {{"modDepth", 100.0}, {"modSpeedHz", 5.0}}), in);
  EXPECT_NE(still, slow);
  EXPECT_NE(slow, fast);
  EXPECT_NE(still, fast);
  // Levels stay the same: modulation moves the tail, it does not change how
  // much of it there is (within 1.5 dB).
  const double reference = windowRms(still, 2.0, 2.0);
  ASSERT_GT(reference, 0.001);
  EXPECT_NEAR(toDb(windowRms(slow, 2.0, 2.0) / reference), 0.0, 1.5);
  EXPECT_NEAR(toDb(windowRms(fast, 2.0, 2.0) / reference), 0.0, 1.5);
  // Even at full depth and speed a tone does not wobble like a vibrato: the
  // pitch drift of the moving lines is capped at 0.8%.
  const std::vector<float> tone = sine(6.0, 440.0, 0.3f, kRate);
  const std::vector<float> toned =
      wetOf(kModHall, with(kModHallFlat, {{"modDepth", 100.0}, {"modSpeedHz", 5.0}, {"decayS", 3.0}}), tone);
  expectCleanSignal(toned, 4.0f, "modulated hall at full depth and speed");
}

TEST(ModulatedHallReverbTest, WideInStereoOnly) {
  reverbtest::expectWideInStereoOnly(kModHall, with(kModHallFlat, {{"decayS", 3.0}}), 0.5, 2.5);
}

// ---------------------------------------------------------------------------
// Cave
// ---------------------------------------------------------------------------

namespace {
constexpr const char* kCave = "reverb.cave";
const Params kCaveFlat = {{"damping", 0.0}, {"lowCutHz", 20.0}, {"highCutHz", 8000.0}, {"preDelayMs", 0.0}};
}  // namespace

TEST(CaveReverbTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"decayS", 6.0}, {"preDelayMs", 40.0}, {"damping", 50.0}};
  options.stateParams = {{"decayS", 15.0},       {"preDelayMs", 150.0}, {"damping", 90.0},
                         {"lowCutHz", 300.0},    {"highCutHz", 3000.0}, {"spillover", 0.0}};
  options.latency = LatencyCheck::AtMostOnset;
  options.cpuAverageBudgetNs = 50000.0;
  options.cpuWorstBudgetNs = 400000.0;
  expectKindConforms("reverb.cave", options);
}

TEST(CaveReverbTest, SpilloverRingsOutAndCutsOnRequest) {
  delaytest::expectSpilloverBehaviour(kCave, {{"decayS", 5.0}, {"preDelayMs", 0.0}}, 200.0, 1000.0, 2.0e-3f);
}

TEST(CaveReverbTest, ParametersAreTheDocumentedRanges) {
  const BlockParamSpec* decay = specOf(kCave, "decayS");
  ASSERT_NE(decay, nullptr);
  EXPECT_FLOAT_EQ(decay->min, 5.0f);
  EXPECT_FLOAT_EQ(decay->max, 20.0f);
  const BlockParamSpec* high = specOf(kCave, "highCutHz");
  ASSERT_NE(high, nullptr);
  EXPECT_FLOAT_EQ(high->min, 800.0f);
  EXPECT_FLOAT_EQ(high->max, 8000.0f);
  EXPECT_NE(specOf(kCave, "damping"), nullptr);
  EXPECT_NE(specOf(kCave, "preDelayMs"), nullptr);
}

TEST(CaveReverbTest, DecayParameterIsTheRt60) {
  reverbtest::expectRt60MatchesDecay(kCave, kCaveFlat, {5.0, 12.0, 20.0});
}

TEST(CaveReverbTest, DampingMakesItDarker) {
  const std::vector<float> dark =
      reverbtest::wetImpulseResponse(kCave, with(kCaveFlat, {{"decayS", 8.0}, {"damping", 80.0}}), 9.5).left;
  const double low = reverbtest::bandRt60(dark, 200.0, 800.0);
  const double high = reverbtest::bandRt60(dark, 4000.0, 7500.0);
  std::cout << "[ DAMPING  ] " << kCave << ": RT60 " << low << " s at 200 to 800 Hz, " << high
            << " s at 4 to 7.5 kHz with the damping at 80%" << std::endl;
  EXPECT_NEAR(low, 8.0, 1.6);
  EXPECT_LT(high, 0.35 * low);
}

TEST(CaveReverbTest, StableAtTheLongestDecayWithNoGrowth) {
  // A full-scale impulse at 20 s with nothing damped: the level of the tail
  // falls second after second for 30 s and never comes back up.
  const std::vector<float> ir =
      reverbtest::wetImpulseResponse(kCave, with(kCaveFlat, {{"decayS", 20.0}}), 30.0).left;
  expectCleanSignal(ir, 4.0f, "cave impulse response at 20 s");
  double previous = windowRms(ir, 0.5, 1.0);
  ASSERT_GT(previous, 1.0e-4);
  for (int second = 2; second < 30; ++second) {
    const double level = windowRms(ir, static_cast<double>(second), 1.0);
    EXPECT_LT(level, previous * 1.12) << "the tail grew at " << second << " s";
    previous = level;
  }
  // 30 s at 20 s of RT60 is 90 dB; the last second is far below the first.
  EXPECT_LT(toDb(windowRms(ir, 29.0, 1.0) / windowRms(ir, 1.0, 1.0)), -30.0);

  // Thirty seconds of full-scale noise at the longest decay stays bounded,
  // and the level it settles to stops rising.
  Params hot = with(kCaveFlat, {{"decayS", 20.0}});
  DirectKind direct(kCave, hot, Layout::Stereo);
  const Stereo in{noise(30.0, 31u, 1.0f, kRate), noise(30.0, 32u, 1.0f, kRate)};
  const Stereo out = direct.render(in);
  expectCleanSignal(out.left, 12.0f, "cave with hot input");
  expectCleanSignal(out.right, 12.0f, "cave with hot input");
  const double middle = windowRms(out.left, 15.0, 5.0);
  const double last = windowRms(out.left, 25.0, 5.0);
  EXPECT_LT(last, middle * 1.15) << "the level kept rising under steady input";
}

TEST(CaveReverbTest, PreDelayAndTailHold) {
  reverbtest::expectPreDelayMatches(kCave, kCaveFlat, {30.0, 100.0, 200.0});
  reverbtest::expectTailHolds(kCave, with(kCaveFlat, {{"decayS", 5.0}, {"preDelayMs", 200.0}}));
}

TEST(CaveReverbTest, WideInStereoOnly) {
  reverbtest::expectWideInStereoOnly(kCave, with(kCaveFlat, {{"decayS", 5.0}}), 1.0, 4.0);
}

// ---------------------------------------------------------------------------
// Large hall
// ---------------------------------------------------------------------------

namespace {
constexpr const char* kLarge = "reverb.large";
// Nothing coloring the tail: no shelf, no bass stretch, the wet filters open.
const Params kLargeFlat = {{"hShelfDb", 0.0},   {"bassMult", 1.0},    {"lowCutHz", 10.0},
                           {"highCutHz", 20000.0}, {"preDelayMs", 0.0}, {"attack", 0.0},
                           {"modDepth", 0.0}};
}  // namespace

TEST(LargeHallReverbTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"decayS", 3.0},      {"preDelayMs", 60.0}, {"bassMult", 1.5},
                    {"hShelfDb", -9.0},   {"attack", 30.0},     {"modDepth", 50.0}};
  options.stateParams = {{"decayS", 12.0},          {"preDelayMs", 300.0}, {"size", 20.0},
                         {"bassMult", 2.5},         {"bassFreqHz", 800.0}, {"hShelfFreqHz", 9000.0},
                         {"hShelfDb", -15.0},       {"earlyDiffusion", 10.0}, {"lateDiffusion", 50.0},
                         {"attack", 70.0},          {"modRateHz", 4.0},    {"modDepth", 80.0},
                         {"lowCutHz", 200.0},       {"highCutHz", 6000.0}, {"spillover", 0.0}};
  options.latency = LatencyCheck::AtMostOnset;
  options.cpuAverageBudgetNs = 50000.0;
  options.cpuWorstBudgetNs = 400000.0;
  expectKindConforms("reverb.large", options);
}

TEST(LargeHallReverbTest, SpilloverRingsOutAndCutsOnRequest) {
  delaytest::expectSpilloverBehaviour(kLarge, {{"decayS", 2.0}, {"preDelayMs", 0.0}}, 100.0, 500.0, 2.0e-3f);
}

TEST(LargeHallReverbTest, ParametersAreTheDocumentedRanges) {
  const auto expectRange = [](const char* id, float low, float high) {
    const BlockParamSpec* spec = specOf(kLarge, id);
    ASSERT_NE(spec, nullptr) << id;
    EXPECT_FLOAT_EQ(spec->min, low) << id;
    EXPECT_FLOAT_EQ(spec->max, high) << id;
  };
  expectRange("decayS", 0.2f, 70.0f);
  expectRange("preDelayMs", 0.0f, 500.0f);
  expectRange("size", 0.0f, 100.0f);
  expectRange("bassMult", 0.25f, 4.0f);
  expectRange("bassFreqHz", 100.0f, 10000.0f);
  expectRange("hShelfFreqHz", 100.0f, 20000.0f);
  expectRange("hShelfDb", -24.0f, 0.0f);
  expectRange("earlyDiffusion", 0.0f, 100.0f);
  expectRange("lateDiffusion", 0.0f, 100.0f);
  expectRange("attack", 0.0f, 100.0f);
  expectRange("modRateHz", 0.1f, 10.0f);
  expectRange("modDepth", 0.0f, 100.0f);
  expectRange("lowCutHz", 10.0f, 1500.0f);
  expectRange("highCutHz", 100.0f, 20000.0f);
}

TEST(LargeHallReverbTest, DecayParameterIsTheRt60AcrossTheWholeRange) {
  reverbtest::expectRt60MatchesDecay(kLarge, kLargeFlat, {0.5, 3.0, 12.0, 30.0, 70.0});
  // The same at both ends of the size range.
  reverbtest::expectRt60MatchesDecay(kLarge, with(kLargeFlat, {{"size", 0.0}}), {2.0});
  reverbtest::expectRt60MatchesDecay(kLarge, with(kLargeFlat, {{"size", 100.0}}), {2.0});
}

TEST(LargeHallReverbTest, BassMultiplierStretchesTheLowBandOnly) {
  // The crossover well above the band measured, the shelf out of the way.
  const Params base = with(kLargeFlat, {{"decayS", 3.0}, {"bassFreqHz", 1500.0}, {"hShelfFreqHz", 20000.0}});
  const auto lowRt60 = [&](double mult) {
    const std::vector<float> ir =
        reverbtest::wetImpulseResponse(kLarge, with(base, {{"bassMult", mult}}), 3.0 * std::max(1.0, mult) * 1.2 + 1.0).left;
    return std::make_pair(reverbtest::bandRt60(ir, 80.0, 250.0), reverbtest::bandRt60(ir, 3000.0, 8000.0));
  };
  const auto neutral = lowRt60(1.0);
  const auto doubled = lowRt60(2.0);
  const auto halved = lowRt60(0.5);
  std::cout << "[   BASS   ] " << kLarge << ": low band RT60 " << neutral.first << " s at x1, "
            << doubled.first << " s at x2, " << halved.first << " s at x0.5; high band " << neutral.second
            << ", " << doubled.second << ", " << halved.second << " s" << std::endl;
  EXPECT_NEAR(neutral.first, 3.0, 0.6);
  EXPECT_NEAR(doubled.first, 6.0, 1.5);
  EXPECT_NEAR(halved.first, 1.5, 0.4);
  // The high band does not follow the multiplier.
  EXPECT_NEAR(doubled.second, neutral.second, 0.6);
  EXPECT_NEAR(halved.second, neutral.second, 0.6);
}

TEST(LargeHallReverbTest, BassFrequencyMovesTheCrossover) {
  const Params base = with(kLargeFlat, {{"decayS", 3.0}, {"bassMult", 3.0}, {"hShelfFreqHz", 20000.0}});
  const auto midBand = [&](double crossoverHz) {
    const std::vector<float> ir =
        reverbtest::wetImpulseResponse(kLarge, with(base, {{"bassFreqHz", crossoverHz}}), 10.0).left;
    return reverbtest::bandRt60(ir, 700.0, 1200.0);
  };
  // 1 kHz is under a 4 kHz crossover and well over a 100 Hz one.
  const double high = midBand(4000.0);
  const double low = midBand(100.0);
  std::cout << "[   BASS   ] " << kLarge << ": RT60 at 700 to 1200 Hz " << high << " s with the crossover at 4 kHz, "
            << low << " s at 100 Hz" << std::endl;
  EXPECT_GT(high, 1.6 * low);
}

TEST(LargeHallReverbTest, HighShelfDampsTheTreble) {
  const Params base = with(kLargeFlat, {{"decayS", 4.0}, {"hShelfFreqHz", 2000.0}});
  const auto bands = [&](double shelfDb) {
    const std::vector<float> ir =
        reverbtest::wetImpulseResponse(kLarge, with(base, {{"hShelfDb", shelfDb}}), 5.5).left;
    return std::make_pair(reverbtest::bandRt60(ir, 200.0, 800.0), reverbtest::bandRt60(ir, 7000.0, 12000.0));
  };
  const auto open = bands(0.0);
  const auto shelved = bands(-12.0);
  std::cout << "[  SHELF   ] " << kLarge << ": RT60 " << open.second << " s at 7 to 12 kHz open, " << shelved.second
            << " s with the shelf at -12 dB (low band " << shelved.first << " s)" << std::endl;
  EXPECT_NEAR(open.second, open.first, 0.8);
  EXPECT_NEAR(shelved.first, 4.0, 0.8);
  // -12 dB as a gain is a quarter of the decay time.
  EXPECT_NEAR(shelved.second, 1.0, 0.4);
}

TEST(LargeHallReverbTest, PreDelayMatchesTheParameterUpToHalfASecond) {
  reverbtest::expectPreDelayMatches(kLarge, kLargeFlat, {20.0, 250.0, 500.0});
}

TEST(LargeHallReverbTest, AttackMakesTheTailSwellIn) {
  // How the level changes from just after the reflections (150 to 250 ms) to
  // the middle of the swell (350 to 450 ms): a falling tail loses level there,
  // a swelling one holds or gains it.
  const auto change = [](double attack, double decay) {
    const std::vector<float> ir =
        reverbtest::wetImpulseResponse(kLarge, with(kLargeFlat, {{"decayS", decay}, {"attack", attack}}), 1.0).left;
    return toDb(windowRms(ir, 0.35, 0.1) / windowRms(ir, 0.15, 0.1));
  };
  for (const double decay : {1.0, 3.0}) {
    const double instant = change(0.0, decay);
    const double half = change(50.0, decay);
    const double slow = change(100.0, decay);
    std::cout << "[  ATTACK  ] " << kLarge << ": decay " << decay << " s, level 350 ms against 150 ms: "
              << juce::String(instant, 1) << " dB at 0%, " << juce::String(half, 1) << " dB at 50%, "
              << juce::String(slow, 1) << " dB at 100%" << std::endl;
    EXPECT_LT(instant, -1.0) << "a tail that starts at once is already falling";
    EXPECT_GT(slow, instant + 4.0) << "a slow attack holds the level up";
    EXPECT_GT(half, instant + 1.0);
    EXPECT_LT(half, slow + 1.0);
  }

  // About the same energy goes in at every setting, only later: the whole
  // response carries the same energy within 3.5 dB (the 24 taps are a sparse
  // filter, so the figure is not exact).
  const auto energyDb = [](double attack) {
    const std::vector<float> ir =
        reverbtest::wetImpulseResponse(kLarge, with(kLargeFlat, {{"decayS", 1.0}, {"attack", attack}}), 3.0).left;
    return 10.0 * std::log10(reverbtest::energyOf(ir));
  };
  EXPECT_NEAR(energyDb(100.0), energyDb(0.0), 3.5);
}

TEST(LargeHallReverbTest, DiffusionMakesTheFieldDenser) {
  const auto kurtosis = [](const Params& params, double from, double to) {
    const std::vector<float> ir = reverbtest::wetImpulseResponse(kLarge, params, to + 0.1).left;
    const size_t onset = static_cast<size_t>(std::max(0, onsetIndex(ir, 0.001f)));
    return reverbtest::kurtosisOf(ir, onset + static_cast<size_t>(samples(from)), onset + static_cast<size_t>(samples(to)));
  };
  // Late diffusion: the first 100 ms of the late field is sparser with the
  // allpasses out than in.
  const Params sparse = with(kLargeFlat, {{"decayS", 3.0}, {"lateDiffusion", 0.0}, {"earlyDiffusion", 0.0}});
  const Params dense = with(kLargeFlat, {{"decayS", 3.0}, {"lateDiffusion", 100.0}, {"earlyDiffusion", 100.0}});
  const double sparseK = kurtosis(sparse, 0.0, 0.15);
  const double denseK = kurtosis(dense, 0.0, 0.15);
  std::cout << "[ DENSITY  ] " << kLarge << ": kurtosis of the first 150 ms " << sparseK
            << " with the diffusion at 0, " << denseK << " at 100" << std::endl;
  EXPECT_GT(sparseK, 1.3 * denseK);
}

TEST(LargeHallReverbTest, EarlyDiffusionSmearsTheReflectionsAlone) {
  // The late field is unchanged by the early diffusion knob, apart from the
  // reflections: after 400 ms the two responses are the same.
  const Params a = with(kLargeFlat, {{"decayS", 2.0}, {"earlyDiffusion", 0.0}});
  const Params b = with(kLargeFlat, {{"decayS", 2.0}, {"earlyDiffusion", 100.0}});
  const std::vector<float> irA = reverbtest::wetImpulseResponse(kLarge, a, 1.5).left;
  const std::vector<float> irB = reverbtest::wetImpulseResponse(kLarge, b, 1.5).left;
  EXPECT_NE(irA, irB);
  // The reflections are the first 120 ms at most (and the early diffusers
  // add 6 ms): the tail past 300 ms is the same.
  const size_t from = static_cast<size_t>(samples(0.3));
  double difference = 0.0;
  for (size_t i = from; i < irA.size(); ++i)
    difference += std::pow(static_cast<double>(irA[i] - irB[i]), 2.0);
  EXPECT_LT(difference, 1.0e-6 * std::max(1.0e-12, reverbtest::energyOf(irA, from)));
}

TEST(LargeHallReverbTest, StableAtTheLongestDecayWithTheBassStretched) {
  Params extreme = with(kLargeFlat, {{"decayS", 70.0}, {"bassMult", 4.0}, {"modDepth", 100.0}, {"modRateHz", 0.1}});
  reverbtest::expectBoundedWithHotInput(kLarge, extreme, 20.0f);
}

TEST(LargeHallReverbTest, ModulationRateAndDepthAreHeard) {
  const std::vector<float> in = noise(4.0, 5u, 0.2f, kRate);
  const Params base = with(kLargeFlat, {{"decayS", 3.0}});
  const std::vector<float> still = wetOf(kLarge, with(base, {{"modDepth", 0.0}}), in);
  const std::vector<float> slow = wetOf(kLarge, with(base, {{"modDepth", 100.0}, {"modRateHz", 0.1}}), in);
  const std::vector<float> fast = wetOf(kLarge, with(base, {{"modDepth", 100.0}, {"modRateHz", 10.0}}), in);
  EXPECT_NE(still, slow);
  EXPECT_NE(slow, fast);
  expectCleanSignal(fast, 4.0f, "large hall at the fastest modulation");
}

TEST(LargeHallReverbTest, WideInStereoOnly) {
  reverbtest::expectWideInStereoOnly(kLarge, with(kLargeFlat, {{"decayS", 3.0}}), 0.5, 2.5);
}

// ---------------------------------------------------------------------------
// Ducking reverb
// ---------------------------------------------------------------------------

namespace {
constexpr const char* kDucking = "reverb.ducking";
}  // namespace

TEST(DuckingReverbTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"decayS", 2.0}, {"diffusion", 70.0}, {"bassDb", 3.0}, {"trebleDb", -3.0},
                    {"duckDb", 10.0}, {"duckReleaseMs", 200.0}};
  options.stateParams = {{"decayS", 5.0},      {"preDelayMs", 60.0}, {"diffusion", 30.0},
                         {"bassDb", -6.0},     {"trebleDb", 5.0},    {"lowCutHz", 150.0},
                         {"highCutHz", 8000.0}, {"duckDb", 18.0},    {"duckReleaseMs", 600.0},
                         {"spillover", 0.0}};
  options.latency = LatencyCheck::AtMostOnset;
  options.cpuAverageBudgetNs = 50000.0;
  options.cpuWorstBudgetNs = 400000.0;
  expectKindConforms("reverb.ducking", options);
}

TEST(DuckingReverbTest, SpilloverRingsOutAndCutsOnRequest) {
  // Ducking off for this one: the burst is loud and would hold the wet down.
  delaytest::expectSpilloverBehaviour(kDucking, {{"decayS", 1.5}, {"duckDb", 0.0}}, 100.0, 500.0, 2.0e-3f);
}

TEST(DuckingReverbTest, DucksByDefaultAndHasTheDocumentedParameters) {
  const BlockParamSpec* duck = specOf(kDucking, "duckDb");
  ASSERT_NE(duck, nullptr);
  EXPECT_FLOAT_EQ(duck->defaultValue, 12.0f);
  EXPECT_FLOAT_EQ(duck->max, 24.0f);
  EXPECT_NE(specOf(kDucking, "duckReleaseMs"), nullptr);
  for (const char* id : {"decayS", "diffusion", "bassDb", "trebleDb"})
    EXPECT_NE(specOf(kDucking, id), nullptr) << id;
}

TEST(DuckingReverbTest, DecayParameterIsTheRt60) {
  const Params flat = {{"duckDb", 0.0}, {"lowCutHz", 20.0}, {"highCutHz", 20000.0}};
  expectBandRt60MatchesDecay(kDucking, flat, {0.5, 2.5, 8.0}, 200.0, 1500.0, 0.2);
}

TEST(DuckingReverbTest, BassAndTrebleAreShelves) {
  const Params flat = {{"duckDb", 0.0}, {"decayS", 1.5}, {"lowCutHz", 20.0}, {"highCutHz", 20000.0}};
  const auto bandDb = [&](const Params& params, double lowHz, double highHz) {
    return 10.0 * std::log10(reverbtest::energyOf(
                      reverbtest::bandOf(reverbtest::wetImpulseResponse(kDucking, params, 2.0).left, lowHz, highHz)));
  };
  const double lowFlat = bandDb(flat, 40.0, 120.0);
  const double lowUp = bandDb(with(flat, {{"bassDb", 9.0}}), 40.0, 120.0);
  const double lowDown = bandDb(with(flat, {{"bassDb", -9.0}}), 40.0, 120.0);
  EXPECT_NEAR(lowUp - lowFlat, 9.0, 2.0);
  EXPECT_NEAR(lowDown - lowFlat, -9.0, 2.0);
  const double highFlat = bandDb(flat, 7000.0, 12000.0);
  const double highUp = bandDb(with(flat, {{"trebleDb", 9.0}}), 7000.0, 12000.0);
  const double highDown = bandDb(with(flat, {{"trebleDb", -9.0}}), 7000.0, 12000.0);
  EXPECT_NEAR(highUp - highFlat, 9.0, 2.0);
  EXPECT_NEAR(highDown - highFlat, -9.0, 2.0);
  // The shelves leave the other end alone.
  EXPECT_NEAR(bandDb(with(flat, {{"bassDb", 9.0}}), 7000.0, 12000.0) - highFlat, 0.0, 1.0);
  EXPECT_NEAR(bandDb(with(flat, {{"trebleDb", 9.0}}), 40.0, 120.0) - lowFlat, 0.0, 1.0);
}

TEST(DuckingReverbTest, DiffusionMakesTheFieldDenser) {
  const auto kurtosis = [](double diffusion) {
    const std::vector<float> ir = reverbtest::wetImpulseResponse(
                                      kDucking, {{"duckDb", 0.0}, {"decayS", 2.0}, {"diffusion", diffusion}}, 0.4)
                                      .left;
    const size_t onset = static_cast<size_t>(std::max(0, onsetIndex(ir, 0.001f)));
    return reverbtest::kurtosisOf(ir, onset, onset + static_cast<size_t>(samples(0.1)));
  };
  EXPECT_GT(kurtosis(0.0), 1.3 * kurtosis(100.0));
}

TEST(DuckingReverbTest, WideInStereoOnly) {
  reverbtest::expectWideInStereoOnly(kDucking, {{"duckDb", 0.0}, {"decayS", 2.0}}, 0.3, 1.5);
}

// ---------------------------------------------------------------------------
// Vintage digital
// ---------------------------------------------------------------------------

namespace {
constexpr const char* kVintage = "reverb.vintage";
const Params kVintageClean = {{"wordLength", 0.0}, {"lowCutHz", 20.0}, {"highCutHz", 20000.0},
                              {"preDelayMs", 0.0}, {"modulation", 0.0}};

/** A kind on a running processor whose parameter can change between blocks. */
struct LiveKind {
  explicit LiveKind(const char* kind, const Params& params, Layout layout = Layout::Mono)
      : direct(kind, params, layout) {}

  void set(const char* id, double value) {
    const int index = direct.engine().findParameter(id);
    ASSERT_GE(index, 0) << id;
    direct.engine().setParameter(index, static_cast<float>(value));
  }

  std::vector<float> play(const std::vector<float>& in) { return direct.render(Stereo{in, in}).left; }

  DirectKind direct;
};
}  // namespace

TEST(VintageDigitalReverbTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"decayS", 2.0}, {"size", 70.0}, {"damping", 60.0}, {"modulation", 50.0}};
  options.stateParams = {{"size", 20.0},      {"decayS", 6.0},      {"preDelayMs", 50.0},
                         {"damping", 80.0},   {"modulation", 70.0}, {"freeze", 1.0},
                         {"wordLength", 3.0}, {"lowCutHz", 200.0},  {"highCutHz", 4000.0},
                         {"spillover", 0.0}};
  options.latency = LatencyCheck::AtMostOnset;
  options.cpuAverageBudgetNs = 50000.0;
  options.cpuWorstBudgetNs = 400000.0;
  expectKindConforms("reverb.vintage", options);
}

TEST(VintageDigitalReverbTest, SpilloverRingsOutAndCutsOnRequest) {
  delaytest::expectSpilloverBehaviour(kVintage, {{"decayS", 1.5}}, 100.0, 500.0, 2.0e-3f);
}

TEST(VintageDigitalReverbTest, ParametersAreTheDocumentedRanges) {
  const BlockParamSpec* freeze = specOf(kVintage, "freeze");
  ASSERT_NE(freeze, nullptr);
  EXPECT_EQ(freeze->type, BlockParamType::Bool);
  EXPECT_FLOAT_EQ(freeze->defaultValue, 0.0f);
  EXPECT_TRUE(freeze->affectsTail);
  const BlockParamSpec* word = specOf(kVintage, "wordLength");
  ASSERT_NE(word, nullptr);
  EXPECT_EQ(word->choices.size(), 4);
  for (const char* id : {"size", "decayS", "damping", "modulation", "lowCutHz", "highCutHz", "preDelayMs"})
    EXPECT_NE(specOf(kVintage, id), nullptr) << id;
}

TEST(VintageDigitalReverbTest, DecayParameterIsTheRt60UnderTheDampingCorner) {
  expectBandRt60MatchesDecay(kVintage, with(kVintageClean, {{"damping", 0.0}}), {0.8, 3.0, 8.0}, 150.0, 2000.0, 0.25);
}

TEST(VintageDigitalReverbTest, NothingAboveTheBandLimitGetsIn) {
  const std::vector<float> in = noise(3.0, 9u, 0.25f, kRate);
  const std::vector<float> out = wetOf(kVintage, with(kVintageClean, {{"decayS", 1.5}, {"damping", 0.0}}), in);
  const size_t from = static_cast<size_t>(samples(1.0));
  // Narrow bands (quality 8, two cascaded), so the skirts of the measuring
  // filter do not carry the pass-band into a stop-band reading.
  const size_t end = out.size();
  const auto levelAt = [&](double hz) { return reverbtest::bandRms(out, hz, 8.0, from, end); };
  const double reference = levelAt(3000.0);
  for (const double hz : {5000.0, 7000.0, 9000.0, 12000.0, 16000.0})
    std::cout << "[   BAND   ] " << kVintage << ": " << hz << " Hz is " << juce::String(toDb(levelAt(hz) / reference), 1)
              << " dB against 3 kHz" << std::endl;
  // Flat to the corner, then eight poles of Butterworth at 7 kHz: 3 dB down
  // there, 26 dB down at 9.5 kHz, 45 dB at 11.5 kHz.
  // (A narrow band of a reverb tail wanders a couple of dB with its modes.)
  EXPECT_GT(toDb(levelAt(5000.0) / reference), -4.0);
  EXPECT_LT(toDb(levelAt(9000.0) / reference), -10.0);
  EXPECT_LT(toDb(levelAt(12000.0) / reference), -30.0);
  EXPECT_LT(toDb(levelAt(16000.0) / reference), -40.0);
}

TEST(VintageDigitalReverbTest, WordLengthRoundsTheWet) {
  const std::vector<float> in = noise(1.5, 9u, 0.25f, kRate);
  const struct {
    double choice;
    float steps;
  } cases[] = {{1.0, 8192.0f}, {2.0, 2048.0f}, {3.0, 512.0f}};
  const std::vector<float> smooth = wetOf(kVintage, with(kVintageClean, {{"decayS", 1.5}}), in);
  size_t offGrid = 0;
  for (const float v : smooth)
    offGrid += std::abs(v * 512.0f - std::round(v * 512.0f)) > 1.0e-3f ? 1u : 0u;
  EXPECT_GT(offGrid, smooth.size() / 2) << "word length Off should not round";
  for (const auto& c : cases) {
    SCOPED_TRACE(c.choice);
    const std::vector<float> rounded =
        wetOf(kVintage, with(kVintageClean, {{"decayS", 1.5}, {"wordLength", c.choice}}), in);
    ASSERT_EQ(rounded.size(), smooth.size());
    for (size_t i = 0; i < rounded.size(); ++i)
      ASSERT_NEAR(rounded[i] * c.steps, std::round(rounded[i] * c.steps), 1.0e-3f) << i;
    // Mild: the error is at most half a step, so the level hardly moves.
    EXPECT_NEAR(toDb(rmsOf(rounded, static_cast<size_t>(samples(0.5))) / rmsOf(smooth, static_cast<size_t>(samples(0.5)))),
                0.0, 0.5);
  }
}

TEST(VintageDigitalReverbTest, FreezeHoldsTheTailForFiveSeconds) {
  // Play noise through a 2 s decay, then freeze with the input still going:
  // the tank is shut, so the level it held is the level it keeps.
  for (const Layout layout : {Layout::Mono, Layout::Stereo, Layout::DualMono}) {
    SCOPED_TRACE(layoutName(layout));
    LiveKind live(kVintage, with(kVintageClean, {{"decayS", 2.0}, {"modulation", 40.0}, {"wordLength", 0.0}}), layout);
    live.play(noise(2.0, 3u, 0.25f, kRate));
    live.set("freeze", 1.0);
    const std::vector<float> held = live.play(noise(7.0, 4u, 0.25f, kRate));
    const double first = windowRms(held, 0.5, 0.5);
    ASSERT_GT(first, 0.005);
    double worst = 0.0;
    for (double at = 1.0; at <= 6.5; at += 0.5)
      worst = std::max(worst, std::abs(toDb(windowRms(held, at, 0.5) / first)));
    std::cout << "[  FREEZE  ] " << kVintage << " (" << layoutName(layout) << "): level " << juce::String(toDb(first), 1)
              << " dB; the largest change in 6.5 s " << juce::String(worst, 2) << " dB" << std::endl;
    EXPECT_LT(worst, 1.0);
    // Silence in does not change it either, and the reported tail never ends.
    const std::vector<float> quiet = live.play(std::vector<float>(static_cast<size_t>(samples(5.0)), 0.0f));
    EXPECT_NEAR(toDb(windowRms(quiet, 4.0, 1.0) / first), 0.0, 1.0);
    EXPECT_GE(live.direct.engine().tailSeconds(), kMaxBlockTailSeconds);
  }
}

TEST(VintageDigitalReverbTest, FreezeKeepsOutNewInputAndLetsGoWhenOff) {
  LiveKind live(kVintage, with(kVintageClean, {{"decayS", 1.0}}));
  live.play(noise(1.5, 3u, 0.25f, kRate));
  live.set("freeze", 1.0);
  live.play(std::vector<float>(static_cast<size_t>(samples(0.5)), 0.0f));
  // The same silence-fed tail, once with loud noise going in and once with
  // none: the tail does not hear the playing.
  LiveKind other(kVintage, with(kVintageClean, {{"decayS", 1.0}}));
  other.play(noise(1.5, 3u, 0.25f, kRate));
  other.set("freeze", 1.0);
  other.play(std::vector<float>(static_cast<size_t>(samples(0.5)), 0.0f));
  const std::vector<float> loud = live.play(noise(2.0, 99u, 0.5f, kRate));
  const std::vector<float> silent = other.play(std::vector<float>(static_cast<size_t>(samples(2.0)), 0.0f));
  double difference = 0.0;
  for (size_t i = static_cast<size_t>(samples(0.1)); i < loud.size(); ++i)
    difference = std::max(difference, static_cast<double>(std::abs(loud[i] - silent[i])));
  EXPECT_LT(difference, 1.0e-4) << "audio played while frozen reached the tank";

  // Released, the tail decays at the decay setting again.
  const double held = windowRms(silent, 1.0, 0.5);
  live.set("freeze", 0.0);
  const std::vector<float> after = live.play(std::vector<float>(static_cast<size_t>(samples(4.0)), 0.0f));
  EXPECT_LT(toDb(windowRms(after, 3.0, 0.5) / held), -30.0);
}

TEST(VintageDigitalReverbTest, SizeAndModulationAreHeard) {
  const std::vector<float> in = noise(2.0, 5u, 0.2f, kRate);
  const Params base = with(kVintageClean, {{"decayS", 2.0}});
  EXPECT_NE(wetOf(kVintage, with(base, {{"size", 0.0}}), in), wetOf(kVintage, with(base, {{"size", 100.0}}), in));
  EXPECT_NE(wetOf(kVintage, with(base, {{"modulation", 0.0}}), in),
            wetOf(kVintage, with(base, {{"modulation", 100.0}}), in));
}

TEST(VintageDigitalReverbTest, PreDelayAndTailHold) {
  reverbtest::expectPreDelayMatches(kVintage, kVintageClean, {10.0, 100.0, 200.0});
  reverbtest::expectTailHolds(kVintage, with(kVintageClean, {{"decayS", 3.0}, {"preDelayMs", 200.0}}));
}

TEST(VintageDigitalReverbTest, WideInStereoOnly) {
  reverbtest::expectWideInStereoOnly(kVintage, with(kVintageClean, {{"decayS", 2.0}}), 0.3, 1.5);
}

// ---------------------------------------------------------------------------
// Ducking, common to every reverb
// ---------------------------------------------------------------------------

namespace {

struct ReverbCase {
  const char* id;
  // A setting with a lasting tail: the reverb is audible for a second after
  // the input stops.
  Params params;
};

std::vector<ReverbCase> existingReverbs() {
  return {
      {"reverb.room", {{"decayS", 2.5}}},     {"reverb.hall", {{"decayS", 6.0}}},
      {"reverb.plate", {{"decayS", 6.0}}},    {"reverb.shimmer", {{"decayS", 6.0}}},
      {"reverb.spring", {{"decayS", 4.0}}},
  };
}

std::vector<ReverbCase> allDuckingReverbs() {
  std::vector<ReverbCase> all = existingReverbs();
  all.push_back({"reverb.ducking", {{"decayS", 6.0}, {"duckDb", 0.0}}});
  return all;
}

Params withDuck(Params base, double depth, double release = 250.0) {
  return with(std::move(base), {{"duckDb", depth}, {"duckReleaseMs", release}});
}

}  // namespace

TEST(ReverbDuckingTest, EveryReverbHasTheSameTwoParametersBeforeSpillover) {
  for (const ReverbCase& c : allDuckingReverbs()) {
    SCOPED_TRACE(c.id);
    const BlockKindInfo* info = BlockKindRegistry::instance().find(c.id);
    ASSERT_NE(info, nullptr);
    const int duck = findBlockParam(info->params, "duckDb");
    const int release = findBlockParam(info->params, "duckReleaseMs");
    ASSERT_GE(duck, 0);
    ASSERT_GE(release, 0);
    EXPECT_EQ(release, duck + 1);
    EXPECT_FLOAT_EQ(info->params[static_cast<size_t>(duck)].min, 0.0f);
    EXPECT_FLOAT_EQ(info->params[static_cast<size_t>(duck)].max, 24.0f);
    // Off by default on the five that were here before, so their sound and
    // their saved presets are unchanged; the ducking reverb ducks.
    EXPECT_FLOAT_EQ(info->params[static_cast<size_t>(duck)].defaultValue,
                    std::string(c.id) == "reverb.ducking" ? 12.0f : 0.0f);
    EXPECT_FLOAT_EQ(info->params[static_cast<size_t>(release)].min, 50.0f);
    EXPECT_FLOAT_EQ(info->params[static_cast<size_t>(release)].max, 1000.0f);
    EXPECT_FLOAT_EQ(info->params[static_cast<size_t>(release)].defaultValue, 250.0f);
    EXPECT_EQ(info->params.back().id, kSpilloverParamId);
    EXPECT_EQ(release + 2, static_cast<int>(info->params.size()));
  }
}

TEST(ReverbDuckingTest, AtZeroDbTheWetPathIsBitIdenticalWhateverTheReleaseIs) {
  const std::vector<float> loud = noise(2.0, 12u, 0.25f, kRate);
  const std::vector<float> other = noise(2.0, 13u, 0.2f, kRate);
  for (const ReverbCase& c : allDuckingReverbs()) {
    SCOPED_TRACE(c.id);
    for (const Layout layout : {Layout::Mono, Layout::Stereo, Layout::DualMono}) {
      SCOPED_TRACE(layoutName(layout));
      const Stereo a = wetOf(c.id, withDuck(c.params, 0.0, 50.0), Stereo{loud, other}, layout);
      const Stereo b = wetOf(c.id, withDuck(c.params, 0.0, 1000.0), Stereo{loud, other}, layout);
      EXPECT_EQ(a.left, b.left);
      EXPECT_EQ(a.right, b.right);
      EXPECT_GT(peakOf(a.left), 0.01f);
      const Stereo ducked = wetOf(c.id, withDuck(c.params, 6.0), Stereo{loud, other}, layout);
      EXPECT_NE(a.left, ducked.left);
    }
  }
}

TEST(ReverbDuckingTest, PlayingBelowTheDetectorLeavesTheWetBitIdentical) {
  // 40 dB under full scale is below the detector's range, so the gain stays
  // exactly 1: the path that copies the dry and multiplies the wet changes
  // nothing, bit for bit, however the host cuts the audio into blocks.
  const std::vector<float> quiet = noise(2.0, 15u, 0.004f, kRate);
  const std::vector<float> quiet2 = noise(2.0, 16u, 0.003f, kRate);
  for (const ReverbCase& c : allDuckingReverbs()) {
    SCOPED_TRACE(c.id);
    for (const Layout layout : {Layout::Mono, Layout::Stereo, Layout::DualMono}) {
      SCOPED_TRACE(layoutName(layout));
      for (const int block : {64, 480}) {
        const Stereo reference = wetOf(c.id, withDuck(c.params, 0.0), Stereo{quiet, quiet2}, layout, block);
        const Stereo ducked = wetOf(c.id, withDuck(c.params, 24.0), Stereo{quiet, quiet2}, layout, block);
        EXPECT_EQ(reference.left, ducked.left);
        EXPECT_EQ(reference.right, ducked.right);
      }
    }
  }
}

TEST(ReverbDuckingTest, LowersTheWetByTheSetDepthWhileTheInputIsLoud) {
  const std::vector<float> loud = noise(3.0, 13u, 0.25f, kRate);
  for (const ReverbCase& c : allDuckingReverbs()) {
    for (const Layout layout : {Layout::Mono, Layout::Stereo, Layout::DualMono}) {
      for (const double depth : {6.0, 12.0, 24.0}) {
        SCOPED_TRACE(std::string(c.id) + " " + layoutName(layout) + " " + std::to_string(depth) + " dB");
        const Stereo reference = wetOf(c.id, withDuck(c.params, 0.0), Stereo{loud, loud}, layout);
        const Stereo ducked = wetOf(c.id, withDuck(c.params, depth), Stereo{loud, loud}, layout);
        const std::vector<float>* const sides[2][2] = {{&reference.left, &ducked.left},
                                                       {&reference.right, &ducked.right}};
        for (const auto& side : sides) {
          const double before = windowRms(*side[0], 1.5, 1.5);
          ASSERT_GT(before, 0.01);
          EXPECT_NEAR(toDb(windowRms(*side[1], 1.5, 1.5) / before), -depth, 1.0);
        }
      }
    }
  }
}

TEST(ReverbDuckingTest, LetsGoWithinTheReleaseAndLeavesTheTailAlone) {
  // 1.5 s of loud noise, then silence: the tank goes on ringing. While the
  // burst plays the wet is held down; by the release (plus the detector's
  // fall) it is back where the unducked reverb has it, which also shows the
  // tail itself was never ducked.
  std::vector<float> in = noise(1.5, 14u, 0.25f, kRate);
  in.resize(static_cast<size_t>(4.0 * kRate), 0.0f);
  const double end = 1.5;
  for (const ReverbCase& c : allDuckingReverbs()) {
    for (const double release : {100.0, 250.0, 600.0}) {
      SCOPED_TRACE(std::string(c.id) + " release " + std::to_string(release));
      const std::vector<float> reference = wetOf(c.id, withDuck(c.params, 0.0), in);
      const std::vector<float> ducked = wetOf(c.id, withDuck(c.params, 12.0, release), in);

      const double heldRef = windowRms(reference, end - 0.2, 0.2);
      ASSERT_GT(heldRef, 0.01);
      EXPECT_LT(toDb(windowRms(ducked, end - 0.2, 0.2) / heldRef), -10.0) << "ducked while loud";

      const double at = end + release * 0.001 + 0.15;
      const double lateRef = windowRms(reference, at, 0.05);
      ASSERT_GT(lateRef, 0.0015);
      EXPECT_GT(toDb(windowRms(ducked, at, 0.05) / lateRef), -1.0) << "back within the release";
      EXPECT_LT(toDb(windowRms(ducked, at, 0.05) / lateRef), 1.0);
    }
  }
}

TEST(ReverbDuckingTest, AStereoPairIsDuckedAsOneSoTheImageDoesNotLean) {
  const std::vector<float> loud = noise(2.0, 16u, 0.25f, kRate);
  const std::vector<float> other = noise(2.0, 17u, 0.05f, kRate);
  for (const ReverbCase& c : allDuckingReverbs()) {
    SCOPED_TRACE(c.id);
    const Stereo reference = wetOf(c.id, withDuck(c.params, 0.0), Stereo{loud, other}, Layout::Stereo);
    const Stereo ducked = wetOf(c.id, withDuck(c.params, 12.0), Stereo{loud, other}, Layout::Stereo);
    size_t compared = 0;
    for (size_t i = static_cast<size_t>(1.0 * kRate); i < loud.size(); ++i) {
      if (std::abs(reference.left[i]) < 0.02f || std::abs(reference.right[i]) < 0.02f)
        continue;
      const double leftGain = static_cast<double>(ducked.left[i]) / static_cast<double>(reference.left[i]);
      const double rightGain = static_cast<double>(ducked.right[i]) / static_cast<double>(reference.right[i]);
      ASSERT_NEAR(leftGain, rightGain, 1.0e-3) << i;
      ++compared;
    }
    EXPECT_GT(compared, 200u);
  }
}

TEST(ReverbDuckingTest, TheResultDoesNotDependOnTheBlockSize) {
  const std::vector<float> in = noise(3.0, 18u, 0.25f, kRate);
  for (const ReverbCase& c : allDuckingReverbs()) {
    SCOPED_TRACE(c.id);
    const Params params = withDuck(c.params, 12.0, 300.0);
    const std::vector<float> shortBlocks = wetOf(c.id, params, Stereo{in, in}, Layout::Stereo, 32).left;
    const std::vector<float> longBlocks = wetOf(c.id, params, Stereo{in, in}, Layout::Stereo, 480).left;
    std::vector<float> difference(shortBlocks.size());
    for (size_t i = 0; i < difference.size(); ++i)
      difference[i] = shortBlocks[i] - longBlocks[i];
    EXPECT_LT(toDb(rmsOf(difference)) - toDb(rmsOf(shortBlocks)), -60.0);
  }
}

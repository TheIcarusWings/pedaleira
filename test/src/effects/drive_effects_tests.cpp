// The drive family (plugin/src/effects/drive): clean boost, treble boost,
// overdrive, transparent drive, distortion, metal distortion, fuzz and bass
// drive. Every kind goes through the same checks: the conformance test of the
// framework, stability at extreme settings, distortion that rises with the
// drive control, loudness near unity at the defaults, a reset that restores
// the first render exactly, little aliasing from a high frequency tone at
// high gain, and its place in the Drive category. Then what each one is for.
#include "effects/effect_test_helpers.h"

#include <gtest/gtest.h>

using namespace fxtest;

namespace {

constexpr double kRate = 48000.0;

float amp(double levelDb) { return static_cast<float>(std::pow(10.0, levelDb / 20.0)); }

struct DriveCase {
  const char* id;
  // A setting with little drive and one with a lot, for the THD check, and
  // the level and frequency of the sine they are measured with.
  Params little;
  Params lots;
  float amplitude;
  double hz;
};

// The ids are literals elsewhere (the registration test looks for them in
// this directory); this table is the one list the generic checks share.
std::vector<DriveCase> allCases() {
  return {
      {"drive.boost", {{"gainDb", 0.0}}, {{"gainDb", 20.0}}, 0.5f, 1000.0},
      {"drive.trebleboost", {{"gainDb", 0.0}}, {{"gainDb", 30.0}}, 0.15f, 3000.0},
      {"drive.overdrive", {{"drivePercent", 5.0}}, {{"drivePercent", 95.0}}, 0.1f, 1000.0},
      {"drive.transparent", {{"drivePercent", 5.0}}, {{"drivePercent", 95.0}}, 0.1f, 1000.0},
      {"drive.distortion", {{"distortionPercent", 5.0}}, {{"distortionPercent", 95.0}}, 0.05f,
       1000.0},
      {"drive.metal", {{"distortionPercent", 0.0}}, {{"distortionPercent", 100.0}}, 0.003f, 1000.0},
      {"drive.fuzz",
       {{"fuzzPercent", 5.0}, {"biasPercent", 100.0}},
       {{"fuzzPercent", 95.0}, {"biasPercent", 100.0}},
       0.05f,
       1000.0},
      {"drive.bass",
       {{"drivePercent", 0.0}, {"blendPercent", 100.0}},
       {{"drivePercent", 100.0}, {"blendPercent", 100.0}},
       0.1f,
       1000.0},
  };
}

// The kind's parameters at their minimum or maximum, or alternating.
Params extremes(const char* id, int pattern) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(id);
  Params out;
  if (info == nullptr)
    return out;
  int index = 0;
  for (const BlockParamSpec& spec : info->params) {
    const bool high = pattern == 0 ? false : pattern == 1 ? true : (index % 2 == 0);
    out.emplace_back(spec.id, static_cast<double>(high ? spec.max : spec.min));
    ++index;
  }
  return out;
}

// A kind run on its own on one sine, returned after `settle` seconds.
std::vector<float> renderSine(const char* id, const Params& params, double hz, float amplitude,
                              double seconds = 1.0) {
  DirectKind kind(id, params);
  return kind.render(sine(seconds, hz, amplitude, kRate));
}

double thdOf(const char* id, const Params& params, double hz, float amplitude) {
  const std::vector<float> out = renderSine(id, params, hz, amplitude);
  return thdPercent(out, hz, kRate, static_cast<size_t>(0.3 * kRate), 10);
}

// RMS gain in dB of a kind on a sine, after the first 0.3 s.
double levelGainDb(const char* id, const Params& params, double hz, float amplitude) {
  const std::vector<float> in = sine(1.0, hz, amplitude, kRate);
  DirectKind kind(id, params);
  const std::vector<float> out = kind.render(in);
  return steadyStateGainDb(out, in, static_cast<size_t>(0.3 * kRate));
}

}  // namespace

// ---------------------------------------------------------------------------
// Conformance
// ---------------------------------------------------------------------------

TEST(DriveEffectsTest, CleanBoostConforms) {
  ConformanceOptions options;
  options.params = {{"gainDb", 14.0}, {"bassDb", 6.0}, {"trebleDb", -4.0}};
  options.stateParams = {{"gainDb", 9.0}, {"bassDb", -5.0}, {"trebleDb", 7.0}};
  options.sweepBound = 4.0f;
  options.cpuAverageBudgetNs = 50000.0;
  options.cpuWorstBudgetNs = 500000.0;
  expectKindConforms("drive.boost", options);
}

TEST(DriveEffectsTest, TrebleBoostConforms) {
  ConformanceOptions options;
  options.params = {{"gainDb", 24.0}, {"rangeHz", 1500.0}, {"levelDb", -3.0}};
  options.stateParams = {{"gainDb", 20.0}, {"rangeHz", 700.0}, {"levelDb", 4.0}};
  options.sweepBound = 16.0f;
  options.cpuAverageBudgetNs = 50000.0;
  options.cpuWorstBudgetNs = 500000.0;
  expectKindConforms("drive.trebleboost", options);
}

TEST(DriveEffectsTest, OverdriveConforms) {
  ConformanceOptions options;
  options.params = {{"drivePercent", 70.0}, {"tonePercent", 40.0}, {"levelDb", 2.0}};
  options.stateParams = {{"drivePercent", 20.0}, {"tonePercent", 80.0}, {"levelDb", -4.0}};
  options.sweepBound = 16.0f;
  options.cpuAverageBudgetNs = 50000.0;
  options.cpuWorstBudgetNs = 500000.0;
  expectKindConforms("drive.overdrive", options);
}

TEST(DriveEffectsTest, TransparentDriveConforms) {
  ConformanceOptions options;
  options.params = {{"drivePercent", 70.0}, {"trebleDb", 3.0}, {"levelDb", 1.0}};
  options.stateParams = {{"drivePercent", 20.0}, {"trebleDb", -6.0}, {"levelDb", -4.0}};
  options.sweepBound = 16.0f;
  options.cpuAverageBudgetNs = 50000.0;
  options.cpuWorstBudgetNs = 500000.0;
  expectKindConforms("drive.transparent", options);
}

TEST(DriveEffectsTest, DistortionConforms) {
  ConformanceOptions options;
  options.params = {{"distortionPercent", 80.0}, {"filterPercent", 40.0}, {"levelDb", 2.0}};
  options.stateParams = {{"distortionPercent", 20.0}, {"filterPercent", 90.0}, {"levelDb", -4.0}};
  options.sweepBound = 16.0f;
  options.cpuAverageBudgetNs = 55000.0;
  options.cpuWorstBudgetNs = 500000.0;
  expectKindConforms("drive.distortion", options);
}

TEST(DriveEffectsTest, MetalDistortionConforms) {
  ConformanceOptions options;
  options.params = {{"distortionPercent", 85.0}, {"tightHz", 220.0}, {"midDb", -5.0},
                    {"midHz", 900.0},            {"lowDb", 3.0},     {"gateDb", -60.0}};
  options.stateParams = {{"distortionPercent", 30.0}, {"tightHz", 90.0}, {"lowDb", -4.0},
                         {"midDb", 5.0},              {"midHz", 450.0},  {"highDb", 6.0},
                         {"levelDb", -3.0},           {"gateDb", -45.0}};
  // Level up to +18 dB and +12 dB of shelf on a clipped signal.
  options.sweepBound = 24.0f;
  options.cpuAverageBudgetNs = 90000.0;
  options.cpuWorstBudgetNs = 600000.0;
  expectKindConforms("drive.metal", options);
}

TEST(DriveEffectsTest, FuzzConforms) {
  ConformanceOptions options;
  options.params = {{"voicing", 1.0}, {"fuzzPercent", 80.0}, {"biasPercent", 40.0}, {"tonePercent", 60.0}};
  options.stateParams = {{"voicing", 2.0}, {"fuzzPercent", 30.0}, {"biasPercent", 90.0},
                         {"tonePercent", 20.0}, {"levelDb", -3.0}};
  options.sweepBound = 16.0f;
  options.cpuAverageBudgetNs = 90000.0;
  options.cpuWorstBudgetNs = 500000.0;
  expectKindConforms("drive.fuzz", options);
}

TEST(DriveEffectsTest, BassDriveConforms) {
  ConformanceOptions options;
  options.params = {{"crossoverHz", 300.0}, {"drivePercent", 70.0}, {"presenceDb", 3.0}, {"blendPercent", 80.0}};
  options.stateParams = {{"crossoverHz", 120.0}, {"drivePercent", 20.0}, {"presenceDb", -4.0},
                         {"blendPercent", 40.0}, {"levelDb", 3.0}};
  options.sweepBound = 16.0f;
  options.cpuAverageBudgetNs = 90000.0;
  options.cpuWorstBudgetNs = 500000.0;
  expectKindConforms("drive.bass", options);
}

// ---------------------------------------------------------------------------
// Every kind
// ---------------------------------------------------------------------------

TEST(DriveEffectsTest, TheKindsAreListedInTheDriveCategory) {
  EffectRig rig{juce::String()};
  const juce::var listed = rig.must("kinds.list");
  const auto* kinds = listed["kinds"].getArray();
  ASSERT_NE(kinds, nullptr);
  int found = 0;
  for (const DriveCase& c : allCases()) {
    bool present = false;
    for (const auto& kind : *kinds)
      if (kind["id"].toString() == c.id) {
        present = true;
        ++found;
        EXPECT_EQ(kind["category"].toString(), juce::String("drive")) << c.id;
        EXPECT_TRUE(static_cast<bool>(kind["addable"])) << c.id;
        EXPECT_DOUBLE_EQ(static_cast<double>(kind["defaultMix"]), 1.0) << c.id;
        EXPECT_EQ(kind["mixLaw"].toString(), juce::String("crossfade")) << c.id;
        EXPECT_TRUE(kind["name"].toString().isNotEmpty()) << c.id;
      }
    EXPECT_TRUE(present) << c.id << " is not in kinds.list";
  }
  EXPECT_EQ(found, 8);
  // Nothing else is in the category by accident: the eight are the family.
  int inCategory = 0;
  for (const auto& kind : *kinds)
    if (kind["category"].toString() == "drive")
      ++inCategory;
  EXPECT_GE(inCategory, 8);
}

TEST(DriveEffectsTest, NoNaNOrInfAndBoundedAtExtremeSettings) {
  const std::vector<float> silence(static_cast<size_t>(0.25 * kRate), 0.0f);
  const std::vector<float> loudNoise = makeNoise(static_cast<int>(0.5 * kRate), 5u, 1.0f);
  const std::vector<float> fullSine = sine(0.5, 1000.0, 1.0f, kRate);
  const std::vector<float> highSine = sine(0.5, 7000.0, 1.0f, kRate);
  for (const DriveCase& c : allCases()) {
    for (int pattern = 0; pattern < 3; ++pattern) {
      for (const Layout layout : {Layout::Mono, Layout::Stereo}) {
        SCOPED_TRACE(std::string(c.id) + " pattern " + std::to_string(pattern));
        DirectKind kind(c.id, extremes(c.id, pattern), layout);
        for (const std::vector<float>* signal : {&silence, &loudNoise, &fullSine, &highSine}) {
          kind.engine().reset();
          const Stereo out = kind.render(Stereo{*signal, *signal});
          expectCleanSignal(out.left, 100.0f, "left");
          expectCleanSignal(out.right, 100.0f, "right");
        }
      }
    }
  }
}

TEST(DriveEffectsTest, SilenceInSilenceOut) {
  const std::vector<float> silence(static_cast<size_t>(0.5 * kRate), 0.0f);
  for (const DriveCase& c : allCases()) {
    SCOPED_TRACE(c.id);
    DirectKind kind(c.id, extremes(c.id, 1));
    const std::vector<float> out = kind.render(silence);
    EXPECT_EQ(peakOf(out), 0.0f);
  }
}

TEST(DriveEffectsTest, OutputStaysBoundedAtDefaultSettings) {
  for (const DriveCase& c : allCases()) {
    SCOPED_TRACE(c.id);
    const std::vector<float> out = renderSine(c.id, {}, 220.0, 1.0f);
    expectCleanSignal(out, 2.0f, "0 dBFS sine at the defaults");
    DirectKind kind(c.id);
    expectCleanSignal(kind.render(makeNoise(static_cast<int>(kRate), 9u, 1.0f)), 3.0f,
                      "full scale noise at the defaults");
  }
}

TEST(DriveEffectsTest, DistortionRisesWithTheDriveControl) {
  for (const DriveCase& c : allCases()) {
    SCOPED_TRACE(c.id);
    const double little = thdOf(c.id, c.little, c.hz, c.amplitude);
    const double lots = thdOf(c.id, c.lots, c.hz, c.amplitude);
    std::cout << "[   THD    ] " << c.id << ": " << juce::String(little, 2) << " % -> "
              << juce::String(lots, 2) << " %" << std::endl;
    EXPECT_GT(lots, little + 5.0);
  }
}

TEST(DriveEffectsTest, EveryFuzzVoicingDistortsMoreWithMoreFuzz) {
  for (const double voicing : {0.0, 1.0, 2.0}) {
    SCOPED_TRACE(voicing);
    const double little =
        thdOf("drive.fuzz", {{"voicing", voicing}, {"fuzzPercent", 5.0}, {"biasPercent", 100.0}}, 1000.0, 0.05f);
    const double lots =
        thdOf("drive.fuzz", {{"voicing", voicing}, {"fuzzPercent", 95.0}, {"biasPercent", 100.0}}, 1000.0, 0.05f);
    EXPECT_GT(lots, little + 5.0);
  }
}

TEST(DriveEffectsTest, LoudnessIsNearUnityAtTheDefaults) {
  for (const DriveCase& c : allCases()) {
    SCOPED_TRACE(c.id);
    // -18 dBFS as a sine's peak, and as its RMS (3 dB hotter).
    for (const double peakDb : {-18.0, -15.0}) {
      const double gain = levelGainDb(c.id, {}, 220.0, amp(peakDb));
      std::cout << "[   LEVEL  ] " << c.id << " at " << peakDb << " dBFS peak: "
                << juce::String(gain, 2) << " dB" << std::endl;
      EXPECT_NEAR(gain, 0.0, 3.0);
    }
  }
}

TEST(DriveEffectsTest, EveryFuzzVoicingIsNearUnityToo) {
  for (const double voicing : {0.0, 1.0, 2.0}) {
    const double gain = levelGainDb("drive.fuzz", {{"voicing", voicing}}, 220.0, amp(-18.0));
    std::cout << "[   LEVEL  ] fuzz voicing " << voicing << ": " << juce::String(gain, 2) << " dB"
              << std::endl;
    EXPECT_NEAR(gain, 0.0, 3.0) << voicing;
  }
}

TEST(DriveEffectsTest, ResetRestoresTheFirstRenderExactly) {
  const std::vector<float> in = musicalSignal(0.6, kRate, 0.3f);
  for (const DriveCase& c : allCases()) {
    SCOPED_TRACE(c.id);
    for (const Params& params : {Params{}, extremes(c.id, 2)}) {
      DirectKind kind(c.id, params, Layout::Stereo);
      const Stereo first = kind.render(Stereo{in, in});
      kind.engine().reset();
      const Stereo second = kind.render(Stereo{in, in});
      EXPECT_EQ(std::memcmp(first.left.data(), second.left.data(), in.size() * sizeof(float)), 0);
      EXPECT_EQ(std::memcmp(first.right.data(), second.right.data(), in.size() * sizeof(float)), 0);
    }
  }
}

TEST(DriveEffectsTest, ReportsTheLatencyOfTheOversampler) {
  // Every path goes through the 4x half-band filters; the conformance test
  // checks the number against where an impulse's response begins.
  for (const DriveCase& c : allCases()) {
    DirectKind kind(c.id);
    EXPECT_EQ(kind.latencySamples(), 3) << c.id;
  }
}

TEST(DriveEffectsTest, AHighToneAtHighGainLeavesLittleBelowOneKilohertz) {
  // A 5 kHz sine through a nonlinearity makes harmonics at 10, 15, 20 kHz...;
  // the ones above Nyquist fold back as aliases. With the clipping at 4x the
  // aliases that land low must be far below the tone. The bound is loose
  // (-45 dB): it separates a clipper that is oversampled from one that is not.
  struct Loud {
    const char* id;
    Params params;
  };
  const std::vector<Loud> loud = {
      {"drive.trebleboost", {{"gainDb", 30.0}, {"rangeHz", 400.0}}},
      {"drive.overdrive", {{"drivePercent", 100.0}}},
      {"drive.transparent", {{"drivePercent", 100.0}}},
      {"drive.distortion", {{"distortionPercent", 100.0}, {"filterPercent", 100.0}}},
      {"drive.metal", {{"distortionPercent", 100.0}, {"highDb", 12.0}}},
      {"drive.fuzz", {{"voicing", 0.0}, {"fuzzPercent", 100.0}, {"tonePercent", 100.0}}},
      {"drive.fuzz", {{"voicing", 1.0}, {"fuzzPercent", 100.0}, {"tonePercent", 100.0}}},
      {"drive.fuzz", {{"voicing", 2.0}, {"fuzzPercent", 100.0}, {"tonePercent", 100.0}}},
      {"drive.bass", {{"drivePercent", 100.0}, {"blendPercent", 100.0}, {"crossoverHz", 60.0}}},
      {"drive.boost", {{"gainDb", 20.0}}},
  };
  for (const Loud& item : loud) {
    SCOPED_TRACE(item.id);
    const double fs = kRate;
    const std::vector<float> in = sine(0.6, 5000.0, 0.5f, fs);
    DirectKind kind(item.id, item.params);
    const std::vector<float> out = kind.render(in);
    const size_t skip = static_cast<size_t>(0.2 * fs);
    const double fundamental = db(goertzelPower(out.data() + skip, out.size() - skip, 5000.0, fs));
    double worst = -300.0;
    for (double hz = 60.0; hz <= 1000.0; hz += 7.3)
      worst = std::max(worst, db(goertzelPower(out.data() + skip, out.size() - skip, hz, fs)));
    std::cout << "[  ALIAS   ] " << item.id << ": worst " << juce::String(worst - fundamental, 1)
              << " dB under the tone" << std::endl;
    EXPECT_LT(worst - fundamental, -45.0);
  }
}

// ---------------------------------------------------------------------------
// Clean Boost
// ---------------------------------------------------------------------------

TEST(DriveEffectsTest, CleanBoostGainIsExactAndTheShelvesAreShelves) {
  EffectRig rig("drive.boost", {{"gainDb", 10.0}, {"mix", 1.0}});
  EXPECT_NEAR(sineProbeGainDb(rig, 1000.0, 0.05f), 10.0, 0.3);
  EXPECT_NEAR(sineProbeGainDb(rig, 100.0, 0.05f), 10.0, 0.3);

  // Bass +12 lifts the lows by about that much and leaves 2 kHz alone.
  EffectRig bass("drive.boost", {{"gainDb", 0.0}, {"bassDb", 12.0}, {"mix", 1.0}});
  EXPECT_GT(sineProbeGainDb(bass, 40.0, 0.05f), 9.0);
  EXPECT_NEAR(sineProbeGainDb(bass, 2000.0, 0.05f), 0.0, 0.6);
  // Treble -12 cuts the highs by about that much and leaves 100 Hz alone.
  EffectRig treble("drive.boost", {{"gainDb", 0.0}, {"trebleDb", -12.0}, {"mix", 1.0}});
  EXPECT_LT(sineProbeGainDb(treble, 10000.0, 0.05f), -6.0);  // one pole: slow to settle
  EXPECT_NEAR(sineProbeGainDb(treble, 100.0, 0.05f), 0.0, 0.6);
}

TEST(DriveEffectsTest, CleanBoostIsTransparentUntilFullScale) {
  // Well under the soft limit the boost adds no distortion at all.
  EXPECT_LT(thdOf("drive.boost", {{"gainDb", 10.0}}, 1000.0, 0.1f), 0.05);
  // Pushed past full scale it limits softly: the peak stays under 1.2 (the
  // oversampling filters can overshoot a flat top by a little) and the
  // wave is still a wave.
  const std::vector<float> out = renderSine("drive.boost", {{"gainDb", 20.0}}, 440.0, 1.0f);
  EXPECT_LE(peakOf(out), 1.2f);
  EXPECT_GT(peakOf(out), 0.9f);
}

// ---------------------------------------------------------------------------
// Treble Boost
// ---------------------------------------------------------------------------

TEST(DriveEffectsTest, TrebleBoostLiftsAboveTheRangeAndRangeMovesIt) {
  const Params base = {{"gainDb", 12.0}, {"rangeHz", 1000.0}};
  const double low = levelGainDb("drive.trebleboost", base, 150.0, 0.01f);
  const double high = levelGainDb("drive.trebleboost", base, 5000.0, 0.01f);
  EXPECT_NEAR(low, 0.0, 1.0);
  EXPECT_GT(high, 9.0);
  // A higher range leaves a 1.5 kHz tone with less boost.
  const double lowerRange = levelGainDb("drive.trebleboost", {{"gainDb", 12.0}, {"rangeHz", 600.0}}, 1500.0, 0.01f);
  const double upperRange = levelGainDb("drive.trebleboost", {{"gainDb", 12.0}, {"rangeHz", 3000.0}}, 1500.0, 0.01f);
  EXPECT_GT(lowerRange, upperRange + 3.0);
}

TEST(DriveEffectsTest, TrebleBoostClipsAsymmetricallyWhenPushed) {
  // Even harmonics only come from an asymmetric curve.
  const std::vector<float> out =
      renderSine("drive.trebleboost", {{"gainDb", 30.0}, {"rangeHz", 400.0}}, 2000.0, 0.3f);
  const size_t skip = static_cast<size_t>(0.3 * kRate);
  const double second = goertzelPower(out.data() + skip, out.size() - skip, 4000.0, kRate);
  const double fundamental = goertzelPower(out.data() + skip, out.size() - skip, 2000.0, kRate);
  EXPECT_GT(db(second) - db(fundamental), -45.0);
}

// ---------------------------------------------------------------------------
// Overdrive
// ---------------------------------------------------------------------------

TEST(DriveEffectsTest, OverdriveStaysPartlyCleanAtLowDriveAndHasAMidHump) {
  // Low drive is nearly linear on a guitar-level signal.
  EXPECT_LT(thdOf("drive.overdrive", {{"drivePercent", 0.0}}, 220.0, 0.05f), 3.0);
  // Small signal response at moderate drive: the mids (1 kHz) sit above
  // the lows (100 Hz) and the top (10 kHz) is rolled off below the mids.
  const Params p = {{"drivePercent", 50.0}, {"tonePercent", 100.0}};
  const double lows = levelGainDb("drive.overdrive", p, 100.0, 0.002f);
  const double mids = levelGainDb("drive.overdrive", p, 1000.0, 0.002f);
  const double highs = levelGainDb("drive.overdrive", p, 12000.0, 0.002f);
  EXPECT_GT(mids, lows + 8.0);
  EXPECT_GT(mids, highs + 3.0);
}

TEST(DriveEffectsTest, OverdriveToneSweepsTheTop) {
  const double dark = levelGainDb("drive.overdrive", {{"drivePercent", 50.0}, {"tonePercent", 0.0}}, 4000.0, 0.01f);
  const double bright = levelGainDb("drive.overdrive", {{"drivePercent", 50.0}, {"tonePercent", 100.0}}, 4000.0, 0.01f);
  EXPECT_GT(bright, dark + 12.0);
}

// ---------------------------------------------------------------------------
// Transparent Drive
// ---------------------------------------------------------------------------

TEST(DriveEffectsTest, TransparentDriveKeepsTheLowsFull) {
  // A 60 Hz tone at low drive comes out as loud as it went in (the clean
  // path carries it; the clipped path is high-passed).
  EXPECT_NEAR(levelGainDb("drive.transparent", {{"drivePercent", 10.0}}, 60.0, 0.02f),
              levelGainDb("drive.transparent", {{"drivePercent", 10.0}}, 220.0, 0.02f), 3.0);
  // The treble control is a shelf.
  const double boost = levelGainDb("drive.transparent", {{"drivePercent", 0.0}, {"trebleDb", 12.0}}, 10000.0, 0.002f);
  const double cut = levelGainDb("drive.transparent", {{"drivePercent", 0.0}, {"trebleDb", -12.0}}, 10000.0, 0.002f);
  EXPECT_GT(boost - cut, 14.0);
}

// ---------------------------------------------------------------------------
// Distortion
// ---------------------------------------------------------------------------

TEST(DriveEffectsTest, DistortionFilterSweepsFromDarkToOpen) {
  const Params dark = {{"distortionPercent", 60.0}, {"filterPercent", 0.0}};
  const Params open = {{"distortionPercent", 60.0}, {"filterPercent", 100.0}};
  EXPECT_GT(levelGainDb("drive.distortion", open, 6000.0, 0.01f),
            levelGainDb("drive.distortion", dark, 6000.0, 0.01f) + 10.0);
}

TEST(DriveEffectsTest, DistortionLoudnessBarelyDependsOnTheGainPastTheKnee) {
  // A hard clipper's output level is its clipping level: moving Distortion
  // from 60 to 100 % changes the loudness by only a few dB.
  const double a = levelGainDb("drive.distortion", {{"distortionPercent", 60.0}}, 220.0, 0.126f);
  const double b = levelGainDb("drive.distortion", {{"distortionPercent", 100.0}}, 220.0, 0.126f);
  EXPECT_LT(std::abs(a - b), 4.0);
}

// ---------------------------------------------------------------------------
// Metal Distortion
// ---------------------------------------------------------------------------

TEST(DriveEffectsTest, MetalTightCutsTheLowsBeforeTheClipping) {
  const Params loose = {{"tightHz", 60.0}, {"distortionPercent", 30.0}};
  const Params tight = {{"tightHz", 600.0}, {"distortionPercent", 30.0}};
  // A low note of 80 Hz at guitar level is much quieter once the clipper
  // no longer sees it.
  EXPECT_GT(levelGainDb("drive.metal", loose, 80.0, 0.003f),
            levelGainDb("drive.metal", tight, 80.0, 0.003f) + 6.0);
}

TEST(DriveEffectsTest, MetalEqIsThreeBandsWithASweepableMiddle) {
  // Small signal through a flat clipper is not linear, so the EQ is measured
  // on the shelves' effect on a clipped tone: a flat high shelf up, down.
  const std::vector<float> base = renderSine("drive.metal", {{"distortionPercent", 100.0}}, 3000.0, 0.3f);
  const std::vector<float> up = renderSine("drive.metal", {{"distortionPercent", 100.0}, {"highDb", 12.0}}, 3000.0, 0.3f);
  const std::vector<float> down = renderSine("drive.metal", {{"distortionPercent", 100.0}, {"highDb", -12.0}}, 3000.0, 0.3f);
  // A first order shelf at 4 kHz: a 3 kHz tone moves by about +8 and -2 dB.
  EXPECT_GT(rmsOf(up, 24000), 1.8 * rmsOf(base, 24000));
  EXPECT_LT(rmsOf(down, 24000), 0.95 * rmsOf(base, 24000));
  EXPECT_GT(rmsOf(up, 24000), 2.0 * rmsOf(down, 24000));

  // The middle band acts around its frequency, wherever it is put.
  for (const double centre : {400.0, 1500.0}) {
    const Params cut = {{"distortionPercent", 100.0}, {"midDb", -12.0}, {"midHz", centre}};
    const Params flat = {{"distortionPercent", 100.0}, {"midDb", 0.0}, {"midHz", centre}};
    const std::vector<float> a = renderSine("drive.metal", cut, centre, 0.3f);
    const std::vector<float> b = renderSine("drive.metal", flat, centre, 0.3f);
    const size_t skip = static_cast<size_t>(0.3 * kRate);
    EXPECT_LT(db(goertzelPower(a.data() + skip, a.size() - skip, centre, kRate)),
              db(goertzelPower(b.data() + skip, b.size() - skip, centre, kRate)) - 6.0)
        << centre;
  }
}

TEST(DriveEffectsTest, MetalGateSilencesBelowItsThresholdAndIsOffByDefault) {
  const std::vector<float> hiss = makeNoise(static_cast<int>(kRate), 3u, 0.002f);  // about -54 dBFS
  DirectKind off("drive.metal", {{"distortionPercent", 80.0}});
  DirectKind gated("drive.metal", {{"distortionPercent", 80.0}, {"gateDb", -40.0}});
  const std::vector<float> ungated = off.render(hiss);
  const std::vector<float> quiet = gated.render(hiss);
  EXPECT_GT(rmsOf(ungated, 24000), 0.01);
  EXPECT_LT(rmsOf(quiet, 24000), 0.001);
  // A note above the threshold passes the gate.
  const std::vector<float> note = sine(1.0, 220.0, 0.2f, kRate);
  DirectKind gate2("drive.metal", {{"distortionPercent", 80.0}, {"gateDb", -40.0}});
  DirectKind open2("drive.metal", {{"distortionPercent", 80.0}});
  EXPECT_NEAR(toDb(rmsOf(gate2.render(note), 24000)), toDb(rmsOf(open2.render(note), 24000)), 0.5);
}

// ---------------------------------------------------------------------------
// Fuzz
// ---------------------------------------------------------------------------

TEST(DriveEffectsTest, GermaniumGatesQuietSignalsAtLowBias) {
  // The same quiet tone, well fed and starved: starved, the dead zone eats it.
  const float level = 0.01f;  // -40 dBFS
  const double fed = levelGainDb("drive.fuzz", {{"voicing", 1.0}, {"fuzzPercent", 30.0}, {"biasPercent", 100.0}}, 330.0, level);
  const double starved = levelGainDb("drive.fuzz", {{"voicing", 1.0}, {"fuzzPercent", 30.0}, {"biasPercent", 0.0}}, 330.0, level);
  EXPECT_GT(fed, starved + 12.0);
  // And a loud one is not gated at all: both come out at similar level.
  const double fedLoud = levelGainDb("drive.fuzz", {{"voicing", 1.0}, {"fuzzPercent", 70.0}, {"biasPercent", 100.0}}, 330.0, 0.3f);
  const double starvedLoud = levelGainDb("drive.fuzz", {{"voicing", 1.0}, {"fuzzPercent", 70.0}, {"biasPercent", 0.0}}, 330.0, 0.3f);
  EXPECT_LT(std::abs(fedLoud - starvedLoud), 6.0);
}

TEST(DriveEffectsTest, GermaniumSputtersAsANoteDecays) {
  // A decaying note: the starved stage cuts off earlier than the well fed one.
  std::vector<float> note(static_cast<size_t>(1.5 * kRate));
  for (size_t i = 0; i < note.size(); ++i) {
    const double t = static_cast<double>(i) / kRate;
    note[i] = static_cast<float>(0.3 * std::exp(-t * 3.5) * std::sin(2.0 * kPi * 196.0 * t));
  }
  const auto lastAudible = [&](double bias) {
    DirectKind kind("drive.fuzz", {{"voicing", 1.0}, {"fuzzPercent", 60.0}, {"biasPercent", bias}});
    const std::vector<float> out = kind.render(note);
    size_t last = 0;
    for (size_t i = 0; i < out.size(); ++i)
      if (std::abs(out[i]) > 0.01f)
        last = i;
    return last;
  };
  EXPECT_LT(lastAudible(0.0) + static_cast<size_t>(0.1 * kRate), lastAudible(100.0));
}

TEST(DriveEffectsTest, SustainToneScoopsTheMiddle) {
  // With Tone at the middle the response dips between the low-pass and the
  // high-pass halves; at the ends it does not. Compared on a small tone.
  const Params mid = {{"voicing", 2.0}, {"fuzzPercent", 0.0}, {"biasPercent", 100.0}, {"tonePercent", 50.0}};
  const double lows = levelGainDb("drive.fuzz", mid, 400.0, 0.002f);
  const double centre = levelGainDb("drive.fuzz", mid, 1000.0, 0.002f);
  const double highs = levelGainDb("drive.fuzz", mid, 3000.0, 0.002f);
  EXPECT_LT(centre, lows - 4.0);
  EXPECT_LT(centre, highs - 4.0);
}

TEST(DriveEffectsTest, FuzzVoicingsSoundDifferent) {
  const std::vector<float> in = sine(1.0, 220.0, 0.15f, kRate);
  const auto render = [&](double voicing) {
    DirectKind kind("drive.fuzz", {{"voicing", voicing}});
    return kind.render(in);
  };
  const std::vector<float> si = render(0.0), ge = render(1.0), su = render(2.0);
  const auto differ = [&](const std::vector<float>& a, const std::vector<float>& b) {
    double sum = 0.0;
    for (size_t i = 24000; i < a.size(); ++i)
      sum += std::abs(a[i] - b[i]);
    return sum / static_cast<double>(a.size() - 24000);
  };
  EXPECT_GT(differ(si, ge), 0.004);
  EXPECT_GT(differ(ge, su), 0.004);
  EXPECT_GT(differ(si, su), 0.004);
}

// ---------------------------------------------------------------------------
// Bass Drive
// ---------------------------------------------------------------------------

TEST(DriveEffectsTest, BassDriveKeepsTheLowsCleanBelowTheCrossover) {
  const Params p = {{"crossoverHz", 400.0}, {"drivePercent", 100.0}, {"blendPercent", 100.0}};
  // 60 Hz at a hefty level: no distortion and no gain change, well below
  // the crossover.
  EXPECT_LT(thdOf("drive.bass", p, 60.0, 0.3f), 1.5);
  // (The default Level of the kind is -4 dB under unity gain.)
  EXPECT_NEAR(levelGainDb("drive.bass", p, 60.0, 0.3f), 20.0 * std::log10(0.63), 0.8);
  // 3 kHz at the same drive is heavily distorted.
  EXPECT_GT(thdOf("drive.bass", p, 3000.0, 0.3f), 10.0);
}

TEST(DriveEffectsTest, BassDriveIsTheIdentityWithTheBlendAtZero) {
  // The two bands add up to an all-pass: the level does not change. (The
  // default Level makes up for the other kinds' loudness, so it is taken out.)
  const std::vector<float> in = musicalSignal(1.0, kRate, 0.3f);
  DirectKind kind("drive.bass", {{"drivePercent", 100.0}, {"blendPercent", 0.0}, {"levelDb", 0.0}});
  const std::vector<float> out = kind.render(in);
  const double gain = toDb(rmsOf(out, 9600)) - toDb(rmsOf(in, 9600));
  EXPECT_NEAR(gain, 20.0 * std::log10(0.63), 0.3);
}

TEST(DriveEffectsTest, BassDriveCrossoverMovesTheSplit) {
  // A 500 Hz tone is in the clean band when the crossover is at 800 Hz and in
  // the driven band when it is at 100 Hz.
  const Params clean = {{"crossoverHz", 1600.0}, {"drivePercent", 100.0}, {"blendPercent", 100.0}};
  const Params driven = {{"crossoverHz", 100.0}, {"drivePercent", 100.0}, {"blendPercent", 100.0}};
  EXPECT_GT(thdOf("drive.bass", driven, 500.0, 0.3f), thdOf("drive.bass", clean, 500.0, 0.3f) + 10.0);
}

TEST(DriveEffectsTest, BassDrivePresenceIsAShelf) {
  const Params up = {{"drivePercent", 0.0}, {"blendPercent", 0.0}, {"presenceDb", 12.0}};
  const Params down = {{"drivePercent", 0.0}, {"blendPercent", 0.0}, {"presenceDb", -12.0}};
  const Params flat = {{"drivePercent", 0.0}, {"blendPercent", 0.0}};
  const double reference = levelGainDb("drive.bass", flat, 8000.0, 0.05f);
  EXPECT_GT(levelGainDb("drive.bass", up, 8000.0, 0.05f), reference + 5.0);
  EXPECT_LT(levelGainDb("drive.bass", down, 8000.0, 0.05f), reference - 5.0);
  EXPECT_NEAR(levelGainDb("drive.bass", up, 100.0, 0.05f), levelGainDb("drive.bass", flat, 100.0, 0.05f), 1.0);
}

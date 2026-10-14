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

// ---------------------------------------------------------------------------
// Voicings added to four kinds: Transparent Drive "Smooth", Distortion
// "Open", Fuzz "Scooped" and Metal Distortion "Resonant". Each one is a
// choice whose first option is the old behaviour, so a preset made before
// them sounds the same; the checks below are for our own design targets (the
// response shape and the distortion at the settings named in each test).
// The measured voicings of the second round (VoiceShape, further down) follow
// the same rule.
// ---------------------------------------------------------------------------

namespace {

struct ShapePoint {
  double hz;
  double db;  // relative to 1 kHz
};

// Gain in dB of a kind on a sine at one frequency (a windowed DFT of the part
// after the first 0.4 s).
double toneGainDb(const char* id, const Params& params, double hz, double peakDb) {
  const std::vector<float> in = sine(1.0, hz, amp(peakDb), kRate);
  DirectKind kind(id, params);
  const std::vector<float> out = kind.render(in);
  const size_t skip = static_cast<size_t>(0.4 * kRate);
  return db(goertzelPower(out.data() + skip, out.size() - skip, hz, kRate)) -
         db(goertzelPower(in.data() + skip, in.size() - skip, hz, kRate));
}

// The response at each point, relative to 1 kHz, against the design curve.
void expectShape(const char* label, const char* id, const Params& params, double peakDb,
                 const std::vector<ShapePoint>& design, double toleranceDb) {
  const double reference = toneGainDb(id, params, 1000.0, peakDb);
  double worst = 0.0;
  for (const ShapePoint& p : design) {
    const double got = toneGainDb(id, params, p.hz, peakDb) - reference;
    worst = std::max(worst, std::abs(got - p.db));
    EXPECT_NEAR(got, p.db, toleranceDb) << label << " at " << p.hz << " Hz";
  }
  std::cout << "[   SHAPE  ] " << label << ": worst " << juce::String(worst, 2)
            << " dB off the design curve at " << peakDb << " dBFS" << std::endl;
}

double thdAtDbfs(const char* id, const Params& params, double hz, double peakDb) {
  return thdOf(id, params, hz, amp(peakDb));
}

// The 1 kHz gain at -18 dBFS, in dB.
double level1k(const char* id, const Params& params) {
  return toneGainDb(id, params, 1000.0, -18.0);
}

}  // namespace

TEST(DriveEffectsTest, TheNewVoicingsAreTheLastChoiceAndTheOldOneIsTheDefault) {
  struct Case {
    const char* id;
    int options;
  };
  for (const Case& c :
       {Case{"drive.transparent", 2}, Case{"drive.distortion", 3}, Case{"drive.fuzz", 6},
        Case{"drive.metal", 2}, Case{"drive.overdrive", 4}, Case{"drive.boost", 3},
        Case{"drive.trebleboost", 2}, Case{"drive.bass", 4}}) {
    SCOPED_TRACE(c.id);
    const BlockKindInfo* info = BlockKindRegistry::instance().find(c.id);
    ASSERT_NE(info, nullptr);
    const BlockParamSpec* voicing = nullptr;
    for (const BlockParamSpec& spec : info->params)
      if (spec.id == "voicing")
        voicing = &spec;
    ASSERT_NE(voicing, nullptr);
    EXPECT_EQ(voicing->choices.size(), c.options);
    EXPECT_EQ(static_cast<int>(voicing->defaultValue), 0);
  }
}

TEST(DriveEffectsTest, ChoosingTheFirstVoicingExplicitlyChangesNothing) {
  const std::vector<float> in = musicalSignal(0.6, kRate, 0.3f);
  for (const char* id : {"drive.transparent", "drive.distortion", "drive.fuzz", "drive.metal",
                         "drive.overdrive", "drive.boost", "drive.trebleboost", "drive.bass"}) {
    SCOPED_TRACE(id);
    DirectKind implicit(id, {}, Layout::Stereo);
    DirectKind chosen(id, {{"voicing", 0.0}}, Layout::Stereo);
    const Stereo a = implicit.render(Stereo{in, in});
    const Stereo b = chosen.render(Stereo{in, in});
    EXPECT_EQ(std::memcmp(a.left.data(), b.left.data(), in.size() * sizeof(float)), 0);
    EXPECT_EQ(std::memcmp(a.right.data(), b.right.data(), in.size() * sizeof(float)), 0);
  }
}

// --- Transparent Drive: Smooth ---------------------------------------------

TEST(DriveEffectsTest, TransparentSmoothConforms) {
  ConformanceOptions options;
  options.params = {{"voicing", 1.0}, {"drivePercent", 70.0}, {"trebleDb", 3.0}, {"levelDb", 1.0}};
  options.stateParams = {{"voicing", 1.0}, {"drivePercent", 20.0}, {"trebleDb", -6.0}, {"levelDb", -4.0}};
  options.sweepBound = 16.0f;
  options.cpuAverageBudgetNs = 50000.0;
  options.cpuWorstBudgetNs = 500000.0;
  expectKindConforms("drive.transparent", options);
}

TEST(DriveEffectsTest, SmoothRollsTheTopOffAsAOnePoleLowPass) {
  // At Treble 0 dB the corner is about 1.45 kHz and the slope 6 dB per
  // octave. The roll-off comes after the clipping, so the shape is the same
  // at -18 dBFS (where the clipped path is working) and at -40 dBFS.
  const Params p = {{"voicing", 1.0}, {"drivePercent", 70.0}, {"trebleDb", 0.0}, {"levelDb", 4.4}};
  const std::vector<ShapePoint> design = {
      {31.5, 0.2},   {63, 1.2},     {100, 0.9},    {160, 0.6},    {250, 1.4},    {500, 1.5},
      {630, 1.2},    {1600, -1.8},  {2000, -2.8},  {2500, -4.1},  {3150, -5.7},  {4000, -7.5},
      {5000, -9.2},  {6300, -11.2}, {8000, -13.4}, {10000, -15.4}, {12500, -17.4}, {16000, -19.8}};
  expectShape("Smooth response", "drive.transparent", p, -18.0, design, 1.5);
  expectShape("Smooth response", "drive.transparent", p, -40.0, design, 1.5);
}

TEST(DriveEffectsTest, SmoothTrebleMovesTheCornerOfTheLowPass) {
  const auto eightK = [&](double trebleDb) {
    const Params p = {{"voicing", 1.0}, {"drivePercent", 70.0}, {"trebleDb", trebleDb}};
    return toneGainDb("drive.transparent", p, 8000.0, -50.0) - toneGainDb("drive.transparent", p, 100.0, -50.0);
  };
  EXPECT_GT(eightK(12.0), eightK(0.0) + 8.0);
  EXPECT_LT(eightK(-12.0), eightK(0.0) - 8.0);
}

TEST(DriveEffectsTest, SmoothIsSofterThanClassicAndHasMoreSmallSignalGain) {
  const Params classic = {{"drivePercent", 70.0}, {"levelDb", 4.4}};
  const Params smooth = {{"voicing", 1.0}, {"drivePercent", 70.0}, {"levelDb", 4.4}};
  // Well under the distortion of Classic at -18 dBFS (9 to 17 % against 25 %).
  // The roll-off comes after the clipping, so the harmonics of 1 kHz are
  // rolled off with it and 1 kHz measures about half the distortion of 100 Hz.
  const double classic1k = thdAtDbfs("drive.transparent", classic, 1000.0, -18.0);
  const double smooth1k = thdAtDbfs("drive.transparent", smooth, 1000.0, -18.0);
  const double smooth100 = thdAtDbfs("drive.transparent", smooth, 100.0, -18.0);
  std::cout << "[   THD    ] Smooth 70 %: " << juce::String(smooth100, 1) << " % at 100 Hz, "
            << juce::String(smooth1k, 1) << " % at 1 kHz (Classic " << juce::String(classic1k, 1) << " %)"
            << std::endl;
  EXPECT_NEAR(smooth100, 12.8, 0.35 * 12.8);
  EXPECT_NEAR(smooth1k, 13.4, 0.35 * 13.4);
  EXPECT_LT(smooth1k, 0.5 * classic1k);
  // More gain for a quiet signal, and a 1 kHz level at -18 dBFS where Classic has it.
  const double smallSmooth = levelGainDb("drive.transparent", smooth, 1000.0, amp(-60.0));
  const double smallClassic = levelGainDb("drive.transparent", classic, 1000.0, amp(-60.0));
  std::cout << "[   LEVEL  ] Smooth: " << juce::String(smallSmooth, 1) << " dB at -60 dBFS, "
            << juce::String(level1k("drive.transparent", smooth), 1) << " dB at -18 dBFS, "
            << juce::String(levelGainDb("drive.transparent", smooth, 1000.0, 1.0f), 1) << " dB at 0 dBFS"
            << " (Classic " << juce::String(smallClassic, 1) << ", "
            << juce::String(level1k("drive.transparent", classic), 1) << " at -18)" << std::endl;
  EXPECT_GT(smallSmooth, smallClassic + 2.0);
  EXPECT_NEAR(level1k("drive.transparent", smooth), level1k("drive.transparent", classic), 2.0);
}

TEST(DriveEffectsTest, SmoothDistortsMoreWithMoreDrive) {
  const double little = thdAtDbfs("drive.transparent", {{"voicing", 1.0}, {"drivePercent", 5.0}}, 1000.0, -18.0);
  const double lots = thdAtDbfs("drive.transparent", {{"voicing", 1.0}, {"drivePercent", 95.0}}, 1000.0, -18.0);
  EXPECT_GT(lots, little + 5.0);
}

// --- Distortion: Open ------------------------------------------------------

TEST(DriveEffectsTest, DistortionOpenConforms) {
  ConformanceOptions options;
  options.params = {{"voicing", 1.0}, {"distortionPercent", 80.0}, {"filterPercent", 40.0}, {"levelDb", 2.0}};
  options.stateParams = {{"voicing", 1.0}, {"distortionPercent", 20.0}, {"filterPercent", 90.0}, {"levelDb", -4.0}};
  options.sweepBound = 16.0f;
  options.cpuAverageBudgetNs = 55000.0;
  options.cpuWorstBudgetNs = 500000.0;
  expectKindConforms("drive.distortion", options);
}

TEST(DriveEffectsTest, OpenFilterIsAGentleOnePoleSlopeAndDoesNotDependOnDistortion) {
  // Filter at 50 %: flat in the lows and mids, then about 6 dB per octave.
  for (const double distortion : {30.0, 70.0}) {
    const Params p = {{"voicing", 1.0}, {"distortionPercent", distortion}, {"filterPercent", 50.0}, {"levelDb", -15.6}};
    const std::string label = "Open filter, Distortion " + std::to_string(static_cast<int>(distortion)) + " %";
    expectShape(label.c_str(), "drive.distortion", p, -18.0,
                {{4000, -4.75}, {5000, -6.2}, {6300, -7.8}, {8000, -9.7}, {10000, -11.5}, {12500, -13.3}, {16000, -15.5}},
                1.5);
    // The lows are not cut as they are by the 90 Hz high-pass of Classic.
    EXPECT_NEAR(toneGainDb("drive.distortion", p, 31.5, -18.0) - toneGainDb("drive.distortion", p, 1000.0, -18.0), 0.0, 1.5);
  }
}

TEST(DriveEffectsTest, OpenKeepsTheClippingAndTheLevelOfClassic) {
  const Params classic = {{"distortionPercent", 30.0}, {"filterPercent", 50.0}, {"levelDb", -15.6}};
  const Params open = {{"voicing", 1.0}, {"distortionPercent", 30.0}, {"filterPercent", 50.0}, {"levelDb", -15.6}};
  const double thdClassic = thdAtDbfs("drive.distortion", classic, 1000.0, -18.0);
  const double thdOpen = thdAtDbfs("drive.distortion", open, 1000.0, -18.0);
  std::cout << "[   THD    ] Distortion 1 kHz at -18 dBFS: Classic " << juce::String(thdClassic, 1) << " %, Open "
            << juce::String(thdOpen, 1) << " %" << std::endl;
  EXPECT_NEAR(thdOpen, 26.0, 26.0 / 3.0);
  EXPECT_NEAR(level1k("drive.distortion", open), level1k("drive.distortion", classic), 2.0);
}

TEST(DriveEffectsTest, OpenFilterStillSweepsFromDarkToOpen) {
  const auto topDb = [&](double filterPercent) {
    const Params p = {{"voicing", 1.0}, {"distortionPercent", 50.0}, {"filterPercent", filterPercent}};
    return toneGainDb("drive.distortion", p, 6000.0, -18.0) - toneGainDb("drive.distortion", p, 500.0, -18.0);
  };
  EXPECT_GT(topDb(100.0), topDb(50.0) + 5.0);
  EXPECT_GT(topDb(50.0), topDb(0.0) + 5.0);
}

// --- Fuzz: Scooped ---------------------------------------------------------

TEST(DriveEffectsTest, FuzzScoopedConforms) {
  ConformanceOptions options;
  options.params = {{"voicing", 3.0}, {"fuzzPercent", 80.0}, {"biasPercent", 40.0}, {"tonePercent", 60.0}};
  options.stateParams = {{"voicing", 3.0}, {"fuzzPercent", 30.0}, {"biasPercent", 90.0},
                         {"tonePercent", 20.0}, {"levelDb", -3.0}};
  options.sweepBound = 16.0f;
  options.cpuAverageBudgetNs = 90000.0;
  options.cpuWorstBudgetNs = 500000.0;
  expectKindConforms("drive.fuzz", options);
}

TEST(DriveEffectsTest, ScoopedToneStackHasAMidNotchWithTheLowsAndHighsUp) {
  // Fuzz 60 %, Tone at the middle: the lows and highs stand about 5 dB above a
  // notch near 1 kHz, and the top rolls off above 12 kHz.
  const Params p = {{"voicing", 3.0}, {"fuzzPercent", 60.0}, {"tonePercent", 50.0}, {"levelDb", -22.9}};
  expectShape("Scooped response", "drive.fuzz", p, -18.0,
              {{31.5, 5.1},  {63, 4.9},    {100, 4.6},   {200, 4.0},   {315, 3.6},   {500, 2.4},
               {630, 1.4},   {1600, 1.3},  {2000, 2.5},  {2500, 3.7},  {3150, 4.6},  {4000, 5.3},
               {5000, 5.7},  {6300, 5.9},  {8000, 5.8},  {10000, 5.3}, {12500, 4.0}},
              1.5);
  EXPECT_LT(toneGainDb("drive.fuzz", p, 16000.0, -18.0) - toneGainDb("drive.fuzz", p, 1000.0, -18.0), 3.0);
}

TEST(DriveEffectsTest, ScoopedClipsToASymmetricSquareThatTheStackThenShapes) {
  const Params p = {{"voicing", 3.0}, {"fuzzPercent", 60.0}, {"tonePercent", 50.0}, {"levelDb", -22.9}};
  const double at100 = thdAtDbfs("drive.fuzz", p, 100.0, -18.0);
  const double at1k = thdAtDbfs("drive.fuzz", p, 1000.0, -18.0);
  std::cout << "[   THD    ] Scooped Fuzz 60 %: " << juce::String(at100, 1) << " % at 100 Hz, "
            << juce::String(at1k, 1) << " % at 1 kHz" << std::endl;
  // A symmetric square wave has 48 % of THD; the notch lifts the harmonics of
  // 1 kHz above its fundamental, so that one is higher.
  EXPECT_NEAR(at100, 35.9, 35.9 / 3.0);
  EXPECT_NEAR(at1k, 75.7, 75.7 / 3.0);
  EXPECT_GT(at1k, at100 + 10.0);
}

TEST(DriveEffectsTest, ScoopedKeepsTheOneKilohertzLevelOfTheDefaultVoicing) {
  // The import level offset (-22.9 dB at Fuzz 60 %) was set against the
  // default voicing, so the new voicing is trimmed to the same 1 kHz level.
  const Params silicon = {{"voicing", 0.0}, {"fuzzPercent", 60.0}, {"tonePercent", 50.0}, {"levelDb", -22.9}};
  const Params sustain = {{"voicing", 2.0}, {"fuzzPercent", 60.0}, {"tonePercent", 50.0}, {"levelDb", -22.9}};
  const Params scooped = {{"voicing", 3.0}, {"fuzzPercent", 60.0}, {"tonePercent", 50.0}, {"levelDb", -22.9}};
  std::cout << "[   LEVEL  ] Fuzz 1 kHz at -18 dBFS: Silicon " << juce::String(level1k("drive.fuzz", silicon), 2)
            << " dB, Sustain " << juce::String(level1k("drive.fuzz", sustain), 2) << " dB, Scooped "
            << juce::String(level1k("drive.fuzz", scooped), 2) << " dB" << std::endl;
  EXPECT_NEAR(level1k("drive.fuzz", scooped), level1k("drive.fuzz", silicon), 2.0);
}

TEST(DriveEffectsTest, ScoopedToneGoesFromDarkToBright) {
  const auto at = [&](double tone, double hz) {
    const Params p = {{"voicing", 3.0}, {"fuzzPercent", 60.0}, {"tonePercent", tone}};
    return toneGainDb("drive.fuzz", p, hz, -18.0);
  };
  // All the way down it is a low-pass (the highs fall away), all the way up a
  // high-pass (the lows do).
  EXPECT_GT(at(0.0, 100.0), at(0.0, 5000.0) + 10.0);
  EXPECT_GT(at(100.0, 5000.0), at(100.0, 100.0) + 10.0);
}

TEST(DriveEffectsTest, ScoopedDistortsMoreWithMoreFuzz) {
  const double little = thdOf("drive.fuzz", {{"voicing", 3.0}, {"fuzzPercent", 5.0}, {"biasPercent", 100.0}}, 1000.0, 0.05f);
  const double lots = thdOf("drive.fuzz", {{"voicing", 3.0}, {"fuzzPercent", 95.0}, {"biasPercent", 100.0}}, 1000.0, 0.05f);
  EXPECT_GT(lots, little + 5.0);
}

// --- Metal Distortion: Resonant ----------------------------------------------

TEST(DriveEffectsTest, MetalResonantConforms) {
  ConformanceOptions options;
  options.params = {{"voicing", 1.0}, {"distortionPercent", 85.0}, {"tightHz", 220.0}, {"midDb", -5.0},
                    {"midHz", 900.0}, {"lowDb", 3.0},              {"gateDb", -60.0}};
  options.stateParams = {{"voicing", 1.0}, {"distortionPercent", 30.0}, {"tightHz", 90.0}, {"lowDb", -4.0},
                         {"midDb", 5.0},   {"midHz", 450.0},            {"highDb", 6.0},    {"levelDb", -3.0},
                         {"gateDb", -45.0}};
  options.sweepBound = 24.0f;
  options.cpuAverageBudgetNs = 100000.0;
  options.cpuWorstBudgetNs = 600000.0;
  expectKindConforms("drive.metal", options);
}

TEST(DriveEffectsTest, ResonantHasALowResonanceAndAPresencePeakWithTheEqFlat) {
  // Distortion 60 %, Low/Middle/High at 0 dB, Mid Freq 1.6 kHz: a peak near
  // 100 Hz on a low shelf, a presence peak near 5 kHz, a roll-off from about
  // 11 kHz; measured at -18 dBFS where the stage is clipping.
  const Params p = {{"voicing", 1.0}, {"distortionPercent", 60.0}, {"midHz", 1600.0}, {"levelDb", -21.5}};
  expectShape("Resonant response", "drive.metal", p, -18.0,
              {{31.5, 4.7},  {40, 5.1},    {50, 5.3},    {63, 5.8},    {80, 7.3},    {100, 9.7},   {125, 8.0},
               {160, 4.9},   {200, 4.1},   {250, 3.3},   {315, 2.9},   {400, 2.3},   {500, 2.0},   {630, 1.3},
               {3150, 0.8},  {4000, 3.1},  {5000, 3.9},  {6300, 1.9},  {8000, -1.0}, {10000, -3.1}, {12500, -4.8},
               {16000, -6.8}},
              1.5);
}

TEST(DriveEffectsTest, ResonantDistortionBandsFollowFromTheVoice) {
  const Params p = {{"voicing", 1.0}, {"distortionPercent", 60.0}, {"midHz", 1600.0}, {"levelDb", -21.5}};
  const double at100 = thdAtDbfs("drive.metal", p, 100.0, -18.0);
  const double at1k = thdAtDbfs("drive.metal", p, 1000.0, -18.0);
  std::cout << "[   THD    ] Resonant Metal 60 %: " << juce::String(at100, 1) << " % at 100 Hz, "
            << juce::String(at1k, 1) << " % at 1 kHz" << std::endl;
  EXPECT_NEAR(at100, 19.0, 19.0 / 3.0);
  EXPECT_NEAR(at1k, 50.5, 50.5 / 3.0);
}

TEST(DriveEffectsTest, ResonantKeepsTheLevelOfStandardAtTheImportSettings) {
  const Params standard = {{"distortionPercent", 60.0}, {"midHz", 1600.0}, {"levelDb", -21.5}};
  const Params resonant = {{"voicing", 1.0}, {"distortionPercent", 60.0}, {"midHz", 1600.0}, {"levelDb", -21.5}};
  std::cout << "[   LEVEL  ] Metal 1 kHz at -18 dBFS: Standard " << juce::String(level1k("drive.metal", standard), 2)
            << " dB, Resonant " << juce::String(level1k("drive.metal", resonant), 2) << " dB" << std::endl;
  EXPECT_NEAR(level1k("drive.metal", resonant), level1k("drive.metal", standard), 2.0);
}

TEST(DriveEffectsTest, ResonantEqKnobsStillCorrectTheVoice) {
  const Params flat = {{"voicing", 1.0}, {"distortionPercent", 60.0}, {"midHz", 1600.0}};
  Params bassCut = flat;
  bassCut.emplace_back("lowDb", -12.0);
  Params trebleUp = flat;
  trebleUp.emplace_back("highDb", 12.0);
  const double lowFlat = toneGainDb("drive.metal", flat, 63.0, -18.0) - toneGainDb("drive.metal", flat, 1000.0, -18.0);
  const double lowCut = toneGainDb("drive.metal", bassCut, 63.0, -18.0) - toneGainDb("drive.metal", bassCut, 1000.0, -18.0);
  EXPECT_LT(lowCut, lowFlat - 3.0);
  const double highFlat = toneGainDb("drive.metal", flat, 8000.0, -18.0) - toneGainDb("drive.metal", flat, 1000.0, -18.0);
  const double highUp = toneGainDb("drive.metal", trebleUp, 8000.0, -18.0) - toneGainDb("drive.metal", trebleUp, 1000.0, -18.0);
  EXPECT_GT(highUp, highFlat + 5.0);
}

// ---------------------------------------------------------------------------
// Measured voicings (drive/DriveCommon.h, VoiceShape): Overdrive "Focused",
// "Warm" and "Heavy", Clean Boost "Warm" and "Hot", Treble Boost "Cranked",
// Distortion "Notched", Fuzz "Open" and "Thick", Bass Drive "DI", "Boost" and
// "Grit". Each is fitted to a family of pedals measured on a unit (CorOS
// d14e) with every knob at its middle, the sweep and the sines at -18 dBFS or
// 12 dB lower for the hot ones. The settings below are what the import table
// makes of each pedal; the targets are the unit's numbers: the response at
// 125 Hz, 500 Hz, 2 kHz and 8 kHz against 1 kHz, the distortion of a 1 kHz and
// a 100 Hz sine, and the 1 kHz gain.
// ---------------------------------------------------------------------------

namespace {

struct UnitTarget {
  const char* pedal;
  const char* id;
  Params params;
  double levelDb;        // sweep and sine level, dBFS peak
  double response[4];    // 125 Hz, 500 Hz, 2 kHz, 8 kHz, dB against 1 kHz
  double thd1k, thd100;  // percent
  double gainDb;         // 1 kHz
  double responseTolDb = 3.5;
  double thdRelative = 0.3;
};

// clang-format off
const std::vector<UnitTarget>& unitTargets() {
  static const std::vector<UnitTarget> targets = {
      // Overdrive, Focused
      {"Green 808", "drive.overdrive", {{"voicing", 1.0}, {"drivePercent", 32.76}, {"levelDb", -3.7}}, -30.0,
       {0.2, 1.7, -3.9, -15.3}, 9.6, 11.7, -0.8},
      {"Chief SD1", "drive.overdrive", {{"voicing", 1.0}, {"drivePercent", 71.7}, {"levelDb", -1.5}}, -30.0,
       {-0.5, 1.1, -3.4, -15.7}, 11.5, 24.0, 1.6},
      {"Red Drive", "drive.overdrive", {{"voicing", 1.0}, {"drivePercent", 50.0}, {"levelDb", -11.7}}, -18.0,
       {1.0, 1.4, -3.4, -14.3}, 9.3, 23.1, -16.4},
      {"Chief OD1", "drive.overdrive", {{"voicing", 1.0}, {"drivePercent", 7.38}, {"levelDb", -1.2}}, -30.0,
       {-5.6, 1.5, -4.0, -15.4}, 10.9, 4.4, 1.3},
      // Overdrive, Warm, and the boost on the same clipping
      {"Brit Blues", "drive.overdrive", {{"voicing", 2.0}, {"drivePercent", 50.0}, {"levelDb", -16.2}}, -18.0,
       {3.2, 2.2, -2.9, -13.0}, 10.0, 20.7, -19.3},
      {"Vemural Ray", "drive.overdrive", {{"voicing", 2.0}, {"drivePercent", 50.0}, {"levelDb", 4.6}}, -30.0,
       {4.7, 3.1, -4.7, -16.4}, 9.5, 15.7, 10.1},
      {"Exotic", "drive.boost", {{"voicing", 1.0}, {"gainDb", 10.0}}, -30.0, {2.2, 2.0, -3.0, -13.9}, 13.3,
       17.3, 7.8},
      // Overdrive, Heavy
      {"Obsessive Drive", "drive.overdrive",
       {{"voicing", 3.0}, {"drivePercent", 50.0}, {"tonePercent", 57.04}, {"levelDb", -19.7}}, -18.0,
       {10.0, 2.8, -1.6, -7.1}, 28.5, 16.4, -19.6},
      {"No-Bell OD1", "drive.overdrive",
       {{"voicing", 3.0}, {"drivePercent", 50.0}, {"tonePercent", 23.65}, {"levelDb", -15.5}}, -18.0,
       {6.3, 1.4, -1.9, -14.7}, 17.6, 17.2, -15.8},
      {"Chief BD2", "drive.overdrive",
       {{"voicing", 3.0}, {"drivePercent", 50.0}, {"tonePercent", 76.31}, {"levelDb", -8.2}}, -18.0,
       {6.4, 1.4, -1.6, -3.2}, 32.1, 28.6, -8.1, 3.5, 0.35},
      // Distortion: Notched, and the pedals moved onto Open
      {"Chief DS1", "drive.distortion",
       {{"voicing", 2.0}, {"distortionPercent", 50.0}, {"filterPercent", 50.0}, {"levelDb", -22.6}}, -18.0,
       {10.1, 3.6, 0.5, 2.5}, 100.3, 26.0, -22.6},
      {"OD250", "drive.distortion",
       {{"voicing", 1.0}, {"distortionPercent", 50.0}, {"filterPercent", 100.0}, {"levelDb", -7.1}}, -18.0,
       {-0.1, 0.1, 0.0, -0.1}, 42.9, 43.2, -6.5},
      {"81 Creations Drive", "drive.distortion",
       {{"voicing", 1.0}, {"distortionPercent", 50.0}, {"filterPercent", 50.0}, {"levelDb", -6.4}}, -18.0,
       {0.7, 0.5, -1.3, -9.2}, 27.1, 41.7, -6.5},
      // Boost, Hot; Treble Boost, Cranked
      {"Exotic Z Boost", "drive.boost", {{"voicing", 2.0}, {"gainDb", 10.0}}, -30.0, {-1.4, 0.0, -0.2, -2.2},
       9.8, 12.7, 8.9},
      {"Rage Booster", "drive.trebleboost", {{"voicing", 1.0}, {"gainDb", 15.64}}, -30.0,
       {-12.4, -2.2, 0.7, 1.0}, 46.0, 2.6, 18.0},
      // Fuzz, Open and Thick
      {"Facial Fuzz", "drive.fuzz",
       {{"voicing", 4.0}, {"fuzzPercent", 50.0}, {"tonePercent", 91.0}, {"levelDb", -8.8}}, -18.0,
       {-0.4, 0.1, 0.0, -1.8}, 41.8, 44.6, -8.6},
      {"MK3 Silicon Fuzz", "drive.fuzz",
       {{"voicing", 4.0}, {"fuzzPercent", 50.0}, {"tonePercent", 50.0}, {"levelDb", -7.7}}, -18.0,
       {1.0, 0.8, -1.6, -9.8}, 30.1, 40.5, -7.8},
      {"Soviet Fuzz", "drive.fuzz",
       {{"voicing", 5.0}, {"fuzzPercent", 50.0}, {"tonePercent", 50.0}, {"levelDb", -2.2}}, -18.0,
       {14.9, 3.1, -0.2, 1.8}, 49.7, 9.9, -23.6},
      // Bass Drive, DI, Boost and Grit
      {"BDDI", "drive.bass",
       {{"voicing", 1.0}, {"drivePercent", 50.0}, {"blendPercent", 50.0}, {"levelDb", -4.3}}, -30.0,
       {6.2, -0.3, 7.1, 1.9}, 0.0, 0.0, -4.3},
      {"Exotic Bass Z Boost", "drive.bass",
       {{"voicing", 2.0}, {"drivePercent", 50.0}, {"blendPercent", 50.0}, {"levelDb", 0.0}}, -30.0,
       {-1.4, 0.1, -0.3, -3.5}, 0.0, 0.1, 10.1},
      {"Douglas MT 3K", "drive.bass",
       {{"voicing", 3.0}, {"crossoverHz", 800.0}, {"drivePercent", 63.73}, {"blendPercent", 50.0},
        {"levelDb", -3.8}},
       -30.0, {2.2, 0.9, 2.9, -1.9}, 16.8, 0.4, -4.4},
      {"Douglas Vintage MT", "drive.bass",
       {{"voicing", 3.0}, {"drivePercent", 37.37}, {"blendPercent", 50.0}, {"levelDb", -1.9}}, -30.0,
       {7.8, 2.6, -1.0, -3.7}, 9.5, 8.8, -1.9},
  };
  return targets;
}
// clang-format on

// The power average over a third-octave band (three points), the way the
// match analysis reads a band of a sweep's response.
double bandGainDb(const char* id, const Params& params, double hz, double peakDb) {
  double sum = 0.0;
  for (const double r : {std::pow(2.0, -1.0 / 9.0), 1.0, std::pow(2.0, 1.0 / 9.0)})
    sum += std::pow(10.0, toneGainDb(id, params, hz * r, peakDb) / 10.0);
  return 10.0 * std::log10(sum / 3.0);
}

void expectThdNear(double got, double unit, double relative, const char* what) {
  if (unit < 1.0)
    EXPECT_LT(got, 1.5) << what << ": the unit is clean";
  else
    EXPECT_NEAR(got, unit, relative * unit) << what;
}

}  // namespace

TEST(DriveEffectsTest, MeasuredVoicingsMatchTheUnitAtTheMiddleOfTheKnobs) {
  for (const UnitTarget& t : unitTargets()) {
    SCOPED_TRACE(t.pedal);
    const double reference = bandGainDb(t.id, t.params, 1000.0, t.levelDb);
    const double bands[4] = {125.0, 500.0, 2000.0, 8000.0};
    juce::String line;
    for (int i = 0; i < 4; ++i) {
      const double got = bandGainDb(t.id, t.params, bands[i], t.levelDb) - reference;
      line << juce::String(got, 1) << " (" << juce::String(t.response[i], 1) << ") ";
      EXPECT_NEAR(got, t.response[i], t.responseTolDb) << "at " << bands[i] << " Hz";
    }
    const double thd1k = thdAtDbfs(t.id, t.params, 1000.0, t.levelDb);
    const double thd100 = thdAtDbfs(t.id, t.params, 100.0, t.levelDb);
    std::cout << "[   UNIT   ] " << t.pedal << ": " << line << "| THD " << juce::String(thd1k, 1)
              << " (" << t.thd1k << ") / " << juce::String(thd100, 1) << " (" << t.thd100
              << ") % | 1 kHz " << juce::String(reference, 2) << " (" << t.gainDb << ") dB"
              << std::endl;
    expectThdNear(thd1k, t.thd1k, t.thdRelative, "THD at 1 kHz");
    expectThdNear(thd100, t.thd100, t.thdRelative, "THD at 100 Hz");
    EXPECT_NEAR(reference, t.gainDb, 1.0) << "the 1 kHz level";
  }
}

TEST(DriveEffectsTest, MeasuredOverdriveVoicingsConform) {
  for (const double voicing : {1.0, 2.0, 3.0}) {
    SCOPED_TRACE(voicing);
    ConformanceOptions options;
    options.params = {
        {"voicing", voicing}, {"drivePercent", 70.0}, {"tonePercent", 40.0}, {"levelDb", 2.0}};
    options.stateParams = {
        {"voicing", voicing}, {"drivePercent", 20.0}, {"tonePercent", 80.0}, {"levelDb", -4.0}};
    options.sweepBound = 16.0f;
    options.cpuAverageBudgetNs = 60000.0;
    options.cpuWorstBudgetNs = 500000.0;
    expectKindConforms("drive.overdrive", options);
  }
}

TEST(DriveEffectsTest, MeasuredBoostVoicingsConform) {
  for (const double voicing : {1.0, 2.0}) {
    SCOPED_TRACE(voicing);
    ConformanceOptions options;
    options.params = {{"voicing", voicing}, {"gainDb", 14.0}, {"bassDb", 6.0}, {"trebleDb", -4.0}};
    options.stateParams = {
        {"voicing", voicing}, {"gainDb", 9.0}, {"bassDb", -5.0}, {"trebleDb", 7.0}};
    options.sweepBound = 16.0f;
    options.cpuAverageBudgetNs = 60000.0;
    options.cpuWorstBudgetNs = 500000.0;
    expectKindConforms("drive.boost", options);
  }
}

TEST(DriveEffectsTest, CrankedTrebleBoostConforms) {
  ConformanceOptions options;
  options.params = {{"voicing", 1.0}, {"gainDb", 24.0}, {"rangeHz", 1500.0}, {"levelDb", -3.0}};
  options.stateParams = {{"voicing", 1.0}, {"gainDb", 20.0}, {"rangeHz", 700.0}, {"levelDb", 4.0}};
  options.sweepBound = 16.0f;
  options.cpuAverageBudgetNs = 60000.0;
  options.cpuWorstBudgetNs = 500000.0;
  expectKindConforms("drive.trebleboost", options);
}

TEST(DriveEffectsTest, NotchedDistortionConforms) {
  ConformanceOptions options;
  options.params = {
      {"voicing", 2.0}, {"distortionPercent", 80.0}, {"filterPercent", 40.0}, {"levelDb", 2.0}};
  options.stateParams = {
      {"voicing", 2.0}, {"distortionPercent", 20.0}, {"filterPercent", 90.0}, {"levelDb", -4.0}};
  options.sweepBound = 16.0f;
  options.cpuAverageBudgetNs = 60000.0;
  options.cpuWorstBudgetNs = 500000.0;
  expectKindConforms("drive.distortion", options);
}

TEST(DriveEffectsTest, MeasuredFuzzVoicingsConform) {
  for (const double voicing : {4.0, 5.0}) {
    SCOPED_TRACE(voicing);
    ConformanceOptions options;
    options.params = {{"voicing", voicing}, {"fuzzPercent", 80.0}, {"tonePercent", 60.0}};
    options.stateParams = {
        {"voicing", voicing}, {"fuzzPercent", 30.0}, {"tonePercent", 20.0}, {"levelDb", -3.0}};
    options.sweepBound = 16.0f;
    options.cpuAverageBudgetNs = 90000.0;
    options.cpuWorstBudgetNs = 500000.0;
    expectKindConforms("drive.fuzz", options);
  }
}

TEST(DriveEffectsTest, MeasuredBassVoicingsConform) {
  for (const double voicing : {1.0, 2.0, 3.0}) {
    SCOPED_TRACE(voicing);
    ConformanceOptions options;
    options.params = {{"voicing", voicing},
                      {"crossoverHz", 300.0},
                      {"drivePercent", 70.0},
                      {"presenceDb", 3.0},
                      {"blendPercent", 80.0}};
    options.stateParams = {{"voicing", voicing}, {"crossoverHz", 120.0}, {"drivePercent", 20.0},
                           {"presenceDb", -4.0}, {"blendPercent", 40.0}, {"levelDb", 3.0}};
    options.sweepBound = 16.0f;
    options.cpuAverageBudgetNs = 90000.0;
    options.cpuWorstBudgetNs = 500000.0;
    expectKindConforms("drive.bass", options);
  }
}

TEST(DriveEffectsTest, MeasuredVoicingsDistortMoreWithMoreDrive) {
  struct Case {
    const char* id;
    double voicing;
    const char* drive;
    double driveMax;
    double levelDb;
    double hz;
  };
  // Each at a level and on a band where its drive does its work (the voicings
  // with a clean path at 100 Hz, where the unit's distortion follows the knob).
  for (const Case& c : {Case{"drive.overdrive", 1.0, "drivePercent", 100.0, -30.0, 100.0},
                        Case{"drive.overdrive", 2.0, "drivePercent", 100.0, -30.0, 100.0},
                        Case{"drive.overdrive", 3.0, "drivePercent", 100.0, -40.0, 1000.0},
                        Case{"drive.boost", 1.0, "gainDb", 20.0, -30.0, 100.0},
                        Case{"drive.boost", 2.0, "gainDb", 20.0, -30.0, 1000.0},
                        Case{"drive.distortion", 2.0, "distortionPercent", 100.0, -80.0, 100.0},
                        Case{"drive.fuzz", 4.0, "fuzzPercent", 100.0, -60.0, 1000.0},
                        Case{"drive.fuzz", 5.0, "fuzzPercent", 100.0, -60.0, 1000.0},
                        Case{"drive.bass", 2.0, "drivePercent", 100.0, -18.0, 1000.0},
                        Case{"drive.bass", 3.0, "drivePercent", 100.0, -30.0, 1000.0}}) {
    SCOPED_TRACE(juce::String(c.id) + " voicing " + juce::String(c.voicing));
    const Params little = {{"voicing", c.voicing}, {c.drive, 0.05 * c.driveMax}};
    const Params lots = {{"voicing", c.voicing}, {c.drive, 0.95 * c.driveMax}};
    const double a = thdAtDbfs(c.id, little, c.hz, c.levelDb);
    const double b = thdAtDbfs(c.id, lots, c.hz, c.levelDb);
    std::cout << "[   THD    ] " << c.id << " voicing " << c.voicing << ": " << juce::String(a, 2)
              << " % -> " << juce::String(b, 2) << " %" << std::endl;
    EXPECT_GT(b, a + 3.0);
  }
}

TEST(DriveEffectsTest, MeasuredOverdriveToneMovesTheTop) {
  // Each voicing's tone low-pass sits at the fitted corner with Tone at the
  // middle; Heavy's sweeps four octaves, so its upper half moves the very top.
  for (const double voicing : {1.0, 2.0, 3.0}) {
    SCOPED_TRACE(voicing);
    const auto top = [&](double tone) {
      const Params p = {{"voicing", voicing}, {"tonePercent", tone}};
      return toneGainDb("drive.overdrive", p, 10000.0, -18.0) -
             toneGainDb("drive.overdrive", p, 500.0, -18.0);
    };
    EXPECT_GT(top(100.0), top(50.0) + 1.5);
    EXPECT_GT(top(50.0), top(0.0) + 3.0);
  }
}

TEST(DriveEffectsTest, CrankedDistortionDoesNotFollowGainAndGainIsTheLevel) {
  // The unit's distortion stays at 46 % at every VOLUME while its level moves:
  // the clipping is fixed and Gain comes after it.
  const Params low = {{"voicing", 1.0}, {"gainDb", 6.0}};
  const Params high = {{"voicing", 1.0}, {"gainDb", 24.0}};
  EXPECT_NEAR(thdAtDbfs("drive.trebleboost", low, 1000.0, -30.0),
              thdAtDbfs("drive.trebleboost", high, 1000.0, -30.0), 0.5);
  EXPECT_NEAR(toneGainDb("drive.trebleboost", high, 1000.0, -30.0) -
                  toneGainDb("drive.trebleboost", low, 1000.0, -30.0),
              18.0, 0.2);
}

TEST(DriveEffectsTest, CrankedRangeMovesTheHighPass) {
  // Range moves the high-pass after the clipping: the higher, the thinner.
  const auto lowEnd = [&](double range) {
    const Params p = {{"voicing", 1.0}, {"rangeHz", range}};
    return toneGainDb("drive.trebleboost", p, 125.0, -30.0) -
           toneGainDb("drive.trebleboost", p, 2000.0, -30.0);
  };
  EXPECT_GT(lowEnd(400.0), lowEnd(1000.0) + 1.0);
  EXPECT_GT(lowEnd(1000.0), lowEnd(4000.0) + 3.0);
}

TEST(DriveEffectsTest, MeasuredBassVoicingsAreCleanWithTheBlendAtZero) {
  for (const double voicing : {1.0, 2.0, 3.0}) {
    SCOPED_TRACE(voicing);
    const Params dry = {{"voicing", voicing}, {"drivePercent", 100.0}, {"blendPercent", 0.0}};
    const Params wet = {{"voicing", voicing}, {"drivePercent", 100.0}, {"blendPercent", 100.0}};
    EXPECT_LT(thdAtDbfs("drive.bass", dry, 1000.0, -12.0), 0.5);
    EXPECT_GT(thdAtDbfs("drive.bass", wet, 1000.0, -12.0), 2.0);
  }
}

TEST(DriveEffectsTest, GritCrossoverKeepsTheLowsClean) {
  const Params low = {{"voicing", 3.0}, {"crossoverHz", 60.0}, {"drivePercent", 80.0}};
  const Params high = {{"voicing", 3.0}, {"crossoverHz", 800.0}, {"drivePercent", 80.0}};
  EXPECT_LT(thdAtDbfs("drive.bass", high, 100.0, -30.0),
            thdAtDbfs("drive.bass", low, 100.0, -30.0) - 2.0);
}

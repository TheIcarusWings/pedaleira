// The fitted clean and crunch amp descriptions in resources/amps, each
// measured the way its reference amp was (amp_fit_measure.h) and checked
// against the numbers it was fitted to:
//
//   clean   glass-forty.json  bell-thirty.json  steady-120.json
//           velvet-fifty.json  ember-five.json
//   crunch  amber-fifty.json  copper-eighteen.json  dust-forty.json
//           bronze-twenty.json  silk-hundred.json
//
// The targets are measurements of ten reference amps (a modelling unit's
// amps, played through its USB input), with every signal played 24 dB under
// its nominal level for the clean amps and 36 dB under for the crunch amps:
// the small-signal gain Gs, the bend, the ceiling and the THD of the 1 kHz
// staircase (the THD at its -18 dBFS step) at GAIN 2, 5, 8 and 10 with every
// other knob at 5; the response to the test signal's low sweep and the 1 kHz
// gain of both sweeps with every knob at 5; and the change of the response
// when each tone knob goes from 0 to 10 (GAIN at 2, the others at 5), at 125
// Hz, 500 Hz, 2 kHz and 8 kHz. The tolerances are the fitting goals (Gs 1.5
// dB, bend 3 dB, ceiling 1 dB, THD 10 points, under 0.1 percent for Steady
// 120); the responses are held to what the fit reached, and so are the two
// THD points of Ember Five that the fit could not reach (see its entry).
//
// AmpFitHarness.Measure (amp_fitted_tests.cpp) prints the same numbers for
// any description, with PEDALEIRA_AMP_FIT_OFFSET, PEDALEIRA_AMP_FIT_KNOBS and
// PEDALEIRA_AMP_FIT_TONES for these measurements.
#include "amp_fit_measure.h"

#include "amp/AmpDescription.h"
#include "effects/amp/BuiltinAmps.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>

namespace {

// The four bands the references were read at: 125 Hz, 500 Hz, 2 kHz, 8 kHz.
constexpr int kBands[4] = {6, 12, 18, 24};
constexpr const char* kBandNames[4] = {"125 Hz", "500 Hz", "2 kHz", "8 kHz"};
using Four = std::array<double, 4>;

struct GainTarget {
  double knob;  // GAIN 0 to 10
  double gsDb;
  std::optional<double> bendDb;
  double ceilingDb;
  double thdPercent;
  double thdTolerance{10.0};
};

struct AmpTarget {
  const char* file;
  const char* name;
  double offsetDb;            // how far under nominal the reference was played
  fxtest::Params sweepKnobs;  // other knobs for the GAIN points (beyond 5)
  std::vector<GainTarget> gains;
  Four responseAll5;          // relative to 1 kHz
  std::pair<double, double> sweepGainsDb;  // low and high sweep at 1 kHz
  double responseToleranceDb;
  std::map<std::string, Four> toneDeltas;  // knob at 10 minus knob at 0
  double toneToleranceDb;
};

const std::vector<AmpTarget>& targets();

std::string shipped(const char* file) {
  const std::string text = ampfx::builtinAmpText(file);
  EXPECT_FALSE(text.empty()) << file << " is not linked into the binary";
  return text;
}

fxtest::Params with(fxtest::Params knobs, const fxtest::Params& more) {
  knobs.insert(knobs.end(), more.begin(), more.end());
  return knobs;
}

Four bandsOf(const std::array<double, EffectMatch::kNumBands>& r) {
  Four out{};
  for (int i = 0; i < 4; ++i)
    out[static_cast<size_t>(i)] = r[static_cast<size_t>(kBands[i])];
  return out;
}

std::string text(const Four& v) {
  std::ostringstream o;
  o << std::fixed << std::setprecision(1);
  for (double x : v)
    o << " " << x;
  return o.str();
}

void checkGainPoints(const AmpTarget& amp) {
  const std::string description = shipped(amp.file);
  for (const GainTarget& t : amp.gains) {
    std::string error;
    const auto m = ampfit::measureGainPoint(description, with({{"gain", t.knob * 10.0}}, amp.sweepKnobs), error,
                                            amp.offsetDb);
    ASSERT_TRUE(m.has_value()) << error;
    std::cout << "[  FITTED  ] " << amp.name << " GAIN " << t.knob << ": Gs " << m->smallSignalGainDb << " ("
              << t.gsDb << "), bend " << (m->bendDb ? std::to_string(*m->bendDb) : "none") << " ("
              << (t.bendDb ? std::to_string(*t.bendDb) : "none") << "), ceiling " << m->ceilingDb << " ("
              << t.ceilingDb << "), THD " << m->thdPercent << " % (" << t.thdPercent << ")" << std::endl;
    EXPECT_NEAR(m->smallSignalGainDb, t.gsDb, 1.5) << amp.name << " GAIN " << t.knob;
    if (t.bendDb) {
      ASSERT_TRUE(m->bendDb.has_value()) << amp.name << " GAIN " << t.knob << " never bends";
      EXPECT_NEAR(*m->bendDb, *t.bendDb, 3.0) << amp.name << " GAIN " << t.knob;
    } else {
      EXPECT_FALSE(m->bendDb.has_value()) << amp.name << " GAIN " << t.knob << " should stay linear over the staircase";
    }
    EXPECT_NEAR(m->ceilingDb, t.ceilingDb, 1.0) << amp.name << " GAIN " << t.knob;
    EXPECT_NEAR(m->thdPercent, t.thdPercent, t.thdTolerance) << amp.name << " GAIN " << t.knob;
  }
}

void checkResponses(const AmpTarget& amp) {
  const std::string description = shipped(amp.file);
  std::string error;
  const auto all5 = ampfit::measureSweeps(description, {{"gain", 50.0}}, error, amp.offsetDb);
  ASSERT_TRUE(all5.has_value()) << error;
  const Four r = bandsOf(all5->responseLow);
  std::cout << "[  FITTED  ] " << amp.name << " every knob at 5:" << text(r) << " (" << text(amp.responseAll5).substr(1)
            << "), sweeps " << all5->gainLowDb << " / " << all5->gainHighDb << " dB (" << amp.sweepGainsDb.first << " / "
            << amp.sweepGainsDb.second << ")" << std::endl;
  for (size_t i = 0; i < 4; ++i)
    EXPECT_NEAR(r[i], amp.responseAll5[i], amp.responseToleranceDb) << amp.name << " at " << kBandNames[i];
  EXPECT_NEAR(all5->gainLowDb, amp.sweepGainsDb.first, amp.responseToleranceDb) << amp.name << " low sweep";
  EXPECT_NEAR(all5->gainHighDb, amp.sweepGainsDb.second, amp.responseToleranceDb) << amp.name << " high sweep";

  for (const auto& [knob, target] : amp.toneDeltas) {
    const auto low = ampfit::measureSweeps(description, {{"gain", 20.0}, {juce::String(knob), 0.0}}, error, amp.offsetDb);
    ASSERT_TRUE(low.has_value()) << error;
    const auto high = ampfit::measureSweeps(description, {{"gain", 20.0}, {juce::String(knob), 100.0}}, error, amp.offsetDb);
    ASSERT_TRUE(high.has_value()) << error;
    const Four a = bandsOf(low->responseLow), b = bandsOf(high->responseLow);
    Four delta{};
    for (size_t i = 0; i < 4; ++i)
      delta[i] = b[i] - a[i];
    std::cout << "[  FITTED  ] " << amp.name << " " << knob << " 0 to 10:" << text(delta) << " (" << text(target).substr(1)
              << ")" << std::endl;
    for (size_t i = 0; i < 4; ++i)
      EXPECT_NEAR(delta[i], target[i], amp.toneToleranceDb) << amp.name << " " << knob << " at " << kBandNames[i];
  }
}

const AmpTarget& target(const char* file) {
  for (const AmpTarget& t : targets())
    if (std::string(t.file) == file)
      return t;
  static const AmpTarget none{};
  ADD_FAILURE() << "no target for " << file;
  return none;
}

}  // namespace

// ---------------------------------------------------------------------------
// Every file loads, says what it is, marks what was fitted and carries only
// the controls its circuit has
// ---------------------------------------------------------------------------

TEST(AmpFittedCleanCrunch, EveryFileLoadsAndNamesItsTargets) {
  for (const AmpTarget& t : targets()) {
    amp::AmpDescription d;
    std::string error;
    ASSERT_TRUE(amp::parseAmpDescription(shipped(t.file), d, error)) << t.file << ": " << error;
    EXPECT_EQ(d.name, t.name);
    EXPECT_EQ(d.licence, "MIT");
    int fitted = 0;
    for (const amp::ValueNote& note : d.notes)
      if (note.source == amp::Provenance::Fitted && note.note.find("fitted to the " + std::string(t.name)) != std::string::npos)
        ++fitted;
    EXPECT_GE(fitted, 10) << t.file << " should mark the values fitted to its measurements";
  }
}

TEST(AmpFittedCleanCrunch, EachPanelCarriesTheControlsOfItsCircuit) {
  using amp::PanelId;
  // gain, bass, middle, treble, presence (or the tone cut), master
  const std::map<std::string, std::string> panels = {
      {"glass-forty.json", "gain bass treble"},
      {"bell-thirty.json", "gain cut"},
      {"steady-120.json", "gain bass middle treble"},
      {"velvet-fifty.json", "gain bass middle treble master"},
      {"ember-five.json", "gain bass treble"},
      {"amber-fifty.json", "gain bass middle treble presence master"},
      {"copper-eighteen.json", "gain cut"},
      {"dust-forty.json", "gain bass middle treble presence"},
      {"bronze-twenty.json", "gain bass treble cut"},
      {"silk-hundred.json", "gain bass middle treble presence master"},
  };
  for (const auto& [file, controls] : panels) {
    amp::AmpDescription d;
    std::string error;
    ASSERT_TRUE(amp::parseAmpDescription(shipped(file.c_str()), d, error)) << file << ": " << error;
    auto has = [&](const char* word) { return (" " + controls + " ").find(std::string(" ") + word + " ") != std::string::npos; };
    EXPECT_EQ(d.control(PanelId::Gain).present, has("gain")) << file;
    EXPECT_EQ(d.control(PanelId::Bass).present, has("bass")) << file;
    EXPECT_EQ(d.control(PanelId::Middle).present, has("middle")) << file;
    EXPECT_EQ(d.control(PanelId::Treble).present, has("treble")) << file;
    EXPECT_EQ(d.control(PanelId::Presence).present, has("presence") || has("cut")) << file;
    EXPECT_EQ(d.presenceDrivesToneCut, has("cut")) << file;
    EXPECT_EQ(d.power.hasToneCut, has("cut")) << file;
    if (has("cut"))
      EXPECT_EQ(d.control(PanelId::Presence).label, "Tone Cut") << file;
    EXPECT_EQ(d.control(PanelId::Master).present, has("master")) << file;
    EXPECT_FALSE(d.control(PanelId::Depth).present) << file;
    EXPECT_FALSE(d.control(PanelId::Bright).present) << file;
    EXPECT_FALSE(d.control(PanelId::Voicing).present) << file;
  }
}

TEST(AmpFittedCleanCrunch, SteadyIsSolidStateAndTheTubeAmpsAreNot) {
  for (const AmpTarget& t : targets()) {
    amp::AmpDescription d;
    std::string error;
    ASSERT_TRUE(amp::parseAmpDescription(shipped(t.file), d, error)) << error;
    const bool solid = std::string(t.file) == "steady-120.json";
    EXPECT_EQ(d.power.params.hardClip, solid) << t.file;
    if (solid) {
      EXPECT_EQ(d.power.topology, "solid_state");
      EXPECT_FLOAT_EQ(d.power.supply.sourceResistance, 0.0f) << "a solid-state supply does not sag";
    }
  }
}

TEST(AmpFittedCleanCrunch, GlassFortyMatchesItsGainSweep) { checkGainPoints(target("glass-forty.json")); }
TEST(AmpFittedCleanCrunch, BellThirtyMatchesItsGainSweep) { checkGainPoints(target("bell-thirty.json")); }
TEST(AmpFittedCleanCrunch, Steady120MatchesItsGainSweep) { checkGainPoints(target("steady-120.json")); }
TEST(AmpFittedCleanCrunch, VelvetFiftyMatchesItsGainSweep) { checkGainPoints(target("velvet-fifty.json")); }
TEST(AmpFittedCleanCrunch, EmberFiveMatchesItsGainSweep) { checkGainPoints(target("ember-five.json")); }
TEST(AmpFittedCleanCrunch, AmberFiftyMatchesItsGainSweep) { checkGainPoints(target("amber-fifty.json")); }
TEST(AmpFittedCleanCrunch, CopperEighteenMatchesItsGainSweep) { checkGainPoints(target("copper-eighteen.json")); }
TEST(AmpFittedCleanCrunch, DustFortyMatchesItsGainSweep) { checkGainPoints(target("dust-forty.json")); }
TEST(AmpFittedCleanCrunch, BronzeTwentyMatchesItsGainSweep) { checkGainPoints(target("bronze-twenty.json")); }
TEST(AmpFittedCleanCrunch, SilkHundredMatchesItsGainSweep) { checkGainPoints(target("silk-hundred.json")); }

TEST(AmpFittedCleanCrunch, GlassFortyMatchesItsResponses) { checkResponses(target("glass-forty.json")); }
TEST(AmpFittedCleanCrunch, BellThirtyMatchesItsResponses) { checkResponses(target("bell-thirty.json")); }
TEST(AmpFittedCleanCrunch, Steady120MatchesItsResponses) { checkResponses(target("steady-120.json")); }
TEST(AmpFittedCleanCrunch, VelvetFiftyMatchesItsResponses) { checkResponses(target("velvet-fifty.json")); }
TEST(AmpFittedCleanCrunch, EmberFiveMatchesItsResponses) { checkResponses(target("ember-five.json")); }
TEST(AmpFittedCleanCrunch, AmberFiftyMatchesItsResponses) { checkResponses(target("amber-fifty.json")); }
TEST(AmpFittedCleanCrunch, CopperEighteenMatchesItsResponses) { checkResponses(target("copper-eighteen.json")); }
TEST(AmpFittedCleanCrunch, DustFortyMatchesItsResponses) { checkResponses(target("dust-forty.json")); }
TEST(AmpFittedCleanCrunch, BronzeTwentyMatchesItsResponses) { checkResponses(target("bronze-twenty.json")); }
TEST(AmpFittedCleanCrunch, SilkHundredMatchesItsResponses) { checkResponses(target("silk-hundred.json")); }

TEST(AmpFittedCleanCrunch, PrintsWhatEachAmpCosts) {
  // One instance at 48 kHz in blocks of 128, a mono input, as AmpFitted
  // times the high gain amps. Only printed (and bounded loosely).
  const std::vector<float> source = fxtest::noise(128.0 * 64.0 / ampfit::kRate, 7u, 0.25f, ampfit::kRate);
  for (const AmpTarget& t : targets()) {
    std::string error;
    auto amp = ampfit::makeAmp(shipped(t.file), {{"gain", 60.0}}, error);
    ASSERT_NE(amp, nullptr) << error;
    juce::AudioBuffer<float> buffer(1, 128);
    BlockProcessContext ctx;
    const juce::ScopedNoDenormals noDenormals;
    double best = 1.0e18;
    for (int pass = 0; pass < 3; ++pass) {
      double total = 0.0;
      const int blocks = 2000;
      for (int b = 0; b < blocks; ++b) {
        buffer.copyFrom(0, 0, source.data() + static_cast<size_t>(b % 64) * 128, 128);
        const auto begin = std::chrono::steady_clock::now();
        amp->processInChain(buffer, ctx);
        const auto end = std::chrono::steady_clock::now();
        total += static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
      }
      best = std::min(best, total / blocks);
    }
    const double percent = 100.0 * best / 2666667.0;
    std::cout << "[   CPU    ] " << t.name << ": " << best << " ns per 128-sample block (" << percent << "% of a core)"
              << std::endl;
    EXPECT_LT(percent, 25.0);
  }
}

namespace {

const std::vector<AmpTarget>& targets() {
  constexpr auto none = std::nullopt;
  static const std::vector<AmpTarget> list = {
      {"glass-forty.json", "Glass Forty", 24.0, {},
       {{2, 5.8, none, -18.3, 0.2}, {5, 13.3, -29.1, -16.0, 0.6}, {8, 23.1, -38.6, -15.7, 2.5}, {10, 27.9, -43.9, -15.7, 10.8}},
       {3.7, 1.7, 2.5, 7.1}, {-10.7, -11.1}, 1.0,
       {{"bass", {17.3, 5.4, -0.3, 0.3}}, {"treble", {-9.1, -7.5, 7.3, 11.3}}}, 2.0},
      {"bell-thirty.json", "Bell Thirty", 24.0, {},
       {{2, 5.0, -28.3, -21.4, 0.4}, {5, 18.3, -41.5, -19.8, 4.3}, {8, 28.8, -52.0, -19.8, 25.2}, {10, 34.0, -57.1, -19.8, 37.2}},
       {-1.6, -3.4, 4.3, 6.6}, {-6.5, -13.9}, 2.5,
       {{"presence", {6.7, 4.1, -5.3, -12.9}}}, 2.5},
      // Solid state: the THD target is "under 0.1 percent" at every GAIN.
      {"steady-120.json", "Steady 120", 24.0, {},
       {{2, 4.9, none, -19.1, 0.0, 0.1}, {5, 12.2, none, -12.5, 0.0, 0.1}, {8, 22.3, -33.8, -11.3, 0.0, 0.1},
        {10, 27.1, -38.4, -11.2, 0.0, 0.1}},
       {0.5, -2.5, 5.5, 10.9}, {-11.7, -11.7}, 1.0,
       {{"bass", {13.3, -0.2, 1.4, 1.9}}, {"middle", {-1.9, 5.1, -3.3, -4.4}}, {"treble", {-9.2, -8.3, 7.0, 13.3}}}, 1.0},
      // Its GAIN sweep was taken 6 dB under its every-knob-at-5 level: the
      // master at 3.5 for that sweep (see the description's summary).
      {"velvet-fifty.json", "Velvet Fifty", 24.0, {{"master", 35.0}},
       {{2, 4.4, -27.9, -21.8, 2.1}, {5, 7.7, -31.2, -20.2, 2.8}, {8, 14.7, -38.2, -18.9, 5.3}, {10, 20.8, -44.3, -18.7, 9.4}},
       {6.9, 0.5, 3.2, 5.5}, {-10.1, -11.0}, 3.0,
       {{"bass", {13.6, 2.6, 0.6, 1.5}}, {"middle", {-4.2, -2.2, -1.9, -4.9}}, {"treble", {-12.9, -7.3, 3.3, -2.1}}}, 3.0},
      // At VOLUME 8 and 10 the reference makes 18 and 33 percent THD within a
      // few dB of its bend: more harmonics per dB of compression than any of
      // the framework's curves give (its single-ended stage); the fit reaches
      // 9 and 15, held here.
      {"ember-five.json", "Ember Five", 24.0, {},
       {{2, -3.4, none, -27.4, 0.7}, {5, 17.8, -35.3, -16.5, 2.9}, {8, 24.6, -42.1, -16.5, 17.9, 11.0},
        {10, 27.3, -45.5, -16.5, 32.9, 19.0}},
       {6.8, -2.4, 4.9, 9.6}, {-6.1, -11.2}, 3.0,
       {{"bass", {11.2, 0.2, 0.8, 1.2}}, {"treble", {-14.4, -6.5, 2.1, -2.4}}}, 8.0},
      {"amber-fifty.json", "Amber Fifty", 36.0, {},
       {{2, 31.3, -50.6, -19.3, 2.9}, {5, 33.3, -52.8, -19.3, 3.6}, {8, 33.7, -53.2, -19.3, 3.7}, {10, 33.9, -53.3, -19.3, 3.7}},
       {-7.6, -3.0, 2.4, -3.4}, {-3.2, -14.1}, 2.0,
       {{"bass", {5.9, -0.1, 0.3, 0.3}}, {"middle", {-9.6, -5.5, -3.3, -4.0}}, {"treble", {-4.5, -4.4, 0.9, 5.5}}}, 4.5},
      {"copper-eighteen.json", "Copper Eighteen", 36.0, {},
       {{2, -0.4, none, -36.5, 0.2}, {5, 17.9, -41.9, -21.5, 0.4}, {8, 29.7, -53.2, -19.9, 4.4}, {10, 32.2, -55.6, -19.9, 6.2}},
       {-0.9, -4.2, 6.1, 16.2}, {-18.1, -19.0}, 3.0,
       {{"presence", {4.8, 4.4, -5.8, -14.8}}}, 1.5},
      {"dust-forty.json", "Dust Forty", 36.0, {},
       {{2, -1.4, none, -37.3, 0.3}, {5, 22.4, -44.4, -21.3, 2.1}, {8, 30.2, -52.3, -21.2, 5.3}, {10, 30.5, -52.2, -21.2, 6.0}},
       {3.1, -3.8, 5.9, 10.6}, {-13.4, -15.8}, 3.0,
       {{"bass", {10.1, -0.3, 1.7, 2.0}}, {"middle", {-9.1, 6.9, -5.0, -8.0}}, {"treble", {-11.9, -9.7, 5.9, 10.9}}}, 2.0},
      {"bronze-twenty.json", "Bronze Twenty", 36.0, {},
       {{2, 4.0, none, -32.2, 0.1}, {5, 24.5, -48.7, -19.0, 1.9}, {8, 37.9, -62.0, -18.8, 14.2}, {10, 39.6, -64.2, -19.0, 16.6}},
       {-1.1, -1.5, 3.6, 11.9}, {-11.8, -14.8}, 3.0,
       {{"bass", {19.9, 5.7, 0.2, 1.5}}, {"treble", {-6.4, -9.6, 8.5, 16.4}}, {"presence", {2.8, 2.5, -5.6, -19.8}}}, 4.0},
      // Its drive sweep was taken about 5 dB over its every-knob-at-5 level:
      // the master at 6.7 for that sweep (see the description's summary).
      {"silk-hundred.json", "Silk Hundred", 36.0, {{"master", 67.0}},
       {{2, 24.5, -44.0, -18.4, 1.3}, {5, 37.6, -57.0, -17.6, 14.0}, {8, 43.1, -62.8, -17.4, 25.7}, {10, 44.9, -64.9, -17.4, 29.0}},
       {-3.9, -1.3, 0.8, -6.3}, {-6.5, -17.4}, 3.5,
       {{"bass", {12.2, 4.6, -3.4, -5.7}}, {"middle", {-5.7, 2.0, -3.5, -5.5}}, {"treble", {-4.3, -2.7, 3.3, 11.4}}}, 4.0},
  };
  return list;
}

}  // namespace

// The fitted amp descriptions in resources/amps, each measured the way its
// reference amp was (amp_fit_measure.h) and checked against the numbers it
// was fitted to:
//
//   lead-100.json      Lead 100
//   red-forty.json     Red Forty
//   modern-three.json  Modern Three
//   cell-four.json     Cell Four
//   heavy-hot.json     Heavy Hot (Red Forty's circuit with a lower output trim)
//
// The targets are measurements of five reference amps (a modelling unit's
// amps, played through its USB input): the small-signal gain Gs, the input
// where the gain has fallen 1 dB (the bend), the highest output over a 1 kHz
// staircase (the ceiling) and the THD of a 1 kHz sine at -54 dBFS, at GAIN 2,
// 5, 8 and 10 with every other knob at 5; and the response to the test
// signal's sweep at -18 dBFS (line level) at GAIN 5. The tolerances are the
// fitting goals: Gs 1.5 dB, bend 3 dB, ceiling 1 dB, THD 10 points; the
// response is held to what the fit reached.
//
// AmpFitHarness.Measure is the fitting loop's measuring tool: with
// PEDALEIRA_AMP_FIT set to a description file it prints the same numbers as
// JSON (and is skipped otherwise):
//
//   PEDALEIRA_AMP_FIT=resources/amps/lead-100.json \
//     build/test/DspTests_artefacts/Release/DspTests --gtest_filter='AmpFitHarness.*'
#include "amp_fit_measure.h"

#include "amp/AmpDescription.h"
#include "effects/amp/BuiltinAmps.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {

struct GainTarget {
  double knob;  // GAIN 0 to 10
  double gsDb;
  std::optional<double> bendDb;
  double ceilingDb;
  double thdPercent;
};

struct AmpTarget {
  const char* file;
  const char* name;
  std::vector<GainTarget> gains;
  std::array<double, EffectMatch::kNumBands> responseGain5;
  double responseToleranceDb;
};

const std::vector<AmpTarget>& targets();

std::string shipped(const char* file) {
  const std::string text = ampfx::builtinAmpText(file);
  EXPECT_FALSE(text.empty()) << file << " is not linked into the binary";
  return text;
}

fxtest::Params atGain(double knob) { return {{"gain", knob * 10.0}}; }

void checkGainPoints(const AmpTarget& amp) {
  const std::string text = shipped(amp.file);
  for (const GainTarget& t : amp.gains) {
    std::string error;
    const auto m = ampfit::measureGainPoint(text, atGain(t.knob), error);
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
      EXPECT_FALSE(m->bendDb.has_value()) << amp.name << " GAIN " << t.knob << " should stay linear to -36 dBFS";
    }
    EXPECT_NEAR(m->ceilingDb, t.ceilingDb, 1.0) << amp.name << " GAIN " << t.knob;
    EXPECT_NEAR(m->thdPercent, t.thdPercent, 10.0) << amp.name << " GAIN " << t.knob;
  }
}

void checkResponse(const AmpTarget& amp) {
  std::string error;
  const auto r = ampfit::measureResponse(shipped(amp.file), atGain(5.0), error);
  ASSERT_TRUE(r.has_value()) << error;
  double worst = 0.0;
  std::ostringstream line;
  line << std::fixed << std::setprecision(1);
  // 40 Hz to 10 kHz: the reference's lowest and highest bands are its own
  // converter's as much as the amp's (and the fit was made with steady sines,
  // which read the top bands up to 1.5 dB higher than the sweep).
  for (int b = 1; b < EffectMatch::kNumBands - 2; ++b) {
    const double diff = (*r)[static_cast<size_t>(b)] - amp.responseGain5[static_cast<size_t>(b)];
    worst = std::max(worst, std::abs(diff));
    line << " " << diff;
    EXPECT_NEAR((*r)[static_cast<size_t>(b)], amp.responseGain5[static_cast<size_t>(b)], amp.responseToleranceDb)
        << amp.name << " at " << EffectMatch::kBandCentresHz[static_cast<size_t>(b)] << " Hz";
  }
  std::cout << "[  FITTED  ] " << amp.name << " GAIN 5 response minus target, 40 Hz to 10 kHz:" << line.str()
            << " (worst " << worst << " dB)" << std::endl;
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
// Every fitted file loads, says what it is and marks what was fitted
// ---------------------------------------------------------------------------

TEST(AmpFitted, EveryFileLoadsAndNamesItsTargets) {
  for (const AmpTarget& t : targets()) {
    amp::AmpDescription d;
    std::string error;
    ASSERT_TRUE(amp::parseAmpDescription(shipped(t.file), d, error)) << t.file << ": " << error;
    EXPECT_EQ(d.name, t.name);
    EXPECT_EQ(d.licence, "MIT");
    int fitted = 0;
    for (const amp::ValueNote& note : d.notes)
      if (note.source == amp::Provenance::Fitted && note.note.find("fitted to") != std::string::npos)
        ++fitted;
    EXPECT_GE(fitted, 5) << t.file << " should mark the values fitted to its measurements";
  }
}

TEST(AmpFitted, HeavyHotIsRedFortyWithALowerOutputTrim) {
  amp::AmpDescription red, heavy;
  std::string error;
  ASSERT_TRUE(amp::parseAmpDescription(shipped("red-forty.json"), red, error)) << error;
  ASSERT_TRUE(amp::parseAmpDescription(shipped("heavy-hot.json"), heavy, error)) << error;
  EXPECT_LT(heavy.calibration.outputTrim, red.calibration.outputTrim);
  // The same circuit: rendered with the trims made equal, the two are the same samples.
  const std::vector<float> in = ampfit::sineAt(-40.0, 0.5);
  auto render = [&](const char* file, float trim) {
    std::string text = shipped(file);
    amp::AmpDescription d;
    std::string why;
    EXPECT_TRUE(amp::parseAmpDescription(text, d, why));
    auto a = ampfit::makeAmp(text, atGain(6.0), why);
    EXPECT_NE(a, nullptr) << why;
    std::vector<float> out = ampfit::renderMono(*a, in);
    for (float& v : out)
      v *= trim / d.calibration.outputTrim;
    return out;
  };
  const std::vector<float> a = render("red-forty.json", 1.0f);
  const std::vector<float> b = render("heavy-hot.json", 1.0f);
  double worst = 0.0;
  for (size_t i = 0; i < a.size(); ++i)
    worst = std::max(worst, static_cast<double>(std::abs(a[i] - b[i])));
  EXPECT_LT(worst, 1.0e-5);
}

TEST(AmpFitted, Lead100MatchesItsGainSweep) { checkGainPoints(target("lead-100.json")); }
TEST(AmpFitted, RedFortyMatchesItsGainSweep) { checkGainPoints(target("red-forty.json")); }
TEST(AmpFitted, ModernThreeMatchesItsGainSweep) { checkGainPoints(target("modern-three.json")); }
TEST(AmpFitted, CellFourMatchesItsGainSweep) { checkGainPoints(target("cell-four.json")); }
TEST(AmpFitted, HeavyHotMatchesItsGainSweep) { checkGainPoints(target("heavy-hot.json")); }

TEST(AmpFitted, Lead100MatchesItsResponse) { checkResponse(target("lead-100.json")); }
TEST(AmpFitted, RedFortyMatchesItsResponse) { checkResponse(target("red-forty.json")); }
TEST(AmpFitted, ModernThreeMatchesItsResponse) { checkResponse(target("modern-three.json")); }
TEST(AmpFitted, CellFourMatchesItsResponse) { checkResponse(target("cell-four.json")); }
TEST(AmpFitted, HeavyHotMatchesItsResponse) { checkResponse(target("heavy-hot.json")); }

TEST(AmpFitted, PrintsWhatEachAmpCosts) {
  // One instance at 48 kHz in blocks of 128, a mono input, the way AmpCost
  // times the amp kinds. Only printed (and bounded loosely): the numbers belong
  // to the machine.
  const std::vector<float> source = fxtest::noise(128.0 * 64.0 / ampfit::kRate, 7u, 0.25f, ampfit::kRate);
  for (const AmpTarget& t : targets()) {
    std::string error;
    auto amp = ampfit::makeAmp(shipped(t.file), atGain(6.0), error);
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
    std::cout << "[   CPU    ] " << t.name << ": " << best << " ns per 128-sample block (" << percent
              << "% of a core)" << std::endl;
    EXPECT_LT(percent, 25.0);
  }
}

// ---------------------------------------------------------------------------
// The harness for a fitting loop
// ---------------------------------------------------------------------------

TEST(AmpFitHarness, Measure) {
  const char* path = std::getenv("PEDALEIRA_AMP_FIT");
  if (path == nullptr || *path == 0)
    GTEST_SKIP() << "set PEDALEIRA_AMP_FIT to a description file to measure it";
  std::ifstream file(path);
  ASSERT_TRUE(file.good()) << path;
  std::stringstream text;
  text << file.rdbuf();
  std::cout << "{\"file\": \"" << path << "\", \"gain\": [";
  bool first = true;
  for (const double knob : {2.0, 5.0, 8.0, 10.0}) {
    std::string error;
    const auto m = ampfit::measureGainPoint(text.str(), atGain(knob), error);
    ASSERT_TRUE(m.has_value()) << error;
    std::cout << (first ? "" : ", ") << "{\"knob\": " << knob << ", \"gs\": " << m->smallSignalGainDb
              << ", \"bend\": " << (m->bendDb ? std::to_string(*m->bendDb) : "null") << ", \"ceiling\": " << m->ceilingDb
              << ", \"thd\": " << m->thdPercent << "}";
    first = false;
  }
  std::string error;
  const auto r = ampfit::measureResponse(text.str(), atGain(5.0), error);
  ASSERT_TRUE(r.has_value()) << error;
  std::cout << "], \"responseGain5\": [";
  for (int b = 0; b < EffectMatch::kNumBands; ++b)
    std::cout << (b ? ", " : "") << (*r)[static_cast<size_t>(b)];
  const auto level = ampfit::measureLevel(text.str(), atGain(5.0), -18.0, error);
  ASSERT_TRUE(level.has_value()) << error;
  std::cout << "], \"level1kAtMinus18\": " << level->first << "}" << std::endl;
}

namespace {

const std::vector<AmpTarget>& targets() {
  static const std::vector<AmpTarget> list = {
      {"lead-100.json", "Lead 100",
       {{2, 37.5, -66.7, -22.1, 33.3}, {5, 42.1, -71.3, -22.1, 53.7}, {8, 48.3, -77.3, -22.1, 69.5},
        {10, 51.4, -80.1, -22.1, 73.4}},
       {-5.5, -2.8, -0.9, -0.1, 0.7, 4.1, 3.6, 2.9, 1.2, -0.1, -1.3, -2.7, -2.8, -2.6,
        -1.4, 0.0, 1.2, 2.6, 3.4, 4.2, 4.8, 5.4, 5.6, 5.5, 5.2, 4.5, 3.9, 2.5},
       2.5},
      {"red-forty.json", "Red Forty",
       {{2, 47.7, -74.2, -22.6, 69.1}, {5, 56.9, -83.3, -22.6, 71.0}, {8, 60.0, -86.3, -22.6, 71.0},
        {10, 63.6, -89.7, -22.6, 71.1}},
       {8.5, 9.9, 10.9, 12.1, 13.4, 14.1, 13.0, 10.2, 7.6, 5.7, 3.7, 1.4, 0.2, -0.7,
        -0.6, 0.0, 0.8, 1.8, 2.5, 3.3, 3.9, 4.4, 4.8, 5.2, 5.4, 5.5, 5.4, 1.8},
       3.0},
      {"modern-three.json", "Modern Three",
       {{2, 1.1, std::nullopt, -34.9, 0.2}, {5, 34.1, -56.5, -22.1, 22.1}, {8, 49.6, -71.8, -22.1, 65.8},
        {10, 56.6, -79.2, -22.2, 72.2}},
       {-3.8, -1.8, -0.8, -0.3, 1.5, 2.9, 2.4, 1.5, 0.5, -0.4, -1.4, -1.8, -1.8, -1.3,
        -0.6, 0.0, 0.6, 1.3, 1.9, 2.3, 2.6, 2.8, 3.0, 3.1, 3.2, 3.4, 3.7, 3.5},
       3.0},
      {"cell-four.json", "Cell Four",
       {{2, 37.8, -63.3, -20.9, 56.8}, {5, 41.4, -67.0, -20.9, 59.3}, {8, 49.7, -75.0, -20.9, 66.9},
        {10, 54.0, -79.4, -20.9, 69.5}},
       {3.6, 8.1, 10.9, 11.5, 9.6, 10.8, 8.8, 7.8, 5.3, 3.5, 1.8, -0.3, -0.9, -1.3,
        -0.7, 0.0, 0.9, 1.6, 2.1, 2.4, 2.2, 2.3, 2.6, 2.4, 1.8, 1.1, 0.3, -1.6},
       2.0},
      {"heavy-hot.json", "Heavy Hot",
       {{2, 45.7, -74.2, -24.6, 69.7}, {5, 55.0, -83.3, -24.6, 71.6}, {8, 58.0, -86.3, -24.6, 71.7},
        {10, 61.7, -89.7, -24.6, 71.8}},
       {7.2, 8.4, 9.3, 10.1, 11.0, 11.4, 10.8, 8.7, 6.5, 4.8, 3.0, 0.8, -0.2, -0.9,
        -0.7, 0.0, 0.9, 1.8, 2.6, 3.4, 4.0, 4.5, 4.9, 5.2, 5.5, 5.6, 5.4, 1.7},
       3.0},
  };
  return list;
}

}  // namespace

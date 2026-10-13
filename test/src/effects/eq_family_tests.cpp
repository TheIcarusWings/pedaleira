// The equalisers other than the graphic one (plugin/src/effects/eq):
// eq.parametric3, eq.parametric8, eq.cut and eq.console4. Each gets the
// conformance test, then what an equaliser is for: neutral is bit-exact,
// a band acts at its frequency by the gain it is set to, a Q is a
// bandwidth, a shelf and a pass band have the shape they are named for, a
// slope is its dB per octave, and moving a control mid-note does not step
// the sound.
#include "effects/effect_test_helpers.h"

#include <gtest/gtest.h>

#include <functional>

using namespace fxtest;

namespace {

constexpr double kRate = 48000.0;
constexpr const char* kP3 = "eq.parametric3";
constexpr const char* kP8 = "eq.parametric8";
constexpr const char* kCut = "eq.cut";
constexpr const char* kConsole = "eq.console4";

// The kind's own magnitude response at `freqs`, from its impulse response
// (the block alone, with nothing around it).
std::vector<double> responseDb(const char* kindId, const Params& params,
                               const std::vector<double>& freqs, int frames = 48000) {
  DirectKind kind(kindId, params);
  return magnitudeResponseDb(kind.render(impulse(frames)), freqs, kRate);
}

double responseAt(const char* kindId, const Params& params, double hz) {
  return responseDb(kindId, params, {hz})[0];
}

// `perOctave` points per octave from `lo` to `hi`.
std::vector<double> logGrid(double lo, double hi, int perOctave) {
  std::vector<double> f;
  for (double hz = lo; hz <= hi * 1.0001; hz *= std::pow(2.0, 1.0 / perOctave))
    f.push_back(hz);
  return f;
}

// Frequencies at which a response that peaks in the middle of `freqs` crosses
// `levelDb`: the lower one while rising, the upper one while falling, by
// interpolation on the log frequency axis.
std::pair<double, double> crossings(const std::vector<double>& freqs,
                                    const std::vector<double>& response, double levelDb) {
  double lower = 0.0;
  double upper = 0.0;
  for (size_t i = 1; i < freqs.size(); ++i) {
    const bool rising = response[i - 1] < levelDb && response[i] >= levelDb;
    const bool falling = response[i - 1] >= levelDb && response[i] < levelDb;
    if (!rising && !falling)
      continue;
    const double t = (levelDb - response[i - 1]) / (response[i] - response[i - 1]);
    const double hz = std::exp(std::log(freqs[i - 1]) + t * (std::log(freqs[i]) - std::log(freqs[i - 1])));
    if (rising && lower == 0.0)
      lower = hz;
    if (falling)
      upper = hz;
  }
  return {lower, upper};
}

// A render in 256-sample blocks with parameters moved before each block:
// `move(block, set)` is called with a setter for the kind's parameters.
std::vector<float> renderWithMoves(const char* kindId, const Params& start,
                                   const std::vector<float>& in,
                                   const std::function<void(int, const std::function<void(const char*, double)>&)>& move) {
  DirectKind kind(kindId, start);
  const std::function<void(const char*, double)> set = [&kind](const char* id, double value) {
    const int index = kind.engine().findParameter(id);
    EXPECT_GE(index, 0) << id;
    if (index >= 0)
      kind.engine().setParameter(index, static_cast<float>(value));
  };
  constexpr size_t kBlock = 256;
  std::vector<float> out;
  for (size_t offset = 0, block = 0; offset < in.size(); offset += kBlock, ++block) {
    move(static_cast<int>(block), set);
    const size_t end = std::min(in.size(), offset + kBlock);
    const std::vector<float> piece(in.begin() + static_cast<std::ptrdiff_t>(offset),
                                   in.begin() + static_cast<std::ptrdiff_t>(end));
    const std::vector<float> got = kind.render(piece);
    out.insert(out.end(), got.begin(), got.end());
  }
  return out;
}

// The largest sample-to-sample step the render of a 1 kHz sine of amplitude
// `amplitude` can have when its level is never above `ceilingGain` times
// that: the tone's own slope at its loudest. A jump in the response splices
// in a step far above it.
float naturalStep(float amplitude, float ceilingGain, double hz = 1000.0) {
  return amplitude * ceilingGain * static_cast<float>(2.0 * 3.14159265358979 * hz / kRate);
}

void expectBitExactPassThrough(const char* kindId, const Params& params, const char* what) {
  SCOPED_TRACE(what);
  DirectKind kind(kindId, params);
  const std::vector<float> in = musicalSignal(1.0, kRate);
  const std::vector<float> out = kind.render(in);
  ASSERT_EQ(out.size(), in.size());
  size_t differing = 0;
  for (size_t i = 0; i < in.size(); ++i)
    differing += out[i] != in[i] ? 1u : 0u;
  EXPECT_EQ(differing, 0u) << differing << " samples differ";
  EXPECT_EQ(kind.latencySamples(), 0);
}

}  // namespace

// ---------------------------------------------------------------------------
// eq.parametric3
// ---------------------------------------------------------------------------

TEST(Parametric3EffectTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"b1Type", 1},     {"b1Freq", 150}, {"b1Gain", 9},  {"b2Freq", 900},
                    {"b2Gain", -7},    {"b2Q", 2.5},    {"b3Type", 1},  {"b3Freq", 5000},
                    {"b3Gain", 8},     {"levelDb", 1.5}};
  options.stateParams = {{"b1Gain", 6},   {"b1Freq", 90},  {"b2Gain", -9}, {"b2Q", 3},
                         {"b3Gain", 12},  {"b3Type", 1},   {"b3Bypass", 1}, {"levelDb", -4}};
  // Three bands at +18 dB and Q 10 can stack at one frequency, with the
  // output at +20 dB, on noise at 0.25.
  options.sweepBound = 1000.0f;
  options.cpuAverageBudgetNs = 8000.0;
  options.cpuWorstBudgetNs = 150000.0;
  expectKindConforms("eq.parametric3", options);
}

TEST(Parametric3EffectTest, NeutralIsBitExactAndAddsNoLatency) {
  expectBitExactPassThrough(kP3, {}, "defaults");
  // A band that is bypassed passes the signal whatever it is set to.
  expectBitExactPassThrough(kP3,
                            {{"b1Gain", 12}, {"b1Bypass", 1}, {"b2Gain", -15}, {"b2Q", 8},
                             {"b2Bypass", 1}, {"b3Type", 1}, {"b3Gain", 9}, {"b3Bypass", 1}},
                            "all bypassed");
}

TEST(Parametric3EffectTest, ParametersAreAsDocumented) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kP3);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->category, BlockCategory::Eq);
  const auto find = [&](const char* id) -> const BlockParamSpec* {
    for (const auto& spec : info->params)
      if (spec.id == id)
        return &spec;
    return nullptr;
  };
  for (int band = 1; band <= 3; ++band) {
    const juce::String n(band);
    ASSERT_NE(find(("b" + n + "Freq").toRawUTF8()), nullptr);
    EXPECT_FLOAT_EQ(find(("b" + n + "Freq").toRawUTF8())->min, 20.0f);
    EXPECT_FLOAT_EQ(find(("b" + n + "Freq").toRawUTF8())->max, 20000.0f);
    EXPECT_LT(find(("b" + n + "Freq").toRawUTF8())->skew, 1.0f);
    EXPECT_FLOAT_EQ(find(("b" + n + "Gain").toRawUTF8())->min, -18.0f);
    EXPECT_FLOAT_EQ(find(("b" + n + "Gain").toRawUTF8())->max, 18.0f);
    EXPECT_FLOAT_EQ(find(("b" + n + "Q").toRawUTF8())->min, 0.1f);
    EXPECT_FLOAT_EQ(find(("b" + n + "Q").toRawUTF8())->max, 10.0f);
    EXPECT_EQ(find(("b" + n + "Bypass").toRawUTF8())->type, BlockParamType::Bool);
    // Only the outer bands have a type.
    EXPECT_EQ(find(("b" + n + "Type").toRawUTF8()) != nullptr, band != 2);
  }
  EXPECT_FLOAT_EQ(find("levelDb")->min, -20.0f);
  EXPECT_FLOAT_EQ(find("levelDb")->max, 20.0f);
}

TEST(Parametric3EffectTest, ABandActsAtItsFrequencyByTheGainSet) {
  for (const double hz : {100.0, 440.0, 1000.0, 5000.0, 12000.0}) {
    for (const double q : {0.5, 1.0, 4.0}) {
      for (const double gain : {-18.0, -9.0, 6.0, 18.0}) {
        SCOPED_TRACE(testing::Message() << hz << " Hz Q " << q << " gain " << gain);
        EXPECT_NEAR(responseAt(kP3, {{"b2Freq", hz}, {"b2Q", q}, {"b2Gain", gain}}, hz), gain, 0.3);
      }
    }
  }
}

TEST(Parametric3EffectTest, QIsTheBandwidthBetweenTheHalfGainPoints) {
  const std::vector<double> freqs = logGrid(125.0, 8000.0, 96);
  for (const double q : {0.5, 1.0, 2.0, 5.0}) {
    SCOPED_TRACE(testing::Message() << "Q " << q);
    const std::vector<double> response =
        responseDb(kP3, {{"b2Freq", 1000}, {"b2Q", q}, {"b2Gain", 12}}, freqs);
    const auto [lower, upper] = crossings(freqs, response, 6.0);
    ASSERT_GT(lower, 0.0);
    ASSERT_GT(upper, 0.0);
    const double expected = 1000.0 / q;
    EXPECT_NEAR(upper - lower, expected, expected * 0.08);
    // The cut mirrors the boost.
    const std::vector<double> cut =
        responseDb(kP3, {{"b2Freq", 1000}, {"b2Q", q}, {"b2Gain", -12}}, freqs);
    std::vector<double> flipped;
    for (const double v : cut)
      flipped.push_back(-v);
    const auto [cutLower, cutUpper] = crossings(freqs, flipped, 6.0);
    EXPECT_NEAR(cutUpper - cutLower, expected, expected * 0.08);
  }
}

TEST(Parametric3EffectTest, ABandLeavesTheFarAwayAlone) {
  const Params params = {{"b2Freq", 1000}, {"b2Q", 4}, {"b2Gain", 18}};
  const std::vector<double> at = responseDb(kP3, params, {40.0, 8000.0, 16000.0});
  for (const double v : at)
    EXPECT_LT(std::abs(v), 0.3);
}

TEST(Parametric3EffectTest, OuterBandsAreShelvesWhenAskedTo) {
  for (const double gain : {12.0, 6.0, -9.0, -18.0}) {
    SCOPED_TRACE(testing::Message() << "gain " << gain);
    // Band 1 as a low shelf at 400 Hz: the full gain far below, half of it
    // (in dB) at the corner, nothing far above.
    const Params low = {{"b1Type", 1}, {"b1Freq", 400}, {"b1Q", 0.7071}, {"b1Gain", gain}};
    const std::vector<double> l = responseDb(kP3, low, {20.0, 400.0, 8000.0});
    EXPECT_NEAR(l[0], gain, 0.4);
    EXPECT_NEAR(l[1], gain / 2.0, 0.3);
    EXPECT_NEAR(l[2], 0.0, 0.3);
    // Band 3 as a high shelf at 2.5 kHz.
    const Params high = {{"b3Type", 1}, {"b3Freq", 2500}, {"b3Q", 0.7071}, {"b3Gain", gain}};
    const std::vector<double> h = responseDb(kP3, high, {60.0, 2500.0, 18000.0});
    EXPECT_NEAR(h[0], 0.0, 0.3);
    EXPECT_NEAR(h[1], gain / 2.0, 0.3);
    EXPECT_NEAR(h[2], gain, 0.6);
  }
  // The same band as a peak is a bell, not a shelf.
  EXPECT_NEAR(responseAt(kP3, {{"b1Type", 0}, {"b1Freq", 400}, {"b1Gain", 12}}, 20.0), 0.0, 0.5);
}

TEST(Parametric3EffectTest, BypassRemovesJustThatBand) {
  const Params on = {{"b1Gain", 10}, {"b1Freq", 150}, {"b2Gain", -8}, {"b2Freq", 1000}};
  Params off = on;
  off.push_back({"b2Bypass", 1});
  const std::vector<double> freqs = {150.0, 1000.0};
  const std::vector<double> a = responseDb(kP3, on, freqs);
  const std::vector<double> b = responseDb(kP3, off, freqs);
  EXPECT_NEAR(a[1], -8.0, 0.5);
  EXPECT_NEAR(b[1], 0.0, 0.5);  // band 2 gone (band 1's skirt is what is left)
  EXPECT_NEAR(a[0], b[0], 0.5);  // band 1 untouched
}

TEST(Parametric3EffectTest, OutputIsAPlainGain) {
  for (const double level : {-20.0, -6.0, 6.0, 20.0}) {
    for (const double v : responseDb(kP3, {{"levelDb", level}}, {40.0, 440.0, 3000.0, 12000.0}))
      EXPECT_NEAR(v, level, 0.01) << level;
  }
}

TEST(Parametric3EffectTest, ExtremeQsStayStableAndFinite) {
  for (const double q : {0.1, 10.0}) {
    for (const double hz : {20.0, 20000.0}) {
      DirectKind kind(kP3, {{"b2Freq", hz}, {"b2Q", q}, {"b2Gain", 18}, {"b1Type", 1},
                            {"b1Gain", 18}, {"b1Q", q}, {"b1Freq", hz}});
      const std::vector<float> out = kind.render(noise(1.0, 7, 0.25f, kRate));
      expectCleanSignal(out, 200.0f, "extreme band");
    }
  }
}

TEST(Parametric3EffectTest, MovingAGainFastDoesNotStepTheSound) {
  const std::vector<float> in = sine(1.2, 1000.0, 0.2f, kRate);
  // Gain swung between -18 and +18 every two blocks: faster than the glide.
  const std::vector<float> out = renderWithMoves(kP3, {{"b2Freq", 1000}, {"b2Q", 1.0}}, in,
                                                 [](int block, const auto& set) {
                                                   set("b2Gain", ((block / 2) % 2) ? 18.0 : -18.0);
                                                 });
  expectCleanSignal(out, 1.7f, "gain sweep");
  EXPECT_LT(maxStep(out, 0, out.size()), naturalStep(0.2f, 7.95f) * 1.1f);
}

TEST(Parametric3EffectTest, MovingFrequencyAndQDoesNotStepTheSound) {
  const std::vector<float> in = sine(1.2, 1000.0, 0.2f, kRate);
  const std::vector<float> out = renderWithMoves(
      kP3, {{"b2Gain", 12}}, in, [](int block, const auto& set) {
        set("b2Freq", (block % 2) ? 300.0 : 3000.0);
        set("b2Q", (block % 3) ? 6.0 : 0.4);
      });
  expectCleanSignal(out, 1.7f, "frequency and Q sweep");
  EXPECT_LT(maxStep(out, 0, out.size()), naturalStep(0.2f, 3.98f) * 1.1f);
}

TEST(Parametric3EffectTest, BypassingAndEngagingABandDoesNotClick) {
  const std::vector<float> in = sine(1.2, 1000.0, 0.2f, kRate);
  const std::vector<float> out = renderWithMoves(
      kP3, {{"b2Gain", 18}, {"b2Q", 0.5}}, in,
      [](int block, const auto& set) { set("b2Bypass", ((block / 4) % 2) ? 1.0 : 0.0); });
  expectCleanSignal(out, 1.7f, "bypass toggling");
  EXPECT_LT(maxStep(out, 0, out.size()), naturalStep(0.2f, 7.95f) * 1.1f);
}

// ---------------------------------------------------------------------------
// eq.parametric8
// ---------------------------------------------------------------------------

TEST(Parametric8EffectTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"b1Type", 1},  {"b1Gain", 7},  {"b2Gain", -5},  {"b2Freq", 150},
                    {"b3Gain", 6},  {"b4Gain", -8}, {"b4Q", 3},      {"b5Gain", 5},
                    {"b6Gain", -6}, {"b7Gain", 9},  {"b8Type", 2},   {"b8Freq", 14000},
                    {"levelDb", 1}};
  options.stateParams = {{"b1Type", 2},  {"b1Freq", 70},   {"b3Gain", 10}, {"b5Q", 4},
                         {"b6Bypass", 1}, {"b8Type", 1},   {"b8Gain", -9}, {"levelDb", -2}};
  options.sweepBound = 150.0f;
  options.cpuAverageBudgetNs = 20000.0;
  options.cpuWorstBudgetNs = 200000.0;
  expectKindConforms("eq.parametric8", options);
}

TEST(Parametric8EffectTest, NeutralIsBitExactAndAddsNoLatency) {
  expectBitExactPassThrough(kP8, {}, "defaults");
  // High-pass and low-pass bands are real filters, bypass takes them out.
  expectBitExactPassThrough(kP8,
                            {{"b1Type", 2}, {"b1Freq", 300}, {"b1Bypass", 1}, {"b8Type", 2},
                             {"b8Freq", 4000}, {"b8Bypass", 1}, {"b4Gain", 12}, {"b4Bypass", 1}},
                            "filters bypassed");
}

TEST(Parametric8EffectTest, ParametersAreAsDocumented) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kP8);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->category, BlockCategory::Eq);
  EXPECT_EQ(info->params.size(), 5u * 2u + 4u * 6u + 1u);
  const auto find = [&](const juce::String& id) -> const BlockParamSpec* {
    for (const auto& spec : info->params)
      if (spec.id == id)
        return &spec;
    return nullptr;
  };
  for (int band = 1; band <= 8; ++band) {
    const juce::String n(band);
    ASSERT_NE(find("b" + n + "Freq"), nullptr);
    EXPECT_FLOAT_EQ(find("b" + n + "Freq")->min, 20.0f);
    EXPECT_FLOAT_EQ(find("b" + n + "Freq")->max, 20000.0f);
    EXPECT_FLOAT_EQ(find("b" + n + "Gain")->min, -18.0f);
    EXPECT_FLOAT_EQ(find("b" + n + "Gain")->max, 18.0f);
    EXPECT_FLOAT_EQ(find("b" + n + "Q")->min, 0.1f);
    EXPECT_FLOAT_EQ(find("b" + n + "Q")->max, 10.0f);
    EXPECT_NE(find("b" + n + "Bypass"), nullptr);
    EXPECT_EQ(find("b" + n + "Type") != nullptr, band == 1 || band == 8);
  }
  EXPECT_EQ(find("b1Type")->choices.size(), 3);
  EXPECT_EQ(find("b8Type")->choices.size(), 3);
}

TEST(Parametric8EffectTest, EveryBandActsAtItsFrequencyByTheGainSet) {
  for (int band = 1; band <= 8; ++band) {
    const juce::String n(band);
    for (const double gain : {-15.0, 9.0, 18.0}) {
      const double hz = 150.0 * std::pow(1.9, band - 1);  // 150 Hz up to about 13.5 kHz
      SCOPED_TRACE(testing::Message() << "band " << band << " at " << hz << " Hz, " << gain << " dB");
      const Params params = {{"b" + n + "Freq", hz}, {"b" + n + "Gain", gain}, {"b" + n + "Q", 1.5}};
      EXPECT_NEAR(responseAt(kP8, params, hz), gain, 0.3);
    }
  }
}

TEST(Parametric8EffectTest, BandsFarApartAddUp) {
  const Params params = {{"b2Freq", 200},  {"b2Gain", 8},   {"b4Freq", 800}, {"b4Gain", -7},
                         {"b6Freq", 3200}, {"b6Gain", 6},   {"b7Freq", 9000}, {"b7Gain", -9},
                         {"b2Q", 3},       {"b4Q", 3},      {"b6Q", 3},      {"b7Q", 3}};
  const std::vector<double> at = responseDb(kP8, params, {200.0, 800.0, 3200.0, 9000.0});
  EXPECT_NEAR(at[0], 8.0, 0.8);
  EXPECT_NEAR(at[1], -7.0, 0.8);
  EXPECT_NEAR(at[2], 6.0, 0.8);
  EXPECT_NEAR(at[3], -9.0, 0.8);
}

TEST(Parametric8EffectTest, TheOuterBandsAreShelvesAndFilters) {
  // Shelves.
  const std::vector<double> low =
      responseDb(kP8, {{"b1Type", 1}, {"b1Freq", 300}, {"b1Q", 0.7071}, {"b1Gain", 10}},
                 {20.0, 300.0, 6000.0});
  EXPECT_NEAR(low[0], 10.0, 0.4);
  EXPECT_NEAR(low[1], 5.0, 0.3);
  EXPECT_NEAR(low[2], 0.0, 0.3);
  const std::vector<double> high =
      responseDb(kP8, {{"b8Type", 1}, {"b8Freq", 3000}, {"b8Q", 0.7071}, {"b8Gain", -10}},
                 {80.0, 3000.0, 17000.0});
  EXPECT_NEAR(high[0], 0.0, 0.3);
  EXPECT_NEAR(high[1], -5.0, 0.3);
  EXPECT_NEAR(high[2], -10.0, 0.7);

  // A 12 dB/oct high-pass at 500 Hz: -3 dB at the corner, about -12 an
  // octave below, flat well above. The gain control does nothing to it.
  const Params hp = {{"b1Type", 2}, {"b1Freq", 500}, {"b1Q", 0.7071}, {"b1Gain", 15}};
  const std::vector<double> h = responseDb(kP8, hp, {500.0, 250.0, 125.0, 5000.0});
  EXPECT_NEAR(h[0], -3.0, 0.3);
  EXPECT_NEAR(h[1], -12.3, 1.5);
  EXPECT_NEAR(h[2], -24.0, 2.0);
  EXPECT_NEAR(h[3], 0.0, 0.1);

  // And the 12 dB/oct low-pass at 4 kHz.
  const Params lp = {{"b8Type", 2}, {"b8Freq", 4000}, {"b8Q", 0.7071}, {"b8Gain", -15}};
  const std::vector<double> l = responseDb(kP8, lp, {4000.0, 8000.0, 400.0});
  EXPECT_NEAR(l[0], -3.0, 0.3);
  EXPECT_NEAR(l[1], -12.3, 1.5);
  EXPECT_NEAR(l[2], 0.0, 0.1);
}

TEST(Parametric8EffectTest, FilterQIsTheResonanceAtTheCorner) {
  // A high-pass at Q 4 peaks near the corner by about 20 log10(Q).
  const std::vector<double> freqs = logGrid(250.0, 1000.0, 48);
  const std::vector<double> response = responseDb(kP8, {{"b1Type", 2}, {"b1Freq", 500}, {"b1Q", 4.0}}, freqs);
  const double top = *std::max_element(response.begin(), response.end());
  EXPECT_NEAR(top, 20.0 * std::log10(4.0), 1.0);
}

TEST(Parametric8EffectTest, MovingTheBandsDoesNotStepTheSound) {
  const std::vector<float> in = sine(1.2, 1000.0, 0.2f, kRate);
  const std::vector<float> gain = renderWithMoves(
      kP8, {{"b4Freq", 1000}, {"b5Freq", 1100}}, in, [](int block, const auto& set) {
        set("b4Gain", ((block / 2) % 2) ? 18.0 : -18.0);
        set("b5Gain", ((block / 3) % 2) ? 12.0 : -12.0);
      });
  expectCleanSignal(gain, 3.0f, "gain sweep");
  EXPECT_LT(maxStep(gain, 0, gain.size()), naturalStep(0.2f, 7.95f * 3.98f) * 1.1f);

  // A high-pass corner swept across the tone.
  const std::vector<float> filter = renderWithMoves(
      kP8, {{"b1Type", 2}}, in, [](int block, const auto& set) {
        set("b1Freq", 100.0 * std::pow(2.0, (block % 40) / 4.0));
      });
  expectCleanSignal(filter, 0.4f, "corner sweep");
}

// ---------------------------------------------------------------------------
// eq.cut
// ---------------------------------------------------------------------------

namespace {

// Slope choice index and its dB per octave (0 = off).
struct Slope {
  int choice;
  double dbPerOctave;
  int order;
};
constexpr Slope kSlopes[] = {{1, 6.0, 1}, {2, 12.0, 2}, {3, 18.0, 3},
                             {4, 24.0, 4}, {5, 36.0, 6}, {6, 48.0, 8}};

// Butterworth magnitude of the given order at f / corner, as high-pass.
double butterworthHighPassDb(int order, double ratio) {
  const double p = std::pow(ratio, 2.0 * order);
  return 10.0 * std::log10(p / (1.0 + p));
}

}  // namespace

TEST(LowHighCutEffectTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"hpSlope", 4}, {"hpFreq", 120}, {"lpSlope", 5}, {"lpFreq", 6000}, {"levelDb", 1}};
  options.stateParams = {{"hpSlope", 6}, {"hpFreq", 200}, {"lpSlope", 2}, {"lpFreq", 9000},
                         {"levelDb", -3}};
  options.sweepBound = 20.0f;
  options.cpuAverageBudgetNs = 12000.0;
  options.cpuWorstBudgetNs = 150000.0;
  expectKindConforms("eq.cut", options);
}

TEST(LowHighCutEffectTest, BothOffIsBitExactAndAddsNoLatency) {
  expectBitExactPassThrough(kCut, {}, "defaults");
  expectBitExactPassThrough(kCut, {{"hpFreq", 3000}, {"lpFreq", 40}}, "off with corners set");
}

TEST(LowHighCutEffectTest, ParametersAreAsDocumented) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kCut);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->category, BlockCategory::Eq);
  juce::StringArray ids;
  for (const auto& spec : info->params)
    ids.add(spec.id);
  EXPECT_EQ(ids.joinIntoString(","), "hpSlope,hpFreq,lpSlope,lpFreq,levelDb");
  EXPECT_FLOAT_EQ(info->params[1].min, 20.0f);
  EXPECT_FLOAT_EQ(info->params[1].max, 20000.0f);
  EXPECT_FLOAT_EQ(info->params[3].min, 20.0f);
  EXPECT_FLOAT_EQ(info->params[3].max, 20000.0f);
  EXPECT_EQ(info->params[0].choices.size(), 7);
  EXPECT_EQ(info->params[0].choices[0], "Off");
  EXPECT_EQ(info->params[0].choices[6], "48 dB/oct");
}

TEST(LowHighCutEffectTest, HighPassSlopeIsItsDbPerOctave) {
  for (const Slope& slope : kSlopes) {
    SCOPED_TRACE(testing::Message() << slope.dbPerOctave << " dB/oct");
    const std::vector<double> at = responseDb(
        kCut, {{"hpSlope", slope.choice}, {"hpFreq", 1000}}, {1000.0, 500.0, 250.0, 125.0, 8000.0});
    // The corner is the -3 dB point whatever the slope.
    EXPECT_NEAR(at[0], -3.01, 0.3);
    // An octave past the corner: the nominal slope to 1.5 dB.
    EXPECT_NEAR(at[1], -slope.dbPerOctave, 1.5);
    EXPECT_NEAR(at[1], butterworthHighPassDb(slope.order, 0.5), 0.3);
    // Two octaves and three: the slope keeps going (to 3 dB, nominal).
    EXPECT_NEAR(at[2], -2.0 * slope.dbPerOctave, 3.0);
    EXPECT_NEAR(at[3], butterworthHighPassDb(slope.order, 0.125), 0.5);
    // The passband is flat.
    EXPECT_NEAR(at[4], 0.0, 0.1);
  }
}

TEST(LowHighCutEffectTest, LowPassSlopeIsItsDbPerOctave) {
  for (const Slope& slope : kSlopes) {
    SCOPED_TRACE(testing::Message() << slope.dbPerOctave << " dB/oct");
    const std::vector<double> at = responseDb(
        kCut, {{"lpSlope", slope.choice}, {"lpFreq", 1000}}, {1000.0, 2000.0, 4000.0, 125.0});
    EXPECT_NEAR(at[0], -3.01, 0.3);
    // (The corner is kept low: the bilinear transform bends the response
    // down more and more towards Nyquist, which is the right behaviour but
    // not the nominal slope.)
    EXPECT_NEAR(at[1], -slope.dbPerOctave, 1.5);
    EXPECT_NEAR(at[2], -2.0 * slope.dbPerOctave, 3.0);
    EXPECT_NEAR(at[3], 0.0, 0.1);
  }
}

TEST(LowHighCutEffectTest, BothFiltersMakeABand) {
  const Params params = {{"hpSlope", 4}, {"hpFreq", 200}, {"lpSlope", 4}, {"lpFreq", 4000}};
  const std::vector<double> at = responseDb(kCut, params, {50.0, 200.0, 1000.0, 4000.0, 16000.0});
  EXPECT_LT(at[0], -23.0);
  EXPECT_NEAR(at[1], -3.0, 0.4);
  EXPECT_NEAR(at[2], 0.0, 0.3);
  EXPECT_NEAR(at[3], -3.0, 0.4);
  EXPECT_LT(at[4], -23.0);
}

TEST(LowHighCutEffectTest, CornersWorkAtTheEndsOfTheRange) {
  EXPECT_NEAR(responseAt(kCut, {{"hpSlope", 2}, {"hpFreq", 20}}, 20.0), -3.0, 0.5);
  EXPECT_NEAR(responseAt(kCut, {{"hpSlope", 2}, {"hpFreq", 20}}, 2000.0), 0.0, 0.05);
  EXPECT_NEAR(responseAt(kCut, {{"lpSlope", 2}, {"lpFreq", 20000}}, 1000.0), 0.0, 0.05);
  // A corner at 20 kHz, near Nyquist, is a plain stable filter.
  DirectKind kind(kCut, {{"lpSlope", 6}, {"lpFreq", 20000}, {"hpSlope", 6}, {"hpFreq", 20000}});
  expectCleanSignal(kind.render(noise(1.0, 3, 0.25f, kRate)), 4.0f, "corner at the top");
}

TEST(LowHighCutEffectTest, OutputIsAPlainGain) {
  for (const double level : {-20.0, -3.0, 12.0, 20.0})
    EXPECT_NEAR(responseAt(kCut, {{"levelDb", level}}, 700.0), level, 0.01);
}

TEST(LowHighCutEffectTest, ChangingTheSlopeOrCornerDoesNotStepTheSound) {
  const std::vector<float> in = sine(1.5, 1000.0, 0.2f, kRate);
  // The slope hopped through every setting, a block at a time. A section
  // fades in and out, and the Q of those that stay glides, so a tone at the
  // corner keeps its level and no sample steps beyond the tone's own slope.
  const std::vector<float> slopes = renderWithMoves(
      kCut, {{"hpFreq", 1000}, {"lpFreq", 1000}}, in, [](int block, const auto& set) {
        set("hpSlope", 1 + (block / 3) % 6);
        set("lpSlope", 1 + (block / 5) % 6);
      });
  expectCleanSignal(slopes, 0.3f, "slope changes");
  EXPECT_LT(maxStep(slopes, 0, slopes.size()), naturalStep(0.2f, 1.0f) * 1.25f);

  // A corner swept right through the tone.
  const std::vector<float> corner = renderWithMoves(
      kCut, {{"hpSlope", 4}, {"lpSlope", 4}}, in, [](int block, const auto& set) {
        set("hpFreq", 100.0 * std::pow(2.0, (block % 32) / 3.0));
        set("lpFreq", 12000.0 / std::pow(2.0, (block % 28) / 3.0));
      });
  expectCleanSignal(corner, 0.5f, "corner sweep");
  EXPECT_LT(maxStep(corner, 0, corner.size()), naturalStep(0.2f, 1.6f) * 1.25f);

  // Switching a filter off and on.
  const std::vector<float> toggle = renderWithMoves(
      kCut, {{"hpFreq", 1000}}, in, [](int block, const auto& set) {
        set("hpSlope", ((block / 4) % 2) ? 0.0 : 6.0);
      });
  expectCleanSignal(toggle, 0.5f, "off and on");
  EXPECT_LT(maxStep(toggle, 0, toggle.size()), naturalStep(0.2f, 1.6f) * 1.25f);
}

// ---------------------------------------------------------------------------
// eq.console4
// ---------------------------------------------------------------------------

TEST(ConsoleEqEffectTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"hpOn", 1},       {"hpFreq", 90},     {"lowGain", 6},  {"lowFreq", 110},
                    {"lowMidGain", -5}, {"lowMidFreq", 350}, {"highMidGain", 7},
                    {"highMidFreq", 3000}, {"highGain", -4}, {"highFreq", 9000},
                    {"lpOn", 1},        {"lpFreq", 14000},  {"levelDb", 1}};
  options.stateParams = {{"hpOn", 1},    {"hpFreq", 150},   {"lowGain", -8},     {"highMidGain", 9},
                         {"highMidFreq", 1200}, {"highGain", 11}, {"lpOn", 1},   {"lpFreq", 5000},
                         {"levelDb", -2}};
  options.sweepBound = 60.0f;
  options.cpuAverageBudgetNs = 12000.0;
  options.cpuWorstBudgetNs = 150000.0;
  expectKindConforms("eq.console4", options);
}

TEST(ConsoleEqEffectTest, NeutralIsBitExactAndAddsNoLatency) {
  expectBitExactPassThrough(kConsole, {}, "defaults");
  // The frequencies move nothing while the gains are at zero and the
  // filters are off.
  expectBitExactPassThrough(kConsole,
                            {{"hpFreq", 400}, {"lowFreq", 150}, {"lowMidFreq", 800},
                             {"highMidFreq", 4000}, {"highFreq", 15000}, {"lpFreq", 2000}},
                            "frequencies moved");
}

TEST(ConsoleEqEffectTest, ParametersAreAsDocumented) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kConsole);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->category, BlockCategory::Eq);
  const auto find = [&](const char* id) -> const BlockParamSpec* {
    for (const auto& spec : info->params)
      if (spec.id == juce::String(id))
        return &spec;
    return nullptr;
  };
  struct Range {
    const char* id;
    float lo;
    float hi;
  };
  for (const Range& r : {Range{"hpFreq", 20, 500}, Range{"lowFreq", 20, 200},
                         Range{"lowMidFreq", 100, 1000}, Range{"highMidFreq", 500, 5000},
                         Range{"highFreq", 2000, 20000}, Range{"lpFreq", 1000, 20000},
                         Range{"lowGain", -12, 12}, Range{"lowMidGain", -12, 12},
                         Range{"highMidGain", -12, 12}, Range{"highGain", -12, 12},
                         Range{"levelDb", -20, 20}}) {
    ASSERT_NE(find(r.id), nullptr) << r.id;
    EXPECT_FLOAT_EQ(find(r.id)->min, r.lo) << r.id;
    EXPECT_FLOAT_EQ(find(r.id)->max, r.hi) << r.id;
  }
  EXPECT_EQ(find("hpOn")->type, BlockParamType::Bool);
  EXPECT_EQ(find("lpOn")->type, BlockParamType::Bool);
}

TEST(ConsoleEqEffectTest, BellsActAtTheirFrequencyByTheGainSet) {
  for (const double gain : {-12.0, -6.0, 6.0, 12.0}) {
    SCOPED_TRACE(testing::Message() << gain << " dB");
    // Across each bell's range the gain lands at the dialled frequency.
    for (const double hz : {100.0, 330.0, 1000.0})
      EXPECT_NEAR(responseAt(kConsole, {{"lowMidFreq", hz}, {"lowMidGain", gain}}, hz), gain, 0.3);
    for (const double hz : {500.0, 1600.0, 5000.0})
      EXPECT_NEAR(responseAt(kConsole, {{"highMidFreq", hz}, {"highMidGain", gain}}, hz), gain, 0.3);
  }
}

TEST(ConsoleEqEffectTest, ShelvesHaveTheirFullGainFarOutAndHalfAtTheCorner) {
  for (const double gain : {-12.0, -6.0, 6.0, 12.0}) {
    SCOPED_TRACE(testing::Message() << gain << " dB");
    const std::vector<double> low =
        responseDb(kConsole, {{"lowFreq", 150}, {"lowGain", gain}}, {20.0, 150.0, 8000.0});
    EXPECT_NEAR(low[0], gain, 0.5);
    EXPECT_NEAR(low[1], gain / 2.0, 0.3);
    EXPECT_NEAR(low[2], 0.0, 0.3);
    const std::vector<double> high =
        responseDb(kConsole, {{"highFreq", 6000}, {"highGain", gain}}, {100.0, 6000.0, 19000.0});
    EXPECT_NEAR(high[0], 0.0, 0.3);
    EXPECT_NEAR(high[1], gain / 2.0, 0.3);
    EXPECT_NEAR(high[2], gain, 0.9);
  }
}

TEST(ConsoleEqEffectTest, FiltersAreTwelveDbPerOctaveAndOffUntilSwitchedOn) {
  // Off: the frequency does nothing.
  EXPECT_NEAR(responseAt(kConsole, {{"hpFreq", 400}}, 100.0), 0.0, 1.0e-6);
  EXPECT_NEAR(responseAt(kConsole, {{"lpFreq", 2000}}, 15000.0), 0.0, 1.0e-6);

  const std::vector<double> hp =
      responseDb(kConsole, {{"hpOn", 1}, {"hpFreq", 200}}, {200.0, 100.0, 50.0, 2000.0});
  EXPECT_NEAR(hp[0], -3.0, 0.3);
  EXPECT_NEAR(hp[1], -12.3, 1.5);
  EXPECT_NEAR(hp[2], -24.0, 2.0);
  EXPECT_NEAR(hp[3], 0.0, 0.1);

  const std::vector<double> lp =
      responseDb(kConsole, {{"lpOn", 1}, {"lpFreq", 4000}}, {4000.0, 8000.0, 400.0});
  EXPECT_NEAR(lp[0], -3.0, 0.3);
  EXPECT_NEAR(lp[1], -12.3, 1.5);
  EXPECT_NEAR(lp[2], 0.0, 0.1);
}

TEST(ConsoleEqEffectTest, OutputIsAPlainGain) {
  for (const double level : {-20.0, -6.0, 6.0, 20.0})
    EXPECT_NEAR(responseAt(kConsole, {{"levelDb", level}}, 1234.0), level, 0.01);
}

TEST(ConsoleEqEffectTest, MovingTheControlsDoesNotStepTheSound) {
  const std::vector<float> in = sine(1.2, 1000.0, 0.2f, kRate);
  const std::vector<float> out = renderWithMoves(
      kConsole, {{"lowMidFreq", 900}, {"highMidFreq", 1100}}, in, [](int block, const auto& set) {
        set("lowMidGain", ((block / 2) % 2) ? 12.0 : -12.0);
        set("highMidGain", ((block / 3) % 2) ? 12.0 : -12.0);
        set("hpOn", ((block / 7) % 2) ? 1.0 : 0.0);
        set("hpFreq", 100.0 + 20.0 * (block % 20));
      });
  expectCleanSignal(out, 3.0f, "console moves");
  EXPECT_LT(maxStep(out, 0, out.size()), naturalStep(0.2f, 3.98f * 3.98f) * 1.1f);
}

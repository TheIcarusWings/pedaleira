// The creative effects (plugin/src/effects/creative): filter.bitcrush,
// filter.ringmod, filter.freeze and filter.glitch (Stutter). Each has the
// conformance test, then what the effect is for, measured with numbers that
// are printed as "[   FX     ]" lines:
//
//   bit crusher     the quantisation noise of the stated bit depth (Δ²/12 with
//                   Δ = 2^(1 - bits)), the hold length of the rate reduction,
//                   the alias the anti-alias switch removes
//   ring modulator  sidebands at f + c and f - c and nothing at f or c, the
//                   odd-harmonic sidebands of the square and the triangle,
//                   the range the LFO sweeps the carrier through
//   freeze          the pad's level over seconds and its spread, a level that
//                   does not dip through the cross-fade, no steps in the
//                   output at engage, release and re-engage
//   stutter         the repeats start on the sample the cell starts and
//                   are the audio from just before it, in reverse, a
//                   semitone-step lower per repeat, at two tempos and from
//                   the host's song position
//
// All of them run on the kind alone (DirectKind: no mix, no DC blocker) so
// the numbers are the effect's own.
#include "effects/allocation_guard.h"
#include "effects/effect_test_helpers.h"
#include "effects/modulation_test_helpers.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace fxtest;

namespace {

constexpr double kSr = 48000.0;

void setParam(DirectKind& kind, const char* id, double value) {
  const int index = kind.engine().findParameter(id);
  ASSERT_GE(index, 0) << id;
  kind.engine().setParameter(index, static_cast<float>(value));
}

std::vector<float> append(std::vector<float> a, const std::vector<float>& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

std::vector<float> zeros(double seconds) {
  return std::vector<float>(static_cast<size_t>(std::lround(seconds * kSr)), 0.0f);
}

double correlation(const std::vector<float>& a, size_t aBegin, const std::vector<float>& b,
                   size_t bBegin, size_t n) {
  double ab = 0.0, aa = 0.0, bb = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double x = a[aBegin + i], y = b[bBegin + i];
    ab += x * y;
    aa += x * x;
    bb += y * y;
  }
  return ab / std::sqrt(std::max(aa * bb, 1.0e-30));
}

double maxAbsDifference(const std::vector<float>& a, size_t aBegin, const std::vector<float>& b,
                        size_t bBegin, size_t n) {
  double worst = 0.0;
  for (size_t i = 0; i < n; ++i)
    worst = std::max(worst, std::abs(static_cast<double>(a[aBegin + i]) - b[bBegin + i]));
  return worst;
}

// RMS in dB of each `windowSeconds` of x between [fromSeconds, toSeconds).
std::vector<double> windowedRmsDb(const std::vector<float>& x, double fromSeconds,
                                  double toSeconds, double windowSeconds = 0.1) {
  std::vector<double> out;
  const size_t window = static_cast<size_t>(windowSeconds * kSr);
  for (size_t begin = static_cast<size_t>(fromSeconds * kSr);
       begin + window <= std::min(x.size(), static_cast<size_t>(toSeconds * kSr));
       begin += window)
    out.push_back(toDb(rmsOf(x, begin, begin + window)));
  return out;
}

double spreadOf(const std::vector<double>& values) {
  return *std::max_element(values.begin(), values.end()) -
         *std::min_element(values.begin(), values.end());
}

double meanOf(const std::vector<double>& values) {
  double sum = 0.0;
  for (const double v : values)
    sum += v;
  return sum / static_cast<double>(values.size());
}

void printFx(const std::string& text) { std::cout << "[   FX     ] " << text << std::endl; }

// Allocations made by operator new on this thread while the kind processes
// `blocks` blocks of noise with `eventually` switched on a third of the way
// in (the capture of a freeze, say) and off two thirds in.
long allocationsWhileProcessing(const juce::String& kindId, const Params& params,
                                const char* toggled = nullptr, Layout layout = Layout::Stereo) {
  DirectKind kind(kindId, params, layout);
  const int channels = layout == Layout::Mono ? 1 : 2;
  const int blocks = 400;
  const std::vector<float> source = noise(1.0, 5u, 0.25f, kSr);
  juce::AudioBuffer<float> buffer(channels, 256);
  buffer.clear();
  kind.processor->processInChain(buffer, kind.ctx);  // anything lazy happens here
  long total = 0;
  for (int b = 0; b < blocks; ++b) {
    for (int ch = 0; ch < channels; ++ch)
      buffer.copyFrom(ch, 0, source.data() + (static_cast<size_t>(b) * 256 % (source.size() - 256)),
                      256);
    if (toggled != nullptr && (b == blocks / 3 || b == 2 * blocks / 3)) {
      const int index = kind.engine().findParameter(toggled);
      kind.engine().setParameter(index, b == blocks / 3 ? 1.0f : 0.0f);
    }
    AllocationGuard guard;
    kind.processor->processInChain(buffer, kind.ctx);
    total += guard.count();
  }
  return total;
}

// Everything every creative kind owes besides the conformance test: reset
// clears the state (silence in after a reset is silence out, and the kind
// renders what a new instance renders), and nothing allocates in process.
void expectResetClearsState(const juce::String& kindId, const Params& params, const char* toggled) {
  const std::vector<float> in = noise(1.5, 3u, 0.3f, kSr);
  // The first instance is played, reset and fed silence: nothing is left.
  {
    DirectKind kind(kindId, params, Layout::Mono);
    if (toggled != nullptr)
      setParam(kind, toggled, 1.0);
    kind.render(in);
    kind.engine().reset();
    if (toggled != nullptr)
      setParam(kind, toggled, 0.0);
    EXPECT_EQ(peakOf(kind.render(zeros(2.0))), 0.0f) << kindId << ": still sounding after a reset";
  }
  // The second is played and reset and then renders exactly what a new
  // instance does.
  DirectKind kind(kindId, params, Layout::Mono);
  DirectKind fresh(kindId, params, Layout::Mono);
  if (toggled != nullptr)
    setParam(kind, toggled, 1.0);
  kind.render(in);
  kind.engine().reset();
  if (toggled != nullptr)
    setParam(kind, toggled, 0.0);
  EXPECT_EQ(kind.render(in), fresh.render(in))
      << kindId << ": a reset instance does not render like a new one";
}

}  // namespace

// ---------------------------------------------------------------------------
// Bit crusher
// ---------------------------------------------------------------------------

namespace {
constexpr const char* kCrush = "filter.bitcrush";

// Noise power of a bit depth in dB relative to a full scale sine's power
// (1/2): Δ²/12 against it, with Δ = 2^(1 - bits).
double expectedNoiseRmsDb(double bits) {
  const double delta = std::pow(2.0, 1.0 - bits);
  return toDb(delta / std::sqrt(12.0));
}
}  // namespace

TEST(BitCrusherTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"bits", 6.0}, {"rateHz", 16000.0}};
  options.stateParams = {{"bits", 5.5}, {"rateHz", 9000.0}, {"antiAlias", 1.0}};
  options.cpuAverageBudgetNs = 8000.0;
  options.cpuWorstBudgetNs = 300000.0;
  expectKindConforms("filter.bitcrush", options);
}

TEST(BitCrusherTest, RegisteredFullyWet) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kCrush);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->category, BlockCategory::Filter);
  EXPECT_FLOAT_EQ(info->defaultMix, 1.0f);
}

TEST(BitCrusherTest, QuantisationNoiseMatchesTheBitDepth) {
  // A 997 Hz sine at 0.8 spans many steps at every depth tested, so the
  // rounding error is the white noise of a uniform quantiser.
  const std::vector<float> in = sine(2.0, 997.0, 0.8f, kSr);
  for (const double bits : {4.0, 6.0, 8.0, 10.0, 12.0, 16.0}) {
    SCOPED_TRACE(bits);
    DirectKind kind(kCrush, {{"bits", bits}, {"rateHz", 48000.0}});
    const std::vector<float> out = kind.render(in);
    std::vector<float> error(in.size());
    for (size_t i = 0; i < in.size(); ++i)
      error[i] = out[i] - in[i];
    const double measured = toDb(rmsOf(error));
    const double expected = expectedNoiseRmsDb(bits);
    printFx("bit crusher " + std::to_string(static_cast<int>(bits)) + " bits: noise " +
            juce::String(measured, 2).toStdString() + " dBFS rms, expected " +
            juce::String(expected, 2).toStdString() + " dBFS, signal to noise " +
            juce::String(toDb(rmsOf(in)) - measured, 1).toStdString() + " dB");
    EXPECT_NEAR(measured, expected, 0.7);
  }
}

TEST(BitCrusherTest, FractionalBitsLieBetweenTheirNeighbours) {
  const std::vector<float> in = sine(2.0, 997.0, 0.8f, kSr);
  const auto noiseDb = [&](double bits) {
    DirectKind kind(kCrush, {{"bits", bits}, {"rateHz", 48000.0}});
    const std::vector<float> out = kind.render(in);
    std::vector<float> error(in.size());
    for (size_t i = 0; i < in.size(); ++i)
      error[i] = out[i] - in[i];
    return toDb(rmsOf(error));
  };
  const double seven = noiseDb(7.0), seven5 = noiseDb(7.5), eight = noiseDb(8.0);
  printFx("bit crusher 7 / 7.5 / 8 bits: " + juce::String(seven, 2).toStdString() + " / " +
          juce::String(seven5, 2).toStdString() + " / " + juce::String(eight, 2).toStdString() +
          " dBFS");
  EXPECT_GT(seven, seven5);
  EXPECT_GT(seven5, eight);
  EXPECT_NEAR(seven5, expectedNoiseRmsDb(7.5), 0.7);
  // Three dB per half bit.
  EXPECT_NEAR(seven - seven5, 3.01, 0.7);
}

TEST(BitCrusherTest, SilenceStaysSilentAtOneBit) {
  DirectKind kind(kCrush, {{"bits", 1.0}, {"rateHz", 3000.0}});
  EXPECT_EQ(peakOf(kind.render(zeros(0.5))), 0.0f);
}

TEST(BitCrusherTest, RateReductionHoldsEachSampleForTheRatio) {
  const std::vector<float> in = noise(1.0, 9u, 0.5f, kSr);
  // Whole ratios: 48000 / 12000 = 4 and 48000 / 16000 = 3.
  for (const auto& [rate, hold] : {std::pair<double, int>{12000.0, 4}, {16000.0, 3}, {24000.0, 2}}) {
    SCOPED_TRACE(rate);
    DirectKind kind(kCrush, {{"bits", 16.0}, {"rateHz", rate}});
    const std::vector<float> out = kind.render(in);
    int wrong = 0;
    for (size_t n = 0; n < in.size(); ++n) {
      const size_t head = n - n % static_cast<size_t>(hold);
      if (out[n] != out[head])
        ++wrong;
      // The held value is the input at the head of the hold, to 16 bits.
      if (std::abs(out[head] - in[head]) > 1.0f / 32768.0f)
        ++wrong;
    }
    printFx("bit crusher " + std::to_string(static_cast<int>(rate)) + " Hz: hold " +
            std::to_string(hold) + " samples, " + std::to_string(wrong) + " wrong of " +
            std::to_string(in.size()));
    EXPECT_EQ(wrong, 0);
  }
}

TEST(BitCrusherTest, AntiAliasRemovesTheFoldedPartial) {
  // 10 kHz sampled at 12 kHz folds to 2 kHz.
  const std::vector<float> in = sine(1.0, 10000.0, 0.5f, kSr);
  const auto alias = [&](double antiAlias) {
    DirectKind kind(kCrush, {{"bits", 16.0}, {"rateHz", 12000.0}, {"antiAlias", antiAlias}});
    const std::vector<float> out = kind.render(in);
    return db(goertzelPower(out.data() + 4800, out.size() - 4800, 2000.0, kSr));
  };
  const double without = alias(0.0), with = alias(1.0);
  printFx("bit crusher alias at 2 kHz: " + juce::String(without, 1).toStdString() +
          " dB without, " + juce::String(with, 1).toStdString() + " dB with anti-alias, " +
          juce::String(without - with, 1).toStdString() + " dB removed");
  EXPECT_GT(without - with, 15.0);
}

TEST(BitCrusherTest, ResetClearsStateAndProcessingDoesNotAllocate) {
  expectResetClearsState(kCrush, {{"bits", 5.0}, {"rateHz", 4000.0}, {"antiAlias", 1.0}}, nullptr);
  for (const Layout layout : {Layout::Mono, Layout::Stereo})
    EXPECT_EQ(allocationsWhileProcessing(kCrush, {{"bits", 5.0}, {"antiAlias", 1.0}}, nullptr,
                                         layout),
              0);
}

TEST(BitCrusherTest, CostIsReported) {
  const KindCost cost = measureKindCost(kCrush, {{"bits", 6.0}, {"rateHz", 12000.0}, {"antiAlias", 1.0}});
  printFx("cpu filter.bitcrush (anti-alias on): " +
          juce::String(cost.averageNs, 0).toStdString() + " ns average, " +
          juce::String(cost.worstNs, 0).toStdString() + " ns worst per 128-sample block");
  SUCCEED();
}

// ---------------------------------------------------------------------------
// Ring modulator
// ---------------------------------------------------------------------------

namespace {
constexpr const char* kRing = "filter.ringmod";

double levelAt(const std::vector<float>& x, double hz) {
  return goertzelPower(x.data() + 4800, x.size() - 4800, hz, kSr);
}
}  // namespace

TEST(RingModulatorTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"frequencyHz", 300.0}, {"shape", 1.0}, {"lfoDepth", 3.0}};
  options.stateParams = {{"frequencyHz", 123.0}, {"shape", 2.0}, {"lfoDepth", 5.0},
                         {"lfoRateHz", 3.3}};
  options.cpuAverageBudgetNs = 12000.0;
  options.cpuWorstBudgetNs = 300000.0;
  expectKindConforms("filter.ringmod", options);
}

TEST(RingModulatorTest, Registered) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kRing);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->category, BlockCategory::Filter);
}

TEST(RingModulatorTest, SineCarrierMakesSumAndDifferenceTones) {
  for (const auto& [input, carrier] :
       {std::pair<double, double>{1000.0, 300.0}, {1500.0, 4000.0}, {700.0, 90.0}}) {
    SCOPED_TRACE(carrier);
    const std::vector<float> in = sine(2.0, input, 0.5f, kSr);
    DirectKind kind(kRing, {{"frequencyHz", carrier}, {"shape", 0.0}});
    const std::vector<float> out = kind.render(in);
    const double reference = levelAt(in, input);
    const double sum = db(levelAt(out, input + carrier)) - db(reference);
    const double difference = db(levelAt(out, std::abs(input - carrier))) - db(reference);
    const double inputLeft = db(levelAt(out, input)) - db(reference);
    printFx("ring modulator " + std::to_string(static_cast<int>(input)) + " Hz x " +
            juce::String(carrier, 1).toStdString() + " Hz: f+c " +
            juce::String(sum, 2).toStdString() + " dB, |f-c| " +
            juce::String(difference, 2).toStdString() + " dB, f " +
            juce::String(inputLeft, 1).toStdString() + " dB");
    // Each sideband is half the input's amplitude: -6.02 dB.
    EXPECT_NEAR(sum, -6.02, 0.3);
    EXPECT_NEAR(difference, -6.02, 0.3);
    EXPECT_LT(inputLeft, -50.0) << "the input itself must be gone";
  }
}

TEST(RingModulatorTest, SquareAndTriangleCarriersAddTheirOddHarmonics) {
  const double input = 1000.0, carrier = 300.0;
  const std::vector<float> in = sine(2.0, input, 0.5f, kSr);
  const double reference = db(levelAt(in, input));
  // Amplitude of the nth harmonic of each carrier (n odd), as a fraction of 1.
  const auto squareHarmonic = [](int n) { return 4.0 / (3.14159265358979 * n); };
  const auto triangleHarmonic = [](int n) { return 8.0 / (3.14159265358979 * 3.14159265358979 * n * n); };
  for (const bool square : {true, false}) {
    DirectKind kind(kRing, {{"frequencyHz", carrier}, {"shape", square ? 1.0 : 2.0}});
    const std::vector<float> out = kind.render(in);
    for (const int n : {1, 3, 5}) {
      const double amplitude = square ? squareHarmonic(n) : triangleHarmonic(n);
      // The sideband of the nth harmonic has half that amplitude.
      const double expected = toDb(0.5 * amplitude);
      const double upper = db(levelAt(out, input + n * carrier)) - reference;
      const double lower = db(levelAt(out, std::abs(input - n * carrier))) - reference;
      printFx(std::string(square ? "square" : "triangle") + " carrier harmonic " +
              std::to_string(n) + ": upper " + juce::String(upper, 2).toStdString() +
              " dB, lower " + juce::String(lower, 2).toStdString() + " dB, expected " +
              juce::String(expected, 2).toStdString() + " dB");
      EXPECT_NEAR(upper, expected, 0.6);
      EXPECT_NEAR(lower, expected, 0.6);
    }
    // No even harmonics.
    EXPECT_LT(db(levelAt(out, input + 2 * carrier)) - reference, -45.0);
  }
}

TEST(RingModulatorTest, TheLfoSweepsTheCarrierByTheStatedSemitones) {
  // A steady 1.0 in leaves the carrier itself.
  DirectKind kind(kRing, {{"frequencyHz", 1000.0}, {"shape", 0.0}, {"lfoDepth", 12.0},
                          {"lfoRateHz", 2.0}});
  const std::vector<float> out =
      kind.render(std::vector<float>(static_cast<size_t>(4.0 * kSr), 1.0f));
  const auto cycles = modtest::cycleFrequencies(out, kSr, 100);
  ASSERT_GT(cycles.hz.size(), 100u);
  const double lowest = *std::min_element(cycles.hz.begin(), cycles.hz.end());
  const double highest = *std::max_element(cycles.hz.begin(), cycles.hz.end());
  const double rate = frequencyModulationRateHz(out, kSr, 100, 0.05);
  printFx("ring modulator LFO 12 st at 2 Hz: carrier " + juce::String(lowest, 0).toStdString() +
          " to " + juce::String(highest, 0).toStdString() + " Hz, sweep rate " +
          juce::String(rate, 3).toStdString() + " Hz");
  EXPECT_NEAR(lowest, 500.0, 25.0);
  EXPECT_NEAR(highest, 2000.0, 100.0);
  EXPECT_NEAR(rate, 2.0, 0.1);
}

TEST(RingModulatorTest, CarrierRangeReachesFourKilohertz) {
  const std::vector<float> ones(static_cast<size_t>(kSr), 1.0f);
  for (const double hz : {1.0, 50.0, 4000.0}) {
    DirectKind kind(kRing, {{"frequencyHz", hz}, {"shape", 0.0}});
    const std::vector<float> out = kind.render(ones);
    const double expected = hz;
    // Count of upward zero crossings over one second.
    int crossings = 0;
    for (size_t i = 1; i < out.size(); ++i)
      if (out[i - 1] < 0.0f && out[i] >= 0.0f)
        ++crossings;
    EXPECT_NEAR(crossings, expected, hz > 1.5 ? 1.5 : 1.01) << hz;
    EXPECT_NEAR(peakOf(out), 1.0f, 1.0e-3f);
  }
}

TEST(RingModulatorTest, ResetClearsStateAndProcessingDoesNotAllocate) {
  expectResetClearsState(kRing, {{"frequencyHz", 700.0}, {"lfoDepth", 7.0}}, nullptr);
  for (const Layout layout : {Layout::Mono, Layout::Stereo})
    EXPECT_EQ(allocationsWhileProcessing(kRing, {{"shape", 1.0}, {"lfoDepth", 6.0}}, nullptr, layout), 0);
}

TEST(RingModulatorTest, CostIsReported) {
  for (const double shape : {0.0, 1.0}) {
    const KindCost cost = measureKindCost(kRing, {{"shape", shape}, {"lfoDepth", 4.0}});
    printFx(std::string("cpu filter.ringmod (") + (shape == 0.0 ? "sine" : "square") +
            ", LFO on): " + juce::String(cost.averageNs, 0).toStdString() + " ns average, " +
            juce::String(cost.worstNs, 0).toStdString() + " ns worst per 128-sample block");
  }
  SUCCEED();
}

// ---------------------------------------------------------------------------
// Freeze
// ---------------------------------------------------------------------------

namespace {
constexpr const char* kFreeze = "filter.freeze";
}

TEST(FreezeTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"grainMs", 200.0}, {"fadeInMs", 50.0}, {"fadeOutMs", 100.0}};
  options.stateParams = {{"freeze", 1.0}, {"grainMs", 420.0}, {"fadeInMs", 80.0},
                         {"fadeOutMs", 900.0}, {"levelDb", -3.0}, {"dryPercent", 25.0}};
  options.cpuAverageBudgetNs = 20000.0;
  options.cpuWorstBudgetNs = 600000.0;
  expectKindConforms("filter.freeze", options);
}

TEST(FreezeTest, PassesTheInputUntouchedUntilTriggered) {
  DirectKind kind(kFreeze, {}, Layout::Mono);
  const std::vector<float> in = musicalSignal(1.0, kSr);
  EXPECT_EQ(kind.render(in), in);
}

TEST(FreezeTest, SustainsAStableLevelFromNoise) {
  DirectKind kind(kFreeze, {{"grainMs", 300.0}, {"fadeInMs", 50.0}, {"fadeOutMs", 500.0}},
                  Layout::Mono);
  const std::vector<float> burst = noise(1.0, 17u, 0.2f, kSr);
  std::vector<float> out = kind.render(burst);
  setParam(kind, "freeze", 1.0);
  out = append(out, kind.render(zeros(6.0)));
  const double inputDb = toDb(rmsOf(burst, static_cast<size_t>(0.7 * kSr)));
  const std::vector<double> levels = windowedRmsDb(out, 1.2, 7.0);
  const double spread = spreadOf(levels);
  printFx("freeze, noise burst, 6 s of pad: input " + juce::String(inputDb, 2).toStdString() +
          " dBFS rms, pad mean " + juce::String(meanOf(levels), 2).toStdString() +
          " dBFS, spread over " + std::to_string(levels.size()) + " windows of 100 ms " +
          juce::String(spread, 2).toStdString() + " dB");
  EXPECT_LT(spread, 1.0);
  EXPECT_NEAR(meanOf(levels), inputDb, 1.0);
}

TEST(FreezeTest, SustainsAStableLevelFromAChord) {
  DirectKind kind(kFreeze, {{"grainMs", 400.0}, {"fadeInMs", 50.0}, {"fadeOutMs", 500.0}},
                  Layout::Mono);
  const std::vector<float> chord = musicalSignal(1.0, kSr, 0.5f);
  std::vector<float> out = kind.render(chord);
  setParam(kind, "freeze", 1.0);
  out = append(out, kind.render(zeros(6.0)));
  const std::vector<double> levels = windowedRmsDb(out, 1.2, 7.0, 0.2);
  const double inputDb = toDb(rmsOf(chord, static_cast<size_t>(0.6 * kSr)));
  printFx("freeze, five-partial chord, 6 s of pad: input " + juce::String(inputDb, 2).toStdString() +
          " dBFS rms, pad mean " + juce::String(meanOf(levels), 2).toStdString() +
          " dBFS, spread over windows of 200 ms " + juce::String(spreadOf(levels), 2).toStdString() +
          " dB");
  // A tone follows itself from grain to grain (the sine below holds within
  // half a dB); a chord of partials a tenth of a second apart in period does
  // not, and beats by a few dB.
  EXPECT_LT(spreadOf(levels), 6.0);
  EXPECT_NEAR(meanOf(levels), inputDb, 3.0);
}

TEST(FreezeTest, PadHoldsTheSpectrumOfTheCapturedMoment) {
  DirectKind kind(kFreeze, {{"grainMs", 300.0}, {"fadeInMs", 20.0}}, Layout::Mono);
  kind.render(sine(1.0, 1000.0, 0.3f, kSr));
  setParam(kind, "freeze", 1.0);
  const std::vector<float> pad = kind.render(zeros(3.0));
  const std::vector<double> levels = windowedRmsDb(pad, 0.5, 3.0, 0.1);
  const double onTone = db(goertzelPower(pad.data() + 24000, 96000, 1000.0, kSr));
  const double offTone = db(goertzelPower(pad.data() + 24000, 96000, 1500.0, kSr));
  printFx("freeze, 1 kHz sine: pad spread " + juce::String(spreadOf(levels), 2).toStdString() +
          " dB in 100 ms windows, 1 kHz is " + juce::String(onTone - offTone, 1).toStdString() +
          " dB above 1.5 kHz");
  EXPECT_GT(onTone - offTone, 30.0);
  EXPECT_LT(spreadOf(levels), 2.0);
}

TEST(FreezeTest, LevelDoesNotDipThroughTheCrossFade) {
  // Noise keeps playing through the engage: the pad (another stretch of the
  // same noise) takes over from the live signal in equal power.
  DirectKind kind(kFreeze, {{"grainMs", 300.0}, {"fadeInMs", 800.0}, {"fadeOutMs", 800.0}},
                  Layout::Mono);
  const std::vector<float> in = noise(8.0, 31u, 0.2f, kSr);
  const auto slice = [&](double from, double to) {
    return std::vector<float>(in.begin() + static_cast<std::ptrdiff_t>(from * kSr),
                              in.begin() + static_cast<std::ptrdiff_t>(to * kSr));
  };
  std::vector<float> out = kind.render(slice(0.0, 2.0));
  setParam(kind, "freeze", 1.0);
  out = append(out, kind.render(slice(2.0, 5.0)));
  setParam(kind, "freeze", 0.0);
  out = append(out, kind.render(slice(5.0, 8.0)));
  const std::vector<double> levels = windowedRmsDb(out, 0.2, 8.0);
  printFx("freeze, equal power cross-fade (engage at 2 s over 800 ms, release at 5 s over 800 ms): "
          "level spread " + juce::String(spreadOf(levels), 2).toStdString() + " dB over " +
          std::to_string(levels.size()) + " windows");
  EXPECT_LT(spreadOf(levels), 1.2);
}

TEST(FreezeTest, EngagingReleasingAndReengagingMakesNoSteps) {
  DirectKind kind(kFreeze, {{"grainMs", 300.0}, {"fadeInMs", 100.0}, {"fadeOutMs", 100.0}},
                  Layout::Mono);
  const std::vector<float> in = sine(5.0, 440.0, 0.3f, kSr);
  const float inputStep = maxStep(in, 0, in.size());
  const auto part = [&](double from, double to) {
    return std::vector<float>(in.begin() + static_cast<std::ptrdiff_t>(from * kSr),
                              in.begin() + static_cast<std::ptrdiff_t>(to * kSr));
  };
  std::vector<float> out = kind.render(part(0.0, 1.0));
  setParam(kind, "freeze", 1.0);
  out = append(out, kind.render(part(1.0, 2.5)));
  setParam(kind, "freeze", 0.0);
  out = append(out, kind.render(part(2.5, 2.6)));
  setParam(kind, "freeze", 1.0);  // again, while the first pad is still fading
  out = append(out, kind.render(part(2.6, 4.0)));
  setParam(kind, "freeze", 0.0);
  out = append(out, kind.render(part(4.0, 5.0)));
  const float step = maxStep(out, 0, out.size());
  printFx("freeze, sine with engage, release, re-engage while fading and release: largest step " +
          juce::String(step, 5).toStdString() + ", the input's own largest step " +
          juce::String(inputStep, 5).toStdString() + " (ratio " +
          juce::String(step / inputStep, 2).toStdString() + ")");
  EXPECT_LT(step, 2.0f * inputStep);
  expectCleanSignal(out, 1.0f, "freeze click test");
}

TEST(FreezeTest, ReleaseEndsAfterTheFadeOutTime) {
  DirectKind kind(kFreeze, {{"grainMs", 200.0}, {"fadeInMs", 50.0}, {"fadeOutMs", 300.0}},
                  Layout::Mono);
  kind.render(noise(1.0, 41u, 0.3f, kSr));
  setParam(kind, "freeze", 1.0);
  kind.render(zeros(1.0));
  setParam(kind, "freeze", 0.0);
  const std::vector<float> after = kind.render(zeros(1.0));
  const double soundingFor = decayTimeSeconds(after, 0, peakOf(after), kSr, -120.0);
  printFx("freeze release with a 300 ms fade out: sounds for " +
          juce::String(soundingFor * 1000.0, 1).toStdString() + " ms; reported tail " +
          juce::String(kind.tailSeconds() * 1000.0, 1).toStdString() + " ms");
  EXPECT_NEAR(soundingFor, 0.3, 0.01);
  EXPECT_GE(kind.tailSeconds(), 0.3);
}

TEST(FreezeTest, LevelAndDryParameters) {
  const auto padRms = [](double levelDb, double dry, std::vector<float>* live = nullptr,
                         std::vector<float>* wet = nullptr) {
    DirectKind kind(kFreeze, {{"grainMs", 300.0}, {"fadeInMs", 20.0}, {"levelDb", levelDb},
                              {"dryPercent", dry}},
                    Layout::Mono);
    kind.render(noise(1.0, 51u, 0.2f, kSr));
    setParam(kind, "freeze", 1.0);
    const std::vector<float> keepPlaying = noise(2.0, 52u, 0.2f, kSr);
    const std::vector<float> out = kind.render(keepPlaying);
    if (live != nullptr)
      *live = keepPlaying;
    if (wet != nullptr)
      *wet = out;
    return rmsOf(out, static_cast<size_t>(0.5 * kSr));
  };
  const double zero = toDb(padRms(0.0, 0.0)), minusSix = toDb(padRms(-6.0, 0.0)),
               plusSix = toDb(padRms(6.0, 0.0));
  printFx("freeze level -6 / 0 / +6 dB: pad " + juce::String(minusSix, 2).toStdString() + " / " +
          juce::String(zero, 2).toStdString() + " / " + juce::String(plusSix, 2).toStdString() +
          " dBFS rms");
  EXPECT_NEAR(zero - minusSix, 6.0, 0.4);
  EXPECT_NEAR(plusSix - zero, 6.0, 0.4);

  std::vector<float> liveA, outA, liveB, outB;
  padRms(0.0, 0.0, &liveA, &outA);
  padRms(0.0, 100.0, &liveB, &outB);
  const size_t from = static_cast<size_t>(0.5 * kSr);
  const double withoutDry = correlation(outA, from, liveA, from, outA.size() - from);
  const double withDry = correlation(outB, from, liveB, from, outB.size() - from);
  printFx("freeze, correlation of the output with the live input: dry 0% " +
          juce::String(withoutDry, 3).toStdString() + ", dry 100% " +
          juce::String(withDry, 3).toStdString() + " (equal live and pad power gives 0.707)");
  EXPECT_LT(std::abs(withoutDry), 0.1);
  EXPECT_NEAR(withDry, 0.707, 0.05);
}

TEST(FreezeTest, StereoImageIsWiderThanMonoAndDualMonoStaysTwoMonos) {
  const std::vector<float> burst = noise(1.0, 61u, 0.2f, kSr);
  const auto padOf = [&](Layout layout) {
    DirectKind kind(kFreeze, {{"grainMs", 400.0}, {"fadeInMs", 20.0}}, layout);
    kind.render(Stereo{burst, burst});
    setParam(kind, "freeze", 1.0);
    return kind.render(Stereo{zeros(3.0), zeros(3.0)});
  };
  const size_t from = static_cast<size_t>(0.5 * kSr);
  const Stereo stereo = padOf(Layout::Stereo);
  const double stereoCorrelation =
      correlation(stereo.left, from, stereo.right, from, stereo.size() - from);
  const Stereo dual = padOf(Layout::DualMono);
  const Stereo mono = padOf(Layout::Mono);
  printFx("freeze pad left/right correlation: true stereo " +
          juce::String(stereoCorrelation, 3).toStdString() + ", dual mono " +
          juce::String(correlation(dual.left, from, dual.right, from, dual.size() - from), 3)
              .toStdString());
  EXPECT_LT(stereoCorrelation, 0.5);
  EXPECT_EQ(dual.left, dual.right);
  EXPECT_EQ(dual.left, mono.left);
}

TEST(FreezeTest, TheSwitchIsAssignableToScenes) {
  // Scene A leaves the pad off, scene B holds it: selecting B freezes the
  // sound that was playing, selecting A lets it go.
  EffectRig rig(kFreeze, {{"grainMs", 200.0}, {"fadeInMs", 20.0}, {"fadeOutMs", 50.0}});
  const juce::String id(rig.blockId());
  rig.must("scene.add");
  rig.must("scene.assignParam", "{\"blockId\":\"" + id + "\",\"param\":\"freeze\"}");
  rig.must("scene.setParamValue",
           "{\"scene\":\"B\",\"blockId\":\"" + id + "\",\"param\":\"freeze\",\"value\":1}");
  rig.restart();

  const std::vector<float> burst = noise(1.0, 91u, 0.2f, kSr);
  const std::vector<float> played = rig.render(burst);
  EXPECT_GT(rmsOf(played, 24000), 0.08) << "scene A passes the sound";
  rig.must("scene.select", "{\"scene\":\"B\"}");
  const std::vector<float> held = rig.render(zeros(1.0));
  const double heldRms = rmsOf(held, 12000);
  rig.must("scene.select", "{\"scene\":\"A\"}");
  const std::vector<float> released = rig.render(zeros(1.0));
  const double releasedPeak = peakOf(released, 24000);
  printFx("freeze assigned to scenes: scene A passes " + juce::String(toDb(rmsOf(played, 24000)), 1).toStdString() +
          " dBFS, scene B holds " + juce::String(toDb(heldRms), 1).toStdString() +
          " dBFS rms after the input stopped, back in scene A the peak is " +
          juce::String(releasedPeak, 6).toStdString());
  EXPECT_GT(heldRms, 0.08);
  EXPECT_LT(releasedPeak, 2.0e-3);
}

TEST(FreezeTest, ResetClearsStateAndProcessingDoesNotAllocate) {
  expectResetClearsState(kFreeze, {{"grainMs", 250.0}, {"fadeOutMs", 200.0}}, "freeze");
  for (const Layout layout : {Layout::Mono, Layout::Stereo})
    EXPECT_EQ(allocationsWhileProcessing(kFreeze, {{"grainMs", 250.0}}, "freeze", layout), 0);
}

TEST(FreezeTest, CostIsReported) {
  const KindCost idle = measureKindCost(kFreeze, {});
  const KindCost frozen = measureKindCost(kFreeze, {{"freeze", 1.0}});
  printFx("cpu filter.freeze: " + juce::String(idle.averageNs, 0).toStdString() +
          " ns average idle, " + juce::String(frozen.averageNs, 0).toStdString() +
          " ns average frozen (" + juce::String(frozen.worstNs, 0).toStdString() +
          " ns worst) per 128-sample block");
  SUCCEED();
}

// ---------------------------------------------------------------------------
// Stutter
// ---------------------------------------------------------------------------

namespace {
constexpr const char* kStutter = "filter.glitch";

// Slice 1/8 (12000 samples at 120 BPM), one cell a bar (96000), 3 repeats.
Params stutterBase() {
  return {{"sliceSync", 1.0}, {"sliceDivision", fx::kDivEighth}, {"every", fx::kDivWhole},
          {"repeats", 3.0},   {"probability", 100.0}};
}

constexpr size_t kFade = 144;  // 3 ms
}  // namespace

TEST(StutterTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"sliceSync", 1.0}, {"sliceDivision", fx::kDivSixteenth},
                    {"every", fx::kDivQuarter}, {"repeats", 2.0}, {"probability", 100.0}};
  options.stateParams = {{"sliceSync", 0.0}, {"sliceTimeMs", 180.0}, {"every", fx::kDivHalf},
                         {"repeats", 5.0},   {"probability", 70.0},  {"reverse", 1.0},
                         {"pitchDown", 3.0}};
  options.cpuAverageBudgetNs = 12000.0;
  options.cpuWorstBudgetNs = 600000.0;
  expectKindConforms("filter.glitch", options);
}

TEST(StutterTest, RepeatsStartOnTheCellAndAreTheAudioBeforeIt) {
  const std::vector<float> in = noise(8.5, 71u, 0.25f, kSr);
  DirectKind kind(kStutter, stutterBase(), Layout::Mono);
  const std::vector<float> out = kind.render(in);
  const size_t slice = 12000, cell = 96000;

  // Before the first cell boundary the block is the input, to the sample.
  EXPECT_EQ(maxAbsDifference(out, 0, in, 0, cell), 0.0);

  for (const size_t c : {size_t{1}, size_t{2}, size_t{3}}) {
    const size_t start = c * cell;
    for (size_t k = 0; k < 3; ++k) {
      const size_t from = start + k * slice + kFade + 8;
      const size_t length = slice - 2 * kFade - 16;
      const double difference = maxAbsDifference(out, from, in, start - slice + kFade + 8, length);
      const double matching = correlation(out, from, in, start - slice + kFade + 8, length);
      // Shifted by 50 samples it is no longer the same.
      const double shifted = correlation(out, from, in, start - slice + kFade + 8 + 50, length);
      printFx("stutter cell " + std::to_string(c) + " (" +
              juce::String(static_cast<double>(start) / kSr, 3).toStdString() + " s), repeat " +
              std::to_string(k) + " at " +
              juce::String(static_cast<double>(start + k * slice) / kSr, 3).toStdString() +
              " s: max difference from the slice " + juce::String(difference, 9).toStdString() +
              ", correlation " + juce::String(matching, 5).toStdString() + " (shifted 50: " +
              juce::String(shifted, 3).toStdString() + ")");
      EXPECT_LT(difference, 1.0e-6);
      EXPECT_GT(matching, 0.99999);
      EXPECT_LT(std::abs(shifted), 0.1);
    }
    // After the last repeat the input is back, to the sample, until the next cell.
    const size_t after = start + 3 * slice + kFade + 8;
    const size_t until = std::min(in.size(), start + cell) - 1;
    ASSERT_LT(after, until);
    EXPECT_EQ(maxAbsDifference(out, after, in, after, until - after), 0.0) << c;
  }
}

TEST(StutterTest, RepeatsAreAtTheTempo) {
  // 90 BPM: a 1/8 slice is 16000 samples and a bar 128000.
  const std::vector<float> in = noise(5.0, 72u, 0.25f, kSr);
  DirectKind kind(kStutter, stutterBase(), Layout::Mono, 256, 90.0);
  const std::vector<float> out = kind.render(in);
  const size_t slice = 16000, cell = 128000;
  EXPECT_EQ(maxAbsDifference(out, 0, in, 0, cell), 0.0);
  for (size_t k = 0; k < 3; ++k) {
    const size_t from = cell + k * slice + kFade + 8;
    const double difference =
        maxAbsDifference(out, from, in, cell - slice + kFade + 8, slice - 2 * kFade - 16);
    printFx("stutter at 90 BPM, repeat " + std::to_string(k) + " at " +
            juce::String(static_cast<double>(cell + k * slice) / kSr, 4).toStdString() +
            " s: max difference " + juce::String(difference, 9).toStdString());
    EXPECT_LT(difference, 1.0e-6);
  }
}

TEST(StutterTest, ReverseAndPitchDownPerRepeat) {
  const std::vector<float> in = noise(3.0, 73u, 0.25f, kSr);
  const size_t slice = 12000, cell = 96000;
  {
    Params params = stutterBase();
    params.emplace_back("reverse", 1.0);
    DirectKind kind(kStutter, params, Layout::Mono);
    const std::vector<float> out = kind.render(in);
    for (size_t k = 0; k < 3; ++k) {
      double worst = 0.0;
      for (size_t j = kFade + 8; j < slice - kFade - 8; ++j)
        worst = std::max(worst, std::abs(static_cast<double>(out[cell + k * slice + j]) -
                                         in[cell - 1 - j]));
      printFx("stutter reverse, repeat " + std::to_string(k) + ": max difference from the reversed slice " +
              juce::String(worst, 9).toStdString());
      EXPECT_LT(worst, 1.0e-6);
    }
  }
  {
    Params params = stutterBase();
    params.emplace_back("pitchDown", 12.0);
    DirectKind kind(kStutter, params, Layout::Mono);
    const std::vector<float> out = kind.render(in);
    // Repeat 1 plays at half speed, repeat 2 at a quarter: out[j] is the
    // slice at j * rate, linearly interpolated.
    for (size_t k = 0; k < 3; ++k) {
      const double rate = std::pow(2.0, -static_cast<double>(k));
      double worst = 0.0;
      for (size_t j = kFade + 8; j < slice - kFade - 8; ++j) {
        const double position = static_cast<double>(j) * rate;
        const size_t whole = static_cast<size_t>(position);
        const double fraction = position - static_cast<double>(whole);
        const size_t base = cell - slice;
        const double expected =
            in[base + whole] + fraction * (in[base + whole + 1] - in[base + whole]);
        worst = std::max(worst, std::abs(static_cast<double>(out[cell + k * slice + j]) - expected));
      }
      printFx("stutter pitch down 12 st, repeat " + std::to_string(k) + " (rate " +
              juce::String(rate, 2).toStdString() + "): max difference " +
              juce::String(worst, 9).toStdString());
      EXPECT_LT(worst, 1.0e-5);
    }
  }
}

TEST(StutterTest, FreeSliceTimeAndRepeatCount) {
  const std::vector<float> in = noise(4.0, 74u, 0.25f, kSr);
  Params params = {{"sliceSync", 0.0}, {"sliceTimeMs", 100.0}, {"every", fx::kDivWhole},
                   {"repeats", 5.0},   {"probability", 100.0}};
  DirectKind kind(kStutter, params, Layout::Mono);
  const std::vector<float> out = kind.render(in);
  const size_t slice = 4800, cell = 96000;
  for (size_t k = 0; k < 5; ++k)
    EXPECT_LT(maxAbsDifference(out, cell + k * slice + kFade + 8, in, cell - slice + kFade + 8,
                               slice - 2 * kFade - 16),
              1.0e-6)
        << k;
  // Five repeats, no sixth: the input is back right after.
  EXPECT_EQ(maxAbsDifference(out, cell + 5 * slice + kFade + 8, in,
                             cell + 5 * slice + kFade + 8, 20000),
            0.0);
}

TEST(StutterTest, ProbabilityDecidesHowManyCellsStutter) {
  const std::vector<float> in = noise(60.0, 75u, 0.25f, kSr);
  // A cell every half beat (12000 samples), a 1/32 slice (3000), one repeat.
  const auto fraction = [&](double probability) {
    DirectKind kind(kStutter, {{"sliceSync", 1.0}, {"sliceDivision", fx::kDivThirtySecond},
                               {"every", fx::kDivEighth}, {"repeats", 1.0},
                               {"probability", probability}},
                    Layout::Mono);
    const std::vector<float> out = kind.render(in);
    int hits = 0, cells = 0;
    for (size_t c = 1; (c + 1) * 12000 <= in.size(); ++c) {
      ++cells;
      const size_t at = c * 12000 + 600;
      if (maxAbsDifference(out, at, in, at, 1000) > 1.0e-6)
        ++hits;
    }
    return std::pair<int, int>{hits, cells};
  };
  for (const double p : {0.0, 25.0, 50.0, 100.0}) {
    const auto [hits, cells] = fraction(p);
    printFx("stutter probability " + juce::String(p, 0).toStdString() + "%: " +
            std::to_string(hits) + " of " + std::to_string(cells) + " cells stutter");
    if (p == 0.0)
      EXPECT_EQ(hits, 0);
    else if (p == 100.0)
      EXPECT_EQ(hits, cells);
    else
      EXPECT_NEAR(static_cast<double>(hits) / cells, p / 100.0, 0.12);
  }
}

TEST(StutterTest, FollowsTheHostSongPosition) {
  // The song starts one beat into the bar: the next bar line is 3 beats
  // (1.5 s = 72000 samples) in.
  const std::vector<float> in = noise(4.0, 76u, 0.25f, kSr);
  DirectKind kind(kStutter, stutterBase(), Layout::Mono);
  kind.ctx.transport.playing = true;
  kind.ctx.transport.fromHost = true;
  std::vector<float> out(in.size());
  const size_t block = 256;
  for (size_t offset = 0; offset < in.size(); offset += block) {
    const size_t n = std::min(block, in.size() - offset);
    kind.ctx.transport.ppqPosition = 1.0 + static_cast<double>(offset) / kSr * 2.0;
    const std::vector<float> chunk(in.begin() + static_cast<std::ptrdiff_t>(offset),
                                   in.begin() + static_cast<std::ptrdiff_t>(offset + n));
    const std::vector<float> rendered = kind.render(chunk);
    std::copy(rendered.begin(), rendered.end(), out.begin() + static_cast<std::ptrdiff_t>(offset));
  }
  const size_t bar = 72000, slice = 12000;
  EXPECT_EQ(maxAbsDifference(out, 0, in, 0, bar), 0.0);
  for (size_t k = 0; k < 3; ++k) {
    const double difference = maxAbsDifference(out, bar + k * slice + kFade + 8, in,
                                               bar - slice + kFade + 8, slice - 2 * kFade - 16);
    printFx("stutter on the host's bar line (song position 1 beat in, bar at 1.5 s), repeat " +
            std::to_string(k) + ": max difference " + juce::String(difference, 9).toStdString());
    EXPECT_LT(difference, 1.0e-6);
  }
}

TEST(StutterTest, NoStepsEvenOnASine) {
  DirectKind kind(kStutter,
                  {{"sliceSync", 1.0}, {"sliceDivision", fx::kDivSixteenth}, {"every", fx::kDivQuarter},
                   {"repeats", 4.0}, {"probability", 100.0}, {"pitchDown", 2.0}},
                  Layout::Mono);
  const std::vector<float> in = sine(6.0, 331.0, 0.4f, kSr);
  const std::vector<float> out = kind.render(in);
  const float step = maxStep(out, 0, out.size()), inputStep = maxStep(in, 0, in.size());
  printFx("stutter on a sine: largest step " + juce::String(step, 5).toStdString() +
          ", input's own " + juce::String(inputStep, 5).toStdString());
  EXPECT_LT(step, 2.0f * inputStep);
}

TEST(StutterTest, ResetClearsStateAndProcessingDoesNotAllocate) {
  expectResetClearsState(kStutter,
                         {{"sliceSync", 1.0}, {"sliceDivision", fx::kDivSixteenth},
                          {"every", fx::kDivEighth}, {"repeats", 3.0}},
                         nullptr);
  for (const Layout layout : {Layout::Mono, Layout::Stereo})
    EXPECT_EQ(allocationsWhileProcessing(kStutter,
                                         {{"sliceSync", 1.0}, {"sliceDivision", fx::kDivSixteenth},
                                          {"every", fx::kDivEighth}, {"repeats", 3.0},
                                          {"pitchDown", 2.0}},
                                         nullptr, layout),
              0);
}

TEST(StutterTest, CostIsReported) {
  const Params busy = {{"sliceSync", 1.0}, {"sliceDivision", fx::kDivSixteenth},
                       {"every", fx::kDivSixteenth}, {"repeats", 16.0}, {"pitchDown", 1.0}};
  const KindCost cost = measureKindCost(kStutter, busy);
  printFx("cpu filter.glitch (an event on every cell): " +
          juce::String(cost.averageNs, 0).toStdString() + " ns average, " +
          juce::String(cost.worstNs, 0).toStdString() + " ns worst per 128-sample block");
  SUCCEED();
}

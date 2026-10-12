// EffectMatch (plugin/src/EffectMatch.cpp): the effect-matching measurement
// kit. The test signal is played through known settings (an offset and a
// gain, and our own delay, compressor, tremolo, limiter and reverb) and the
// analysis has to recover what was set.
//
// Each analysis of the whole signal takes a few seconds, so the shared ones
// are made once.

#include "EffectMatch.h"
#include "effects/effect_test_helpers.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace fxtest;
using EffectMatch::MatchAnalysis;

namespace {

constexpr double kRate = 48000.0;

const EffectMatch::TestSignal& signalUnderTest() {
  static const EffectMatch::TestSignal signal = EffectMatch::makeTestSignal(kRate);
  return signal;
}

juce::AudioBuffer<float> toBuffer(const std::vector<float>& x) {
  juce::AudioBuffer<float> buffer(1, static_cast<int>(x.size()));
  std::copy(x.begin(), x.end(), buffer.getWritePointer(0));
  return buffer;
}

MatchAnalysis analyse(const std::vector<float>& recording) {
  return EffectMatch::analyseRecording(toBuffer(recording), kRate, signalUnderTest());
}

// The signal with `extraSeconds` of silence after it, the room a recording
// leaves for tails and delays.
std::vector<float> played(double extraSeconds = 3.0) {
  std::vector<float> x = signalUnderTest().samples;
  x.insert(x.end(), static_cast<size_t>(extraSeconds * kRate), 0.0f);
  return x;
}

std::vector<float> shifted(const std::vector<float>& x, int offset, double gainDb) {
  std::vector<float> out(x.size() + static_cast<size_t>(offset), 0.0f);
  const float gain = static_cast<float>(std::pow(10.0, gainDb / 20.0));
  for (size_t i = 0; i < x.size(); ++i)
    out[i + static_cast<size_t>(offset)] = x[i] * gain;
  return out;
}

// What a mix control does around a wet-only effect: the dry signal plus the
// effect's output.
std::vector<float> dryPlusWet(const std::string& kind, const Params& params) {
  const std::vector<float> dry = played();
  DirectKind effect(kind, params);
  const std::vector<float> wet = effect.render(dry);
  std::vector<float> out(dry.size());
  for (size_t i = 0; i < dry.size(); ++i)
    out[i] = dry[i] + wet[i];
  return out;
}

// The recording stretched by `ppm` parts per million (what a recording clock
// that runs slow does), by linear interpolation.
std::vector<float> stretched(const std::vector<float>& x, double ppm) {
  const double factor = 1.0 + ppm * 1.0e-6;
  std::vector<float> out(static_cast<size_t>(static_cast<double>(x.size()) * factor));
  for (size_t i = 0; i < out.size(); ++i) {
    const double position = static_cast<double>(i) / factor;
    const size_t base = static_cast<size_t>(position);
    const double fraction = position - static_cast<double>(base);
    const float a = base < x.size() ? x[base] : 0.0f;
    const float b = base + 1 < x.size() ? x[base + 1] : 0.0f;
    out[i] = static_cast<float>(a + fraction * (b - a));
  }
  return out;
}

// The recording of "nothing between the interface's output and its input",
// with an offset and a gain: the analysis of the signal itself.
const MatchAnalysis& identity() {
  static const MatchAnalysis analysis = analyse(shifted(played(), 0, 0.0));
  return analysis;
}

const EffectMatch::MetricComparison* findMetric(const EffectMatch::MatchComparison& comparison,
                                                const std::string& group, const std::string& name) {
  for (const auto& metric : comparison.metrics)
    if (metric.group == group && metric.name == name)
      return &metric;
  return nullptr;
}

// Least-squares slope of out against in over the staircase points whose input
// level lies in [fromDb, toDb].
double curveSlope(const EffectMatch::ChannelAnalysis& channel, double fromDb, double toDb) {
  double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (const auto& point : channel.staticCurve) {
    if (point.inDb < fromDb || point.inDb > toDb)
      continue;
    ++n;
    sx += point.inDb;
    sy += point.outDb;
    sxx += point.inDb * point.inDb;
    sxy += point.inDb * point.outDb;
  }
  return (n * sxy - sx * sy) / (n * sxx - sx * sx);
}

}  // namespace

TEST(EffectMatchTest, SignalLayoutAndManifest) {
  const auto& signal = signalUnderTest();
  EXPECT_EQ(signal.version, EffectMatch::kSignalVersion);
  const double seconds = static_cast<double>(signal.samples.size()) / kRate;
  EXPECT_GT(seconds, 50.0);
  EXPECT_LT(seconds, 90.0);

  float peak = 0.0f;
  for (const float v : signal.samples)
    peak = std::max(peak, std::abs(v));
  EXPECT_LE(peak, 1.0f);

  for (const char* name : {"syncStart", "sweepLow", "sweepHigh", "sine100", "sine1k", "staircase",
                           "steps", "burst", "silence", "syncEnd"})
    ASSERT_NE(signal.find(name), nullptr) << name;
  // Segments run one after another, and the sweeps and the burst leave 3 s or more.
  std::int64_t end = 0;
  for (const auto& segment : signal.segments) {
    EXPECT_GE(segment.startSample, end) << segment.name;
    end = segment.startSample + segment.lengthSamples +
          static_cast<std::int64_t>(segment.params.at("tailSamples"));
  }
  EXPECT_EQ(end, static_cast<std::int64_t>(signal.samples.size()));
  for (const char* name : {"sweepLow", "sweepHigh", "burst"})
    EXPECT_GE(signal.find(name)->params.at("tailSamples"), 3.0 * kRate) << name;
  EXPECT_EQ(signal.find("sweepLow")->lengthSamples, static_cast<std::int64_t>(8.0 * kRate));
  EXPECT_NEAR(signal.find("staircase")->params.at("steps"), 21.0, 0.0);

  const juce::var manifest = EffectMatch::toVar(signal);
  EXPECT_EQ(static_cast<int>(manifest["signalVersion"]), EffectMatch::kSignalVersion);
  EXPECT_EQ(manifest["segments"].getArray()->size(), static_cast<int>(signal.segments.size()));

  // The same call twice gives the same samples.
  EXPECT_EQ(EffectMatch::makeTestSignal(kRate).samples, signal.samples);
}

TEST(EffectMatchTest, IdentityWithOffsetAndGain) {
  const MatchAnalysis analysis = analyse(shifted(played(), 1776, -3.0));  // 37 ms
  EXPECT_NEAR(analysis.alignment.latencyMs, 37.0, 0.05);
  EXPECT_NEAR(analysis.alignment.driftPpm, 0.0, 5.0);
  EXPECT_GT(analysis.alignment.startScore, 0.9);
  EXPECT_GT(analysis.alignment.endScore, 0.9);

  ASSERT_TRUE(analysis.left.sweepLow.has_value());
  ASSERT_TRUE(analysis.left.sweepHigh.has_value());
  for (const auto& sweep : {*analysis.left.sweepLow, *analysis.left.sweepHigh}) {
    for (int band = 0; band < EffectMatch::kNumBands; ++band)
      EXPECT_NEAR(sweep.responseDb[static_cast<size_t>(band)], 0.0, 0.5)
          << EffectMatch::kBandCentresHz[static_cast<size_t>(band)] << " Hz";
    EXPECT_NEAR(sweep.levelGainDb, -3.0, 0.2);
    EXPECT_TRUE(sweep.echoes.empty());
    EXPECT_FALSE(sweep.delayMs.has_value());
  }

  // The static curve, sines and the rest of a linear path.
  ASSERT_EQ(analysis.left.staticCurve.size(), 21u);
  for (const auto& point : analysis.left.staticCurve)
    EXPECT_NEAR(point.outDb, point.inDb - 3.0, 0.3) << point.inDb;
  ASSERT_TRUE(analysis.left.sine1k.has_value());
  EXPECT_NEAR(analysis.left.sine1k->levelDb, -21.0, 0.2);
  ASSERT_TRUE(analysis.left.sine1k->thdPct.has_value());
  EXPECT_LT(*analysis.left.sine1k->thdPct, 0.05);
  EXPECT_LT(analysis.left.sine1k->ampDepthDb, 0.05);
  EXPECT_FALSE(analysis.left.sine1k->ampRateHz.has_value());
  EXPECT_LT(analysis.left.sine100->pitchDepthCents, 0.5);
  EXPECT_FALSE(analysis.left.steps.attackMs.has_value());
  EXPECT_FALSE(analysis.left.steps.releaseMs.has_value());
  EXPECT_FALSE(analysis.left.burst.tailT60s.has_value());
  EXPECT_LT(analysis.left.burst.tailEndMs, 20.0);
  EXPECT_LE(analysis.left.noiseFloorDb, -100.0);
}

TEST(EffectMatchTest, ClockDriftIsMeasured) {
  const MatchAnalysis analysis = analyse(stretched(shifted(played(), 480, 0.0), 100.0));
  EXPECT_NEAR(analysis.alignment.driftPpm, 100.0, 20.0);
  EXPECT_NEAR(analysis.alignment.latencyMs, 10.0, 0.1);
  // The segments are still found where they are: the static curve is intact.
  ASSERT_EQ(analysis.left.staticCurve.size(), 21u);
  for (const auto& point : analysis.left.staticCurve)
    EXPECT_NEAR(point.outDb, point.inDb, 0.5) << point.inDb;
}

TEST(EffectMatchTest, MissingMarkersFailClearly) {
  const std::vector<float> silent(static_cast<size_t>(10 * kRate), 0.0f);
  EXPECT_THROW(analyse(silent), EffectMatch::MatchError);
  // A recording cut short before the closing marker.
  std::vector<float> cut = played();
  cut.resize(cut.size() / 2);
  try {
    analyse(cut);
    FAIL() << "expected a MatchError";
  } catch (const EffectMatch::MatchError& e) {
    EXPECT_NE(std::string(e.what()).find("end sync marker"), std::string::npos) << e.what();
  }
  // A recording at the wrong rate is refused rather than misread.
  EXPECT_THROW(EffectMatch::analyseRecording(toBuffer(played()), 44100.0, signalUnderTest()),
               EffectMatch::MatchError);
}

TEST(EffectMatchTest, SimpleDelayTimeAndFeedback) {
  const MatchAnalysis analysis = analyse(
      dryPlusWet("delay.simple", {{"timeMs", 250.0}, {"feedback", 50.0}, {"toneHz", 20000.0}}));
  ASSERT_TRUE(analysis.left.sweepLow.has_value());
  const auto& sweep = *analysis.left.sweepLow;
  ASSERT_GE(sweep.echoes.size(), 4u);
  ASSERT_TRUE(sweep.delayMs.has_value());
  ASSERT_TRUE(sweep.feedbackDb.has_value());
  EXPECT_NEAR(*sweep.delayMs, 250.0, 1.0);
  // 50% feedback: each echo is half the one before, -6.02 dB.
  EXPECT_NEAR(*sweep.feedbackDb, 20.0 * std::log10(0.5), 1.5);
  // A clean echo train has no decay curve worth reading.
  EXPECT_FALSE(sweep.decay.t30S.has_value());
  EXPECT_NEAR(analysis.left.sweepHigh->delayMs.value_or(0.0), 250.0, 1.0);
  // The tail after the burst runs for the echoes: well over a second.
  EXPECT_GT(analysis.left.burst.tailEndMs, 1000.0);
}

TEST(EffectMatchTest, CompressorStaticCurveAndBallistics) {
  DirectKind compressor("dynamics.compressor",
                        {{"thresholdDb", -30.0},
                         {"ratio", 4.0},
                         {"kneeDb", 0.0},
                         {"attackMs", 20.0},
                         {"releaseMs", 200.0}});
  const MatchAnalysis analysis = analyse(compressor.render(played()));
  const auto& channel = analysis.left;
  ASSERT_EQ(channel.staticCurve.size(), 21u);
  // Below the threshold the level passes; above it, one dB out for every 4 in.
  EXPECT_NEAR(curveSlope(channel, -60.0, -36.0), 1.0, 0.05);
  EXPECT_NEAR(curveSlope(channel, -27.0, 0.0), 0.25, 0.05);

  // The steps go from -40 (under the threshold) to -10 (over it) and back.
  ASSERT_TRUE(channel.steps.attackMs.has_value());
  ASSERT_TRUE(channel.steps.releaseMs.has_value());
  EXPECT_GT(*channel.steps.attackMs, 10.0);
  EXPECT_LT(*channel.steps.attackMs, 40.0);
  EXPECT_GT(*channel.steps.releaseMs, 100.0);
  EXPECT_LT(*channel.steps.releaseMs, 400.0);
}

TEST(EffectMatchTest, TremoloRateAndDepth) {
  DirectKind tremolo("modulation.tremolo", {{"rateHz", 5.0}, {"depth", 80.0}});
  const MatchAnalysis analysis = analyse(tremolo.render(played()));
  for (const auto& sine : {analysis.left.sine100, analysis.left.sine1k}) {
    ASSERT_TRUE(sine.has_value());
    ASSERT_TRUE(sine->ampRateHz.has_value());
    EXPECT_NEAR(*sine->ampRateHz, 5.0, 0.25);
    // 80% depth: the gain swings between 0.2 and 1, 14 dB.
    EXPECT_NEAR(sine->ampDepthDb, 14.0, 2.0);
    EXPECT_LT(sine->pitchDepthCents, 2.0);
  }
}

TEST(EffectMatchTest, VibratoPitchRateAndDepth) {
  DirectKind vibrato("modulation.vibrato", {{"rateHz", 5.0}, {"depthCents", 30.0}, {"riseMs", 0.0}});
  const MatchAnalysis analysis = analyse(vibrato.render(played()));
  for (const auto& sine : {analysis.left.sine100, analysis.left.sine1k}) {
    ASSERT_TRUE(sine.has_value());
    ASSERT_TRUE(sine->pitchRateHz.has_value());
    EXPECT_NEAR(*sine->pitchRateHz, 5.0, 0.25);
    EXPECT_GT(sine->pitchDepthCents, 20.0);
    EXPECT_LT(sine->pitchDepthCents, 80.0);
    EXPECT_LT(sine->ampDepthDb, 0.5);
  }
}

TEST(EffectMatchTest, LimiterLatency) {
  DirectKind limiter("dynamics.limiter");
  const MatchAnalysis analysis = analyse(limiter.render(played()));
  EXPECT_NEAR(analysis.alignment.latencyMs, 2.0, 0.05);
  // The bands are not shaped by a delay of two milliseconds: the response is
  // measured from the direct sound.
  ASSERT_TRUE(analysis.left.sweepLow.has_value());
  for (int band = 0; band < EffectMatch::kNumBands; ++band)
    EXPECT_NEAR(analysis.left.sweepLow->responseDb[static_cast<size_t>(band)], 0.0, 0.7);
}

TEST(EffectMatchTest, ReverbDecayGrowsWithTheDecayParameter) {
  const auto t30 = [](double decay) {
    const MatchAnalysis analysis = analyse(dryPlusWet("reverb.room", {{"decayS", decay}}));
    EXPECT_TRUE(analysis.left.sweepLow.has_value());
    return analysis.left.sweepLow ? analysis.left.sweepLow->decay.t30S : std::optional<double>();
  };
  const auto shortDecay = t30(0.5);
  const auto longDecay = t30(1.5);
  ASSERT_TRUE(shortDecay.has_value());
  ASSERT_TRUE(longDecay.has_value());
  EXPECT_GT(*shortDecay, 0.0);
  EXPECT_GT(*longDecay, 1.5 * *shortDecay);
}

TEST(EffectMatchTest, StereoRecordingsReportWidth) {
  const std::vector<float> mono = shifted(played(), 100, 0.0);
  juce::AudioBuffer<float> stereo(2, static_cast<int>(mono.size()));
  std::copy(mono.begin(), mono.end(), stereo.getWritePointer(0));
  std::copy(mono.begin(), mono.end(), stereo.getWritePointer(1));
  MatchAnalysis analysis = EffectMatch::analyseRecording(stereo, kRate, signalUnderTest());
  EXPECT_EQ(analysis.channels, 2);
  ASSERT_TRUE(analysis.right.has_value());
  ASSERT_TRUE(analysis.stereo.has_value());
  EXPECT_NEAR(analysis.stereo->correlation, 1.0, 0.001);
  EXPECT_NEAR(analysis.stereo->width, 0.0, 0.001);

  // One side inverted: fully out of phase.
  for (int i = 0; i < stereo.getNumSamples(); ++i)
    stereo.setSample(1, i, -stereo.getSample(1, i));
  analysis = EffectMatch::analyseRecording(stereo, kRate, signalUnderTest());
  EXPECT_NEAR(analysis.stereo->correlation, -1.0, 0.001);
}

TEST(EffectMatchTest, ComparisonOfAnAnalysisWithItselfPasses) {
  const MatchAnalysis& analysis = identity();
  const EffectMatch::MatchComparison comparison = EffectMatch::compareAnalyses(analysis, analysis);
  EXPECT_TRUE(comparison.allPass);
  EXPECT_EQ(comparison.passed, comparison.judged);
  EXPECT_GT(comparison.judged, 40);
  EXPECT_DOUBLE_EQ(comparison.score, 100.0);

  const juce::String markdown = EffectMatch::toMarkdown(comparison);
  EXPECT_TRUE(markdown.contains("Score 100.0"));
  EXPECT_TRUE(markdown.contains("| Metric |"));
  const juce::var json = EffectMatch::toVar(comparison);
  EXPECT_TRUE(static_cast<bool>(json["allPass"]));
  EXPECT_EQ(json["metrics"].getArray()->size(), static_cast<int>(comparison.metrics.size()));
}

TEST(EffectMatchTest, ComparisonFlagsAChangedDelayTime) {
  const Params base = {{"feedback", 50.0}, {"toneHz", 20000.0}};
  Params at250 = base, at300 = base;
  at250.push_back({"timeMs", 250.0});
  at300.push_back({"timeMs", 300.0});
  const MatchAnalysis reference = analyse(dryPlusWet("delay.simple", at250));
  const MatchAnalysis same = analyse(dryPlusWet("delay.simple", at250));
  const MatchAnalysis changed = analyse(dryPlusWet("delay.simple", at300));

  const EffectMatch::MatchComparison match = EffectMatch::compareAnalyses(reference, same);
  EXPECT_TRUE(match.allPass);

  const EffectMatch::MatchComparison mismatch = EffectMatch::compareAnalyses(reference, changed);
  EXPECT_FALSE(mismatch.allPass);
  const auto* delay = findMetric(mismatch, "sweepLow", "delay time");
  ASSERT_NE(delay, nullptr);
  EXPECT_FALSE(delay->pass);
  EXPECT_NEAR(delay->difference, 50.0, 1.5);
  EXPECT_LT(mismatch.score, 100.0);
  EXPECT_TRUE(EffectMatch::toMarkdown(mismatch).contains("FAIL"));

  // A looser tolerance lets the same pair through.
  EffectMatch::Tolerances loose;
  loose.delayMs = 60.0;
  const auto relaxed = EffectMatch::compareAnalyses(reference, changed, loose);
  EXPECT_TRUE(findMetric(relaxed, "sweepLow", "delay time")->pass);
}

TEST(EffectMatchTest, AnalysisSurvivesItsJson) {
  DirectKind compressor("dynamics.compressor", {{"thresholdDb", -30.0}, {"ratio", 4.0}});
  const MatchAnalysis analysis = analyse(compressor.render(played()));
  const juce::String text = juce::JSON::toString(EffectMatch::toVar(analysis), false);
  juce::var parsed;
  ASSERT_TRUE(juce::JSON::parse(text, parsed).wasOk());
  const MatchAnalysis restored = EffectMatch::analysisFromVar(parsed);
  // Rounded numbers: the same analysis within the printed precision.
  EXPECT_TRUE(EffectMatch::compareAnalyses(analysis, restored).allPass);
  EXPECT_EQ(restored.left.staticCurve.size(), analysis.left.staticCurve.size());
  EXPECT_EQ(restored.left.steps.attackMs.has_value(), analysis.left.steps.attackMs.has_value());

  EXPECT_THROW(EffectMatch::analysisFromVar(juce::var(42)), EffectMatch::MatchError);
}

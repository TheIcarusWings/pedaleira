// The shared effect DSP helpers (plugin/include/effects/dsp, namespace fx):
// each one against the property an effect relies on it for.
#include "Processor.h"
#include "effects/EffectDsp.h"
#include "test_helpers.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace {

// Steady-state gain in dB of a per-sample processor at `freq`, from a sine.
template <typename Process>
double gainDbAt(double freq, Process&& process, double seconds = 0.5) {
  const int frames = static_cast<int>(seconds * kFs);
  const std::vector<float> in = makeSine(frames, freq, 0.5f);
  std::vector<float> out(in.size());
  for (size_t i = 0; i < in.size(); ++i)
    out[i] = process(in[i]);
  const size_t skip = in.size() / 2;
  return db(goertzelPower(out.data() + skip, out.size() - skip, freq)) -
         db(goertzelPower(in.data() + skip, in.size() - skip, freq));
}

}  // namespace

TEST(EffectDspTest, SmoothedParamSnapsFirstThenRamps) {
  fx::SmoothedParam p;
  p.prepare(48000.0, 0.01);  // 480 samples
  p.setTarget(0.25f);
  EXPECT_FLOAT_EQ(p.current(), 0.25f) << "the first target after prepare is taken at once";
  EXPECT_FALSE(p.isSmoothing());

  p.setTarget(1.0f);
  EXPECT_TRUE(p.isSmoothing());
  float previous = p.current();
  for (int i = 0; i < 480; ++i) {
    const float v = p.next();
    EXPECT_GE(v, previous);
    EXPECT_LT(v - previous, 0.01f) << "a ramp, not a jump";
    previous = v;
  }
  EXPECT_FLOAT_EQ(p.current(), 1.0f);
  EXPECT_FALSE(p.isSmoothing());

  p.snapOnNextTarget();
  p.setTarget(0.0f);
  EXPECT_FLOAT_EQ(p.current(), 0.0f);
}

TEST(EffectDspTest, DelayLineIntegerReadsAndFeedbackLength) {
  fx::DelayLine line;
  line.prepare(2, 1000);
  EXPECT_EQ(line.numChannels(), 2);
  EXPECT_EQ(line.maxDelaySamples(), 1000);

  // Read before write: a delay of d returns x[n - d].
  for (int n = 0; n < 3000; ++n) {
    const float echo = line.read(0, 700);
    EXPECT_FLOAT_EQ(echo, n >= 700 ? static_cast<float>(n - 700) : 0.0f);
    line.write(0, static_cast<float>(n));
  }
  // Delay 1 is the latest sample written; channels are independent.
  EXPECT_FLOAT_EQ(line.read(0, 1), 2999.0f);
  EXPECT_FLOAT_EQ(line.read(1, 1), 0.0f);
  // Out-of-range delays are clamped, not read out of bounds.
  EXPECT_FLOAT_EQ(line.read(0, 0), 2999.0f);
  EXPECT_FLOAT_EQ(line.read(0, 5000), line.read(0, 1000));

  line.clear();
  EXPECT_FLOAT_EQ(line.read(0, 1), 0.0f);
}

TEST(EffectDspTest, DelayLineFractionalReads) {
  fx::DelayLine line;
  line.prepare(1, 256);
  // A ramp: both interpolators are exact on a straight line.
  for (int n = 0; n < 400; ++n)
    line.write(0, static_cast<float>(n));
  // Latest written is 399 at delay 1, so delay d reads 400 - d.
  EXPECT_NEAR(line.readLinear(0, 10.5f), 389.5f, 1e-3f);
  EXPECT_NEAR(line.readHermite(0, 10.5f), 389.5f, 1e-3f);
  EXPECT_FLOAT_EQ(line.readLinear(0, 10.0f), 390.0f);
  EXPECT_FLOAT_EQ(line.readHermite(0, 10.0f), 390.0f) << "Hermite is exact at whole samples";

  // On a 2 kHz sine at a fractional delay, Hermite is far closer than linear.
  line.clear();
  const double w = 2.0 * kPi * 2000.0 / kFs;
  double worstLinear = 0.0, worstHermite = 0.0;
  for (int n = 0; n < 2000; ++n) {
    line.write(0, static_cast<float>(std::sin(w * n)));
    if (n < 100)
      continue;
    // After writing x[n], delay d reads x[n + 1 - d].
    const double expected = std::sin(w * (n + 1 - 37.37));
    worstLinear = std::max(worstLinear, std::abs(line.readLinear(0, 37.37f) - expected));
    worstHermite = std::max(worstHermite, std::abs(line.readHermite(0, 37.37f) - expected));
  }
  EXPECT_LT(worstLinear, 0.01);
  EXPECT_LT(worstHermite, 0.001);
  EXPECT_LT(worstHermite, worstLinear * 0.25);
}

TEST(EffectDspTest, LfoShapesRatePhaseAndTempo) {
  fx::Lfo lfo;
  lfo.prepare(kFs);
  lfo.setRateHz(5.0);

  // Sine: starts at zero rising, a quarter turn ahead is the cosine, the
  // range is +-1 and there are 5 cycles in a second.
  EXPECT_NEAR(lfo.valueAt(0.0f), 0.0f, 1e-6f);
  EXPECT_NEAR(lfo.valueAt(0.25f), 1.0f, 1e-6f);
  int risingCrossings = 0;
  float previous = -1.0f, lo = 0.0f, hi = 0.0f;
  for (int i = 0; i < static_cast<int>(kFs); ++i) {
    const float v = lfo.next();
    lo = std::min(lo, v);
    hi = std::max(hi, v);
    if (previous < 0.0f && v >= 0.0f)
      ++risingCrossings;
    previous = v;
  }
  EXPECT_EQ(risingCrossings, 5);
  EXPECT_NEAR(hi, 1.0f, 1e-3f);
  EXPECT_NEAR(lo, -1.0f, 1e-3f);

  // Triangle: zero, peak, zero, trough at the quarters.
  lfo.setShape(fx::Lfo::Shape::Triangle);
  lfo.reset();
  EXPECT_NEAR(lfo.valueAt(0.0f), 0.0f, 1e-6f);
  EXPECT_NEAR(lfo.valueAt(0.25f), 1.0f, 1e-6f);
  EXPECT_NEAR(lfo.valueAt(0.5f), 0.0f, 1e-6f);
  EXPECT_NEAR(lfo.valueAt(0.75f), -1.0f, 1e-6f);
  EXPECT_NEAR(lfo.valueAt(0.125f), 0.5f, 1e-6f);

  // Sample and hold: one value per cycle, in range, the same every run.
  lfo.setShape(fx::Lfo::Shape::SampleAndHold);
  lfo.setRateHz(10.0);
  lfo.reset();
  std::vector<float> steps;
  float held = lfo.valueAt();
  int changes = 0;
  for (int i = 0; i < static_cast<int>(kFs); ++i) {
    const float v = lfo.next();
    EXPECT_GE(v, -1.0f);
    EXPECT_LE(v, 1.0f);
    if (v != held) {
      ++changes;
      held = v;
    }
    steps.push_back(v);
  }
  EXPECT_NEAR(changes, 10, 1);
  fx::Lfo again;
  again.prepare(kFs);
  again.setShape(fx::Lfo::Shape::SampleAndHold);
  again.setRateHz(10.0);
  for (const float step : steps)
    ASSERT_EQ(again.next(), step);

  // Tempo: one cycle per beat at 120 bpm is 2 Hz; with a rolling transport
  // the phase follows the song position.
  lfo.setShape(fx::Lfo::Shape::Sine);
  lfo.reset();
  lfo.setRateFromTempo(120.0, 1.0);
  for (int i = 0; i < static_cast<int>(kFs / 8); ++i)  // a quarter of a 0.5 s cycle
    lfo.advance();
  EXPECT_NEAR(lfo.phaseTurns(), 0.25, 1e-6);

  BlockTransport transport;
  transport.ppqPosition = 10.75;
  lfo.syncPhaseToTransport(transport, 1.0);
  EXPECT_NEAR(lfo.phaseTurns(), 0.25, 1e-6) << "no rolling transport: the LFO free-runs";
  transport.playing = true;
  lfo.syncPhaseToTransport(transport, 1.0);
  EXPECT_NEAR(lfo.phaseTurns(), 0.75, 1e-9);
  lfo.syncPhaseToTransport(transport, 4.0);
  EXPECT_NEAR(lfo.phaseTurns(), 10.75 / 4.0 - 2.0, 1e-9);
}

TEST(EffectDspTest, OnePoleCutoffAndComplement) {
  fx::OnePole lp;
  lp.setCutoff(1000.0, kFs);
  EXPECT_NEAR(gainDbAt(1000.0, [&](float x) { return lp.lowpass(x); }), -3.0, 0.4);
  lp.reset();
  EXPECT_GT(gainDbAt(50.0, [&](float x) { return lp.lowpass(x); }), -0.1);
  lp.reset();
  EXPECT_LT(gainDbAt(10000.0, [&](float x) { return lp.lowpass(x); }), -15.0);

  fx::OnePole hp;
  hp.setCutoff(1000.0, kFs);
  EXPECT_LT(gainDbAt(50.0, [&](float x) { return hp.highpass(x); }), -20.0);
  hp.reset();
  EXPECT_GT(gainDbAt(10000.0, [&](float x) { return hp.highpass(x); }), -1.0);
}

TEST(EffectDspTest, StateVariableFilterResponseAndFastModulation) {
  fx::StateVariableFilter svf;
  svf.set(1000.0, 0.7071, kFs);
  EXPECT_NEAR(gainDbAt(1000.0, [&](float x) { return svf.lowpass(x); }), -3.0, 0.2);
  svf.reset();
  EXPECT_NEAR(gainDbAt(1000.0, [&](float x) { return svf.highpass(x); }), -3.0, 0.2);
  svf.reset();
  EXPECT_NEAR(gainDbAt(100.0, [&](float x) { return svf.lowpass(x); }), 0.0, 0.1);
  svf.reset();
  EXPECT_LT(gainDbAt(8000.0, [&](float x) { return svf.lowpass(x); }), -30.0);
  svf.reset();
  // The band output peaks at q.
  svf.set(1000.0, 4.0, kFs);
  EXPECT_NEAR(gainDbAt(1000.0, [&](float x) { return svf.bandpass(x); }), 20.0 * std::log10(4.0),
              0.3);

  // A new, unrelated cutoff and a high resonance every sample: the output
  // stays bounded and finite (a direct-form biquad would blow up here).
  svf.reset();
  std::mt19937 rng(3);
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  const std::vector<float> in = makeNoise(96000, 5u, 0.5f);
  float peak = 0.0f;
  for (const float x : in) {
    svf.set(20.0 * std::pow(1000.0, unit(rng)), 0.5 + 15.0 * unit(rng), kFs);
    const fx::StateVariableFilter::Outputs y = svf.process(x);
    ASSERT_TRUE(std::isfinite(y.low) && std::isfinite(y.band) && std::isfinite(y.high));
    peak = std::max({peak, std::abs(y.low), std::abs(y.band), std::abs(y.high)});
  }
  EXPECT_LT(peak, 100.0f);
}

TEST(EffectDspTest, BiquadDesignsMatchTheirMeasuredResponse) {
  struct Case {
    const char* name;
    fx::BiquadCoeffs coeffs;
    double probeHz;
    double expectedDb;
  };
  const Case cases[] = {
      {"lowpass at cutoff", fx::BiquadCoeffs::lowpass(1000.0, 0.7071, kFs), 1000.0, -3.01},
      {"lowpass passband", fx::BiquadCoeffs::lowpass(1000.0, 0.7071, kFs), 100.0, 0.0},
      {"highpass at cutoff", fx::BiquadCoeffs::highpass(1000.0, 0.7071, kFs), 1000.0, -3.01},
      {"bandpass centre", fx::BiquadCoeffs::bandpass(1000.0, 2.0, kFs), 1000.0, 0.0},
      {"peak centre", fx::BiquadCoeffs::peak(1000.0, 1.0, 6.0, kFs), 1000.0, 6.0},
      {"peak far away", fx::BiquadCoeffs::peak(1000.0, 1.0, 6.0, kFs), 50.0, 0.0},
      {"low shelf plateau", fx::BiquadCoeffs::lowShelf(500.0, 0.7071, -9.0, kFs), 30.0, -9.0},
      {"low shelf above", fx::BiquadCoeffs::lowShelf(500.0, 0.7071, -9.0, kFs), 10000.0, 0.0},
      {"high shelf plateau", fx::BiquadCoeffs::highShelf(2000.0, 0.7071, 4.0, kFs), 18000.0, 4.0},
      {"high shelf below", fx::BiquadCoeffs::highShelf(2000.0, 0.7071, 4.0, kFs), 100.0, 0.0},
      {"allpass", fx::BiquadCoeffs::allpass(1000.0, 0.7071, kFs), 3000.0, 0.0},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    fx::Biquad filter;
    filter.setCoeffs(c.coeffs);
    const double measured = gainDbAt(c.probeHz, [&](float x) { return filter.process(x); });
    EXPECT_NEAR(measured, c.expectedDb, 0.15);
    // The analytic response says the same.
    EXPECT_NEAR(20.0 * std::log10(c.coeffs.magnitudeAt(c.probeHz, kFs)), measured, 0.05);
  }
  fx::Biquad notch;
  notch.setCoeffs(fx::BiquadCoeffs::notch(1000.0, 2.0, kFs));
  EXPECT_LT(gainDbAt(1000.0, [&](float x) { return notch.process(x); }), -40.0);
}

TEST(EffectDspTest, EnvelopeFollowerTimesAndModes) {
  fx::EnvelopeFollower peak;
  peak.prepare(kFs);
  peak.setTimes(10.0f, 100.0f);
  // A step to 1: about 63% after the attack time, then close to 1.
  float value = 0.0f;
  for (int i = 0; i < 480; ++i)
    value = peak.process(1.0f);
  EXPECT_NEAR(value, 0.632f, 0.01f);
  for (int i = 0; i < 4800; ++i)
    value = peak.process(1.0f);
  EXPECT_NEAR(value, 1.0f, 1e-3f);
  // And back down: about 37% left after the release time.
  for (int i = 0; i < 4800; ++i)
    value = peak.process(0.0f);
  EXPECT_NEAR(value, 0.368f, 0.01f);
  EXPECT_FLOAT_EQ(peak.current(), value);

  // RMS of a sine is its amplitude over sqrt(2).
  fx::EnvelopeFollower rms;
  rms.prepare(kFs);
  rms.setMode(fx::EnvelopeFollower::Mode::Rms);
  rms.setTimes(50.0f, 50.0f);
  for (const float x : makeSine(48000, 1000.0, 0.5f))
    value = rms.process(x);
  EXPECT_NEAR(value, 0.5f / std::sqrt(2.0f), 0.01f);

  // Zero times follow at once.
  fx::EnvelopeFollower instant;
  instant.prepare(kFs);
  instant.setTimes(0.0f, 0.0f);
  EXPECT_FLOAT_EQ(instant.process(-0.7f), 0.7f);
  EXPECT_FLOAT_EQ(instant.process(0.1f), 0.1f);
}

TEST(EffectDspTest, NoteDivisionsAndTheStandardTimeParameters) {
  EXPECT_EQ(fx::noteDivisionNames().size(), fx::kNumNoteDivisions);
  EXPECT_EQ(fx::noteDivisionNames()[fx::kDivWhole], juce::String("1/1"));
  EXPECT_EQ(fx::noteDivisionNames()[fx::kDivQuarter], juce::String("1/4"));
  EXPECT_EQ(fx::noteDivisionNames()[fx::kDivEighthDotted], juce::String("1/8D"));
  EXPECT_EQ(fx::noteDivisionNames()[fx::kDivThirtySecondTriplet], juce::String("1/32T"));

  EXPECT_DOUBLE_EQ(fx::noteDivisionBeats(fx::kDivWhole), 4.0);
  EXPECT_DOUBLE_EQ(fx::noteDivisionBeats(fx::kDivQuarter), 1.0);
  EXPECT_DOUBLE_EQ(fx::noteDivisionBeats(fx::kDivEighthDotted), 0.75);
  EXPECT_DOUBLE_EQ(fx::noteDivisionBeats(fx::kDivQuarterTriplet), 2.0 / 3.0);
  EXPECT_DOUBLE_EQ(fx::noteDivisionBeats(fx::kDivThirtySecond), 0.125);
  EXPECT_DOUBLE_EQ(fx::noteDivisionBeats(-3), 4.0) << "clamped";
  EXPECT_DOUBLE_EQ(fx::noteDivisionBeats(99), fx::noteDivisionBeats(fx::kDivThirtySecondTriplet));
  EXPECT_DOUBLE_EQ(fx::noteDivisionSeconds(fx::kDivQuarter, 120.0), 0.5);
  EXPECT_DOUBLE_EQ(fx::noteDivisionSeconds(fx::kDivEighthDotted, 100.0), 0.45);

  BlockParamTable table;
  table.push_back(BlockParamSpec::floatParam("depth", "Depth", "%", 0.0f, 100.0f, 50.0f));
  fx::TimeParamOptions options;
  options.affectsTail = true;
  const fx::TimeParamIndices time = fx::appendTimeParams(table, options);
  EXPECT_EQ(time.sync, 1);
  EXPECT_EQ(time.division, 2);
  EXPECT_EQ(time.timeMs, 3);
  EXPECT_EQ(table[1].id, juce::String("sync"));
  EXPECT_EQ(table[2].id, juce::String("division"));
  EXPECT_EQ(table[3].id, juce::String("timeMs"));
  EXPECT_EQ(table[1].type, BlockParamType::Bool);
  EXPECT_EQ(table[2].type, BlockParamType::Choice);
  EXPECT_EQ(table[2].choices.size(), fx::kNumNoteDivisions);
  EXPECT_EQ(table[3].unit, juce::String("ms"));
  EXPECT_TRUE(table[3].tempoSync);
  EXPECT_TRUE(table[1].affectsTail && table[2].affectsTail && table[3].affectsTail);

  fx::TimeParamOptions second;
  second.idPrefix = "left";
  fx::appendTimeParams(table, second);
  EXPECT_EQ(table[4].id, juce::String("leftSync"));
  EXPECT_EQ(table[5].id, juce::String("leftDivision"));
  EXPECT_EQ(table[6].id, juce::String("leftTimeMs"));
  EXPECT_FALSE(table[6].affectsTail);

  const fx::RateParamIndices rate = fx::appendRateParams(table, {});
  EXPECT_EQ(table[static_cast<size_t>(rate.rateHz)].id, juce::String("rateHz"));
  EXPECT_EQ(table[static_cast<size_t>(rate.rateHz)].unit, juce::String("Hz"));

  BlockTransport transport;
  transport.bpm = 150.0;
  EXPECT_DOUBLE_EQ(fx::resolveTimeSeconds(0.0f, fx::kDivQuarter, 250.0f, transport), 0.25);
  EXPECT_DOUBLE_EQ(fx::resolveTimeSeconds(1.0f, fx::kDivQuarter, 250.0f, transport), 0.4);
  EXPECT_DOUBLE_EQ(fx::resolveRateHz(0.0f, fx::kDivQuarter, 3.0f, transport), 3.0);
  EXPECT_DOUBLE_EQ(fx::resolveRateHz(1.0f, fx::kDivHalf, 3.0f, transport), 1.25);
}

TEST(EffectDspTest, SaturationLevelsAndDenormals) {
  float previousTanh = -2.0f, previousSoft = -2.0f;
  for (float x = -6.0f; x <= 6.0f; x += 0.01f) {
    const float t = fx::fastTanh(x);
    const float s = fx::softClip(x);
    EXPECT_NEAR(t, std::tanh(x), 0.03f);
    EXPECT_LE(std::abs(t), 1.0f);
    EXPECT_LE(std::abs(s), 1.0f);
    EXPECT_GE(t, previousTanh) << "monotonic";
    EXPECT_GE(s, previousSoft) << "monotonic";
    previousTanh = t;
    previousSoft = s;
  }
  EXPECT_FLOAT_EQ(fx::fastTanh(0.0f), 0.0f);
  EXPECT_NEAR(fx::fastTanh(0.01f), 0.01f, 1e-5f);
  EXPECT_FLOAT_EQ(fx::fastTanh(10.0f), 1.0f);
  EXPECT_NEAR(fx::softClip(0.01f), 0.01f, 1e-5f);
  EXPECT_FLOAT_EQ(fx::softClip(2.0f), 1.0f);
  EXPECT_FLOAT_EQ(fx::softClip(-2.0f), -1.0f);
  EXPECT_FLOAT_EQ(fx::hardClip(3.0f, 0.5f), 0.5f);

  EXPECT_NEAR(fx::dbToGain(-6.0206f), 0.5f, 1e-4f);
  EXPECT_NEAR(fx::gainToDb(2.0f), 6.0206f, 1e-3f);
  EXPECT_FLOAT_EQ(fx::dbToGain(-100.0f), 0.0f);
  EXPECT_FLOAT_EQ(fx::gainToDb(0.0f), -100.0f);

  EXPECT_FLOAT_EQ(fx::flushDenormal(1.0e-30f), 0.0f);
  EXPECT_FLOAT_EQ(fx::flushDenormal(-1.0e-30f), 0.0f);
  EXPECT_FLOAT_EQ(fx::flushDenormal(1.0e-6f), 1.0e-6f);
}

TEST(EffectDspTest, NoiseIsRepeatableAndFollowsTheOfflineSeed) {
  const auto stream = [](std::uint32_t salt) {
    fx::NoiseSource source(salt);
    std::vector<float> out(4096);
    for (float& v : out)
      v = source.nextBipolar();
    return out;
  };

  const std::vector<float> a = stream(1);
  EXPECT_EQ(a, stream(1)) << "the same salt gives the same stream";
  EXPECT_NE(a, stream(2)) << "another salt gives another";

  double sum = 0.0;
  for (const float v : a) {
    EXPECT_GE(v, -1.0f);
    EXPECT_LT(v, 1.0f);
    sum += v;
  }
  EXPECT_NEAR(sum / static_cast<double>(a.size()), 0.0, 0.05);
  fx::NoiseSource unipolar(9);
  for (int i = 0; i < 1000; ++i) {
    const float v = unipolar.nextUnipolar();
    EXPECT_GE(v, 0.0f);
    EXPECT_LT(v, 1.0f);
  }

  // The hook the Spread wobble uses for offline renders moves effect noise
  // too: pinned, the stream is a different but equally repeatable one.
  TONE3000Processor::setDeterministicRandomForOfflineRender(42);
  const std::vector<float> pinned = stream(1);
  EXPECT_NE(pinned, a);
  EXPECT_EQ(pinned, stream(1));
  TONE3000Processor::setDeterministicRandomForOfflineRender(43);
  EXPECT_NE(stream(1), pinned);
  TONE3000Processor::clearDeterministicRandomForOfflineRender();
  EXPECT_EQ(stream(1), a);
}

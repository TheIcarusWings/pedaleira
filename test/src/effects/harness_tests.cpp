// The measurements in effect_test_helpers.h, each against a signal whose
// answer is known. The effect tests lean on these numbers, so they are
// checked here once rather than trusted.
#include "effects/EffectDsp.h"
#include "effects/effect_test_helpers.h"
#include "test_block_kinds.h"

#include <gtest/gtest.h>

using namespace fxtest;

namespace {
constexpr double kRate = 48000.0;
}

TEST(EffectHarnessTest, ReferenceRigIsTransparent) {
  // What "the rig" adds around a block: nothing but the DC blocker.
  EffectRig reference{juce::String()};
  const std::vector<float> ir = captureImpulseResponse(reference, 0.5);
  EXPECT_EQ(peakIndexOf(ir), 0);
  EXPECT_EQ(onsetIndex(ir), 0);
  EXPECT_NEAR(ir[0], 1.0f, 1e-3f);
  EXPECT_LT(peakOf(ir, 1), 1e-3f);
  const std::vector<double> response = magnitudeResponseDb(ir, {40.0, 100.0, 1000.0, 10000.0}, kRate);
  for (const double level : response)
    EXPECT_NEAR(level, 0.0, 0.1);

  // Mono in, the same on both outputs; a second render continues the first.
  const std::vector<float> in = noise(0.5, 3u, 0.25f, kRate);
  const Stereo out = reference.render(Stereo{in, in});
  EXPECT_EQ(out.left, out.right);
  EXPECT_NEAR(steadyStateGainDb(out.left, in, 4800), 0.0, 0.01);

  // Odd lengths end on a short block and do not disturb the stream.
  EffectRig a{juce::String()}, b{juce::String()};
  const std::vector<float> whole = a.render(in);
  std::vector<float> pieces = b.render(std::vector<float>(in.begin(), in.begin() + 1000));
  const std::vector<float> rest = b.render(std::vector<float>(in.begin() + 1000, in.end()));
  pieces.insert(pieces.end(), rest.begin(), rest.end());
  ASSERT_EQ(whole.size(), pieces.size());
  float worst = 0.0f;
  for (size_t i = 0; i < whole.size(); ++i)
    worst = std::max(worst, std::abs(whole[i] - pieces[i]));
  EXPECT_LT(worst, 1e-6f);
}

TEST(EffectHarnessTest, DualMonoRigCarriesTwoChannels) {
  EffectRig rig("utility.gain", {{"gainDb", 6.0}}, rigFor(Layout::DualMono));
  const std::vector<float> left = sine(0.25, 440.0, 0.1f, kRate);
  const std::vector<float> right = sine(0.25, 660.0, 0.2f, kRate);
  const Stereo out = rig.render(Stereo{left, right});
  EXPECT_NEAR(rmsOf(out.left, 2400) / rmsOf(left, 2400), 1.995, 0.01);
  EXPECT_NEAR(rmsOf(out.right, 2400) / rmsOf(right, 2400), 1.995, 0.01);
  EXPECT_GT(db(goertzelPower(out.right, 660.0)) - db(goertzelPower(out.right, 440.0)), 60.0);
}

TEST(EffectHarnessTest, LayoutsHandTheBlockTheBufferTheyPromise) {
  // What a block is handed, pinned for each way the processor can run it.
  auto& seen = TestSpillProcessor::seen();
  const std::vector<float> a = sine(0.1, 440.0, 0.2f, kRate);
  const std::vector<float> b = sine(0.1, 660.0, 0.1f, kRate);
  const Params rightHalved = {{"rightGain", 0.5}, {"mix", 1.0}};

  {
    // A one-channel bus: one channel.
    EffectRig rig("test.spill", rightHalved, rigFor(Layout::Mono));
    const Stereo out = rig.render(Stereo{a, b});
    EXPECT_EQ(seen.lastChannels.load(), 1);
    EXPECT_FALSE(seen.lastDualMono.load());
    EXPECT_EQ(out.left, out.right) << "a mono rig has one output";
    EXPECT_NEAR(rmsOf(out.left, 2400) / rmsOf(a, 2400), 1.0, 1e-3) << "the right input is unused";
  }
  {
    // A two-channel bus, default input mode: a true stereo buffer. The kind
    // receives the mono fold of the source on both channels, and what it
    // does to one channel reaches that output alone.
    EffectRig rig("test.spill", rightHalved, rigFor(Layout::Stereo));
    const Stereo out = rig.render(Stereo{a, b});
    EXPECT_EQ(seen.lastChannels.load(), 2);
    EXPECT_FALSE(seen.lastDualMono.load());
    float worst = 0.0f;
    for (size_t i = 0; i < a.size(); ++i)
      worst = std::max(worst, std::abs(out.right[i] - 0.5f * out.left[i]));
    EXPECT_LT(worst, 1e-6f) << "the channels the block wrote are the outputs";
    // The fold: half of each input on the left output.
    EffectRig folded("test.spill", {{"mix", 1.0}}, rigFor(Layout::Stereo));
    const Stereo sum = folded.render(Stereo{a, b});
    EXPECT_GT(db(goertzelPower(sum.left, 660.0)), db(goertzelPower(b, 660.0)) - 6.5);
    EXPECT_EQ(sum.left, sum.right);
  }
  {
    // The dual input mode: two channels, flagged, each its own signal.
    EffectRig rig("test.spill", rightHalved, rigFor(Layout::DualMono));
    const Stereo out = rig.render(Stereo{a, b});
    EXPECT_EQ(seen.lastChannels.load(), 2);
    EXPECT_TRUE(seen.lastDualMono.load());
    EXPECT_NEAR(rmsOf(out.right, 2400) / rmsOf(b, 2400), 1.0, 1e-3)
        << "the kind's stereo treatment stays off in dual mono";
    EXPECT_GT(db(goertzelPower(out.right, 660.0)) - db(goertzelPower(out.right, 440.0)), 60.0);
  }
  {
    // The processor's two-lane stereo mode: each lane is one channel.
    EffectRig rig("test.spill", rightHalved, rigFor(Layout::Stereo));
    rig.must("chain.setStereo", "{\"enabled\":true}");
    rig.restart();
    rig.render(Stereo{a, b});
    EXPECT_EQ(seen.lastChannels.load(), 1);
    EXPECT_FALSE(seen.lastDualMono.load());
  }
  {
    // Channels that differ on the way IN only exist behind a block that made
    // them differ; the kind alone can be handed them.
    DirectKind kind("test.spill", {{"rightGain", 0.5}}, Layout::Stereo);
    const Stereo out = kind.render(Stereo{a, b});
    EXPECT_EQ(seen.lastChannels.load(), 2);
    EXPECT_FALSE(seen.lastDualMono.load());
    EXPECT_EQ(out.left, a);
    EXPECT_FLOAT_EQ(out.right[100], 0.5f * b[100]);
    DirectKind dual("test.spill", {{"rightGain", 0.5}}, Layout::DualMono);
    EXPECT_EQ(dual.render(Stereo{a, b}).right, b);
    EXPECT_TRUE(seen.lastDualMono.load());

    // And in front of an amp model the stereo image has no future: the NAM
    // block keeps channel 0 only, so the kind is told to stay mono.
    DirectKind beforeAmp("test.spill", {{"rightGain", 0.5}}, Layout::Stereo);
    beforeAmp.ctx.blockIndex = 0;
    beforeAmp.ctx.nam.lastNamIndex = 2;
    EXPECT_EQ(beforeAmp.render(Stereo{a, b}).right, b);
  }
  BlockProcessContext ctx;
  EXPECT_TRUE(blockHasStereoImage(2, ctx));
  EXPECT_FALSE(blockHasStereoImage(1, ctx));
  ctx.dualMono = true;
  EXPECT_FALSE(blockHasStereoImage(2, ctx));
  ctx.dualMono = false;
  ctx.blockIndex = 4;
  ctx.nam.lastNamIndex = 3;
  EXPECT_TRUE(blockHasStereoImage(2, ctx)) << "the last amp is before this block";
  ctx.nam.lastNamIndex = 5;
  EXPECT_FALSE(blockHasStereoImage(2, ctx)) << "an amp still follows";
}

TEST(EffectHarnessTest, DelayEstimateFindsAKnownDelay) {
  const std::vector<float> in = withSilence(noise(0.25, 8u, 0.5f, kRate), 24000);
  for (const int delay : {1, 37, 4800, 20000}) {
    std::vector<float> out(in.size(), 0.0f);
    for (size_t i = static_cast<size_t>(delay); i < in.size(); ++i)
      out[i] = -0.3f * in[i - static_cast<size_t>(delay)];  // inverted and quieter
    EXPECT_EQ(estimateDelaySamples(out, in, 0, 24000), delay);
  }
  EXPECT_EQ(onsetIndex(impulse(100, 42)), 42);
  EXPECT_EQ(onsetIndex(std::vector<float>(100, 0.0f)), -1);
}

TEST(EffectHarnessTest, Rt60AndDecayTimeOfAnExponentialDecay) {
  for (const double rt60 : {0.4, 1.5}) {
    // Noise under an envelope that falls 60 dB in rt60 seconds.
    std::vector<float> ir = noise(rt60 * 1.6, 12u, 1.0f, kRate);
    for (size_t i = 0; i < ir.size(); ++i)
      ir[i] *= static_cast<float>(std::pow(10.0, -3.0 * (static_cast<double>(i) / kRate) / rt60));
    EXPECT_NEAR(estimateRt60Seconds(ir, kRate), rt60, rt60 * 0.05);
    EXPECT_NEAR(decayTimeSeconds(ir, 0, 1.0f, kRate), rt60, rt60 * 0.1);
  }
  // A capture too short to show the decay says so instead of guessing.
  EXPECT_EQ(estimateRt60Seconds(noise(0.1, 2u, 1.0f, kRate), kRate), 0.0);
}

TEST(EffectHarnessTest, MagnitudeResponseOfAKnownFilter) {
  const fx::BiquadCoeffs coeffs = fx::BiquadCoeffs::peak(1000.0, 2.0, 9.0, kRate);
  fx::Biquad filter;
  filter.setCoeffs(coeffs);
  std::vector<float> ir = impulse(8192);
  for (float& v : ir)
    v = filter.process(v);
  const std::vector<double> freqs = {100.0, 700.0, 1000.0, 1500.0, 8000.0};
  const std::vector<double> measured = magnitudeResponseDb(ir, freqs, kRate);
  for (size_t i = 0; i < freqs.size(); ++i)
    EXPECT_NEAR(measured[i], toDb(coeffs.magnitudeAt(freqs[i], kRate)), 0.02) << freqs[i];
  EXPECT_NEAR(measured[2], 9.0, 0.02);
}

TEST(EffectHarnessTest, SineProbeAndThd) {
  EffectRig rig("utility.gain", {{"gainDb", -9.0}});
  EXPECT_NEAR(sineProbeGainDb(rig, 1000.0), -9.0, 0.05);
  EXPECT_NEAR(sineProbeGainDb(rig, 150.0), -9.0, 0.05);

  // A sine with 1% third and 0.5% fifth harmonic: THD = sqrt(1^2 + 0.5^2) %.
  std::vector<float> x(48000);
  for (size_t i = 0; i < x.size(); ++i) {
    const double t = static_cast<double>(i) / kRate;
    x[i] = static_cast<float>(0.5 * (std::sin(2.0 * kPi * 500.0 * t) +
                                     0.01 * std::sin(2.0 * kPi * 1500.0 * t) +
                                     0.005 * std::sin(2.0 * kPi * 2500.0 * t)));
  }
  EXPECT_NEAR(thdPercent(x, 500.0, kRate), std::sqrt(1.0 + 0.25), 0.02);
  EXPECT_LT(thdPercent(sine(1.0, 500.0, 0.5f, kRate), 500.0, kRate), 0.01);
  // A hard-clipped sine is far from clean.
  std::vector<float> clipped = sine(1.0, 500.0, 1.0f, kRate);
  for (float& v : clipped)
    v = juce::jlimit(-0.5f, 0.5f, v);
  EXPECT_GT(thdPercent(clipped, 500.0, kRate), 10.0);
}

TEST(EffectHarnessTest, ModulationRates) {
  for (const double rate : {0.7, 3.0, 8.0}) {
    SCOPED_TRACE(rate);
    // Tremolo: a 440 Hz carrier, 60% deep.
    std::vector<float> tremolo(static_cast<size_t>(6.0 * kRate));
    // Vibrato: the same carrier, +-10 Hz.
    std::vector<float> vibrato(tremolo.size());
    double phase = 0.0;
    for (size_t i = 0; i < tremolo.size(); ++i) {
      const double t = static_cast<double>(i) / kRate;
      const double lfo = std::sin(2.0 * kPi * rate * t);
      tremolo[i] = static_cast<float>(0.5 * (1.0 + 0.6 * lfo) * std::sin(2.0 * kPi * 440.0 * t));
      phase += 2.0 * kPi * (440.0 + 10.0 * lfo) / kRate;
      vibrato[i] = static_cast<float>(0.5 * std::sin(phase));
    }
    EXPECT_NEAR(amplitudeModulationRateHz(tremolo, kRate), rate, rate * 0.03);
    EXPECT_NEAR(frequencyModulationRateHz(vibrato, kRate), rate, rate * 0.03);
  }
  // Nothing modulated, nothing found.
  const std::vector<float> steady = sine(3.0, 440.0, 0.5f, kRate);
  EXPECT_EQ(amplitudeModulationRateHz(steady, kRate, 4800), 0.0);
}

TEST(EffectHarnessTest, SignalScanAndSteps) {
  std::vector<float> x = sine(0.1, 100.0, 0.5f, kRate);
  SignalScan scan = scanSignal(x);
  EXPECT_TRUE(scan.finite());
  EXPECT_EQ(scan.denormals, 0);
  EXPECT_NEAR(scan.peak, 0.5f, 1e-3f);
  // 0.5 at 100 Hz moves 0.0065 per sample.
  EXPECT_NEAR(maxStep(x, 0, x.size()), 0.5f * 2.0f * static_cast<float>(kPi) * 100.0f / 48000.0f,
              1e-4f);

  x[10] = std::numeric_limits<float>::quiet_NaN();
  x[20] = std::numeric_limits<float>::infinity();
  x[30] = std::numeric_limits<float>::denorm_min() * 8.0f;
  x[31] = -std::numeric_limits<float>::denorm_min();
  scan = scanSignal(x);
  EXPECT_TRUE(scan.hasNaN);
  EXPECT_TRUE(scan.hasInf);
  EXPECT_EQ(scan.denormals, 2);
  EXPECT_FALSE(scan.finite());

  std::vector<float> clicked = sine(0.1, 100.0, 0.5f, kRate);
  clicked[2000] += 0.2f;
  EXPECT_GT(maxStep(clicked, 0, clicked.size()), 0.19f);
}

TEST(EffectHarnessTest, DirectKindIsTheWetPathAlone) {
  // A gain run on its own: exactly the gain, from the first sample, with no
  // rig around it.
  DirectKind gain("utility.gain", {{"gainDb", 6.0206}});
  ASSERT_TRUE(gain.valid());
  const std::vector<float> in = noise(0.1, 4u, 0.25f, kRate);
  const std::vector<float> out = gain.render(in);
  for (size_t i = 0; i < in.size(); i += 97)
    EXPECT_NEAR(out[i], 2.0f * in[i], 1e-5f);
  EXPECT_EQ(gain.latencySamples(), 0);
  EXPECT_EQ(gain.tailSeconds(), 0.0);

  // The delay on its own: the echo and nothing else, at the tempo given.
  DirectKind delay("delay.simple", {{"sync", 1.0}, {"division", static_cast<double>(fx::kDivQuarter)},
                                    {"feedback", 0.0}},
                   Layout::Mono, 128, 100.0);
  const std::vector<float> ir = delay.render(impulse(48000));
  EXPECT_EQ(peakIndexOf(ir), 28800);  // 600 ms
  EXPECT_FLOAT_EQ(ir[28800], 1.0f);
  EXPECT_EQ(ir[0], 0.0f);
  delay.prepare();
  EXPECT_EQ(delay.render(impulse(48000)), ir);
}

TEST(EffectHarnessTest, CpuCostIsMeasuredPerBlock) {
  const KindCost gain = measureKindCost("utility.gain", {{"gainDb", 3.0}}, 500);
  EXPECT_EQ(gain.blocks, 500);
  EXPECT_GT(gain.averageNs, 0.0);
  EXPECT_GE(gain.worstNs, gain.averageNs);
  // A gain on 128 stereo frames: far under a tenth of a millisecond.
  EXPECT_LT(gain.averageNs, 100000.0);
}

TEST(EffectHarnessTest, TheGenericChecksPassOnTheGainKind) {
  // utility.gain predates the effects folder and is no effect file, but it
  // is a well-behaved kind: the reusable checks hold for it, which is what
  // shows they are not tuned to delay.simple.
  const Params params = {{"gainDb", -6.0}, {"pan", 0.25}};
  expectBypassNull("utility.gain", params);
  expectStateRoundTrip("utility.gain", {{"gainDb", -6.0}, {"pan", 0.25}, {"phaseInvert", 1.0}});
  expectStableUnderParameterSweep("utility.gain", params, 2.0, 8.0f);
  expectLatencyMatches("utility.gain", params, LatencyCheck::MatchesOnset);
  expectDeterministic("utility.gain", params);
  expectRateAndBlockSizeIndependent("utility.gain", params);
  expectDualMonoIsTwoMonoRigs("utility.gain", {{"gainDb", -6.0}});
  expectHandlesTrueStereoInput("utility.gain", params);
  for (const Layout layout : {Layout::Mono, Layout::DualMono}) {
    expectBypassNull("utility.gain", params, rigFor(layout));
    expectDeterministic("utility.gain", params, rigFor(layout));
    expectRateAndBlockSizeIndependent("utility.gain", params, -60.0, 0.5, layout);
  }
  const TailMeasurement tail = expectSilenceAfterReportedTail("utility.gain", params);
  EXPECT_EQ(tail.reportedSeconds, 0.0);
  EXPECT_EQ(tail.measuredSeconds, 0.0);

  // And they notice a kind that is not what it says: test.latency delays by
  // 64 samples and reports it, so its onset matches; lie about nothing here,
  // just show the latent path through the same checks.
  expectLatencyMatches("test.latency", {}, LatencyCheck::MatchesOnset);
  expectBypassNull("test.latency");
}

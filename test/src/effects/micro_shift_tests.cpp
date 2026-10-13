// modulation.doubler "Micro Shift" (plugin/src/effects/modulation/MicroShift.cpp):
// the conformance test, then what a doubler is for: two voices a few cents
// sharp and flat, each behind a short delay, spread across a true stereo
// image by gains that add back to the same two voices, and a pitch that is
// the stated number of cents all the way through the wraps of the delay
// ramp.
//
// The voices are measured one at a time through the spread: at 100% the left
// side is the flat voice alone and the right side the sharp one.
#include "effects/effect_test_helpers.h"
#include "effects/modulation_test_helpers.h"

#include <gtest/gtest.h>

using namespace fxtest;
using namespace modtest;

namespace {

constexpr const char* kKind = "modulation.doubler";

// Mean pitch of a rendered sine in cents against its carrier: cycles counted
// between the first and the last upward zero crossing, which weights time,
// not cycles.
double meanCents(const std::vector<float>& x, double carrierHz, size_t skip) {
  std::vector<double> crossingTimes;
  for (size_t i = std::max<size_t>(skip, 1); i < x.size(); ++i)
    if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
      const double frac = static_cast<double>(-x[i - 1]) /
                          (static_cast<double>(x[i]) - static_cast<double>(x[i - 1]));
      crossingTimes.push_back((static_cast<double>(i - 1) + frac) / kRate);
    }
  if (crossingTimes.size() < 8)
    return 0.0;
  const double hz = static_cast<double>(crossingTimes.size() - 1) /
                    (crossingTimes.back() - crossingTimes.front());
  return cents(hz, carrierHz);
}

}  // namespace

TEST(MicroShiftTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"detuneCents", 20.0}, {"delayMs", 25.0}, {"spread", 80.0}};
  options.stateParams = {{"detuneCents", 7.0}, {"delayMs", 31.0}, {"spread", 40.0},
                         {"toneHz", 6000.0}};
  // The wet is two delayed voices (10 ms or more), never the input itself:
  // the effect, not latency. The kind reports none.
  options.latency = LatencyCheck::AtMostOnset;
  options.cpuAverageBudgetNs = 40000.0;
  options.cpuWorstBudgetNs = 300000.0;
  expectKindConforms("modulation.doubler", options);
}

TEST(MicroShiftTest, RegisteredAsACrossfadeAtFiftyPercent) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kKind);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->category, BlockCategory::Modulation);
  EXPECT_EQ(info->mixLaw, BlockMixLaw::Crossfade);
  EXPECT_FLOAT_EQ(info->defaultMix, 0.5f);
  EXPECT_EQ(info->name, juce::String("Micro Shift"));
}

TEST(MicroShiftTest, VoicesAreDetunedByTheStatedCentsUpAndDown) {
  // 1 kHz and 500 Hz: the two taps of a voice sit half a window (4 ms) apart,
  // a whole number of cycles of either, so the fades between them are
  // seamless and the zero crossings follow one pitch. 4 s covers several
  // wraps of the ramp.
  for (const double carrierHz : {1000.0, 500.0})
    for (const double detune : {4.0, 12.0, 30.0}) {
      SCOPED_TRACE(carrierHz);
      SCOPED_TRACE(detune);
      EffectRig rig(kKind, {{"detuneCents", detune}, {"spread", 100.0}, {"toneHz", 20000.0},
                            {"mix", 1.0}});
      const Stereo out = rig.renderBoth(sine(4.0, carrierHz, 0.25f, kRate));
      const size_t skip = static_cast<size_t>(samples(0.3));
      // Left: the flat voice. Right: the sharp one.
      EXPECT_NEAR(meanCents(out.left, carrierHz, skip), -detune, 0.4);
      EXPECT_NEAR(meanCents(out.right, carrierHz, skip), detune, 0.4);
    }
}

TEST(MicroShiftTest, NoDetuneLeavesTwoPlainDelays) {
  // Voices at 85% and 115% of the delay: 17 ms and 23 ms at 20 ms.
  EffectRig rig(kKind, {{"detuneCents", 0.0}, {"delayMs", 20.0}, {"spread", 0.0}, {"toneHz", 20000.0},
                        {"mix", 1.0}});
  EffectRig reference{juce::String()};
  const int origin = peakIndexOf(captureImpulseResponse(reference, 0.05));
  const std::vector<float> ir = captureImpulseResponse(rig, 0.1);
  const int first = peakIndexOf(std::vector<float>(ir.begin(), ir.begin() + ms(20.0)));
  EXPECT_NEAR(first - origin, ms(17.0), 2);
  const std::vector<float> second(ir.begin() + ms(20.0), ir.end());
  EXPECT_NEAR(peakIndexOf(second) + ms(20.0) - origin, ms(23.0), 2);
  // Two voices at 1 / sqrt(2), less the 20 kHz high cut.
  EXPECT_NEAR(peakOf(ir), 0.7f, 0.08f);
  // Nothing else: the input does not reach the output undelayed.
  EXPECT_LT(peakOf(std::vector<float>(ir.begin(), ir.begin() + ms(15.0))), 0.02f);
}

TEST(MicroShiftTest, DelayParameterMovesBothVoices) {
  for (const double delayMs : {12.0, 30.0}) {
    SCOPED_TRACE(delayMs);
    EffectRig rig(kKind, {{"detuneCents", 0.0}, {"delayMs", delayMs}, {"spread", 0.0},
                          {"toneHz", 20000.0}, {"mix", 1.0}});
    EffectRig reference{juce::String()};
    const int origin = peakIndexOf(captureImpulseResponse(reference, 0.05));
    const std::vector<float> ir = captureImpulseResponse(rig, 0.12);
    const int split = ms(delayMs);
    const int first = peakIndexOf(std::vector<float>(ir.begin(), ir.begin() + split));
    EXPECT_NEAR(first - origin, ms(0.85 * delayMs), 2);
    const std::vector<float> second(ir.begin() + split, ir.end());
    EXPECT_NEAR(peakIndexOf(second) + split - origin, ms(1.15 * delayMs), 2);
  }
}

TEST(MicroShiftTest, PitchHoldsThroughTheWrapsOfTheRamp) {
  // Fastest ramp (50 cents wraps every 0.27 s): over 5 s the level of the
  // sharp voice, a steady sine, stays put to a fraction of a dB.
  EffectRig rig(kKind, {{"detuneCents", 50.0}, {"spread", 100.0}, {"toneHz", 20000.0}, {"mix", 1.0}});
  const Stereo out = rig.renderBoth(sine(5.0, 1000.0, 0.25f, kRate));
  for (const std::vector<float>* side : {&out.left, &out.right}) {
    double top = 0.0, bottom = 1.0e9;
    const size_t window = static_cast<size_t>(samples(0.01));
    for (size_t begin = static_cast<size_t>(samples(0.5)); begin + window <= side->size();
         begin += window) {
      const double level = rmsOf(*side, begin, begin + window);
      top = std::max(top, level);
      bottom = std::min(bottom, level);
    }
    EXPECT_NEAR(top, 0.25 / std::sqrt(2.0), 0.01);
    EXPECT_GT(bottom / top, 0.97) << "the fade between the taps dips";
  }
  expectCleanSignal(out.left, 0.3f, "micro shift, left");
}

TEST(MicroShiftTest, SpreadMovesOnlyGainsSoTheSidesAddBackToTheSameVoices) {
  const std::vector<float> in = noise(3.0, 7u, 0.25f, kRate);
  const auto render = [&](double spread) {
    EffectRig rig(kKind, {{"spread", spread}, {"mix", 1.0}, {"toneHz", 20000.0}});
    return rig.renderBoth(in);
  };
  const Stereo centred = render(0.0);
  const Stereo wide = render(100.0);
  float worst = 0.0f;
  for (size_t i = static_cast<size_t>(samples(0.2)); i < in.size(); ++i)
    worst = std::max(worst, std::abs((centred.left[i] + centred.right[i]) -
                                     1.41421356f * (wide.left[i] + wide.right[i])));
  EXPECT_LT(worst, 2.0e-4f);
  EXPECT_LT(maxDifference(centred.left, centred.right), 1.0e-6f);
  EXPECT_GT(relativeDifferenceDb(wide.left, wide.right, static_cast<size_t>(samples(0.2))), -6.0);
  // Level of each side holds as the spread moves.
  EXPECT_NEAR(toDb(rmsOf(wide.left, static_cast<size_t>(samples(0.2)))),
              toDb(rmsOf(centred.left, static_cast<size_t>(samples(0.2)))), 1.2);
}

TEST(MicroShiftTest, SidesDifferOnlyInATrueStereoImage) {
  const std::vector<float> in = noise(2.0, 3u, 0.25f, kRate);
  {
    EffectRig rig(kKind, {{"spread", 100.0}, {"mix", 1.0}}, rigFor(Layout::DualMono));
    const Stereo out = rig.renderBoth(in);
    EXPECT_LT(maxDifference(out.left, out.right), 1.0e-6f);
  }
  {
    DirectKind kind(kKind, {{"spread", 100.0}}, Layout::Stereo);
    kind.ctx.nam.lastNamIndex = 3;
    const Stereo out = kind.render(Stereo{in, in});
    EXPECT_LT(maxDifference(out.left, out.right), 1.0e-6f);
  }
}

TEST(MicroShiftTest, WetLevelFollowsTheInput) {
  EffectRig rig(kKind, {{"toneHz", 20000.0}, {"mix", 1.0}, {"spread", 0.0}});
  const std::vector<float> in = noise(4.0, 9u, 0.25f, kRate);
  const std::vector<float> out = rig.render(in);
  // Two voices at 1 / sqrt(2) each, 12 cents apart and 3.5 ms apart in time,
  // so they partly cancel at high frequencies (white noise): measured -2.7
  // dB with the 20 kHz high cut's 1.3 dB included.
  EXPECT_NEAR(steadyStateGainDb(out, in, static_cast<size_t>(samples(0.5))), -2.7, 1.0);
}

TEST(MicroShiftTest, ToneIsAHighCutOnTheWet) {
  const auto gainAt8k = [](double toneHz) {
    EffectRig rig(kKind, {{"detuneCents", 0.0}, {"toneHz", toneHz}, {"mix", 1.0}});
    return sineProbeGainDb(rig, 8000.0);
  };
  EXPECT_LT(gainAt8k(1000.0), gainAt8k(20000.0) - 14.0);
}

TEST(MicroShiftTest, StaysBoundedAtTheExtremes) {
  for (const Layout layout : {Layout::Mono, Layout::Stereo}) {
    DirectKind kind(kKind, {{"detuneCents", 50.0}, {"delayMs", 40.0}, {"toneHz", 20000.0}}, layout);
    const std::vector<float> hot = noise(5.0, 17u, 1.0f, kRate);
    const Stereo out = kind.render(Stereo{hot, hot});
    expectCleanSignal(out.left, 3.0f, "extreme micro shift, left");
    expectCleanSignal(out.right, 3.0f, "extreme micro shift, right");
  }
}

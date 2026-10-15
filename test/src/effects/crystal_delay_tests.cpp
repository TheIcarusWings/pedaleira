// delay.crystal: a delay whose feedback loop pitch shifts every pass. Beyond
// the conformance test: the first repeat lands on the set time (free and
// synced), each repeat moves by the set interval (a sine's repeats measured
// pass by pass), unison is a plain delay, the loop stays bounded and free of
// DC at 95 % feedback with hot input for 30 s, the shifted repeats have no
// clicks, and the reported tail covers the decay.
#include "effects/delay_test_support.h"
#include "effects/dsp/TempoSync.h"

#include <gtest/gtest.h>

#include <iostream>
#include <numeric>

using namespace delaytest;

namespace {

constexpr const char* kKind = "delay.crystal";

// One clean pass: no modulation, the tone and the low cut wide open.
const Params kClean = {{"modDepth", 0.0}, {"toneHz", 20000.0}, {"lowCutHz", 20.0}, {"direct", 0.0},
                       {"voice2", 0.0}};

std::vector<float> wetOf(const Params& params, const std::vector<float>& in, double bpm = 120.0) {
  DirectKind direct(kKind, params, Layout::Mono, 256, bpm);
  return direct.render(Stereo{in, in}).left;
}

// A sine with 5 ms raised-cosine edges, `seconds` long, then silence to
// `totalSeconds`.
std::vector<float> softBurst(double seconds, double hz, float amplitude, double totalSeconds) {
  std::vector<float> x(static_cast<size_t>(std::lround(totalSeconds * kRate)), 0.0f);
  const size_t n = static_cast<size_t>(std::lround(seconds * kRate));
  const size_t edge = static_cast<size_t>(ms(5.0));
  for (size_t i = 0; i < n && i < x.size(); ++i) {
    double gain = 1.0;
    if (i < edge)
      gain = 0.5 - 0.5 * std::cos(kPi * static_cast<double>(i) / static_cast<double>(edge));
    else if (n - i < edge)
      gain = 0.5 - 0.5 * std::cos(kPi * static_cast<double>(n - i) / static_cast<double>(edge));
    x[i] = amplitude * static_cast<float>(gain * std::sin(2.0 * kPi * hz * static_cast<double>(i) / kRate));
  }
  return x;
}

// The frequency of the output between two times, from its zero crossings.
double frequencyBetween(const std::vector<float>& out, double fromMs, double toMs) {
  return analysePitchModulation(out, static_cast<size_t>(ms(fromMs)), 1.0, 1.0,
                                static_cast<size_t>(ms(toMs)))
      .carrierHz;
}

size_t loudestSample(const std::vector<float>& x, size_t begin, size_t end) {
  size_t best = begin;
  for (size_t i = begin; i < end && i < x.size(); ++i)
    if (std::abs(x[i]) > std::abs(x[best]))
      best = i;
  return best;
}

}  // namespace

TEST(CrystalDelayTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"timeMs", 150.0}, {"feedback", 50.0}, {"voice2", 1.0}, {"direct", 20.0}};
  options.stateParams = {{"sync", 1.0},          {"division", fx::kDivEighth}, {"timeMs", 333.0},
                         {"feedback", 65.0},     {"pitch", -5.0},              {"voice2", 2.0},
                         {"voice2Level", 70.0},  {"direct", 25.0},             {"toneHz", 5000.0},
                         {"lowCutHz", 250.0},    {"modDepth", 60.0},           {"duckDb", 6.0},
                         {"duckReleaseMs", 400.0}, {"spillover", 0.0}};
  // The wet path is the echo: it starts at the set time, not at once.
  options.latency = LatencyCheck::AtMostOnset;
  // Two shifters run here (voice 2 on): about twice pitch.shifter's cost.
  options.cpuAverageBudgetNs = 110000.0;
  options.cpuWorstBudgetNs = 1200000.0;
  expectKindConforms("delay.crystal", options);
}

TEST(CrystalDelayTest, ParametersAreTheDocumentedRanges) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kKind);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->name, "Crystal Delay");
  EXPECT_EQ(info->category, BlockCategory::Delay);
  EXPECT_EQ(info->mixLaw, BlockMixLaw::DryPreserving);
  const auto spec = [&](const char* id) -> const BlockParamSpec* {
    const int index = findBlockParam(info->params, id);
    return index < 0 ? nullptr : &info->params[static_cast<size_t>(index)];
  };
  ASSERT_NE(spec("timeMs"), nullptr);
  EXPECT_TRUE(spec("timeMs")->tempoSync);
  EXPECT_NE(spec("sync"), nullptr);
  EXPECT_NE(spec("division"), nullptr);
  ASSERT_NE(spec("feedback"), nullptr);
  EXPECT_FLOAT_EQ(spec("feedback")->min, 0.0f);
  EXPECT_FLOAT_EQ(spec("feedback")->max, 95.0f);
  ASSERT_NE(spec("pitch"), nullptr);
  EXPECT_FLOAT_EQ(spec("pitch")->min, -12.0f);
  EXPECT_FLOAT_EQ(spec("pitch")->max, 12.0f);
  EXPECT_FLOAT_EQ(spec("pitch")->defaultValue, 12.0f);
  ASSERT_NE(spec("voice2"), nullptr);
  EXPECT_EQ(spec("voice2")->choices, juce::StringArray({"Off", "Octave Down", "Fifth"}));
  for (const char* id : {"voice2Level", "direct", "toneHz", "lowCutHz", "modDepth", "duckDb", "duckReleaseMs"})
    EXPECT_NE(spec(id), nullptr) << id;
}

TEST(CrystalDelayTest, TheFirstRepeatArrivesAtTheSetTime) {
  // An impulse, one pass (no feedback): the shifted echo, the unshifted one
  // and a mix of both all land on the time.
  std::vector<float> in(static_cast<size_t>(ms(800.0)), 0.0f);
  const size_t at = 1000;
  in[at] = 0.8f;
  for (const double pitch : {12.0, 7.0, -5.0, -12.0}) {
    for (const double direct : {0.0, 100.0}) {
      SCOPED_TRACE("pitch " + std::to_string(pitch) + " direct " + std::to_string(direct));
      const std::vector<float> out = wetOf(
          withParams(kClean, {{"timeMs", 300.0}, {"feedback", 0.0}, {"pitch", pitch}, {"direct", direct}}), in);
      ASSERT_GT(peakOf(out), 0.2f);
      const long arrival = static_cast<long>(loudestSample(out, 0, out.size()));
      EXPECT_NEAR(arrival, static_cast<long>(at) + ms(300.0), 4);
    }
  }
}

TEST(CrystalDelayTest, SyncedTimeFollowsTheTempo) {
  std::vector<float> in(static_cast<size_t>(ms(1200.0)), 0.0f);
  const size_t at = 1000;
  in[at] = 0.8f;
  const Params synced = withParams(kClean, {{"sync", 1.0}, {"division", fx::kDivQuarter}, {"feedback", 0.0}});
  for (const double bpm : {120.0, 90.0}) {
    SCOPED_TRACE(bpm);
    const std::vector<float> out = wetOf(synced, in, bpm);
    const long expected = static_cast<long>(at) + static_cast<long>(std::lround(60.0 / bpm * kRate));
    EXPECT_NEAR(static_cast<long>(loudestSample(out, 0, out.size())), expected, 4);
  }
}

TEST(CrystalDelayTest, EachRepeatIsShiftedByTheSetInterval) {
  // A 200 ms sine, 400 ms repeats: repeat n is the input moved n times.
  struct Case {
    double pitch;
    double inputHz;
    int passes;
  };
  for (const Case c : {Case{12.0, 220.0, 4}, Case{-12.0, 1760.0, 3}, Case{7.0, 220.0, 4}, Case{-5.0, 660.0, 4}}) {
    const std::vector<float> in = softBurst(0.2, c.inputHz, 0.5f, 0.4 * (c.passes + 1));
    const std::vector<float> out =
        wetOf(withParams(kClean, {{"timeMs", 400.0}, {"feedback", 70.0}, {"pitch", c.pitch}}), in);
    const double ratio = std::pow(2.0, c.pitch / 12.0);
    for (int n = 1; n <= c.passes; ++n) {
      const double expected = c.inputHz * std::pow(ratio, n);
      const double measured = frequencyBetween(out, 400.0 * n + 70.0, 400.0 * n + 170.0);
      std::cout << "[  PITCH   ] " << c.pitch << " st, repeat " << n << ": " << measured << " Hz (expected "
                << expected << ", error " << 100.0 * (measured / expected - 1.0) << " %)" << std::endl;
      EXPECT_NEAR(measured / expected, 1.0, 0.01) << c.pitch << " st, repeat " << n;
    }
  }
}

TEST(CrystalDelayTest, AtUnisonItIsAPlainDelay) {
  const std::vector<float> in = noise(0.6, 4u, 0.3f, kRate);
  const std::vector<float> out =
      wetOf(withParams(kClean, {{"timeMs", 250.0}, {"feedback", 0.0}, {"pitch", 0.0}}), in);
  const size_t delay = static_cast<size_t>(ms(250.0));
  float worst = 0.0f;
  for (size_t i = delay; i < in.size(); ++i)
    worst = std::max(worst, std::abs(out[i] - in[i - delay]));
  EXPECT_LT(worst, 1.0e-6f);
}

TEST(CrystalDelayTest, TheSecondVoiceAndTheDirectShareAreHeard) {
  // One pass of a 440 Hz sine at +12: the octave-down voice adds 220 Hz, the
  // fifth adds 659 Hz, and the direct share brings 440 Hz back.
  const std::vector<float> in = sine(1.0, 440.0, 0.5f, kRate);
  const auto band = [&](const Params& extra, double hz) {
    const std::vector<float> out =
        wetOf(withParams(kClean, withParams({{"timeMs", 200.0}, {"feedback", 0.0}, {"pitch", 12.0}}, extra)), in);
    return bandPower(out, static_cast<size_t>(ms(500.0)), hz * 0.97, hz * 1.03);
  };
  const double main = band({}, 880.0);
  ASSERT_GT(main, 1.0);
  EXPECT_LT(band({}, 220.0), main * 1.0e-3);
  EXPECT_GT(band({{"voice2", 1.0}, {"voice2Level", 100.0}}, 220.0), main * 0.2);
  EXPECT_GT(band({{"voice2", 2.0}, {"voice2Level", 100.0}}, 440.0 * std::pow(2.0, 7.0 / 12.0)), main * 0.2);
  EXPECT_LT(band({}, 440.0), main * 1.0e-3);
  EXPECT_GT(band({{"direct", 50.0}}, 440.0), main * 0.1);
}

TEST(CrystalDelayTest, TheLoopStaysBoundedAt95PercentWithHotInputFor30Seconds) {
  // 15 s of full-scale noise, then 15 s of a near full-scale low E: no
  // runaway, no DC, nothing non-finite, and the level settles instead of
  // climbing.
  const std::vector<float> loud = noise(15.0, 41u, 1.0f, kRate);
  const std::vector<float> tone = sine(15.0, 82.4, 0.95f, kRate);
  std::vector<float> in = loud;
  in.insert(in.end(), tone.begin(), tone.end());
  const std::vector<Params> settings = {
      {{"pitch", 12.0}},
      {{"pitch", -12.0}},
      {{"pitch", 12.0}, {"voice2", 2.0}, {"voice2Level", 100.0}, {"direct", 30.0}},
      {{"pitch", 7.0}, {"voice2", 1.0}, {"voice2Level", 100.0}, {"modDepth", 100.0}},
      {{"pitch", -12.0}, {"voice2", 1.0}, {"voice2Level", 100.0}, {"lowCutHz", 20.0}, {"toneHz", 20000.0}},
  };
  for (const Params& extra : settings) {
    // Through the rig (the chain's 5 Hz DC blocker included, the mix all
    // wet): the soft limit, fully driven here, bends an asymmetric pass and
    // so makes a little DC of its own on the way out, which the chain takes
    // off; what is measured is that the loop does not collect any.
    EffectRig rig(kKind, withParams({{"timeMs", 300.0}, {"feedback", 95.0}, {"mix", 1.0}}, extra));
    const std::vector<float> out = rig.render(in);
    expectCleanSignal(out, 1.5f, "30 s of hot input at 95% feedback");
    const size_t lastSeconds = out.size() - static_cast<size_t>(5.0 * kRate);
    const double mean =
        std::accumulate(out.begin() + static_cast<std::ptrdiff_t>(lastSeconds), out.end(), 0.0) /
        static_cast<double>(out.size() - lastSeconds);
    const double early = rmsOf(out, static_cast<size_t>(20.0 * kRate), static_cast<size_t>(25.0 * kRate));
    const double late = rmsOf(out, lastSeconds, out.size());
    std::cout << "[  LOOP    ] peak " << peakOf(out) << ", rms 20-25 s " << early << ", 25-30 s " << late
              << ", mean " << mean << std::endl;
    EXPECT_LT(std::abs(mean), 0.01) << "DC in the loop";
    EXPECT_LT(late, 0.9);
    EXPECT_LT(late, early * 1.25 + 0.01) << "the level keeps climbing";
  }
}

TEST(CrystalDelayTest, ShiftedRepeatsHaveNoClicks) {
  // A steady sine: the largest step between two samples of the wet output is
  // what the shifted sine itself makes, with a little room; a splice that
  // jumped would step by far more. One pass at each interval, and a falling
  // loop whose content is all at or below the first repeat's frequency.
  const double inputHz = 440.0;
  const std::vector<float> in = sine(4.0, inputHz, 0.5f, kRate);
  struct Case {
    double pitch;
    double feedback;
  };
  for (const Case c : {Case{12.0, 0.0}, Case{7.0, 0.0}, Case{-12.0, 0.0}, Case{-12.0, 60.0}, Case{-5.0, 60.0}}) {
    const std::vector<float> out =
        wetOf(withParams(kClean, {{"timeMs", 200.0}, {"feedback", c.feedback}, {"pitch", c.pitch}}), in);
    const size_t from = static_cast<size_t>(ms(1000.0));
    const double topHz = inputHz * std::max(1.0, std::pow(2.0, c.pitch / 12.0));
    const float peak = peakOf(out, from);
    const float step = maxStep(out, from, out.size());
    const double smooth = static_cast<double>(peak) * 2.0 * kPi * topHz / kRate;
    std::cout << "[  CLICK   ] " << c.pitch << " st, feedback " << c.feedback << ": largest step " << step
              << ", sine's own " << smooth << std::endl;
    ASSERT_GT(peak, 0.1f);
    EXPECT_LT(step, smooth * 1.3) << c.pitch << " st, feedback " << c.feedback;
  }
}

TEST(CrystalDelayTest, TheTailCoversTheDecay) {
  const Params params = withParams(kClean, {{"timeMs", 300.0}, {"feedback", 60.0}, {"pitch", -5.0}});
  const std::vector<float> burst = noise(0.3, 12u, 0.4f, kRate);
  std::vector<float> in(static_cast<size_t>(ms(15000.0)), 0.0f);
  std::copy(burst.begin(), burst.end(), in.begin());
  DirectKind direct(kKind, params, Layout::Mono);
  const std::vector<float> out = direct.render(Stereo{in, in}).left;
  const double decay = decayTimeSeconds(out, burst.size(), peakOf(out), kRate);
  std::cout << "[  TAIL    ] decay " << decay << " s, reported " << direct.tailSeconds() << " s" << std::endl;
  EXPECT_GT(decay, 1.0) << "the feedback does ring";
  EXPECT_LE(decay, direct.tailSeconds() + 0.1);
  EXPECT_LT(DirectKind(kKind, withParams(params, {{"feedback", 0.0}})).tailSeconds(), direct.tailSeconds());
  EXPECT_GT(DirectKind(kKind, withParams(params, {{"timeMs", 900.0}})).tailSeconds(), direct.tailSeconds());
}

TEST(CrystalDelayTest, ResetLeavesNothingInTheLoop) {
  for (const Layout layout : {Layout::Mono, Layout::Stereo, Layout::DualMono}) {
    SCOPED_TRACE(layoutName(layout));
    DirectKind direct(kKind, {{"timeMs", 150.0}, {"feedback", 80.0}, {"voice2", 1.0}}, layout);
    const std::vector<float> in = noise(1.0, 7u, 0.3f, kRate);
    const Stereo played = direct.render(Stereo{in, in});
    ASSERT_GT(peakOf(played.left), 0.05f);
    direct.engine().reset();
    const std::vector<float> quiet(48000, 0.0f);
    const Stereo after = direct.render(Stereo{quiet, quiet});
    EXPECT_EQ(peakOf(after.left), 0.0f);
    EXPECT_EQ(peakOf(after.right), 0.0f);
  }
}

TEST(CrystalDelayTest, WithVoiceTwoOffItCostsOneShifter) {
  // The cost is the shifters' (PitchShift, one per voice): at the default,
  // voice 2 off, its engine does not run, and the block costs about what
  // pitch.shifter does. The conformance test covers both voices. Measured
  // on a Linux container: 45,000 ns average, 290,000 ns worst with one voice
  // and 78,000 / 600,000 ns with two (pitch.shifter: 25,000 / 270,000 ns).
  expectCpuUnderBudget(kKind, {}, 70000.0, 600000.0);
}

TEST(CrystalDelayTest, SpilloverRingsOutAfterBypassAndStopsAtTheReportedTail) {
  expectSpilloverBehaviour(kKind,
                           {{"timeMs", 200.0}, {"feedback", 35.0}, {"pitch", 7.0}, {"toneHz", 20000.0},
                            {"lowCutHz", 20.0}},
                           100.0, 400.0, 6.0e-3f);
}

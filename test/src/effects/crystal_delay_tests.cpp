// delay.crystal: two pitch-shifted taps, each with its own line, time,
// interval, level and pan, each shifting again on every pass of its own
// feedback loop. Beyond the conformance test: each tap lands on its own time
// (free and synced), tap 1's repeats climb by its interval pass after pass
// (the octave cascade: +12, +24, +36 semitones), tap 2's detune walks the
// same way a few cents at a time, the pan law is constant power at cos / sin
// with the middle 3 dB down, the levels are true dB, the feedback sets the
// loss per pass, unison is a plain delay, the loops stay bounded and free of
// DC at 95 % feedback with hot input for 30 s and at every extreme, the
// shifted repeats have no clicks, the reported tail covers the decay, and a
// block saved by the single-tap version loads sounding as it did.
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
// One tap heard, the other off.
const Params kTap1Only = withParams(kClean, {{"tap2Db", -60.0}});
const Params kTap2Only = withParams(kClean, {{"tap1Db", -60.0}});

double ratioDb(double ratio) { return 20.0 * std::log10(std::max(ratio, 1.0e-12)); }

std::vector<float> wetOf(const Params& params, const std::vector<float>& in, double bpm = 120.0) {
  DirectKind direct(kKind, params, Layout::Mono, 256, bpm);
  return direct.render(Stereo{in, in}).left;
}

Stereo stereoWetOf(const Params& params, const std::vector<float>& in) {
  DirectKind direct(kKind, params, Layout::Stereo, 256);
  return direct.render(Stereo{in, in});
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

// The most the wet output can reach: the two tap levels summed (each tap's
// repeat is soft limited to 1, each pan gain is at most 1), with a little
// room for the chain's DC blocker in a rig.
float levelCeiling(const Params& params) {
  double db[2] = {0.0, 0.0};
  for (const auto& [id, value] : params) {
    if (id == "tap1Db")
      db[0] = value;
    if (id == "tap2Db")
      db[1] = value;
  }
  return static_cast<float>(std::pow(10.0, db[0] / 20.0) + std::pow(10.0, db[1] / 20.0) + 0.05);
}

double rmsBetween(const std::vector<float>& x, double fromMs, double toMs) {
  return rmsOf(x, static_cast<size_t>(ms(fromMs)), static_cast<size_t>(ms(toMs)));
}

}  // namespace

TEST(CrystalDelayTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"timeMs", 150.0}, {"tap2TimeMs", 220.0}, {"feedback", 50.0}, {"voice2", 1.0}, {"direct", 20.0}};
  options.stateParams = {{"sync", 1.0},          {"division", fx::kDivEighth}, {"timeMs", 333.0},
                         {"pitch", -5.0},        {"detune1", 3.0},             {"tap1Db", -6.0},
                         {"tap1Pan", 20.0},      {"tap2Sync", 1.0},            {"tap2Division", fx::kDivEighth},
                         {"tap2TimeMs", 777.0},  {"pitch2", 7.0},              {"detune2", 12.0},
                         {"tap2Db", -12.0},      {"tap2Pan", -30.0},           {"panMode", 0.0},
                         {"feedback", 65.0},     {"voice2", 2.0},              {"voice2Level", 70.0},
                         {"direct", 25.0},       {"toneHz", 5000.0},           {"lowCutHz", 250.0},
                         {"modDepth", 60.0},     {"duckDb", 6.0},              {"duckReleaseMs", 400.0},
                         {"spillover", 0.0}};
  // The wet path is the echo: it starts at the set time, not at once.
  options.latency = LatencyCheck::AtMostOnset;
  // Three shifters run here (both taps and tap 1's harmony): about three
  // times pitch.shifter's cost.
  options.cpuAverageBudgetNs = 160000.0;
  options.cpuWorstBudgetNs = 1500000.0;
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
  for (const char* id : {"timeMs", "tap2TimeMs"}) {
    ASSERT_NE(spec(id), nullptr) << id;
    EXPECT_TRUE(spec(id)->tempoSync) << id;
    EXPECT_FLOAT_EQ(spec(id)->min, 50.0f) << id;
    EXPECT_FLOAT_EQ(spec(id)->max, 6000.0f) << id;
  }
  for (const char* id : {"sync", "division", "tap2Sync", "tap2Division"})
    EXPECT_NE(spec(id), nullptr) << id;
  ASSERT_NE(spec("feedback"), nullptr);
  EXPECT_FLOAT_EQ(spec("feedback")->min, 0.0f);
  EXPECT_FLOAT_EQ(spec("feedback")->max, 95.0f);
  for (const char* id : {"pitch", "pitch2"}) {
    ASSERT_NE(spec(id), nullptr) << id;
    EXPECT_FLOAT_EQ(spec(id)->min, -12.0f);
    EXPECT_FLOAT_EQ(spec(id)->max, 12.0f);
  }
  EXPECT_FLOAT_EQ(spec("pitch")->defaultValue, 12.0f) << "tap 1 is the octave";
  EXPECT_FLOAT_EQ(spec("pitch2")->defaultValue, 0.0f) << "tap 2 is the detuned double";
  for (const char* id : {"detune1", "detune2"}) {
    ASSERT_NE(spec(id), nullptr) << id;
    EXPECT_FLOAT_EQ(spec(id)->min, -50.0f);
    EXPECT_FLOAT_EQ(spec(id)->max, 50.0f);
  }
  EXPECT_FLOAT_EQ(spec("detune2")->defaultValue, -6.0f);
  for (const char* id : {"tap1Db", "tap2Db"}) {
    ASSERT_NE(spec(id), nullptr) << id;
    EXPECT_FLOAT_EQ(spec(id)->min, -60.0f) << "the bottom is off";
    EXPECT_FLOAT_EQ(spec(id)->max, 6.0f);
    EXPECT_FLOAT_EQ(spec(id)->defaultValue, 0.0f);
  }
  for (const char* id : {"tap1Pan", "tap2Pan"}) {
    ASSERT_NE(spec(id), nullptr) << id;
    EXPECT_FLOAT_EQ(spec(id)->min, -100.0f);
    EXPECT_FLOAT_EQ(spec(id)->max, 100.0f);
  }
  ASSERT_NE(spec("panMode"), nullptr);
  EXPECT_EQ(spec("panMode")->choices, juce::StringArray({"Centre", "Panned"}));
  // The single-tap version's ids keep their meaning and their choice order.
  ASSERT_NE(spec("voice2"), nullptr);
  EXPECT_EQ(spec("voice2")->choices, juce::StringArray({"Off", "Octave Down", "Fifth"}));
  for (const char* id : {"voice2Level", "direct", "toneHz", "lowCutHz", "modDepth", "duckDb", "duckReleaseMs"})
    EXPECT_NE(spec(id), nullptr) << id;
}

TEST(CrystalDelayTest, EachTapArrivesAtItsOwnSetTime) {
  // An impulse, one pass (no feedback): each tap's echo, shifted or plain,
  // lands on its own time; with both heard, two echoes at the two times.
  std::vector<float> in(static_cast<size_t>(ms(900.0)), 0.0f);
  const size_t at = 1000;
  in[at] = 0.8f;
  for (const double pitch : {12.0, 7.0, -5.0, -12.0}) {
    for (const double direct : {0.0, 100.0}) {
      SCOPED_TRACE("pitch " + std::to_string(pitch) + " direct " + std::to_string(direct));
      const std::vector<float> out = wetOf(
          withParams(kTap1Only, {{"timeMs", 300.0}, {"feedback", 0.0}, {"pitch", pitch}, {"direct", direct}}),
          in);
      ASSERT_GT(peakOf(out), 0.1f);
      EXPECT_NEAR(static_cast<long>(loudestSample(out, 0, out.size())), static_cast<long>(at) + ms(300.0), 4);
    }
  }
  for (const double detune : {-6.0, 0.0, 30.0}) {
    SCOPED_TRACE("tap 2 detune " + std::to_string(detune));
    const std::vector<float> out =
        wetOf(withParams(kTap2Only, {{"tap2TimeMs", 450.0}, {"feedback", 0.0}, {"detune2", detune}}), in);
    ASSERT_GT(peakOf(out), 0.1f);
    EXPECT_NEAR(static_cast<long>(loudestSample(out, 0, out.size())), static_cast<long>(at) + ms(450.0), 4);
  }
  const std::vector<float> both =
      wetOf(withParams(kClean, {{"timeMs", 200.0}, {"tap2TimeMs", 550.0}, {"feedback", 0.0}}), in);
  const long first = static_cast<long>(loudestSample(both, 0, at + static_cast<size_t>(ms(375.0))));
  const long second = static_cast<long>(loudestSample(both, at + static_cast<size_t>(ms(375.0)), both.size()));
  EXPECT_NEAR(first, static_cast<long>(at) + ms(200.0), 4);
  EXPECT_NEAR(second, static_cast<long>(at) + ms(550.0), 4);
}

TEST(CrystalDelayTest, SyncedTimesFollowTheTempo) {
  std::vector<float> in(static_cast<size_t>(ms(1400.0)), 0.0f);
  const size_t at = 1000;
  in[at] = 0.8f;
  const Params synced1 = withParams(kTap1Only, {{"sync", 1.0}, {"division", fx::kDivQuarter}, {"feedback", 0.0}});
  const Params synced2 =
      withParams(kTap2Only, {{"tap2Sync", 1.0}, {"tap2Division", fx::kDivHalf}, {"feedback", 0.0}});
  for (const double bpm : {120.0, 180.0}) {
    SCOPED_TRACE(bpm);
    const long beat = static_cast<long>(std::lround(60.0 / bpm * kRate));
    const std::vector<float> out1 = wetOf(synced1, in, bpm);
    EXPECT_NEAR(static_cast<long>(loudestSample(out1, 0, out1.size())), static_cast<long>(at) + beat, 4);
    const std::vector<float> out2 = wetOf(synced2, in, bpm);
    EXPECT_NEAR(static_cast<long>(loudestSample(out2, 0, out2.size())), static_cast<long>(at) + 2 * beat, 4);
  }
}

TEST(CrystalDelayTest, TapOneCascadesItsIntervalPassAfterPass) {
  // A 200 ms sine, 400 ms repeats: repeat n is the input moved n times. At
  // +12 that is the octave cascade (+12, +24, +36, +48 semitones).
  struct Case {
    double pitch;
    double inputHz;
    int passes;
  };
  for (const Case c : {Case{12.0, 220.0, 4}, Case{-12.0, 1760.0, 3}, Case{7.0, 220.0, 4}, Case{-5.0, 660.0, 4}}) {
    const std::vector<float> in = softBurst(0.2, c.inputHz, 0.5f, 0.4 * (c.passes + 1));
    const std::vector<float> out =
        wetOf(withParams(kTap1Only, {{"timeMs", 400.0}, {"feedback", 70.0}, {"pitch", c.pitch}}), in);
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

TEST(CrystalDelayTest, TapTwoIsADetunedDoubleThatWalksFurtherEachPass) {
  // Tap 2 at its default, 6 cents flat: one pass of a steady 1 kHz sine is
  // 6 cents flat, and with feedback each repeat is 6 cents flatter than the
  // one before. Measured by zero crossings over 100 ms of each repeat.
  const std::vector<float> steady = sine(2.0, 1000.0, 0.5f, kRate);
  const std::vector<float> once = wetOf(withParams(kTap2Only, {{"tap2TimeMs", 300.0}, {"feedback", 0.0}}), steady);
  const double cents = 1200.0 * std::log2(frequencyBetween(once, 600.0, 1900.0) / 1000.0);
  std::cout << "[  DETUNE  ] one pass: " << cents << " cents" << std::endl;
  EXPECT_NEAR(cents, -6.0, 0.3);

  const std::vector<float> burst = softBurst(0.2, 1000.0, 0.5f, 2.0);
  const std::vector<float> out =
      wetOf(withParams(kTap2Only, {{"tap2TimeMs", 400.0}, {"feedback", 70.0}}), burst);
  for (int n = 1; n <= 3; ++n) {
    const double measured = 1200.0 * std::log2(frequencyBetween(out, 400.0 * n + 50.0, 400.0 * n + 170.0) / 1000.0);
    std::cout << "[  DETUNE  ] repeat " << n << ": " << measured << " cents (expected " << -6.0 * n << ")"
              << std::endl;
    EXPECT_NEAR(measured, -6.0 * n, 1.5) << "repeat " << n;
  }
}

TEST(CrystalDelayTest, ThePanIsConstantPowerAtCosAndSin) {
  // One tap, plain (unison) so the echo is the input, in a stereo image: a
  // pan of p puts it at cos and sin of (p + 100) / 200 * 90 degrees on the
  // left and the right. Hard left at 0 dB is the input at unity on the left.
  const std::vector<float> in = sine(1.0, 500.0, 0.5f, kRate);
  const double inputRms = rmsBetween(in, 500.0, 900.0);
  for (const double pan : {-100.0, -50.0, 0.0, 30.0, 100.0}) {
    SCOPED_TRACE("pan " + std::to_string(pan));
    const Stereo out = stereoWetOf(
        withParams(kTap1Only, {{"timeMs", 200.0}, {"feedback", 0.0}, {"pitch", 0.0}, {"tap1Pan", pan}}), in);
    const double angle = (pan + 100.0) / 200.0 * kPi / 2.0;
    const double left = rmsBetween(out.left, 500.0, 900.0) / inputRms;
    const double right = rmsBetween(out.right, 500.0, 900.0) / inputRms;
    std::cout << "[  PAN     ] " << pan << ": left " << ratioDb(left) << " dB, right " << ratioDb(right) << " dB"
              << std::endl;
    EXPECT_NEAR(left, std::cos(angle), 0.01);
    EXPECT_NEAR(right, std::sin(angle), 0.01);
    EXPECT_NEAR(left * left + right * right, 1.0, 0.02) << "constant power";
  }
  // Centre mode puts both taps in the middle whatever their pans: 3 dB down
  // on each side.
  const Stereo centred = stereoWetOf(
      withParams(kTap1Only, {{"timeMs", 200.0}, {"feedback", 0.0}, {"pitch", 0.0}, {"tap1Pan", -100.0},
                             {"panMode", 0.0}}),
      in);
  EXPECT_NEAR(ratioDb(rmsBetween(centred.left, 500.0, 900.0) / inputRms), -3.01, 0.1);
  EXPECT_NEAR(ratioDb(rmsBetween(centred.right, 500.0, 900.0) / inputRms), -3.01, 0.1);
  // A mono row hears the fold of the two sides: the middle is 3 dB down,
  // hard to one side 6 dB down.
  const auto monoDb = [&](double pan) {
    const std::vector<float> out =
        wetOf(withParams(kTap1Only, {{"timeMs", 200.0}, {"feedback", 0.0}, {"pitch", 0.0}, {"tap1Pan", pan}}), in);
    return ratioDb(rmsBetween(out, 500.0, 900.0) / inputRms);
  };
  EXPECT_NEAR(monoDb(0.0), -3.01, 0.1);
  EXPECT_NEAR(monoDb(-100.0), -6.02, 0.1);
}

TEST(CrystalDelayTest, TheTwoTapsSitWhereTheirPansPutThem) {
  // Default pans (-50 and +50): tap 1 louder on the left, tap 2 on the
  // right, by the same cos / sin split.
  std::vector<float> in(static_cast<size_t>(ms(1000.0)), 0.0f);
  const std::vector<float> burst = softBurst(0.1, 500.0, 0.5f, 0.1);
  std::copy(burst.begin(), burst.end(), in.begin());
  const Stereo out = stereoWetOf(
      withParams(kClean, {{"timeMs", 200.0}, {"tap2TimeMs", 600.0}, {"feedback", 0.0}, {"pitch", 0.0}, {"detune2", 0.0}}),
      in);
  const double expected = ratioDb(std::tan(kPi / 8.0));  // sin / cos of 22.5 degrees: -7.66 dB
  const double tap1 = ratioDb(rmsBetween(out.right, 210.0, 290.0) / rmsBetween(out.left, 210.0, 290.0));
  const double tap2 = ratioDb(rmsBetween(out.left, 610.0, 690.0) / rmsBetween(out.right, 610.0, 690.0));
  std::cout << "[  PAN     ] tap 1 right re left " << tap1 << " dB, tap 2 left re right " << tap2 << " dB"
            << std::endl;
  EXPECT_NEAR(tap1, expected, 0.2);
  EXPECT_NEAR(tap2, expected, 0.2);
}

TEST(CrystalDelayTest, TheLevelsAreTrueDecibels) {
  // -40 dB is 40 dB down, not muted; the bottom of the range is off.
  const std::vector<float> in = sine(1.0, 500.0, 0.5f, kRate);
  const auto levelOf = [&](double db) {
    const std::vector<float> out =
        wetOf(withParams(kTap2Only, {{"tap2TimeMs", 200.0}, {"feedback", 0.0}, {"detune2", 0.0}, {"tap2Db", db}}), in);
    return rmsBetween(out, 500.0, 900.0);
  };
  const double reference = levelOf(0.0);
  ASSERT_GT(reference, 0.05);
  for (const double db : {-40.0, -12.0, 6.0}) {
    const double measured = ratioDb(levelOf(db) / reference);
    std::cout << "[  LEVEL   ] " << db << " dB: " << measured << " dB" << std::endl;
    EXPECT_NEAR(measured, db, 0.05);
  }
  EXPECT_EQ(levelOf(-60.0), 0.0) << "off";
}

TEST(CrystalDelayTest, EachPassLosesWhatTheFeedbackSays) {
  // A 100 ms burst of 150 Hz, 400 ms repeats: each repeat is the feedback
  // times the one before (within the tone and low cut, wide open here). On
  // tap 2 (6 cents flat) the step is the feedback within 0.25 dB. On tap 1's
  // octave cascade, kept well in band (300 to 2400 Hz), the octave shifter's
  // splice fades take a little more, measured 0.3 to 0.6 dB a pass: the
  // cascade fades a touch faster than its percentage says and never slower.
  // Higher up the tone filter and the band limit take their share too, which
  // is what ends an upward cascade.
  const std::vector<float> in = softBurst(0.1, 150.0, 0.5f, 2.2);
  struct Case {
    Params tap;
    double extraLossDb;
  };
  const std::vector<Case> cases = {{withParams(kTap2Only, {{"tap2TimeMs", 400.0}}), 0.25},
                                   {withParams(kTap1Only, {{"timeMs", 400.0}}), 0.8}};
  for (const double feedback : {50.0, 80.0}) {
    for (const Case& c : cases) {
      const std::vector<float> out = wetOf(withParams(c.tap, {{"feedback", feedback}}), in);
      const double nominal = ratioDb(feedback * 0.01);
      for (int n = 1; n <= 3; ++n) {
        const double step = ratioDb(rmsBetween(out, 400.0 * (n + 1) + 20.0, 400.0 * (n + 1) + 80.0) /
                                    rmsBetween(out, 400.0 * n + 20.0, 400.0 * n + 80.0));
        std::cout << "[  PASS    ] feedback " << feedback << ", repeat " << n << " to " << n + 1 << ": " << step
                  << " dB (expected " << nominal << ")" << std::endl;
        EXPECT_LT(step, nominal + 0.1) << "feedback " << feedback << ", repeat " << n;
        EXPECT_GT(step, nominal - c.extraLossDb) << "feedback " << feedback << ", repeat " << n;
      }
    }
  }
}

TEST(CrystalDelayTest, AtUnisonATapIsAPlainDelay) {
  // Pitch 0 and no detune: the echo is the input, delayed, through the
  // tone and the low cut (wide open). An impulse comes back as one tight
  // click: nearly all its energy within 8 samples of the set time.
  std::vector<float> in(static_cast<size_t>(ms(600.0)), 0.0f);
  const size_t at = 1000;
  in[at] = 0.8f;
  for (const Params& tap : {withParams(kTap1Only, {{"timeMs", 250.0}, {"pitch", 0.0}}),
                            withParams(kTap2Only, {{"tap2TimeMs", 250.0}, {"detune2", 0.0}})}) {
    const std::vector<float> out = wetOf(withParams(tap, {{"feedback", 0.0}}), in);
    const size_t echo = at + static_cast<size_t>(ms(250.0));
    double near8 = 0.0;
    double all = 0.0;
    for (size_t i = 0; i < out.size(); ++i) {
      const double e = static_cast<double>(out[i]) * out[i];
      all += e;
      if (i + 8 >= echo && i <= echo + 8)
        near8 += e;
    }
    ASSERT_GT(all, 0.0);
    EXPECT_GT(near8 / all, 0.99);
    EXPECT_NEAR(static_cast<long>(loudestSample(out, 0, out.size())), static_cast<long>(echo), 1);
  }
}

TEST(CrystalDelayTest, TheLoopsStayBoundedAt95PercentWithHotInputFor30Seconds) {
  // 15 s of full-scale noise, then 15 s of a near full-scale low E: no
  // runaway, no DC, nothing non-finite, and the level settles instead of
  // climbing.
  const std::vector<float> loud = noise(15.0, 41u, 1.0f, kRate);
  const std::vector<float> tone = sine(15.0, 82.4, 0.95f, kRate);
  std::vector<float> in = loud;
  in.insert(in.end(), tone.begin(), tone.end());
  const std::vector<Params> settings = {
      {},
      {{"pitch", -12.0}, {"pitch2", 12.0}},
      {{"pitch", 12.0}, {"voice2", 2.0}, {"voice2Level", 100.0}, {"direct", 30.0}, {"tap1Db", 6.0}, {"tap2Db", 6.0}},
      {{"pitch", 7.0}, {"voice2", 1.0}, {"voice2Level", 100.0}, {"modDepth", 100.0}, {"detune2", -50.0}},
      {{"pitch", -12.0}, {"voice2", 1.0}, {"voice2Level", 100.0}, {"lowCutHz", 20.0}, {"toneHz", 20000.0},
       {"pitch2", 0.0}, {"detune2", 0.0}},
  };
  for (const Params& extra : settings) {
    // Through the rig (the chain's 5 Hz DC blocker included, the mix all
    // wet): the soft limit, fully driven here, bends an asymmetric pass and
    // so makes a little DC of its own on the way out, which the chain takes
    // off; what is measured is that the loop does not collect any.
    const Params params = withParams({{"timeMs", 300.0}, {"tap2TimeMs", 470.0}, {"feedback", 95.0}, {"mix", 1.0}}, extra);
    EffectRig rig(kKind, params);
    const std::vector<float> out = rig.render(in);
    expectCleanSignal(out, levelCeiling(params), "30 s of hot input at 95% feedback");
    const size_t lastSeconds = out.size() - static_cast<size_t>(5.0 * kRate);
    const double mean =
        std::accumulate(out.begin() + static_cast<std::ptrdiff_t>(lastSeconds), out.end(), 0.0) /
        static_cast<double>(out.size() - lastSeconds);
    const double early = rmsOf(out, static_cast<size_t>(20.0 * kRate), static_cast<size_t>(25.0 * kRate));
    const double late = rmsOf(out, lastSeconds, out.size());
    std::cout << "[  LOOP    ] peak " << peakOf(out) << ", rms 20-25 s " << early << ", 25-30 s " << late
              << ", mean " << mean << std::endl;
    EXPECT_LT(std::abs(mean), 0.01) << "DC in the loop";
    EXPECT_LT(late, 0.5 * levelCeiling(params));
    EXPECT_LT(late, early * 1.25 + 0.01) << "the level keeps climbing";
  }
}

TEST(CrystalDelayTest, EveryExtremeStaysFiniteAndBounded) {
  // Every parameter at an end of its range, in groups: shortest and longest
  // times, widest intervals and detunes, darkest and brightest filters,
  // everything up. The output stays finite and bounded and does not grow.
  const std::vector<float> in = noise(6.0, 9u, 0.8f, kRate);
  const std::vector<Params> settings = {
      {{"timeMs", 50.0}, {"tap2TimeMs", 50.0}, {"pitch", 12.0}, {"detune1", 50.0}, {"pitch2", 12.0},
       {"detune2", 50.0}, {"modDepth", 100.0}, {"voice2", 2.0}, {"voice2Level", 100.0}, {"direct", 100.0}},
      {{"timeMs", 50.0}, {"tap2TimeMs", 6000.0}, {"pitch", -12.0}, {"detune1", -50.0}, {"pitch2", -12.0},
       {"detune2", -50.0}, {"toneHz", 1000.0}, {"lowCutHz", 2000.0}},
      {{"timeMs", 6000.0}, {"tap2TimeMs", 50.0}, {"tap1Db", 6.0}, {"tap2Db", 6.0}, {"panMode", 0.0},
       {"duckDb", 24.0}, {"duckReleaseMs", 50.0}},
      {{"tap1Db", -60.0}, {"tap2Db", -60.0}},
  };
  for (const Layout layout : {Layout::Mono, Layout::Stereo, Layout::DualMono}) {
    for (const Params& extra : settings) {
      SCOPED_TRACE(layoutName(layout));
      DirectKind direct(kKind, withParams({{"feedback", 95.0}}, extra), layout, 128);
      const Stereo out = direct.render(Stereo{in, in});
      expectCleanSignal(out.left, levelCeiling(extra), "an extreme setting, left");
      expectCleanSignal(out.right, levelCeiling(extra), "an extreme setting, right");
      const double early = rmsOf(out.left, static_cast<size_t>(3.0 * kRate), static_cast<size_t>(4.5 * kRate));
      const double late = rmsOf(out.left, static_cast<size_t>(4.5 * kRate), out.left.size());
      EXPECT_LT(late, early * 1.5 + 0.01) << "the level keeps climbing";
    }
  }
}

TEST(CrystalDelayTest, ShiftedRepeatsHaveNoClicks) {
  // A steady sine: the largest step between two samples of the wet output is
  // what the shifted sine itself makes, with a little room; a splice that
  // jumped would step by far more. One pass at each interval, and a falling
  // loop whose content is all at or below the first repeat's frequency, on
  // tap 1; tap 2's detuned loop.
  const double inputHz = 440.0;
  const std::vector<float> in = sine(4.0, inputHz, 0.5f, kRate);
  struct Case {
    Params params;
    double topRatio;
  };
  const std::vector<Case> cases = {
      {withParams(kTap1Only, {{"feedback", 0.0}, {"pitch", 12.0}}), 2.0},
      {withParams(kTap1Only, {{"feedback", 0.0}, {"pitch", 7.0}}), std::pow(2.0, 7.0 / 12.0)},
      {withParams(kTap1Only, {{"feedback", 0.0}, {"pitch", -12.0}}), 1.0},
      {withParams(kTap1Only, {{"feedback", 60.0}, {"pitch", -12.0}}), 1.0},
      {withParams(kTap1Only, {{"feedback", 60.0}, {"pitch", -5.0}}), 1.0},
      {withParams(kTap2Only, {{"feedback", 60.0}}), 1.0},
  };
  for (const Case& c : cases) {
    const std::vector<float> out = wetOf(withParams(c.params, {{"timeMs", 200.0}, {"tap2TimeMs", 200.0}}), in);
    const size_t from = static_cast<size_t>(ms(1000.0));
    const float peak = peakOf(out, from);
    const float step = maxStep(out, from, out.size());
    const double smooth = static_cast<double>(peak) * 2.0 * kPi * inputHz * c.topRatio / kRate;
    std::cout << "[  CLICK   ] largest step " << step << ", sine's own " << smooth << std::endl;
    ASSERT_GT(peak, 0.1f);
    EXPECT_LT(step, smooth * 1.3);
  }
}

TEST(CrystalDelayTest, TheTailCoversTheDecay) {
  const Params params =
      withParams(kClean, {{"timeMs", 300.0}, {"tap2TimeMs", 700.0}, {"feedback", 60.0}, {"pitch", -5.0}});
  const std::vector<float> burst = noise(0.3, 12u, 0.4f, kRate);
  std::vector<float> in(static_cast<size_t>(ms(20000.0)), 0.0f);
  std::copy(burst.begin(), burst.end(), in.begin());
  DirectKind direct(kKind, params, Layout::Mono);
  const std::vector<float> out = direct.render(Stereo{in, in}).left;
  const double decay = decayTimeSeconds(out, burst.size(), peakOf(out), kRate);
  std::cout << "[  TAIL    ] decay " << decay << " s, reported " << direct.tailSeconds() << " s" << std::endl;
  EXPECT_GT(decay, 1.0) << "the feedback does ring";
  EXPECT_LE(decay, direct.tailSeconds() + 0.1);
  EXPECT_LT(DirectKind(kKind, withParams(params, {{"feedback", 0.0}})).tailSeconds(), direct.tailSeconds());
  EXPECT_GT(DirectKind(kKind, withParams(params, {{"tap2TimeMs", 1500.0}})).tailSeconds(), direct.tailSeconds())
      << "the longer line sets the tail";
}

TEST(CrystalDelayTest, ResetLeavesNothingInTheLoops) {
  for (const Layout layout : {Layout::Mono, Layout::Stereo, Layout::DualMono}) {
    SCOPED_TRACE(layoutName(layout));
    DirectKind direct(kKind, {{"timeMs", 150.0}, {"tap2TimeMs", 260.0}, {"feedback", 80.0}, {"voice2", 1.0}},
                      layout);
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

TEST(CrystalDelayTest, ABlockSavedWithOneTapLoadsAsItSounded) {
  // A state of the single-tap version (version 1, no tap 2 ids): tap 2 is
  // off, the pan mode is centre and tap 1 is 3 dB up, so the one voice is at
  // unity on both sides as it was. The saved values are kept.
  DirectKind direct(kKind, {}, Layout::Stereo);
  juce::ValueTree old("KindState");
  old.setProperty("kind", kKind, nullptr);
  old.setProperty("version", 1, nullptr);
  for (const auto& [id, value] : Params{{"timeMs", 200.0}, {"feedback", 0.0}, {"pitch", 0.0}, {"toneHz", 20000.0},
                                        {"lowCutHz", 20.0}, {"modDepth", 0.0}}) {
    juce::ValueTree param("Param");
    param.setProperty("id", juce::String(id), nullptr);
    param.setProperty("value", value, nullptr);
    old.appendChild(param, nullptr);
  }
  BlockProcessor& engine = direct.engine();
  engine.setKindState(old);
  EXPECT_FLOAT_EQ(engine.getParameter(engine.findParameter("tap2Db")), -60.0f);
  EXPECT_FLOAT_EQ(engine.getParameter(engine.findParameter("panMode")), 0.0f);
  EXPECT_NEAR(engine.getParameter(engine.findParameter("tap1Db")), 3.0103f, 1e-3f);
  EXPECT_FLOAT_EQ(engine.getParameter(engine.findParameter("timeMs")), 200.0f);
  direct.prepare();
  const std::vector<float> in = sine(1.0, 500.0, 0.5f, kRate);
  const Stereo out = direct.render(Stereo{in, in});
  const double inputRms = rmsBetween(in, 500.0, 900.0);
  EXPECT_NEAR(ratioDb(rmsBetween(out.left, 500.0, 900.0) / inputRms), 0.0, 0.05);
  EXPECT_NEAR(ratioDb(rmsBetween(out.right, 500.0, 900.0) / inputRms), 0.0, 0.05);

  // A state of this version is taken as it is.
  juce::ValueTree current = engine.getKindState("Crystal Delay");
  EXPECT_EQ(static_cast<int>(current.getProperty("version")), 2);
  DirectKind fresh(kKind, {}, Layout::Stereo);
  fresh.engine().setKindState(DirectKind(kKind, {{"tap2Db", -3.0}, {"panMode", 1.0}}).engine().getKindState("x"));
  EXPECT_FLOAT_EQ(fresh.engine().getParameter(fresh.engine().findParameter("tap2Db")), -3.0f);
  EXPECT_FLOAT_EQ(fresh.engine().getParameter(fresh.engine().findParameter("panMode")), 1.0f);
}

TEST(CrystalDelayTest, AtItsDefaultItCostsTwoShifters) {
  // The cost is the shifters' (PitchShift, one per voice): at the default
  // both taps run and the harmony does not, so the block costs about twice
  // what pitch.shifter does. The conformance test covers all three. Measured
  // on a Linux container: 62,000 ns average, 340,000 ns worst at the default
  // and 85,000 to 92,000 / 530,000 to 740,000 ns with all three (the
  // single-tap version: 45,000 / 290,000 ns with one shifter).
  expectCpuUnderBudget(kKind, {}, 110000.0, 1200000.0);
}

TEST(CrystalDelayTest, SpilloverRingsOutAfterBypassAndStopsAtTheReportedTail) {
  expectSpilloverBehaviour(kKind,
                           {{"timeMs", 200.0}, {"tap2TimeMs", 260.0}, {"feedback", 35.0}, {"pitch", 7.0},
                            {"toneHz", 20000.0}, {"lowCutHz", 20.0}},
                           100.0, 400.0, 6.0e-3f);
}

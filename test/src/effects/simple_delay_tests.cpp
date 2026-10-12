// delay.simple (plugin/src/effects/delay/SimpleDelay.cpp), the reference
// built-in effect: the conformance test every effect runs, then what is
// specific to this one: its delay time, free and tempo-synced, and the tail
// spillover it is the first kind to declare.
#include "effects/dsp/TempoSync.h"
#include "effects/effect_test_helpers.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

using namespace fxtest;

namespace {

constexpr const char* kKind = "delay.simple";
constexpr double kRate = 48000.0;

int ms(double milliseconds) { return static_cast<int>(std::lround(milliseconds * 0.001 * kRate)); }

// out - reference, sample by sample: what the block adds to the dry signal.
std::vector<float> minus(const std::vector<float>& a, const std::vector<float>& b) {
  std::vector<float> d(a.size());
  for (size_t i = 0; i < a.size(); ++i)
    d[i] = a[i] - b[i];
  return d;
}

}  // namespace

TEST(SimpleDelayTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"timeMs", 120.0}, {"feedback", 40.0}};
  options.stateParams = {{"sync", 1.0},      {"division", fx::kDivEighthDotted},
                         {"timeMs", 123.0},  {"feedback", 55.0},
                         {"toneHz", 2500.0}, {"spillover", 0.0}};
  // The wet path is the echo alone: it starts a delay time after the input,
  // which is the effect and not latency. The kind reports none.
  options.latency = LatencyCheck::AtMostOnset;
  expectKindConforms("delay.simple", options);
}

TEST(SimpleDelayTest, DelayTimeMatchesTheParameter) {
  for (const double timeMs : {25.0, 250.0, 1234.0}) {
    SCOPED_TRACE(timeMs);
    EffectRig rig(kKind, {{"timeMs", timeMs}, {"feedback", 50.0}, {"toneHz", 20000.0},
                          {"mix", 1.0}});
    EffectRig reference{juce::String()};
    const int origin = peakIndexOf(captureImpulseResponse(reference, 0.05));
    const std::vector<float> ir = captureImpulseResponse(rig, 3.0);

    // The first echo, and the second one a delay later at the feedback level.
    const int first = peakIndexOf(ir) - origin;
    EXPECT_NEAR(first, ms(timeMs), 1);
    const size_t second = static_cast<size_t>(origin + 2 * ms(timeMs));
    const float firstLevel = peakOf(ir, static_cast<size_t>(origin + first) - 2,
                                    static_cast<size_t>(origin + first) + 3);
    const float secondLevel = peakOf(ir, second - 3, second + 4);
    EXPECT_NEAR(secondLevel / firstLevel, 0.5f, 0.05f);

    // The same answer from noise, by cross-correlation.
    rig.restart();
    const std::vector<float> in = withSilence(noise(0.2, 3u, 0.25f, kRate), ms(timeMs) + 4800);
    EXPECT_NEAR(estimateDelaySamples(rig.render(in), in, 1, ms(timeMs) + 2400), ms(timeMs), 1);
  }
}

TEST(SimpleDelayTest, SyncedTimeFollowsTheTempo) {
  struct Case {
    double bpm;
    int division;
    double expectedMs;
  };
  // An eighth at 100 bpm, a dotted eighth at 150: 300 ms both ways round the
  // beat, then a quarter at 150.
  for (const Case& c : {Case{100.0, fx::kDivEighth, 300.0}, Case{150.0, fx::kDivEighthDotted, 300.0},
                        Case{150.0, fx::kDivQuarter, 400.0}}) {
    SCOPED_TRACE(c.bpm);
    RigOptions options;
    options.bpm = c.bpm;
    EffectRig rig(kKind, {{"sync", 1.0}, {"division", static_cast<double>(c.division)},
                          {"timeMs", 77.0}, {"feedback", 0.0}, {"mix", 1.0}},
                  options);
    const std::vector<float> ir = captureImpulseResponse(rig, 1.0);
    EXPECT_NEAR(peakIndexOf(ir), ms(c.expectedMs), 1);
  }

  // A tempo change on a running rig moves the time, and the reported tail
  // with it once the rig has seen the new tempo.
  EffectRig rig(kKind, {{"sync", 1.0}, {"division", static_cast<double>(fx::kDivQuarter)},
                        {"feedback", 0.0}, {"mix", 1.0}});
  rig.renderSilence(4800);
  const double tailAt120 = rig.tailSeconds();
  rig.setTempo(60.0);
  rig.renderSilence(48000);  // the glide to the new time
  const std::vector<float> out = rig.render(impulse(96000));
  EXPECT_NEAR(peakIndexOf(out), ms(1000.0), 1);
  EXPECT_NEAR(rig.tailSeconds() - tailAt120, 0.5, 1e-6);
}

TEST(SimpleDelayTest, TailFollowsTimeAndFeedback) {
  EffectRig rig(kKind, {{"timeMs", 200.0}, {"feedback", 0.0}});
  // No feedback: one echo, a delay time after the input (plus the glide
  // allowance).
  EXPECT_NEAR(rig.tailSeconds(), 0.2 + 0.05, 1e-6);
  // 50%: the tenth echo after the first is 60 dB down (0.5^10 < 0.001).
  rig.set("feedback", 50.0);
  EXPECT_NEAR(rig.tailSeconds(), 0.2 * 11.0 + 0.05, 1e-6);
  rig.set("timeMs", 100.0);
  EXPECT_NEAR(rig.tailSeconds(), 0.1 * 11.0 + 0.05, 1e-6);
  // And the processor reports the longest block tail to the host.
  rig.settle();
  EXPECT_GE(rig.proc.getTailLengthSeconds(), 1.15 - 1e-6);
}

// A burst, the block switched off right after it, then silence.
struct BypassRender {
  std::vector<float> added;  // what the block adds to the dry signal
  size_t bypassAt{0};
  double tailSeconds{0.0};
  float idleResidue{0.0f};  // what is still added once the block is idle
};

BypassRender renderBypass(bool spillover, bool viaRelease = false) {
  EffectRig rig(kKind, {{"timeMs", 200.0}, {"feedback", 60.0}, {"toneHz", 20000.0},
                        {"spillover", spillover ? 1.0 : 0.0}, {"mix", 0.5}});
  EffectRig reference{juce::String()};
  BypassRender result;
  result.tailSeconds = rig.tailSeconds();

  // 0.3 s of noise, the switch, then a second burst a second later that the
  // closed input must keep out of the delay line.
  const std::vector<float> burst = noise(0.3, 5u, 0.25f, kRate);
  std::vector<float> rest(static_cast<size_t>(6.0 * kRate), 0.0f);
  const std::vector<float> late = noise(0.1, 6u, 0.25f, kRate);
  std::copy(late.begin(), late.end(), rest.begin() + ms(1000.0));

  // Whole blocks, so the switch lands exactly between two of them.
  const size_t first = (burst.size() / 256) * 256;
  std::vector<float> in(burst.begin(), burst.begin() + static_cast<std::ptrdiff_t>(first));
  result.bypassAt = in.size();

  std::vector<float> out = rig.render(in);
  std::vector<float> dry = reference.render(in);
  if (viaRelease)
    EXPECT_TRUE(rig.proc.setBlockReleased(rig.blockId(), true));
  else
    rig.setEnabled(false);
  const std::vector<float> outRest = rig.render(rest);
  const std::vector<float> dryRest = reference.render(rest);
  out.insert(out.end(), outRest.begin(), outRest.end());
  dry.insert(dry.end(), dryRest.begin(), dryRest.end());

  result.added = minus(out, dry);
  // Idle once the tail and the fades have passed. Half a second later the
  // rig's DC blocker has forgotten the tail as well (see the harness notes),
  // and the rig is the dry rig.
  const size_t idleFrom =
      result.bypassAt + static_cast<size_t>((result.tailSeconds + 0.1 + 0.5) * kRate);
  EXPECT_LT(idleFrom, out.size());
  result.idleResidue = peakOf(result.added, idleFrom);
  return result;
}

TEST(SimpleDelayTest, TailRingsOutAfterBypassWithSpilloverOn) {
  const BypassRender r = renderBypass(true);

  // The echoes of the burst keep coming after the switch: the one 600 ms
  // later is the burst three delays on, at 0.6^2 of the first echo (at 50%
  // the dry-preserving mix plays dry and wet both at unity).
  const float beforeBypass = peakOf(r.added, 0, r.bypassAt);
  const float ringing = peakOf(r.added, r.bypassAt + static_cast<size_t>(ms(500.0)),
                               r.bypassAt + static_cast<size_t>(ms(700.0)));
  EXPECT_GT(beforeBypass, 0.05f);
  EXPECT_GT(ringing, 0.02f) << "the tail was cut";
  // And they decay like the feedback says, not faster (the wet level is
  // held, not faded): each 200 ms window is about 0.6 of the one before.
  const double early = rmsOf(r.added, r.bypassAt + static_cast<size_t>(ms(300.0)),
                             r.bypassAt + static_cast<size_t>(ms(500.0)));
  const double later = rmsOf(r.added, r.bypassAt + static_cast<size_t>(ms(500.0)),
                             r.bypassAt + static_cast<size_t>(ms(700.0)));
  EXPECT_NEAR(later / early, 0.6, 0.05);

  // The input is closed: the second burst (1.0 to 1.1 s after the switch)
  // passes dry but never echoes. Its first echo would land 200 ms later at
  // the burst's own level (the wet share is 1 at this mix); what is there
  // instead is only the old tail, six echoes on and 25 dB under that.
  const float lateEcho = peakOf(r.added, r.bypassAt + static_cast<size_t>(ms(1200.0)),
                                r.bypassAt + static_cast<size_t>(ms(1300.0)));
  EXPECT_LT(lateEcho, 0.03f) << "audio played after the bypass reached the delay line";

  // Once the tail has passed nothing is added to the dry signal any more.
  EXPECT_LT(r.idleResidue, 1.0e-6f);
  expectCleanSignal(r.added, 1.0f, "spillover render");
}

TEST(SimpleDelayTest, TailIsCutAfterBypassWithSpilloverOff) {
  const BypassRender r = renderBypass(false);
  EXPECT_GT(peakOf(r.added, 0, r.bypassAt), 0.05f);
  // The 25 ms bypass fade, then nothing but the DC blocker settling.
  EXPECT_LT(peakOf(r.added, r.bypassAt + static_cast<size_t>(ms(30.0))), 5.0e-4f);
  EXPECT_LT(peakOf(r.added, r.bypassAt + static_cast<size_t>(ms(400.0))), 1.0e-6f);
  EXPECT_LT(r.idleResidue, 1.0e-6f);

  // The same block with the switch on rings; so the difference is the switch.
  const BypassRender spill = renderBypass(true);
  EXPECT_GT(peakOf(spill.added, spill.bypassAt + static_cast<size_t>(ms(30.0))), 0.02f);
}

TEST(SimpleDelayTest, ReleasingABlockRingsOutLikeAPowerOffAndLeavesTheSwitchAlone) {
  // The scene-change entry point: same tail, but `enabled` is not touched.
  const BypassRender released = renderBypass(true, /*viaRelease=*/true);
  const BypassRender powered = renderBypass(true, /*viaRelease=*/false);
  ASSERT_EQ(released.added.size(), powered.added.size());
  EXPECT_EQ(std::memcmp(released.added.data(), powered.added.data(),
                        released.added.size() * sizeof(float)),
            0);
  EXPECT_LT(released.idleResidue, 1.0e-6f);

  EffectRig rig(kKind);
  EXPECT_FALSE(rig.proc.isBlockReleased(rig.blockId()));
  const juce::var revision = rig.proc.getChainState(-1)["revision"];
  EXPECT_TRUE(rig.proc.setBlockReleased(rig.blockId(), true));
  EXPECT_TRUE(rig.proc.isBlockReleased(rig.blockId()));
  EXPECT_TRUE(static_cast<bool>(rig.blockState()["params"]["enabled"]));
  EXPECT_FALSE(rig.proc.setBlockReleased("no-such-block", true));
  // Back in: the block plays again.
  EXPECT_TRUE(rig.proc.setBlockReleased(rig.blockId(), false));
  rig.set("mix", 1.0);
  rig.renderSilence(4800);
  EXPECT_GT(peakOf(rig.render(impulse(48000))), 0.5f);

  // A kind without the capability is simply bypassed by a release.
  EffectRig gain("utility.gain", {{"gainDb", 12.0}});
  const std::vector<float> in = sine(0.5, 440.0, 0.1f, kRate);
  EXPECT_GT(peakOf(gain.render(in)), 0.3f);
  EXPECT_TRUE(gain.proc.setBlockReleased(gain.blockId(), true));
  gain.render(in);  // the fade
  EXPECT_NEAR(peakOf(gain.render(in)), 0.1f, 0.005f);
}

TEST(SimpleDelayTest, ReEnablingDuringTheTailHasNoDiscontinuity) {
  // A steady low sine through a ringing delay: the block is switched off,
  // back on 150 ms into its tail, off again and on again. Nothing in the
  // output may step by more than the signal itself does.
  EffectRig rig(kKind, {{"timeMs", 130.0}, {"feedback", 70.0}, {"toneHz", 20000.0},
                        {"mix", 0.5}});
  const std::vector<float> in = sine(4.0, 110.0, 0.25f, kRate);
  const auto slice = [&in](double fromSeconds, double toSeconds) {
    return std::vector<float>(in.begin() + static_cast<std::ptrdiff_t>(fromSeconds * kRate),
                              in.begin() + static_cast<std::ptrdiff_t>(toSeconds * kRate));
  };

  std::vector<float> out = rig.render(slice(0.0, 1.5));
  const float steadyStep = maxStep(out, static_cast<size_t>(kRate), out.size());
  const float steadyPeak = peakOf(out, static_cast<size_t>(kRate));
  const auto append = [&out](const std::vector<float>& more) {
    out.insert(out.end(), more.begin(), more.end());
  };

  rig.setEnabled(false);
  append(rig.render(slice(1.5, 1.65)));
  rig.setEnabled(true);  // mid-tail
  append(rig.render(slice(1.65, 2.0)));
  rig.setEnabled(false);
  append(rig.render(slice(2.0, 2.01)));  // mid-fade
  rig.setEnabled(true);
  append(rig.render(slice(2.01, 3.0)));
  // And the scene-style release, toggled the same way.
  rig.proc.setBlockReleased(rig.blockId(), true);
  append(rig.render(slice(3.0, 3.2)));
  rig.proc.setBlockReleased(rig.blockId(), false);
  append(rig.render(slice(3.2, 4.0)));

  expectCleanSignal(out, 1.0f, "re-enable");
  // The sine at 0.25 moves 0.0036 per sample; with the echoes on top the
  // steady rig moves a little more. A splice of the input or of the tail
  // would step by a tenth or more.
  EXPECT_LT(steadyStep, 0.02f);
  EXPECT_LT(maxStep(out, static_cast<size_t>(1.5 * kRate), out.size()), steadyStep * 1.5f + 0.002f)
      << "a switch during the tail stepped the waveform";
  // Dry at unity (0.25) plus a tail that is never louder than the wet share
  // was while the block played.
  EXPECT_LT(peakOf(out, static_cast<size_t>(1.5 * kRate)), 0.25f + steadyPeak);
  // It is processing again at the end: echoes are back on top of the dry.
  EffectRig reference{juce::String()};
  const std::vector<float> dry = reference.render(in);
  EXPECT_GT(rmsOf(minus(out, dry), static_cast<size_t>(3.7 * kRate)), 0.01);
}

TEST(SimpleDelayTest, ATailLongerThanTheCapIsReportedAtTheCapAndFadedThere) {
  // 2 s at 95% takes 136 echoes, 272 s, to fall 60 dB. The chain reports and
  // runs 30 s of it (kMaxBlockTailSeconds).
  const Params longest = {{"timeMs", 2000.0}, {"feedback", 95.0}, {"toneHz", 20000.0}};
  DirectKind alone(kKind, longest);
  EXPECT_NEAR(alone.engine().tailSeconds(), 2.0 * 136.0 + 0.05, 1e-6);
  EXPECT_DOUBLE_EQ(alone.tailSeconds(), kMaxBlockTailSeconds);

  Params settings = longest;
  settings.emplace_back("mix", 0.5);
  EffectRig rig(kKind, settings);
  EXPECT_DOUBLE_EQ(rig.tailSeconds(), kMaxBlockTailSeconds);
  EXPECT_DOUBLE_EQ(rig.proc.getTailLengthSeconds(), kMaxBlockTailSeconds);

  // The conformance tail check accepts a tail at the cap (and only there)
  // without measuring a decay it cannot reach.
  const TailMeasurement tail = expectSilenceAfterReportedTail(kKind, longest);
  EXPECT_TRUE(tail.capped);
  EXPECT_DOUBLE_EQ(tail.reportedSeconds, kMaxBlockTailSeconds);

  // Released, the echoes are still loud when the cap arrives (the 15th is
  // at 0.95^14, about half the first). They are faded out there, not cut.
  // Two seconds of a 220 Hz sine fill the line exactly (440 cycles), so the
  // tail is one continuous tone, a little quieter every 2 s.
  const std::vector<float> burst = makeSine(ms(2000.0), 220.0, 0.25f);
  rig.render(burst);
  rig.setEnabled(false);
  const std::vector<float> out = rig.renderSilence(ms(32000.0));
  const size_t cap = static_cast<size_t>(ms(30000.0));
  // What plays in the two seconds before the cap, and what is left after it.
  const float before = peakOf(out, cap - static_cast<size_t>(ms(2000.0)), cap - 4800);
  EXPECT_GT(before, 0.05f) << "nothing was ringing at the cap";
  EXPECT_LT(peakOf(out, cap + static_cast<size_t>(ms(100.0))), 1.0e-3f)
      << "the tail went on past the cap";
  // 220 Hz at this level moves about 0.003 per sample; the fade adds nothing
  // to that, a cut would step by the echo's level.
  const float ringingStep = maxStep(out, cap - static_cast<size_t>(ms(2000.0)), cap - 4800);
  EXPECT_LT(maxStep(out, cap - 4800, cap + 9600), ringingStep * 1.5f + 0.001f)
      << "the tail was cut at the cap instead of faded";
}

TEST(SimpleDelayTest, SwitchingBackOnAfterTheTailStartsFromSilence) {
  // Spillover off: the tail is cut, and the line must not replay it when
  // the block comes back (BlockProcessor::reset on going idle).
  EffectRig rig(kKind, {{"timeMs", 500.0}, {"feedback", 80.0}, {"spillover", 0.0}, {"mix", 1.0}});
  rig.render(noise(0.6, 9u, 0.25f, kRate));
  rig.setEnabled(false);
  rig.renderSilence(ms(100.0));
  rig.setEnabled(true);
  // Echoes would be near 0.2; what is left is the DC blocker's residue.
  EXPECT_LT(peakOf(rig.renderSilence(ms(2000.0))), 1.0e-3f)
      << "echoes from before the bypass came back";
}

TEST(SimpleDelayTest, RemovingABlockWhileItsTailRingsUnderAudio) {
  // The removal handshake waits for the wet path to be silent; a tail held
  // open by spillover must close for it rather than hold it up.
  EffectRig rig(kKind, {{"timeMs", 300.0}, {"feedback", 90.0}, {"mix", 0.5}});
  std::atomic<bool> stop{false};
  std::atomic<float> worstStep{0.0f};
  std::atomic<int> blocksRun{0};
  std::thread pump([&] {
    const int block = rig.blockSize();
    const int loopFrames = 93 * block;  // 250 Hz loops seamlessly (see block_framework_tests)
    const auto loop = makeSine(loopFrames, 250.0, 0.1f);
    juce::AudioBuffer<float> buffer(2, block);
    juce::MidiBuffer midi;
    int offset = 0;
    float previous = 0.0f, step = 0.0f;
    while (!stop.load()) {
      buffer.copyFrom(0, 0, loop.data() + offset, block);
      buffer.copyFrom(1, 0, loop.data() + offset, block);
      offset = (offset + block) % loopFrames;
      rig.proc.processBlock(buffer, midi);
      const float* y = buffer.getReadPointer(0);
      for (int i = 0; i < block; ++i) {
        step = std::max(step, std::abs(y[i] - previous));
        previous = y[i];
      }
      worstStep.store(step);
      blocksRun.fetch_add(1);
      juce::Thread::sleep(1);
    }
  });
  const auto runBlocks = [&blocksRun](int count) {
    const int target = blocksRun.load() + count;
    while (blocksRun.load() < target)
      juce::Thread::sleep(2);
  };

  runBlocks(200);  // a second of echoes building up
  rig.setEnabled(false);
  runBlocks(40);  // ringing
  const auto before = juce::Time::getMillisecondCounter();
  EXPECT_TRUE(rig.proc.removeChainBlock(rig.blockId()));
  const auto took = juce::Time::getMillisecondCounter() - before;
  runBlocks(40);
  stop.store(true);
  pump.join();

  EXPECT_LT(took, 1500u) << "the removal waited on the ringing tail";
  EXPECT_FALSE(rig.blockState().isObject());
  // 0.1 at 250 Hz moves 0.0033 per sample; the echoes at most double it.
  EXPECT_LT(worstStep.load(), 0.03f) << "the removal spliced the waveform";
}

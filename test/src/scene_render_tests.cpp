// Scenes end to end: a rig built, given two scenes and switched between them
// purely through commands, rendered the way tools/render_tool.cpp
// (PedaleiraRender) renders: multicore off, prepared, built, settled, prepared
// again like a host starting transport, then processed in fixed blocks with
// the reported latency compensated. The scene switch is a command run
// between two blocks, which is all a footswitch press is.
//
// The rig is an amp capture into a delay into a reverb. Scene A is dry: the
// delay's mix at zero and the reverb off. Scene B has both. The same note is
// played once on each side of the switch, and what is left of it half a
// second later tells the scenes apart.
#include "scene_test_helpers.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

using namespace scenetest;

namespace {

constexpr double kRate = 48000.0;
constexpr int kBlock = 128;

// A plucked-string-like note: a few harmonics of 110 Hz, the higher ones
// dying faster, gone after about 0.3 s. Peak -12 dBFS.
std::vector<float> pluck(int frames) {
  std::vector<float> x(static_cast<size_t>(frames), 0.0f);
  double peak = 0.0;
  std::vector<double> raw(x.size());
  for (size_t i = 0; i < raw.size(); ++i) {
    const double t = static_cast<double>(i) / kRate;
    double v = 0.0;
    for (int h = 1; h <= 4; ++h)
      v += std::sin(2.0 * kPi * 110.0 * h * t) / h * std::exp(-t * 9.0 * h);
    // A hard stop well before the windows the test listens in.
    raw[i] = t < 0.3 ? v * std::min(1.0, (0.3 - t) * 50.0) : 0.0;
    peak = std::max(peak, std::abs(raw[i]));
  }
  for (size_t i = 0; i < x.size(); ++i)
    x[i] = static_cast<float>(raw[i] * 0.25 / peak);
  return x;
}

}  // namespace

TEST(SceneRenderTest, APhraseAcrossASceneSwitch) {
  SceneRig rig(kRate, kBlock);

  // The rig, by command.
  const juce::String amp =
      rig.must("chain.addToneFromFiles",
               "{\"paths\":[" + quoted(testFile("a2-amp-test.nam").getFullPathName()) + "]}")
          ["blockId"].toString();
  ASSERT_TRUE(amp.isNotEmpty());
  rig.waitLoaded();
  const juce::String delay = rig.addKind("delay.simple");
  const juce::String reverb = rig.addKind("reverb.hall");
  rig.setParam(delay, "timeMs", 250.0);
  rig.setParam(delay, "feedback", 40.0);

  // Scene A: dry. The delay stays on with its mix at zero (the mix is the
  // scene parameter), the reverb is off.
  rig.assign(delay, "mix");
  rig.setParam(delay, "mix", 0.0);
  rig.setEnabled(reverb, false);
  // Scene B: echoes at unity and the reverb on.
  rig.setSceneValue("B", delay, "mix", 0.5);
  rig.must("scene.rename", R"({"scene":"A","name":"Dry"})");
  rig.must("scene.rename", R"({"scene":"B","name":"Wet"})");
  ASSERT_FALSE(rig.sceneBlock("A", reverb));
  ASSERT_TRUE(rig.sceneBlock("B", reverb));
  ASSERT_EQ(rig.activeScene(), "A");

  // Prepared again with the rig in place, as the renderer does.
  rig.settle();
  ASSERT_EQ(rig.proc.getLatencySamples(), 0) << "the windows below assume an unshifted render";

  // The phrase: the note at 0 s in scene A, the switch at 1 s, the same note
  // again, then nothing while scene B rings, then back to scene A at 2.5 s.
  const int second = static_cast<int>(kRate);
  const int switchToB = second / kBlock * kBlock;        // 1 s, on a block edge
  const int switchToA = (5 * second / 2) / kBlock * kBlock;  // 2.5 s
  const int total = 4 * second / kBlock * kBlock;
  std::vector<float> input(static_cast<size_t>(total), 0.0f);
  const std::vector<float> note = pluck(second / 2);
  std::copy(note.begin(), note.end(), input.begin());
  std::copy(note.begin(), note.end(), input.begin() + switchToB);

  std::vector<float> out(input.size(), 0.0f);
  juce::AudioBuffer<float> buffer(2, kBlock);
  juce::MidiBuffer midi;
  for (int start = 0; start < total; start += kBlock) {
    if (start == switchToB)
      rig.select("B");
    if (start == switchToA)
      rig.select("A");
    EXPECT_FALSE(rig.proc.isChainEditFadeHeld()) << "at sample " << start;
    buffer.copyFrom(0, 0, input.data() + start, kBlock);
    buffer.copyFrom(1, 0, input.data() + start, kBlock);
    rig.proc.processBlock(buffer, midi);
    std::copy(buffer.getReadPointer(0), buffer.getReadPointer(0) + kBlock,
              out.begin() + start);
  }
  ASSERT_TRUE(allFinite(out));
  EXPECT_EQ(rig.activeScene(), "A");

  const auto level = [&](double fromSeconds, double toSeconds) {
    return rmsOf(out, static_cast<size_t>(fromSeconds * kRate),
                 static_cast<size_t>(toSeconds * kRate));
  };
  const auto dbOf = [](double value) { return 20.0 * std::log10(std::max(value, 1e-9)); };

  // Both notes are heard through the amp at the same level: the switch does
  // not duck or boost the attack that follows it.
  const double noteA = level(0.0, 0.25);
  const double noteB = level(1.0, 1.25);
  ASSERT_GT(noteA, 0.005) << "the amp is not passing the note";
  EXPECT_NEAR(dbOf(noteB), dbOf(noteA), 1.5);

  // Half a second after the note stopped: scene A leaves nothing, scene B
  // leaves the echoes and the hall.
  const double tailA = level(0.55, 0.95);
  const double tailB = level(1.55, 1.95);
  std::cout << "[scene render] notes " << dbOf(noteA) << " / " << dbOf(noteB) << " dB, tails "
            << dbOf(tailA) << " / " << dbOf(tailB) << " dB\n";
  EXPECT_LT(dbOf(tailA), dbOf(noteA) - 40.0) << "scene A should be dry";
  EXPECT_GT(dbOf(tailB), dbOf(noteA) - 30.0) << "scene B should ring";
  EXPECT_GT(dbOf(tailB), dbOf(tailA) + 20.0);

  // The echoes come 250 ms apart and at unity in scene B: the first repeat of
  // the second note stands well clear of what scene A had at the same place.
  EXPECT_GT(dbOf(level(1.25, 1.5)), dbOf(level(0.25, 0.5)) + 6.0);

  // No click at the switch back to the dry scene (the one with nothing
  // playing to hide it): the largest step between samples around it is far
  // below what the phrase itself has.
  float stepAroundSwitchBack = 0.0f;
  for (int i = switchToA - 64; i < switchToA + 2048; ++i)
    stepAroundSwitchBack =
        std::max(stepAroundSwitchBack,
                 std::abs(out[static_cast<size_t>(i)] - out[static_cast<size_t>(i - 1)]));
  EXPECT_LE(stepAroundSwitchBack, largestStep(out) * 0.5f);

  // Back in scene A at 2.5 s: the reverb is released, not cut (its tail is
  // still there right after the switch), and it has rung out a second later
  // while nothing new is fed to it.
  const double justAfter = level(2.52, 2.62);
  const double later = level(3.6, 3.95);
  std::cout << "[scene render] after the switch back " << dbOf(justAfter) << " dB, a second later "
            << dbOf(later) << " dB; largest step " << largestStep(out) << ", at the switch back "
            << stepAroundSwitchBack << "\n";
  EXPECT_GT(dbOf(justAfter), dbOf(later) + 10.0) << "the tail did not ring out across the switch";
  EXPECT_LT(dbOf(later), dbOf(tailB) - 20.0);
}

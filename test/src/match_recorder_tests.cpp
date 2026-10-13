// The recording half of PedaleiraMatch (tools/MatchRecorder.cpp and
// tools/MatchTake.cpp). No sound card is needed: the recorder is an audio
// callback, so a fake device drives it, and the fake loops the outputs back to
// the inputs with a fixed delay. The analysis of the effect-matching kit then
// has to find the sync markers and measure exactly that delay, which proves the
// signal and the recording are sample aligned whatever the block size.

#include "EffectMatch.h"
#include "MatchRecorder.h"
#include "MatchTake.h"

#include <gtest/gtest.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

constexpr double kRate = EffectMatch::kSignalRate;
constexpr int kTail = 96000;  // 2 s

const EffectMatch::TestSignal& signalUnderTest() {
  static const EffectMatch::TestSignal signal = EffectMatch::makeTestSignal(kRate);
  return signal;
}

// What the fake device saw the recorder write to its outputs.
struct Seen {
  std::vector<std::vector<float>> outputs;
};

struct Fake {
  int numOut = 2;
  int numIn = 2;
  int block = 37;
  int delay = 300;
  // Per device input: which device output it hears, and how loud.
  std::vector<int> source = {0, 0};
  std::vector<float> gain = {1.0f, 1.0f};
};

// Runs the take to its end the way a device would: every block hands the
// callback inputs that were produced `delay` samples after the outputs of
// the same moment (so delay has to be at least one block: a device cannot
// capture what it has not played yet). Outputs start as garbage to prove the
// recorder overwrites them.
Seen drive(MatchRecorder& recorder, const Fake& fake) {
  EXPECT_GE(fake.delay, fake.block) << "a loopback cannot be faster than its block";
  Seen seen;
  seen.outputs.assign(static_cast<size_t>(fake.numOut), {});
  juce::AudioBuffer<float> out(fake.numOut, fake.block);
  juce::AudioBuffer<float> in(fake.numIn, fake.block);
  juce::AudioIODeviceCallbackContext context;

  recorder.start();
  long position = 0;
  const long limit = recorder.totalSamples() + 4L * fake.block + fake.delay;
  while (!recorder.finished() && position < limit) {
    for (int c = 0; c < fake.numIn; ++c) {
      for (int n = 0; n < fake.block; ++n) {
        const long t = position + n - fake.delay;
        const auto& heard = seen.outputs[static_cast<size_t>(fake.source[static_cast<size_t>(c)])];
        in.setSample(c, n, t >= 0 ? fake.gain[static_cast<size_t>(c)] * heard[static_cast<size_t>(t)] : 0.0f);
      }
    }
    for (int c = 0; c < fake.numOut; ++c)
      for (int n = 0; n < fake.block; ++n)
        out.setSample(c, n, 0.5f);
    recorder.audioDeviceIOCallbackWithContext(in.getArrayOfReadPointers(), fake.numIn,
                                              out.getArrayOfWritePointers(), fake.numOut, fake.block,
                                              context);
    for (int c = 0; c < fake.numOut; ++c)
      seen.outputs[static_cast<size_t>(c)].insert(seen.outputs[static_cast<size_t>(c)].end(),
                                                  out.getReadPointer(c),
                                                  out.getReadPointer(c) + fake.block);
    position += fake.block;
  }
  return seen;
}

struct Case {
  int block;
  int delay;
  bool stereo;
};

class LoopbackTest : public ::testing::TestWithParam<Case> {};

TEST_P(LoopbackTest, AnalysisMeasuresTheDelayExactly) {
  const Case c = GetParam();
  Fake fake;
  fake.block = c.block;
  fake.delay = c.delay;
  fake.numOut = 4;
  fake.numIn = 3;
  // The signal goes out on device outputs 1 and 3 and comes back on inputs 0
  // and 2 (input 1 listens to a silent output).
  fake.source = {1, 0, 3};
  fake.gain = {0.5f, 1.0f, 0.5f};

  MatchRecorder recorder;
  const std::vector<int> inputs = c.stereo ? std::vector<int>{0, 2} : std::vector<int>{2};
  recorder.prepare(signalUnderTest().samples, {1, 3}, inputs, kTail);
  const Seen seen = drive(recorder, fake);

  ASSERT_TRUE(recorder.finished());
  EXPECT_EQ(recorder.waitUntilFinished(10), MatchRecorder::Wait::finished);
  EXPECT_EQ(recorder.totalSamples(), static_cast<int>(signalUnderTest().samples.size()) + kTail);
  EXPECT_DOUBLE_EQ(recorder.progress(), 1.0);

  const juce::AudioBuffer<float>& recording = recorder.recording();
  ASSERT_EQ(recording.getNumChannels(), c.stereo ? 2 : 1);
  ASSERT_EQ(recording.getNumSamples(), recorder.totalSamples());

  const EffectMatch::MatchAnalysis analysis =
      EffectMatch::analyseRecording(recording, kRate, signalUnderTest());
  EXPECT_NEAR(analysis.alignment.latencyMs * kRate / 1000.0, static_cast<double>(c.delay), 0.01);
  EXPECT_GT(analysis.alignment.startScore, 0.9);
  EXPECT_GT(analysis.alignment.endScore, 0.9);
  EXPECT_EQ(analysis.channels, c.stereo ? 2 : 1);

  // The take checks pass: markers, no clipping, latency, plausible level.
  const MatchTake::Check check = MatchTake::checkTake(recording, kRate, signalUnderTest());
  EXPECT_TRUE(check.passed) << (check.problems.empty() ? "" : check.problems.front());
  EXPECT_NEAR(check.latencyMs * kRate / 1000.0, static_cast<double>(c.delay), 0.01);

  // Outputs 0 and 2 stay silent; 1 and 3 carry the signal and then silence.
  const auto& samples = signalUnderTest().samples;
  for (const int silent : {0, 2})
    EXPECT_TRUE(std::all_of(seen.outputs[static_cast<size_t>(silent)].begin(),
                            seen.outputs[static_cast<size_t>(silent)].end(),
                            [](float v) { return v == 0.0f; }));
  for (const int played : {1, 3}) {
    const auto& o = seen.outputs[static_cast<size_t>(played)];
    ASSERT_GE(o.size(), samples.size());
    EXPECT_TRUE(std::equal(samples.begin(), samples.end(), o.begin()));
    EXPECT_TRUE(std::all_of(o.begin() + static_cast<long>(samples.size()), o.end(),
                            [](float v) { return v == 0.0f; }));
  }
}

INSTANTIATE_TEST_SUITE_P(BlockSizes, LoopbackTest,
                         ::testing::Values(Case{37, 300, false}, Case{512, 700, true},
                                           Case{37, 411, true}, Case{512, 512, false}),
                         [](const ::testing::TestParamInfo<Case>& info) {
                           return "Block" + std::to_string(info.param.block) + "Delay" +
                                  std::to_string(info.param.delay) +
                                  (info.param.stereo ? "Stereo" : "Mono");
                         });

// The recording is sample for sample the delayed signal, on the channel asked
// for (so playback and recording share one position).
TEST(MatchRecorderTest, RecordingIsTheDelayedSignalSampleForSample) {
  Fake fake;
  fake.numOut = 2;
  fake.numIn = 2;
  fake.block = 64;
  fake.delay = 250;
  fake.source = {1, 1};
  fake.gain = {1.0f, 0.25f};

  MatchRecorder recorder;
  recorder.prepare(signalUnderTest().samples, {1}, {1, 0}, kTail);
  drive(recorder, fake);
  ASSERT_TRUE(recorder.finished());

  const auto& samples = signalUnderTest().samples;
  const auto& recording = recorder.recording();
  for (int n = 0; n < recorder.totalSamples(); ++n) {
    const long t = n - fake.delay;
    const float expected = t >= 0 && t < static_cast<long>(samples.size()) ? samples[static_cast<size_t>(t)] : 0.0f;
    ASSERT_EQ(recording.getSample(0, n), 0.25f * expected) << "at " << n;
    ASSERT_EQ(recording.getSample(1, n), expected) << "at " << n;
  }
}

TEST(MatchRecorderTest, StaysSilentAndKeepsItsTakeAfterTheEnd) {
  MatchRecorder recorder;
  recorder.prepare({0.5f, 0.5f, 0.5f, 0.5f, 0.5f}, {0}, {0}, 3);
  EXPECT_EQ(recorder.waitUntilFinished(20), MatchRecorder::Wait::timeout);

  // Not started yet: the outputs are silent and nothing is recorded.
  juce::AudioBuffer<float> in(1, 16), out(1, 16);
  in.clear(0, 0, 16);
  juce::AudioIODeviceCallbackContext context;
  for (int n = 0; n < 16; ++n)
    in.setSample(0, n, 0.25f), out.setSample(0, n, 1.0f);
  recorder.audioDeviceIOCallbackWithContext(in.getArrayOfReadPointers(), 1, out.getArrayOfWritePointers(),
                                            1, 16, context);
  EXPECT_FALSE(recorder.finished());
  EXPECT_EQ(out.getMagnitude(0, 16), 0.0f);

  // One block longer than the take: it ends inside the block.
  recorder.start();
  recorder.audioDeviceIOCallbackWithContext(in.getArrayOfReadPointers(), 1, out.getArrayOfWritePointers(),
                                            1, 16, context);
  ASSERT_TRUE(recorder.finished());
  EXPECT_EQ(recorder.waitUntilFinished(1000), MatchRecorder::Wait::finished);
  EXPECT_EQ(recorder.totalSamples(), 8);
  for (int n = 0; n < 16; ++n)
    EXPECT_EQ(out.getSample(0, n), n < 5 ? 0.5f : 0.0f);
  for (int n = 0; n < 8; ++n)
    EXPECT_EQ(recorder.recording().getSample(0, n), 0.25f);

  // Later blocks change nothing.
  for (int n = 0; n < 16; ++n)
    in.setSample(0, n, 0.75f), out.setSample(0, n, 1.0f);
  recorder.audioDeviceIOCallbackWithContext(in.getArrayOfReadPointers(), 1, out.getArrayOfWritePointers(),
                                            1, 16, context);
  EXPECT_EQ(out.getMagnitude(0, 16), 0.0f);
  for (int n = 0; n < 8; ++n)
    EXPECT_EQ(recorder.recording().getSample(0, n), 0.25f);
}

TEST(MatchRecorderTest, CopesWithChannelsTheDeviceDidNotDeliver) {
  MatchRecorder recorder;
  recorder.prepare({1.0f, 1.0f, 1.0f, 1.0f}, {0, 5}, {0, 3}, 0);
  juce::AudioBuffer<float> in(1, 8), out(2, 8);
  for (int n = 0; n < 8; ++n)
    in.setSample(0, n, 0.5f);
  juce::AudioIODeviceCallbackContext context;
  recorder.start();
  recorder.audioDeviceIOCallbackWithContext(in.getArrayOfReadPointers(), 1, out.getArrayOfWritePointers(),
                                            2, 8, context);
  ASSERT_TRUE(recorder.finished());
  EXPECT_EQ(recorder.recording().getSample(0, 3), 0.5f);
  EXPECT_EQ(recorder.recording().getSample(1, 3), 0.0f);  // input 3 does not exist: silence
  EXPECT_EQ(out.getSample(0, 0), 1.0f);
  EXPECT_EQ(out.getSample(1, 0), 0.0f);  // output 5 does not exist: ignored
}

TEST(MatchRecorderTest, ReportsAStoppedDevice) {
  MatchRecorder recorder;
  recorder.prepare({0.0f}, {0}, {0}, 1000);
  recorder.start();
  recorder.audioDeviceStopped();
  EXPECT_EQ(recorder.waitUntilFinished(1000), MatchRecorder::Wait::deviceStopped);
  recorder.audioDeviceAboutToStart(nullptr);
  EXPECT_EQ(recorder.waitUntilFinished(20), MatchRecorder::Wait::timeout);
}

TEST(MatchRecorderTest, ChannelIndicesCountOnlyTheActiveChannels) {
  juce::BigInteger mask;
  mask.setBit(2);
  mask.setBit(5);
  mask.setBit(6);
  EXPECT_EQ(MatchRecorder::activeChannelIndex(mask, 2), 0);
  EXPECT_EQ(MatchRecorder::activeChannelIndex(mask, 5), 1);
  EXPECT_EQ(MatchRecorder::activeChannelIndex(mask, 6), 2);
  EXPECT_EQ(MatchRecorder::activeChannelIndex(mask, 3), -1);
  EXPECT_EQ(MatchRecorder::activeChannelIndex(mask, -1), -1);
}

// ####################
// THE CHECK
// ####################
juce::AudioBuffer<float> delayedSignal(int delay, float gain) {
  const auto& samples = signalUnderTest().samples;
  juce::AudioBuffer<float> buffer(1, static_cast<int>(samples.size()) + kTail);
  buffer.clear();
  for (size_t n = 0; n < samples.size(); ++n)
    buffer.setSample(0, static_cast<int>(n) + delay, gain * samples[n]);
  return buffer;
}

TEST(MatchTakeTest, CheckAcceptsACleanTake) {
  const auto check = MatchTake::checkTake(delayedSignal(480, 0.5f), kRate, signalUnderTest());
  EXPECT_TRUE(check.passed);
  EXPECT_TRUE(check.markersFound);
  EXPECT_FALSE(check.clipped);
  EXPECT_NEAR(check.latencyMs, 10.0, 0.001);
  EXPECT_NEAR(check.levelGainDb, -6.0, 0.5);
}

TEST(MatchTakeTest, CheckAcceptsAUnityPathThatTouchesFullScale) {
  // The staircase's top step is 0 dBFS: a clean unity path reaches full scale
  // for a sample at each peak, which is not clipping (a real unit's bypass).
  const auto check = MatchTake::checkTake(delayedSignal(480, 1.0f), kRate, signalUnderTest());
  EXPECT_FALSE(check.clipped);
  EXPECT_TRUE(check.passed);
}

TEST(MatchTakeTest, CheckRefusesAClippedTake) {
  // 3 dB too hot into a converter that stops at full scale: flat tops.
  juce::AudioBuffer<float> hot = delayedSignal(480, 1.41f);
  for (int n = 0; n < hot.getNumSamples(); ++n)
    hot.setSample(0, n, juce::jlimit(-1.0f, 1.0f, hot.getSample(0, n)));
  const auto check = MatchTake::checkTake(hot, kRate, signalUnderTest());
  EXPECT_TRUE(check.clipped);
  EXPECT_FALSE(check.passed);
}

TEST(MatchTakeTest, CheckRefusesSamplesPastFullScale) {
  // A float path with no converter limit: past full scale is clipping too.
  const auto check = MatchTake::checkTake(delayedSignal(480, 1.2f), kRate, signalUnderTest());
  EXPECT_TRUE(check.clipped);
  EXPECT_FALSE(check.passed);
}

TEST(MatchTakeTest, CheckRefusesSilenceAndMissingMarkers) {
  juce::AudioBuffer<float> silence(1, static_cast<int>(signalUnderTest().samples.size()) + kTail);
  silence.clear();
  const auto check = MatchTake::checkTake(silence, kRate, signalUnderTest());
  EXPECT_FALSE(check.passed);
  EXPECT_FALSE(check.markersFound);
  ASSERT_FALSE(check.problems.empty());
  EXPECT_NE(check.problems.front().find("sync markers"), std::string::npos);
}

TEST(MatchTakeTest, AttenuatedSignalIsScaledAndRefusesGainAboveZero) {
  const auto& samples = signalUnderTest().samples;
  const std::vector<float> same = MatchTake::attenuatedSamples(samples, 0.0);
  EXPECT_EQ(same, samples);
  const std::vector<float> quieter = MatchTake::attenuatedSamples(samples, -12.0);
  ASSERT_EQ(quieter.size(), samples.size());
  float peak = 0.0f, peakQuiet = 0.0f;
  for (size_t n = 0; n < samples.size(); ++n) {
    peak = std::max(peak, std::abs(samples[n]));
    peakQuiet = std::max(peakQuiet, std::abs(quieter[n]));
  }
  EXPECT_NEAR(20.0 * std::log10(peakQuiet / peak), -12.0, 0.001);
  EXPECT_THROW(MatchTake::attenuatedSamples(samples, 1.0), std::invalid_argument);
  EXPECT_THROW(MatchTake::attenuatedSamples(samples, std::nan("")), std::invalid_argument);
}

TEST(MatchTakeTest, CheckReportsTheLevelAgainstWhatWasPlayed) {
  // Played 12 dB down through a unity path: the level is 0 dB against what
  // was played, -12 dB against nominal, and the take is not clipped.
  const float gain = std::pow(10.0f, -12.0f / 20.0f);
  const auto nominal = MatchTake::checkTake(delayedSignal(480, gain), kRate, signalUnderTest());
  EXPECT_NEAR(nominal.levelGainDb, -12.0, 0.5);
  const auto played = MatchTake::checkTake(delayedSignal(480, gain), kRate, signalUnderTest(), -12.0);
  EXPECT_NEAR(played.levelGainDb, 0.0, 0.5);
  EXPECT_FALSE(played.clipped);
  EXPECT_TRUE(played.passed);
}

TEST(MatchTakeTest, ClipCheckStillFiresWhenThePlayedSignalWasAttenuated) {
  // The unit adds 15 dB to a signal played 6 dB down: still a clipped take.
  juce::AudioBuffer<float> hot = delayedSignal(480, std::pow(10.0f, 9.0f / 20.0f));
  for (int n = 0; n < hot.getNumSamples(); ++n)
    hot.setSample(0, n, juce::jlimit(-1.0f, 1.0f, hot.getSample(0, n)));
  const auto check = MatchTake::checkTake(hot, kRate, signalUnderTest(), -6.0);
  EXPECT_TRUE(check.clipped);
  EXPECT_FALSE(check.passed);
}

TEST(MatchTakeTest, CheckRefusesAnImplausibleLevel) {
  const auto check = MatchTake::checkTake(delayedSignal(480, 0.001f), kRate, signalUnderTest());  // -60 dB
  EXPECT_FALSE(check.passed);
  EXPECT_TRUE(check.markersFound);
}

TEST(MatchTakeTest, AQuietSignalThroughAHighGainPathIsPlausible) {
  // Played 36 dB down and made up by a 32 dB gain (a driven amp): 32 dB against what was
  // played but -4 dB against nominal, so the level check accepts it.
  const auto check = MatchTake::checkTake(delayedSignal(480, std::pow(10.0f, -4.0f / 20.0f)), kRate,
                                          signalUnderTest(), -36.0);
  EXPECT_NEAR(check.levelGainDb, 32.0, 0.5);
  EXPECT_TRUE(check.passed);
}

TEST(MatchTakeTest, WavIsFloat48kAndTheRightWidth) {
  juce::TemporaryFile temp(".wav");
  juce::AudioBuffer<float> stereo(2, 1000);
  for (int n = 0; n < 1000; ++n) {
    stereo.setSample(0, n, 0.001f * static_cast<float>(n));
    stereo.setSample(1, n, -0.0005f * static_cast<float>(n));
  }
  MatchTake::writeTakeWav(temp.getFile(), stereo, kRate);

  juce::WavAudioFormat wav;
  std::unique_ptr<juce::AudioFormatReader> reader(wav.createReaderFor(temp.getFile().createInputStream().release(), true));
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(reader->sampleRate, kRate);
  EXPECT_EQ(reader->numChannels, 2u);
  EXPECT_EQ(reader->bitsPerSample, 32u);
  EXPECT_TRUE(reader->usesFloatingPointData);
  EXPECT_EQ(reader->lengthInSamples, 1000);
  juce::AudioBuffer<float> back(2, 1000);
  reader->read(&back, 0, 1000, 0, true, true);
  EXPECT_EQ(back.getSample(0, 999), stereo.getSample(0, 999));
  EXPECT_EQ(back.getSample(1, 500), stereo.getSample(1, 500));
}

// ####################
// THE PLAN FILE AND THE COMMAND LINE
// ####################
TEST(MatchPlanTest, ParsesTakesAndSkipsCommentsAndBlankLines) {
  const auto takes = MatchTake::parsePlan(
      "# a plan\n"
      "\n"
      "bypass | Bypass every block\r\n"
      "  delay-250ms-fb35  |  Delay: time 250 ms, feedback 35% | mix 100%  \n"
      "   # indented comment\n"
      "reverb-hall-usb | Hall reverb, decay 3 s");
  ASSERT_EQ(takes.size(), 3u);
  EXPECT_EQ(takes[0].stem, "bypass");
  EXPECT_EQ(takes[0].instruction, "Bypass every block");
  EXPECT_EQ(takes[1].stem, "delay-250ms-fb35");
  EXPECT_EQ(takes[1].instruction, "Delay: time 250 ms, feedback 35% | mix 100%");
  EXPECT_EQ(takes[2].stem, "reverb-hall-usb");
}

TEST(MatchPlanTest, RejectsBadLinesWithTheLineNumber) {
  auto message = [](const juce::String& plan) {
    try {
      MatchTake::parsePlan(plan);
    } catch (const MatchTake::PlanError& e) {
      return std::string(e.what());
    }
    return std::string();
  };
  EXPECT_NE(message("ok | fine\nno separator here").find("plan line 2"), std::string::npos);
  EXPECT_NE(message("# c\n | no stem").find("plan line 2"), std::string::npos);
  EXPECT_NE(message("../escape | x").find("not a file stem"), std::string::npos);
  EXPECT_NE(message("sub/dir | x").find("not a file stem"), std::string::npos);
  EXPECT_NE(message(".hidden | x").find("not a file stem"), std::string::npos);
  EXPECT_NE(message("a b | x").find("not a file stem"), std::string::npos);
  EXPECT_NE(message("empty |   ").find("instruction"), std::string::npos);
  EXPECT_NE(message("a | one\nb | two\na | again").find("plan line 3"), std::string::npos);
  EXPECT_TRUE(MatchTake::parsePlan("# nothing\n\n").empty());
}

TEST(MatchPlanTest, TheExamplePlanParses) {
  const juce::File example = juce::File(T3K_TEST_FILES_DIR).getParentDirectory().getParentDirectory().getChildFile(
      "docs/match-plan-example.txt");
  ASSERT_TRUE(example.existsAsFile()) << example.getFullPathName();
  const auto takes = MatchTake::parsePlan(example.loadFileAsString());
  EXPECT_GE(takes.size(), 8u);
  EXPECT_EQ(takes.front().stem, "bypass");
}

TEST(MatchChannelsTest, ParsesOneBasedLists) {
  EXPECT_EQ(MatchTake::parseChannelList("3", 2, "--in-ch"), (std::vector<int>{3}));
  EXPECT_EQ(MatchTake::parseChannelList("1, 12", 2, "--in-ch"), (std::vector<int>{1, 12}));
  EXPECT_THROW(MatchTake::parseChannelList("0", 2, "--in-ch"), std::invalid_argument);
  EXPECT_THROW(MatchTake::parseChannelList("1,2,3", 2, "--in-ch"), std::invalid_argument);
  EXPECT_THROW(MatchTake::parseChannelList("1,1", 2, "--in-ch"), std::invalid_argument);
  EXPECT_THROW(MatchTake::parseChannelList("a", 2, "--in-ch"), std::invalid_argument);
  EXPECT_THROW(MatchTake::parseChannelList("", 2, "--in-ch"), std::invalid_argument);
  EXPECT_THROW(MatchTake::parseChannelList("1,", 2, "--in-ch"), std::invalid_argument);

  const juce::BigInteger mask = MatchTake::channelMask({1, 4});
  EXPECT_TRUE(mask[0]);
  EXPECT_FALSE(mask[1]);
  EXPECT_TRUE(mask[3]);
  EXPECT_EQ(mask.countNumberOfSetBits(), 2);
}

}  // namespace

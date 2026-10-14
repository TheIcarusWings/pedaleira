// The capture wizard's engine side, driven without hardware: a simulated
// device feeds the processor's callback with the signal it played, passed
// through a known chain (a gain, a soft clipper, a fixed delay, added noise).
// The tests check the metering, that a recording lines up sample for sample
// with what was played and that the latency check recovers the delay, that
// the data checks fire on bad recordings, the state machine, persistence, the
// trainer runner against a stand-in trainer script, and the permissions.
// Real training is never run.

#include "CaptureAudio.h"
#include "CaptureChecks.h"
#include "CaptureSession.h"
#include "CaptureStudio.h"
#include "TrainerRunner.h"
#include "agent_test_helpers.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <random>
#include <thread>
#include <vector>

namespace {

constexpr int kDelay = 200;       // samples between a played sample and its return
constexpr float kGain = 2.5f;     // into the soft clipper
constexpr float kNoise = 1.0e-4f; // uniform noise, about -85 dBFS RMS

juce::String idOf(const juce::var& session) { return session["id"].toString(); }

juce::String codeOfResult(const CommandResult& result) {
  return result.ok ? juce::String("ok") : result.errorCode;
}

double levelDb(double linear) { return 20.0 * std::log10(std::max(linear, 1.0e-9)); }

// A stand-in for the NAM standard input file: bursts of low-passed noise with
// silences between them, 48 kHz.
std::vector<float> makeStandardInput(double seconds) {
  const int n = static_cast<int>(seconds * 48000.0);
  std::vector<float> x(static_cast<size_t>(n), 0.0f);
  std::mt19937 gen(1234);
  std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
  float state = 0.0f;
  for (int i = 0; i < n; ++i) {
    const double t = i / 48000.0;
    // Two seconds of signal, one second of silence.
    const bool silent = std::fmod(t, 3.0) >= 2.0;
    state = 0.6f * state + 0.4f * noise(gen);
    x[static_cast<size_t>(i)] = silent ? 0.0f : state * 1.4f;
  }
  float peak = 0.0f;
  for (const float v : x)
    peak = std::max(peak, std::abs(v));
  for (float& v : x)
    v *= 0.9f / peak;
  return x;
}

// The device: runs blocks through the processor, feeding it what comes back
// from `respond` and keeping everything the processor played.
struct Device {
  explicit Device(AgentRig& r) : rig(r) {}

  AgentRig& rig;
  std::vector<float> out0, out1;
  size_t cursor = 0;
  std::mt19937 gen{99};

  void reset() {
    out0.clear();
    out1.clear();
    cursor = 0;
  }

  float noise() {
    return std::uniform_real_distribution<float>(-kNoise, kNoise)(gen);
  }

  // The return of a loopback: the played channel 0, delayed, gained,
  // clipped, with noise.
  float loopback(size_t t, float gain = kGain, int delay = kDelay) {
    const float played = t >= static_cast<size_t>(delay) && t - delay < out0.size()
                             ? out0[t - static_cast<size_t>(delay)]
                             : 0.0f;
    return std::tanh(gain * played) + noise();
  }

  void pump(int blocks, const std::function<std::pair<float, float>(size_t, Device&)>& input) {
    juce::AudioBuffer<float> buffer(2, rig.block);
    juce::MidiBuffer midi;
    for (int b = 0; b < blocks; ++b) {
      for (int i = 0; i < rig.block; ++i) {
        const auto [l, r] = input(cursor + static_cast<size_t>(i), *this);
        buffer.setSample(0, i, l);
        buffer.setSample(1, i, r);
      }
      rig.proc.processBlock(buffer, midi);
      for (int i = 0; i < rig.block; ++i) {
        out0.push_back(buffer.getSample(0, i));
        out1.push_back(buffer.getSample(1, i));
      }
      cursor += static_cast<size_t>(rig.block);
    }
  }

  void pumpLoopback(int blocks, float gain = kGain, int delay = kDelay) {
    pump(blocks, [gain, delay](size_t t, Device& d) {
      return std::make_pair(d.loopback(t, gain, delay), 0.0f);
    });
  }
};

juce::String args(const juce::String& id, const juce::String& rest = {}) {
  return "{\"id\":" + agentQuoted(id) + (rest.isEmpty() ? "" : "," + rest) + "}";
}

// A re-amp capture taken to the levels step, channels 0 and 0.
juce::String reampAtLevels(AgentRig& rig, const char* name = "Brit 800 Crunch") {
  const juce::var created = rig.mustLocal(
      "capture.create", "{\"name\":" + agentQuoted(name) +
                            ",\"source\":\"ampHead\",\"method\":\"reamp\",\"gearMake\":\"Marshall\"}");
  const juce::String id = idOf(created);
  rig.mustLocal("capture.setStep", args(id, "\"step\":\"wiring\""));
  rig.mustLocal("capture.setChannels", args(id, "\"send\":0,\"returnIn\":0"));
  rig.mustLocal("capture.setStep", args(id, "\"step\":\"levels\""));
  rig.mustLocal("capture.setLevels", args(id, "\"sendDb\":-20"));
  return id;
}

// Plays the tone through the simulated amp until the readings settle, and
// returns the last levelStatus.
juce::var settleLevels(AgentRig& rig, Device& dev, const juce::String& id, int blocks = 300) {
  dev.reset();
  rig.mustLocal("capture.startTone", args(id));
  dev.pumpLoopback(blocks);
  return rig.mustLocal("capture.levelStatus", args(id));
}

std::vector<float> readWav(const juce::File& file) {
  juce::AudioBuffer<float> audio;
  double rate = 0.0;
  juce::String error;
  EXPECT_TRUE(AudioFiles::read(file, audio, rate, error, 1000.0)) << error;
  EXPECT_NEAR(rate, 48000.0, 0.5);
  if (audio.getNumChannels() < 1)
    return {};
  return std::vector<float>(audio.getReadPointer(0), audio.getReadPointer(0) + audio.getNumSamples());
}

// Pumps recordings until the session says the recording is complete.
juce::var finishRecording(AgentRig& rig, Device& dev, const juce::String& id,
                          const std::function<std::pair<float, float>(size_t, Device&)>& input,
                          int maxBlocks = 12000) {
  juce::var status;
  for (int blocks = 0; blocks < maxBlocks;) {
    dev.pump(256, input);
    blocks += 256;
    status = rig.mustLocal("capture.recordingStatus", args(id));
    if (status["state"].toString() != "recording")
      break;
  }
  // The files are written by a thread of their own.
  for (int i = 0; i < 400 && status["state"].toString() == "writing"; ++i) {
    juce::Thread::sleep(10);
    status = rig.mustLocal("capture.recordingStatus", args(id));
  }
  return status;
}

}  // namespace

// ####################
// TEST TONE AND METERING
// ####################
TEST(CaptureLevels, ToneIsMeteredAndRangeDetected) {
  AgentRig rig;
  Device dev(rig);
  const juce::String id = reampAtLevels(rig);

  const juce::var status = settleLevels(rig, dev, id);
  EXPECT_TRUE(static_cast<bool>(status["active"]));
  EXPECT_TRUE(static_cast<bool>(status["tone"]));
  // A 1 kHz tone at -20 dBFS peak through tanh(2.5 x): -12.2 dBFS.
  const double hold = status["returnLevel"]["holdDb"];
  EXPECT_NEAR(hold, levelDb(std::tanh(kGain * 0.1)), 0.3);
  // The RMS of a (nearly) sine wave sits 3 dB under its peak.
  EXPECT_NEAR(static_cast<double>(status["returnLevel"]["rmsDb"]), hold - 3.0, 0.8);
  EXPECT_EQ(static_cast<int>(status["returnLevel"]["clippedSamples"]), 0);
  EXPECT_EQ(status["state"].toString(), "inRange");
  EXPECT_TRUE(static_cast<bool>(status["inRange"]));

  // The tone really leaves on the send channel only, at the send level.
  float peak0 = 0.0f, peak1 = 0.0f;
  for (size_t i = dev.out0.size() / 2; i < dev.out0.size(); ++i) {
    peak0 = std::max(peak0, std::abs(dev.out0[i]));
    peak1 = std::max(peak1, std::abs(dev.out1[i]));
  }
  EXPECT_NEAR(levelDb(peak0), -20.0, 0.2);
  EXPECT_EQ(peak1, 0.0f);

  // In range lets the wizard on, and leaving the step ends the run.
  const juce::var moved = rig.mustLocal("capture.setStep", args(id, "\"step\":\"record\""));
  EXPECT_EQ(moved["step"].toString(), "record");
  EXPECT_FALSE(static_cast<bool>(moved["audioActive"]));
  EXPECT_EQ(rig.proc.captureAudio.getMode(), CaptureAudio::Mode::off);
}

TEST(CaptureLevels, LowHighAndClippingBlockTheStep) {
  AgentRig rig;
  Device dev(rig);
  const juce::String id = reampAtLevels(rig);

  // Too quiet: a send level of -40 dB gives -32 dB at the return.
  rig.mustLocal("capture.setLevels", args(id, "\"sendDb\":-40"));
  juce::var status = settleLevels(rig, dev, id);
  EXPECT_EQ(status["state"].toString(), "low");
  EXPECT_FALSE(static_cast<bool>(status["inRange"]));
  CommandResult refused = rig.local("capture.setStep", args(id, "\"step\":\"record\""));
  EXPECT_FALSE(refused.ok);
  EXPECT_TRUE(refused.errorMessage.contains("outside the target band")) << refused.errorMessage;

  // Changing the send level while the run goes on takes effect at once, and
  // the old peak hold is cleared with it.
  rig.mustLocal("capture.setLevels", args(id, "\"sendDb\":-20"));
  dev.pumpLoopback(300);
  status = rig.mustLocal("capture.levelStatus", args(id));
  EXPECT_EQ(status["state"].toString(), "inRange");

  // Too loud: a hot return clips.
  rig.mustLocal("capture.levelStatus", args(id, "\"resetHold\":true"));
  dev.pumpLoopback(300, /*gain=*/40.0f);
  status = rig.mustLocal("capture.levelStatus", args(id));
  EXPECT_EQ(status["state"].toString(), "clipping");
  EXPECT_GT(static_cast<int>(status["returnLevel"]["clippedSamples"]), 0);
  EXPECT_FALSE(static_cast<bool>(status["inRange"]));
}

TEST(CaptureLevels, NoiseFloorIsMeasuredWithTheToneOff) {
  AgentRig rig;
  Device dev(rig);
  const juce::String id = reampAtLevels(rig);
  settleLevels(rig, dev, id, 100);

  rig.mustLocal("capture.measureNoiseFloor", args(id, "\"seconds\":0.5"));
  // 0.3 s settling plus 0.5 s: 0.8 s is 300 blocks; the noise is uniform
  // +-1e-4, RMS 5.8e-5, -84.8 dBFS (the tone is held off during it).
  dev.pumpLoopback(400);
  const juce::var status = rig.mustLocal("capture.levelStatus", args(id));
  EXPECT_EQ(status["noiseFloor"]["state"].toString(), "done");
  EXPECT_NEAR(static_cast<double>(status["noiseFloor"]["rmsDb"]), levelDb(kNoise / std::sqrt(3.0)), 1.5);
}

TEST(CaptureLevels, DryWetMetersBothInputsAndPlaysNothing) {
  AgentRig rig;
  Device dev(rig);
  const juce::var created = rig.mustLocal(
      "capture.create", R"({"name":"Dry wet test","source":"ampHead","method":"dryWet"})");
  const juce::String id = idOf(created);
  rig.mustLocal("capture.setStep", args(id, "\"step\":\"wiring\""));
  rig.mustLocal("capture.setChannels", args(id, "\"dry\":0,\"wet\":1"));
  rig.mustLocal("capture.setStep", args(id, "\"step\":\"levels\""));
  rig.mustLocal("capture.startTone", args(id));

  // Scaled so that the stretch that is played has its peak at -12 dBFS.
  const std::vector<float> guitar = pluckedClip(4.0, 0.25f, 0.5);
  float guitarPeak = 0.0f;
  for (size_t i = 0; i < 600u * 128u; ++i)
    guitarPeak = std::max(guitarPeak, std::abs(guitar[i]));
  dev.pump(600, [&](size_t t, Device&) {
    const float dry = 0.25f / guitarPeak * guitar[t % guitar.size()];
    return std::make_pair(dry, 0.25f * std::tanh(3.0f * dry / 0.25f) / std::tanh(3.0f));
  });
  const juce::var status = rig.mustLocal("capture.levelStatus", args(id));
  EXPECT_FALSE(static_cast<bool>(status["tone"]));
  EXPECT_NEAR(static_cast<double>(status["dryLevel"]["holdDb"]), -12.0, 0.2);
  EXPECT_NEAR(static_cast<double>(status["wetLevel"]["holdDb"]), -12.0, 0.2);
  EXPECT_EQ(status["state"].toString(), "inRange");
  for (const float v : dev.out0)
    EXPECT_EQ(v, 0.0f);
  for (const float v : dev.out1)
    EXPECT_EQ(v, 0.0f);
}

// The audio thread never waits for the message thread, and a run can be
// started, replaced and stopped while callbacks go on.
TEST(CaptureAudioThreads, RunsStartAndStopWhileTheCallbackGoesOn) {
  CaptureAudio engine;
  std::atomic<bool> stop{false};
  std::atomic<int> blocks{0};
  std::thread audio([&] {
    juce::AudioBuffer<float> buffer(2, 128);
    while (!stop.load()) {
      for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 128; ++i)
          buffer.setSample(ch, i, 0.1f);
      {
        CaptureAudio::BlockScope scope(engine, buffer);
        // Whatever the rig does with the silence it was handed.
      }
      blocks.fetch_add(1);
    }
  });

  const std::vector<float> signal = makeSine(20000, 440.0, 0.5f);
  for (int round = 0; round < 200; ++round) {
    engine.startLevels(48000.0, {0, 0, -1}, -20.0f, true);
    engine.setSendLevel(-10.0f - static_cast<float>(round % 5));
    engine.measureNoise(0.01);
    engine.startPlay(48000.0, {1, 0, -1}, signal, 100, 100);
    engine.startListen(48000.0, {0, 0, 1}, 5000);
    if (round % 3 == 0)
      engine.stop();
    const int seen = blocks.load();
    while (blocks.load() < seen + 2)
      std::this_thread::yield();
  }
  engine.stop();
  stop.store(true);
  audio.join();
  EXPECT_GT(blocks.load(), 400);
  EXPECT_EQ(engine.getMode(), CaptureAudio::Mode::off);
}

// A finished run hands over exactly what was played and heard, in order, and
// the meters read what came in.
TEST(CaptureAudioThreads, PlayRecordsTheInputFromTheFirstBlockOn) {
  CaptureAudio engine;
  std::vector<float> signal(1000);
  for (size_t i = 0; i < signal.size(); ++i)
    signal[i] = 0.001f * static_cast<float>(i + 1);
  engine.startPlay(48000.0, {1, 0, -1}, signal, 300, 50);
  EXPECT_EQ(engine.plannedSamples(), 1350);

  std::vector<float> played;
  juce::AudioBuffer<float> buffer(2, 128);
  int index = 0;
  while (engine.getRunState() == CaptureAudio::RunState::running) {
    for (int i = 0; i < 128; ++i) {
      buffer.setSample(0, i, 0.5f - 0.0001f * static_cast<float>(index + i));
      buffer.setSample(1, i, 0.9f);
    }
    {
      CaptureAudio::BlockScope scope(engine, buffer);
    }
    for (int i = 0; i < 128; ++i)
      played.push_back(buffer.getSample(1, i));
    // The other channel is silent.
    EXPECT_EQ(buffer.getSample(0, 5), 0.0f);
    index += 128;
  }
  ASSERT_EQ(engine.getRunState(), CaptureAudio::RunState::full);
  // 300 samples of pre-roll, then the signal, then silence.
  EXPECT_EQ(played[299], 0.0f);
  EXPECT_FLOAT_EQ(played[300], 0.001f);
  EXPECT_FLOAT_EQ(played[1299], 1.0f);
  EXPECT_EQ(played[1300], 0.0f);

  std::vector<float> heard, unused;
  ASSERT_TRUE(engine.take(heard, unused));
  ASSERT_EQ(heard.size(), 1350u);
  for (size_t i = 0; i < heard.size(); ++i)
    ASSERT_FLOAT_EQ(heard[i], 0.5f - 0.0001f * static_cast<float>(i)) << i;
  EXPECT_EQ(engine.getRunState(), CaptureAudio::RunState::idle);
}

// ####################
// RE-AMP RECORDING
// ####################
TEST(CaptureRecording, ReampLinesUpWithWhatWasPlayedAndTheLatencyIsFound) {
  AgentRig rig;
  Device dev(rig);

  // The standard input file, as the settings name it.
  const std::vector<float> input = makeStandardInput(12.0);
  const juce::File inputFile = writeTestWav(rig.dirs.dir.getChildFile("standard.wav"), input);
  rig.mustLocal("capture.setSettings",
                "{\"standardInputFile\":" + agentQuoted(inputFile.getFullPathName()) + "}");
  const juce::var settings = rig.mustLocal("capture.getSettings");
  EXPECT_TRUE(static_cast<bool>(settings["standardInput"]["readable"]));
  // Not the NAM project's file, so it is held back unless allowed.
  EXPECT_FALSE(static_cast<bool>(settings["standardInput"]["recognised"]));

  const juce::String id = reampAtLevels(rig);
  settleLevels(rig, dev, id);
  rig.mustLocal("capture.setStep", args(id, "\"step\":\"record\""));

  const CommandResult unrecognised = rig.local("capture.startRecording", args(id));
  EXPECT_FALSE(unrecognised.ok);
  EXPECT_TRUE(unrecognised.errorMessage.contains("standard input file"));

  dev.reset();
  juce::var status =
      rig.mustLocal("capture.startRecording", args(id, "\"allowUnrecognisedInput\":true"));
  EXPECT_EQ(status["state"].toString(), "recording");
  // The rig is muted while it records.
  status = finishRecording(rig, dev, id, [](size_t t, Device& d) {
    return std::make_pair(d.loopback(t), 0.0f);
  });
  ASSERT_EQ(status["state"].toString(), "complete") << status["error"].toString();
  EXPECT_NEAR(static_cast<double>(status["recordedSeconds"]), 12.0, 0.1);

  const juce::var session = rig.mustLocal("capture.get", args(id));
  const juce::File folder(session["folder"].toString());
  const std::vector<float> returned = readWav(folder.getChildFile("return.wav"));
  ASSERT_EQ(returned.size(), input.size());

  // Sample for sample: what came back at i is the amp's answer to what was
  // played at i - delay, the file scaled so that its peak is the send level.
  float peak = 0.0f;
  for (const float v : input)
    peak = std::max(peak, std::abs(v));
  const float scale = std::pow(10.0f, -20.0f / 20.0f) / peak;
  double worst = 0.0;
  for (size_t i = kDelay; i < input.size(); ++i) {
    const float expected = std::tanh(kGain * input[i - kDelay] * scale);
    worst = std::max(worst, static_cast<double>(std::abs(returned[i] - expected)));
  }
  EXPECT_LT(worst, 5.0e-4) << "the recording is not aligned with what was played";

  // The checks find the delay and pass a clean recording.
  const juce::var checked = rig.mustLocal("capture.runChecks", args(id));
  EXPECT_EQ(checked["step"].toString(), "check");
  const juce::var report = checked["checks"];
  EXPECT_EQ(report["verdict"].toString(), "pass") << juce::JSON::toString(report);
  EXPECT_EQ(static_cast<int>(report["latencySamples"]), kDelay);
  EXPECT_NEAR(static_cast<double>(report["latencyMs"]), 1000.0 * kDelay / 48000.0, 0.01);
  EXPECT_GT(static_cast<double>(report["latencyConfidence"]), 0.8);
}

TEST(CaptureRecording, CancelDiscardsAndTheRigIsHeardAgain) {
  AgentRig rig;
  Device dev(rig);
  const std::vector<float> input = makeStandardInput(6.0);
  const juce::File inputFile = writeTestWav(rig.dirs.dir.getChildFile("standard.wav"), input);
  rig.mustLocal("capture.setSettings",
                "{\"standardInputFile\":" + agentQuoted(inputFile.getFullPathName()) + "}");
  const juce::String id = reampAtLevels(rig);
  settleLevels(rig, dev, id);
  rig.mustLocal("capture.setStep", args(id, "\"step\":\"record\""));
  rig.mustLocal("capture.startRecording", args(id, "\"allowUnrecognisedInput\":true"));
  dev.pumpLoopback(500);

  // Another capture cannot take the device meanwhile.
  const juce::var other = rig.mustLocal("capture.create", R"({"name":"Other"})");
  EXPECT_EQ(rig.proc.captureAudio.getMode(), CaptureAudio::Mode::play);
  const juce::var cancelled = rig.mustLocal("capture.cancelRecording", args(id));
  EXPECT_EQ(cancelled["state"].toString(), "none");
  EXPECT_EQ(rig.proc.captureAudio.getMode(), CaptureAudio::Mode::off);
  EXPECT_EQ(rig.mustLocal("capture.get", args(id))["step"].toString(), "record");

  // And the live rig passes its input again.
  const std::vector<float> tone = makeSine(1024, 440.0, 0.3f);
  const std::vector<float> out = rig.processLive(tone);
  double energy = 0.0;
  for (const float v : out)
    energy += static_cast<double>(v) * v;
  EXPECT_GT(energy, 1.0);
  (void)other;
}

TEST(CaptureRecording, DryWetRecordsBothInputsTogetherAndPlaysNothing) {
  AgentRig rig;
  Device dev(rig);
  const juce::var created = rig.mustLocal(
      "capture.create", R"({"name":"Dry wet","source":"ampHead","method":"dryWet"})");
  const juce::String id = idOf(created);
  rig.mustLocal("capture.setStep", args(id, "\"step\":\"wiring\""));
  rig.mustLocal("capture.setChannels", args(id, "\"dry\":0,\"wet\":1"));
  rig.mustLocal("capture.setStep", args(id, "\"step\":\"levels\""));
  rig.mustLocal("capture.setLevels", args(id, "\"recordSeconds\":10"));

  // The guitar, and the amp's answer to it 30 samples later.
  std::vector<float> guitar = pluckedClip(10.0, 0.25f, 0.6);
  float guitarPeak = 0.0f;
  for (size_t i = 0; i < 400u * 128u; ++i)  // what the levels run hears
    guitarPeak = std::max(guitarPeak, std::abs(guitar[i]));
  for (float& v : guitar)
    v *= 0.25f / guitarPeak;
  const int ampDelay = 30;
  const auto dryAt = [&](size_t t) { return guitar[t % guitar.size()]; };
  const auto wetAt = [&](size_t t) {
    return t >= static_cast<size_t>(ampDelay)
               ? 0.25f * std::tanh(3.0f * dryAt(t - ampDelay) / 0.25f) / std::tanh(3.0f)
               : 0.0f;
  };
  const auto feed = [&](size_t t, Device&) { return std::make_pair(dryAt(t), wetAt(t)); };

  rig.mustLocal("capture.startTone", args(id));
  dev.pump(400, feed);
  rig.mustLocal("capture.levelStatus", args(id));
  rig.mustLocal("capture.setStep", args(id, "\"step\":\"record\""));

  dev.reset();
  juce::var status = rig.mustLocal("capture.startRecording", args(id));
  EXPECT_EQ(status["state"].toString(), "recording");
  status = finishRecording(rig, dev, id, feed);
  ASSERT_EQ(status["state"].toString(), "complete") << status["error"].toString();

  const juce::File folder(rig.mustLocal("capture.get", args(id))["folder"].toString());
  const std::vector<float> dry = readWav(folder.getChildFile("dry.wav"));
  const std::vector<float> wet = readWav(folder.getChildFile("wet.wav"));
  ASSERT_EQ(dry.size(), 480000u);
  ASSERT_EQ(wet.size(), 480000u);
  for (size_t i = 0; i < dry.size(); i += 7) {
    ASSERT_NEAR(dry[i], dryAt(i), 2.0e-6) << i;
    ASSERT_NEAR(wet[i], wetAt(i), 2.0e-6) << i;
  }
  // Nothing was played while it recorded (afterwards the rig passes the
  // input through again).
  for (size_t i = 0; i < 480000u; ++i)
    ASSERT_EQ(dev.out0[i], 0.0f) << i;

  // Ten seconds is too short for a dry/wet capture; everything else holds,
  // and the delay between the two is found.
  const juce::var checked = rig.mustLocal("capture.runChecks", args(id));
  const juce::var report = checked["checks"];
  EXPECT_EQ(report["verdict"].toString(), "fail");
  EXPECT_EQ(static_cast<int>(report["latencySamples"]), ampDelay);
  for (const auto& check : *report["checks"].getArray()) {
    if (check["id"].toString() == "length")
      EXPECT_EQ(check["status"].toString(), "fail");
    if (check["id"].toString() == "copy")
      EXPECT_EQ(check["status"].toString(), "pass") << juce::JSON::toString(check);
  }

  // The failed checks stop training until they are overridden.
  const CommandResult refused = rig.local("capture.startTraining", args(id));
  EXPECT_FALSE(refused.ok);
  EXPECT_TRUE(refused.errorMessage.contains("checks failed")) << refused.errorMessage;
}

// ####################
// THE CHECKS
// ####################
namespace {

CaptureChecks::Options reampOptions() {
  CaptureChecks::Options o;
  o.sampleRate = 48000.0;
  return o;
}

struct Pair {
  std::vector<float> played, returned;
};

// A clean pair: the stand-in input and its answer through tanh and a delay.
Pair cleanPair(float gain = kGain, int delay = kDelay, float noise = kNoise, float dc = 0.0f) {
  Pair p;
  p.played = makeStandardInput(14.0);
  for (float& v : p.played)
    v *= 0.1f;
  p.returned.assign(p.played.size(), 0.0f);
  std::mt19937 gen(5);
  std::uniform_real_distribution<float> n(-noise, noise);
  for (size_t i = 0; i < p.played.size(); ++i) {
    const float source = i >= static_cast<size_t>(delay) ? p.played[i - delay] : 0.0f;
    p.returned[i] = std::tanh(gain * source) + n(gen) + dc;
  }
  return p;
}

CaptureChecks::Status statusOf(const CaptureChecks::Report& report, const char* id) {
  const auto* r = report.find(id);
  EXPECT_NE(r, nullptr) << id;
  return r != nullptr ? r->status : CaptureChecks::Status::fail;
}

}  // namespace

TEST(CaptureChecksTest, ACleanReampPassesAndTheDelayIsExact) {
  const Pair p = cleanPair();
  const auto report = CaptureChecks::checkReamp(p.played, p.returned, reampOptions());
  EXPECT_EQ(report.verdict, CaptureChecks::Status::pass) << juce::JSON::toString(report.toVar());
  EXPECT_TRUE(report.latencyKnown);
  EXPECT_EQ(report.latencySamples, kDelay);
  EXPECT_NEAR(report.latencyMs, 1000.0 * kDelay / 48000.0, 0.01);
  EXPECT_EQ(report.results.size(), 6u);
}

TEST(CaptureChecksTest, ClippingIsCountedAndFails) {
  Pair p = cleanPair();
  // Flat tops: three places at full scale.
  for (const size_t start : {100000u, 300000u, 500000u})
    for (size_t i = start; i < start + 20; ++i)
      p.returned[i] = 1.0f;
  const auto report = CaptureChecks::checkReamp(p.played, p.returned, reampOptions());
  EXPECT_EQ(statusOf(report, "clipping"), CaptureChecks::Status::fail);
  EXPECT_NE(report.find("clipping")->value.indexOf("60 samples in 3 places"), -1)
      << report.find("clipping")->value;
  EXPECT_EQ(report.verdict, CaptureChecks::Status::fail);
  // A single touch is only a warning.
  Pair q = cleanPair();
  q.returned[100000] = 1.0f;
  EXPECT_EQ(statusOf(CaptureChecks::checkReamp(q.played, q.returned, reampOptions()), "clipping"),
            CaptureChecks::Status::warn);
}

TEST(CaptureChecksTest, LowAndHighLevelsAreOutsideTheBand) {
  // Target -12 dB, band 3: a peak of -22 is 7 dB under the band's floor.
  Pair low = cleanPair(0.25f);
  const auto lowReport = CaptureChecks::checkReamp(low.played, low.returned, reampOptions());
  EXPECT_EQ(statusOf(lowReport, "level"), CaptureChecks::Status::fail);
  EXPECT_TRUE(lowReport.find("level")->message.contains("low"));

  // A little under the band's floor: a warning.
  Pair closeBy = cleanPair();
  for (float& v : closeBy.returned)
    v *= static_cast<float>(std::pow(10.0, -3.0 / 20.0));
  EXPECT_EQ(statusOf(CaptureChecks::checkReamp(closeBy.played, closeBy.returned, reampOptions()), "level"),
            CaptureChecks::Status::warn);

  Pair high = cleanPair(30.0f);  // saturates near 0 dBFS
  const auto highReport = CaptureChecks::checkReamp(high.played, high.returned, reampOptions());
  EXPECT_EQ(statusOf(highReport, "level"), CaptureChecks::Status::fail);

  // A different target moves the band: -8 dB, as TONE3000 asks.
  CaptureChecks::Options other = reampOptions();
  other.targetDb = -8.0;
  Pair ok = cleanPair(3.4f);  // tanh(0.34): -8.7 dBFS
  EXPECT_EQ(statusOf(CaptureChecks::checkReamp(ok.played, ok.returned, other), "level"),
            CaptureChecks::Status::pass);
}

TEST(CaptureChecksTest, NoiseHumAndOffsetAreFlagged) {
  // White noise at -40 dBFS RMS under a -12 dB peak.
  Pair noisy = cleanPair(kGain, kDelay, 0.0173f);
  const auto noisyReport = CaptureChecks::checkReamp(noisy.played, noisy.returned, reampOptions());
  EXPECT_EQ(statusOf(noisyReport, "noise"), CaptureChecks::Status::fail);

  Pair offset = cleanPair(kGain, kDelay, kNoise, 0.02f);
  EXPECT_EQ(statusOf(CaptureChecks::checkReamp(offset.played, offset.returned, reampOptions()), "dc"),
            CaptureChecks::Status::fail);
  Pair slight = cleanPair(kGain, kDelay, kNoise, 0.005f);
  EXPECT_EQ(statusOf(CaptureChecks::checkReamp(slight.played, slight.returned, reampOptions()), "dc"),
            CaptureChecks::Status::warn);
}

TEST(CaptureChecksTest, LengthAndUnrelatedReturnsFail) {
  Pair p = cleanPair();
  // Shorter than the signal: cut off.
  std::vector<float> shortReturn(p.returned.begin(), p.returned.end() - 4800);
  EXPECT_EQ(statusOf(CaptureChecks::checkReamp(p.played, shortReturn, reampOptions()), "length"),
            CaptureChecks::Status::fail);
  // A little longer: the trainer takes up to a second.
  std::vector<float> longReturn = p.returned;
  longReturn.resize(longReturn.size() + 4800, 0.0f);
  EXPECT_EQ(statusOf(CaptureChecks::checkReamp(p.played, longReturn, reampOptions()), "length"),
            CaptureChecks::Status::warn);
  longReturn.resize(p.returned.size() + 60000, 0.0f);
  EXPECT_EQ(statusOf(CaptureChecks::checkReamp(p.played, longReturn, reampOptions()), "length"),
            CaptureChecks::Status::fail);

  // Something that does not follow what was played.
  Pair unrelated = cleanPair();
  const Pair other = cleanPair();
  std::mt19937 gen(77);
  std::uniform_real_distribution<float> n(-0.2f, 0.2f);
  for (float& v : unrelated.returned)
    v = n(gen);
  const auto report = CaptureChecks::checkReamp(unrelated.played, unrelated.returned, reampOptions());
  EXPECT_EQ(statusOf(report, "latency"), CaptureChecks::Status::fail);
  EXPECT_FALSE(report.latencyKnown && report.latencyConfidence > 0.2);
  (void)other;

  // A delay beyond what the trainer can calibrate.
  Pair late = cleanPair(kGain, 12000);
  CaptureChecks::Options wide = reampOptions();
  EXPECT_EQ(statusOf(CaptureChecks::checkReamp(late.played, late.returned, wide), "latency"),
            CaptureChecks::Status::fail);
}

TEST(CaptureChecksTest, DryWetNeedsSignalOnBothAndTheWetMustDiffer) {
  const std::vector<float> clip = pluckedClip(20.0, 0.25f, 0.6);
  std::vector<float> dry = clip;
  float peak = 0.0f;
  for (const float v : dry)
    peak = std::max(peak, std::abs(v));
  for (float& v : dry)
    v *= 0.25f / peak;
  CaptureChecks::Options o = reampOptions();
  o.minSeconds = 10.0;
  o.failSeconds = 5.0;

  std::vector<float> wet(dry.size(), 0.0f);
  for (size_t i = 40; i < dry.size(); ++i)
    wet[i] = 0.25f * std::tanh(3.0f * dry[i - 40] / 0.25f) / std::tanh(3.0f);
  auto report = CaptureChecks::checkDryWet(dry, wet, o);
  // Playing with no pause to measure noise in only warns.
  EXPECT_NE(report.verdict, CaptureChecks::Status::fail) << juce::JSON::toString(report.toVar());
  EXPECT_EQ(statusOf(report, "length"), CaptureChecks::Status::pass);
  EXPECT_EQ(statusOf(report, "level.dry"), CaptureChecks::Status::pass);
  EXPECT_EQ(statusOf(report, "level.wet"), CaptureChecks::Status::pass);
  EXPECT_NE(statusOf(report, "noise.wet"), CaptureChecks::Status::fail);
  EXPECT_TRUE(report.latencyKnown);
  EXPECT_EQ(report.latencySamples, 40);
  EXPECT_EQ(statusOf(report, "copy"), CaptureChecks::Status::pass);

  // The wet before the dry gives a negative delay.
  std::vector<float> early(dry.size(), 0.0f);
  for (size_t i = 0; i + 25 < dry.size(); ++i)
    early[i] = 0.25f * std::tanh(3.0f * dry[i + 25] / 0.25f) / std::tanh(3.0f);
  EXPECT_EQ(CaptureChecks::checkDryWet(dry, early, o).latencySamples, -25);

  // The same signal on both inputs: not a capture of anything.
  std::vector<float> copy(dry.size());
  for (size_t i = 0; i < dry.size(); ++i)
    copy[i] = dry[i] * 0.9f;
  report = CaptureChecks::checkDryWet(dry, copy, o);
  EXPECT_EQ(statusOf(report, "copy"), CaptureChecks::Status::fail);
  EXPECT_EQ(report.verdict, CaptureChecks::Status::fail);

  // Nothing on the wet input.
  report = CaptureChecks::checkDryWet(dry, std::vector<float>(dry.size(), 0.0f), o);
  EXPECT_EQ(statusOf(report, "level.wet"), CaptureChecks::Status::fail);
  EXPECT_EQ(statusOf(report, "copy"), CaptureChecks::Status::fail);

  // Short playing is a warning at first and a failure when very short.
  const std::vector<float> shortDry(dry.begin(), dry.begin() + 48000 * 8);
  const std::vector<float> shortWet(wet.begin(), wet.begin() + 48000 * 8);
  EXPECT_EQ(statusOf(CaptureChecks::checkDryWet(shortDry, shortWet, o), "length"),
            CaptureChecks::Status::warn);
  const std::vector<float> tinyDry(dry.begin(), dry.begin() + 48000 * 3);
  const std::vector<float> tinyWet(wet.begin(), wet.begin() + 48000 * 3);
  EXPECT_EQ(statusOf(CaptureChecks::checkDryWet(tinyDry, tinyWet, o), "length"),
            CaptureChecks::Status::fail);
}

// ####################
// THE STATE MACHINE AND PERSISTENCE
// ####################
TEST(CaptureSessionTest, StepsOnlyAdvanceWhenTheirConditionsHold) {
  CaptureSession s = CaptureSession::make("test-1234", "Test");
  using Step = Capture::Step;
  EXPECT_EQ(s.step, Step::source);
  EXPECT_TRUE(s.whatBlocks().isEmpty());
  // Two steps at a time is never allowed.
  EXPECT_TRUE(s.whyCannotMoveTo(Step::levels).contains("one at a time"));
  EXPECT_TRUE(s.whyCannotMoveTo(Step::wiring).isEmpty());
  s.name = "  ";
  EXPECT_FALSE(s.whatBlocks().isEmpty());
  s.name = "Test";

  s.moveTo(Step::wiring);
  s.moveTo(Step::levels);
  EXPECT_TRUE(s.whyCannotMoveTo(Step::record).contains("Measure the levels"));
  s.levels.measured = true;
  EXPECT_TRUE(s.whyCannotMoveTo(Step::record).contains("outside the target band"));
  s.levels.inRange = true;
  EXPECT_TRUE(s.whyCannotMoveTo(Step::record).isEmpty());
  s.moveTo(Step::record);
  EXPECT_TRUE(s.whyCannotMoveTo(Step::check).contains("not finished"));
  s.recording.state = "complete";
  s.returnFile = "return.wav";
  s.moveTo(Step::check);
  EXPECT_TRUE(s.whyCannotMoveTo(Step::train).contains("Run the checks"));
  s.checks = agentJson(R"({"verdict":"fail"})");
  EXPECT_TRUE(s.whyCannotMoveTo(Step::train).contains("checks failed"));
  s.checksOverridden = true;
  EXPECT_TRUE(s.whyCannotMoveTo(Step::train).isEmpty());
  s.checksOverridden = false;
  s.checks = agentJson(R"({"verdict":"warn"})");
  EXPECT_TRUE(s.whyCannotMoveTo(Step::train).isEmpty());
  s.moveTo(Step::train);
  EXPECT_TRUE(s.whyCannotMoveTo(Step::review).contains("not finished"));
  s.training.state = "done";
  s.modelFile = "model.nam";
  s.moveTo(Step::review);

  // Back to the record step throws the recording, checks and training away.
  const std::vector<juce::String> stale = s.moveTo(Step::record);
  EXPECT_EQ(s.step, Step::record);
  EXPECT_FALSE(s.recording.complete());
  EXPECT_TRUE(s.checks.isVoid());
  EXPECT_EQ(s.training.state, "none");
  EXPECT_TRUE(s.returnFile.isEmpty());
  EXPECT_NE(std::find(stale.begin(), stale.end(), juce::String("return.wav")), stale.end());
  EXPECT_NE(std::find(stale.begin(), stale.end(), juce::String("model.nam")), stale.end());
  // The levels it was taken at stay.
  EXPECT_TRUE(s.levels.inRange);
}

TEST(CaptureCommandsTest, OutOfOrderStepsAreRefused) {
  AgentRig rig;
  const juce::String id = idOf(rig.mustLocal("capture.create", R"({"name":"Order"})"));
  const auto refused = [&](const char* command, const juce::String& a = {}) {
    const CommandResult r = rig.local(command, a.isEmpty() ? args(id) : a);
    EXPECT_FALSE(r.ok) << command;
    return r;
  };
  EXPECT_TRUE(refused("capture.setStep", args(id, "\"step\":\"levels\""))
                  .errorMessage.contains("one at a time"));
  refused("capture.startTone");
  refused("capture.startRecording");
  refused("capture.runChecks");
  refused("capture.startTraining");
  refused("capture.review");
  refused("capture.addToRig");
  refused("capture.save");
  EXPECT_EQ(refused("capture.get", args("no-such-capture")).errorCode, CommandError::notFound);
  EXPECT_EQ(refused("capture.get", args("../escape")).errorCode, CommandError::invalidArgs);

  // Dry and wet on one channel is not a pair.
  const juce::String dw = idOf(
      rig.mustLocal("capture.create", R"({"name":"Pair","method":"dryWet"})"));
  EXPECT_EQ(refused("capture.setChannels", args(dw, "\"dry\":1,\"wet\":1")).errorCode,
            CommandError::invalidArgs);
  // The method is fixed once the levels step is reached.
  rig.mustLocal("capture.setStep", args(dw, "\"step\":\"wiring\""));
  rig.mustLocal("capture.setStep", args(dw, "\"step\":\"levels\""));
  EXPECT_EQ(refused("capture.setSource", args(dw, "\"method\":\"reamp\"")).errorCode,
            CommandError::failed);
  // A name and gear can still be changed.
  const juce::var renamed = rig.mustLocal("capture.setSource", args(dw, "\"name\":\"Renamed\",\"gearMake\":\"Fender\""));
  EXPECT_EQ(renamed["name"].toString(), "Renamed");
  EXPECT_EQ(renamed["gear"]["make"].toString(), "Fender");
}

TEST(CaptureCommandsTest, SessionSurvivesARestart) {
  AgentRig rig;
  const juce::var created = rig.mustLocal(
      "capture.create",
      R"({"name":"Plexi lead","source":"ampCab","gainTag":"highGain","modelSize":"lite",
          "gearMake":"Marshall","gearModel":"Super Lead","notes":"Master 6, load box DI."})");
  const juce::String id = idOf(created);
  EXPECT_EQ(created["gear"]["type"].toString(), "ampCab");
  rig.mustLocal("capture.setStep", args(id, "\"step\":\"wiring\""));
  rig.mustLocal("capture.setChannels", args(id, "\"send\":1,\"returnIn\":1,\"calibrationDbu\":4"));
  rig.mustLocal("capture.setStep", args(id, "\"step\":\"levels\""));
  rig.mustLocal("capture.setLevels", args(id, "\"sendDb\":-18,\"targetDb\":-8,\"bandDb\":2"));
  const juce::var before = rig.mustLocal("capture.get", args(id));
  EXPECT_TRUE(juce::File(before["folder"].toString()).getChildFile("session.json").existsAsFile());

  // A second studio over the same library is what a restart makes.
  CaptureStudio second(rig.proc, rig.commands.registry());
  const CommandResult again = second.get(id);
  ASSERT_TRUE(again.ok) << again.errorMessage;
  for (const char* key : {"name", "source", "method", "gainTag", "modelSize", "step", "notes"})
    EXPECT_EQ(again.value[key].toString(), before[key].toString()) << key;
  EXPECT_EQ(again.value["gear"]["make"].toString(), "Marshall");
  EXPECT_EQ(again.value["gear"]["model"].toString(), "Super Lead");
  EXPECT_EQ(static_cast<int>(again.value["channels"]["send"]), 1);
  EXPECT_NEAR(static_cast<double>(again.value["calibration"]["returnDbu"]), 4.0, 1e-9);
  EXPECT_NEAR(static_cast<double>(again.value["levels"]["sendDb"]), -18.0, 1e-9);
  EXPECT_NEAR(static_cast<double>(again.value["levels"]["targetDb"]), -8.0, 1e-9);
  EXPECT_NEAR(static_cast<double>(again.value["bandLowDb"]), -10.0, 1e-9);
  EXPECT_EQ(second.list().value["captures"].size(), 1);

  // A run that was going when the application stopped is not going now.
  CaptureSession session;
  juce::String error;
  ASSERT_TRUE(second.load(id, session, error)) << error;
  session.recording.state = "recording";
  session.training.state = "running";
  const juce::File file = rig.dirs.library.getChildFile("Captures").getChildFile(id).getChildFile("session.json");
  ASSERT_TRUE(file.replaceWithText(juce::JSON::toString(session.toVar(), false)));
  CaptureStudio third(rig.proc, rig.commands.registry());
  const CommandResult interrupted = third.get(id);
  ASSERT_TRUE(interrupted.ok);
  EXPECT_EQ(interrupted.value["recording"]["state"].toString(), "failed");
  EXPECT_TRUE(interrupted.value["recording"]["error"].toString().contains("interrupted"));
  EXPECT_EQ(interrupted.value["training"]["state"].toString(), "failed");
  CaptureSession reloaded;

  // Nothing but a session file is accepted as one.
  EXPECT_FALSE(CaptureSession::fromVar(agentJson(R"({"hello":1})"), reloaded, error));
}

TEST(CaptureCommandsTest, PermissionsAreLocalExceptGetAndList) {
  AgentRig rig;
  const juce::var all = rig.commands.registry().describe();
  int captureCommands = 0;
  for (int i = 0; i < all.size(); ++i) {
    const juce::String name = all[i]["name"].toString();
    if (!name.startsWith("capture."))
      continue;
    ++captureCommands;
    const juce::String permission = all[i]["permission"].toString();
    if (name == "capture.get" || name == "capture.list")
      EXPECT_EQ(permission, "read") << name;
    else
      EXPECT_EQ(permission, "local") << name;
  }
  EXPECT_GE(captureCommands, 25);

  // A command server client may read captures and nothing else about them.
  const auto client = rig.hub.openSession("test agent");
  const juce::String id = idOf(rig.mustLocal("capture.create", R"({"name":"Visible"})"));
  EXPECT_TRUE(rig.as(client, "capture.list").ok);
  EXPECT_TRUE(rig.as(client, "capture.get", args(id)).ok);
  EXPECT_EQ(codeOfResult(rig.as(client, "capture.create", R"({"name":"Sneaky"})")), CommandError::permissionDenied);
  EXPECT_EQ(codeOfResult(rig.as(client, "capture.startTone", args(id))), CommandError::permissionDenied);
  EXPECT_EQ(codeOfResult(rig.as(client, "capture.startTraining", args(id))), CommandError::permissionDenied);
  EXPECT_EQ(codeOfResult(rig.as(client, "capture.delete", args(id))), CommandError::permissionDenied);
  EXPECT_EQ(codeOfResult(rig.as(client, "capture.setSettings", R"({"pythonExecutable":"x"})")), CommandError::permissionDenied);
  rig.hub.closeSession(client);
}

TEST(CaptureCommandsTest, AssetsListLeavesWorkingFilesAlone) {
  AgentRig rig;
  const juce::String id = idOf(rig.mustLocal("capture.create", R"({"name":"Working"})"));
  const juce::File folder(rig.mustLocal("capture.get", args(id))["folder"].toString());
  writeTestWav(folder.getChildFile("return.wav"), makeSine(4800, 440.0, 0.1f));
  ASSERT_TRUE(testFile("a2-amp-test.nam").copyFileTo(folder.getChildFile("model.nam")));
  // A saved capture is a library file in the Captures folder itself.
  ASSERT_TRUE(testFile("a2-amp-test.nam").copyFileTo(rig.dirs.library.getChildFile("Captures").getChildFile("Saved.nam")));

  const juce::var listed = rig.mustLocal("assets.list");
  juce::StringArray names;
  for (const auto& asset : *listed["assets"].getArray())
    names.add(asset["name"].toString());
  EXPECT_TRUE(names.contains("Saved")) << names.joinIntoString(",");
  EXPECT_FALSE(names.contains("return"));
  EXPECT_FALSE(names.contains("model"));
}

// ####################
// THE TRAINER
// ####################
namespace {

#if !JUCE_WINDOWS

// A stand-in for `python`: answers the version query, then plays the
// trainer's part with the lines the helper script prints.
juce::File writeFakePython(const juce::File& dir, const juce::String& behaviour,
                           const juce::File& model, const juce::String& version = "0.13.0") {
  const juce::String body =
      "#!/bin/sh\n"
      "if [ \"$1\" = \"-c\" ]; then\n" +
      (behaviour == "nopackage"
           ? juce::String("  echo \"ModuleNotFoundError: No module named 'nam'\" >&2; exit 1\n")
           : "  echo \"" + version + "\"; exit 0\n") +
      "fi\n"
      "outdir=\"\"\n"
      "while [ $# -gt 0 ]; do\n"
      "  case \"$1\" in --outdir) outdir=\"$2\"; shift;; esac\n"
      "  shift\n"
      "done\n"
      "mkdir -p \"$outdir\"\n"
      "for e in 0 1 2 3; do\n"
      "  printf 'Epoch %s:  10%%|#   | 1/10\\r' $e\n"
      "  echo \"PEDALEIRA_EPOCH {\\\"epoch\\\": $((e+1)), \\\"epochs\\\": 4, \\\"esr\\\": 0.0$((5-e))}\"\n"
      "  sleep 0.05\n"
      "done\n" +
      (behaviour == "slow" ? juce::String("exec sleep 60\n") : juce::String()) +
      (behaviour == "checksfail"
           ? juce::String("echo 'Failed checks!'\n"
                          "echo 'PEDALEIRA_RESULT {\"ok\": false, \"reason\": \"checks\", "
                          "\"latency\": 12, \"checksPassed\": false}'\n"
                          "exit 3\n")
           : juce::String()) +
      "cp \"" + model.getFullPathName() + "\" \"$outdir/model.nam\"\n"
      "echo \"PEDALEIRA_RESULT {\\\"ok\\\": true, \\\"esr\\\": 0.0123, \\\"latency\\\": 4, "
      "\\\"checksPassed\\\": true, \\\"modelFile\\\": \\\"$outdir/model.nam\\\"}\"\n";
  const juce::File script = dir.getChildFile("fake-python-" + behaviour);
  EXPECT_TRUE(script.replaceWithText(body, false, false, "\n"));
  EXPECT_TRUE(script.setExecutePermission(true));
  return script;
}

TrainerRunner::Job fakeJob(const juce::File& dir, const juce::File& python) {
  TrainerRunner::Job job;
  job.python = python.getFullPathName();
  job.mode = "reamp";
  job.workDir = dir.getChildFile("work");
  job.outDir = dir.getChildFile("work").getChildFile("training");
  job.input = dir.getChildFile("in.wav");
  job.output = dir.getChildFile("out.wav");
  job.epochs = 4;
  job.metadata = agentJson(R"({"name":"Fake"})");
  return job;
}

bool waitForRunner(TrainerRunner& runner, int timeoutMs = 20000) {
  const auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(timeoutMs);
  while (runner.isActive() && juce::Time::getMillisecondCounter() < deadline)
    juce::Thread::sleep(10);
  return !runner.isActive();
}

#endif

}  // namespace

TEST(TrainerRunnerTest, VersionsAndBands) {
  EXPECT_TRUE(TrainerRunner::versionAtLeast("0.13.0", "0.13.0"));
  EXPECT_TRUE(TrainerRunner::versionAtLeast("0.13.1", "0.13.0"));
  EXPECT_TRUE(TrainerRunner::versionAtLeast("0.14.0.dev3", "0.13.0"));
  EXPECT_TRUE(TrainerRunner::versionAtLeast("1.0.0", "0.13.0"));
  EXPECT_TRUE(TrainerRunner::versionAtLeast("v0.13.0rc1", "0.13.0"));
  EXPECT_FALSE(TrainerRunner::versionAtLeast("0.12.9", "0.13.0"));
  EXPECT_FALSE(TrainerRunner::versionAtLeast("0.9.2", "0.13.0"));
  EXPECT_FALSE(TrainerRunner::versionAtLeast("garbage", "0.13.0"));

  EXPECT_STREQ(TrainerRunner::esrBand(0.005), "great");
  EXPECT_STREQ(TrainerRunner::esrBand(0.0099), "great");
  EXPECT_STREQ(TrainerRunner::esrBand(0.01), "notBad");
  EXPECT_STREQ(TrainerRunner::esrBand(0.034), "notBad");
  EXPECT_STREQ(TrainerRunner::esrBand(0.05), "mightBeOkay");
  EXPECT_STREQ(TrainerRunner::esrBand(0.2), "poor");
  EXPECT_STREQ(TrainerRunner::esrBand(0.3), "failed");
  EXPECT_STREQ(TrainerRunner::esrBandLabel(0.05), "Might be okay");
}

TEST(TrainerRunnerTest, ReadsNamFilesAndRefusesWhatIsNotA2) {
  NamFile::Info info;
  ASSERT_TRUE(NamFile::read(testFile("a2-amp-test.nam"), info).wasOk());
  EXPECT_EQ(info.architecture, "SlimmableContainer");
  EXPECT_TRUE(info.hasEsr);
  EXPECT_NEAR(info.validationEsr, 0.0513, 0.001);
  EXPECT_TRUE(NamFile::requireA2(info).wasOk());

  AgentTestDirs dirs;
  const juce::File old = dirs.dir.getChildFile("old.nam");
  old.replaceWithText(R"({"version":"0.5.4","architecture":"WaveNet","config":{},"metadata":{},"weights":[]})");
  ASSERT_TRUE(NamFile::read(old, info).wasOk());
  const auto refused = NamFile::requireA2(info);
  EXPECT_TRUE(refused.failed());
  EXPECT_TRUE(refused.getErrorMessage().contains("not NAM A2")) << refused.getErrorMessage();
  EXPECT_TRUE(refused.getErrorMessage().contains("0.13.0"));

  const juce::File junk = dirs.dir.getChildFile("junk.nam");
  junk.replaceWithText("not json at all");
  EXPECT_TRUE(NamFile::read(junk, info).failed());
  EXPECT_TRUE(NamFile::read(dirs.dir.getChildFile("missing.nam"), info).failed());
}

TEST(TrainerRunnerTest, MetadataIsSetOnACopyAndNothingElseChanges) {
  AgentTestDirs dirs;
  const juce::File source = testFile("a2-amp-test.nam");
  const juce::File copy = dirs.dir.getChildFile("copy.nam");
  const juce::var updates = agentJson(
      R"({"name":"Brit 800","gear_type":"amp","gear_make":"Marshall","tone_type":"crunch","output_level_dbu":12.5})");
  NamFile::Info before;
  ASSERT_TRUE(NamFile::read(source, before).wasOk());
  EXPECT_FALSE(NamFile::metadataHas(before, updates));

  ASSERT_TRUE(NamFile::writeCopyWithMetadata(source, copy, updates).wasOk());
  NamFile::Info after;
  ASSERT_TRUE(NamFile::read(copy, after).wasOk());
  EXPECT_TRUE(NamFile::metadataHas(after, updates));
  EXPECT_EQ(after.metadata["name"].toString(), "Brit 800");
  EXPECT_NEAR(static_cast<double>(after.metadata["output_level_dbu"]), 12.5, 1e-9);
  // What the trainer wrote stays: loudness, the training record.
  EXPECT_NEAR(static_cast<double>(after.metadata["loudness"]),
              static_cast<double>(before.metadata["loudness"]), 1e-9);
  EXPECT_NEAR(after.validationEsr, before.validationEsr, 1e-12);
  EXPECT_EQ(after.architecture, before.architecture);

  // The weights are the same bytes: only the metadata object differs.
  const juce::String a = source.loadFileAsString(), b = copy.loadFileAsString();
  const auto weightsAt = [](const juce::String& text) { return text.substring(text.indexOf("\"config\"")); };
  EXPECT_EQ(weightsAt(a), weightsAt(b));
  EXPECT_TRUE(source.existsAsFile());
  // And the copy still loads as a tone.
  AgentRig rig;
  const CommandResult added =
      rig.local("chain.addToneFromFiles", "{\"paths\":[" + agentQuoted(copy.getFullPathName()) + "]}");
  EXPECT_TRUE(added.ok) << added.errorMessage;
}

#if !JUCE_WINDOWS

TEST(TrainerRunnerTest, RunsAFakeTrainerAndReadsItsProgress) {
  AgentTestDirs dirs;
  const juce::File python = writeFakePython(dirs.dir, "ok", testFile("a2-amp-test.nam"));
  const TrainerRunner::Install install = TrainerRunner::checkInstall(python.getFullPathName());
  EXPECT_TRUE(install.versionOk);
  EXPECT_EQ(install.version, "0.13.0");

  TrainerRunner runner;
  ASSERT_TRUE(runner.start(fakeJob(dirs.dir, python)).wasOk());
  ASSERT_TRUE(waitForRunner(runner));
  const juce::var status = runner.status();
  EXPECT_EQ(status["state"].toString(), "done") << juce::JSON::toString(status);
  EXPECT_EQ(static_cast<int>(status["epoch"]), 4);
  EXPECT_EQ(static_cast<int>(status["epochs"]), 4);
  EXPECT_NEAR(static_cast<double>(status["progress"]), 1.0, 1e-9);
  EXPECT_NEAR(static_cast<double>(status["esr"]), 0.0123, 1e-9);
  EXPECT_EQ(status["esrBand"].toString(), "notBad");
  EXPECT_EQ(static_cast<int>(status["latencySamples"]), 4);
  EXPECT_TRUE(static_cast<bool>(status["trainerChecksPassed"]));
  EXPECT_EQ(status["architecture"].toString(), "SlimmableContainer");
  EXPECT_TRUE(juce::File(status["modelFile"].toString()).existsAsFile());
  EXPECT_GT(status["log"].size(), 0);
  // The helper and the job are written beside the work, for a hand run.
  EXPECT_TRUE(dirs.dir.getChildFile("work").getChildFile("train_helper.py").existsAsFile());
  EXPECT_TRUE(dirs.dir.getChildFile("work").getChildFile("job.json").existsAsFile());
}

TEST(TrainerRunnerTest, CancelKillsTheProcess) {
  AgentTestDirs dirs;
  const juce::File python = writeFakePython(dirs.dir, "slow", testFile("a2-amp-test.nam"));
  TrainerRunner runner;
  ASSERT_TRUE(runner.start(fakeJob(dirs.dir, python)).wasOk());
  // Wait for the first epochs to show, then cancel.
  for (int i = 0; i < 400 && static_cast<int>(runner.status()["epoch"]) < 2; ++i)
    juce::Thread::sleep(10);
  EXPECT_GE(static_cast<int>(runner.status()["epoch"]), 2);
  EXPECT_TRUE(runner.isActive());
  const auto began = juce::Time::getMillisecondCounter();
  runner.cancel();
  ASSERT_TRUE(waitForRunner(runner, 10000));
  EXPECT_LT(juce::Time::getMillisecondCounter() - began, 5000u);
  EXPECT_EQ(runner.status()["state"].toString(), "cancelled");
}

TEST(TrainerRunnerTest, RefusesOldVersionsMissingPackagesAndNonA2Output) {
  AgentTestDirs dirs;
  // A trainer from before A2.
  {
    TrainerRunner runner;
    ASSERT_TRUE(runner.start(fakeJob(dirs.dir, writeFakePython(dirs.dir, "ok", testFile("a2-amp-test.nam"), "0.12.1"))).wasOk());
    ASSERT_TRUE(waitForRunner(runner));
    const juce::var status = runner.status();
    EXPECT_EQ(status["state"].toString(), "failed");
    EXPECT_TRUE(status["error"].toString().contains("0.13.0")) << status["error"].toString();
    EXPECT_TRUE(status["error"].toString().contains("A2"));
  }
  // No package at all.
  {
    TrainerRunner runner;
    ASSERT_TRUE(runner.start(fakeJob(dirs.dir, writeFakePython(dirs.dir, "nopackage", testFile("a2-amp-test.nam")))).wasOk());
    ASSERT_TRUE(waitForRunner(runner));
    EXPECT_TRUE(runner.status()["error"].toString().contains("not installed"));
  }
  // No Python.
  {
    TrainerRunner runner;
    TrainerRunner::Job job = fakeJob(dirs.dir, dirs.dir.getChildFile("no-such-python"));
    ASSERT_TRUE(runner.start(job).wasOk());
    ASSERT_TRUE(waitForRunner(runner));
    EXPECT_EQ(runner.status()["state"].toString(), "failed");
    EXPECT_TRUE(runner.status()["error"].toString().isNotEmpty());
  }
  // A trainer that writes a model that is not A2.
  {
    const juce::File old = dirs.dir.getChildFile("old-model.nam");
    old.replaceWithText(R"({"version":"0.5.4","architecture":"WaveNet","config":{},"metadata":{},"weights":[]})");
    TrainerRunner runner;
    ASSERT_TRUE(runner.start(fakeJob(dirs.dir, writeFakePython(dirs.dir, "ok", old))).wasOk());
    ASSERT_TRUE(waitForRunner(runner));
    const juce::var status = runner.status();
    EXPECT_EQ(status["state"].toString(), "failed");
    EXPECT_TRUE(status["error"].toString().contains("not NAM A2")) << status["error"].toString();
    EXPECT_EQ(status["architecture"].toString(), "WaveNet");
  }
  // The trainer's own data checks failing is reported as that.
  {
    TrainerRunner runner;
    ASSERT_TRUE(runner.start(fakeJob(dirs.dir, writeFakePython(dirs.dir, "checksfail", testFile("a2-amp-test.nam")))).wasOk());
    ASSERT_TRUE(waitForRunner(runner));
    const juce::var status = runner.status();
    EXPECT_EQ(status["state"].toString(), "failed");
    EXPECT_TRUE(status["error"].toString().contains("data checks"));
    EXPECT_FALSE(static_cast<bool>(status["trainerChecksPassed"]));
  }
}

namespace {

// A session at the check step with a recording on disk, without making one.
juce::String sessionReadyToTrain(AgentRig& rig, const char* method, const char* verdict) {
  const juce::String id = idOf(rig.mustLocal(
      "capture.create", juce::String("{\"name\":\"Ready to train\",\"method\":\"") + method +
                            "\",\"gearMake\":\"Marshall\",\"gainTag\":\"crunch\"}"));
  CaptureStudio& studio = rig.hub.captures();
  CaptureSession session;
  juce::String error;
  EXPECT_TRUE(studio.load(id, session, error));
  const juce::File folder = studio.sessionDir(id);
  const bool dryWet = juce::String(method) == "dryWet";
  const juce::File input = writeTestWav(rig.dirs.dir.getChildFile("standard.wav"), makeStandardInput(2.0));
  writeTestWav(folder.getChildFile(dryWet ? "dry.wav" : "return.wav"), makeStandardInput(2.0));
  if (dryWet)
    writeTestWav(folder.getChildFile("wet.wav"), makeStandardInput(2.0));
  session.step = Capture::Step::check;
  session.levels.measured = session.levels.inRange = true;
  session.recording.state = "complete";
  session.recording.samples = 96000;
  session.recording.seconds = 2.0;
  session.recording.inputFile = dryWet ? juce::String() : input.getFullPathName();
  session.returnFile = dryWet ? "" : "return.wav";
  session.dryFile = dryWet ? "dry.wav" : "";
  session.wetFile = dryWet ? "wet.wav" : "";
  session.checks = agentJson(juce::String("{\"verdict\":\"") + verdict + "\",\"latencySamples\":17}");
  const juce::File file = folder.getChildFile("session.json");
  EXPECT_TRUE(file.replaceWithText(juce::JSON::toString(session.toVar(), false)));
  return id;
}

juce::var waitForTraining(AgentRig& rig, const juce::String& id) {
  juce::var status;
  // No iteration cap: a loaded machine can starve the training thread for
  // longer than any fixed count; ctest's timeout bounds a real hang.
  for (;;) {
    status = rig.mustLocal("capture.trainingStatus", args(id));
    const juce::String state = status["state"].toString();
    if (state == "done" || state == "failed" || state == "cancelled")
      break;
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
  }
  return status;
}

}  // namespace

TEST(CaptureTraining, FromChecksToReviewToSavedFile) {
  AgentRig rig;
  const juce::File python = writeFakePython(rig.dirs.dir, "ok", testFile("a2-amp-test.nam"));
  rig.mustLocal("capture.setSettings",
                "{\"pythonExecutable\":" + agentQuoted(python.getFullPathName()) + "}");
  EXPECT_TRUE(TrainerRunner::checkInstall(python.getFullPathName()).versionOk);

  const juce::String id = sessionReadyToTrain(rig, "reamp", "pass");
  const juce::var started = rig.mustLocal("capture.startTraining", args(id, "\"epochs\":4"));
  EXPECT_TRUE(started["state"].toString() == "checking" || started["state"].toString() == "running");
  // A second run cannot start beside the first.
  EXPECT_FALSE(rig.local("capture.startTraining", args(id)).ok);

  const juce::var status = waitForTraining(rig, id);
  ASSERT_EQ(status["state"].toString(), "done") << juce::JSON::toString(status);
  EXPECT_NEAR(static_cast<double>(status["esr"]), 0.0123, 1e-9);
  EXPECT_EQ(status["esrBand"].toString(), "notBad");

  // Done moves the capture to review, with the model beside the session.
  const juce::var session = rig.mustLocal("capture.get", args(id));
  EXPECT_EQ(session["step"].toString(), "review");
  const juce::File folder(session["folder"].toString());
  EXPECT_TRUE(folder.getChildFile("model.nam").existsAsFile());
  EXPECT_FALSE(folder.getChildFile("training").exists());

  const juce::var review = rig.mustLocal("capture.review", args(id));
  EXPECT_TRUE(static_cast<bool>(review["a2"]));
  EXPECT_EQ(review["band"].toString(), "notBad");
  EXPECT_EQ(review["metadata"]["gear_type"].toString(), "amp");
  EXPECT_EQ(review["metadata"]["tone_type"].toString(), "crunch");
  EXPECT_NEAR(static_cast<double>(review["metadata"]["output_level_dbu"]), 12.5, 1e-9);

  // Added to the rig for an A/B against the real thing.
  const juce::var added = rig.mustLocal("capture.addToRig", args(id));
  EXPECT_TRUE(added["blockId"].toString().isNotEmpty()) << juce::JSON::toString(added);

  // Saved into the library with a readable name and the metadata set.
  const juce::var saved = rig.mustLocal("capture.save", args(id));
  const juce::File file(saved["file"].toString());
  EXPECT_EQ(file.getFileName(), "Ready to train.nam");
  EXPECT_EQ(file.getParentDirectory(), rig.dirs.library.getChildFile("Captures"));
  NamFile::Info info;
  ASSERT_TRUE(NamFile::read(file, info).wasOk());
  EXPECT_EQ(info.metadata["name"].toString(), "Ready to train");
  EXPECT_EQ(info.metadata["gear_make"].toString(), "Marshall");
  EXPECT_EQ(info.metadata["tone_type"].toString(), "crunch");
  EXPECT_EQ(info.architecture, "SlimmableContainer");
  EXPECT_FALSE(folder.getChildFile("model.nam").existsAsFile());
  // A second save does not overwrite; the same capture is not saved twice.
  EXPECT_TRUE(static_cast<bool>(rig.mustLocal("capture.save", args(id))["alreadySaved"]));
  EXPECT_EQ(rig.mustLocal("capture.get", args(id))["paths"]["saved"].toString(), file.getFullPathName());

  // Delete takes the working folder and keeps the saved capture.
  rig.mustLocal("capture.delete", args(id));
  EXPECT_FALSE(folder.exists());
  EXPECT_TRUE(file.existsAsFile());
}

TEST(CaptureTraining, FailedChecksNeedAnExplicitOverride) {
  AgentRig rig;
  const juce::File python = writeFakePython(rig.dirs.dir, "ok", testFile("a2-amp-test.nam"));
  rig.mustLocal("capture.setSettings",
                "{\"pythonExecutable\":" + agentQuoted(python.getFullPathName()) + "}");
  const juce::String id = sessionReadyToTrain(rig, "dryWet", "fail");
  const CommandResult refused = rig.local("capture.startTraining", args(id));
  EXPECT_FALSE(refused.ok);
  EXPECT_EQ(rig.mustLocal("capture.get", args(id))["step"].toString(), "check");

  rig.mustLocal("capture.startTraining", args(id, "\"overrideChecks\":true"));
  const juce::var status = waitForTraining(rig, id);
  EXPECT_EQ(status["state"].toString(), "done") << juce::JSON::toString(status);
  const juce::var session = rig.mustLocal("capture.get", args(id));
  EXPECT_TRUE(static_cast<bool>(session["checksOverridden"]));
  EXPECT_EQ(session["step"].toString(), "review");
  // The dry/wet job was given our delay and the wet/dry pair.
  const juce::var job = agentJson(juce::File(session["folder"].toString()).getChildFile("job.json").loadFileAsString());
  EXPECT_EQ(job["mode"].toString(), "dryWet");
  EXPECT_EQ(static_cast<int>(job["latency"]), 17);
  EXPECT_TRUE(job["input"].toString().endsWith("dry.wav"));
  EXPECT_TRUE(job["output"].toString().endsWith("wet.wav"));
}

TEST(CaptureTraining, CancelAndAWrongTrainerLeaveTheCaptureAtTheTrainStep) {
  AgentRig rig;
  const juce::File slow = writeFakePython(rig.dirs.dir, "slow", testFile("a2-amp-test.nam"));
  rig.mustLocal("capture.setSettings",
                "{\"pythonExecutable\":" + agentQuoted(slow.getFullPathName()) + "}");
  const juce::String id = sessionReadyToTrain(rig, "reamp", "pass");
  rig.mustLocal("capture.startTraining", args(id));
  for (int i = 0; i < 300 && static_cast<int>(rig.mustLocal("capture.trainingStatus", args(id))["epoch"]) < 2; ++i)
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
  const juce::var cancelled = rig.mustLocal("capture.cancelTraining", args(id));
  EXPECT_EQ(cancelled["state"].toString(), "cancelled");
  EXPECT_EQ(rig.mustLocal("capture.get", args(id))["step"].toString(), "train");

  // Going back and training again with an old trainer fails with its version.
  const juce::File old = writeFakePython(rig.dirs.dir, "old", testFile("a2-amp-test.nam"), "0.11.0");
  rig.mustLocal("capture.setSettings",
                "{\"pythonExecutable\":" + agentQuoted(old.getFullPathName()) + "}");
  rig.mustLocal("capture.startTraining", args(id));
  const juce::var failed = waitForTraining(rig, id);
  EXPECT_EQ(failed["state"].toString(), "failed");
  EXPECT_TRUE(failed["error"].toString().contains("0.13.0")) << failed["error"].toString();
  EXPECT_EQ(rig.mustLocal("capture.get", args(id))["training"]["state"].toString(), "failed");
}

#endif

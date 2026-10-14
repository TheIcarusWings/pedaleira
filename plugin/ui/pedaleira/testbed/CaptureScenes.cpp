#include "CaptureScenes.h"

#include "Processor.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <iostream>
#include <random>

namespace pedaleira::ui::testbed {

namespace {

constexpr int kBlock = 128;
constexpr double kRate = 48000.0;

// The device: runs blocks through the processor and answers with what it
// played, passed through a gain into a soft clipper, a delay and a little
// noise: the amp, as the engine's capture tests simulate it.
class SimDevice {
public:
  explicit SimDevice(TONE3000Processor& p) : processor(p) {}

  void loopback(int blocks, float gain, int delay = 200) {
    juce::AudioBuffer<float> buffer(2, kBlock);
    juce::MidiBuffer midi;
    for (int b = 0; b < blocks; ++b) {
      for (int i = 0; i < kBlock; ++i) {
        const size_t t = cursor + static_cast<size_t>(i);
        const float played = t >= static_cast<size_t>(delay) && t - static_cast<size_t>(delay) < played0.size()
                                 ? played0[t - static_cast<size_t>(delay)]
                                 : 0.0f;
        buffer.setSample(0, i, std::tanh(gain * played) + noise());
        buffer.setSample(1, i, 0.0f);
      }
      processor.processBlock(buffer, midi);
      for (int i = 0; i < kBlock; ++i)
        played0.push_back(buffer.getSample(0, i));
      cursor += kBlock;
    }
  }

  // A guitar on input 1 (the dry side) and the amp's answer on input 2.
  void guitar(int blocks, float gain) {
    juce::AudioBuffer<float> buffer(2, kBlock);
    juce::MidiBuffer midi;
    for (int b = 0; b < blocks; ++b) {
      for (int i = 0; i < kBlock; ++i) {
        const size_t t = cursor + static_cast<size_t>(i);
        const double seconds = static_cast<double>(t) / kRate;
        pluck = 0.97f * pluck + 0.03f * noise() * 400.0f;
        const float envelope = 0.55f + 0.45f * static_cast<float>(std::sin(seconds * 3.1));
        const float dry = 0.3f * envelope * std::sin(static_cast<float>(seconds * 2.0 * juce::MathConstants<double>::pi * 196.0)) + pluck;
        buffer.setSample(0, i, dry);
        buffer.setSample(1, i, std::tanh(gain * dry) * 0.8f + noise());
      }
      processor.processBlock(buffer, midi);
      cursor += kBlock;
    }
  }

  int blocksFor(double seconds) const { return static_cast<int>(seconds * kRate / kBlock); }

private:
  float noise() { return std::uniform_real_distribution<float>(-1.0e-4f, 1.0e-4f)(gen); }

  TONE3000Processor& processor;
  std::vector<float> played0;
  size_t cursor = 0;
  float pluck = 0.0f;
  std::mt19937 gen{99};
};

// A stand-in for the NAM standard input file: bursts of low-passed noise with
// silences between them, 48 kHz, 24 bit.
juce::File writeStandardInput(const juce::File& folder, double seconds) {
  const juce::File file = folder.getChildFile("standard-input.wav");
  const int n = static_cast<int>(seconds * kRate);
  juce::AudioBuffer<float> audio(1, n);
  std::mt19937 gen(1234);
  std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
  float state = 0.0f, peak = 0.0f;
  for (int i = 0; i < n; ++i) {
    const bool silent = std::fmod(i / kRate, 3.0) >= 2.0;
    state = 0.6f * state + 0.4f * noise(gen);
    const float v = silent ? 0.0f : state * 1.4f;
    audio.setSample(0, i, v);
    peak = std::max(peak, std::abs(v));
  }
  audio.applyGain(0.9f / peak);
  std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
  juce::WavAudioFormat wav;
  auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                .withSampleRate(kRate)
                                                .withNumChannels(1)
                                                .withBitsPerSample(24)
                                                .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::integral));
  if (writer != nullptr)
    writer->writeFromAudioSampleBuffer(audio, 0, n);
  return file;
}

// A stand-in for `python` that plays the trainer's part: it answers the
// version query, prints the lines the helper script prints (an epoch every
// time, the ESR falling), and then ends as asked.
//   running  stops after `limit` epochs and waits, so the page sees it mid-run
//   done     runs all 100 and writes the model of the repo's test files
//   fail     stops after `limit` epochs with a failed data check
juce::File writeTrainer(const juce::File& folder, const juce::String& behaviour, int limit, const juce::File& model) {
  juce::String body =
      "#!/bin/sh\n"
      "if [ \"$1\" = \"-c\" ]; then echo \"0.13.1\"; exit 0; fi\n"
      "outdir=\"\"\n"
      "while [ $# -gt 0 ]; do\n"
      "  case \"$1\" in --outdir) outdir=\"$2\"; shift;; esac\n"
      "  shift\n"
      "done\n"
      "mkdir -p \"$outdir\"\n"
      "e=1\n"
      "while [ $e -le " + juce::String(limit) + " ]; do\n"
      "  esr=$(awk \"BEGIN{printf \\\"%.4f\\\", 0.0081+0.0939*exp(-$e/8.5)}\")\n"
      "  echo \"Epoch $e: training\"\n"
      "  echo \"PEDALEIRA_EPOCH {\\\"epoch\\\": $e, \\\"epochs\\\": 100, \\\"esr\\\": $esr}\"\n"
      "  e=$((e+1))\n"
      "done\n";
  if (behaviour == "running") {
    body += "exec sleep 600\n";
  } else if (behaviour == "fail") {
    body += "echo 'Failed checks! The data checks of the trainer did not pass.'\n"
            "echo 'PEDALEIRA_RESULT {\"ok\": false, \"reason\": \"checks\", \"latency\": 12, \"checksPassed\": false}'\n"
            "exit 3\n";
  } else {
    body += "cp \"" + model.getFullPathName() + "\" \"$outdir/model.nam\"\n"
            "echo \"PEDALEIRA_RESULT {\\\"ok\\\": true, \\\"esr\\\": 0.0081, \\\"latency\\\": 200, \\\"checksPassed\\\": true, "
            "\\\"modelFile\\\": \\\"$outdir/model.nam\\\"}\"\n";
  }
  const juce::File script = folder.getChildFile("fake-python-" + behaviour);
  script.replaceWithText(body, false, false, "\n");
  script.setExecutePermission(true);
  return script;
}

struct Driver {
  EngineClient& client;
  TONE3000Processor& processor;
  juce::File scratch;
  CapturePage& page;
  SimDevice device;
  juce::String id;
  juce::String error;

  Driver(EngineClient& c, TONE3000Processor& p, const juce::File& s, CapturePage& pg) : client(c), processor(p), scratch(s), page(pg), device(p) {}

  juce::var command(const juce::String& name, const juce::var& args) {
    const EngineReply reply = client.run(name, args);
    if (!reply.ok && error.isEmpty())
      error = name + ": " + reply.errorCode + ": " + reply.errorMessage;
    return reply.value;
  }

  juce::var withId(std::initializer_list<std::pair<const char*, juce::var>> more) {
    juce::DynamicObject::Ptr args = new juce::DynamicObject();
    args->setProperty("id", id);
    for (const auto& [key, value] : more)
      args->setProperty(key, value);
    return juce::var(args.get());
  }

  void step(const juce::String& to) { command("capture.setStep", withId({{"step", to}})); }

  juce::File model() const { return juce::File(PEDALEIRA_SOURCE_DIR).getChildFile("test/files/a2-amp-test.nam"); }

  void wait(int ms) {
    const juce::uint32 end = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(ms);
    while (juce::Time::getMillisecondCounter() < end)
      juce::Thread::sleep(10);
  }

  void run(const juce::String& token) {
    const juce::String key = token.upToFirstOccurrenceOf(":", false, false);
    const juce::StringArray values = juce::StringArray::fromTokens(token.fromFirstOccurrenceOf(":", false, false), ":", "");
    const auto value = [&](int i, const juce::String& fallback = {}) { return i < values.size() ? values[i] : fallback; };

    if (key == "name") {
      page.source().setName(value(0));
    } else if (key == "source") {
      page.source().chooseSource(value(0).getIntValue());
    } else if (key == "new") {
      // new:<source>:<method>:<name>
      const juce::var created = command("capture.create", pedaleira::ui::object({{"name", value(2, "Harbor Crunch")},
                                                                                {"source", value(0, "ampHead")},
                                                                                {"method", value(1, "reamp")},
                                                                                {"gainTag", "crunch"},
                                                                                {"gearMake", "Northfield"},
                                                                                {"notes", "Master 6, gain 5, mids 7. Load box DI."}}));
      id = created["id"].toString();
      page.open(id);
    } else if (key == "goto") {
      for (const auto& to : values)
        step(to);
      page.open(id);
    } else if (key == "variant") {
      page.wiring().chooseVariant(value(0).getIntValue());
    } else if (key == "channels") {
      // channels:<send or dry>:<return or wet>
      const bool dryWet = page.currentSession()["method"].toString() == "dryWet";
      command("capture.setChannels", withId({{dryWet ? "dry" : "send", value(0).getIntValue()}, {dryWet ? "wet" : "returnIn", value(1).getIntValue()}}));
      page.open(id);
    } else if (key == "send") {
      command("capture.setLevels", withId({{"sendDb", value(0).getDoubleValue()}}));
      page.open(id);
    } else if (key == "tone") {
      // tone:<gain of the amp>: the tone plays and the readings settle.
      page.levels().setTone(true);
      device.loopback(300, value(0, "2.5").getFloatValue());
      page.levels().poll();
    } else if (key == "listen") {
      // listen:<gain>: dry/wet, the guitar played.
      page.levels().setTone(true);
      device.guitar(300, value(0, "2.5").getFloatValue());
      page.levels().poll();
    } else if (key == "noise") {
      command("capture.measureNoiseFloor", withId({{"seconds", 1.0}}));
      device.loopback(device.blocksFor(1.8), 2.5f);
      page.levels().poll();
    } else if (key == "input") {
      // input:none, input:wav (a stand-in the trainer would not recognise), input:missing
      juce::String path;
      if (value(0) == "wav")
        path = writeStandardInput(scratch, 12.0).getFullPathName();
      else if (value(0) == "missing")
        path = scratch.getChildFile("v3_0_0.wav").getFullPathName();
      command("capture.setSettings", pedaleira::ui::object({{"standardInputFile", path}}));
    } else if (key == "record") {
      // record:<percent>:<gain>: the recording starts and runs that far.
      command("capture.startRecording", withId({{"allowUnrecognisedInput", true}}));
      const double total = 12.0 + 0.75;
      const int blocks = device.blocksFor(total * value(0, "100").getDoubleValue() / 100.0);
      device.loopback(blocks, value(1, "2.5").getFloatValue());
      if (value(0, "100").getIntValue() >= 100) {
        for (int i = 0; i < 400; ++i) {
          const juce::var status = command("capture.recordingStatus", withId({}));
          if (status["state"].toString() == "complete" || status["state"].toString() == "failed")
            break;
          device.loopback(8, value(1, "2.5").getFloatValue());
          juce::Thread::sleep(10);
        }
      }
      page.open(id);
    } else if (key == "checks") {
      command("capture.runChecks", withId({}));
      page.open(id);
    } else if (key == "python") {
      // python:ok, python:none (no such Python) or python:default
      juce::String path;
      if (value(0) == "ok")
        path = writeTrainer(scratch, "done", 100, model()).getFullPathName();
      else if (value(0) == "none")
        path = scratch.getChildFile("no-such-python").getFullPathName();
      command("capture.setSettings", pedaleira::ui::object({{"pythonExecutable", path}}));
    } else if (key == "trainer") {
      page.checkTrainerNow();
    } else if (key == "train") {
      // train:<behaviour>:<epochs>: start the stand-in trainer and wait until it got that far.
      const juce::String behaviour = value(0, "running");
      const int epochs = value(1, "46").getIntValue();
      const juce::File script = writeTrainer(scratch, behaviour, epochs, model());
      command("capture.setSettings", pedaleira::ui::object({{"pythonExecutable", script.getFullPathName()}}));
      command("capture.startTraining", withId({{"overrideChecks", true}}));
      for (int i = 0; i < 200; ++i) {
        const juce::var status = command("capture.trainingStatus", withId({}));
        const juce::String state = status["state"].toString();
        if ((behaviour == "running" && static_cast<int>(status["epoch"]) >= epochs) || state == "done" || state == "failed")
          break;
        juce::Thread::sleep(50);
      }
      wait(100);
      command("capture.trainingStatus", withId({}));
      page.open(id);
    } else if (key == "cancel") {
      command("capture.cancelTraining", withId({}));
      page.open(id);
    } else if (key == "save") {
      command("capture.save", withId({}));
      page.open(id);
    } else if (key == "rig") {
      page.review().addToRig();
    } else if (key == "press") {
      if (value(0) == "primary")
        page.pressPrimary();
      else if (value(0) == "secondary")
        page.pressSecondary();
      else
        page.pressBack();
    } else if (key == "open") {
      page.open(id);
    } else {
      error = "unknown capture step '" + key + "'";
    }
  }
};

}  // namespace

juce::String driveCapture(EngineClient& client, TONE3000Processor& processor, const juce::File& scratch, CapturePage& page,
                          const juce::String& script) {
  Driver driver(client, processor, scratch, page);
  for (const auto& token : juce::StringArray::fromTokens(script, ";", "")) {
    driver.run(token);
    if (driver.error.isNotEmpty())
      return driver.error;
  }
  return {};
}

const char* captureStepsHelp() {
  return "name:<text> source:<n> new:<source>:<method>:<name> goto:<step>... variant:<n> channels:<a>:<b> send:<dB> tone:<gain> "
         "listen:<gain> noise input:<none|wav|missing> record:<percent>:<gain> checks python:<ok|none|default> trainer "
         "train:<running|done|fail>:<epochs> cancel save rig press:<primary|secondary|back> open";
}

}  // namespace pedaleira::ui::testbed

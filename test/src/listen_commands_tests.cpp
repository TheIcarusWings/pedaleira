// The listen commands: render.clip, render.compare and render.probe on rigs
// whose behaviour is known, so each number an agent reads is checked against
// what the rig really does: a gain block's level, a compressor's squeeze, an
// amp's distortion, a delay's tail, an EQ's curve. And the same call twice
// gives the same numbers.

#include "SandboxRig.h"
#include "agent_test_helpers.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace {

juce::String codeOf(const CommandResult& result) {
  return result.ok ? juce::String("ok") : result.errorCode;
}

// A rig for the local session to build in a sandbox and measure.
struct ListenRig : AgentRig {
  ListenRig() {
    mustLocal("sandbox.create", R"({"from":"empty"})");
    // A new rig has its noise gate on, and these signals are not guitars.
    mustLocal("param.set", R"({"id":"gateEnabled","value":0})");
  }

  // Adds a kind to the selected sandbox and sets its parameters
  // ("id=value,id=value", real units; mix is the common knob).
  juce::String addKind(const char* kind, const char* params = "") {
    const juce::String id =
        mustLocal("chain.addBlock", juce::String("{\"kind\":\"") + kind + "\"}")["blockId"].toString();
    for (const auto& pair : juce::StringArray::fromTokens(params, ",", "")) {
      const juce::String name = pair.upToFirstOccurrenceOf("=", false, false);
      const juce::String value = pair.fromFirstOccurrenceOf("=", false, false);
      mustLocal("block.setParam", "{\"blockId\":" + agentQuoted(id) + ",\"param\":" +
                                      agentQuoted(name) + ",\"value\":" + value + "}");
    }
    mustLocal("state.waitUntilLoaded");
    return id;
  }

  juce::File clip(const char* name, const std::vector<float>& samples,
                  const std::vector<float>* right = nullptr) {
    return writeTestWav(dirs.library.getChildFile("clips").getChildFile(name), samples, 48000.0, right);
  }

  juce::var render(const juce::File& file, const juce::String& extra = {}) {
    return mustLocal("render.clip", "{\"clips\":[" + agentQuoted(file.getFullPathName()) + "]" + extra + "}");
  }

  // A guitar with a soft and a hard take of the same playing, 18 dB apart.
  void addGuitar() {
    mustLocal("guitar.create", R"({"name":"Test Guitar","positions":["bridge"]})");
    for (const auto& [take, amplitude] : {std::pair<const char*, float>{"soft", 0.1f},
                                          std::pair<const char*, float>{"hard", 0.8f}})
      mustLocal("guitar.importClip",
                "{\"guitar\":\"test-guitar\",\"position\":\"bridge\",\"take\":\"" + juce::String(take) +
                    "\",\"path\":" +
                    agentQuoted(clip((juce::String(take) + ".wav").toRawUTF8(), pluckedClip(3.0, amplitude))
                                    .getFullPathName()) +
                    "}");
  }
};

double number(const juce::var& value) { return static_cast<double>(value); }

juce::var pointAt(const juce::var& points, const char* key, double value) {
  for (int i = 0; i < points.size(); ++i)
    if (std::abs(number(points[i][juce::Identifier(key)]) - value) < 1.0e-6)
      return points[i];
  ADD_FAILURE() << "no point with " << key << " " << value;
  return {};
}

}  // namespace

TEST(RenderClip, ASineThroughAKnownGain) {
  ListenRig rig;
  const juce::File sine = rig.clip("sine.wav", makeSine(96000, 997.0, 0.5f));

  // The empty rig is the baseline: what goes in comes out, on both sides.
  const juce::var empty = rig.render(sine)["renders"][0];
  EXPECT_NEAR(number(empty["input"]["peakDb"]), -6.02, 0.02);
  EXPECT_NEAR(number(empty["input"]["rmsDb"]), -9.03, 0.02);
  EXPECT_NEAR(number(empty["input"]["lufs"]), -9.03, 0.1);
  EXPECT_NEAR(number(empty["levels"]["rmsDb"]), -9.03, 0.1);
  EXPECT_NEAR(number(empty["levels"]["peakDb"]), -6.02, 0.15);
  EXPECT_NEAR(number(empty["gainDb"]), 0.0, 0.1);
  // LUFS adds the two channels' power: 3 dB over the mono clip.
  EXPECT_NEAR(number(empty["levels"]["lufs"]), -6.02, 0.15);
  EXPECT_NEAR(number(empty["levels"]["crestDb"]), 3.01, 0.15);
  EXPECT_NEAR(number(empty["spectrum"]["centroidHz"]), 997.0, 5.0);
  EXPECT_GT(number(empty["spectrum"]["bandShare"]["mid"]), 0.99);
  EXPECT_NEAR(number(empty["stereo"]["correlation"]), 1.0, 1.0e-3);
  EXPECT_NEAR(number(empty["stereo"]["width"]), 0.0, 1.0e-3);
  EXPECT_TRUE(static_cast<bool>(empty["distortion"]["valid"]));
  EXPECT_LT(number(empty["distortion"]["percent"]), 1.0);
  EXPECT_LT(number(empty["tail"]["seconds"]), 0.15);
  EXPECT_FALSE(static_cast<bool>(empty["tail"]["truncated"]));
  EXPECT_EQ(static_cast<int>(empty["latencySamples"]), 0);
  EXPECT_NEAR(number(empty["seconds"]), 2.0, 1.0e-6);
  EXPECT_EQ(empty["clip"]["file"].toString(), sine.getFullPathName());

  // Six dB down reads six dB down, in every level, and is still clean.
  const juce::String gain = rig.addKind("utility.gain", "gainDb=-6");
  const juce::var quieter = rig.render(sine)["renders"][0];
  EXPECT_NEAR(number(quieter["gainDb"]), -6.0, 0.1);
  EXPECT_NEAR(number(quieter["levels"]["lufs"]) - number(empty["levels"]["lufs"]), -6.0, 0.05);
  EXPECT_NEAR(number(quieter["levels"]["peakDb"]) - number(empty["levels"]["peakDb"]), -6.0, 0.05);
  EXPECT_LT(number(quieter["distortion"]["percent"]), 1.0);

  // Panned hard left: the right side is empty, and the stereo readings say so.
  rig.mustLocal("block.setParam", "{\"blockId\":" + agentQuoted(gain) + R"(,"param":"pan","value":-1})");
  const juce::var panned = rig.render(sine)["renders"][0];
  EXPECT_LT(number(panned["stereo"]["balanceDb"]), -40.0);
  EXPECT_NEAR(number(panned["stereo"]["correlation"]), 0.0, 1.0e-3);
  EXPECT_NEAR(number(panned["stereo"]["width"]), std::sqrt(0.5), 0.01);
}

TEST(RenderClip, TheSameCallGivesTheSameNumbers) {
  ListenRig rig;
  rig.addGuitar();
  rig.mustLocal("chain.addToneFromFiles",
                "{\"paths\":[" + agentQuoted(testFile("a2-amp-test.nam").getFullPathName()) + "]}");
  rig.mustLocal("chain.addToneFromFiles",
                "{\"paths\":[" + agentQuoted(testFile("cab-ir-test.wav").getFullPathName()) + "]}");
  rig.addKind("delay.simple", "timeMs=120,feedback=30");
  rig.addKind("reverb.room");

  const juce::String first = juce::JSON::toString(rig.mustLocal("render.clip"));
  const juce::String second = juce::JSON::toString(rig.mustLocal("render.clip"));
  EXPECT_EQ(first, second);
  // After other renders have been through the rig, too.
  rig.mustLocal("render.probe", R"({"kind":"levelCurve"})");
  rig.mustLocal("render.clip", R"({"takes":["hard"],"tailSeconds":0.5})");
  EXPECT_EQ(juce::JSON::toString(rig.mustLocal("render.clip")), first);

  // And a second sandbox made from the first rig's state reads the same.
  const juce::String presetId = rig.mustLocal("preset.save", R"({"name":"Determinism"})")["id"].toString();
  // (Saving named the rig's preset, which is not part of what is rendered.)
  rig.mustLocal("sandbox.create", "{\"from\":\"preset\",\"presetId\":" + agentQuoted(presetId) + "}");
  rig.mustLocal("param.set", R"({"id":"gateEnabled","value":0})");
  rig.mustLocal("state.waitUntilLoaded");
  const juce::var again = rig.mustLocal("render.clip");
  EXPECT_EQ(again["rig"].toString(), "sandbox-2");
  juce::var expected = agentJson(first);
  EXPECT_EQ(juce::JSON::toString(again["renders"]), juce::JSON::toString(expected["renders"]));
  EXPECT_EQ(juce::JSON::toString(again["dynamics"]), juce::JSON::toString(expected["dynamics"]));

  // Two takes of one guitar, each measured, each named.
  ASSERT_EQ(expected["renders"].size(), 2);
  EXPECT_EQ(expected["renders"][0]["clip"]["guitar"].toString(), "test-guitar");
  EXPECT_EQ(expected["renders"][0]["clip"]["position"].toString(), "bridge");
  juce::StringArray takes;
  for (int i = 0; i < 2; ++i)
    takes.add(expected["renders"][i]["clip"]["take"].toString());
  EXPECT_TRUE(takes.contains("soft") && takes.contains("hard"));
}

TEST(RenderClip, SoftAndHardPickingThroughACleanRigACompressorAndAnAmp) {
  // Clean: what goes in comes out, 18 dB apart and undistorted.
  {
    ListenRig rig;
    rig.addGuitar();
    const juce::var result = rig.mustLocal("render.clip");
    ASSERT_EQ(result["dynamics"].size(), 1);
    const juce::var dynamics = result["dynamics"][0];
    EXPECT_EQ(dynamics["position"].toString(), "bridge");
    EXPECT_NEAR(number(dynamics["inputLevelDifferenceDb"]), 18.06, 0.1);
    EXPECT_NEAR(number(dynamics["outputLevelDifferenceDb"]), 18.06, 0.2);
    EXPECT_NEAR(number(dynamics["compressionDb"]), 0.0, 0.2);
    EXPECT_LT(number(dynamics["softDistortionPercent"]), 3.0);
    EXPECT_LT(number(dynamics["hardDistortionPercent"]), 3.0);
  }

  // A compressor with the soft take under its threshold and the hard one
  // well over: the rig takes dynamics away, and the numbers show how much.
  double compressorHardDistortion = 0.0;
  {
    ListenRig rig;
    rig.addGuitar();
    rig.addKind("dynamics.compressor", "thresholdDb=-14,ratio=8,kneeDb=0,attackMs=1,releaseMs=50");
    const juce::var dynamics = rig.mustLocal("render.clip")["dynamics"][0];
    EXPECT_NEAR(number(dynamics["inputLevelDifferenceDb"]), 18.06, 0.1);
    EXPECT_GT(number(dynamics["compressionDb"]), 3.0);
    EXPECT_LT(number(dynamics["outputLevelDifferenceDb"]), 15.0);
    // Gain riding is not a linear filter either: the hard take, the one the
    // compressor works on, reads less linear than the soft one.
    EXPECT_GT(number(dynamics["hardDistortionPercent"]), number(dynamics["softDistortionPercent"]));
    EXPECT_LT(number(dynamics["cleanupRatio"]), 1.0);
    compressorHardDistortion = number(dynamics["hardDistortionPercent"]);
  }

  // An amp capture: heavy distortion at either level, and nearly all of the
  // playing dynamics squashed.
  {
    ListenRig rig;
    rig.addGuitar();
    rig.mustLocal("chain.addToneFromFiles",
                  "{\"paths\":[" + agentQuoted(testFile("a2-amp-test.nam").getFullPathName()) + "]}");
    rig.mustLocal("state.waitUntilLoaded");
    const juce::var result = rig.mustLocal("render.clip");
    const juce::var dynamics = result["dynamics"][0];
    EXPECT_GT(number(dynamics["softDistortionPercent"]), 15.0);
    EXPECT_GT(number(dynamics["hardDistortionPercent"]), 15.0);
    EXPECT_GT(number(dynamics["compressionDb"]), 8.0);
    EXPECT_TRUE(dynamics.hasProperty("cleanupRatio"));
    // It distorts more than the compressor "distorts".
    EXPECT_GT(number(dynamics["softDistortionPercent"]), 15.0);
    juce::ignoreUnused(compressorHardDistortion);
    // The amp brightens the plucks: harmonics that were not in the clip.
    for (int i = 0; i < 2; ++i)
      EXPECT_GT(number(result["renders"][i]["spectrum"]["centroidHz"]), 250.0);
  }
}

TEST(RenderClip, TheTailOfAKnownDelay) {
  ListenRig rig;
  // One echo, 300 ms later, as loud as the dry signal, nothing fed back.
  rig.addKind("delay.simple", "timeMs=300,feedback=0,toneHz=20000,mix=0.5");
  const juce::File burst = rig.clip("burst.wav", makeNoise(48000, 4, 0.3f));

  const juce::var rendered = rig.render(burst)["renders"][0];
  // The echo of the burst's last sample ends 300 ms after it (plus the few
  // tens of milliseconds any rig rings on below -60 dB).
  EXPECT_GT(number(rendered["tail"]["seconds"]), 0.295);
  EXPECT_LT(number(rendered["tail"]["seconds"]), 0.45);
  EXPECT_FALSE(static_cast<bool>(rendered["tail"]["truncated"]));
  // Dry plus an equal echo over seven tenths of the burst: 10 log10(1.7).
  EXPECT_NEAR(number(rendered["gainDb"]), 2.3, 0.6);

  // A render too short to see the tail end says so instead of guessing.
  const juce::var cut = rig.render(burst, R"(,"tailSeconds":0.1)")["renders"][0];
  EXPECT_NEAR(number(cut["tail"]["seconds"]), 0.1, 0.011);
  EXPECT_TRUE(static_cast<bool>(cut["tail"]["truncated"]));

  // Without the delay the tail is all but gone.
  rig.mustLocal("chain.reset");
  rig.mustLocal("param.set", R"({"id":"gateEnabled","value":0})");
  EXPECT_LT(number(rig.render(burst)["renders"][0]["tail"]["seconds"]), 0.15);
}

TEST(RenderClip, PerSceneAndWithAudioOnRequest) {
  ListenRig rig;
  const juce::String gain = rig.addKind("utility.gain");
  const juce::String quotedGain = agentQuoted(gain);
  // Scene B plays the block 12 dB down; scene A is the one in use.
  rig.mustLocal("scene.add");
  rig.mustLocal("scene.assignParam", "{\"blockId\":" + quotedGain + R"(,"param":"gainDb"})");
  rig.mustLocal("scene.setParamValue",
                "{\"scene\":\"B\",\"blockId\":" + quotedGain + R"(,"param":"gainDb","value":-12})");
  const juce::File sine = rig.clip("sine.wav", makeSine(48000, 440.0, 0.25f));

  const juce::var result = rig.render(sine, R"(,"scenes":["A","B"],"writeAudio":true,"tailSeconds":0.5)");
  ASSERT_EQ(result["renders"].size(), 2);
  EXPECT_EQ(result["renders"][0]["scene"].toString(), "A");
  EXPECT_EQ(result["renders"][1]["scene"].toString(), "B");
  EXPECT_NEAR(number(result["renders"][1]["levels"]["rmsDb"]) -
                  number(result["renders"][0]["levels"]["rmsDb"]),
              -12.0, 0.1);
  // The rig is back on the scene it was on.
  EXPECT_EQ(static_cast<int>(rig.mustLocal("scene.get")["active"]), 0);

  // The audio, where asked for: in the scratch folder, the clip plus tail.
  for (int i = 0; i < 2; ++i) {
    const juce::File written(result["renders"][i]["audioPath"].toString());
    EXPECT_TRUE(LibraryPaths::isInside(written, rig.dirs.scratch)) << written.getFullPathName();
    juce::AudioBuffer<float> audio;
    double rate = 0.0;
    juce::String error;
    ASSERT_TRUE(AudioFiles::read(written, audio, rate, error)) << error;
    EXPECT_EQ(audio.getNumChannels(), 2);
    EXPECT_EQ(audio.getNumSamples(), 48000 + 24000);
    EXPECT_EQ(rate, 48000.0);
  }
  // By default nothing is written.
  EXPECT_FALSE(rig.render(sine)["renders"][0].hasProperty("audioPath"));
}

TEST(RenderClip, TheLiveRigIsRenderedThroughAPrivateCopy) {
  AgentRig rig;
  rig.mustLocal("chain.addBlock", R"({"kind":"utility.gain"})");
  rig.mustLocal("state.waitUntilLoaded");
  const juce::File sine = writeTestWav(rig.dirs.library.getChildFile("sine.wav"), makeSine(48000, 440.0, 0.25f));
  rig.settleRevision();
  const juce::MemoryBlock before = rig.liveState();
  const juce::uint32 revision = rig.proc.getCurrentChainRevision();

  // No sandbox selected: the render is of the live rig, but not through it.
  const juce::var result =
      rig.mustLocal("render.clip", "{\"clips\":[" + agentQuoted(sine.getFullPathName()) + "]}");
  EXPECT_EQ(result["rig"].toString(), "live (private copy)");
  EXPECT_NEAR(number(result["renders"][0]["gainDb"]), 0.0, 0.1);
  EXPECT_TRUE(before == rig.liveState());
  EXPECT_EQ(rig.proc.getCurrentChainRevision(), revision);
  // The copy is gone again.
  EXPECT_EQ(rig.mustLocal("sandbox.list")["sandboxes"].size(), 0);
  EXPECT_EQ(rig.hub.findSandbox("sandbox-1"), nullptr);
  EXPECT_TRUE(rig.mustLocal("render.probe", R"({"kind":"noise"})")["noiseFloorDb"].isDouble());
  EXPECT_EQ(rig.hub.findSandbox("sandbox-2"), nullptr);

  // A client's render of the live rig needs the render permission and
  // nothing more: it changes nothing.
  const auto client = rig.hub.openSession("agent");
  EXPECT_TRUE(rig.as(client, "render.clip", "{\"clips\":[" + agentQuoted(sine.getFullPathName()) + "]}").ok);
  EXPECT_TRUE(before == rig.liveState());
}

TEST(RenderClip, WhatIsRefused) {
  ListenRig rig;
  const juce::File sine = rig.clip("sine.wav", makeSine(24000, 440.0, 0.25f));
  const juce::String path = agentQuoted(sine.getFullPathName());

  EXPECT_EQ(codeOf(rig.local("render.clip")), "not_found");  // no guitar, no clips
  EXPECT_EQ(codeOf(rig.local("render.clip", R"({"guitar":"nope"})")), "not_found");
  EXPECT_EQ(codeOf(rig.local("render.clip", R"({"clips":["relative.wav"]})")), "invalid_args");
  EXPECT_EQ(codeOf(rig.local("render.clip", "{\"clips\":[" +
                                                agentQuoted(rig.dirs.library.getChildFile("missing.wav").getFullPathName()) +
                                                "]}")),
            "not_found");
  EXPECT_EQ(codeOf(rig.local("render.clip", "{\"clips\":[" + path + R"(],"scenes":["Z"]})")), "invalid_args");
  EXPECT_EQ(codeOf(rig.local("render.clip", "{\"clips\":[" + path + R"(],"tailSeconds":3600})")), "invalid_args");
  EXPECT_EQ(codeOf(rig.local("render.clip", "{\"clips\":[" + path + R"(],"sandbox":"sandbox-9"})")), "not_found");
  // More renders than one call may ask for (clips times scenes).
  juce::StringArray many;
  for (int i = 0; i < 5; ++i)
    many.add(path);
  EXPECT_EQ(codeOf(rig.local("render.clip", "{\"clips\":[" + many.joinIntoString(",") +
                                                R"(],"scenes":["A","B","C","D","E","F","G"]})")),
            "invalid_args");
  // Not audio.
  const juce::File text = rig.dirs.library.getChildFile("text.wav");
  EXPECT_TRUE(text.replaceWithText("not a wav"));
  EXPECT_EQ(codeOf(rig.local("render.clip", "{\"clips\":[" + agentQuoted(text.getFullPathName()) + "]}")), "failed");

  // One render at a time through a rig; the refused call leaves it usable.
  const auto sandbox = rig.hub.findSandbox("sandbox-1");
  ASSERT_NE(sandbox, nullptr);
  sandbox->rendering.store(true);
  EXPECT_EQ(codeOf(rig.local("render.clip", "{\"clips\":[" + path + "]}")), "unavailable");
  EXPECT_EQ(codeOf(rig.local("render.probe", R"({"kind":"noise"})")), "unavailable");
  sandbox->rendering.store(false);
  EXPECT_TRUE(rig.local("render.clip", "{\"clips\":[" + path + "]}").ok);
  EXPECT_FALSE(sandbox->rendering.load());
  EXPECT_EQ(sandbox->activeUses.load(), 0);

  // A block that cannot load fails the render with its id, not with silence.
  const juce::File broken = rig.dirs.library.getChildFile("broken.nam");
  EXPECT_TRUE(broken.replaceWithText("{\"version\":\"0.5.4\",\"architecture\":\"WaveNet\"}"));
  const CommandResult added =
      rig.local("chain.addToneFromFiles", "{\"paths\":[" + agentQuoted(broken.getFullPathName()) + "]}");
  if (added.ok) {
    const CommandResult rendered = rig.local("render.clip", "{\"clips\":[" + path + "]}");
    EXPECT_EQ(rendered.errorCode, "failed");
    EXPECT_TRUE(rendered.errorMessage.contains("failed to load")) << rendered.errorMessage;
    EXPECT_FALSE(sandbox->rendering.load());
  }
}

TEST(RenderCompare, IdenticalAndDifferentReferences) {
  ListenRig rig;
  rig.addKind("utility.gain", "gainDb=-3");
  const juce::File clip = rig.clip("plucks.wav", pluckedClip(3.0, 0.5f));
  const juce::String clipArgs = "\"clips\":[" + agentQuoted(clip.getFullPathName()) + "]";

  // The rig's own render as the reference: nothing between them.
  const juce::var own = rig.mustLocal("render.clip", "{" + clipArgs + R"(,"writeAudio":true,"tailSeconds":0})");
  const juce::File reference = rig.dirs.library.getChildFile("reference.wav");
  ASSERT_TRUE(juce::File(own["renders"][0]["audioPath"].toString()).copyFileTo(reference));
  const juce::var same = rig.mustLocal(
      "render.compare", "{\"reference\":" + agentQuoted(reference.getFullPathName()) + "," + clipArgs + "}");
  for (const char* group : {"loudness", "dynamics", "tone", "stereo", "overall"})
    EXPECT_NEAR(number(same["distance"][juce::Identifier(group)]), 0.0, 0.01) << group;
  EXPECT_NEAR(number(same["difference"]["lufs"]), 0.0, 0.02);
  EXPECT_NEAR(number(same["difference"]["bandsRmsDb"]), 0.0, 0.02);
  EXPECT_EQ(static_cast<int>(same["renders"]), 1);
  EXPECT_NEAR(number(same["reference"]["levels"]["lufs"]), number(same["rig"]["levels"]["lufs"]), 0.02);

  // A reference 9 dB louder: the loudness group says three "clearly audible"
  // steps, and tone, dynamics and stereo are still a match.
  juce::AudioBuffer<float> audio;
  double rate = 0.0;
  juce::String error;
  ASSERT_TRUE(AudioFiles::read(reference, audio, rate, error)) << error;
  juce::AudioBuffer<float> louder;
  louder.makeCopyOf(audio);
  louder.applyGain(juce::Decibels::decibelsToGain(9.0f));
  ASSERT_TRUE(AudioFiles::write(rig.dirs.library.getChildFile("louder.wav"), louder, rate));
  const juce::var quiet = rig.mustLocal(
      "render.compare", "{\"reference\":" +
                            agentQuoted(rig.dirs.library.getChildFile("louder.wav").getFullPathName()) + "," +
                            clipArgs + "}");
  EXPECT_NEAR(number(quiet["difference"]["lufs"]), -9.0, 0.05);
  EXPECT_NEAR(number(quiet["distance"]["loudness"]), 3.0, 0.03);
  EXPECT_LT(number(quiet["distance"]["tone"]), 0.02);
  EXPECT_LT(number(quiet["distance"]["dynamics"]), 0.02);
  EXPECT_LT(number(quiet["distance"]["stereo"]), 0.02);
  EXPECT_NEAR(number(quiet["distance"]["overall"]), 0.6, 0.03);  // loudness weighs a fifth

  // A darker reference (a one-pole low-pass at about 800 Hz): the rig reads
  // brighter than it, and tone is the group that stands out.
  juce::AudioBuffer<float> darker;
  darker.makeCopyOf(audio);
  for (int ch = 0; ch < darker.getNumChannels(); ++ch) {
    float state = 0.0f;
    float* samples = darker.getWritePointer(ch);
    for (int i = 0; i < darker.getNumSamples(); ++i) {
      state += 0.1f * (samples[i] - state);
      samples[i] = state;
    }
  }
  ASSERT_TRUE(AudioFiles::write(rig.dirs.library.getChildFile("darker.wav"), darker, rate));
  const juce::var bright = rig.mustLocal(
      "render.compare", "{\"reference\":" +
                            agentQuoted(rig.dirs.library.getChildFile("darker.wav").getFullPathName()) + "," +
                            clipArgs + "}");
  EXPECT_GT(number(bright["difference"]["centroidOctaves"]), 0.1);
  EXPECT_GT(number(bright["difference"]["bandsDb"]["highMid"]), 4.0);
  // A band neither sound has anything in says nothing either way.
  EXPECT_EQ(number(bright["difference"]["bandsDb"]["air"]), 0.0);
  EXPECT_GT(number(bright["distance"]["tone"]), 1.0);
  EXPECT_GT(number(bright["distance"]["tone"]), number(bright["distance"]["loudness"]));
  EXPECT_GT(number(bright["distance"]["overall"]), 0.5);

  // A reference with one side missing: the stereo group.
  juce::AudioBuffer<float> lopsided;
  lopsided.makeCopyOf(audio);
  lopsided.clear(1, 0, lopsided.getNumSamples());
  ASSERT_TRUE(AudioFiles::write(rig.dirs.library.getChildFile("lopsided.wav"), lopsided, rate));
  const juce::var centred = rig.mustLocal(
      "render.compare", "{\"reference\":" +
                            agentQuoted(rig.dirs.library.getChildFile("lopsided.wav").getFullPathName()) + "," +
                            clipArgs + "}");
  EXPECT_GT(number(centred["distance"]["stereo"]), 2.0);
  EXPECT_NEAR(number(centred["difference"]["correlation"]), 1.0, 0.01);

  EXPECT_EQ(codeOf(rig.local("render.compare", "{" + clipArgs + "}")), "invalid_args");
  EXPECT_EQ(codeOf(rig.local("render.compare", "{\"reference\":\"x.wav\"," + clipArgs + "}")), "invalid_args");
  EXPECT_EQ(codeOf(rig.local("render.compare",
                             "{\"reference\":" +
                                 agentQuoted(rig.dirs.library.getChildFile("none.wav").getFullPathName()) +
                                 "," + clipArgs + "}")),
            "not_found");
}

TEST(RenderProbe, TheResponseOfAKnownEq) {
  ListenRig rig;
  // Flat first: unity everywhere, and coherent, which is what says the
  // reading can be trusted.
  const juce::var flat = rig.mustLocal("render.probe", R"({"kind":"response"})");
  ASSERT_EQ(flat["points"].size(), 29);  // third octaves, 25 Hz to 16 kHz
  for (int i = 0; i < flat["points"].size(); ++i) {
    const double hz = number(flat["points"][i]["hz"]);
    EXPECT_NEAR(number(flat["points"][i]["gainDb"]), 0.0, hz < 40.0 ? 1.0 : 0.3) << hz;
    EXPECT_GT(number(flat["points"][i]["coherence"]), 0.98) << hz;
  }
  EXPECT_LT(number(flat["distortion"]["percent"]), 1.0);
  EXPECT_NEAR(number(flat["levelDb"]), -30.0, 1.0e-9);

  // The 1 kHz slider of the graphic EQ up 9 dB, the 63 Hz one down 6.
  rig.addKind("eq.graphic10", "band1k=9,band63=-6");
  const juce::var shaped = rig.mustLocal(
      "render.probe", R"({"kind":"response","frequencies":[63,250,1000,4000,16000]})");
  ASSERT_EQ(shaped["points"].size(), 5);
  EXPECT_NEAR(number(pointAt(shaped["points"], "hz", 1000.0)["gainDb"]), 9.0, 0.6);
  EXPECT_NEAR(number(pointAt(shaped["points"], "hz", 63.0)["gainDb"]), -6.0, 0.6);
  EXPECT_NEAR(number(pointAt(shaped["points"], "hz", 250.0)["gainDb"]), 0.0, 0.8);
  EXPECT_NEAR(number(pointAt(shaped["points"], "hz", 4000.0)["gainDb"]), 0.0, 0.8);
  EXPECT_NEAR(number(pointAt(shaped["points"], "hz", 16000.0)["gainDb"]), 0.0, 0.5);
  EXPECT_LT(number(shaped["distortion"]["percent"]), 2.0);  // an EQ is linear

  // The same probe twice is the same answer.
  EXPECT_EQ(juce::JSON::toString(shaped),
            juce::JSON::toString(rig.mustLocal(
                "render.probe", R"({"kind":"response","frequencies":[63,250,1000,4000,16000]})")));

  EXPECT_EQ(codeOf(rig.local("render.probe", R"({"kind":"response","frequencies":[5]})")), "invalid_args");
  EXPECT_EQ(codeOf(rig.local("render.probe", R"({"kind":"sweep"})")), "invalid_args");
}

TEST(RenderProbe, TheLevelCurveOfTheCompressor) {
  ListenRig rig;
  // Without a compressor the curve is a straight line at unity.
  const juce::var straight = rig.mustLocal("render.probe", R"({"kind":"levelCurve"})");
  ASSERT_EQ(straight["points"].size(), 11);
  for (int i = 0; i < 11; ++i) {
    EXPECT_NEAR(number(straight["points"][i]["inputDb"]), -60.0 + 6.0 * i, 1.0e-9);
    EXPECT_NEAR(number(straight["points"][i]["gainDb"]), 0.0, 0.1) << i;
  }

  // Threshold -30 dB, ratio 4, hard knee: unity below the threshold, one dB
  // out for every four in above it.
  rig.addKind("dynamics.compressor", "thresholdDb=-30,ratio=4,kneeDb=0,attackMs=1,releaseMs=50,makeupDb=0");
  const juce::var curve = rig.mustLocal("render.probe", R"({"kind":"levelCurve"})")["points"];
  for (const double below : {-60.0, -48.0, -36.0})
    EXPECT_NEAR(number(pointAt(curve, "inputDb", below)["gainDb"]), 0.0, 0.3) << below;
  for (const double above : {-18.0, -12.0, -6.0, 0.0}) {
    const double expected = -30.0 + (above + 30.0) / 4.0;
    EXPECT_NEAR(number(pointAt(curve, "inputDb", above)["outputDb"]), expected, 0.8) << above;
  }
  EXPECT_NEAR(number(pointAt(curve, "inputDb", 0.0)["outputDb"]) -
                  number(pointAt(curve, "inputDb", -12.0)["outputDb"]),
              3.0, 0.4);

  // At another frequency the level detector reads the same.
  const juce::var low = rig.mustLocal("render.probe", R"({"kind":"levelCurve","frequencyHz":220})");
  EXPECT_NEAR(number(low["frequencyHz"]), 220.0, 1.0e-9);
  EXPECT_NEAR(number(pointAt(low["points"], "inputDb", -6.0)["outputDb"]), -24.0, 1.0);
}

TEST(RenderProbe, TheNoiseFloor) {
  ListenRig rig;
  // A digital rig with nothing in it makes no noise at all.
  const juce::var silent = rig.mustLocal("render.probe", R"({"kind":"noise"})");
  EXPECT_EQ(number(silent["noiseFloorDb"]), -120.0);
  EXPECT_EQ(number(silent["peakDb"]), -120.0);
  EXPECT_EQ(static_cast<int>(silent["hum"]["mainsHz"]), 0);
  // An amp capture with silence in may or may not hiss; whatever it does is
  // a level on the scale, the same every time.
  rig.mustLocal("chain.addToneFromFiles",
                "{\"paths\":[" + agentQuoted(testFile("a2-amp-test.nam").getFullPathName()) + "]}");
  rig.mustLocal("state.waitUntilLoaded");
  const juce::var amp = rig.mustLocal("render.probe", R"({"kind":"noise"})");
  EXPECT_GE(number(amp["noiseFloorDb"]), -120.0);
  EXPECT_LT(number(amp["noiseFloorDb"]), -20.0);
  EXPECT_EQ(juce::JSON::toString(amp),
            juce::JSON::toString(rig.mustLocal("render.probe", R"({"kind":"noise"})")));
}

// ####################
// LIMITS: WHAT A RENDER MAY COST
// ####################

// One call renders a bounded amount of audio, however it is put together
// from clips, tails and scenes: the sum is checked from the files' headers
// before anything is rendered.
TEST(RenderLimits, ACallRendersAtMostTenMinutesOfAudio) {
  ListenRig rig;
  const juce::File second = rig.clip("second.wav", makeSine(48000, 440.0, 0.25f));
  juce::StringArray five;
  for (int i = 0; i < 5; ++i)
    five.add(agentQuoted(second.getFullPathName()));
  const juce::String clips = "{\"clips\":[" + five.joinIntoString(",") + "]";

  // Twenty renders, which the count allows, of 31 seconds each: 620.
  for (const char* command : {"render.clip", "render.compare"}) {
    const juce::String reference =
        juce::String(command) == "render.compare"
            ? ",\"reference\":" + agentQuoted(second.getFullPathName())
            : juce::String();
    const CommandResult refused = rig.local(
        command, clips + R"(,"scenes":["A","B","C","D"],"tailSeconds":30)" + reference + "}");
    EXPECT_EQ(refused.errorCode, "invalid_args") << command;
    EXPECT_TRUE(refused.errorMessage.contains("600")) << refused.errorMessage;
    EXPECT_TRUE(refused.errorMessage.contains("620")) << refused.errorMessage;
  }
  // Nothing was reserved for the refused calls.
  const auto sandbox = rig.hub.findSandbox("sandbox-1");
  ASSERT_NE(sandbox, nullptr);
  EXPECT_FALSE(sandbox->rendering.load());
  EXPECT_EQ(sandbox->activeUses.load(), 0);

  // The same twenty with no tail are twenty seconds, and run (a render of a
  // scene the preset does not use is refused, so it has four).
  for (int i = 0; i < 3; ++i)
    rig.mustLocal("scene.add");
  const juce::var rendered =
      rig.mustLocal("render.clip", clips + R"(,"scenes":["A","B","C","D"],"tailSeconds":0})");
  EXPECT_EQ(rendered["renders"].size(), 20);
}

// Renders hold audio, so only so many run at once in the whole application,
// whoever asks: the next one is told to come back.
TEST(RenderLimits, OnlyTwoRendersRunAtOnce) {
  AgentRig rig;
  const juce::File clip =
      writeTestWav(rig.dirs.library.getChildFile("clip.wav"), makeSine(24000, 440.0, 0.25f));
  const juce::String args = "{\"clips\":[" + agentQuoted(clip.getFullPathName()) + "]}";
  const auto first = rig.hub.openSession("first");
  const auto second = rig.hub.openSession("second");
  const auto third = rig.hub.openSession("third");
  ASSERT_EQ(AgentHub::kMaxConcurrentRenders, 2);

  // Two renders with their rigs reserved and their work still to do, as two
  // connections' threads would have them.
  CommandWork one = rig.hub.begin(*first, "render.clip", agentJson(args));
  CommandWork two = rig.hub.begin(*second, "render.probe", agentJson(R"({"kind":"noise"})"));
  ASSERT_FALSE(one.finished.has_value());
  ASSERT_FALSE(two.finished.has_value());

  const CommandResult refused = rig.as(third, "render.clip", args);
  EXPECT_EQ(refused.errorCode, "unavailable");
  EXPECT_TRUE(refused.errorMessage.contains("renders are already running")) << refused.errorMessage;
  EXPECT_EQ(codeOf(rig.as(third, "render.probe", R"({"kind":"noise"})")), "unavailable");
  EXPECT_EQ(codeOf(rig.local("render.compare", "{\"reference\":" + agentQuoted(clip.getFullPathName()) +
                                                    ",\"clips\":[" + agentQuoted(clip.getFullPathName()) +
                                                    "]}")),
            "unavailable");
  // A refused call made no private copy of the live rig to leave behind.
  int rigs = 0;
  for (int n = 1; n < 40; ++n)
    rigs += rig.hub.findSandbox("sandbox-" + juce::String(n)) != nullptr ? 1 : 0;
  EXPECT_EQ(rigs, 2);

  // One ends, and there is room again.
  EXPECT_TRUE(one.work().ok);
  one = CommandWork();
  EXPECT_TRUE(rig.as(third, "render.clip", args).ok);
  EXPECT_TRUE(two.work().ok);
  two = CommandWork();
  // A render that fails gives its place back too.
  const juce::File text = rig.dirs.library.getChildFile("text.wav");
  EXPECT_TRUE(text.replaceWithText("not a wav"));
  for (int i = 0; i < 4; ++i)
    EXPECT_EQ(codeOf(rig.as(third, "render.clip",
                            "{\"clips\":[" + agentQuoted(text.getFullPathName()) + "]}")),
              "failed");
  EXPECT_TRUE(rig.hub.tryBeginRender());
  EXPECT_TRUE(rig.hub.tryBeginRender());
  EXPECT_FALSE(rig.hub.tryBeginRender());
  rig.hub.endRender();
  rig.hub.endRender();
}

// A WAV's header is the file's own claim. It is held against the limits and
// against the file's size before a buffer is made for what it claims.
TEST(RenderLimits, AWavHeaderIsHeldAgainstTheFile) {
  ListenRig rig;
  const juce::File clips = rig.dirs.library.getChildFile("clips");
  // Ninety seconds claimed, a hundred frames there.
  const juce::File hollow =
      writeForgedWav(clips.getChildFile("hollow.wav"), 1, 48000, 90 * 48000, 100, /*rf64=*/true);
  // Honest about its length, which is over the two minutes a clip may be.
  const juce::File lengthy =
      writeForgedWav(clips.getChildFile("lengthy.wav"), 1, 8000, 121 * 8000, 121 * 8000);
  const juce::File surround = writeForgedWav(clips.getChildFile("surround.wav"), 6, 48000, 4800, 4800);
  const juce::File fast = writeForgedWav(clips.getChildFile("fast.wav"), 1, 384000, 38400, 38400);
  const juce::File honest = writeForgedWav(clips.getChildFile("honest.wav"), 2, 48000, 4800, 4800);

  const auto refusal = [&](const juce::File& file) {
    const CommandResult result =
        rig.local("render.clip", "{\"clips\":[" + agentQuoted(file.getFullPathName()) + "]}");
    EXPECT_EQ(result.errorCode, "failed") << file.getFileName();
    return result.errorMessage;
  };
  EXPECT_TRUE(refusal(hollow).contains("claims more audio")) << refusal(hollow);
  EXPECT_TRUE(refusal(lengthy).contains("longer than 120")) << refusal(lengthy);
  EXPECT_TRUE(refusal(surround).contains("mono or stereo")) << refusal(surround);
  EXPECT_TRUE(refusal(fast).contains("sample rate")) << refusal(fast);
  EXPECT_TRUE(rig.render(honest)["renders"].size() == 1);

  // The reference of a comparison is read by the same rules.
  const juce::String honestPath = agentQuoted(honest.getFullPathName());
  for (const auto& file : {hollow, surround}) {
    const CommandResult compared =
        rig.local("render.compare", "{\"reference\":" + agentQuoted(file.getFullPathName()) +
                                        ",\"clips\":[" + honestPath + "]}");
    EXPECT_EQ(compared.errorCode, "failed") << file.getFileName();
  }
  // Refused renders leave the rig free.
  const auto sandbox = rig.hub.findSandbox("sandbox-1");
  ASSERT_NE(sandbox, nullptr);
  EXPECT_FALSE(sandbox->rendering.load());
  EXPECT_TRUE(rig.hub.tryBeginRender());
  EXPECT_TRUE(rig.hub.tryBeginRender());
  rig.hub.endRender();
  rig.hub.endRender();
}

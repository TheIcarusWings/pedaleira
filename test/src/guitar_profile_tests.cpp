// Guitar profiles: a guitar as its recorded DI clips and their measurements.
// A profile is created, given clips of known level, brightness and hum, and
// the numbers come back right and survive a restart; the recording path
// copies exactly what went into processBlock, without touching what comes
// out; and assets.list finds the captures and IRs of a library.

#include "AssetTags.h"
#include "InputRecorder.h"
#include "agent_test_helpers.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

namespace {

juce::String codeOf(const CommandResult& result) {
  return result.ok ? juce::String("ok") : result.errorCode;
}

juce::var takeOf(const juce::var& profile, const char* position, const char* take) {
  if (const auto* takes = profile["takes"].getArray())
    for (const auto& entry : *takes)
      if (entry["position"].toString() == position && entry["take"].toString() == take)
        return entry;
  return {};
}

juce::String importArgs(const char* guitar, const char* position, const char* take,
                        const juce::File& file) {
  return "{\"guitar\":" + agentQuoted(guitar) + ",\"position\":" + agentQuoted(position) +
         ",\"take\":" + agentQuoted(take) + ",\"path\":" + agentQuoted(file.getFullPathName()) +
         "}";
}

// Feeds `left` and `right` through the live rig's processBlock and returns
// the left output.
std::vector<float> feed(AgentRig& rig, const std::vector<float>& left,
                        const std::vector<float>& right) {
  std::vector<float> out(left.size());
  juce::AudioBuffer<float> buffer(2, rig.block);
  juce::MidiBuffer midi;
  for (size_t start = 0; start < left.size(); start += static_cast<size_t>(rig.block)) {
    const int count = static_cast<int>(std::min<size_t>(static_cast<size_t>(rig.block), left.size() - start));
    buffer.clear();
    buffer.copyFrom(0, 0, left.data() + start, count);
    buffer.copyFrom(1, 0, right.data() + start, count);
    rig.proc.processBlock(buffer, midi);
    std::copy(buffer.getReadPointer(0), buffer.getReadPointer(0) + count,
              out.begin() + static_cast<std::ptrdiff_t>(start));
  }
  return out;
}

juce::var waitForRecording(AgentRig& rig) {
  juce::var status;
  for (int i = 0; i < 400; ++i) {
    status = rig.mustLocal("guitar.record.status");
    const juce::String state = status["state"].toString();
    if (state == "done" || state == "failed" || state == "idle")
      break;
    juce::Thread::sleep(10);
  }
  return status;
}

}  // namespace

TEST(GuitarProfiles, CreateImportAnalyzeRoundTrip) {
  AgentRig rig;
  EXPECT_EQ(rig.mustLocal("guitar.list")["guitars"].size(), 0);

  const juce::var created = rig.mustLocal(
      "guitar.create",
      R"({"name":"Les Paul #2","positions":["Bridge","Neck"],"pickupType":"humbucker",
          "tuning":"E standard","notes":"New strings."})");
  EXPECT_EQ(created["id"].toString(), "les-paul-2");
  EXPECT_EQ(created["name"].toString(), "Les Paul #2");
  EXPECT_EQ(created["pickupType"].toString(), "humbucker");
  EXPECT_EQ(created["positions"].size(), 2);
  EXPECT_EQ(created["takes"].size(), 0);
  EXPECT_TRUE(static_cast<bool>(created["isDefault"]));  // the first guitar

  // Clips with answers known in advance:
  //  - soft: a 220 Hz sine at amplitude 0.05 with 50 Hz hum at -40 dBFS
  //  - hard: the same note at amplitude 0.6, no hum
  //  - neck notes: dull plucks; bridge notes: bright plucks
  const juce::File incoming = rig.dirs.library.getChildFile("incoming");
  auto soft = makeSine(96000, 220.0, 0.05f);
  const auto hum = makeSine(96000, 50.0, 0.01f);
  for (size_t i = 0; i < soft.size(); ++i)
    soft[i] += hum[i];
  const juce::File softFile = writeTestWav(incoming.getChildFile("soft.wav"), soft);
  const juce::File hardFile =
      writeTestWav(incoming.getChildFile("hard.wav"), makeSine(96000, 220.0, 0.6f));
  const juce::File dullFile =
      writeTestWav(incoming.getChildFile("dull.wav"), pluckedClip(3.0, 0.4f, 0.0));
  const juce::File brightFile =
      writeTestWav(incoming.getChildFile("bright.wav"), pluckedClip(3.0, 0.4f, 1.0));

  rig.mustLocal("guitar.importClip", importArgs("les-paul-2", "bridge", "soft", softFile));
  rig.mustLocal("guitar.importClip", importArgs("les-paul-2", "Bridge", "hard", hardFile));
  rig.mustLocal("guitar.importClip", importArgs("les-paul-2", "bridge", "notes", brightFile));
  const juce::var profile =
      rig.mustLocal("guitar.importClip", importArgs("les-paul-2", "neck", "notes", dullFile));
  ASSERT_EQ(profile["takes"].size(), 4);

  // Level: a sine's peak and RMS are its amplitude and 3 dB less.
  const juce::var hard = takeOf(profile, "Bridge", "hard")["measurements"];
  EXPECT_NEAR(static_cast<double>(hard["peakDb"]), -4.44, 0.05);
  EXPECT_NEAR(static_cast<double>(hard["rmsDb"]), -7.45, 0.05);
  // LUFS: the RMS less 0.69, less what K-weighting takes off at 220 Hz.
  EXPECT_NEAR(static_cast<double>(hard["lufs"]), -8.6, 1.0);
  // A steady tone has no pause to read a noise floor in.
  EXPECT_TRUE(hard["noiseFloorDb"].isVoid());
  EXPECT_NEAR(static_cast<double>(hard["centroidHz"]), 220.0, 5.0);
  EXPECT_NEAR(static_cast<double>(hard["lowEndShare"]), 1.0, 0.01);
  EXPECT_EQ(static_cast<int>(hard["hum"]["mainsHz"]), 0);
  EXPECT_NEAR(static_cast<double>(hard["seconds"]), 2.0, 1.0e-6);
  EXPECT_EQ(static_cast<int>(hard["channels"]), 1);

  // Hum: found at 50 Hz, at the level it was put in.
  const juce::var softMeasured = takeOf(profile, "Bridge", "soft")["measurements"];
  EXPECT_EQ(static_cast<int>(softMeasured["hum"]["mainsHz"]), 50);
  EXPECT_NEAR(static_cast<double>(softMeasured["hum"]["hz50Db"]), -40.0, 0.5);
  EXPECT_LT(static_cast<double>(softMeasured["hum"]["hz60Db"]), -60.0);
  EXPECT_NEAR(static_cast<double>(softMeasured["peakDb"]), 20.0 * std::log10(0.06), 0.3);

  // Brightness: the bright plucks have the higher centroid and the smaller
  // low end.
  const juce::var bright = takeOf(profile, "Bridge", "notes")["measurements"];
  const juce::var dull = takeOf(profile, "Neck", "notes")["measurements"];
  EXPECT_GT(static_cast<double>(bright["centroidHz"]), 2.0 * static_cast<double>(dull["centroidHz"]));
  EXPECT_LT(static_cast<double>(bright["lowEndShare"]), static_cast<double>(dull["lowEndShare"]));
  EXPECT_GT(static_cast<double>(bright["spectrum"]["bandsDb"]["highMid"]),
            static_cast<double>(dull["spectrum"]["bandsDb"]["highMid"]) + 6.0);

  // The summary puts words on the numbers.
  const juce::var summary = profile["summary"];
  EXPECT_EQ(static_cast<int>(summary["takeCount"]), 4);
  EXPECT_EQ(summary["outputLevel"].toString(), "high");  // the loudest take peaks above -6 dBFS
  EXPECT_EQ(summary["noise"].toString(), "noisy");       // -40 dBFS of hum
  EXPECT_EQ(static_cast<int>(summary["mainsHz"]), 50);
  EXPECT_GE(static_cast<double>(summary["peakDb"]), -4.5);
  EXPECT_TRUE(juce::StringArray({"dark", "balanced", "bright"}).contains(summary["brightness"].toString()));

  // On disk where the documentation says, and read back by a new instance
  // (the next start of the application).
  const juce::File folder = rig.dirs.library.getChildFile("Guitars").getChildFile("les-paul-2");
  EXPECT_TRUE(folder.getChildFile("profile.json").existsAsFile());
  EXPECT_TRUE(folder.getChildFile("bridge-soft.wav").existsAsFile());
  EXPECT_TRUE(folder.getChildFile("neck-notes.wav").existsAsFile());
  EXPECT_EQ(created["folder"].toString(), folder.getFullPathName());
  {
    ChainTestProcessor next;
    EngineCommands nextCommands(next);
    const juce::var reread =
        nextCommands.registry().run("guitar.get", agentJson(R"({"guitar":"les-paul-2"})")).value;
    EXPECT_EQ(juce::JSON::toString(reread), juce::JSON::toString(profile));
    const juce::var list = nextCommands.registry().run("guitar.list").value;
    ASSERT_EQ(list["guitars"].size(), 1);
    EXPECT_EQ(list["default"].toString(), "les-paul-2");
    EXPECT_EQ(static_cast<int>(list["guitars"][0]["takeCount"]), 4);
    EXPECT_EQ(list["guitars"][0]["summary"]["noise"].toString(), "noisy");
  }

  // A take is replaced, not added to, when it is imported again; analyze
  // picks up a clip that changed on disk.
  rig.mustLocal("guitar.importClip", importArgs("les-paul-2", "bridge", "soft", hardFile));
  juce::var again = rig.mustLocal("guitar.get", R"({"guitar":"les-paul-2"})");
  EXPECT_EQ(again["takes"].size(), 4);
  EXPECT_EQ(again["summary"]["noise"].toString(), "quiet");
  writeTestWav(folder.getChildFile("bridge-soft.wav"), soft);
  again = rig.mustLocal("guitar.analyze", R"({"guitar":"les-paul-2"})");
  EXPECT_EQ(again["summary"]["noise"].toString(), "noisy");
  EXPECT_EQ(again["unreadable"].size(), 0);
  // A take whose file has gone is reported, not silently dropped.
  EXPECT_TRUE(folder.getChildFile("neck-notes.wav").deleteFile());
  EXPECT_EQ(rig.mustLocal("guitar.analyze", R"({"guitar":"les-paul-2"})")["unreadable"][0].toString(),
            "neck-notes.wav");
}

TEST(GuitarProfiles, UpdateDefaultDeleteAndWhatIsRefused) {
  AgentRig rig;
  rig.mustLocal("guitar.create", R"({"name":"Strat","positions":["bridge","middle","neck"]})");
  // A second guitar of the same name gets its own id; the first stays default.
  const juce::var second = rig.mustLocal("guitar.create", R"({"name":"Strat","positions":["bridge"]})");
  EXPECT_EQ(second["id"].toString(), "strat-2");
  EXPECT_FALSE(static_cast<bool>(second["isDefault"]));
  EXPECT_EQ(second["pickupType"].toString(), "other");

  const juce::var updated = rig.mustLocal(
      "guitar.update",
      R"j({"guitar":"strat","name":"Strat (maple)","pickupType":"single-coil","tuning":"Eb","notes":"11s"})j");
  EXPECT_EQ(updated["name"].toString(), "Strat (maple)");
  EXPECT_EQ(updated["pickupType"].toString(), "single-coil");
  EXPECT_EQ(updated["tuning"].toString(), "Eb");
  EXPECT_EQ(updated["positions"].size(), 3);  // left out: unchanged
  EXPECT_EQ(updated["id"].toString(), "strat");  // the id is permanent

  EXPECT_EQ(rig.mustLocal("guitar.setDefault", R"({"guitar":"strat-2"})")["default"].toString(), "strat-2");
  EXPECT_TRUE(static_cast<bool>(rig.mustLocal("guitar.get", R"({"guitar":"strat-2"})")["isDefault"]));
  EXPECT_FALSE(static_cast<bool>(rig.mustLocal("guitar.get", R"({"guitar":"strat"})")["isDefault"]));

  const juce::File clip = writeTestWav(rig.dirs.library.getChildFile("c.wav"), makeSine(48000, 330.0, 0.2f));
  rig.mustLocal("guitar.importClip", importArgs("strat", "middle", "chords", clip));
  // A position with takes cannot be dropped from under them.
  EXPECT_EQ(codeOf(rig.local("guitar.update", R"({"guitar":"strat","positions":["bridge","neck"]})")), "failed");
  EXPECT_TRUE(rig.local("guitar.update", R"({"guitar":"strat","positions":["bridge","middle"]})").ok);

  // What is refused, and with which code.
  EXPECT_EQ(codeOf(rig.local("guitar.get", R"({"guitar":"nope"})")), "not_found");
  EXPECT_EQ(codeOf(rig.local("guitar.get", R"({"guitar":"../../etc"})")), "not_found");
  EXPECT_EQ(codeOf(rig.local("guitar.create", R"({"name":"  ","positions":["a"]})")), "invalid_args");
  EXPECT_EQ(codeOf(rig.local("guitar.create", R"({"name":"X","positions":[]})")), "invalid_args");
  EXPECT_EQ(codeOf(rig.local("guitar.create",
                             R"({"name":"X","positions":["1","2","3","4","5","6","7","8","9"]})")),
            "invalid_args");
  EXPECT_EQ(codeOf(rig.local("guitar.create", R"({"name":"X","positions":["a"],"pickupType":"laser"})")),
            "invalid_args");
  EXPECT_EQ(codeOf(rig.local("guitar.importClip", importArgs("strat", "bridge", "loud", clip))),
            "invalid_args");  // not one of the four takes
  EXPECT_EQ(codeOf(rig.local("guitar.importClip", importArgs("strat", "tailpiece", "soft", clip))),
            "invalid_args");
  EXPECT_EQ(codeOf(rig.local("guitar.importClip", importArgs("nope", "bridge", "soft", clip))),
            "not_found");
  EXPECT_EQ(codeOf(rig.local("guitar.importClip",
                             importArgs("strat", "bridge", "soft",
                                        rig.dirs.library.getChildFile("missing.wav")))),
            "not_found");
  // Not audio: refused, and the take that was there stays.
  const juce::File text = rig.dirs.library.getChildFile("notes.wav");
  EXPECT_TRUE(text.replaceWithText("this is not a wav file"));
  EXPECT_EQ(codeOf(rig.local("guitar.importClip", importArgs("strat", "middle", "chords", text))), "failed");
  EXPECT_EQ(rig.mustLocal("guitar.get", R"({"guitar":"strat"})")["takes"].size(), 1);

  // Deleting takes the folder with it; the default goes when its guitar does.
  const juce::var remaining = rig.mustLocal("guitar.delete", R"({"guitar":"strat-2"})");
  EXPECT_EQ(remaining["guitars"].size(), 1);
  EXPECT_EQ(remaining["default"].toString(), "");
  EXPECT_FALSE(rig.dirs.library.getChildFile("Guitars").getChildFile("strat-2").exists());
  EXPECT_EQ(codeOf(rig.local("guitar.delete", R"({"guitar":"strat-2"})")), "not_found");
  // With no default, a render has to be told which guitar.
  EXPECT_EQ(codeOf(rig.local("render.clip")), "not_found");
}

// A clip handed to a profile is somebody's file: what its header claims is
// checked against the limits and against the file before any of it is read.
TEST(GuitarProfiles, AClipsHeaderIsHeldAgainstTheFile) {
  AgentRig rig;
  rig.mustLocal("guitar.create", R"({"name":"Strat","positions":["bridge"]})");
  const juce::File incoming = rig.dirs.library.getChildFile("incoming");

  juce::AudioBuffer<float> audio;
  double rate = 0.0;
  AudioFiles::Info info;
  const auto refused = [&](const juce::File& file, const char* why) {
    juce::String readError, inspectError;
    audio.setSize(0, 0);
    EXPECT_FALSE(AudioFiles::read(file, audio, rate, readError)) << file.getFileName();
    EXPECT_EQ(audio.getNumSamples(), 0) << "a buffer was made for " << file.getFileName();
    EXPECT_FALSE(AudioFiles::inspect(file, info, inspectError)) << file.getFileName();
    EXPECT_TRUE(readError.contains(why)) << readError;
    EXPECT_EQ(readError, inspectError);
    // And through the command an agent would use.
    EXPECT_EQ(codeOf(rig.local("guitar.importClip", importArgs("strat", "bridge", "soft", file))),
              "failed")
        << file.getFileName();
  };
  // A header that names a minute and a half in a file of a few hundred
  // bytes: the buffer for it would be seventeen megabytes of nothing.
  refused(writeForgedWav(incoming.getChildFile("hollow.wav"), 1, 48000, 90 * 48000, 100,
                         /*rf64=*/true),
          "claims more audio");
  // Barely more than is there is still more than is there.
  refused(writeForgedWav(incoming.getChildFile("short.wav"), 2, 48000, 48000, 47000,
                         /*rf64=*/true),
          "claims more audio");
  // The plain RIFF form of the same lie is caught one layer down.
  refused(writeForgedWav(incoming.getChildFile("hollow-riff.wav"), 1, 48000, 90 * 48000, 100),
          "empty");
  refused(writeForgedWav(incoming.getChildFile("surround.wav"), 3, 48000, 4800, 4800),
          "mono or stereo");
  refused(writeForgedWav(incoming.getChildFile("fast.wav"), 1, 384000, 38400, 38400), "sample rate");
  refused(writeForgedWav(incoming.getChildFile("lengthy.wav"), 1, 8000, 121 * 8000, 121 * 8000),
          "longer than 120");
  EXPECT_EQ(rig.mustLocal("guitar.get", R"({"guitar":"strat"})")["takes"].size(), 0);

  // What is as it says is read, and inspect answers from the header alone.
  const juce::File honest = writeForgedWav(incoming.getChildFile("honest.wav"), 2, 44100, 44100, 44100);
  const juce::File honest64 =
      writeForgedWav(incoming.getChildFile("honest64.wav"), 1, 48000, 4800, 4800, /*rf64=*/true);
  juce::String error;
  ASSERT_TRUE(AudioFiles::inspect(honest64, info, error)) << error;
  EXPECT_EQ(info.frames, 4800);
  ASSERT_TRUE(AudioFiles::inspect(honest, info, error)) << error;
  EXPECT_EQ(info.channels, 2);
  EXPECT_EQ(info.frames, 44100);
  EXPECT_DOUBLE_EQ(info.sampleRate, 44100.0);
  EXPECT_DOUBLE_EQ(info.seconds(), 1.0);
  ASSERT_TRUE(AudioFiles::read(honest, audio, rate, error)) << error;
  EXPECT_EQ(audio.getNumChannels(), 2);
  EXPECT_EQ(audio.getNumSamples(), 44100);
  EXPECT_TRUE(rig.local("guitar.importClip", importArgs("strat", "bridge", "soft", honest)).ok);
}

TEST(GuitarProfiles, RecordingCopiesTheInputAndLeavesTheOutputAlone) {
  AgentRig rig;
  rig.mustLocal("guitar.create", R"({"name":"Tele","positions":["bridge","neck"]})");

  const int frames = 48000 + 4800;  // a little more than the second asked for
  const auto guitar = makeSine(frames, 196.0, 0.3f);
  const auto other = makeNoise(frames, 9, 0.2f);

  // What the rig puts out for this input when nothing is being recorded.
  // (Fed once before, so both runs start from the same filter states.)
  feed(rig, guitar, other);
  const std::vector<float> reference = feed(rig, guitar, other);

  EXPECT_EQ(rig.mustLocal("guitar.record.status")["state"].toString(), "idle");
  const juce::var started = rig.mustLocal(
      "guitar.record.start", R"({"guitar":"tele","position":"bridge","take":"hard","seconds":1})");
  EXPECT_EQ(started["state"].toString(), "recording");
  EXPECT_NEAR(static_cast<double>(started["progress"]), 0.0, 1.0e-9);

  // Half way: still recording, with a level to show.
  // (Split on a block boundary, so both runs cut the audio into the same
  // blocks.)
  const int half = 188 * 128;
  const std::vector<float> firstHalf(guitar.begin(), guitar.begin() + half);
  const std::vector<float> otherHalf(other.begin(), other.begin() + half);
  const std::vector<float> outputHalf = feed(rig, firstHalf, otherHalf);
  const juce::var midway = rig.mustLocal("guitar.record.status");
  EXPECT_EQ(midway["state"].toString(), "recording");
  EXPECT_NEAR(static_cast<double>(midway["progress"]), 0.5, 0.01);
  EXPECT_NEAR(static_cast<double>(midway["recordedSeconds"]), half / 48000.0, 1.0e-3);
  EXPECT_NEAR(static_cast<double>(midway["peakDb"]), 20.0 * std::log10(0.3), 0.1);

  // The rest. The audio thread fills the buffer and stops by itself.
  const std::vector<float> restGuitar(guitar.begin() + half, guitar.end());
  const std::vector<float> restOther(other.begin() + half, other.end());
  const std::vector<float> outputRest = feed(rig, restGuitar, restOther);
  EXPECT_EQ(rig.proc.inputRecorder.getState(), InputRecorder::State::full);

  // Recording changed nothing about what the rig put out.
  for (size_t i = 0; i < outputHalf.size(); ++i)
    ASSERT_EQ(outputHalf[i], reference[i]) << "sample " << i;
  for (size_t i = 0; i < outputRest.size(); ++i)
    ASSERT_EQ(outputRest[i], reference[static_cast<size_t>(half) + i]) << "sample " << i;

  // The first status call after the buffer filled hands it to the writer.
  const juce::var done = waitForRecording(rig);
  ASSERT_EQ(done["state"].toString(), "done") << juce::JSON::toString(done);
  EXPECT_EQ(done["result"]["position"].toString(), "bridge");
  EXPECT_EQ(done["result"]["take"].toString(), "hard");
  EXPECT_NEAR(static_cast<double>(done["result"]["measurements"]["peakDb"]), 20.0 * std::log10(0.3), 0.05);
  EXPECT_NEAR(static_cast<double>(done["result"]["seconds"]), 1.0, 1.0e-6);
  EXPECT_EQ(rig.proc.inputRecorder.getState(), InputRecorder::State::idle);

  // The file holds exactly the first second of channel 0.
  const juce::File clip =
      rig.dirs.library.getChildFile("Guitars").getChildFile("tele").getChildFile("bridge-hard.wav");
  juce::AudioBuffer<float> recorded;
  double rate = 0.0;
  juce::String error;
  ASSERT_TRUE(AudioFiles::read(clip, recorded, rate, error)) << error;
  EXPECT_EQ(rate, 48000.0);
  ASSERT_EQ(recorded.getNumSamples(), 48000);
  ASSERT_EQ(recorded.getNumChannels(), 1);
  for (int i = 0; i < 48000; ++i)
    ASSERT_EQ(recorded.getSample(0, i), guitar[static_cast<size_t>(i)]) << "sample " << i;
  EXPECT_EQ(rig.mustLocal("guitar.get", R"({"guitar":"tele"})")["takes"].size(), 1);

  // Another channel, when the guitar is plugged in there.
  rig.mustLocal("guitar.record.start",
                R"({"guitar":"tele","position":"neck","take":"soft","seconds":1,"channel":1})");
  feed(rig, guitar, other);
  ASSERT_EQ(waitForRecording(rig)["state"].toString(), "done");
  ASSERT_TRUE(AudioFiles::read(clip.getSiblingFile("neck-soft.wav"), recorded, rate, error)) << error;
  for (int i = 0; i < 48000; i += 97)
    ASSERT_EQ(recorded.getSample(0, i), other[static_cast<size_t>(i)]) << "sample " << i;

  // Cancelled: nothing is kept, and more audio records nothing.
  rig.mustLocal("guitar.record.start", R"({"guitar":"tele","position":"neck","take":"chords","seconds":1})");
  feed(rig, firstHalf, otherHalf);
  EXPECT_EQ(rig.mustLocal("guitar.record.cancel")["state"].toString(), "idle");
  feed(rig, guitar, other);
  EXPECT_EQ(rig.mustLocal("guitar.record.status")["state"].toString(), "idle");
  EXPECT_EQ(rig.mustLocal("guitar.get", R"({"guitar":"tele"})")["takes"].size(), 2);
  EXPECT_FALSE(clip.getSiblingFile("neck-chords.wav").exists());

  // What cannot be recorded.
  EXPECT_EQ(codeOf(rig.local("guitar.record.start", R"({"guitar":"nope","position":"neck","take":"soft"})")),
            "not_found");
  EXPECT_EQ(codeOf(rig.local("guitar.record.start", R"({"guitar":"tele","position":"nut","take":"soft"})")),
            "invalid_args");
  EXPECT_EQ(codeOf(rig.local("guitar.record.start",
                             R"({"guitar":"tele","position":"neck","take":"soft","seconds":600})")),
            "invalid_args");
  // A client needs the user's say-so to listen to the input.
  const auto client = rig.hub.openSession("agent");
  EXPECT_EQ(codeOf(rig.as(client, "guitar.record.start", R"({"guitar":"tele","position":"neck","take":"soft"})")),
            "needs_approval");
  EXPECT_EQ(rig.proc.inputRecorder.getState(), InputRecorder::State::idle);
}

TEST(GuitarProfiles, RecordingNeedsARunningDevice) {
  AgentTestDirs dirs;
  ChainTestProcessor idle;  // never prepared: no sample rate, no audio
  EngineCommands commands(idle);
  ASSERT_TRUE(commands.registry()
                  .run("guitar.create", agentJson(R"({"name":"G","positions":["bridge"]})"))
                  .ok);
  const CommandResult refused = commands.registry().run(
      "guitar.record.start", agentJson(R"({"guitar":"g","position":"bridge","take":"soft"})"));
  EXPECT_EQ(refused.errorCode, "unavailable");
}

TEST(GuitarProfiles, TheRecorderIsSafeAgainstTheAudioThread) {
  // The audio thread captures while the message thread starts, cancels and
  // takes recordings over and over: whatever is taken out is whole and came
  // from the input, and nothing is written outside the buffer (the sanitizer
  // builds would say).
  InputRecorder recorder;
  std::atomic<bool> stop{false};
  std::thread audio([&] {
    juce::AudioBuffer<float> block(2, 64);
    for (int i = 0; i < 64; ++i) {
      block.setSample(0, i, 0.25f);
      block.setSample(1, i, -0.5f);
    }
    while (!stop.load())
      recorder.capture(block);
  });

  int taken = 0;
  for (int round = 0; round < 300; ++round) {
    const int channel = round % 3;  // 2 is a channel the buffer does not have
    recorder.start(1000 + round, channel);
    if (round % 4 == 0) {
      recorder.cancel();
      EXPECT_EQ(recorder.getState(), InputRecorder::State::idle);
      EXPECT_TRUE(recorder.take().empty());
      continue;
    }
    // Wait for the audio thread to fill it, however long it is descheduled:
    // a spin count would give up on a loaded machine. The audio thread
    // always finishes a recording, and ctest's timeout bounds a real hang.
    while (recorder.getState() != InputRecorder::State::full)
      std::this_thread::yield();
    const std::vector<float> samples = recorder.take();
    ASSERT_EQ(samples.size(), static_cast<size_t>(1000 + round));
    // A channel beyond the buffer falls back to the last one it has.
    const float expected = channel == 0 ? 0.25f : -0.5f;
    for (const float sample : samples)
      ASSERT_EQ(sample, expected);
    ++taken;
  }
  stop.store(true);
  audio.join();
  EXPECT_GT(taken, 100);

  // Idle, capture is nothing at all.
  juce::AudioBuffer<float> block(1, 32);
  block.clear();
  recorder.capture(block);
  EXPECT_EQ(recorder.getState(), InputRecorder::State::idle);
  EXPECT_EQ(recorder.recordedSamples(), 0);
}

TEST(AssetsList, FindsTheCapturesAndIrsOfTheLibrary) {
  AgentRig rig;
  const juce::File amps = rig.dirs.library.getChildFile("Amps").getChildFile("High Gain");
  const juce::File cabs = rig.dirs.library.getChildFile("Cabs");
  ASSERT_TRUE(amps.createDirectory().wasOk());
  ASSERT_TRUE(cabs.createDirectory().wasOk());
  ASSERT_TRUE(testFile("a2-amp-test.nam").copyFileTo(amps.getChildFile("Test Amp.nam")));
  ASSERT_TRUE(testFile("a2-amp-cab-test.nam").copyFileTo(amps.getChildFile("Other Amp.nam")));
  ASSERT_TRUE(testFile("cab-ir-test.wav").copyFileTo(cabs.getChildFile("Test Cab.wav")));
  // A guitar's clips are WAVs in the library too, and are not IRs.
  rig.mustLocal("guitar.create", R"({"name":"G","positions":["bridge"]})");
  const juce::File clip = writeTestWav(rig.dirs.scratch.getChildFile("clip.wav"), makeSine(48000, 220.0, 0.2f));
  rig.mustLocal("guitar.importClip",
                "{\"guitar\":\"g\",\"position\":\"bridge\",\"take\":\"soft\",\"path\":" +
                    agentQuoted(clip.getFullPathName()) + "}");

  // The library's files only: the model stash is shared by the whole test
  // run and holds whatever earlier tests loaded.
  const auto libraryAssets = [&](const juce::String& args = {}) {
    const juce::var listed = rig.mustLocal("assets.list", args);
    int count = 0;
    for (int i = 0; i < listed["assets"].size(); ++i)
      count += listed["assets"][i]["source"].toString() == "library" ? 1 : 0;
    return count;
  };
  const juce::var all = rig.mustLocal("assets.list", R"({"limit":1000})");
  ASSERT_EQ(libraryAssets(R"({"limit":1000})"), 3) << juce::JSON::toString(all);
  EXPECT_FALSE(static_cast<bool>(all["truncated"]));
  juce::var amp, cab;
  for (int i = 0; i < all["assets"].size(); ++i) {
    if (all["assets"][i]["name"].toString() == "Test Amp")
      amp = all["assets"][i];
    if (all["assets"][i]["name"].toString() == "Test Cab")
      cab = all["assets"][i];
  }
  ASSERT_FALSE(amp.isVoid());
  EXPECT_EQ(amp["kind"].toString(), "capture");
  EXPECT_EQ(amp["source"].toString(), "library");
  EXPECT_EQ(amp["path"].toString(), amps.getChildFile("Test Amp.nam").getFullPathName());
  ASSERT_EQ(amp["tags"].size(), 2);
  EXPECT_EQ(amp["tags"][0].toString(), "Amps");
  EXPECT_EQ(amp["tags"][1].toString(), "High Gain");
  // What the capture file says about itself (read without the weights).
  EXPECT_NEAR(static_cast<double>(amp["nam_gain"]), 0.82, 0.01);
  EXPECT_NEAR(static_cast<double>(amp["nam_loudness"]), -14.13, 0.01);
  EXPECT_TRUE(amp["nam_name"].isVoid());  // null in the file: left out
  ASSERT_FALSE(cab.isVoid());
  EXPECT_EQ(cab["kind"].toString(), "ir");
  EXPECT_EQ(cab["tags"][0].toString(), "Cabs");
  EXPECT_GT(static_cast<double>(cab["sampleRate"]), 8000.0);
  EXPECT_GT(static_cast<double>(cab["seconds"]), 0.0);

  EXPECT_EQ(libraryAssets(R"({"kind":"ir","limit":1000})"), 1);
  EXPECT_EQ(libraryAssets(R"({"kind":"capture","limit":1000})"), 2);
  EXPECT_EQ(libraryAssets(R"({"query":"other amp","limit":1000})"), 1);
  EXPECT_EQ(libraryAssets(R"({"query":"high gain","limit":1000})"), 2);
  EXPECT_EQ(rig.mustLocal("assets.list", R"({"query":"no such thing anywhere"})")["assets"].size(), 0);
  const juce::var limited = rig.mustLocal("assets.list", R"({"limit":1})");
  EXPECT_EQ(limited["assets"].size(), 1);
  EXPECT_GE(static_cast<int>(limited["total"]), 3);
  EXPECT_TRUE(static_cast<bool>(limited["truncated"]));

  // What a listed path is for: loading it. Then it shows as in the rig, and
  // its stashed copy as an asset of its own.
  rig.mustLocal("chain.addToneFromFiles", "{\"paths\":[" + agentQuoted(amp["path"].toString()) + "]}");
  rig.mustLocal("state.waitUntilLoaded");
  const juce::var after = rig.mustLocal("assets.list", R"({"limit":1000})");
  ASSERT_EQ(after["inRig"].size(), 1);
  EXPECT_EQ(after["inRig"][0]["title"].toString(), "Test Amp");
  EXPECT_EQ(after["inRig"][0]["kind"].toString(), "capture");
  EXPECT_EQ(after["rig"].toString(), "live");
  bool stashed = false;
  for (int i = 0; i < after["assets"].size(); ++i)
    stashed = stashed || after["assets"][i]["source"].toString() == "stash";
  EXPECT_TRUE(stashed);
}

TEST(AssetTags, TagsAreNormalised) {
  EXPECT_EQ(AssetTags::normalise("  Edge   of\tBreakup "), juce::String("edge of breakup"));
  EXPECT_EQ(AssetTags::normalise("V30"), juce::String("v30"));
  EXPECT_TRUE(AssetTags::normalise("   ").isEmpty());
}

TEST(AssetTags, TagsFollowTheFileAndFilterTheList) {
  AgentRig rig;
  const juce::File amps = rig.dirs.library.getChildFile("Amps");
  ASSERT_TRUE(amps.createDirectory().wasOk());
  const juce::File amp = amps.getChildFile("Tag Amp.nam");
  const juce::File cab = rig.dirs.library.getChildFile("Tag Cab.wav");
  ASSERT_TRUE(testFile("a2-amp-test.nam").copyFileTo(amp));
  ASSERT_TRUE(testFile("cab-ir-test.wav").copyFileTo(cab));

  const auto libraryAssets = [&](const juce::String& args) {
    const juce::var listed = rig.mustLocal("assets.list", args);
    juce::StringArray names;
    for (int i = 0; i < listed["assets"].size(); ++i)
      if (listed["assets"][i]["source"].toString() == "library")
        names.add(listed["assets"][i]["name"].toString());
    return names;
  };

  // Nothing tagged yet: both are up for review.
  juce::var summary = rig.mustLocal("assets.tagSummary");
  EXPECT_GE(static_cast<int>(summary["captures"]["total"]), 1);
  EXPECT_EQ(static_cast<int>(summary["captures"]["tagged"]), 0);
  EXPECT_EQ(static_cast<int>(summary["irs"]["tagged"]), 0);
  EXPECT_TRUE(summary["vocabulary"]["gain"].isArray());
  EXPECT_TRUE(libraryAssets(R"({"untagged":true,"limit":1000})").contains("Tag Amp"));

  // Tagging, normalised; removing wins over adding.
  const juce::var tagged = rig.mustLocal(
      "assets.tag", "{\"file\":" + agentQuoted(amp.getFullPathName()) +
                        ",\"add\":[\"Crunch\",\"  Bright \",\"mid forward\"],\"remove\":[\"mid forward\"]}");
  ASSERT_EQ(tagged["tags"].size(), 2);
  EXPECT_EQ(tagged["tags"][0].toString(), "crunch");
  EXPECT_EQ(tagged["tags"][1].toString(), "bright");
  rig.mustLocal("assets.tag", "{\"file\":" + agentQuoted(cab.getFullPathName()) + ",\"add\":[\"v30\"]}");

  // The list shows them, merged with the folder tags, and filters by them.
  const juce::var listed = rig.mustLocal("assets.list", R"({"tags":["crunch"],"limit":1000})");
  juce::var entry;
  for (int i = 0; i < listed["assets"].size(); ++i)
    if (listed["assets"][i]["name"].toString() == "Tag Amp")
      entry = listed["assets"][i];
  ASSERT_TRUE(entry.isObject()) << juce::JSON::toString(listed);
  EXPECT_TRUE(static_cast<bool>(entry["tagged"]));
  EXPECT_EQ(entry["userTags"].size(), 2);
  EXPECT_EQ(entry["tags"][0].toString(), "Amps");
  EXPECT_EQ(libraryAssets(R"({"tags":["amps","bright"],"limit":1000})"), juce::StringArray("Tag Amp"));
  EXPECT_TRUE(libraryAssets(R"({"tags":["crunch","v30"],"limit":1000})").isEmpty());
  EXPECT_EQ(libraryAssets(R"({"query":"v30","kind":"ir","limit":1000})"), juce::StringArray("Tag Cab"));
  EXPECT_FALSE(libraryAssets(R"({"untagged":true,"limit":1000})").contains("Tag Amp"));

  summary = rig.mustLocal("assets.tagSummary");
  EXPECT_EQ(static_cast<int>(summary["captures"]["tagged"]), 1);
  EXPECT_EQ(static_cast<int>(summary["irs"]["tagged"]), 1);
  EXPECT_EQ(summary["tags"].size(), 3);

  // The tags are the file's content's: a renamed file keeps them.
  const juce::File renamed = amps.getChildFile("Renamed Amp.nam");
  ASSERT_TRUE(amp.moveFileTo(renamed));
  EXPECT_EQ(libraryAssets(R"({"tags":["crunch"],"limit":1000})"), juce::StringArray("Renamed Amp"));

  // Refusals: a tag too long, a file that is not a capture or IR, no file.
  const juce::String longTag(juce::String::repeatedString("x", 33));
  EXPECT_FALSE(rig.local("assets.tag", "{\"file\":" + agentQuoted(renamed.getFullPathName()) +
                                           ",\"add\":[\"" + longTag + "\"]}").ok);
  const juce::File text = rig.dirs.library.getChildFile("notes.txt");
  ASSERT_TRUE(text.replaceWithText("x"));
  EXPECT_EQ(rig.local("assets.tag", "{\"file\":" + agentQuoted(text.getFullPathName()) +
                                        ",\"add\":[\"x\"]}").errorCode,
            juce::String(CommandError::invalidArgs));
  EXPECT_EQ(rig.local("assets.tag", "{\"file\":" +
                                        agentQuoted(amps.getChildFile("gone.nam").getFullPathName()) +
                                        ",\"add\":[\"x\"]}").errorCode,
            juce::String(CommandError::notFound));

  // Removing the last tag leaves the file untagged again.
  rig.mustLocal("assets.tag", "{\"file\":" + agentQuoted(cab.getFullPathName()) + ",\"remove\":[\"v30\"]}");
  EXPECT_EQ(static_cast<int>(rig.mustLocal("assets.tagSummary")["irs"]["tagged"]), 0);
  EXPECT_TRUE(AssetTags::indexFile(rig.dirs.library).existsAsFile());
}

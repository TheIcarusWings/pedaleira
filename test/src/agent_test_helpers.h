#pragma once

// Shared pieces of the agent layer's test suites (agent_hub_tests,
// command_server_tests, listen_commands_tests, guitar_profile_tests): a
// processor with its command layer and hub in scratch folders of its own, a
// line client for the command server, and small signal and file helpers.

#include "AgentHub.h"
#include "CommandServer.h"
#include "EngineCommands.h"
#include "GuitarProfiles.h"
#include "LibraryPaths.h"
#include "chain_test_helpers.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <functional>
#include <string>
#include <thread>
#include <vector>

// Fresh folders for everything the agent layer keeps on disk, for one test;
// the process-wide overrides the test main set are put back afterwards.
struct AgentTestDirs {
  AgentTestDirs()
      : dir(juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("t3k-agent-test-" + juce::Uuid().toString())) {
    EXPECT_TRUE(dir.createDirectory().wasOk());
    library = dir.getChildFile("Library");
    scratch = dir.getChildFile("Scratch");
    presets = dir.getChildFile("Presets");
    outside = dir.getChildFile("Outside");
    for (const auto& folder : {library, scratch, presets, outside})
      EXPECT_TRUE(folder.createDirectory().wasOk());
    previousSettings = AgentSettings::setFileForTesting(dir.getChildFile("agents.settings"));
    previousConnection =
        CommandServer::setConnectionFileForTesting(dir.getChildFile("command-server.json"));
    previousLibrary = LibraryPaths::setLibraryRootForTesting(library);
    previousScratch = LibraryPaths::setScratchDirForTesting(scratch);
  }
  ~AgentTestDirs() {
    AgentSettings::setFileForTesting(previousSettings);
    CommandServer::setConnectionFileForTesting(previousConnection);
    LibraryPaths::setLibraryRootForTesting(previousLibrary);
    LibraryPaths::setScratchDirForTesting(previousScratch);
    dir.deleteRecursively();
  }

  juce::File dir, library, scratch, presets;
  // A folder no allow-list covers.
  juce::File outside;

private:
  juce::File previousSettings, previousConnection, previousLibrary, previousScratch;
};

inline juce::var agentJson(const juce::String& json) {
  juce::var value;
  if (json.isNotEmpty())
    EXPECT_TRUE(juce::JSON::parse(json, value).wasOk()) << json;
  return value;
}

inline juce::String agentQuoted(const juce::String& text) {
  return juce::JSON::toString(juce::var(text));
}

// The live rig with its command layer and hub. Commands run either as the
// local session (the user: no gate) or as a client session (an agent: the
// permission gate, sandbox routing and approvals apply), which is the same
// path a command server connection takes.
struct AgentRig {
  AgentTestDirs dirs;
  ChainTestProcessor proc;
  EngineCommands commands{proc};
  AgentHub& hub{commands.agents()};
  double rate = 48000.0;
  int block = 128;

  AgentRig() {
    // One library: the setlists and assets beside the guitar profiles.
    proc.setPresetStoreForTesting(dirs.presets, dirs.library);
    proc.setMultiCoreEnabled(false, /*persist=*/false);
    proc.setPlayConfigDetails(2, 2, rate, block);
    // The measurement suites feed quiet and synthetic signals the gate would
    // have opinions about. (A sandbox made from this rig copies the setting.)
    proc.parameters.getParameter("gateEnabled")->setValueNotifyingHost(0.0f);
    proc.prepareToPlay(rate, block);
  }

  CommandResult local(const char* name, const juce::String& argsJson = {}) {
    return hub.run(hub.localSession(), name, agentJson(argsJson));
  }

  juce::var mustLocal(const char* name, const juce::String& argsJson = {}) {
    const CommandResult result = local(name, argsJson);
    EXPECT_TRUE(result.ok) << name << " " << argsJson << ": " << result.errorCode << ": "
                           << result.errorMessage;
    return result.value;
  }

  CommandResult as(const std::shared_ptr<AgentSession>& session, const char* name,
                   const juce::String& argsJson = {}) {
    return hub.run(*session, name, agentJson(argsJson));
  }

  juce::var mustAs(const std::shared_ptr<AgentSession>& session, const char* name,
                   const juce::String& argsJson = {}) {
    const CommandResult result = as(session, name, argsJson);
    EXPECT_TRUE(result.ok) << name << " " << argsJson << ": " << result.errorCode << ": "
                           << result.errorMessage;
    return result.value;
  }

  // One block of the live rig's audio: `in` on both input channels.
  std::vector<float> processLive(const std::vector<float>& in) {
    std::vector<float> out(in.size());
    juce::AudioBuffer<float> buffer(2, block);
    juce::MidiBuffer midi;
    for (size_t start = 0; start < in.size(); start += static_cast<size_t>(block)) {
      const int count = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), in.size() - start));
      buffer.clear();
      for (int ch = 0; ch < 2; ++ch)
        buffer.copyFrom(ch, 0, in.data() + start, count);
      proc.processBlock(buffer, midi);
      std::copy(buffer.getReadPointer(0), buffer.getReadPointer(0) + count,
                out.begin() + static_cast<std::ptrdiff_t>(start));
    }
    return out;
  }

  // A parameter edit's revision bump lands 0.4 s after the edit (see
  // state.revision). Tests that watch the live rig's revision for outside
  // interference let its own edits settle first.
  void settleRevision() {
    const auto until = juce::Time::getMillisecondCounter() + 600;
    while (juce::Time::getMillisecondCounter() < until)
      juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    proc.getCurrentChainRevision();
  }

  juce::MemoryBlock liveState() {
    juce::MemoryBlock state;
    proc.getStateInformation(state);
    return state;
  }
};

// Writes a mono (or, with `right`, stereo) 32-bit float WAV.
inline juce::File writeTestWav(const juce::File& file, const std::vector<float>& left,
                               double rate = 48000.0, const std::vector<float>* right = nullptr) {
  juce::AudioBuffer<float> audio(right != nullptr ? 2 : 1, static_cast<int>(left.size()));
  std::copy(left.begin(), left.end(), audio.getWritePointer(0));
  if (right != nullptr)
    std::copy(right->begin(), right->end(), audio.getWritePointer(1));
  EXPECT_TRUE(AudioFiles::write(file, audio, rate)) << file.getFullPathName();
  return file;
}

// A WAV whose header says one thing and whose body is another: 16-bit PCM of
// `channels` at `rate`, a data chunk that claims `claimedFrames` and holds
// `actualFrames` (of silence). What a file from anywhere may look like.
//
// A plain RIFF file that claims more than it holds is already turned away by
// JUCE's reader (it reports no audio at all). `rf64` writes the 64-bit
// variant, where the length that counts is in a chunk of its own (ds64) and
// the reader takes it at its word: the forgery that gets through to us.
inline juce::File writeForgedWav(const juce::File& file, int channels, int rate,
                                 juce::int64 claimedFrames, juce::int64 actualFrames,
                                 bool rf64 = false) {
  const int frameBytes = channels * 2;
  juce::MemoryOutputStream out;
  if (rf64) {
    out.write("RF64", 4);
    out.writeInt(-1);
    out.write("WAVEds64", 8);
    out.writeInt(28);
    out.writeInt64(72 + claimedFrames * frameBytes);  // the RIFF chunk
    out.writeInt64(claimedFrames * frameBytes);       // the data chunk
    out.writeInt64(claimedFrames);                    // the sample count
    out.writeInt(0);                                  // no further table
    out.write("fmt ", 4);
  } else {
    out.write("RIFF", 4);
    out.writeInt(static_cast<int>(36 + claimedFrames * frameBytes));
    out.write("WAVEfmt ", 8);
  }
  out.writeInt(16);
  out.writeShort(1);  // PCM
  out.writeShort(static_cast<short>(channels));
  out.writeInt(rate);
  out.writeInt(rate * frameBytes);
  out.writeShort(static_cast<short>(frameBytes));
  out.writeShort(16);
  out.write("data", 4);
  // In RF64 the chunk's own 32-bit length is not the one that counts; a
  // reader checks it against the file all the same, so it is kept honest.
  out.writeInt(static_cast<int>((rf64 ? actualFrames : claimedFrames) * frameBytes));
  out.writeRepeatedByte(0, static_cast<size_t>(actualFrames * frameBytes));
  EXPECT_TRUE(file.getParentDirectory().createDirectory().wasOk());
  EXPECT_TRUE(file.replaceWithData(out.getData(), out.getDataSize())) << file.getFullPathName();
  return file;
}

// Something guitar-like for clips: plucked notes, each a few harmonics under
// a decaying envelope, two a second. `brightness` tilts the harmonics (0:
// dull, 1: bright).
inline std::vector<float> pluckedClip(double seconds, float amplitude, double brightness = 0.5,
                                      double rate = 48000.0) {
  const int frames = static_cast<int>(seconds * rate);
  const int noteFrames = static_cast<int>(rate / 2);
  const double notes[] = {110.0, 146.83, 196.0, 164.81};
  std::vector<float> x(static_cast<size_t>(frames));
  for (int i = 0; i < frames; ++i) {
    const double f0 = notes[(i / noteFrames) % 4];
    const double t = static_cast<double>(i % noteFrames) / rate;
    double v = 0.0, norm = 0.0;
    for (int h = 1; h <= 12; ++h) {
      const double weight = std::pow(static_cast<double>(h), -2.0 + 1.8 * brightness);
      v += weight * std::sin(2.0 * kPi * f0 * h * t) * std::exp(-t * (3.0 + h));
      norm += weight;
    }
    // A short ramp at each onset keeps the pluck from being a click.
    const double attack = std::min(1.0, t * 400.0);
    x[static_cast<size_t>(i)] = amplitude * static_cast<float>(attack * v / norm * 2.0);
  }
  return x;
}

// Pumps the message loop until `done` reads true or the time is up.
inline bool pumpUntil(const std::function<bool()>& done, int timeoutMs = 20000) {
  const auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(timeoutMs);
  while (!done()) {
    if (juce::Time::getMillisecondCounter() > deadline)
      return false;
    juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
  }
  return true;
}

// Runs `work` on a thread of its own while this thread, the message thread,
// keeps its loop turning: what a command server client needs in order to get
// answers. Assertions belong after it returns, on the results `work` left.
inline void runOffMessageThread(const std::function<void()>& work, int timeoutMs = 60000) {
  std::atomic<bool> finished{false};
  std::thread thread([&] {
    work();
    finished.store(true);
  });
  const bool intime = pumpUntil([&] { return finished.load(); }, timeoutMs);
  thread.join();
  EXPECT_TRUE(intime) << "the client thread did not finish in time";
}

// A client of the command server: lines over a socket.
struct LineClient {
  juce::StreamingSocket socket;
  std::string buffer;
  bool closed = false;
  std::vector<juce::var> events;
  int nextId = 1;

  bool connect(int port) { return socket.connect("127.0.0.1", port, 3000); }

  bool sendRaw(const std::string& bytes) {
    // Bounded on purpose: once the other end has gone, some platforms keep
    // reporting the socket writable while every write fails, so neither a
    // failed write nor a "ready" answer may be retried without limit.
    const auto deadline = juce::Time::getMillisecondCounter() + 5000u;
    size_t sent = 0;
    int stalls = 0;
    while (sent < bytes.size()) {
      const int count = socket.write(bytes.data() + sent, static_cast<int>(bytes.size() - sent));
      if (count > 0) {
        sent += static_cast<size_t>(count);
        stalls = 0;
        continue;
      }
      // A line larger than the socket's buffer goes out in pieces: wait for
      // room, a few times at most.
      if (++stalls > 8 || juce::Time::getMillisecondCounter() >= deadline)
        return false;
      if (socket.waitUntilReady(false, 500) != 1)
        return false;
    }
    return true;
  }

  bool sendLine(const juce::String& line) { return sendRaw(line.toStdString() + "\n"); }

  // The next line as JSON; void when none came in time or the server closed
  // the connection (then `closed` is set).
  juce::var readLine(int timeoutMs = 10000) {
    const auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(timeoutMs);
    for (;;) {
      const size_t end = buffer.find('\n');
      if (end != std::string::npos) {
        const std::string line = buffer.substr(0, end);
        buffer.erase(0, end + 1);
        juce::var value;
        juce::JSON::parse(juce::String::fromUTF8(line.c_str()), value);
        return value;
      }
      const auto now = juce::Time::getMillisecondCounter();
      if (now >= deadline)
        return {};
      const int ready = socket.waitUntilReady(true, static_cast<int>(deadline - now));
      if (ready < 0) {
        closed = true;
        return {};
      }
      if (ready == 0)
        return {};
      char chunk[8192];
      const int count = socket.read(chunk, static_cast<int>(sizeof(chunk)), false);
      if (count <= 0) {
        closed = true;
        return {};
      }
      buffer.append(chunk, static_cast<size_t>(count));
    }
  }

  // The next line that is an answer; events on the way are kept in `events`.
  juce::var readAnswer(int timeoutMs = 10000) {
    for (;;) {
      const juce::var line = readLine(timeoutMs);
      if (line.isVoid() || line["event"].isVoid())
        return line;
      events.push_back(line);
    }
  }

  juce::var hello(const juce::String& token, const juce::String& name = "test client") {
    sendLine("{\"auth\":" + agentQuoted(token) + ",\"client\":" + agentQuoted(name) + "}");
    return readAnswer();
  }

  juce::var call(const juce::String& command, const juce::String& argsJson = "{}",
                 int timeoutMs = 30000) {
    sendLine("{\"id\":" + juce::String(nextId++) + ",\"command\":" + agentQuoted(command) +
             ",\"args\":" + argsJson + "}");
    return readAnswer(timeoutMs);
  }
};

// The token a client reads from the connection file.
inline juce::String connectionToken() {
  juce::var info;
  juce::JSON::parse(CommandServer::connectionFile().loadFileAsString(), info);
  return info["token"].toString();
}

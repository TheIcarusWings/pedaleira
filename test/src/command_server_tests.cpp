// CommandServer: the command layer on a local socket. A connection has to
// present the token before anything else; requests are lines of JSON, one
// answer each; a client that sends too much, too fast or nonsense is dropped
// and nobody else notices; a slow command holds up only its own connection;
// change events arrive without polling; and with the server stopped nothing
// listens.
//
// Clients run on threads of their own while the test's thread, the message
// thread, keeps its loop turning (runOffMessageThread); they leave what they
// saw in plain variables and the assertions are made afterwards.

#include "agent_test_helpers.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if !JUCE_WINDOWS
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

// A rig with a server of its own, so each test sets the limits it is about.
struct ServedRig : AgentRig {
  std::unique_ptr<CommandServer> server{std::make_unique<CommandServer>(hub)};

  int start() {
    const juce::String error = server->start(0);
    EXPECT_EQ(error, "");
    EXPECT_TRUE(server->isRunning());
    EXPECT_GT(server->port(), 0);
    token = connectionToken();
    return server->port();
  }

  juce::String token;
};

double millisecondsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// A transport with no sockets: connections are scripts. Each receive runs
// the connection's next step, which hands over a line or does whatever a
// test wants a connection to do at that point, throwing included.
struct ScriptedConnectionState {
  using Received = CommandTransport::Connection::Received;
  std::mutex mutex;
  std::deque<std::function<Received(std::string&)>> steps;
  std::vector<std::string> sent;
  std::atomic<bool> closed{false};

  void line(const juce::String& text) {
    const std::lock_guard<std::mutex> lock(mutex);
    steps.push_back([text](std::string& message) {
      message = text.toStdString();
      return Received::message;
    });
  }
  void step(std::function<Received(std::string&)> action) {
    const std::lock_guard<std::mutex> lock(mutex);
    steps.push_back(std::move(action));
  }
  std::vector<juce::var> answers() {
    const std::lock_guard<std::mutex> lock(mutex);
    std::vector<juce::var> parsed;
    for (const auto& text : sent) {
      juce::var value;
      juce::JSON::parse(juce::String::fromUTF8(text.c_str()), value);
      parsed.push_back(value);
    }
    return parsed;
  }
};

struct ScriptedConnection final : CommandTransport::Connection {
  explicit ScriptedConnection(std::shared_ptr<ScriptedConnectionState> s) : state(std::move(s)) {}

  Received receive(std::string& message, int timeoutMs, int) override {
    std::function<Received(std::string&)> next;
    {
      const std::lock_guard<std::mutex> lock(state->mutex);
      if (!state->steps.empty()) {
        next = std::move(state->steps.front());
        state->steps.pop_front();
      }
    }
    if (next != nullptr)
      return next(message);
    if (state->closed.load())
      return Received::closed;
    juce::Thread::sleep(std::min(timeoutMs, 10));
    return Received::nothing;
  }
  bool send(const std::string& message) override {
    const std::lock_guard<std::mutex> lock(state->mutex);
    state->sent.push_back(message);
    return !state->closed.load();
  }
  void close() override { state->closed.store(true); }

  std::shared_ptr<ScriptedConnectionState> state;
};

struct ScriptedTransport final : CommandTransport {
  struct Queue {
    std::mutex mutex;
    std::condition_variable arrived;
    std::deque<std::shared_ptr<ScriptedConnectionState>> waiting;
    bool stopped = false;

    std::shared_ptr<ScriptedConnectionState> connect() {
      auto state = std::make_shared<ScriptedConnectionState>();
      return state;
    }
    void admit(const std::shared_ptr<ScriptedConnectionState>& state) {
      {
        const std::lock_guard<std::mutex> lock(mutex);
        waiting.push_back(state);
      }
      arrived.notify_all();
    }
  };
  std::shared_ptr<Queue> queue = std::make_shared<Queue>();

  bool listen(int) override { return true; }
  int boundPort() const override { return 1; }
  std::unique_ptr<Connection> accept() override {
    std::unique_lock<std::mutex> lock(queue->mutex);
    queue->arrived.wait(lock, [&] { return queue->stopped || !queue->waiting.empty(); });
    if (queue->waiting.empty())
      return nullptr;
    auto state = queue->waiting.front();
    queue->waiting.pop_front();
    return std::make_unique<ScriptedConnection>(std::move(state));
  }
  void stopListening() override {
    {
      const std::lock_guard<std::mutex> lock(queue->mutex);
      queue->stopped = true;
    }
    queue->arrived.notify_all();
  }
  juce::String name() const override { return "scripted"; }
};

// True once the server has closed its end of an idle socket.
bool socketWasClosed(juce::StreamingSocket& socket) {
  if (socket.waitUntilReady(true, 50) <= 0)
    return !socket.isConnected();
  char byte = 0;
  return socket.read(&byte, 1, false) <= 0;
}

}  // namespace

TEST(CommandServerTest, TheConnectionFileNamesThePortAndAFreshToken) {
  ServedRig rig;
  const juce::File file = CommandServer::connectionFile();
  EXPECT_FALSE(file.existsAsFile());
  const int port = rig.start();

  juce::var info;
  ASSERT_TRUE(juce::JSON::parse(file.loadFileAsString(), info).wasOk());
  EXPECT_EQ(static_cast<int>(info["port"]), port);
  EXPECT_EQ(info["host"].toString(), "127.0.0.1");
  EXPECT_EQ(info["transport"].toString(), "tcp-ndjson");
  EXPECT_EQ(static_cast<int>(info["protocol"]), CommandServer::kProtocolVersion);
  EXPECT_EQ(info["token"].toString().length(), 64);
  EXPECT_TRUE(info["token"].toString().containsOnly("0123456789abcdef"));
  EXPECT_GT(static_cast<int>(info["pid"]), 0);
#if !JUCE_WINDOWS
  // Readable and writable by the user and nobody else.
  struct stat status {};
  ASSERT_EQ(::stat(file.getFullPathName().toRawUTF8(), &status), 0);
  EXPECT_EQ(status.st_mode & 0777, 0600);
#endif

  // A restart makes a new token: an old one is worth nothing.
  const juce::String first = info["token"].toString();
  rig.server->stop();
  EXPECT_FALSE(file.existsAsFile());
  rig.start();
  EXPECT_NE(connectionToken(), first);
}

TEST(CommandServerTest, AConnectionMustPresentTheTokenFirst) {
  ServedRig rig;
  rig.server->limits.authTimeoutMs = 300;
  const int port = rig.start();

  juce::var noToken, wrongToken, rightToken, afterAuth, stateWithoutAuth, lateAuth;
  bool noTokenClosed = false, wrongClosed = false, silentClosed = false;
  runOffMessageThread([&] {
    {
      // A command instead of the token: refused, and the connection is over.
      LineClient client;
      client.connect(port);
      client.sendLine(R"({"id":1,"command":"state.get","args":{}})");
      noToken = client.readLine();
      stateWithoutAuth = client.readLine(1500);
      noTokenClosed = client.closed;
    }
    {
      LineClient client;
      client.connect(port);
      wrongToken = client.hello(juce::String::repeatedString("0", 64));
      client.sendLine(R"({"id":1,"command":"state.get"})");
      client.readLine(1500);
      wrongClosed = client.closed;
    }
    {
      // Saying nothing is no better: the wait is short.
      LineClient client;
      client.connect(port);
      client.readLine(2500);
      silentClosed = client.closed;
    }
    {
      LineClient client;
      client.connect(port);
      rightToken = client.hello(rig.token, "the right one");
      afterAuth = client.call("state.revision");
      // The token is asked for once; it is not a command.
      client.sendLine("{\"auth\":" + agentQuoted(rig.token) + "}");
      lateAuth = client.readLine(2000);
    }
  });

  EXPECT_FALSE(static_cast<bool>(noToken["ok"]));
  EXPECT_EQ(noToken["error"]["code"].toString(), "unauthorized");
  EXPECT_TRUE(stateWithoutAuth.isVoid()) << "a command ran before the token was presented";
  EXPECT_TRUE(noTokenClosed);
  EXPECT_EQ(wrongToken["error"]["code"].toString(), "unauthorized");
  EXPECT_TRUE(wrongClosed);
  EXPECT_TRUE(silentClosed);

  EXPECT_TRUE(static_cast<bool>(rightToken["ok"]));
  EXPECT_EQ(static_cast<int>(rightToken["result"]["protocol"]), 1);
  EXPECT_EQ(rightToken["result"]["client"].toString(), "the right one");
  EXPECT_GT(static_cast<int>(rightToken["result"]["sessionId"]), 0);
  EXPECT_TRUE(static_cast<bool>(afterAuth["ok"]));
  EXPECT_EQ(lateAuth["error"]["code"].toString(), "invalid_request");
}

TEST(CommandServerTest, RequestsAreLinesAndAnswersCarryTheirId) {
  ServedRig rig;
  const int port = rig.start();

  std::vector<juce::var> answers;
  juce::var unknown, badArgs, stringId, noId, split;
  runOffMessageThread([&] {
    LineClient client;
    client.connect(port);
    client.hello(rig.token);

    // Three requests in one write: three answers, in order, each with its id.
    client.sendRaw("{\"id\":7,\"command\":\"state.revision\"}\n"
                   "{\"id\":8,\"command\":\"tempo.get\",\"args\":{}}\r\n"
                   "{\"id\":9,\"command\":\"kinds.list\",\"args\":null}\n");
    for (int i = 0; i < 3; ++i)
      answers.push_back(client.readAnswer());

    // One request in two writes is still one request.
    client.sendRaw("{\"id\":10,\"comm");
    juce::Thread::sleep(150);
    client.sendRaw("and\":\"state.revision\"}\n");
    split = client.readAnswer();

    unknown = client.call("no.such.thing");
    badArgs = client.call("tempo.set", R"({"bpm":"fast"})");
    client.sendLine(R"({"id":"abc","command":"state.revision"})");
    stringId = client.readAnswer();
    client.sendLine(R"({"command":"state.revision"})");
    noId = client.readAnswer();
  });

  ASSERT_EQ(answers.size(), 3u);
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(static_cast<int>(answers[static_cast<size_t>(i)]["id"]), 7 + i);
    EXPECT_TRUE(static_cast<bool>(answers[static_cast<size_t>(i)]["ok"]));
  }
  EXPECT_GT(static_cast<int>(answers[0]["result"]["revision"]), 0);
  EXPECT_NEAR(static_cast<double>(answers[1]["result"]["bpm"]), 120.0, 1.0e-6);
  EXPECT_GT(answers[2]["result"]["kinds"].size(), 10);
  EXPECT_EQ(static_cast<int>(split["id"]), 10);
  EXPECT_TRUE(static_cast<bool>(split["ok"]));

  // The engine's own errors come through as they are.
  EXPECT_EQ(unknown["error"]["code"].toString(), "unknown_command");
  EXPECT_EQ(badArgs["error"]["code"].toString(), "invalid_args");
  EXPECT_TRUE(badArgs["error"]["message"].toString().contains("bpm"));
  EXPECT_EQ(stringId["id"].toString(), "abc");
  EXPECT_TRUE(noId["id"].isVoid());
  EXPECT_TRUE(static_cast<bool>(noId["ok"]));
}

TEST(CommandServerTest, OversizedAndGarbageInputDropsOnlyThatClient) {
  ServedRig rig;
  rig.server->limits.maxRequestBytes = 4096;
  rig.server->limits.maxBadRequests = 3;
  rig.server->limits.maxRequestsPerSecond = 40;
  const int port = rig.start();

  juce::var tooLarge, garbage1, garbageLast, flood, bystander;
  bool largeClosed = false, endlessClosed = false, garbageClosed = false, floodClosed = false;
  int garbageAnswers = 0;
  runOffMessageThread([&] {
    // Someone well behaved, connected throughout.
    LineClient good;
    good.connect(port);
    good.hello(rig.token, "bystander");

    {
      // A line beyond the limit.
      LineClient client;
      client.connect(port);
      client.hello(rig.token);
      client.sendLine("{\"id\":1,\"command\":\"state.get\",\"args\":{\"pad\":\"" +
                      juce::String::repeatedString("x", 8000) + "\"}}");
      tooLarge = client.readAnswer();
      client.readLine(1500);
      largeClosed = client.closed;
    }
    {
      // Bytes without end and without a line break: cut off at the limit,
      // not buffered for ever.
      LineClient client;
      client.connect(port);
      client.hello(rig.token);
      const std::string chunk(1024, 'y');
      for (int i = 0; i < 64 && client.sendRaw(chunk); ++i) {
      }
      for (int i = 0; i < 20 && !client.closed; ++i)
        client.readLine(200);
      endlessClosed = client.closed;
    }
    {
      // Lines that are not requests: answered a few times, then dropped.
      LineClient client;
      client.connect(port);
      client.hello(rig.token);
      client.sendLine("this is not json");
      garbage1 = client.readAnswer();
      ++garbageAnswers;
      for (const char* line : {"[1,2,3]", "{\"command\":42}", "\x01\x02\x03", "{}", "nope"}) {
        client.sendLine(line);
        const juce::var answer = client.readAnswer(1500);
        if (answer.isVoid())
          break;
        garbageLast = answer;
        ++garbageAnswers;
      }
      client.readLine(1000);
      garbageClosed = client.closed;
    }
    {
      // Requests as fast as they can be written.
      LineClient client;
      client.connect(port);
      client.hello(rig.token);
      std::string burst;
      for (int i = 0; i < 400; ++i)
        burst += "{\"id\":" + std::to_string(i) + ",\"command\":\"state.revision\"}\n";
      client.sendRaw(burst);
      for (int i = 0; i < 500 && !client.closed; ++i) {
        const juce::var answer = client.readLine(1500);
        if (answer.isVoid())
          break;
        if (!static_cast<bool>(answer["ok"]))
          flood = answer;
      }
      client.readLine(500);
      floodClosed = client.closed;
    }

    bystander = good.call("state.revision");
  });

  EXPECT_EQ(tooLarge["error"]["code"].toString(), "too_large");
  EXPECT_TRUE(largeClosed);
  EXPECT_TRUE(endlessClosed);
  EXPECT_EQ(garbage1["error"]["code"].toString(), "invalid_request");
  EXPECT_EQ(garbageLast["error"]["code"].toString(), "invalid_request");
  EXPECT_EQ(garbageAnswers, 3);
  EXPECT_TRUE(garbageClosed);
  EXPECT_EQ(flood["error"]["code"].toString(), "rate_limited");
  EXPECT_TRUE(floodClosed);

  // The one who behaved was served all along, and the rig is as it was.
  EXPECT_TRUE(static_cast<bool>(bystander["ok"]));
  EXPECT_TRUE(rig.local("state.get").ok);
}

TEST(CommandServerTest, ManyClientsAtOnceEachGetTheirOwnAnswers) {
  ServedRig rig;
  rig.server->limits.maxConnections = 4;
  const int port = rig.start();

  // Four: as many sandbox rigs as there may be.
  constexpr int kClients = 4, kCalls = 30;
  std::atomic<int> correct{0}, failures{0};
  juce::var refused;
  std::atomic<bool> release{false};
  runOffMessageThread([&] {
    std::atomic<int> connected{0};
    std::vector<std::thread> threads;
    for (int n = 0; n < kClients; ++n)
      threads.emplace_back([&, n] {
        LineClient client;
        if (!client.connect(port) || !static_cast<bool>(client.hello(rig.token, "client " + juce::String(n))["ok"])) {
          failures.fetch_add(kCalls);
          connected.fetch_add(1);
          return;
        }
        // Each works in a sandbox of its own, so the edits do not meet.
        const juce::var made = client.call("sandbox.create", R"({"from":"empty"})");
        connected.fetch_add(1);
        for (int i = 0; i < kCalls; ++i) {
          const double bpm = 60.0 + n * 10 + (i % 5);
          const juce::var set = client.call("tempo.set", "{\"bpm\":" + juce::String(bpm) + "}");
          const juce::var got = client.call("tempo.get");
          if (static_cast<bool>(set["ok"]) && static_cast<bool>(got["ok"]) &&
              std::abs(static_cast<double>(got["result"]["bpm"]) - bpm) < 1.0e-6)
            correct.fetch_add(1);
          else
            failures.fetch_add(1);
        }
        while (!release.load())
          juce::Thread::sleep(5);
      });

    while (connected.load() < kClients)
      juce::Thread::sleep(5);
    // The limit on connections: one more is told so and closed.
    LineClient extra;
    extra.connect(port);
    refused = extra.readLine(3000);
    release.store(true);
    for (auto& thread : threads)
      thread.join();
  });
  EXPECT_EQ(correct.load(), kClients * kCalls);
  EXPECT_EQ(failures.load(), 0);
  EXPECT_EQ(refused["error"]["code"].toString(), "unavailable");
}

TEST(CommandServerTest, ASlowCommandDoesNotHoldUpAnotherClient) {
  ServedRig rig;
  // A command that takes most of a second, the way a render does: its slow
  // part is work for the caller's thread.
  std::atomic<bool> slowStarted{false};
  rig.commands.registry().add({
      "test.slow",
      "Sleeps.",
      {},
      false,
      CommandPermission::read,
      [&slowStarted](const juce::var&) {
        return CommandWork::later([&slowStarted] {
          slowStarted.store(true);
          juce::Thread::sleep(800);
          return CommandResult::success(juce::var("slept"));
        });
      },
  });
  const int port = rig.start();

  double slowMs = 0.0, quickMs = 0.0;
  juce::var slowAnswer, quickAnswer;
  int quickCalls = 0;
  runOffMessageThread([&] {
    std::thread slow([&] {
      LineClient client;
      client.connect(port);
      client.hello(rig.token, "slow");
      const auto start = std::chrono::steady_clock::now();
      slowAnswer = client.call("test.slow");
      slowMs = millisecondsSince(start);
    });

    LineClient client;
    client.connect(port);
    client.hello(rig.token, "quick");
    while (!slowStarted.load())
      juce::Thread::sleep(2);
    // While the other is busy, this one is answered at once, many times.
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) {
      quickAnswer = client.call("state.revision");
      if (static_cast<bool>(quickAnswer["ok"]))
        ++quickCalls;
    }
    quickMs = millisecondsSince(start);
    slow.join();
  });

  EXPECT_EQ(slowAnswer["result"].toString(), "slept");
  EXPECT_GE(slowMs, 750.0);
  EXPECT_EQ(quickCalls, 20);
  EXPECT_LT(quickMs, 600.0) << "twenty quick calls waited for the slow one";
}

TEST(CommandServerTest, SubscribersAreToldWhenTheRevisionChanges) {
  ServedRig rig;
  const int port = rig.start();

  juce::var subscribed, afterUnsubscribe;
  std::vector<juce::var> liveEvents, sandboxEvents, lateEvents;
  int revisionAtSubscribe = 0;
  std::atomic<int> stage{0};
  std::thread clientThread;
  std::atomic<bool> finished{false};
  clientThread = std::thread([&] {
    LineClient client;
    client.connect(port);
    client.hello(rig.token, "watcher");
    subscribed = client.call("events.subscribe");
    revisionAtSubscribe = subscribed["result"]["currentRevision"];
    stage.store(1);

    // Stage 1: the user edits the live rig; this client only listens.
    for (int i = 0; i < 300 && !client.closed && (stage.load() < 2 || client.events.empty()); ++i) {
      const juce::var line = client.readLine(100);
      if (!line["event"].isVoid())
        client.events.push_back(line);
    }
    liveEvents = client.events;
    client.events.clear();

    // Stage 2: the client moves to a sandbox and edits it: the events
    // follow the rig it is working on.
    client.call("sandbox.create", R"({"from":"empty"})");
    // A live event still on its way when the client moved (the live edit's
    // revision can be sent after stage 1 stopped listening) is not the
    // sandbox's: only what follows the move counts.
    client.events.clear();
    client.call("chain.addBlock", R"({"kind":"utility.gain"})");
    for (int i = 0; i < 20; ++i) {
      const juce::var line = client.readLine(100);
      if (!line["event"].isVoid())
        client.events.push_back(line);
    }
    sandboxEvents = client.events;
    client.events.clear();

    // Stage 3: unsubscribed, nothing more arrives.
    afterUnsubscribe = client.call("events.subscribe", R"({"revision":false})");
    client.events.clear();  // what was already on its way before the answer
    client.call("chain.addBlock", R"({"kind":"utility.gain"})");
    for (int i = 0; i < 6; ++i) {
      const juce::var line = client.readLine(100);
      if (!line["event"].isVoid())
        client.events.push_back(line);
    }
    lateEvents = client.events;
    finished.store(true);
  });

  ASSERT_TRUE(pumpUntil([&] { return stage.load() >= 1; }));
  // Give the connection one poll to take its baseline, then change the rig.
  pumpUntil([] { return false; }, 300);
  rig.mustLocal("chain.addBlock", R"({"kind":"utility.gain"})");
  stage.store(2);
  EXPECT_TRUE(pumpUntil([&] { return finished.load(); }, 30000));
  clientThread.join();

  EXPECT_TRUE(static_cast<bool>(subscribed["ok"]));
  EXPECT_EQ(subscribed["result"]["rig"].toString(), "live");
  ASSERT_FALSE(liveEvents.empty());
  EXPECT_EQ(liveEvents.back()["event"].toString(), "revision");
  EXPECT_EQ(liveEvents.back()["rig"].toString(), "live");
  EXPECT_GT(static_cast<int>(liveEvents.back()["revision"]), revisionAtSubscribe);

  ASSERT_FALSE(sandboxEvents.empty());
  for (const auto& event : sandboxEvents)
    EXPECT_EQ(event["rig"].toString(), "sandbox-1");
  EXPECT_TRUE(lateEvents.empty());
  EXPECT_FALSE(static_cast<bool>(afterUnsubscribe["result"]["revision"]));
}

TEST(CommandServerTest, ClientsAreListedAndCanBeDisconnected) {
  ServedRig rig;
  const int port = rig.start();

  std::atomic<bool> ready{false}, finished{false};
  bool sawClose = false;
  std::thread clientThread([&] {
    LineClient client;
    client.connect(port);
    client.hello(rig.token, "Tone Agent");
    client.call("sandbox.create", R"({"from":"empty"})");
    client.call("state.revision");
    ready.store(true);
    for (int i = 0; i < 100 && !client.closed; ++i)
      client.readLine(100);
    sawClose = client.closed;
    finished.store(true);
  });
  ASSERT_TRUE(pumpUntil([&] { return ready.load(); }));

  const juce::var listed = rig.mustLocal("agents.list");
  ASSERT_EQ(listed["clients"].size(), 1);
  const juce::var client = listed["clients"][0];
  EXPECT_EQ(client["name"].toString(), "Tone Agent");
  EXPECT_EQ(client["lastCommand"].toString(), "state.revision");
  EXPECT_EQ(static_cast<int>(client["commandCount"]), 2);
  EXPECT_EQ(client["rig"].toString(), "sandbox-1");
  EXPECT_TRUE(client["connected"].toString().isNotEmpty());
  EXPECT_EQ(rig.server->connectionCount(), 1);

  EXPECT_EQ(rig.local("agents.disconnect", R"({"id":9999})").errorCode, "not_found");
  rig.mustLocal("agents.disconnect", "{\"id\":" + client["id"].toString() + "}");
  EXPECT_TRUE(pumpUntil([&] { return finished.load(); }, 15000));
  clientThread.join();
  EXPECT_TRUE(sawClose);

  // Gone from the list, and its sandbox with it.
  EXPECT_TRUE(pumpUntil([&] { return rig.mustLocal("agents.list")["clients"].size() == 0; }, 5000));
  EXPECT_TRUE(pumpUntil([&] { return rig.mustLocal("sandbox.list")["sandboxes"].size() == 0; }, 5000));
}

TEST(CommandServerTest, StoppedMeansNothingListens) {
  ServedRig rig;
  const int port = rig.start();
  {
    juce::StreamingSocket probe;
    EXPECT_TRUE(probe.connect("127.0.0.1", port, 2000));
  }

  // A client in the middle of things when the server stops is cut off, and
  // stopping does not wait on it.
  std::atomic<bool> ready{false}, finished{false};
  bool sawClose = false;
  std::thread clientThread([&] {
    LineClient client;
    client.connect(port);
    client.hello(rig.token);
    ready.store(true);
    for (int i = 0; i < 100 && !client.closed; ++i)
      client.readLine(100);
    sawClose = client.closed;
    finished.store(true);
  });
  ASSERT_TRUE(pumpUntil([&] { return ready.load(); }));

  const auto start = std::chrono::steady_clock::now();
  rig.server->stop();
  EXPECT_LT(millisecondsSince(start), 3000.0);
  EXPECT_FALSE(rig.server->isRunning());
  EXPECT_EQ(rig.server->port(), 0);
  EXPECT_FALSE(CommandServer::connectionFile().existsAsFile());
  EXPECT_TRUE(pumpUntil([&] { return finished.load(); }, 15000));
  clientThread.join();
  EXPECT_TRUE(sawClose);

  juce::StreamingSocket probe;
  EXPECT_FALSE(probe.connect("127.0.0.1", port, 500));
}

TEST(CommandServerTest, TheSettingStartsAndStopsTheHubsServer) {
  AgentRig rig;
  EXPECT_EQ(rig.hub.server(), nullptr);
  rig.hub.applyServerSetting();  // off by default: nothing starts
  EXPECT_EQ(rig.hub.server(), nullptr);

  const juce::var started = rig.mustLocal("server.setEnabled", R"({"enabled":true,"port":0})");
  EXPECT_TRUE(static_cast<bool>(started["running"]));
  EXPECT_TRUE(static_cast<bool>(started["enabled"]));
  const int port = started["port"];
  EXPECT_GT(port, 0);
  EXPECT_TRUE(AgentSettings::serverEnabled());
  ASSERT_NE(rig.hub.server(), nullptr);

  // A client gets through the hub's own server, the default permissions in
  // force: it reads, it is refused the local commands.
  juce::var read, localOnly;
  const juce::String token = connectionToken();
  runOffMessageThread([&] {
    LineClient client;
    client.connect(port);
    client.hello(token);
    read = client.call("state.get");
    localOnly = client.call("permissions.set", R"({"live":true})");
  });
  EXPECT_TRUE(static_cast<bool>(read["ok"]));
  EXPECT_EQ(localOnly["error"]["code"].toString(), "permission_denied");
  EXPECT_FALSE(rig.hub.policy().live);

  // The next start of the application brings it up again from the setting.
  {
    ChainTestProcessor next;
    EngineCommands nextCommands(next);
    rig.mustLocal("server.setEnabled", R"({"enabled":false})");
    EXPECT_EQ(rig.hub.server(), nullptr);
    AgentSettings::setServerEnabled(true);
    nextCommands.agents().applyServerSetting();
    ASSERT_NE(nextCommands.agents().server(), nullptr);
    EXPECT_TRUE(nextCommands.agents().server()->isRunning());
  }
  // ... and it went down with that instance.
  EXPECT_FALSE(CommandServer::connectionFile().existsAsFile());
  juce::StreamingSocket probe;
  EXPECT_FALSE(probe.connect("127.0.0.1", port, 500));
}

// ####################
// SECURITY REVIEW: WHAT A CONNECTION CANNOT DO TO THE PROCESS
// ####################

// JUCE's JSON parser recurses once per bracket and a connection's thread has
// a small stack: a line of nothing but brackets has to be turned away before
// the parser sees it, on the first line (the token) as on any later one.
TEST(CommandServerTest, ALineOfNestedBracketsIsRefusedBeforeItIsParsed) {
  ServedRig rig;
  rig.server->limits.maxBadRequests = 3;
  const int port = rig.start();

  const juce::String deepArray = juce::String::repeatedString("[", 200000);
  const juce::String deepObjects = juce::String::repeatedString("{\"a\":", 100000);
  juce::var firstLine, deepRequest, deepArgs, deepButClosed, allowedDepth, bystander, third;
  bool firstLineClosed = false, deepClosed = false;
  runOffMessageThread([&] {
    LineClient good;
    good.connect(port);
    good.hello(rig.token, "bystander");

    {
      // Where the token belongs: 4096 bytes is the most the first line may
      // be, and that many brackets is already too deep to parse safely.
      LineClient client;
      client.connect(port);
      client.sendLine(juce::String::repeatedString("[", 4000));
      firstLine = client.readLine();
      client.readLine(1500);
      firstLineClosed = client.closed;
    }
    {
      LineClient client;
      client.connect(port);
      client.hello(rig.token);
      client.sendLine(deepArray);
      deepRequest = client.readAnswer();
      // Inside an otherwise ordinary request, and closed properly.
      client.sendLine("{\"id\":5,\"command\":\"state.get\",\"args\":" + deepObjects + "1" +
                      juce::String::repeatedString("}", 100000) + "}");
      deepArgs = client.readAnswer();
      // Each counts as a line that is not a request: the third closes.
      client.sendLine("{\"id\":6,\"command\":\"state.get\",\"args\":" +
                      juce::String::repeatedString("[", 33) + juce::String::repeatedString("]", 33) +
                      "}");
      deepButClosed = client.readAnswer();
      client.sendLine(R"({"id":7,"command":"state.revision"})");
      third = client.readAnswer(1500);
      deepClosed = client.closed;
    }
    {
      // Thirty-two levels are a request like any other (the command then
      // has its own opinion of the argument).
      LineClient client;
      client.connect(port);
      client.hello(rig.token);
      client.sendLine("{\"id\":8,\"command\":\"state.get\",\"args\":{\"x\":" +
                      juce::String::repeatedString("[", 30) + juce::String::repeatedString("]", 30) +
                      "}}");
      allowedDepth = client.readAnswer();
    }
    bystander = good.call("state.revision");
  });

  EXPECT_EQ(firstLine["error"]["code"].toString(), "unauthorized");
  EXPECT_TRUE(firstLineClosed);
  EXPECT_EQ(deepRequest["error"]["code"].toString(), "invalid_request");
  EXPECT_EQ(deepArgs["error"]["code"].toString(), "invalid_request");
  EXPECT_EQ(deepButClosed["error"]["code"].toString(), "invalid_request");
  EXPECT_TRUE(deepButClosed["error"]["message"].toString().contains("32"));
  EXPECT_TRUE(third.isVoid()) << "the third bad line did not close the connection";
  EXPECT_TRUE(deepClosed);
  EXPECT_EQ(allowedDepth["error"]["code"].toString(), "invalid_args");
  EXPECT_EQ(static_cast<int>(allowedDepth["id"]), 8);
  EXPECT_TRUE(static_cast<bool>(bystander["ok"]));
  EXPECT_TRUE(rig.local("state.get").ok);
}

// An exception on a connection's thread (out of memory on a large line,
// anything a transport throws) ends that connection with an error. It never
// leaves the thread, where it would end the process.
TEST(CommandServerTest, AFailureWhileServingClosesOnlyThatConnection) {
  AgentRig rig;
  auto transport = std::make_unique<ScriptedTransport>();
  const auto queue = transport->queue;
  CommandServer server(rig.hub, std::move(transport));
  ASSERT_EQ(server.start(0), "");
  const juce::String hello = "{\"auth\":" + agentQuoted(connectionToken()) + ",\"client\":\"scripted\"}";
  using Received = CommandTransport::Connection::Received;

  // Out of memory in the middle of a session, with a sandbox of its own.
  const auto starved = queue->connect();
  starved->line(hello);
  starved->line(R"({"id":1,"command":"sandbox.create","args":{"from":"empty"}})");
  starved->step([](std::string&) -> Received { throw std::bad_alloc(); });
  // Something that is not even a std::exception.
  const auto odd = queue->connect();
  odd->line(hello);
  odd->step([](std::string&) -> Received { throw 42; });
  // And before the token: the first receive throws.
  const auto early = queue->connect();
  early->step([](std::string&) -> Received { throw std::runtime_error("broken transport"); });
  queue->admit(starved);
  ASSERT_TRUE(pumpUntil([&] { return starved->closed.load(); }));
  queue->admit(odd);
  ASSERT_TRUE(pumpUntil([&] { return odd->closed.load(); }));
  queue->admit(early);
  ASSERT_TRUE(pumpUntil([&] { return early->closed.load(); }));

  const auto starvedAnswers = starved->answers();
  ASSERT_EQ(starvedAnswers.size(), 3u);
  EXPECT_TRUE(static_cast<bool>(starvedAnswers[0]["ok"]));
  EXPECT_TRUE(static_cast<bool>(starvedAnswers[1]["ok"]));
  EXPECT_EQ(starvedAnswers[2]["error"]["code"].toString(), "failed");
  EXPECT_TRUE(starvedAnswers[2]["error"]["message"].toString().contains("memory"));
  ASSERT_EQ(odd->answers().size(), 2u);
  EXPECT_EQ(odd->answers()[1]["error"]["code"].toString(), "failed");
  ASSERT_EQ(early->answers().size(), 1u);
  EXPECT_EQ(early->answers()[0]["error"]["code"].toString(), "failed");

  // Their sessions are closed like any other: gone from the list, and the
  // sandbox with them.
  EXPECT_TRUE(pumpUntil([&] { return rig.mustLocal("agents.list")["clients"].size() == 0; }, 5000));
  EXPECT_TRUE(pumpUntil([&] { return rig.mustLocal("sandbox.list")["sandboxes"].size() == 0; }, 5000));

  // The server is still a server.
  const auto next = queue->connect();
  next->line(hello);
  next->line(R"({"id":9,"command":"state.revision"})");
  queue->admit(next);
  ASSERT_TRUE(pumpUntil([&] { return next->answers().size() >= 2; }));
  EXPECT_TRUE(static_cast<bool>(next->answers()[1]["ok"]));
  EXPECT_EQ(static_cast<int>(next->answers()[1]["id"]), 9);
  EXPECT_TRUE(server.isRunning());
  server.stop();
  EXPECT_TRUE(next->closed.load());
}

// A client may ask to wait ten minutes for the rig being played to load.
// Quitting, or switching the server off, must not wait with it.
TEST(CommandServerTest, StopDoesNotWaitForAClientInsideALongWait) {
  ServedRig rig;
  const int port = rig.start();

  // The stop is made in the message loop turn right behind the one that
  // starts the command, so the client is inside its wait (or about to look
  // for the first time) with nobody left to answer its looks.
  std::atomic<bool> armed{false}, posted{false}, stopped{false};
  double stopMs = -1.0;
  const std::function<void()> previous = rig.commands.registry().beforeRun;
  rig.commands.registry().beforeRun = [&] {
    if (previous != nullptr)
      previous();
    if (armed.load() && !posted.exchange(true))
      juce::MessageManager::callAsync([&] {
        const auto start = std::chrono::steady_clock::now();
        rig.server->stop();
        stopMs = millisecondsSince(start);
        stopped.store(true);
      });
  };

  juce::var answer;
  std::atomic<bool> finished{false};
  double clientMs = 0.0;
  std::thread clientThread([&] {
    LineClient client;
    client.connect(port);
    client.hello(rig.token, "patient");
    armed.store(true);
    const auto start = std::chrono::steady_clock::now();
    answer = client.call("state.waitUntilLoaded", R"({"timeoutMs":600000})", 20000);
    clientMs = millisecondsSince(start);
    finished.store(true);
  });

  EXPECT_TRUE(pumpUntil([&] { return stopped.load(); }, 30000));
  EXPECT_TRUE(pumpUntil([&] { return finished.load(); }, 30000));
  clientThread.join();
  rig.commands.registry().beforeRun = previous;

  EXPECT_GE(stopMs, 0.0);
  EXPECT_LT(stopMs, 2000.0) << "stopping the server waited for a client's state.waitUntilLoaded";
  EXPECT_LT(clientMs, 5000.0);
  // The wait was cut short, not answered: cancelled, or the line was gone
  // before the answer could be written.
  if (!answer.isVoid()) {
    EXPECT_EQ(answer["error"]["code"].toString(), "unavailable") << juce::JSON::toString(answer);
  }
  EXPECT_FALSE(rig.server->isRunning());
}

// The same for agents.disconnect, through the hub's own server: a client in
// the middle of slow work is let go at once, not when the work is over.
TEST(CommandServerTest, DisconnectingAClientEndsItsLongWait) {
  AgentRig rig;
  const juce::var started = rig.mustLocal("server.setEnabled", R"({"enabled":true,"port":0})");
  const int port = started["port"];
  const juce::String token = connectionToken();

  // Slow work that only ends when it is told to: it reads the flag the
  // command layer hands every slow command (the calling session's).
  std::atomic<bool> waiting{false};
  rig.commands.registry().add({
      "test.waitForever",
      "Waits like state.waitUntilLoaded on a rig that never settles.",
      {},
      false,
      CommandPermission::read,
      [&](const juce::var&) {
        const std::atomic<bool>* cancel = rig.commands.slowWorkCancel();
        return CommandWork::later([&waiting, cancel] {
          waiting.store(true);
          const auto start = std::chrono::steady_clock::now();
          while (!cancel->load() && millisecondsSince(start) < 60000.0)
            juce::Thread::sleep(5);
          return cancel->load() ? CommandResult::failure(CommandError::unavailable, "cancelled")
                                : CommandResult::success();
        });
      },
  });

  juce::var answer;
  std::atomic<bool> finished{false};
  std::thread clientThread([&] {
    LineClient client;
    client.connect(port);
    client.hello(token, "patient");
    answer = client.call("test.waitForever", "{}", 30000);
    finished.store(true);
  });
  ASSERT_TRUE(pumpUntil([&] { return waiting.load(); }));

  const juce::var listed = rig.mustLocal("agents.list");
  ASSERT_EQ(listed["clients"].size(), 1);
  const auto start = std::chrono::steady_clock::now();
  rig.mustLocal("agents.disconnect", "{\"id\":" + listed["clients"][0]["id"].toString() + "}");
  EXPECT_TRUE(pumpUntil([&] { return finished.load(); }, 10000));
  EXPECT_LT(millisecondsSince(start), 2000.0);
  clientThread.join();
  EXPECT_EQ(answer["error"]["code"].toString(), "unavailable");
}

#if !JUCE_WINDOWS
// The connection file holds the token. It is never made through a link or
// in a file somebody else put there, never readable by others for a moment,
// in a folder of the user's own, and it is only removed by the server it
// belongs to.
TEST(CommandServerTest, TheConnectionFileIsMadePrivatelyAndRemovedOnlyByItsOwner) {
  ServedRig rig;
  const juce::File folder = rig.dirs.dir.getChildFile("fresh").getChildFile("Pedaleira");
  const juce::File file = folder.getChildFile("command-server.json");
  CommandServer::setConnectionFileForTesting(file);
  const auto modeOf = [](const juce::File& path) {
    struct stat status {};
    EXPECT_EQ(::lstat(path.getFullPathName().toRawUTF8(), &status), 0) << path.getFullPathName();
    return status;
  };
  const auto infoIn = [](const juce::File& path) {
    juce::var info;
    juce::JSON::parse(path.loadFileAsString(), info);
    return info;
  };

  rig.start();
  // The folders it had to make are the user's alone.
  EXPECT_EQ(modeOf(folder).st_mode & 0777, 0700);
  EXPECT_EQ(modeOf(folder.getParentDirectory()).st_mode & 0777, 0700);
  EXPECT_TRUE(S_ISREG(modeOf(file).st_mode));
  EXPECT_EQ(modeOf(file).st_mode & 0777, 0600);
  EXPECT_EQ(static_cast<int>(infoIn(file)["pid"]), static_cast<int>(::getpid()));
  // Nothing of the temporary file it was written as is left.
  EXPECT_EQ(folder.getNumberOfChildFiles(juce::File::findFilesAndDirectories), 1);
  rig.server->stop();
  EXPECT_FALSE(file.exists());

  // A file left by a process that has gone is replaced.
  const juce::String stale = R"({"protocol":1,"host":"127.0.0.1","port":1,"token":"old","pid":2147483600})";
  ASSERT_TRUE(file.replaceWithText(stale));
  ::chmod(file.getFullPathName().toRawUTF8(), 0644);
  rig.start();
  EXPECT_EQ(static_cast<int>(infoIn(file)["pid"]), static_cast<int>(::getpid()));
  EXPECT_EQ(infoIn(file)["token"].toString().length(), 64);
  EXPECT_EQ(modeOf(file).st_mode & 0777, 0600);

  // Another instance has written its own since: stopping leaves that alone.
  ASSERT_TRUE(file.replaceWithText(stale));
  rig.server->stop();
  EXPECT_TRUE(file.existsAsFile()) << "a server removed a connection file that was not its own";
  EXPECT_EQ(infoIn(file)["token"].toString(), "old");

  // A link where the file goes is replaced, not written through.
  const juce::File victim = rig.dirs.outside.getChildFile("victim.txt");
  ASSERT_TRUE(victim.replaceWithText("untouched"));
  ASSERT_TRUE(file.deleteFile());
  ASSERT_TRUE(victim.createSymbolicLink(file, true));
  rig.start();
  EXPECT_TRUE(S_ISREG(modeOf(file).st_mode));
  EXPECT_EQ(victim.loadFileAsString(), "untouched");
  EXPECT_EQ(infoIn(file)["token"].toString(), rig.token);
  EXPECT_EQ(folder.getNumberOfChildFiles(juce::File::findFilesAndDirectories), 1);
  rig.server->stop();
  EXPECT_FALSE(file.exists());
}
#endif

// Connections that never present the token have a small allowance of their
// own. However many of them there are, they take none of the clients'
// places, and a client that does present it gets in.
TEST(CommandServerTest, SocketsWithoutATokenCannotKeepAClientOut) {
  ServedRig rig;
  rig.server->limits.authTimeoutMs = 20000;  // they would sit there all test long
  rig.server->limits.unauthenticatedGraceMs = 60;
  const int port = rig.start();

  constexpr int kIdle = 12;  // more than there are places for clients
  std::vector<std::unique_ptr<juce::StreamingSocket>> idle;
  juce::var hello, answer, second;
  int idleStillOpen = 0;
  runOffMessageThread([&] {
    for (int i = 0; i < kIdle; ++i) {
      idle.push_back(std::make_unique<juce::StreamingSocket>());
      idle.back()->connect("127.0.0.1", port, 3000);
    }
    LineClient client;
    client.connect(port);
    hello = client.hello(rig.token, "the real one");
    answer = client.call("state.revision");
    // And another, with the first still connected.
    LineClient other;
    other.connect(port);
    second = other.hello(rig.token, "another");
    for (auto& socket : idle)
      idleStillOpen += socketWasClosed(*socket) ? 0 : 1;
  });

  EXPECT_TRUE(static_cast<bool>(hello["ok"])) << juce::JSON::toString(hello);
  EXPECT_TRUE(static_cast<bool>(answer["ok"]));
  EXPECT_TRUE(static_cast<bool>(second["ok"]));
  EXPECT_LE(idleStillOpen, rig.server->limits.maxUnauthenticated);
}

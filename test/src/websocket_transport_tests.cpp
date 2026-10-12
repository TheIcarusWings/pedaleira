// The WebSocket transport of the command server (RFC 6455): the upgrade and
// the refusals that go with it, masked text frames in, unmasked text frames
// out, fragments, ping, close codes, and the size limit judged from a frame's
// header. Every test talks to a real server over a real socket, with a small
// client that can send what a well behaved browser never would.

#include "agent_test_helpers.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

const char* const kRfcKey = "dGhlIHNhbXBsZSBub25jZQ==";
const char* const kRfcAccept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

// A rig whose server speaks WebSocket.
struct WsRig : AgentRig {
  std::unique_ptr<CommandServer> server;
  juce::String token;

  explicit WsRig(std::vector<juce::String> origins = {})
      : server(std::make_unique<CommandServer>(hub, makeWebSocketTransport(std::move(origins)))) {}

  int start() {
    const juce::String error = server->start(0);
    EXPECT_EQ(error, "");
    EXPECT_TRUE(server->isRunning());
    token = connectionToken();
    return server->port();
  }
};

struct Frame {
  bool ok = false;
  bool fin = false;
  int opcode = -1;
  std::string payload;

  // The status of a close frame; -1 when there is none.
  int status() const {
    if (opcode != 0x8 || payload.size() < 2)
      return -1;
    return (static_cast<unsigned char>(payload[0]) << 8) | static_cast<unsigned char>(payload[1]);
  }
};

struct WsClient {
  juce::StreamingSocket socket;
  std::string buffer;
  bool closed = false;

  bool connect(int port) { return socket.connect("127.0.0.1", port, 3000); }

  bool sendRaw(const std::string& bytes) {
    size_t sent = 0;
    int stalls = 0;
    while (sent < bytes.size()) {
      const int count = socket.write(bytes.data() + sent, static_cast<int>(bytes.size() - sent));
      if (count > 0) {
        sent += static_cast<size_t>(count);
        stalls = 0;
        continue;
      }
      if (++stalls > 8 || socket.waitUntilReady(false, 500) != 1)
        return false;
    }
    return true;
  }

  static std::string request(int port, const std::string& key = kRfcKey,
                             const std::string& origin = {}, const std::string& host = {},
                             const std::string& version = "13") {
    std::string text = "GET /remote HTTP/1.1\r\nHost: " +
                       (host.empty() ? "127.0.0.1:" + std::to_string(port) : host) +
                       "\r\nUpgrade: WebSocket\r\nConnection: keep-alive, Upgrade\r\n";
    if (!key.empty())
      text += "Sec-WebSocket-Key: " + key + "\r\n";
    if (!version.empty())
      text += "Sec-WebSocket-Version: " + version + "\r\n";
    if (!origin.empty())
      text += "Origin: " + origin + "\r\n";
    return text + "\r\n";
  }

  bool fill(int timeoutMs) {
    if (socket.waitUntilReady(true, timeoutMs) <= 0)
      return false;
    char chunk[8192];
    const int count = socket.read(chunk, static_cast<int>(sizeof(chunk)), false);
    if (count <= 0) {
      closed = true;
      return false;
    }
    buffer.append(chunk, static_cast<size_t>(count));
    return true;
  }

  // The response head, through the blank line; empty when none came.
  std::string readHead(int timeoutMs = 5000) {
    const auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(timeoutMs);
    for (;;) {
      const size_t end = buffer.find("\r\n\r\n");
      if (end != std::string::npos) {
        const std::string head = buffer.substr(0, end + 4);
        buffer.erase(0, end + 4);
        return head;
      }
      const auto now = juce::Time::getMillisecondCounter();
      if (now >= deadline || closed || !fill(static_cast<int>(deadline - now)))
        return {};
    }
  }

  // Connects, upgrades, and returns true on a 101.
  bool upgrade(int port) {
    if (!connect(port) || !sendRaw(request(port)))
      return false;
    return readHead().rfind("HTTP/1.1 101", 0) == 0;
  }

  static std::string maskedFrame(int opcode, const std::string& payload, bool fin = true,
                                 bool masked = true) {
    std::string frame;
    frame.push_back(static_cast<char>((fin ? 0x80 : 0) | opcode));
    const int maskBit = masked ? 0x80 : 0;
    if (payload.size() <= 125) {
      frame.push_back(static_cast<char>(maskBit | payload.size()));
    } else {
      frame.push_back(static_cast<char>(maskBit | 126));
      frame.push_back(static_cast<char>((payload.size() >> 8) & 0xff));
      frame.push_back(static_cast<char>(payload.size() & 0xff));
    }
    const unsigned char mask[4] = {0x12, 0x34, 0x56, 0x78};
    if (masked)
      frame.append(reinterpret_cast<const char*>(mask), 4);
    for (size_t i = 0; i < payload.size(); ++i)
      frame.push_back(
          masked ? static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask[i % 4]) : payload[i]);
    return frame;
  }

  bool sendFrame(int opcode, const std::string& payload, bool fin = true, bool masked = true) {
    return sendRaw(maskedFrame(opcode, payload, fin, masked));
  }

  // Only a header that declares `length` bytes (64-bit form), masked.
  bool sendHeaderOnly(int opcode, uint64_t length) {
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | opcode));
    frame.push_back(static_cast<char>(0x80 | 127));
    for (int shift = 56; shift >= 0; shift -= 8)
      frame.push_back(static_cast<char>((length >> shift) & 0xff));
    frame.append("\x01\x02\x03\x04", 4);
    return sendRaw(frame);
  }

  Frame readFrame(int timeoutMs = 10000) {
    const auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(timeoutMs);
    for (;;) {
      if (buffer.size() >= 2) {
        const auto* b = reinterpret_cast<const unsigned char*>(buffer.data());
        size_t header = 2;
        uint64_t length = b[1] & 0x7f;
        bool complete = true;
        if (length == 126) {
          header = 4;
          complete = buffer.size() >= 4;
          if (complete)
            length = (static_cast<uint64_t>(b[2]) << 8) | b[3];
        } else if (length == 127) {
          header = 10;
          complete = buffer.size() >= 10;
          if (complete) {
            length = 0;
            for (int i = 0; i < 8; ++i)
              length = (length << 8) | b[2 + i];
          }
        }
        if (complete && buffer.size() >= header + length) {
          Frame frame;
          frame.ok = true;
          frame.fin = (b[0] & 0x80) != 0;
          frame.opcode = b[0] & 0x0f;
          frame.payload = buffer.substr(header, static_cast<size_t>(length));
          buffer.erase(0, header + static_cast<size_t>(length));
          return frame;
        }
      }
      const auto now = juce::Time::getMillisecondCounter();
      if (now >= deadline || closed || !fill(static_cast<int>(deadline - now)))
        return {};
    }
  }

  juce::var readJson(int timeoutMs = 10000) {
    const Frame frame = readFrame(timeoutMs);
    juce::var value;
    if (frame.ok && frame.opcode == 0x1)
      juce::JSON::parse(juce::String::fromUTF8(frame.payload.c_str()), value);
    return value;
  }

  // True once the server has closed its end (what is still unread is dropped).
  bool waitClosed(int timeoutMs = 3000) {
    const auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(timeoutMs);
    while (!closed && juce::Time::getMillisecondCounter() < deadline)
      fill(50);
    return closed;
  }

  // Upgrades and presents the token; true on the server's ok.
  bool authenticate(int port, const juce::String& token) {
    if (!upgrade(port))
      return false;
    sendFrame(0x1, "{\"auth\":" + agentQuoted(token).toStdString() + ",\"client\":\"ws test\"}");
    return static_cast<bool>(readJson()["ok"]);
  }
};

std::string headerLine(const std::string& head, const std::string& name) {
  const size_t at = head.find(name + ": ");
  if (at == std::string::npos)
    return {};
  const size_t from = at + name.size() + 2;
  return head.substr(from, head.find("\r\n", from) - from);
}

}  // namespace

TEST(WebSocketTransportTest, TheAcceptKeyIsTheOneInTheRfc) {
  WsRig rig;
  const int port = rig.start();
  std::string head;
  runOffMessageThread([&] {
    WsClient client;
    ASSERT_TRUE(client.connect(port));
    ASSERT_TRUE(client.sendRaw(WsClient::request(port)));
    head = client.readHead();
  });
  EXPECT_EQ(head.rfind("HTTP/1.1 101 Switching Protocols\r\n", 0), 0u);
  EXPECT_EQ(headerLine(head, "Sec-WebSocket-Accept"), kRfcAccept);
  EXPECT_EQ(headerLine(head, "Upgrade"), "websocket");
  EXPECT_EQ(headerLine(head, "Connection"), "Upgrade");
  EXPECT_EQ(headerLine(head, "Sec-WebSocket-Extensions"), "");
}

TEST(WebSocketTransportTest, TheConnectionFileNamesTheTransport) {
  WsRig rig;
  rig.start();
  juce::var info;
  ASSERT_TRUE(juce::JSON::parse(CommandServer::connectionFile().loadFileAsString(), info).wasOk());
  EXPECT_EQ(info["transport"].toString(), "websocket");
}

TEST(WebSocketTransportTest, ACommandIsAnsweredOverWebSocket) {
  WsRig rig;
  const int port = rig.start();
  juce::var rejected, answer;
  bool authenticated = false;
  runOffMessageThread([&] {
    {
      WsClient client;
      if (client.upgrade(port)) {
        client.sendFrame(0x1, "{\"auth\":\"nope\",\"client\":\"x\"}");
        rejected = client.readJson();
      }
    }
    WsClient client;
    authenticated = client.authenticate(port, rig.token);
    client.sendFrame(0x1, R"({"id":41,"command":"kinds.list","args":{}})");
    answer = client.readJson();
  });
  EXPECT_FALSE(static_cast<bool>(rejected["ok"]));
  EXPECT_EQ(rejected["error"]["code"].toString(), "unauthorized");
  EXPECT_TRUE(authenticated);
  EXPECT_EQ(static_cast<int>(answer["id"]), 41);
  EXPECT_TRUE(static_cast<bool>(answer["ok"]));
}

TEST(WebSocketTransportTest, AFragmentedRequestIsAnswered) {
  WsRig rig;
  const int port = rig.start();
  juce::var answer;
  runOffMessageThread([&] {
    WsClient client;
    ASSERT_TRUE(client.authenticate(port, rig.token));
    const std::string request = R"({"id":5,"command":"kinds.list","args":{}})";
    client.sendFrame(0x1, request.substr(0, 10), false);
    client.sendFrame(0x9, "mid", true);  // a ping between the fragments is allowed
    client.sendFrame(0x0, request.substr(10, 12), false);
    client.sendFrame(0x0, request.substr(22), true);
    const Frame pong = client.readFrame();
    EXPECT_EQ(pong.opcode, 0xA);
    EXPECT_EQ(pong.payload, "mid");
    answer = client.readJson();
  });
  EXPECT_EQ(static_cast<int>(answer["id"]), 5);
  EXPECT_TRUE(static_cast<bool>(answer["ok"]));
}

TEST(WebSocketTransportTest, APingGetsAPongWithTheSamePayload) {
  WsRig rig;
  const int port = rig.start();
  Frame pong;
  juce::var answer;
  runOffMessageThread([&] {
    WsClient client;
    ASSERT_TRUE(client.authenticate(port, rig.token));
    client.sendFrame(0x9, "are you there");
    pong = client.readFrame();
    client.sendFrame(0xA, "unasked");  // a pong is ignored
    client.sendFrame(0x1, R"({"id":2,"command":"state.get","args":{}})");
    answer = client.readJson();
  });
  EXPECT_EQ(pong.opcode, 0xA);
  EXPECT_TRUE(pong.fin);
  EXPECT_EQ(pong.payload, "are you there");
  EXPECT_EQ(static_cast<int>(answer["id"]), 2);
}

TEST(WebSocketTransportTest, ALargeAnswerUsesTheLongLengthForms) {
  WsRig rig;
  const int port = rig.start();
  juce::var answer;
  runOffMessageThread([&] {
    WsClient client;
    ASSERT_TRUE(client.authenticate(port, rig.token));
    // A request of 300 bytes goes out in the 16-bit form; the answer to
    // state.get is long enough for it as well.
    client.sendFrame(0x1, R"({"id":9,"command":"state.get","args":{},"pad":")" +
                              std::string(300, 'x') + "\"}");
    answer = client.readJson();
  });
  EXPECT_EQ(static_cast<int>(answer["id"]), 9);
  EXPECT_TRUE(static_cast<bool>(answer["ok"]));
}

TEST(WebSocketTransportTest, RequestsThatAreNotAcceptableAreRefusedAndClosed) {
  WsRig rig({"http://localhost:5173"});
  const int port = rig.start();

  struct Case {
    std::string name;
    std::string request;
    const char* status;
  };
  const std::vector<Case> cases = {
      {"wrong host", WsClient::request(port, kRfcKey, {}, "evil.example:" + std::to_string(port)),
       "HTTP/1.1 403"},
      {"host without the port", WsClient::request(port, kRfcKey, {}, "127.0.0.1"), "HTTP/1.1 403"},
      {"foreign origin", WsClient::request(port, kRfcKey, "http://evil.example"), "HTTP/1.1 403"},
      {"origin with another port", WsClient::request(port, kRfcKey, "http://localhost:5174"),
       "HTTP/1.1 403"},
      {"missing key", WsClient::request(port, ""), "HTTP/1.1 400"},
      {"key of the wrong size", WsClient::request(port, "c2hvcnQ="), "HTTP/1.1 400"},
      {"missing version", WsClient::request(port, kRfcKey, {}, {}, ""), "HTTP/1.1 400"},
      {"old version", WsClient::request(port, kRfcKey, {}, {}, "8"), "HTTP/1.1 426"},
      {"not an upgrade",
       "GET / HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\n\r\n", "HTTP/1.1 400"},
      {"wrong method", "POST / HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\n\r\n",
       "HTTP/1.1 400"},
  };
  std::vector<std::string> heads(cases.size());
  std::vector<bool> closedAfter(cases.size(), false);
  runOffMessageThread([&] {
    for (size_t i = 0; i < cases.size(); ++i) {
      WsClient client;
      ASSERT_TRUE(client.connect(port));
      client.sendRaw(cases[i].request);
      heads[i] = client.readHead();
      closedAfter[i] = client.waitClosed();
    }
  });
  for (size_t i = 0; i < cases.size(); ++i) {
    SCOPED_TRACE(cases[i].name);
    EXPECT_EQ(heads[i].rfind(cases[i].status, 0), 0u) << heads[i];
    EXPECT_TRUE(closedAfter[i]);
  }
  EXPECT_EQ(headerLine(heads[7], "Sec-WebSocket-Version"), "13");
}

TEST(WebSocketTransportTest, AnOversizedHeaderIsRefusedWith431) {
  WsRig rig;
  const int port = rig.start();
  std::string head;
  bool closed = false;
  runOffMessageThread([&] {
    WsClient client;
    ASSERT_TRUE(client.connect(port));
    client.sendRaw("GET / HTTP/1.1\r\nX-Pad: " + std::string(9000, 'a') + "\r\n\r\n");
    head = client.readHead();
    closed = client.waitClosed();
  });
  EXPECT_EQ(head.rfind("HTTP/1.1 431", 0), 0u) << head;
  EXPECT_TRUE(closed);
}

TEST(WebSocketTransportTest, AnAllowedOriginIsUpgraded) {
  WsRig rig({"http://localhost:5173"});
  const int port = rig.start();
  std::string head, nativeHead;
  runOffMessageThread([&] {
    {
      WsClient client;
      ASSERT_TRUE(client.connect(port));
      client.sendRaw(WsClient::request(port, kRfcKey, "HTTP://LocalHost:5173", "localhost:" + std::to_string(port)));
      head = client.readHead();
    }
    {
      // No Origin at all is a native client, and welcome.
      WsClient client;
      ASSERT_TRUE(client.connect(port));
      client.sendRaw(WsClient::request(port));
      nativeHead = client.readHead();
    }
  });
  EXPECT_EQ(head.rfind("HTTP/1.1 101", 0), 0u) << head;
  EXPECT_EQ(nativeHead.rfind("HTTP/1.1 101", 0), 0u) << nativeHead;
}

TEST(WebSocketTransportTest, AnUnmaskedFrameClosesWith1002) {
  WsRig rig;
  const int port = rig.start();
  Frame close;
  bool closed = false;
  runOffMessageThread([&] {
    WsClient client;
    ASSERT_TRUE(client.authenticate(port, rig.token));
    client.sendFrame(0x1, R"({"id":1,"command":"state.get","args":{}})", true, /*masked=*/false);
    close = client.readFrame();
    closed = client.waitClosed();
  });
  EXPECT_EQ(close.opcode, 0x8);
  EXPECT_EQ(close.status(), 1002);
  EXPECT_TRUE(closed);
}

TEST(WebSocketTransportTest, ABinaryFrameClosesWith1003) {
  WsRig rig;
  const int port = rig.start();
  Frame close;
  bool closed = false;
  runOffMessageThread([&] {
    WsClient client;
    ASSERT_TRUE(client.authenticate(port, rig.token));
    client.sendFrame(0x2, "\x01\x02");
    close = client.readFrame();
    closed = client.waitClosed();
  });
  EXPECT_EQ(close.status(), 1003);
  EXPECT_TRUE(closed);
}

TEST(WebSocketTransportTest, BrokenFramingClosesWith1002) {
  WsRig rig;
  const int port = rig.start();
  std::vector<int> statuses;
  runOffMessageThread([&] {
    const auto attempt = [&](const std::function<void(WsClient&)>& misbehave) {
      WsClient client;
      if (!client.authenticate(port, rig.token)) {
        statuses.push_back(-2);
        return;
      }
      misbehave(client);
      statuses.push_back(client.readFrame().status());
    };
    // Reserved bit set.
    attempt([](WsClient& c) {
      std::string frame = WsClient::maskedFrame(0x1, "hi");
      frame[0] = static_cast<char>(frame[0] | 0x40);
      c.sendRaw(frame);
    });
    // A continuation with nothing to continue.
    attempt([](WsClient& c) { c.sendFrame(0x0, "x"); });
    // A new message while another is half sent.
    attempt([](WsClient& c) {
      c.sendFrame(0x1, "a", false);
      c.sendFrame(0x1, "b");
    });
    // A fragmented control frame.
    attempt([](WsClient& c) { c.sendFrame(0x9, "p", false); });
    // An opcode that does not exist.
    attempt([](WsClient& c) { c.sendFrame(0x3, "x"); });
    // A length with the top bit set.
    attempt([](WsClient& c) { c.sendHeaderOnly(0x1, 0x8000000000000001ull); });
  });
  EXPECT_EQ(statuses, (std::vector<int>{1002, 1002, 1002, 1002, 1002, 1002}));
}

TEST(WebSocketTransportTest, TextThatIsNotUtf8ClosesWith1007) {
  WsRig rig;
  const int port = rig.start();
  Frame close;
  runOffMessageThread([&] {
    WsClient client;
    ASSERT_TRUE(client.authenticate(port, rig.token));
    client.sendFrame(0x1, "{\"id\":1,\"x\":\"\xC3\x28\"}");
    close = client.readFrame();
  });
  EXPECT_EQ(close.status(), 1007);
}

TEST(WebSocketTransportTest, ACloseFrameIsEchoedAndTheConnectionEnds) {
  WsRig rig;
  const int port = rig.start();
  Frame close;
  bool closed = false;
  runOffMessageThread([&] {
    WsClient client;
    ASSERT_TRUE(client.authenticate(port, rig.token));
    client.sendFrame(0x8, std::string("\x03\xE8", 2));
    close = client.readFrame();
    closed = client.waitClosed();
  });
  EXPECT_EQ(close.status(), 1000);
  EXPECT_TRUE(closed);
}

TEST(WebSocketTransportTest, ALengthOverTheLimitIsRefusedBeforeThePayloadArrives) {
  WsRig rig;
  rig.server->limits.maxRequestBytes = 1024;
  const int port = rig.start();
  juce::var answer;
  bool closed = false;
  runOffMessageThread([&] {
    WsClient client;
    ASSERT_TRUE(client.authenticate(port, rig.token));
    // The header says a megabyte; not a byte of it follows.
    client.sendHeaderOnly(0x1, 1024 * 1024);
    answer = client.readJson();
    closed = client.waitClosed();
  });
  EXPECT_FALSE(static_cast<bool>(answer["ok"]));
  EXPECT_EQ(answer["error"]["code"].toString(), "too_large");
  EXPECT_TRUE(closed);
}

TEST(WebSocketTransportTest, FragmentsCountTowardsTheLimitTogether) {
  WsRig rig;
  rig.server->limits.maxRequestBytes = 1024;
  const int port = rig.start();
  juce::var answer;
  runOffMessageThread([&] {
    WsClient client;
    ASSERT_TRUE(client.authenticate(port, rig.token));
    client.sendFrame(0x1, std::string(700, 'a'), false);
    client.sendFrame(0x0, std::string(700, 'a'), true);
    answer = client.readJson();
  });
  EXPECT_EQ(answer["error"]["code"].toString(), "too_large");
}

TEST(WebSocketTransportTest, StopReturnsPromptlyWithAClientHalfWayThroughTheUpgrade) {
  WsRig rig;
  const int port = rig.start();
  WsClient client;
  ASSERT_TRUE(client.connect(port));
  ASSERT_TRUE(client.sendRaw("GET / HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nUpgr"));
  juce::Thread::sleep(200);
  const auto start = std::chrono::steady_clock::now();
  rig.server->stop();
  const double elapsed =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  EXPECT_LT(elapsed, 2000.0);
  EXPECT_FALSE(rig.server->isRunning());
}

TEST(WebSocketTransportTest, ASlowUpgradeDoesNotKeepOthersWaiting) {
  WsRig rig;
  const int port = rig.start();
  WsClient slow;
  ASSERT_TRUE(slow.connect(port));
  ASSERT_TRUE(slow.sendRaw("GET / HTTP/1.1\r\n"));
  bool authenticated = false;
  runOffMessageThread([&] {
    WsClient client;
    authenticated = client.authenticate(port, rig.token);
  });
  EXPECT_TRUE(authenticated);
}

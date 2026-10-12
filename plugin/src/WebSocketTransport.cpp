#include "CommandServer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <utility>

namespace {

// ####################
// SHA-1, FOR THE HANDSHAKE
// ####################
// Only the Sec-WebSocket-Accept value needs it, which proves the server read
// the upgrade request; it protects nothing, so a small plain version will do.
std::array<unsigned char, 20> sha1(const std::string& data) {
  uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};

  std::string padded = data;
  padded.push_back(static_cast<char>(0x80));
  while (padded.size() % 64 != 56)
    padded.push_back('\0');
  const uint64_t bits = static_cast<uint64_t>(data.size()) * 8u;
  for (int shift = 56; shift >= 0; shift -= 8)
    padded.push_back(static_cast<char>((bits >> shift) & 0xffu));

  const auto rotl = [](uint32_t value, int count) {
    return (value << count) | (value >> (32 - count));
  };
  for (size_t offset = 0; offset < padded.size(); offset += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      const unsigned char* p = reinterpret_cast<const unsigned char*>(padded.data()) + offset + i * 4;
      w[i] = (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
             (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
    }
    for (int i = 16; i < 80; ++i)
      w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999u;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1u;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDCu;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6u;
      }
      const uint32_t next = rotl(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rotl(b, 30);
      b = a;
      a = next;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }

  std::array<unsigned char, 20> digest{};
  for (int i = 0; i < 5; ++i)
    for (int j = 0; j < 4; ++j)
      digest[static_cast<size_t>(i * 4 + j)] = static_cast<unsigned char>((h[i] >> (24 - j * 8)) & 0xffu);
  return digest;
}

std::string acceptValue(const std::string& key) {
  const auto digest = sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
  return juce::Base64::toBase64(digest.data(), digest.size()).toStdString();
}

// ####################
// SMALL TEXT HELPERS
// ####################
std::string lower(std::string text) {
  for (char& c : text)
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  return text;
}

std::string trimmed(const std::string& text) {
  size_t begin = 0, end = text.size();
  while (begin < end && (text[begin] == ' ' || text[begin] == '\t'))
    ++begin;
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t'))
    --end;
  return text.substr(begin, end - begin);
}

// True when the comma separated `list` has `token` (compared in lower case).
bool listHasToken(const std::string& list, const std::string& token) {
  size_t start = 0;
  while (start <= list.size()) {
    size_t comma = list.find(',', start);
    if (comma == std::string::npos)
      comma = list.size();
    if (lower(trimmed(list.substr(start, comma - start))) == token)
      return true;
    start = comma + 1;
  }
  return false;
}

// Well formed UTF-8 as RFC 3629 defines it: no overlong forms, no surrogates,
// nothing above U+10FFFF.
bool validUtf8(const std::string& text) {
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text.data());
  const size_t n = text.size();
  size_t i = 0;
  while (i < n) {
    const unsigned char c = p[i];
    if (c < 0x80) {
      ++i;
      continue;
    }
    size_t extra;
    unsigned char lowest = 0x80, highest = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) {
      extra = 1;
    } else if (c >= 0xE0 && c <= 0xEF) {
      extra = 2;
      if (c == 0xE0)
        lowest = 0xA0;
      if (c == 0xED)
        highest = 0x9F;
    } else if (c >= 0xF0 && c <= 0xF4) {
      extra = 3;
      if (c == 0xF0)
        lowest = 0x90;
      if (c == 0xF4)
        highest = 0x8F;
    } else {
      return false;
    }
    if (i + extra >= n)
      return false;
    if (p[i + 1] < lowest || p[i + 1] > highest)
      return false;
    for (size_t k = 2; k <= extra; ++k)
      if ((p[i + k] & 0xC0) != 0x80)
        return false;
    i += extra + 1;
  }
  return true;
}

// ####################
// WEBSOCKET (RFC 6455), ONE TEXT MESSAGE PER COMMAND LINE
// ####################
constexpr size_t kMaxHeaderBytes = 8 * 1024;

class WebSocketConnection final : public CommandTransport::Connection {
public:
  WebSocketConnection(std::unique_ptr<juce::StreamingSocket> s, int port,
                      std::vector<juce::String> origins)
      : socket(std::move(s)), boundPort(port), allowedOrigins(std::move(origins)) {}

  Received receive(std::string& message, int timeoutMs, int maxBytes) override {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, timeoutMs));

    // The upgrade is answered here, on this connection's own thread and
    // within the caller's wait, so a client that is slow to speak costs
    // nobody else anything.
    while (!handshaken.load()) {
      if (handshakeFailed)
        return Received::closed;
      if (const auto outcome = tryHandshake())
        return *outcome;
      switch (readMore(deadline)) {
        case Fill::data: break;
        case Fill::timeout: return Received::nothing;
        case Fill::closed: return Received::closed;
      }
    }

    for (;;) {
      if (const auto outcome = tryFrame(message, maxBytes))
        return *outcome;
      switch (readMore(deadline)) {
        case Fill::data: break;
        case Fill::timeout: return Received::nothing;
        case Fill::closed: return Received::closed;
      }
    }
  }

  bool send(const std::string& message) override {
    if (!handshaken.load())
      return false;
    return writeFrame(0x1, message);
  }

  void close() override {
    closing.store(true);
    socket->close();
  }

private:
  enum class Fill { data, timeout, closed };

  // One read into the buffer, waited for in short slices: closing a socket
  // from another thread does not wake a wait on every platform, the flag does.
  Fill readMore(std::chrono::steady_clock::time_point deadline) {
    for (;;) {
      if (closing.load())
        return Fill::closed;
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 deadline - std::chrono::steady_clock::now())
                                 .count();
      if (remaining <= 0)
        return Fill::timeout;
      const int ready =
          socket->waitUntilReady(true, static_cast<int>(std::min<long long>(remaining, 50)));
      if (ready < 0)
        return Fill::closed;
      if (ready == 0)
        continue;
      char chunk[16384];
      const int count = socket->read(chunk, static_cast<int>(sizeof(chunk)), false);
      // Readable with nothing to read is how a closed connection looks.
      if (count <= 0)
        return Fill::closed;
      buffer.append(chunk, static_cast<size_t>(count));
      return Fill::data;
    }
  }

  // Writes everything, looping over partial writes. Holds the write mutex.
  bool writeAll(const std::string& bytes) {
    const std::lock_guard<std::mutex> lock(writeMutex);
    size_t sent = 0;
    while (sent < bytes.size()) {
      const int count = socket->write(bytes.data() + sent, static_cast<int>(bytes.size() - sent));
      if (count <= 0)
        return false;
      sent += static_cast<size_t>(count);
    }
    return true;
  }

  // One attempt, result ignored: for the last words to a client that is being
  // dropped, which must not hold up a thread that is about to end.
  void writeOnce(const std::string& bytes) {
    const std::lock_guard<std::mutex> lock(writeMutex);
    socket->write(bytes.data(), static_cast<int>(bytes.size()));
  }

  static std::string frameBytes(int opcode, const std::string& payload) {
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | opcode));
    const uint64_t size = payload.size();
    if (size <= 125) {
      frame.push_back(static_cast<char>(size));
    } else if (size <= 0xffff) {
      frame.push_back(static_cast<char>(126));
      frame.push_back(static_cast<char>((size >> 8) & 0xff));
      frame.push_back(static_cast<char>(size & 0xff));
    } else {
      frame.push_back(static_cast<char>(127));
      for (int shift = 56; shift >= 0; shift -= 8)
        frame.push_back(static_cast<char>((size >> shift) & 0xff));
    }
    frame += payload;
    return frame;
  }

  bool writeFrame(int opcode, const std::string& payload) {
    return writeAll(frameBytes(opcode, payload));
  }

  static std::string closePayload(int status) {
    std::string payload;
    payload.push_back(static_cast<char>((status >> 8) & 0xff));
    payload.push_back(static_cast<char>(status & 0xff));
    return payload;
  }

  // The transport itself ends the connection: say why, once, and return the
  // result for receive.
  Received failWith(int status) {
    writeOnce(frameBytes(0x8, closePayload(status)));
    return Received::closed;
  }

  // ----- handshake -----

  Received refuse(const char* statusLine, bool askForVersion = false) {
    handshakeFailed = true;
    std::string response = std::string("HTTP/1.1 ") + statusLine + "\r\n";
    if (askForVersion)
      response += "Sec-WebSocket-Version: 13\r\n";
    response += "Connection: close\r\nContent-Length: 0\r\n\r\n";
    writeOnce(response);
    return Received::closed;
  }

  bool originAllowed(const std::string& origin) const {
    for (const auto& allowed : allowedOrigins)
      if (lower(allowed.toStdString()) == lower(origin))
        return true;
    return false;
  }

  bool hostAllowed(const std::string& host) const {
    const std::string wanted = ":" + std::to_string(boundPort);
    const std::string value = lower(host);
    for (const char* name : {"127.0.0.1", "localhost"}) {
      if (value == std::string(name) + wanted)
        return true;
      if (boundPort == 80 && value == name)
        return true;
    }
    return false;
  }

  // Nullopt while the request is still incomplete.
  std::optional<Received> tryHandshake() {
    const size_t end = buffer.find("\r\n\r\n");
    if (end == std::string::npos) {
      if (buffer.size() > kMaxHeaderBytes)
        return refuse("431 Request Header Fields Too Large");
      return std::nullopt;
    }
    if (end + 4 > kMaxHeaderBytes)
      return refuse("431 Request Header Fields Too Large");

    const std::string head = buffer.substr(0, end);
    buffer.erase(0, end + 4);

    // Request line, then header lines.
    size_t lineEnd = head.find("\r\n");
    const std::string requestLine = head.substr(0, lineEnd);
    const size_t firstSpace = requestLine.find(' ');
    const size_t lastSpace = requestLine.rfind(' ');
    if (firstSpace == std::string::npos || lastSpace == firstSpace)
      return refuse("400 Bad Request");
    if (requestLine.substr(0, firstSpace) != "GET" || requestLine.substr(lastSpace + 1) != "HTTP/1.1")
      return refuse("400 Bad Request");

    std::map<std::string, std::string> headers;
    size_t pos = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
    while (pos < head.size()) {
      size_t next = head.find("\r\n", pos);
      if (next == std::string::npos)
        next = head.size();
      const std::string line = head.substr(pos, next - pos);
      pos = next + 2;
      const size_t colon = line.find(':');
      // No folded lines, no header without a name.
      if (colon == std::string::npos || colon == 0 || line[0] == ' ' || line[0] == '\t')
        return refuse("400 Bad Request");
      const std::string name = lower(line.substr(0, colon));
      if (name.find_first_of(" \t") != std::string::npos)
        return refuse("400 Bad Request");
      const std::string value = trimmed(line.substr(colon + 1));
      const auto existing = headers.find(name);
      if (existing == headers.end()) {
        headers[name] = value;
      } else if (name == "host" || name == "origin" || name == "sec-websocket-key" ||
                 name == "sec-websocket-version" || name == "upgrade") {
        // A request that says these twice may mean two things to two readers.
        return refuse("400 Bad Request");
      } else {
        existing->second += ", " + value;
      }
    }
    const auto header = [&](const char* name) -> const std::string* {
      const auto found = headers.find(name);
      return found == headers.end() ? nullptr : &found->second;
    };

    // Against DNS rebinding and other web pages, before anything else.
    const std::string* host = header("host");
    if (host == nullptr || !hostAllowed(*host))
      return refuse("403 Forbidden");
    if (const std::string* origin = header("origin"))
      if (!originAllowed(*origin))
        return refuse("403 Forbidden");

    const std::string* upgrade = header("upgrade");
    const std::string* connection = header("connection");
    if (upgrade == nullptr || lower(*upgrade) != "websocket" || connection == nullptr ||
        !listHasToken(*connection, "upgrade"))
      return refuse("400 Bad Request");

    const std::string* version = header("sec-websocket-version");
    if (version == nullptr)
      return refuse("400 Bad Request");
    if (*version != "13")
      return refuse("426 Upgrade Required", true);

    const std::string* key = header("sec-websocket-key");
    if (key == nullptr || key->size() != 24)
      return refuse("400 Bad Request");
    juce::MemoryOutputStream decoded;
    if (!juce::Base64::convertFromBase64(decoded, juce::String(*key)) || decoded.getDataSize() != 16)
      return refuse("400 Bad Request");

    const std::string response =
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " +
        acceptValue(*key) + "\r\n\r\n";
    if (!writeAll(response)) {
      handshakeFailed = true;
      return Received::closed;
    }
    handshaken.store(true);
    // What follows the request in the buffer is the first frame.
    return std::nullopt;
  }

  // ----- frames -----

  // Nullopt when more bytes are needed to go on.
  std::optional<Received> tryFrame(std::string& message, int maxBytes) {
    for (;;) {
      if (buffer.size() < 2)
        return std::nullopt;
      const auto* b = reinterpret_cast<const unsigned char*>(buffer.data());
      const bool fin = (b[0] & 0x80) != 0;
      const int opcode = b[0] & 0x0f;
      const bool masked = (b[1] & 0x80) != 0;
      const int lengthCode = b[1] & 0x7f;

      if ((b[0] & 0x70) != 0)
        return failWith(1002);
      const bool control = (opcode & 0x8) != 0;
      if (opcode != 0x0 && opcode != 0x1 && opcode != 0x2 && opcode != 0x8 && opcode != 0x9 &&
          opcode != 0xA)
        return failWith(1002);
      if (!masked)
        return failWith(1002);
      if (control && (!fin || lengthCode > 125))
        return failWith(1002);
      if (opcode == 0x2)
        return failWith(1003);
      if (opcode == 0x1 && inMessage)
        return failWith(1002);
      if (opcode == 0x0 && !inMessage)
        return failWith(1002);

      size_t headerSize = 2;
      uint64_t length = static_cast<uint64_t>(lengthCode);
      if (lengthCode == 126) {
        headerSize += 2;
        if (buffer.size() < headerSize)
          return std::nullopt;
        length = (static_cast<uint64_t>(b[2]) << 8) | b[3];
      } else if (lengthCode == 127) {
        headerSize += 8;
        if (buffer.size() < headerSize)
          return std::nullopt;
        length = 0;
        for (int i = 0; i < 8; ++i)
          length = (length << 8) | b[2 + i];
        if ((length >> 63) != 0)
          return failWith(1002);
      }
      headerSize += 4;
      if (buffer.size() < headerSize)
        return std::nullopt;

      // Too long is known from the header: the payload is never read.
      if (!control && (partial.size() + length > static_cast<uint64_t>(std::max(0, maxBytes))))
        return Received::tooLong;

      if (buffer.size() - headerSize < length)
        return std::nullopt;
      const unsigned char* mask = b + headerSize - 4;
      std::string payload(buffer, headerSize, static_cast<size_t>(length));
      for (size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask[i % 4]);
      buffer.erase(0, headerSize + static_cast<size_t>(length));

      if (opcode == 0x9) {
        if (!writeFrame(0xA, payload))
          return Received::closed;
        continue;
      }
      if (opcode == 0xA)
        continue;
      if (opcode == 0x8) {
        if (payload.size() == 1)
          return failWith(1002);
        writeOnce(frameBytes(0x8, payload.substr(0, 2)));
        return Received::closed;
      }

      partial += payload;
      inMessage = !fin;
      if (!fin)
        continue;
      if (!validUtf8(partial))
        return failWith(1007);
      message = std::move(partial);
      partial.clear();
      return Received::message;
    }
  }

  std::atomic<bool> closing{false};
  std::atomic<bool> handshaken{false};
  // Only the receiving thread reads or writes these.
  bool handshakeFailed = false;
  bool inMessage = false;
  std::string buffer;
  std::string partial;

  std::mutex writeMutex;
  std::unique_ptr<juce::StreamingSocket> socket;
  const int boundPort;
  const std::vector<juce::String> allowedOrigins;
};

class WebSocketTransport final : public CommandTransport {
public:
  explicit WebSocketTransport(std::vector<juce::String> origins) : allowedOrigins(std::move(origins)) {}

  bool listen(int port) override {
    // The address is part of the security model: bound to the loopback
    // interface, the socket cannot be reached from another machine.
    if (!listener.createListener(port, "127.0.0.1"))
      return false;
    bound = listener.getBoundPort();
    return bound > 0;
  }

  int boundPort() const override { return bound; }

  // Instant on purpose: the upgrade is read on the connection's own thread.
  std::unique_ptr<Connection> accept() override {
    std::unique_ptr<juce::StreamingSocket> socket(listener.waitForNextConnection());
    if (socket == nullptr)
      return nullptr;
    // Cannot happen with the listener bound as above; checked all the same,
    // since everything else rests on it.
    if (socket->getHostName() != "127.0.0.1") {
      socket->close();
      return nullptr;
    }
    return std::make_unique<WebSocketConnection>(std::move(socket), bound, allowedOrigins);
  }

  void stopListening() override {
    // See TcpLineTransport::stopListening: the wake-up call goes to the real
    // port, since JUCE's close would call port 0.
    if (bound > 0) {
      juce::StreamingSocket wake;
      wake.connect("127.0.0.1", bound, 500);
    }
    listener.close();
  }

  juce::String name() const override { return "websocket"; }

private:
  juce::StreamingSocket listener;
  int bound = 0;
  std::vector<juce::String> allowedOrigins;
};

}  // namespace

std::unique_ptr<CommandTransport> makeWebSocketTransport(std::vector<juce::String> allowedOrigins) {
  return std::make_unique<WebSocketTransport>(std::move(allowedOrigins));
}

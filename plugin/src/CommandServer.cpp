#include "CommandServer.h"

#include "AgentHub.h"
#include "AppIdentity.h"
#include "LibraryPaths.h"
#include "PresetJson.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <exception>
#include <new>
#include <random>
#include <utility>

#if JUCE_WINDOWS
#include <process.h>
#include <winsock2.h>
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

std::mutex& overrideMutex() {
  static std::mutex mutex;
  return mutex;
}

juce::File& connectionFileOverride() {
  static juce::File file;
  return file;
}

// How deep the arrays and objects of one line may nest. No command takes
// arguments nested more than a few levels; JUCE's parser recurses once per
// level without a limit of its own, and a connection's thread has a small
// stack, so a line of nothing but brackets would otherwise end the process.
constexpr int kMaxRequestDepth = 32;

// One line as JSON, behind the depth check (presetjson::parseJsonGuarded
// scans the brackets before the parser sees any of them).
bool parseLine(const std::string& line, juce::var& out) {
  return t3k::presetjson::parseJsonGuarded(
             juce::String::fromUTF8(line.data(), static_cast<int>(line.size())), out,
             kMaxRequestDepth)
      .wasOk();
}

int processId() {
#if JUCE_WINDOWS
  return static_cast<int>(_getpid());
#else
  return static_cast<int>(::getpid());
#endif
}

// 256 bits from the system's random source, as hex. std::random_device is
// the operating system's generator on every platform this builds for.
std::string makeToken() {
  std::random_device source;
  static const char digits[] = "0123456789abcdef";
  std::string token;
  for (int i = 0; i < 8; ++i) {
    const unsigned int word = source();
    for (int nibble = 0; nibble < 8; ++nibble)
      token.push_back(digits[(word >> (nibble * 4)) & 0xfu]);
  }
  return token;
}

// Compares without stopping at the first difference, so the time taken says
// nothing about how much of a guess was right.
bool sameToken(const std::string& expected, const juce::String& given) {
  const std::string candidate = given.toStdString();
  unsigned char difference = expected.size() == candidate.size() ? 0 : 1;
  for (size_t i = 0; i < expected.size(); ++i)
    difference |= static_cast<unsigned char>(
        expected[i] ^ (i < candidate.size() ? candidate[i] : '\0'));
  return difference == 0;
}

// One line of JSON. Numbers go out with at most nine decimals: JUCE's
// default of fifteen prints a double's binary dust (-17.13 as
// -17.129999999999999), which is noise to read and tokens to pay for.
juce::String wireJson(const juce::var& value) { return juce::JSON::toString(value, true, 9); }

std::string errorLine(const juce::var& id, const char* code, const juce::String& message) {
  juce::DynamicObject::Ptr error = new juce::DynamicObject();
  error->setProperty("code", code);
  error->setProperty("message", message);
  juce::DynamicObject::Ptr reply = new juce::DynamicObject();
  if (!id.isVoid() && !id.isUndefined())
    reply->setProperty("id", id);
  reply->setProperty("ok", false);
  reply->setProperty("error", juce::var(error.get()));
  return wireJson(juce::var(reply.get())).toStdString();
}

// ####################
// TCP, ONE JSON OBJECT PER LINE
// ####################
}  // namespace

void shutDownSocket(juce::StreamingSocket& socket) {
  const int handle = socket.getRawSocketHandle();
  if (handle < 0)
    return;
#if JUCE_WINDOWS
  ::shutdown(static_cast<SOCKET>(handle), SD_BOTH);
#else
  ::shutdown(handle, SHUT_RDWR);
#endif
}

namespace {

class TcpLineConnection final : public CommandTransport::Connection {
public:
  explicit TcpLineConnection(std::unique_ptr<juce::StreamingSocket> s) : socket(std::move(s)) {}

  Received receive(std::string& message, int timeoutMs, int maxBytes) override {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, timeoutMs));
    for (;;) {
      const size_t lineEnd = buffer.find('\n', searchFrom);
      if (lineEnd != std::string::npos) {
        if (lineEnd > static_cast<size_t>(maxBytes))
          return Received::tooLong;
        message.assign(buffer, 0, lineEnd);
        if (!message.empty() && message.back() == '\r')
          message.pop_back();
        buffer.erase(0, lineEnd + 1);
        searchFrom = 0;
        return Received::message;
      }
      // Nothing before this point holds a line end; do not look again.
      searchFrom = buffer.size();
      if (buffer.size() > static_cast<size_t>(maxBytes))
        return Received::tooLong;

      if (closing.load())
        return Received::closed;
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 deadline - std::chrono::steady_clock::now())
                                 .count();
      if (remaining <= 0)
        return Received::nothing;
      // Waited in short slices: closing a socket from another thread does
      // not wake a wait on every platform, the flag above does.
      const int ready =
          socket->waitUntilReady(true, static_cast<int>(std::min<long long>(remaining, 50)));
      if (ready < 0)
        return Received::closed;
      if (ready == 0)
        continue;
      char chunk[16384];
      const int count = socket->read(chunk, static_cast<int>(sizeof(chunk)), false);
      // Readable with nothing to read is how a closed connection looks.
      if (count <= 0)
        return Received::closed;
      buffer.append(chunk, static_cast<size_t>(count));
    }
  }

  bool send(const std::string& message) override {
    std::string line = message;
    line.push_back('\n');
    size_t sent = 0;
    while (sent < line.size()) {
      const int count =
          socket->write(line.data() + sent, static_cast<int>(line.size() - sent));
      if (count <= 0)
        return false;
      sent += static_cast<size_t>(count);
    }
    return true;
  }

  // Called from any thread (see shutDownSocket); the socket itself is
  // closed as the connection goes.
  void close() override {
    closing.store(true);
    shutDownSocket(*socket);
  }

private:
  std::atomic<bool> closing{false};
  std::unique_ptr<juce::StreamingSocket> socket;
  std::string buffer;
  size_t searchFrom = 0;
};

class TcpLineTransport final : public CommandTransport {
public:
  bool listen(int port) override {
    // The address is part of the security model: bound to the loopback
    // interface, the socket cannot be reached from another machine.
    if (!listener.createListener(port, "127.0.0.1"))
      return false;
    bound = listener.getBoundPort();
    return bound > 0;
  }

  int boundPort() const override { return bound; }

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
    return std::make_unique<TcpLineConnection>(std::move(socket));
  }

  void stopListening() override {
    // A thread waiting in accept is only woken by a connection. JUCE's close
    // makes one to the port it was asked for, which is 0 when the system
    // picked the port, so the wake-up call is made here, to the real one.
    if (bound > 0) {
      juce::StreamingSocket wake;
      wake.connect("127.0.0.1", bound, 500);
    }
    shutDownSocket(listener);
  }

  void releaseListener() override {
    listener.close();
    bound = 0;
  }

  juce::String name() const override { return "tcp-ndjson"; }

private:
  juce::StreamingSocket listener;
  int bound = 0;
};

}  // namespace

std::unique_ptr<CommandTransport> makeTcpLineTransport() {
  return std::make_unique<TcpLineTransport>();
}

// ####################
// SERVER
// ####################
struct CommandServer::Client {
  std::unique_ptr<CommandTransport::Connection> connection;
  std::thread thread;
  std::atomic<bool> finished{false};
  // Has presented the token. Until then the connection counts against the
  // small limit of its own (Limits::maxUnauthenticated), not against the
  // clients'.
  std::atomic<bool> authenticated{false};
  // Closed to make room for a newer connection while it still had not
  // presented the token.
  std::atomic<bool> evicted{false};
  std::chrono::steady_clock::time_point accepted = std::chrono::steady_clock::now();

  // The session is made on the connection's thread and read by stop() on
  // the message thread.
  std::shared_ptr<AgentSession> currentSession() {
    const std::lock_guard<std::mutex> lock(sessionMutex);
    return session;
  }
  void setSession(std::shared_ptr<AgentSession> opened) {
    const std::lock_guard<std::mutex> lock(sessionMutex);
    session = std::move(opened);
  }

private:
  std::mutex sessionMutex;
  std::shared_ptr<AgentSession> session;
};

CommandServer::CommandServer(AgentHub& agentHub, std::unique_ptr<CommandTransport> carrier)
    : hub(agentHub), transport(std::move(carrier)) {
  if (transport == nullptr)
    transport = makeTcpLineTransport();
}

CommandServer::~CommandServer() { stop(); }

juce::File CommandServer::connectionFile() {
  {
    const std::lock_guard<std::mutex> lock(overrideMutex());
    if (connectionFileOverride() != juce::File())
      return connectionFileOverride();
  }
  return LibraryPaths::appDataDir().getChildFile("command-server.json");
}

juce::File CommandServer::setConnectionFileForTesting(const juce::File& file) {
  const std::lock_guard<std::mutex> lock(overrideMutex());
  return std::exchange(connectionFileOverride(), file);
}

bool CommandServer::writeConnectionFile() {
  const juce::File file = connectionFile();
  const juce::File folder = file.getParentDirectory();
  // Missing folders are made private to the user (0700 on POSIX).
  if (!LibraryPaths::createPrivateDirectory(folder))
    return false;

  juce::DynamicObject::Ptr info = new juce::DynamicObject();
  info->setProperty("protocol", kProtocolVersion);
  info->setProperty("transport", transport->name());
  info->setProperty("host", "127.0.0.1");
  info->setProperty("port", boundPort.load());
  info->setProperty("token", juce::String(token));
  info->setProperty("pid", processId());
  info->setProperty("started", juce::Time::getCurrentTime().toISO8601(true));
  const std::string text = (juce::JSON::toString(juce::var(info.get())) + "\n").toStdString();

#if JUCE_WINDOWS
  // Windows: the file is made in place. It has no mode of its own to set:
  // what keeps other accounts out is the ACL of the user's profile, which
  // %APPDATA% and everything made under it inherit (docs/agents.md,
  // "Connection file"). A connection file redirected to a folder outside
  // the profile is only as private as that folder.
  file.deleteFile();
  if (!file.create().wasOk())
    return false;
  juce::FileOutputStream out(file);
  if (!out.openedOk())
    return false;
  out.setPosition(0);
  out.truncate();
  out.write(text.data(), text.size());
  out.flush();
  if (!out.getStatus().wasOk())
    return false;
#else
  // The application's own data folder is tightened to the user when it was
  // made with a wider mode by an earlier version. Only that folder, and only
  // when it is the user's: a folder the file was redirected to (a test, the
  // headless host's --data, which is also the app data folder it runs with)
  // belongs to whoever made it, and the shared folder above ours is never
  // touched.
  if (folder == LibraryPaths::defaultAppDataDir()) {
    struct stat status {};
    if (::lstat(folder.getFullPathName().toRawUTF8(), &status) == 0 && S_ISDIR(status.st_mode) &&
        status.st_uid == ::geteuid() && (status.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
      // Best effort: the file below is private by its own mode either way.
      const int tightened = ::chmod(folder.getFullPathName().toRawUTF8(), S_IRWXU);
      juce::ignoreUnused(tightened);
    }
  }

  // Written under a name nobody can have guessed, made by this call and by
  // nothing else (O_EXCL), never through a link (O_NOFOLLOW), private from
  // its first byte (the mode is open's own), and only then given the real
  // name: rename replaces whatever is there, a link included, in one step,
  // so a reader sees the old file or the whole new one and the token is
  // never in a file anyone else can read.
  const juce::File temp = folder.getChildFile(file.getFileName() + ".tmp-" +
                                              juce::String(processId()) + "-" +
                                              juce::String(makeToken().substr(0, 16)));
  const std::string tempPath = temp.getFullPathName().toStdString();
  const int fd = ::open(tempPath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                        S_IRUSR | S_IWUSR);
  if (fd < 0)
    return false;
  // (A umask can only have taken bits away; said again for the record.)
  bool written = ::fchmod(fd, S_IRUSR | S_IWUSR) == 0;
  for (size_t sent = 0; written && sent < text.size();) {
    const ssize_t count = ::write(fd, text.data() + sent, text.size() - sent);
    if (count <= 0)
      written = false;
    else
      sent += static_cast<size_t>(count);
  }
  written = written && ::fsync(fd) == 0;
  written = (::close(fd) == 0) && written;
  if (!written || ::rename(tempPath.c_str(), file.getFullPathName().toRawUTF8()) != 0) {
    ::unlink(tempPath.c_str());
    return false;
  }
#endif
  writtenFile = file;
  return true;
}

void CommandServer::removeConnectionFile() {
  const juce::File file = std::exchange(writtenFile, juce::File());
  // Only a file that still names this server: another instance started
  // since (or a restart of this one) has put its own there, and taking that
  // away would cut its clients off.
  if (file == juce::File() || !file.existsAsFile() || file.getSize() > 64 * 1024)
    return;
  juce::var info;
  if (t3k::presetjson::parseJsonGuarded(file.loadFileAsString(), info, kMaxRequestDepth).failed() ||
      info.getDynamicObject() == nullptr)
    return;
  if (static_cast<int>(info["pid"]) == processId() && sameToken(token, info["token"].toString()))
    file.deleteFile();
}

juce::String CommandServer::start(int port) {
  if (running.load())
    return {};
#if !JUCE_WINDOWS
  // Writing to a client that has gone raises SIGPIPE, which would end the
  // process; the write's error return is all that is wanted. (JUCE's sockets
  // do not mask it themselves.)
  std::signal(SIGPIPE, SIG_IGN);
#endif

  token = makeToken();
  stopping.store(false);
  if (!transport->listen(juce::jlimit(0, 65535, port)))
    return "could not listen on 127.0.0.1:" + juce::String(port);
  boundPort.store(transport->boundPort());
  if (!writeConnectionFile()) {
    transport->stopListening();
    transport->releaseListener();
    boundPort.store(0);
    return "could not write the connection file " + connectionFile().getFullPathName();
  }
  running.store(true);
  acceptThread = std::thread([this] { acceptLoop(); });
  return {};
}

void CommandServer::stop() {
  if (!running.exchange(false))
    return;
  stopping.store(true);
  transport->stopListening();
  if (acceptThread.joinable())
    acceptThread.join();
  transport->releaseListener();
  reapFinishedClients(/*all=*/true);
  boundPort.store(0);
  removeConnectionFile();
}

int CommandServer::connectionCount() const {
  const std::lock_guard<std::mutex> lock(clientsMutex);
  int count = 0;
  for (const auto& client : clients)
    if (!client->finished.load())
      ++count;
  return count;
}

void CommandServer::reapFinishedClients(bool all) {
  std::vector<std::shared_ptr<Client>> gone;
  {
    const std::lock_guard<std::mutex> lock(clientsMutex);
    for (auto it = clients.begin(); it != clients.end();) {
      if (all || (*it)->finished.load()) {
        gone.push_back(*it);
        it = clients.erase(it);
      } else {
        ++it;
      }
    }
  }
  // Every one is told to stop before any is waited for. Closing the socket
  // ends a read or a write the thread is waiting in; the session's flag ends
  // slow work it is in the middle of (waiting for loads, a render) and a wait
  // for this very thread, which is busy here and would not answer.
  for (auto& client : gone) {
    if (const auto session = client->currentSession())
      session->cancelled.store(true);
    client->connection->close();
  }
  for (auto& client : gone)
    if (client->thread.joinable())
      client->thread.join();
}

void CommandServer::acceptLoop() {
  while (!stopping.load()) {
    std::unique_ptr<CommandTransport::Connection> connection = transport->accept();
    if (stopping.load())
      break;
    if (connection == nullptr) {
      juce::Thread::sleep(10);
      continue;
    }
    reapFinishedClients(/*all=*/false);

    auto client = std::make_shared<Client>();
    client->connection = std::move(connection);

    // Two limits, kept apart so that sockets which never say anything cannot
    // use up the clients' places: at most maxConnections that have presented
    // the token, and beside them at most maxUnauthenticated that have not
    // yet. When those few are taken, the one that has waited longest makes
    // room, but not before it has had unauthenticatedGraceMs to speak: a
    // real client sends its token at once, so it is in long before that, and
    // a burst of real clients only waits here for a few milliseconds.
    bool admitted = false, full = false;
    while (!stopping.load()) {
      {
        const std::lock_guard<std::mutex> lock(clientsMutex);
        int authenticated = 0, waiting = 0;
        Client* oldest = nullptr;
        for (const auto& other : clients) {
          if (other->finished.load() || other->evicted.load())
            continue;
          if (other->authenticated.load()) {
            ++authenticated;
          } else {
            ++waiting;
            if (oldest == nullptr)
              oldest = other.get();  // the list is in order of arrival
          }
        }
        if (authenticated >= limits.maxConnections) {
          full = true;
          break;
        }
        if (waiting < std::max(1, limits.maxUnauthenticated)) {
          client->accepted = std::chrono::steady_clock::now();
          clients.push_back(client);
          admitted = true;
          break;
        }
        if (std::chrono::steady_clock::now() - oldest->accepted >=
            std::chrono::milliseconds(limits.unauthenticatedGraceMs)) {
          oldest->evicted.store(true);
          oldest->connection->close();
          continue;
        }
      }
      juce::Thread::sleep(5);
    }
    if (!admitted) {
      if (full)
        client->connection->send(
            errorLine({}, "unavailable", "too many connections to the command server"));
      client->connection->close();
      continue;
    }
    client->thread = std::thread([this, client] {
      serveGuarded(client);
      client->finished.store(true);
    });
  }
}

bool CommandServer::authenticate(Client& client) {
  // The first line is the token and nothing else is looked at before it: a
  // short limit, a short wait, and any failure ends the connection.
  std::string line;
  const auto received = client.connection->receive(line, limits.authTimeoutMs, 4096);
  if (received != CommandTransport::Connection::Received::message)
    return false;

  juce::var hello;
  const bool parsed = parseLine(line, hello);
  if (!parsed || hello.getDynamicObject() == nullptr || !hello["auth"].isString() ||
      !sameToken(token, hello["auth"].toString())) {
    // A pause before the answer: guessing a 256-bit token is hopeless, and
    // this makes even trying slow.
    juce::Thread::sleep(200);
    client.connection->send(errorLine({}, "unauthorized",
                                      "the first line must be {\"auth\":\"<token>\",\"client\":"
                                      "\"<name>\"} with the token from the connection file"));
    return false;
  }

  // From here it is a client, if there is a place left among them (the
  // check when it was accepted may be out of date by now).
  bool full = false;
  {
    const std::lock_guard<std::mutex> lock(clientsMutex);
    int authenticated = 0;
    for (const auto& other : clients)
      if (!other->finished.load() && !other->evicted.load() && other->authenticated.load())
        ++authenticated;
    full = authenticated >= limits.maxConnections || client.evicted.load();
    if (!full)
      client.authenticated.store(true);
  }
  if (full) {
    client.connection->send(
        errorLine({}, "unavailable", "too many connections to the command server"));
    return false;
  }

  const std::shared_ptr<AgentSession> session = hub.openSession(hello["client"].toString());
  client.setSession(session);
  juce::DynamicObject::Ptr result = new juce::DynamicObject();
  result->setProperty("protocol", kProtocolVersion);
  result->setProperty("app", kAppDataFolderName);
  result->setProperty("sessionId", session->id);
  result->setProperty("client", session->name);
  juce::DynamicObject::Ptr reply = new juce::DynamicObject();
  reply->setProperty("ok", true);
  reply->setProperty("result", juce::var(result.get()));
  return client.connection->send(wireJson(juce::var(reply.get())).toStdString());
}

void CommandServer::serveGuarded(const std::shared_ptr<Client>& client) {
  // Nothing thrown while serving a connection may leave its thread: an
  // exception that escapes a std::thread ends the whole process, and with it
  // the audio. Running out of memory on a large request, or anything else
  // that throws below, costs that one client its connection.
  const char* problem = nullptr;
  try {
    serve(client);
    return;
  } catch (const std::bad_alloc&) {
    problem = "the application ran out of memory serving this connection";
  } catch (const std::exception&) {
    problem = "an internal error ended this connection";
  } catch (...) {
    problem = "an internal error ended this connection";
  }

  // Each step by itself: what failed above may fail again.
  try {
    juce::Logger::writeToLog(juce::String("[Agents] ") + problem);
  } catch (...) {
  }
  try {
    client->connection->send(errorLine({}, "failed", juce::String(problem) + "; it is closed"));
  } catch (...) {
  }
  try {
    hub.closeSession(client->currentSession());
  } catch (...) {
  }
  try {
    client->connection->close();
  } catch (...) {
  }
}

void CommandServer::serve(const std::shared_ptr<Client>& client) {
  using Received = CommandTransport::Connection::Received;
  CommandTransport::Connection& connection = *client->connection;

  if (!authenticate(*client)) {
    connection.close();
    return;
  }
  const std::shared_ptr<AgentSession> session = client->currentSession();

  int badRequests = 0;
  int requestsThisSecond = 0;
  auto windowStart = std::chrono::steady_clock::now();
  bool watching = false;
  AgentHub::RigRevision lastSeen;

  while (!stopping.load() && !session->disconnectRequested.load()) {
    std::string line;
    const Received received = connection.receive(line, limits.eventPollMs, limits.maxRequestBytes);

    // Change events: one atomic read per turn, sent only when the client
    // asked and the number moved (or the client switched rigs).
    if (session->wantsRevisionEvents.load()) {
      const AgentHub::RigRevision now = hub.revisionFor(*session);
      if (!watching) {
        // The subscribe answer carried the current revision; start from it.
        watching = true;
        lastSeen = now;
      } else if (now.rig != lastSeen.rig || now.revision != lastSeen.revision) {
        lastSeen = now;
        juce::DynamicObject::Ptr event = new juce::DynamicObject();
        event->setProperty("event", "revision");
        event->setProperty("rig", now.rig);
        event->setProperty("revision", static_cast<int>(now.revision));
        if (!connection.send(wireJson(juce::var(event.get())).toStdString()))
          break;
      }
    } else {
      watching = false;
    }

    if (received == Received::nothing)
      continue;
    if (received == Received::closed)
      break;
    if (received == Received::tooLong) {
      connection.send(errorLine({}, "too_large",
                                "a request line may be at most " +
                                    juce::String(limits.maxRequestBytes) + " bytes"));
      break;
    }

    // A client that sends faster than any real use would is cut off rather
    // than queued: each request is a turn of the message thread.
    const auto now = std::chrono::steady_clock::now();
    if (now - windowStart >= std::chrono::seconds(1)) {
      windowStart = now;
      requestsThisSecond = 0;
    }
    if (++requestsThisSecond > limits.maxRequestsPerSecond) {
      connection.send(errorLine({}, "rate_limited", "too many requests; the connection is closed"));
      break;
    }

    if (line.empty())
      continue;
    juce::var request;
    const bool parsed = parseLine(line, request);
    if (!parsed || request.getDynamicObject() == nullptr || !request["command"].isString()) {
      const juce::var id = request.getDynamicObject() != nullptr ? request["id"] : juce::var();
      if (!connection.send(errorLine(id, "invalid_request",
                                     "a request is one line of JSON like {\"id\":1,\"command\":"
                                     "\"state.get\",\"args\":{}}, nested at most " +
                                         juce::String(kMaxRequestDepth) + " levels deep")) ||
          ++badRequests >= limits.maxBadRequests)
        break;
      continue;
    }
    badRequests = 0;

    const CommandResult result = hub.runFromAnyThread(
        session, request["command"].toString(), request["args"], limits.commandTimeoutMs);
    juce::var envelope = result.toVar();
    // An id is echoed as given (a number or a string); other shapes are not
    // worth carrying.
    const juce::var id = request["id"];
    if (id.isInt() || id.isInt64() || id.isDouble() || id.isString())
      envelope.getDynamicObject()->setProperty("id", id);
    if (!connection.send(wireJson(envelope).toStdString()))
      break;
  }

  hub.closeSession(session);
  connection.close();
}

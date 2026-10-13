#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class AgentHub;
struct AgentSession;

// The command layer on a local socket, for callers in other processes: the
// MCP server today, a browser remote later.
//
// Wire format (docs/agents.md has the full write-up): one JSON object per
// line, UTF-8, in both directions.
//
//   first line, client:  {"auth":"<token>","client":"<name>"}
//   answer:              {"ok":true,"result":{"protocol":1,"sessionId":N,...}}
//   then, per request:   {"id":<any>,"command":"chain.addBlock","args":{...}}
//   answer:              {"id":<same>,"ok":true,"result":...}
//                        {"id":<same>,"ok":false,"error":{"code":"...","message":"..."}}
//   pushed, if asked:    {"event":"revision","rig":"live","revision":N}
//
// Safety, in the order a connection meets it: the listener is bound to
// 127.0.0.1 and nothing else; the first line must carry the token from the
// connection file (random per start, readable by the user only) within a few
// seconds, or the connection is closed; a line longer than the limit, too
// many lines that are not requests, or requests faster than the limit close
// it too; and every request then goes through AgentHub::run, which applies
// the permission policy and the folder allow-list. Nothing listens while the
// server is stopped, which is the default.
//
// Threads: one that accepts and one per connection, none of them the audio
// thread and none of them touching the processor: a request is handed to the
// message thread (AgentHub::runFromAnyThread) and its answer written back. A
// slow command's work runs on its own connection's thread, so it delays only
// that client.

// One way of carrying lines between the server and its clients. The server
// is written against this, so a WebSocket transport can sit beside the TCP
// one without the server changing.
class CommandTransport {
public:
  // One client. receive and send may be called from one thread while close
  // is called from another: close only ends the traffic, and the connection
  // is let go of the socket when it is destroyed, after that thread is done.
  class Connection {
  public:
    enum class Received { message, nothing, closed, tooLong };

    virtual ~Connection() = default;
    // Waits up to `timeoutMs` for the next whole message. `tooLong` when the
    // client sent more than `maxBytes` without finishing one.
    virtual Received receive(std::string& message, int timeoutMs, int maxBytes) = 0;
    virtual bool send(const std::string& message) = 0;
    virtual void close() = 0;
  };

  virtual ~CommandTransport() = default;
  // Starts listening on the loopback interface. `port` 0 picks a free one.
  virtual bool listen(int port) = 0;
  virtual int boundPort() const = 0;
  // Blocks until a client connects or stopListening is called (then null).
  virtual std::unique_ptr<Connection> accept() = 0;
  virtual void stopListening() = 0;
  // Closes the listening socket. Called once no thread waits in accept any
  // more (after stopListening and the accepting thread's end), since closing
  // it under that thread is the same race as shutDownSocket describes.
  virtual void releaseListener() {}
  // The name written to the connection file ("tcp-ndjson").
  virtual juce::String name() const = 0;
};

// Ends a socket's traffic from another thread without closing it: a read or
// write waiting in it returns, but the handle stays the socket's own until
// the thread that uses it is done and the socket is destroyed. Closing it
// from that other thread instead (StreamingSocket::close) frees the handle
// and changes the object under the thread still using them, and the next
// accept can be handed the same handle number.
void shutDownSocket(juce::StreamingSocket& socket);

// Newline-delimited JSON over TCP on 127.0.0.1, with JUCE's sockets.
std::unique_ptr<CommandTransport> makeTcpLineTransport();

// One JSON object per WebSocket text message (RFC 6455) on 127.0.0.1, for a
// browser remote. A request is refused unless its Host names the loopback
// address and the bound port, and unless it has no Origin header (native
// clients) or one of `allowedOrigins`; the default list is empty, so browsers
// are refused until a caller names the remote's origin.
std::unique_ptr<CommandTransport> makeWebSocketTransport(std::vector<juce::String> allowedOrigins = {});

class CommandServer {
public:
  struct Limits {
    // Longest request line. Requests are small; a megabyte leaves room for a
    // long reorder or a pasted note and stops a client from making the
    // server buffer without end.
    int maxRequestBytes = 1024 * 1024;
    // Clients: connections that have presented the token.
    int maxConnections = 8;
    // Beside those: connections that have not presented it yet. When these
    // few are taken and another connection arrives, the one that has waited
    // longest is closed, once it has had `unauthenticatedGraceMs` to speak.
    // Sockets that say nothing can therefore never keep a real client out.
    int maxUnauthenticated = 2;
    int unauthenticatedGraceMs = 300;
    // How long a new connection has to present the token.
    int authTimeoutMs = 5000;
    // Requests one connection may send within any one second.
    int maxRequestsPerSecond = 400;
    // Lines in a row that are not a request before the connection is dropped.
    int maxBadRequests = 8;
    // How long a request may wait for the message thread to pick it up.
    int commandTimeoutMs = 15000;
    // How often a connection looks for a new revision to push.
    int eventPollMs = 100;
  };

  // `transport` defaults to the TCP one.
  explicit CommandServer(AgentHub& hub, std::unique_ptr<CommandTransport> transport = nullptr);
  ~CommandServer();

  // Starts listening and writes the connection file, replacing one left by
  // a process that has gone. `port` 0 picks a free port. Returns an error
  // message, empty on success. Message thread.
  juce::String start(int port);
  // Closes every connection, stops listening and removes the connection
  // file if it is still this server's (another instance may have written its
  // own since). Returns promptly whatever the clients are in the middle of:
  // each connection's slow work is cancelled, none is waited out. Message
  // thread.
  void stop();
  bool isRunning() const { return running.load(); }
  int port() const { return boundPort.load(); }
  int connectionCount() const;

  // Where a client finds the port and the token:
  // <app data>/Pedaleira/command-server.json, holding
  // { "protocol", "transport", "host", "port", "token", "pid", "started" }.
  // On POSIX it is written under a temporary name opened with O_CREAT |
  // O_EXCL | O_NOFOLLOW and mode 0600, then renamed into place, in a folder
  // that is 0700 when this class made it; on Windows it relies on the ACL of
  // the user's profile.
  static juce::File connectionFile();
  // Tests and the headless host: write the connection file there instead.
  // Process wide; an empty File restores the default. Returns the previous
  // override.
  static juce::File setConnectionFileForTesting(const juce::File& file);

  // Set before start().
  Limits limits;

  static constexpr int kProtocolVersion = 1;

private:
  struct Client;

  void acceptLoop();
  // serve() with nothing thrown allowed out: a failure closes that
  // connection with an error and the process carries on.
  void serveGuarded(const std::shared_ptr<Client>& client);
  void serve(const std::shared_ptr<Client>& client);
  bool authenticate(Client& client);
  void reapFinishedClients(bool all);
  bool writeConnectionFile();
  void removeConnectionFile();

  AgentHub& hub;
  std::unique_ptr<CommandTransport> transport;
  std::string token;
  // The connection file this server wrote; empty while there is none.
  juce::File writtenFile;
  std::atomic<bool> running{false};
  std::atomic<bool> stopping{false};
  std::atomic<int> boundPort{0};
  std::thread acceptThread;

  mutable std::mutex clientsMutex;
  std::vector<std::shared_ptr<Client>> clients;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CommandServer)
};

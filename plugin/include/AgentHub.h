#pragma once

#include "CommandRegistry.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

class CommandServer;
class GuitarLibrary;
class AssetTags;
class CaptureStudio;
class SandboxRig;
class TONE3000Processor;
// The rig a sandbox.applyToLive request was made for (AgentHub.cpp).
struct ApprovedRig;

// The agent layer: everything between "a caller that is not the user" and the
// engine's commands. A command server client (an MCP server, later the
// remote) never reaches the registry directly; it goes through
// AgentHub::run with its session, and the hub decides
//
//   - whether the command may run at all (AgentPolicy, the allowed folders),
//   - which rig it acts on (the one being played, or the sandbox rig the
//     session has selected),
//   - whether it has to wait for the user (the approval queue),
//
// and keeps the record a settings page shows: who is connected, what they
// ran, what is waiting for approval.
//
// The hub also registers the commands that belong to the application rather
// than to one rig (sandbox.*, render.*, guitar.*, capture.*, assets.list, agents.*,
// approvals.*, permissions.*, server.*, roots.*, events.*), so they are in
// commands.describe and docs/commands.md with everything else.
//
// Threading: message thread, except what says otherwise. docs/agents.md is
// the write-up.

// One switch per permission class. The defaults are the safe ones: an agent
// can look, build in a sandbox and measure, but cannot change what is being
// played, delete anything or touch device settings until the user says so.
struct AgentPolicy {
  bool read = true;       // read library, presets and rig state
  bool edit = true;       // create and edit presets and rigs
  bool render = true;     // render audio clips to measure a tone
  bool live = false;      // change the rig being played
  bool destroy = false;   // delete presets and library entries
  bool settings = false;  // change audio, device and machine-wide settings

  bool allows(CommandPermission permission) const;
  juce::var toVar() const;
};

// The agent layer's own settings file: <app data>/Pedaleira/agents.settings,
// next to the engine's preferences file and kept apart from it for the same
// reason the UI's is (two PropertiesFile objects on one file overwrite each
// other). Opened for one read or one write at a time.
namespace AgentSettings {
juce::File file();
// Tests and the headless host: keep the settings in `file` instead. Process
// wide; an empty File restores the default. Returns the previous override.
juce::File setFileForTesting(const juce::File& file);

AgentPolicy loadPolicy();
void savePolicy(const AgentPolicy& policy);
bool serverEnabled();
void setServerEnabled(bool enabled);
// 0: any free port.
int serverPort();
void setServerPort(int port);
// Folders the user added to the allowed ones.
juce::StringArray userRoots();
void setUserRoots(const juce::StringArray& roots);

// Capture (docs/capture.md). The Python executable the trainer runs under
// (empty: python3 on the path), the trainer's standard input file (the one
// the NAM project publishes; empty: not set), and the level the capture's
// peak should reach.
juce::String capturePython();
void setCapturePython(const juce::String& python);
juce::String captureInputFile();
void setCaptureInputFile(const juce::String& path);
double captureTargetDb();
void setCaptureTargetDb(double db);
}  // namespace AgentSettings

// One caller of the agent layer: a command server connection, or the local
// session that stands for in-process callers (the UI, tests, the headless
// host's own input).
struct AgentSession {
  int id = 0;
  juce::String name;
  // Local sessions are not gated: no permission checks, no approvals, and
  // the local-only commands are theirs.
  bool local = false;
  juce::Time connected;

  // Set by agents.disconnect; the transport closes the connection when it
  // sees it.
  std::atomic<bool> disconnectRequested{false};
  // Set by events.subscribe; the transport then sends revision events.
  std::atomic<bool> wantsRevisionEvents{false};
  // Set when the session is on its way out: its connection is being closed
  // (the server stops, agents.disconnect, the session is closed) or the hub
  // shuts down. The session's slow work reads it between steps and gives up
  // with `unavailable`, and so does a wait for the message thread made on
  // its behalf, so nothing a client is in the middle of can hold up a quit.
  std::atomic<bool> cancelled{false};

  // The rest is guarded by the hub's mutex.
  // The sandbox rig the session's rig commands act on; empty: the rig being
  // played.
  juce::String sandboxId;
  juce::String lastCommand;
  juce::Time lastCommandTime;
  int commandCount = 0;
};

class AgentHub {
public:
  // `liveRegistry` is the app-scope registry over `live`; the hub registers
  // its own commands into it (registerCommands) and must be destroyed before
  // it.
  AgentHub(TONE3000Processor& live, CommandRegistry& liveRegistry);
  ~AgentHub();

  // Adds the agent layer's commands to the live registry. Called once, after
  // the rig commands are in, which is how the hub knows which ones are rig
  // commands.
  void registerCommands();

  // ---- Sessions (any thread) ----
  std::shared_ptr<AgentSession> openSession(const juce::String& clientName);
  // Also discards the sandbox rigs the session made (on the message thread,
  // later).
  void closeSession(const std::shared_ptr<AgentSession>& session);
  AgentSession& localSession() { return *local; }

  // ---- Running commands ----
  // Message thread. What a transport calls instead of registry.run: gate,
  // route, run, record. begin() leaves a slow command's work to the caller.
  CommandWork begin(AgentSession& session, const juce::String& name, const juce::var& args);
  CommandResult run(AgentSession& session, const juce::String& name, const juce::var& args = {});
  // Any thread but the audio thread: begin() on the message thread, a slow
  // command's work on the calling one. `unavailable` when the message thread
  // does not pick it up within `timeoutMs` or the hub is shutting down.
  CommandResult runFromAnyThread(const std::shared_ptr<AgentSession>& session,
                                 const juce::String& name, const juce::var& args, int timeoutMs);

  // The revision of the rig a session's commands act on, and that rig's name
  // ("live" or the sandbox id). Any thread; an atomic read under a short
  // lock, which is what lets a transport poll it for change events.
  struct RigRevision {
    juce::String rig;
    juce::uint32 revision = 0;
  };
  RigRevision revisionFor(const AgentSession& session) const;

  // ---- Policy ----
  AgentPolicy policy() const;
  void setPolicy(const AgentPolicy& newPolicy, bool persist = true);

  // ---- Allowed folders ----
  // The library root, the model stash, the scratch folder and the folders
  // the user added. Any thread.
  std::vector<juce::File> allowedRoots() const;
  bool isPathAllowed(const juce::File& path) const;
  // For this run only (the headless host's --allow-root); not persisted.
  void addSessionRoot(const juce::File& root);

  // ---- The command server ----
  // Starts or stops it. `port` 0 picks a free port, -1 the persisted one.
  // Returns an error message, empty on success.
  juce::String setServerEnabled(bool enabled, bool persist = true, int port = -1);
  // Starts the server if the persisted setting says so (application start).
  void applyServerSetting();
  CommandServer* server() const { return commandServer.get(); }

  // ---- For the UI ----
  // Called on the message thread when a request starts or stops waiting for
  // approval, so a prompt can be shown; approvals.list has the details.
  std::function<void()> onApprovalsChanged;
  int pendingApprovalCount() const;
  // Called on the message thread whenever the tone request ("Describe a
  // tone") changes: made, claimed, a step reported, proposed, answered,
  // stopped. toneRequest.get has the details.
  std::function<void()> onToneRequestChanged;

  // ---- Pieces the command files share ----
  TONE3000Processor& liveRig() { return live; }
  CommandRegistry& registry() { return liveCommands; }
  GuitarLibrary& guitars() { return *guitarLibrary; }
  // The capture wizard's engine (CaptureStudio.h).
  CaptureStudio& captures() { return *captureStudio; }
  // The user's tags on captures and IRs (assets.tag, assets.list).
  AssetTags& assetTags() { return *tagIndex; }
  // The session whose command is running (message thread, inside a handler);
  // the local session when the registry was called directly.
  AgentSession& currentSession() { return current != nullptr ? *current : *local; }
  // True once the hub is being destroyed: slow work stops at its next step.
  const std::atomic<bool>& shuttingDown() const { return closing; }
  // What the slow work of the command now running reads between steps: the
  // calling session's flag, which is set when its connection goes and when
  // the hub shuts down. Asked on the message thread, inside a handler; the
  // session outlives the work it started.
  const std::atomic<bool>& slowWorkCancel() { return currentSession().cancelled; }
  // callOnMessageThread that gives up when the hub shuts down.
  bool onMessageThread(std::function<void()> call, int timeoutMs = 10000) const;

  // Renders running at once, over all connections: each holds the clips, the
  // output and the measurement buffers of what it plays, so the count is
  // what bounds the memory a set of clients can make the application take.
  static constexpr int kMaxConcurrentRenders = 2;
  // False when that many are already running. Any thread.
  bool tryBeginRender();
  void endRender();

  // Sandbox rigs. Message thread.
  static constexpr int kMaxSandboxes = 4;
  // By id alone, whoever owns it: for the hub's own pieces and for tests.
  // Never with an id a caller supplied; that is sandboxFor.
  std::shared_ptr<SandboxRig> findSandbox(const juce::String& id) const;
  // The sandbox `id` as `session` may address it: one it made itself (a
  // local caller, who is the user: anybody's), and never the private copy a
  // render of the live rig makes for itself. Null otherwise, which a command
  // answers with not_found: whether a sandbox of that id exists is not a
  // client's to learn.
  std::shared_ptr<SandboxRig> sandboxFor(const AgentSession& session, const juce::String& id) const;
  // "live", "empty" or "preset" (then `presetId`). Null with `error` set
  // when it cannot be made.
  // A `temporary` one (a render's private copy of the live rig) does not
  // count against the limit.
  std::shared_ptr<SandboxRig> createSandbox(const juce::String& from, const juce::String& presetId,
                                            int ownerSession, juce::String& error,
                                            bool temporary = false);
  // False when the rig is in use by a render or a slow command.
  bool discardSandbox(const juce::String& id);
  // The same from any thread: now on the message thread, else on its next
  // turn (or with the hub, when that comes first).
  void discardSandboxSoon(const juce::String& id);

private:
  struct Approval;
  struct Activity {
    juce::int64 sequence = 0;
    juce::Time time;
    int sessionId = 0;
    juce::String client, command, args, rig, code;
  };

  void registerAgentCommands();
  void registerSandboxCommands();
  // ListenCommands.cpp
  void registerListenCommands();
  // GuitarCommands.cpp: guitar.* and assets.*.
  void registerGuitarCommands();
  // NamCommands.cpp: nam.*.
  void registerNamCommands();
  // ToneRequestCommands.cpp: toneRequest.*.
  void registerToneRequestCommands();
  // CaptureCommands.cpp: capture.*.
  void registerCaptureCommands();
  // The tone request lets go of a session that has gone: back to waiting
  // for an agent, its proposal dropped (the sandbox went with the session).
  // Message thread.
  void releaseToneRequest(int sessionId);
  // A change to the tone request: bumps its sequence (which a waiting
  // agent polls) and tells the UI. Message thread.
  void toneRequestChanged();

  // The sandbox a session's rig commands go to, or null for the live rig.
  std::shared_ptr<SandboxRig> selectedSandbox(const AgentSession& session) const;
  // Whether `session` may address `rig` (see sandboxFor). Lock held or not:
  // it reads only what never changes after the rig is made.
  static bool mayAddress(const AgentSession& session, const SandboxRig& rig);
  // Sets `cancelled` on every client session, and on the local one too when
  // the hub itself is going.
  void cancelSessions(bool includingLocal);
  bool isRigCommand(const juce::String& name) const;
  // Empty when every path argument lies in an allowed folder.
  juce::String pathRefusal(const Command& command, const juce::var& args) const;
  juce::int64 recordActivity(const AgentSession& session, const juce::String& command,
                             const juce::var& args, const juce::String& rig,
                             const juce::String& code);
  void finishActivity(juce::int64 sequence, const juce::String& code);
  // `applied`, for sandbox.applyToLive: the sandbox's rig as it is now, kept
  // with the request and applied when it is approved.
  CommandResult queueApproval(AgentSession& session, const Command& command,
                              const juce::var& args, const juce::String& sandboxId,
                              std::unique_ptr<ApprovedRig> applied);
  // Puts the rig an approval captured onto the live rig.
  CommandResult applyApprovedRig(const Approval& approval);
  void expireApprovals();
  // `forUser`: with what a prompt needs and a client's poll does not (whether
  // the sandbox has moved on since the request was made).
  juce::var approvalVar(const Approval& approval, bool withResult, bool forUser = false) const;
  void discardSandboxesOf(int sessionId);

  TONE3000Processor& live;
  CommandRegistry& liveCommands;
  juce::StringArray rigCommandNames;

  // Guards the session list, each session's guarded fields, the sandbox map,
  // the policy, the activity ring and the session roots: everything a
  // transport thread reads.
  mutable std::mutex mutex;
  std::shared_ptr<AgentSession> local;
  std::vector<std::shared_ptr<AgentSession>> sessions;
  int nextSessionId = 1;
  std::map<juce::String, std::shared_ptr<SandboxRig>> sandboxes;
  int nextSandboxNumber = 1;
  mutable std::optional<AgentPolicy> cachedPolicy;
  std::deque<Activity> activity;
  juce::int64 nextActivity = 1;
  std::vector<juce::File> sessionRoots;

  // Message thread only.
  AgentSession* current = nullptr;
  std::vector<std::unique_ptr<Approval>> approvals;
  int nextApproval = 1;

  std::unique_ptr<GuitarLibrary> guitarLibrary;
  std::unique_ptr<CaptureStudio> captureStudio;
  // "Describe a tone": the one request the user has open (ToneRequestCommands.cpp).
  // Changed on the message thread only; `toneMutex` guards it for the
  // connection threads that wait on it.
  struct ToneRequest;
  mutable std::mutex toneMutex;
  std::unique_ptr<ToneRequest> toneRequest;
  std::atomic<juce::int64> toneSequence{0};
  int nextToneRequest = 1;
  std::unique_ptr<AssetTags> tagIndex;
  std::unique_ptr<CommandServer> commandServer;
  std::atomic<bool> closing{false};
  std::atomic<int> activeRenders{0};
  // Lets work posted to the message thread find out the hub is gone.
  std::shared_ptr<bool> lifetime;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AgentHub)
};

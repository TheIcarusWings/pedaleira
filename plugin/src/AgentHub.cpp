#include "AgentHub.h"

// Processor.h first: it pulls in the JUCE module headers, which must come
// before anything that drags in <windows.h>.
#include "Processor.h"

#include "AssetTags.h"
#include "CaptureStudio.h"
#include "CommandServer.h"
#include "EngineCommands.h"
#include "GuitarProfiles.h"
#include "LibraryPaths.h"
#include "SandboxRig.h"
#include "ToneRequestState.h"

#include <juce_data_structures/juce_data_structures.h>

#include <algorithm>
#include <utility>

namespace {

using P = CommandParam;
using T = CommandParamType;
using Properties = std::initializer_list<std::pair<const char*, juce::var>>;

constexpr int kActivityLimit = 200;
constexpr int kMaxPendingApprovals = 16;
constexpr int kMaxPendingApprovalsPerSession = 4;
constexpr int kKeptResolvedApprovals = 50;
constexpr juce::int64 kApprovalLifetimeMs = 10 * 60 * 1000;

juce::var object(Properties properties) {
  juce::DynamicObject::Ptr obj = new juce::DynamicObject();
  for (const auto& [name, value] : properties)
    obj->setProperty(name, value);
  return juce::var(obj.get());
}

CommandResult ok(Properties properties) { return CommandResult::success(object(properties)); }

CommandResult fail(const char* code, const juce::String& message) {
  return CommandResult::failure(code, message);
}

juce::String isoTime(const juce::Time& time) {
  return time.toMilliseconds() > 0 ? time.toISO8601(true) : juce::String();
}

// A client's own name for itself, made safe to show and to log.
juce::String tidyClientName(const juce::String& name) {
  juce::String out;
  for (const juce::juce_wchar c : name.trim().substring(0, 64))
    out << juce::String::charToString(c < 32 || c == 127 ? juce::juce_wchar(' ') : c);
  return out.trim().isEmpty() ? juce::String("unnamed client") : out.trim();
}

// Arguments as one short line, for the activity log.
juce::String compactArgs(const juce::var& args) {
  if (args.isVoid() || args.isUndefined())
    return "{}";
  const juce::String text = juce::JSON::toString(args, true);
  return text.length() > 160 ? text.substring(0, 157) + "..." : text;
}

// ---- Settings file ----
std::mutex& settingsMutex() {
  static std::mutex mutex;
  return mutex;
}

juce::File& settingsOverride() {
  static juce::File file;
  return file;
}

// Opened for one read or one write and closed again, like the footswitch
// settings: two PropertiesFile objects kept open on one file overwrite each
// other's keys.
std::unique_ptr<juce::PropertiesFile> openSettings() {
  return std::make_unique<juce::PropertiesFile>(AgentSettings::file(),
                                                juce::PropertiesFile::Options());
}

constexpr const char* kServerEnabledKey = "serverEnabled";
constexpr const char* kServerPortKey = "serverPort";
constexpr const char* kUserRootsKey = "allowedFolders";
constexpr const char* kCapturePythonKey = "capturePython";
constexpr const char* kCaptureInputKey = "captureInputFile";
constexpr const char* kCaptureTargetKey = "captureTargetDb";

struct PolicySwitch {
  const char* key;      // settings key
  const char* name;     // name in commands
  bool AgentPolicy::*field;
};
constexpr PolicySwitch kPolicySwitches[] = {
    {"allowRead", "read", &AgentPolicy::read},
    {"allowEdit", "edit", &AgentPolicy::edit},
    {"allowRender", "render", &AgentPolicy::render},
    {"allowLive", "live", &AgentPolicy::live},
    {"allowDelete", "delete", &AgentPolicy::destroy},
    {"allowSettings", "settings", &AgentPolicy::settings},
};

}  // namespace

// ####################
// POLICY AND SETTINGS
// ####################
bool AgentPolicy::allows(CommandPermission permission) const {
  switch (permission) {
    case CommandPermission::read: return read;
    case CommandPermission::edit: return edit;
    case CommandPermission::render: return render;
    case CommandPermission::live: return live;
    case CommandPermission::destroy: return destroy;
    case CommandPermission::settings: return settings;
    case CommandPermission::local:
    case CommandPermission::unclassified: break;
  }
  return false;
}

juce::var AgentPolicy::toVar() const {
  juce::DynamicObject::Ptr obj = new juce::DynamicObject();
  for (const auto& entry : kPolicySwitches)
    obj->setProperty(entry.name, this->*entry.field);
  return juce::var(obj.get());
}

namespace AgentSettings {

juce::File file() {
  {
    const std::lock_guard<std::mutex> lock(settingsMutex());
    if (settingsOverride() != juce::File())
      return settingsOverride();
  }
  return LibraryPaths::appDataDir().getChildFile("agents.settings");
}

juce::File setFileForTesting(const juce::File& newFile) {
  const std::lock_guard<std::mutex> lock(settingsMutex());
  return std::exchange(settingsOverride(), newFile);
}

AgentPolicy loadPolicy() {
  const auto settings = openSettings();
  AgentPolicy policy;
  for (const auto& entry : kPolicySwitches)
    policy.*entry.field = settings->getBoolValue(entry.key, policy.*entry.field);
  return policy;
}

void savePolicy(const AgentPolicy& policy) {
  const auto settings = openSettings();
  for (const auto& entry : kPolicySwitches)
    settings->setValue(entry.key, policy.*entry.field);
  settings->saveIfNeeded();
}

bool serverEnabled() { return openSettings()->getBoolValue(kServerEnabledKey, false); }

void setServerEnabled(bool enabled) {
  const auto settings = openSettings();
  settings->setValue(kServerEnabledKey, enabled);
  settings->saveIfNeeded();
}

int serverPort() { return juce::jlimit(0, 65535, openSettings()->getIntValue(kServerPortKey, 0)); }

void setServerPort(int port) {
  const auto settings = openSettings();
  settings->setValue(kServerPortKey, juce::jlimit(0, 65535, port));
  settings->saveIfNeeded();
}

juce::StringArray userRoots() {
  juce::StringArray roots;
  roots.addLines(openSettings()->getValue(kUserRootsKey));
  roots.removeEmptyStrings();
  return roots;
}

void setUserRoots(const juce::StringArray& roots) {
  const auto settings = openSettings();
  settings->setValue(kUserRootsKey, roots.joinIntoString("\n"));
  settings->saveIfNeeded();
}

juce::String capturePython() { return openSettings()->getValue(kCapturePythonKey).trim(); }

void setCapturePython(const juce::String& python) {
  const auto settings = openSettings();
  settings->setValue(kCapturePythonKey, python.trim());
  settings->saveIfNeeded();
}

juce::String captureInputFile() { return openSettings()->getValue(kCaptureInputKey).trim(); }

void setCaptureInputFile(const juce::String& path) {
  const auto settings = openSettings();
  settings->setValue(kCaptureInputKey, path.trim());
  settings->saveIfNeeded();
}

double captureTargetDb() {
  return juce::jlimit(-30.0, -1.0, openSettings()->getDoubleValue(kCaptureTargetKey, -12.0));
}

void setCaptureTargetDb(double db) {
  const auto settings = openSettings();
  settings->setValue(kCaptureTargetKey, juce::jlimit(-30.0, -1.0, db));
  settings->saveIfNeeded();
}

}  // namespace AgentSettings

// ####################
// HUB
// ####################
// What a request to apply a sandbox is a request for: that sandbox's rig as
// it was when the request was made. The user is asked about this, and this
// is what an approval applies, whatever the sandbox has become since.
struct ApprovedRig {
  juce::String sandboxId;
  SandboxRig::Snapshot rig;
};

struct AgentHub::Approval {
  juce::String id;
  int sessionId = 0;
  // `sandboxId`: the rig the session had selected, which is where an
  // approved rig command is routed.
  juce::String client, command, sandboxId;
  juce::var args;
  juce::Time created, resolved;
  // pending, approved, denied or expired
  juce::String state = "pending";
  juce::var result;
  // sandbox.applyToLive only.
  std::unique_ptr<ApprovedRig> applied;
};

AgentHub::AgentHub(TONE3000Processor& liveProcessor, CommandRegistry& liveRegistry)
    : live(liveProcessor), liveCommands(liveRegistry), lifetime(std::make_shared<bool>(true)) {
  local = std::make_shared<AgentSession>();
  local->id = 0;
  local->name = "local";
  local->local = true;
  local->connected = juce::Time::getCurrentTime();
  guitarLibrary = std::make_unique<GuitarLibrary>(live);
  captureStudio = std::make_unique<CaptureStudio>(live, liveCommands);
  tagIndex = std::make_unique<AssetTags>();
}

AgentHub::~AgentHub() {
  // Server threads may be waiting for this thread; the flag makes them stop
  // waiting, and the server's own stop then joins them.
  closing.store(true);
  cancelSessions(/*includingLocal=*/true);
  commandServer.reset();
  captureStudio.reset();
  guitarLibrary.reset();
  sandboxes.clear();
}

void AgentHub::cancelSessions(bool includingLocal) {
  const std::lock_guard<std::mutex> lock(mutex);
  for (const auto& session : sessions)
    session->cancelled.store(true);
  if (includingLocal)
    local->cancelled.store(true);
}

bool AgentHub::tryBeginRender() {
  int running = activeRenders.load();
  while (running < kMaxConcurrentRenders)
    if (activeRenders.compare_exchange_weak(running, running + 1))
      return true;
  return false;
}

void AgentHub::endRender() { activeRenders.fetch_sub(1); }

void AgentHub::registerCommands() {
  rigCommandNames = liveCommands.names();
  registerAgentCommands();
  registerSandboxCommands();
  registerListenCommands();
  registerGuitarCommands();
  registerNamCommands();
  registerCaptureCommands();
  registerToneRequestCommands();
}

bool AgentHub::isRigCommand(const juce::String& name) const {
  return rigCommandNames.contains(name);
}

bool AgentHub::onMessageThread(std::function<void()> call, int timeoutMs) const {
  return CommandRegistry::callOnMessageThread(std::move(call), timeoutMs, &closing);
}

// ---- Sessions ----
std::shared_ptr<AgentSession> AgentHub::openSession(const juce::String& clientName) {
  auto session = std::make_shared<AgentSession>();
  session->name = tidyClientName(clientName);
  session->connected = juce::Time::getCurrentTime();
  const std::lock_guard<std::mutex> lock(mutex);
  session->id = nextSessionId++;
  sessions.push_back(session);
  return session;
}

void AgentHub::closeSession(const std::shared_ptr<AgentSession>& session) {
  if (session == nullptr)
    return;
  // Whatever it still has running lets go.
  session->cancelled.store(true);
  {
    const std::lock_guard<std::mutex> lock(mutex);
    sessions.erase(std::remove(sessions.begin(), sessions.end(), session), sessions.end());
  }
  // Its sandbox rigs hold processors, which only the message thread may
  // destroy.
  const int id = session->id;
  if (juce::MessageManager::existsAndIsCurrentThread()) {
    releaseToneRequest(id);
    discardSandboxesOf(id);
    return;
  }
  std::weak_ptr<bool> alive = lifetime;
  juce::MessageManager::callAsync([this, alive, id] {
    if (alive.expired())
      return;
    releaseToneRequest(id);
    discardSandboxesOf(id);
  });
}

void AgentHub::discardSandboxesOf(int sessionId) {
  std::vector<std::shared_ptr<SandboxRig>> owned;
  {
    const std::lock_guard<std::mutex> lock(mutex);
    for (const auto& [id, rig] : sandboxes)
      if (rig->ownerSession == sessionId)
        owned.push_back(rig);
  }
  for (const auto& rig : owned) {
    // One that is in use (a render is running through it) cannot go yet. The
    // mark is set before the attempt, so whoever ends that use either sees
    // it and has the rig discarded, or had already let go and the attempt
    // succeeds: it is never left behind for the life of the hub.
    rig->orphaned.store(true);
    discardSandbox(rig->id);
  }
  owned.clear();  // on this thread, the message thread: processors go here

  // Nobody is left to read the outcome of what it asked for.
  bool changed = false;
  for (auto& approval : approvals)
    if (approval->state == "pending" && approval->sessionId == sessionId) {
      approval->state = "expired";
      approval->resolved = juce::Time::getCurrentTime();
      changed = true;
    }
  if (changed && onApprovalsChanged != nullptr)
    onApprovalsChanged();
}

// ---- Policy ----
AgentPolicy AgentHub::policy() const {
  const std::lock_guard<std::mutex> lock(mutex);
  // Read on first use, not at construction: every EngineCommands has a hub,
  // and most of them (tests, the render tool) never ask.
  if (!cachedPolicy.has_value())
    cachedPolicy = AgentSettings::loadPolicy();
  return *cachedPolicy;
}

void AgentHub::setPolicy(const AgentPolicy& newPolicy, bool persist) {
  {
    const std::lock_guard<std::mutex> lock(mutex);
    cachedPolicy = newPolicy;
  }
  if (persist)
    AgentSettings::savePolicy(newPolicy);
}

// ---- Allowed folders ----
std::vector<juce::File> AgentHub::allowedRoots() const {
  // The root of the library the live rig uses: LibraryPaths::libraryRoot()
  // unless that processor's library was rooted elsewhere for a test.
  std::vector<juce::File> roots{live.getLibraryRoot(), TONE3000Processor::getLocalModelsDir()};
  // Not when it cannot be used (it is a link): then nothing is allowed there.
  if (const juce::File scratch = LibraryPaths::scratchDir(); scratch != juce::File())
    roots.push_back(scratch);
  for (const auto& path : AgentSettings::userRoots())
    if (juce::File::isAbsolutePath(path))
      roots.emplace_back(path);
  const std::lock_guard<std::mutex> lock(mutex);
  roots.insert(roots.end(), sessionRoots.begin(), sessionRoots.end());
  return roots;
}

bool AgentHub::isPathAllowed(const juce::File& path) const {
  return LibraryPaths::isInsideAny(path, allowedRoots());
}

void AgentHub::addSessionRoot(const juce::File& root) {
  const std::lock_guard<std::mutex> lock(mutex);
  sessionRoots.push_back(root);
}

juce::String AgentHub::pathRefusal(const Command& command, const juce::var& args) const {
  const auto check = [&](const juce::var& value, const CommandParam& param) -> juce::String {
    if (!value.isString())
      return {};  // the registry's own validation reports the type
    const juce::String text = value.toString();
    if (!juce::File::isAbsolutePath(text))
      return param.pathMayBeName ? juce::String() : "path '" + text + "' is not absolute";
    if (!isPathAllowed(juce::File(text)))
      return "path '" + text + "' is outside the allowed folders (see roots.list)";
    // The folder the command uses beside the file is held to the same
    // folders: one that is a link to somewhere else resolves to there.
    if (param.pathSibling.isNotEmpty() &&
        !isPathAllowed(juce::File(text).getSiblingFile(param.pathSibling)))
      return "the " + param.pathSibling + " folder beside '" + text +
             "' is outside the allowed folders (see roots.list)";
    return {};
  };
  for (const auto& param : command.params) {
    if (!param.isPath)
      continue;
    const juce::var value = args[juce::Identifier(param.name)];
    if (const auto* items = value.getArray()) {
      for (const auto& item : *items)
        if (const juce::String problem = check(item, param); problem.isNotEmpty())
          return problem;
    } else if (const juce::String problem = check(value, param); problem.isNotEmpty()) {
      return problem;
    }
  }
  return {};
}

// ---- Activity ----
juce::int64 AgentHub::recordActivity(const AgentSession& session, const juce::String& command,
                                     const juce::var& args, const juce::String& rig,
                                     const juce::String& code) {
  Activity entry;
  entry.time = juce::Time::getCurrentTime();
  entry.sessionId = session.id;
  entry.client = session.name;
  entry.command = command;
  entry.args = compactArgs(args);
  entry.rig = rig;
  entry.code = code;
  const std::lock_guard<std::mutex> lock(mutex);
  entry.sequence = nextActivity++;
  activity.push_back(entry);
  while (static_cast<int>(activity.size()) > kActivityLimit)
    activity.pop_front();
  return entry.sequence;
}

void AgentHub::finishActivity(juce::int64 sequence, const juce::String& code) {
  const std::lock_guard<std::mutex> lock(mutex);
  for (auto& entry : activity)
    if (entry.sequence == sequence)
      entry.code = code;
}

// ---- Sandboxes ----
std::shared_ptr<SandboxRig> AgentHub::findSandbox(const juce::String& id) const {
  const std::lock_guard<std::mutex> lock(mutex);
  const auto found = sandboxes.find(id);
  return found != sandboxes.end() ? found->second : nullptr;
}

bool AgentHub::mayAddress(const AgentSession& session, const SandboxRig& rig) {
  return !rig.temporary && (session.local || rig.ownerSession == session.id);
}

std::shared_ptr<SandboxRig> AgentHub::sandboxFor(const AgentSession& session,
                                                 const juce::String& id) const {
  const std::lock_guard<std::mutex> lock(mutex);
  const auto found = sandboxes.find(id);
  return found != sandboxes.end() && mayAddress(session, *found->second) ? found->second : nullptr;
}

std::shared_ptr<SandboxRig> AgentHub::selectedSandbox(const AgentSession& session) const {
  const std::lock_guard<std::mutex> lock(mutex);
  if (session.sandboxId.isEmpty())
    return nullptr;
  const auto found = sandboxes.find(session.sandboxId);
  // A selection is only ever made of a sandbox the session may address
  // (sandbox.create, sandbox.use); checked again all the same.
  return found != sandboxes.end() && mayAddress(session, *found->second) ? found->second : nullptr;
}

std::shared_ptr<SandboxRig> AgentHub::createSandbox(const juce::String& from,
                                                    const juce::String& presetId,
                                                    int ownerSession, juce::String& error,
                                                    bool temporary) {
  JUCE_ASSERT_MESSAGE_THREAD
  juce::String id;
  {
    const std::lock_guard<std::mutex> lock(mutex);
    int kept = 0;
    for (const auto& [existingId, existing] : sandboxes)
      kept += existing->temporary ? 0 : 1;
    // Temporary copies are bounded by the connection limit: one render per
    // connection at a time.
    if (!temporary && kept >= kMaxSandboxes) {
      error = "there are already " + juce::String(kMaxSandboxes) +
              " sandbox rigs; discard one first (sandbox.list, sandbox.discard)";
      return nullptr;
    }
    id = "sandbox-" + juce::String(nextSandboxNumber++);
  }

  auto rig = std::make_shared<SandboxRig>(id, live);
  rig->ownerSession = ownerSession;
  rig->temporary = temporary;
  // Its slow commands stop for the same reasons the live rig's do.
  rig->commands().slowWorkCancel = [this] { return &slowWorkCancel(); };
  if (from == "live") {
    rig->copyStateFrom(live);
  } else if (from == "preset") {
    if (!rig->processor().loadPreset(presetId)) {
      error = "no preset with id '" + presetId + "', or it could not be loaded";
      return nullptr;
    }
  }
  const std::lock_guard<std::mutex> lock(mutex);
  sandboxes[id] = rig;
  return rig;
}

bool AgentHub::discardSandbox(const juce::String& id) {
  JUCE_ASSERT_MESSAGE_THREAD
  std::shared_ptr<SandboxRig> rig;
  {
    const std::lock_guard<std::mutex> lock(mutex);
    const auto found = sandboxes.find(id);
    if (found == sandboxes.end())
      return true;
    // A render or a slow command on another thread is still inside it.
    if (found->second->rendering.load() || found->second->activeUses.load() > 0)
      return false;
    rig = found->second;
    sandboxes.erase(found);
    for (auto& session : sessions)
      if (session->sandboxId == id)
        session->sandboxId.clear();
    if (local->sandboxId == id)
      local->sandboxId.clear();
  }
  rig.reset();  // the processor goes here, on the message thread
  return true;
}

void AgentHub::discardSandboxSoon(const juce::String& id) {
  if (juce::MessageManager::existsAndIsCurrentThread()) {
    discardSandbox(id);
    return;
  }
  std::weak_ptr<bool> alive = lifetime;
  juce::MessageManager::callAsync([this, alive, id] {
    if (!alive.expired())
      discardSandbox(id);
  });
}

AgentHub::RigRevision AgentHub::revisionFor(const AgentSession& session) const {
  const std::lock_guard<std::mutex> lock(mutex);
  if (session.sandboxId.isNotEmpty()) {
    const auto found = sandboxes.find(session.sandboxId);
    if (found != sandboxes.end() && mayAddress(session, *found->second))
      return {found->first, found->second->processor().getCurrentChainRevision()};
  }
  return {"live", live.getCurrentChainRevision()};
}

// ---- Approvals ----
int AgentHub::pendingApprovalCount() const {
  int count = 0;
  for (const auto& approval : approvals)
    if (approval->state == "pending")
      ++count;
  return count;
}

void AgentHub::expireApprovals() {
  const juce::Time now = juce::Time::getCurrentTime();
  bool changed = false;
  for (auto& approval : approvals)
    if (approval->state == "pending" &&
        (now - approval->created).inMilliseconds() > kApprovalLifetimeMs) {
      approval->state = "expired";
      approval->resolved = now;
      changed = true;
    }
  // Resolved requests are kept for a while so the client that asked can read
  // the outcome; the oldest go first.
  int resolved = 0;
  for (const auto& approval : approvals)
    if (approval->state != "pending")
      ++resolved;
  for (auto it = approvals.begin(); it != approvals.end() && resolved > kKeptResolvedApprovals;) {
    if ((*it)->state != "pending") {
      it = approvals.erase(it);
      --resolved;
    } else {
      ++it;
    }
  }
  if (changed && onApprovalsChanged != nullptr)
    onApprovalsChanged();
}

juce::var AgentHub::approvalVar(const Approval& approval, bool withResult, bool forUser) const {
  juce::var value = object({
      {"id", approval.id},
      {"state", approval.state},
      {"client", approval.client},
      {"sessionId", approval.sessionId},
      {"command", approval.command},
      {"args", approval.args},
      {"created", isoTime(approval.created)},
  });
  if (approval.sandboxId.isNotEmpty())
    value.getDynamicObject()->setProperty("sandbox", approval.sandboxId);
  if (approval.applied != nullptr) {
    // What an approval would apply: the sandbox's rig as captured when the
    // request was made.
    juce::var applies = object({
        {"sandbox", approval.applied->sandboxId},
        {"revision", static_cast<int>(approval.applied->rig.revision)},
        {"captured", isoTime(approval.created)},
    });
    if (forUser && approval.state == "pending") {
      // A preview rendered through the sandbox shows the request's rig only
      // while the sandbox is still that rig.
      const auto rig = findSandbox(approval.applied->sandboxId);
      applies.getDynamicObject()->setProperty(
          "sandboxChanged",
          rig == nullptr || !rig->capture().tree.isEquivalentTo(approval.applied->rig.tree));
    }
    value.getDynamicObject()->setProperty("applies", applies);
  }
  if (approval.state != "pending")
    value.getDynamicObject()->setProperty("resolved", isoTime(approval.resolved));
  if (withResult && !approval.result.isVoid())
    value.getDynamicObject()->setProperty("result", approval.result);
  return value;
}

CommandResult AgentHub::applyApprovedRig(const Approval& approval) {
  // For the client that asked, as an approved command runs.
  AgentSession proxy;
  proxy.id = approval.sessionId;
  proxy.name = approval.client;
  proxy.local = true;
  CommandResult result =
      SandboxRig::applySnapshot(approval.applied->rig, live)
          ? ok({
                {"applied", approval.applied->sandboxId},
                {"revision", static_cast<int>(live.getCurrentChainRevision())},
            })
          : fail(CommandError::failed, "the live rig refused the sandbox's rig");
  recordActivity(proxy, approval.command, approval.args, "live",
                 result.ok ? juce::String("ok") : result.errorCode);
  return result;
}

CommandResult AgentHub::queueApproval(AgentSession& session, const Command& command,
                                      const juce::var& args, const juce::String& sandboxId,
                                      std::unique_ptr<ApprovedRig> applied) {
  const auto answer = [&](const Approval& approval) {
    CommandResult result = fail(
        CommandError::needsApproval,
        command.name + " changes the rig being played, which is switched off for agents; the "
                       "request is waiting for the user. Poll approvals.status with id '" +
            approval.id + "'. (Edits to a sandbox rig need no approval: sandbox.create.)");
    result.errorDetails = object({{"approvalId", approval.id}});
    return result;
  };

  // An agent that asks again for the same thing is asking about the request
  // it already has.
  const juce::String argsText = juce::JSON::toString(args, true);
  const auto sameRig = [](const ApprovedRig& a, const ApprovedRig& b) {
    return a.sandboxId == b.sandboxId && a.rig.presetId == b.rig.presetId &&
           a.rig.presetName == b.rig.presetName && a.rig.edited == b.rig.edited &&
           a.rig.tree.isEquivalentTo(b.rig.tree);
  };
  int pending = 0, ownPending = 0;
  bool superseded = false;
  for (const auto& approval : approvals) {
    if (approval->state != "pending")
      continue;
    if (approval->sessionId == session.id && approval->command == command.name &&
        approval->sandboxId == sandboxId &&
        juce::JSON::toString(approval->args, true) == argsText) {
      if (applied == nullptr || (approval->applied != nullptr && sameRig(*approval->applied, *applied)))
        return answer(*approval);
      // The same words, but the sandbox is another rig now. The earlier
      // request stays what it was (an approval of it would apply the earlier
      // rig), so it is withdrawn rather than left for the user to pick from
      // two that look alike.
      approval->state = "expired";
      approval->resolved = juce::Time::getCurrentTime();
      superseded = true;
      continue;
    }
    ++pending;
    if (approval->sessionId == session.id)
      ++ownPending;
  }
  if (pending >= kMaxPendingApprovals || ownPending >= kMaxPendingApprovalsPerSession) {
    if (superseded && onApprovalsChanged != nullptr)
      onApprovalsChanged();
    return fail(CommandError::permissionDenied,
                "too many requests are already waiting for the user's approval");
  }

  auto approval = std::make_unique<Approval>();
  approval->id = "approval-" + juce::String(nextApproval++);
  approval->sessionId = session.id;
  approval->client = session.name;
  approval->command = command.name;
  approval->sandboxId = sandboxId;
  approval->args = args;
  approval->applied = std::move(applied);
  approval->created = juce::Time::getCurrentTime();
  const CommandResult result = answer(*approval);
  approvals.push_back(std::move(approval));
  if (onApprovalsChanged != nullptr)
    onApprovalsChanged();
  return result;
}

// ####################
// RUNNING COMMANDS
// ####################
CommandWork AgentHub::begin(AgentSession& session, const juce::String& name,
                            const juce::var& args) {
  JUCE_ASSERT_MESSAGE_THREAD
  expireApprovals();

  const Command* command = liveCommands.find(name);
  if (command == nullptr) {
    recordActivity(session, name, args, {}, CommandError::unknownCommand);
    return CommandWork::done(
        fail(CommandError::unknownCommand, "unknown command '" + name + "'"));
  }

  // Where it runs: a rig command goes to the session's sandbox rig when it
  // has selected one. Machine-wide settings are not a rig's, whichever
  // registry holds the command.
  const bool rigCommand = isRigCommand(name);
  std::shared_ptr<SandboxRig> sandbox = rigCommand ? selectedSandbox(session) : nullptr;
  if (sandbox != nullptr && (command->permission == CommandPermission::settings ||
                             !sandbox->commands().registry().contains(name)))
    sandbox = nullptr;
  const juce::String rigName =
      sandbox != nullptr ? sandbox->id : rigCommand ? juce::String("live") : juce::String();

  const auto refuse = [&](const char* code, const juce::String& message) {
    recordActivity(session, name, args, rigName, code);
    return CommandWork::done(fail(code, message));
  };

  if (!session.local) {
    const CommandPermission permission = command->permission;
    if (permission == CommandPermission::local || permission == CommandPermission::unclassified)
      return refuse(CommandError::permissionDenied,
                    name + " is only available inside the application, not to clients");

    const AgentPolicy allowed = policy();
    if (permission != CommandPermission::live && !allowed.allows(permission))
      return refuse(CommandError::permissionDenied,
                    name + " needs the '" + commandPermissionName(permission) +
                        "' permission, which is switched off (see permissions.get)");

    if (const juce::String problem = pathRefusal(*command, args); problem.isNotEmpty())
      return refuse(CommandError::permissionDenied, name + ": " + problem);

    // Changing what is being played: a live-class command, or any edit that
    // lands on the live rig rather than on a sandbox.
    const bool changesLive =
        permission == CommandPermission::live ||
        (permission == CommandPermission::edit && rigCommand && sandbox == nullptr &&
         command->mutates);
    if (changesLive && !allowed.live) {
      // Checked before anything is queued, so the user is never asked to
      // approve a request that could not have run.
      if (const auto invalid = liveCommands.check(name, args))
        return refuse(invalid->errorCode == CommandError::invalidArgs
                          ? CommandError::invalidArgs
                          : CommandError::unknownCommand,
                      invalid->errorMessage);
      const juce::String sandboxId = [&] {
        const std::lock_guard<std::mutex> lock(mutex);
        return session.sandboxId;
      }();
      // Applying a sandbox is asked about as the rig it is now: taken here,
      // so that nothing the client does to the sandbox between the question
      // and the answer changes what the answer is about.
      std::unique_ptr<ApprovedRig> applied;
      if (name == "sandbox.applyToLive") {
        const juce::String wanted =
            args["sandbox"].toString().isNotEmpty() ? args["sandbox"].toString() : sandboxId;
        if (wanted.isEmpty())
          return refuse(CommandError::invalidArgs,
                        "sandbox.applyToLive: no sandbox given and none selected (sandbox.create)");
        const auto rig = sandboxFor(session, wanted);
        if (rig == nullptr)
          return refuse(CommandError::notFound, "no sandbox rig with id '" + wanted + "'");
        applied = std::make_unique<ApprovedRig>();
        applied->sandboxId = rig->id;
        applied->rig = rig->capture();
      }
      CommandResult result = queueApproval(session, *command, args, sandboxId, std::move(applied));
      recordActivity(session, name, args, rigName, result.errorCode);
      return CommandWork::done(std::move(result));
    }
  }

  if (sandbox != nullptr && command->mutates && sandbox->rendering.load())
    return refuse(CommandError::unavailable,
                  "sandbox rig '" + sandbox->id + "' is rendering; try again when it is done");

  CommandWork work;
  {
    const juce::ScopedValueSetter<AgentSession*> scope(current, &session);
    work = sandbox != nullptr ? sandbox->commands().registry().begin(name, args)
                              : liveCommands.begin(name, args);
  }

  const auto codeOf = [](const CommandResult& result) {
    return result.ok ? juce::String("ok") : result.errorCode;
  };
  const juce::int64 sequence =
      recordActivity(session, name, args, rigName,
                     work.finished.has_value() ? codeOf(*work.finished) : juce::String("running"));
  {
    const std::lock_guard<std::mutex> lock(mutex);
    session.lastCommand = name;
    session.lastCommandTime = juce::Time::getCurrentTime();
    ++session.commandCount;
  }

  if (work.work != nullptr) {
    // The work may run on a transport thread. It keeps the sandbox it was
    // routed to from being discarded underneath it, and writes its outcome
    // into the activity entry made above.
    struct Use {
      Use(AgentHub& h, std::shared_ptr<SandboxRig> r) : hub(h), rig(std::move(r)) {
        if (rig != nullptr)
          rig->activeUses.fetch_add(1);
      }
      ~Use() {
        if (rig == nullptr)
          return;
        rig->activeUses.fetch_sub(1);
        // Its owner went while this was running: it goes now.
        if (rig->orphaned.load()) {
          const juce::String id = rig->id;
          rig.reset();
          hub.discardSandboxSoon(id);
        }
      }
      AgentHub& hub;
      std::shared_ptr<SandboxRig> rig;
    };
    auto use = std::make_shared<Use>(*this, sandbox);
    work.work = [this, use, sequence, codeOf, inner = std::move(work.work)] {
      CommandResult result = inner();
      finishActivity(sequence, codeOf(result));
      return result;
    };
  }
  return work;
}

CommandResult AgentHub::run(AgentSession& session, const juce::String& name,
                            const juce::var& args) {
  CommandWork work = begin(session, name, args);
  if (work.finished.has_value())
    return *work.finished;
  return work.work();
}

CommandResult AgentHub::runFromAnyThread(const std::shared_ptr<AgentSession>& session,
                                         const juce::String& name, const juce::var& args,
                                         int timeoutMs) {
  if (juce::MessageManager::existsAndIsCurrentThread())
    return run(*session, name, args);

  auto work = std::make_shared<CommandWork>();
  std::weak_ptr<bool> alive = lifetime;
  // Given up on when the session is cancelled, which covers the hub shutting
  // down (it cancels every session) and the session's own connection being
  // closed by a server that is stopping on the very thread waited for here.
  const bool ran = CommandRegistry::callOnMessageThread(
      [this, alive, work, session, name, args] {
        *work = alive.expired() ? CommandWork::done(fail(CommandError::unavailable,
                                                         "the application is shutting down"))
                                : begin(*session, name, args);
      },
      timeoutMs, &session->cancelled);
  if (!ran)
    return fail(CommandError::unavailable,
                closing.load()
                    ? juce::String("the application is shutting down")
                    : session->cancelled.load()
                          ? juce::String("the connection is closing")
                          : "timed out after " + juce::String(timeoutMs) +
                                " ms waiting for the application; '" + name + "' was not run");
  if (work->finished.has_value())
    return *work->finished;
  return work->work();
}

// ####################
// THE COMMAND SERVER
// ####################
juce::String AgentHub::setServerEnabled(bool enabled, bool persist, int port) {
  JUCE_ASSERT_MESSAGE_THREAD
  if (persist) {
    AgentSettings::setServerEnabled(enabled);
    if (port >= 0)
      AgentSettings::setServerPort(port);
  }
  if (!enabled) {
    if (commandServer != nullptr) {
      // Stopping joins the connection threads, and one of them may be
      // waiting for this very thread (a request to hand over, a render
      // asking for its rig to be prepared). The flag makes every such wait
      // give up, as it does when the hub itself goes.
      const bool wasClosing = closing.exchange(true);
      commandServer.reset();
      closing.store(wasClosing);
    }
    return {};
  }
  if (commandServer != nullptr && commandServer->isRunning())
    return {};
  commandServer = std::make_unique<CommandServer>(*this);
  const juce::String error = commandServer->start(port >= 0 ? port : AgentSettings::serverPort());
  if (error.isNotEmpty())
    commandServer.reset();
  return error;
}

void AgentHub::applyServerSetting() {
  if (!AgentSettings::serverEnabled())
    return;
  if (const juce::String error = setServerEnabled(true, /*persist=*/false); error.isNotEmpty())
    juce::Logger::writeToLog("[Agents] The command server did not start: " + error);
}

// ####################
// COMMANDS: AGENTS, APPROVALS, PERMISSIONS, SERVER, FOLDERS
// ####################
void AgentHub::registerAgentCommands() {
  const auto serverStatus = [this] {
    const bool running = commandServer != nullptr && commandServer->isRunning();
    return object({
        {"enabled", AgentSettings::serverEnabled()},
        {"running", running},
        {"port", running ? commandServer->port() : 0},
        {"connections", running ? commandServer->connectionCount() : 0},
        {"connectionFile", CommandServer::connectionFile().getFullPathName()},
    });
  };

  liveCommands.add({
      "agents.list",
      "Lists the clients connected to the command server: id, the name each declared, when it "
      "connected, its last command and when, how many commands it has run and the rig it is "
      "working on (live or a sandbox id); plus the server's status.",
      {},
      false,
      CommandPermission::local,
      [this, serverStatus](const juce::var&) {
        juce::Array<juce::var> clients;
        {
          const std::lock_guard<std::mutex> lock(mutex);
          for (const auto& session : sessions)
            clients.add(object({
                {"id", session->id},
                {"name", session->name},
                {"connected", isoTime(session->connected)},
                {"lastCommand", session->lastCommand},
                {"lastCommandTime", isoTime(session->lastCommandTime)},
                {"commandCount", session->commandCount},
                {"rig", session->sandboxId.isEmpty() ? juce::String("live") : session->sandboxId},
            }));
        }
        return ok({{"clients", clients}, {"server", serverStatus()}});
      },
  });

  liveCommands.add({
      "agents.disconnect",
      "Disconnects a client of the command server; its sandbox rigs are discarded and its "
      "requests waiting for approval are denied.",
      {P::required("id", T::integer, "The client's id, from agents.list.")},
      true,
      CommandPermission::local,
      [this](const juce::var& args) {
        const int id = args["id"];
        bool found = false;
        {
          const std::lock_guard<std::mutex> lock(mutex);
          for (const auto& session : sessions)
            if (session->id == id) {
              session->disconnectRequested.store(true);
              // Whatever it is in the middle of stops, so the connection can
              // close now and not when a long wait or render is over.
              session->cancelled.store(true);
              found = true;
            }
        }
        if (!found)
          return fail(CommandError::notFound, "no client with id " + juce::String(id));
        bool changed = false;
        for (auto& approval : approvals)
          if (approval->state == "pending" && approval->sessionId == id) {
            approval->state = "denied";
            approval->resolved = juce::Time::getCurrentTime();
            changed = true;
          }
        if (changed && onApprovalsChanged != nullptr)
          onApprovalsChanged();
        return ok({{"disconnected", id}});
      },
  });

  liveCommands.add({
      "agents.activity",
      "Returns the most recent commands that came through the agent layer, newest last (the "
      "last 200 are kept): time, client, command, its arguments shortened to one line, the rig "
      "it acted on and how it ended (ok, an error code, or running).",
      {P::optional("limit", T::integer, "How many entries to return.").range(1, 200).withDefault(50)},
      false,
      CommandPermission::local,
      [this](const juce::var& args) {
        const int limit = args["limit"];
        juce::Array<juce::var> entries;
        const std::lock_guard<std::mutex> lock(mutex);
        const int skip = std::max(0, static_cast<int>(activity.size()) - limit);
        for (size_t i = static_cast<size_t>(skip); i < activity.size(); ++i) {
          const Activity& entry = activity[i];
          entries.add(object({
              {"time", isoTime(entry.time)},
              {"clientId", entry.sessionId},
              {"client", entry.client},
              {"command", entry.command},
              {"args", entry.args},
              {"rig", entry.rig},
              {"result", entry.code},
          }));
        }
        return ok({{"activity", entries}});
      },
  });

  liveCommands.add({
      "approvals.list",
      "Lists the requests waiting for the user's approval (a client asked to change the rig "
      "being played while that is switched off), each with its id, client, command and "
      "arguments; with all true also the recently resolved ones. A request to apply a sandbox "
      "carries applies: the sandbox, the revision it was captured at, when, and sandboxChanged "
      "(the sandbox is no longer the rig the request would apply, so a preview rendered "
      "through it would show something else).",
      {P::optional("all", T::boolean, "Include approved, denied and expired requests.")
           .withDefault(false)},
      false,
      CommandPermission::local,
      [this](const juce::var& args) {
        expireApprovals();
        juce::Array<juce::var> list;
        for (const auto& approval : approvals)
          if (approval->state == "pending" || static_cast<bool>(args["all"]))
            list.add(approvalVar(*approval, false, /*forUser=*/true));
        return ok({{"approvals", list}, {"pending", pendingApprovalCount()}});
      },
  });

  liveCommands.add({
      "approvals.resolve",
      "Approves or denies a waiting request. Approving runs the command exactly as it was "
      "asked for and keeps its result for the client, which reads it with approvals.status. A "
      "request to apply a sandbox (sandbox.applyToLive) applies the rig the sandbox was when "
      "the request was made (applies: its sandbox, revision and capture time), whatever has "
      "been done to the sandbox since.",
      {P::required("id", T::string, "The request's id, from approvals.list."),
       P::required("approve", T::boolean, "True to run it, false to refuse it.")},
      true,
      CommandPermission::local,
      [this](const juce::var& args) {
        expireApprovals();
        Approval* approval = nullptr;
        for (auto& candidate : approvals)
          if (candidate->id == args["id"].toString())
            approval = candidate.get();
        if (approval == nullptr)
          return fail(CommandError::notFound,
                      "no approval request with id '" + args["id"].toString() + "'");
        if (approval->state != "pending")
          return fail(CommandError::failed, "that request is already " + approval->state);

        approval->resolved = juce::Time::getCurrentTime();
        if (!static_cast<bool>(args["approve"])) {
          approval->state = "denied";
        } else {
          // Run for the client that asked: its id and the rig it had
          // selected, with the user's say-so standing in for the gate.
          AgentSession proxy;
          proxy.id = approval->sessionId;
          proxy.name = approval->client;
          proxy.local = true;
          proxy.sandboxId = approval->sandboxId;
          approval->state = "approved";
          // Kept in a local first: the command may queue or expire requests
          // and move the list this pointer came from.
          const juce::String id = approval->id;
          // A sandbox is applied as the rig that was captured with the
          // request, not looked up again: the sandbox may have been edited
          // or discarded since the user was asked.
          const juce::var outcome = approval->applied != nullptr
                                        ? applyApprovedRig(*approval).toVar()
                                        : run(proxy, approval->command, approval->args).toVar();
          for (auto& candidate : approvals)
            if (candidate->id == id)
              candidate->result = outcome;
        }
        if (onApprovalsChanged != nullptr)
          onApprovalsChanged();
        for (const auto& candidate : approvals)
          if (candidate->id == args["id"].toString())
            return CommandResult::success(approvalVar(*candidate, true));
        return fail(CommandError::failed, "the request is gone");
      },
  });

  liveCommands.add({
      "approvals.status",
      "Returns one approval request: state (pending, approved, denied or expired) and, once "
      "approved, result, the envelope the command answered with. A client sees only its own "
      "requests.",
      {P::required("id", T::string, "The approvalId a needs_approval error carried.")},
      false,
      CommandPermission::read,
      [this](const juce::var& args) {
        expireApprovals();
        const AgentSession& session = currentSession();
        for (const auto& approval : approvals)
          if (approval->id == args["id"].toString() &&
              (session.local || approval->sessionId == session.id))
            return CommandResult::success(approvalVar(*approval, true));
        return fail(CommandError::notFound,
                    "no approval request with id '" + args["id"].toString() + "'");
      },
  });

  liveCommands.add({
      "permissions.get",
      "Returns what command server clients may do, one switch per permission class: read "
      "(library, presets and rig state), edit (create and edit presets and rigs), render "
      "(render clips to measure a tone), live (change the rig being played; off by default, "
      "and then such a request waits for approval), delete (delete presets, setlists and "
      "guitars; off by default) and settings (audio, device and machine-wide settings; off by "
      "default).",
      {},
      false,
      CommandPermission::read,
      [this](const juce::var&) { return CommandResult::success(policy().toVar()); },
  });

  liveCommands.add({
      "permissions.set",
      "Changes the permission switches for command server clients; switches left out stay as "
      "they are. Kept across restarts.",
      {P::optional("read", T::boolean, "Read library, presets and rig state."),
       P::optional("edit", T::boolean, "Create and edit presets and rigs."),
       P::optional("render", T::boolean, "Render audio clips to measure a tone."),
       P::optional("live", T::boolean, "Change the rig being played without asking."),
       P::optional("delete", T::boolean, "Delete presets and guitar profiles."),
       P::optional("settings", T::boolean, "Change audio, device and machine-wide settings.")},
      true,
      CommandPermission::local,
      [this](const juce::var& args) {
        AgentPolicy changed = policy();
        for (const auto& entry : kPolicySwitches)
          if (const juce::var value = args[juce::Identifier(entry.name)]; !value.isVoid())
            changed.*entry.field = static_cast<bool>(value);
        setPolicy(changed);
        return CommandResult::success(changed.toVar());
      },
  });

  liveCommands.add({
      "server.status",
      "Returns the command server's state: enabled (the saved setting), running, port, the "
      "number of connections and the path of the connection file clients read the port and "
      "token from.",
      {},
      false,
      CommandPermission::local,
      [serverStatus](const juce::var&) { return CommandResult::success(serverStatus()); },
  });

  liveCommands.add({
      "server.setEnabled",
      "Starts or stops the command server (off by default) and keeps the choice across "
      "restarts. It listens on 127.0.0.1 only and every client must present the token from "
      "the connection file; stopping it disconnects every client.",
      {P::required("enabled", T::boolean, "True to start listening, false to stop."),
       P::optional("port", T::integer, "The port to listen on; 0 picks a free one. Default: "
                                       "the port used last time, or a free one.")
           .range(0, 65535)},
      true,
      CommandPermission::local,
      [this, serverStatus](const juce::var& args) {
        const int port = args["port"].isVoid() ? -1 : static_cast<int>(args["port"]);
        const juce::String error = setServerEnabled(static_cast<bool>(args["enabled"]), true, port);
        if (error.isNotEmpty())
          return fail(CommandError::failed, error);
        return CommandResult::success(serverStatus());
      },
  });

  const auto rootsResult = [this] {
    juce::Array<juce::var> all, added;
    for (const auto& root : allowedRoots())
      all.add(root.getFullPathName());
    for (const auto& root : AgentSettings::userRoots())
      added.add(root);
    return ok({{"roots", all}, {"added", added}});
  };

  liveCommands.add({
      "roots.list",
      "Lists the folders a command server client may name in a path argument (roots): the "
      "library, the local model stash, the scratch folder and the ones the user added (added). "
      "A path anywhere else is refused.",
      {},
      false,
      CommandPermission::read,
      [rootsResult](const juce::var&) { return rootsResult(); },
  });

  liveCommands.add({
      "roots.add",
      "Adds a folder to the ones clients may read files from (captures, IRs, reference "
      "recordings). Kept across restarts.",
      {P::required("path", T::string, "An existing folder, as an absolute path.").path()},
      true,
      CommandPermission::local,
      [rootsResult](const juce::var& args) {
        const juce::String path = args["path"].toString();
        if (!juce::File::isAbsolutePath(path) || !juce::File(path).isDirectory())
          return fail(CommandError::notFound, "not a folder: " + path);
        juce::StringArray roots = AgentSettings::userRoots();
        roots.addIfNotAlreadyThere(juce::File(path).getFullPathName());
        AgentSettings::setUserRoots(roots);
        return rootsResult();
      },
  });

  liveCommands.add({
      "roots.remove",
      "Removes a folder added with roots.add.",
      {P::required("path", T::string, "The folder, as roots.list shows it.").path()},
      true,
      CommandPermission::local,
      [rootsResult](const juce::var& args) {
        juce::StringArray roots = AgentSettings::userRoots();
        const int before = roots.size();
        roots.removeString(args["path"].toString());
        if (juce::File::isAbsolutePath(args["path"].toString()))
          roots.removeString(juce::File(args["path"].toString()).getFullPathName());
        if (roots.size() == before)
          return fail(CommandError::notFound,
                      "not an added folder: " + args["path"].toString());
        AgentSettings::setUserRoots(roots);
        return rootsResult();
      },
  });

  liveCommands.add({
      "events.subscribe",
      "Turns change notifications on or off for the calling connection. With revision on, the "
      "server sends {\"event\":\"revision\",\"rig\":...,\"revision\":N} whenever the revision of "
      "the rig the connection is working on (the live rig or its selected sandbox) changes, so "
      "a client can refetch state.get without polling.",
      {P::optional("revision", T::boolean, "Send revision events.").withDefault(true)},
      false,
      CommandPermission::read,
      [this](const juce::var& args) {
        AgentSession& session = currentSession();
        session.wantsRevisionEvents.store(static_cast<bool>(args["revision"]));
        const RigRevision now = revisionFor(session);
        return ok({
            {"revision", static_cast<bool>(args["revision"])},
            {"rig", now.rig},
            {"currentRevision", static_cast<int>(now.revision)},
        });
      },
  });
}

// ####################
// COMMANDS: SANDBOX RIGS
// ####################
void AgentHub::registerSandboxCommands() {
  const auto describe = [this](const SandboxRig& rig, const AgentSession& session) {
    const juce::String selected = [&] {
      const std::lock_guard<std::mutex> lock(mutex);
      return session.sandboxId;
    }();
    return object({
        {"sandbox", rig.id},
        {"name", rig.name},
        {"owner", rig.ownerSession},
        {"created", isoTime(rig.created)},
        {"rendering", rig.rendering.load()},
        {"selected", selected == rig.id},
        {"revision", static_cast<int>(
                         const_cast<SandboxRig&>(rig).processor().getCurrentChainRevision())},
    });
  };

  // The sandbox a command names, or the session's selected one.
  const auto resolve = [this](const juce::var& args, const char* command,
                              std::shared_ptr<SandboxRig>& rig) -> std::optional<CommandResult> {
    juce::String id = args["sandbox"].toString();
    if (id.isEmpty()) {
      const std::lock_guard<std::mutex> lock(mutex);
      id = currentSession().sandboxId;
    }
    if (id.isEmpty())
      return fail(CommandError::invalidArgs,
                  juce::String(command) + ": no sandbox given and none selected (sandbox.create)");
    // Its own only: another session's sandbox is, to this one, not there.
    rig = sandboxFor(currentSession(), id);
    if (rig == nullptr)
      return fail(CommandError::notFound, "no sandbox rig with id '" + id + "'");
    return std::nullopt;
  };

  liveCommands.add({
      "sandbox.create",
      "Creates a sandbox rig: a second rig that is never heard, for building and measuring a "
      "tone without disturbing the one being played. It starts as a copy of the live rig "
      "(from live), empty, or from a preset, and is selected for the calling connection unless "
      "use is false: from then on every rig command of that connection (chain.*, block.*, "
      "scene.*, param.*, preset.*, state.* and the rest) acts on the sandbox instead of the "
      "live rig. A copy loads its blocks in the background like a project being opened, so "
      "call state.waitUntilLoaded before editing them. A sandbox belongs to the connection "
      "that made it: no other client can select, edit, render, apply or discard it. At most "
      "four exist at a time; a connection's sandboxes are discarded when it closes.",
      {P::optional("from", T::string, "What the sandbox starts as.")
           .oneOf({"live", "empty", "preset"})
           .withDefault("live"),
       P::optional("presetId", T::string, "With from preset: the preset's id, from preset.list."),
       P::optional("name", T::string, "A label for sandbox.list."),
       P::optional("use", T::boolean, "Select it for this connection.").withDefault(true)},
      true,
      CommandPermission::edit,
      [this, describe](const juce::var& args) {
        const juce::String from = args["from"].toString();
        if (from == "preset" && args["presetId"].toString().isEmpty())
          return fail(CommandError::invalidArgs,
                      "sandbox.create: 'presetId' is required with from preset");
        AgentSession& session = currentSession();
        juce::String error;
        const auto rig = createSandbox(from, args["presetId"].toString(), session.id, error);
        if (rig == nullptr)
          return fail(from == "preset" ? CommandError::notFound : CommandError::failed, error);
        rig->name = args["name"].toString().substring(0, 80);
        if (static_cast<bool>(args["use"])) {
          const std::lock_guard<std::mutex> lock(mutex);
          session.sandboxId = rig->id;
        }
        return CommandResult::success(describe(*rig, session));
      },
  });

  liveCommands.add({
      "sandbox.use",
      "Selects the rig the calling connection's rig commands act on: one of its own sandboxes "
      "by id, or live for the rig being played.",
      {P::required("sandbox", T::string, "A sandbox id from sandbox.list, or live.")},
      true,
      CommandPermission::edit,
      [this](const juce::var& args) {
        const juce::String id = args["sandbox"].toString();
        AgentSession& session = currentSession();
        if (id != "live" && sandboxFor(session, id) == nullptr)
          return fail(CommandError::notFound, "no sandbox rig with id '" + id + "'");
        {
          const std::lock_guard<std::mutex> lock(mutex);
          session.sandboxId = id == "live" ? juce::String() : id;
        }
        return ok({{"rig", id}});
      },
  });

  liveCommands.add({
      "sandbox.list",
      "Lists the calling connection's own sandbox rigs (a local caller sees every client's): "
      "id, name, the client that made it, whether a render is running through it and whether "
      "it is the calling connection's selected rig; selected names that rig (live when none).",
      {},
      false,
      CommandPermission::read,
      [this, describe](const juce::var&) {
        const AgentSession& session = currentSession();
        std::vector<std::shared_ptr<SandboxRig>> rigs;
        juce::String selected;
        {
          const std::lock_guard<std::mutex> lock(mutex);
          for (const auto& [id, rig] : sandboxes)
            if (mayAddress(session, *rig))
              rigs.push_back(rig);
          selected = session.sandboxId;
        }
        juce::Array<juce::var> list;
        for (const auto& rig : rigs)
          list.add(describe(*rig, session));
        return ok({
            {"sandboxes", list},
            {"selected", selected.isEmpty() ? juce::String("live") : selected},
        });
      },
  });

  liveCommands.add({
      "sandbox.discard",
      "Discards one of the calling connection's sandbox rigs and frees what it held; the "
      "connection goes back to the live rig if it had it selected. Default: the calling "
      "connection's selected sandbox.",
      {P::optional("sandbox", T::string, "The sandbox id.")},
      true,
      CommandPermission::edit,
      [this, resolve](const juce::var& args) {
        std::shared_ptr<SandboxRig> rig;
        if (auto error = resolve(args, "sandbox.discard", rig))
          return *error;
        const juce::String id = rig->id;
        rig.reset();
        if (!discardSandbox(id))
          return fail(CommandError::unavailable,
                      "sandbox rig '" + id + "' is in use by a render; try again when it is done");
        return ok({{"discarded", id}});
      },
  });

  liveCommands.add({
      "sandbox.applyToLive",
      "Copies a sandbox rig onto the rig being played, the way loading a preset would: the "
      "chain with its scenes and the preset-managed parameters, as one undo step (chain.undo "
      "on the live rig brings the previous chain back). The sandbox is kept. This is the step "
      "that changes what the user hears: with the live permission off it answers "
      "needs_approval and runs when the user approves; what is applied then is the sandbox's "
      "rig as it was when this was called, so edit first and ask after (asking again after an "
      "edit replaces the earlier request). Default: the calling connection's selected "
      "sandbox.",
      {P::optional("sandbox", T::string, "The sandbox id.")},
      true,
      CommandPermission::live,
      [this, resolve](const juce::var& args) {
        std::shared_ptr<SandboxRig> rig;
        if (auto error = resolve(args, "sandbox.applyToLive", rig))
          return *error;
        if (!rig->applyTo(live))
          return fail(CommandError::failed, "the live rig refused the sandbox's rig");
        return ok({
            {"applied", rig->id},
            {"revision", static_cast<int>(live.getCurrentChainRevision())},
        });
      },
  });
}

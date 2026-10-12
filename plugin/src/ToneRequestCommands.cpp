// toneRequest.*: "Describe a tone" (docs/agents.md, "Tone requests"). The
// user asks in the application for a tone; the agent connected over MCP
// picks the request up, builds the tone in its own sandbox rig, reports its
// steps, proposes the result and answers the user's refine messages; the
// user applies it to the rig being played, or stops it.
//
// The user's side is local (the UI); the agent's side needs `edit`, like
// the sandbox it builds in. One request is open at a time.
//
// Everything that changes the request runs on the message thread, inside a
// handler; toneMutex guards it for toneRequest.wait, which waits on a
// connection's thread by polling toneSequence.
#include "AgentHub.h"

// Processor.h first: it pulls in the JUCE module headers, which must come
// before anything that drags in <windows.h>.
#include "Processor.h"

#include "SandboxRig.h"
#include "ToneRequestState.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

namespace {

using P = CommandParam;
using T = CommandParamType;
using Properties = std::initializer_list<std::pair<const char*, juce::var>>;

// What one request may hold, so a client cannot make the application keep
// text without end.
constexpr int kMaxPromptChars = 4000;
constexpr int kMaxTextChars = 2000;
constexpr int kMaxSteps = 200;
constexpr int kMaxMessages = 100;
constexpr int kMaxChanges = 64;
// The longest toneRequest.wait; below the MCP server's request timeout.
constexpr int kMaxWaitMs = 25000;

juce::var object(Properties properties) {
  juce::DynamicObject::Ptr obj = new juce::DynamicObject();
  for (const auto& [key, value] : properties)
    obj->setProperty(key, value);
  return juce::var(obj.get());
}

CommandResult ok(Properties properties) { return CommandResult::success(object(properties)); }

CommandResult fail(const char* code, const juce::String& message) {
  return CommandResult::failure(code, message);
}

bool isOpen(const juce::String& status) {
  return status == "waiting" || status == "working" || status == "ready";
}

}  // namespace

void AgentHub::toneRequestChanged() {
  toneSequence.fetch_add(1);
  if (onToneRequestChanged)
    onToneRequestChanged();
}

void AgentHub::releaseToneRequest(int sessionId) {
  {
    const std::lock_guard<std::mutex> lock(toneMutex);
    if (toneRequest == nullptr || toneRequest->claimedBy != sessionId ||
        !isOpen(toneRequest->status))
      return;
    toneRequest->claimedBy = 0;
    toneRequest->agentName.clear();
    // The sandbox went with the session, and the proposal with it.
    toneRequest->sandbox.clear();
    toneRequest->status = "waiting";
    toneRequest->steps.push_back({juce::Time::getCurrentTime(), "The agent disconnected."});
  }
  toneRequestChanged();
}

void AgentHub::registerToneRequestCommands() {
  // The request as both sides read it. Called with toneMutex held.
  const auto describe = [](const ToneRequest& request) {
    juce::Array<juce::var> steps;
    for (const auto& step : request.steps)
      steps.add(object({{"time", step.time.toISO8601(true)}, {"text", step.text}}));
    juce::Array<juce::var> messages;
    for (const auto& message : request.messages)
      messages.add(object({{"time", message.time.toISO8601(true)},
                           {"from", message.from},
                           {"text", message.text}}));
    return object({
        {"id", request.id},
        {"status", request.status},
        {"prompt", request.prompt},
        {"guitar", request.guitar},
        {"reference", request.reference},
        {"target", request.target},
        {"limits", request.limits},
        {"created", request.created.toISO8601(true)},
        {"agent", request.agentName},
        {"step", request.step},
        {"stepsTotal", request.stepsTotal},
        {"steps", steps},
        {"messages", messages},
        {"unansweredMessages", static_cast<int>(request.messages.size()) - request.messagesSeen},
        {"sandbox", request.sandbox},
        {"title", request.title},
        {"summary", request.summary},
        {"changes", request.changes},
        {"measured", request.measured},
    });
  };

  // The open request as `session` may act on it as the agent: claimed by
  // it. Null with the error set otherwise. Called with toneMutex held.
  const auto agentsRequest = [this](const juce::var& args, CommandResult& error) -> ToneRequest* {
    if (toneRequest == nullptr || toneRequest->id != args["id"].toString()) {
      error = fail(CommandError::notFound, "no tone request with id '" + args["id"].toString() + "'");
      return nullptr;
    }
    if (!isOpen(toneRequest->status)) {
      error = fail(CommandError::failed, "the tone request is " + toneRequest->status);
      return nullptr;
    }
    if (toneRequest->claimedBy != currentSession().id) {
      error = fail(CommandError::failed, "the tone request is not this connection's: claim it first");
      return nullptr;
    }
    return toneRequest.get();
  };

  // ---- The user's side (local) ----

  liveCommands.add({
      "toneRequest.create",
      "Opens a tone request for the connected agent: what the user wants to hear, the guitar "
      "profile to build it for, a reference recording to aim for, limits and where it goes. "
      "Replaces a request that is finished or stopped; refused while one is open.",
      {P::required("prompt", T::string, "What the user wants to hear."),
       P::optional("guitar", T::string, "The guitar profile's id."),
       P::optional("reference", T::string, "A recording to aim for, an absolute path.").path(),
       P::optional("limits", T::object, "Limits the agent keeps to (blocks, CPU, kinds)."),
       P::optional("target", T::string, "Where the tone goes when applied.")
           .oneOf({"newPreset", "currentPreset", "emptyRows"})
           .withDefault("newPreset")},
      true,
      CommandPermission::local,
      [this, describe](const juce::var& args) {
        const juce::String prompt = args["prompt"].toString().trim();
        if (prompt.isEmpty())
          return fail(CommandError::invalidArgs, "the prompt is empty");
        if (prompt.length() > kMaxPromptChars)
          return fail(CommandError::invalidArgs,
                      "the prompt is longer than " + juce::String(kMaxPromptChars) + " characters");
        juce::var created;
        {
          const std::lock_guard<std::mutex> lock(toneMutex);
          if (toneRequest != nullptr && isOpen(toneRequest->status))
            return fail(CommandError::failed,
                        "a tone request is open: stop it (toneRequest.stop) or apply it first");
          auto request = std::make_unique<ToneRequest>();
          request->id = "tone-" + juce::String(nextToneRequest++);
          request->prompt = prompt;
          request->guitar = args["guitar"].toString();
          request->reference = args["reference"].toString();
          request->target = args["target"].toString();
          request->limits = args["limits"].isObject() ? args["limits"] : juce::var(new juce::DynamicObject());
          request->created = juce::Time::getCurrentTime();
          toneRequest = std::move(request);
          created = describe(*toneRequest);
        }
        toneRequestChanged();
        return CommandResult::success(created);
      },
  });

  liveCommands.add({
      "toneRequest.reply",
      "The user's refine message on the open request (\"a bit darker, less delay in the "
      "verse\"). A request that was ready goes back to working.",
      {P::required("text", T::string, "The message.")},
      true,
      CommandPermission::local,
      [this, describe](const juce::var& args) {
        const juce::String text = args["text"].toString().trim();
        if (text.isEmpty() || text.length() > kMaxTextChars)
          return fail(CommandError::invalidArgs,
                      "a message is 1 to " + juce::String(kMaxTextChars) + " characters");
        juce::var result;
        {
          const std::lock_guard<std::mutex> lock(toneMutex);
          if (toneRequest == nullptr || !isOpen(toneRequest->status))
            return fail(CommandError::notFound, "no tone request is open");
          if (static_cast<int>(toneRequest->messages.size()) >= kMaxMessages)
            return fail(CommandError::failed, "the conversation is full: start a new request");
          toneRequest->messages.push_back({juce::Time::getCurrentTime(), "user", text});
          if (toneRequest->status == "ready")
            toneRequest->status = "working";
          result = describe(*toneRequest);
        }
        toneRequestChanged();
        return CommandResult::success(result);
      },
  });

  liveCommands.add({
      "toneRequest.stop",
      "Stops the open request. What the agent built so far stays in its sandbox; the agent's "
      "next step on the request is refused.",
      {},
      true,
      CommandPermission::local,
      [this, describe](const juce::var&) {
        juce::var result;
        {
          const std::lock_guard<std::mutex> lock(toneMutex);
          if (toneRequest == nullptr || !isOpen(toneRequest->status))
            return fail(CommandError::notFound, "no tone request is open");
          toneRequest->status = "stopped";
          result = describe(*toneRequest);
        }
        toneRequestChanged();
        return CommandResult::success(result);
      },
  });

  liveCommands.add({
      "toneRequest.apply",
      "Applies the proposed tone to the rig being played (sandbox.applyToLive on the agent's "
      "sandbox, as the user: one undo step) and closes the request. This is the user's own "
      "approval, so no prompt follows.",
      {},
      true,
      CommandPermission::local,
      [this, describe](const juce::var&) {
        juce::String sandboxId;
        {
          const std::lock_guard<std::mutex> lock(toneMutex);
          if (toneRequest == nullptr || toneRequest->status != "ready")
            return fail(CommandError::failed, "no proposed tone to apply");
          sandboxId = toneRequest->sandbox;
        }
        const auto rig = findSandbox(sandboxId);
        if (rig == nullptr)
          return fail(CommandError::notFound, "the proposed tone's sandbox is gone");
        if (!rig->applyTo(live))
          return fail(CommandError::failed, "the live rig refused the sandbox's rig");
        juce::var result;
        {
          const std::lock_guard<std::mutex> lock(toneMutex);
          if (toneRequest != nullptr && toneRequest->sandbox == sandboxId)
            toneRequest->status = "applied";
          if (toneRequest != nullptr)
            result = describe(*toneRequest);
        }
        toneRequestChanged();
        return CommandResult::success(result);
      },
  });

  // ---- Both sides ----

  liveCommands.add({
      "toneRequest.get",
      "The tone request, open or last finished: status (waiting for an agent, working, ready "
      "with a proposal, applied, stopped), what was asked, the agent's steps, the refine "
      "conversation, and the proposal (sandbox, title, summary, changes, measured). Null "
      "when there has been none.",
      {},
      false,
      CommandPermission::read,
      [this, describe](const juce::var&) {
        const std::lock_guard<std::mutex> lock(toneMutex);
        return CommandResult::success(
            object({{"request", toneRequest != nullptr ? describe(*toneRequest) : juce::var()},
                    {"sequence", static_cast<juce::int64>(toneSequence.load())}}));
      },
  });

  // ---- The agent's side ----

  liveCommands.add({
      "toneRequest.wait",
      "Waits until the tone request changes after `since` (the sequence a previous call "
      "returned; 0 for the first call): a new request, a refine message from the user, a "
      "stop. Returns the request and the new sequence, or the same sequence when nothing "
      "changed within the wait. An agent that serves the user's tone requests calls this in "
      "a loop.",
      {P::optional("since", T::integer, "The sequence last seen.").withDefault(0),
       P::optional("timeoutMs", T::integer, "How long to wait.")
           .range(0, kMaxWaitMs)
           .withDefault(20000)},
      false,
      CommandPermission::read,
      [this, describe](const juce::var& args) {
        const juce::int64 since = static_cast<juce::int64>(args["since"]);
        const int timeoutMs = args["timeoutMs"];
        const std::atomic<bool>* cancel = &slowWorkCancel();
        return CommandWork::later([this, describe, since, timeoutMs, cancel] {
          const auto deadline =
              std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
          while (toneSequence.load() <= since && std::chrono::steady_clock::now() < deadline) {
            if (shuttingDown().load() || cancel->load())
              return fail(CommandError::unavailable, "the connection is closing");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
          }
          const std::lock_guard<std::mutex> lock(toneMutex);
          return CommandResult::success(
              object({{"request", toneRequest != nullptr ? describe(*toneRequest) : juce::var()},
                      {"sequence", static_cast<juce::int64>(toneSequence.load())}}));
        });
      },
  });

  liveCommands.add({
      "toneRequest.claim",
      "Takes on the open tone request: this connection now works on it, and the user sees "
      "its name. Refused when another connection has it. Build the tone in a sandbox of your "
      "own (sandbox.create), report steps with toneRequest.progress and finish with "
      "toneRequest.propose.",
      {P::required("id", T::string, "The request's id.")},
      true,
      CommandPermission::edit,
      [this, describe](const juce::var& args) {
        juce::var result;
        {
          const std::lock_guard<std::mutex> lock(toneMutex);
          if (toneRequest == nullptr || toneRequest->id != args["id"].toString())
            return fail(CommandError::notFound,
                        "no tone request with id '" + args["id"].toString() + "'");
          if (!isOpen(toneRequest->status))
            return fail(CommandError::failed, "the tone request is " + toneRequest->status);
          AgentSession& session = currentSession();
          if (toneRequest->claimedBy != 0 && toneRequest->claimedBy != session.id)
            return fail(CommandError::failed, "another connection is working on this request");
          toneRequest->claimedBy = session.id;
          toneRequest->agentName = session.name;
          if (toneRequest->status == "waiting")
            toneRequest->status = "working";
          result = describe(*toneRequest);
        }
        toneRequestChanged();
        return CommandResult::success(result);
      },
  });

  liveCommands.add({
      "toneRequest.progress",
      "Reports a step on the claimed request (\"Adding a plate reverb\"), for the user to "
      "follow; step and of give the progress (step 5 of 8). Marks the user's messages so far "
      "as seen.",
      {P::required("id", T::string, "The request's id."),
       P::required("text", T::string, "What the agent is doing."),
       P::optional("step", T::integer, "This step's number.").range(0, 1000),
       P::optional("of", T::integer, "How many steps there will be.").range(0, 1000)},
      true,
      CommandPermission::edit,
      [this, agentsRequest](const juce::var& args) {
        const juce::String text = args["text"].toString().trim();
        if (text.isEmpty() || text.length() > kMaxTextChars)
          return fail(CommandError::invalidArgs,
                      "a step is 1 to " + juce::String(kMaxTextChars) + " characters");
        {
          const std::lock_guard<std::mutex> lock(toneMutex);
          CommandResult error;
          ToneRequest* request = agentsRequest(args, error);
          if (request == nullptr)
            return error;
          if (static_cast<int>(request->steps.size()) >= kMaxSteps)
            request->steps.erase(request->steps.begin());
          request->steps.push_back({juce::Time::getCurrentTime(), text});
          if (args.hasProperty("step"))
            request->step = args["step"];
          if (args.hasProperty("of"))
            request->stepsTotal = args["of"];
          request->messagesSeen = static_cast<int>(request->messages.size());
          request->status = "working";
        }
        toneRequestChanged();
        return ok({{"ok", true}});
      },
  });

  liveCommands.add({
      "toneRequest.propose",
      "Hands the user the tone: the sandbox it was built in (this connection's own), a title, "
      "a summary in a sentence or two (it is also the agent's answer in the conversation), "
      "what changed as [{name, before, after}] and what was measured. The request is then "
      "ready: the user auditions it, applies it or asks for changes (toneRequest.wait sees the "
      "message).",
      {P::required("id", T::string, "The request's id."),
       P::required("sandbox", T::string, "The sandbox holding the tone."),
       P::required("summary", T::string, "What was built, for the user."),
       P::optional("title", T::string, "A name for the tone."),
       P::optional("changes", T::array, "What changed: [{name, before, after}].").items(T::object),
       P::optional("measured", T::object, "What was measured (render.clip readings).")},
      true,
      CommandPermission::edit,
      [this, describe, agentsRequest](const juce::var& args) {
        const juce::String summary = args["summary"].toString().trim();
        const juce::String title = args["title"].toString().trim();
        if (summary.isEmpty() || summary.length() > kMaxTextChars || title.length() > 100)
          return fail(CommandError::invalidArgs,
                      "a summary is 1 to " + juce::String(kMaxTextChars) +
                          " characters and a title at most 100");
        if (args["changes"].size() > kMaxChanges)
          return fail(CommandError::invalidArgs,
                      "at most " + juce::String(kMaxChanges) + " changes");
        if (args["measured"].isObject() &&
            juce::JSON::toString(args["measured"], true).length() > 16 * kMaxTextChars)
          return fail(CommandError::invalidArgs, "the measurements are too long");
        // The sandbox must be one this connection may address, and never the
        // rig being played.
        if (sandboxFor(currentSession(), args["sandbox"].toString()) == nullptr)
          return fail(CommandError::notFound,
                      "no sandbox rig with id '" + args["sandbox"].toString() + "'");
        juce::var result;
        {
          const std::lock_guard<std::mutex> lock(toneMutex);
          CommandResult error;
          ToneRequest* request = agentsRequest(args, error);
          if (request == nullptr)
            return error;
          if (static_cast<int>(request->messages.size()) >= kMaxMessages)
            return fail(CommandError::failed, "the conversation is full");
          request->sandbox = args["sandbox"].toString();
          request->title = title;
          request->summary = summary;
          request->changes = args["changes"].isArray() ? args["changes"] : juce::var(juce::Array<juce::var>());
          request->measured = args["measured"];
          request->messages.push_back({juce::Time::getCurrentTime(), "agent", summary});
          request->messagesSeen = static_cast<int>(request->messages.size());
          request->status = "ready";
          result = describe(*request);
        }
        toneRequestChanged();
        return CommandResult::success(result);
      },
  });
}

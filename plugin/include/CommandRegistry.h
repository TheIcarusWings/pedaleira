#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

// The command layer's transport-neutral half: a table of named commands with
// JSON-shaped arguments, validated against a declared spec before any handler
// runs, and described back as JSON so a caller (an MCP server, a WebSocket
// remote, the headless render tool, a test) can discover what exists without
// reading C++. It knows nothing about the processor; EngineCommands fills one
// of these with the engine's commands.

// The fixed set of error codes a command can fail with. Transports map these
// onto their own error shapes, so the set stays small and stable.
namespace CommandError {
// No command of that name is registered.
constexpr const char* unknownCommand = "unknown_command";
// The arguments do not match the command's parameter spec, or a value that
// names a fixed choice (a parameter id, a file extension) is not one of them.
constexpr const char* invalidArgs = "invalid_args";
// An object the arguments refer to by id does not exist (block, preset, file).
constexpr const char* notFound = "not_found";
// The arguments were valid but the engine refused or could not do it.
constexpr const char* failed = "failed";
// The command could not be run or finished in time (message thread busy,
// loads still pending). Trying again later can succeed.
constexpr const char* unavailable = "unavailable";
// The caller is not allowed to run this command: its permission class is
// switched off, the command is local only, or a path lies outside the
// allowed folders. Only callers that come through the agent layer (a command
// server client) can get this; local callers are not gated.
constexpr const char* permissionDenied = "permission_denied";
// The command changes the rig being played and that is switched off; the
// request now waits for the user. The error carries an approvalId to poll
// with approvals.status.
constexpr const char* needsApproval = "needs_approval";
}  // namespace CommandError

// What a command is allowed to touch, for callers that are not the user
// (agents over the command server). Every command declares one; the agent
// layer's policy has one switch per class (see AgentHub.h).
enum class CommandPermission {
  // Not declared. A registration that leaves it out lands here, and a test
  // fails on any command still in this class.
  unclassified,
  // Reads the rig, the library, presets or settings.
  read,
  // Creates or edits rigs and presets. An edit of the rig being played also
  // needs `live`; edits of a sandbox rig do not.
  edit,
  // Renders audio offline to measure a tone.
  render,
  // Changes the rig being played, or records its input.
  live,
  // Deletes presets or library entries.
  destroy,
  // Changes machine-wide, audio or device settings.
  settings,
  // Never available to command server clients: only in-process callers.
  local,
};

const char* commandPermissionName(CommandPermission permission);

struct CommandResult {
  bool ok = true;
  juce::var value;
  juce::String errorCode;
  juce::String errorMessage;
  // Extra properties of the error object (needs_approval's approvalId).
  juce::var errorDetails;

  static CommandResult success(juce::var result = {});
  static CommandResult failure(const char* code, const juce::String& message);

  // {"ok":true,"result":...} or {"ok":false,"error":{"code":"...","message":"..."}}
  juce::var toVar() const;
};

enum class CommandParamType { string, number, integer, boolean, object, array };

// One argument of a command. Built with the two named constructors and the
// chained refinements, so a registration reads like the reference it produces:
//
//   CommandParam::required("value", CommandParamType::number, "Mix, 0 to 1").range(0.0, 1.0)
struct CommandParam {
  juce::String name;
  CommandParamType type = CommandParamType::string;
  bool isRequired = false;
  juce::String description;
  // Allowed values of a string argument; empty = any string.
  juce::StringArray enumValues;
  // Inclusive bounds of a number or integer argument.
  std::optional<double> min, max;
  // Element type of an array argument; unset = any elements.
  std::optional<CommandParamType> itemType;
  // Filled in for an absent optional argument before the handler runs; void =
  // the handler sees the argument as absent.
  juce::var defaultValue;
  // The argument names a file or folder (a string, or an array of them).
  // The agent layer checks such arguments against the allowed folders before
  // the command runs for a command server client.
  bool isPath = false;
  // With isPath: the argument may also be a name or identifier that is not a
  // path (a plugin identifier). Only values that are absolute paths are then
  // checked.
  bool pathMayBeName = false;
  // With isPath: the command also reads or writes a folder of this name
  // beside the file (the Assets folder of a preset export). The agent layer
  // holds that folder to the allowed folders as well, links resolved.
  juce::String pathSibling;

  static CommandParam required(const juce::String& name, CommandParamType type,
                               const juce::String& description);
  static CommandParam optional(const juce::String& name, CommandParamType type,
                               const juce::String& description);
  CommandParam oneOf(const juce::StringArray& values) const;
  CommandParam range(double minimum, double maximum) const;
  CommandParam atLeast(double minimum) const;
  CommandParam items(CommandParamType elementType) const;
  CommandParam withDefault(const juce::var& value) const;
  CommandParam path(bool mayBeName = false) const;
  CommandParam withSibling(const juce::String& folderName) const;
};

// What a slow command hands back from the message thread: either its result,
// or the rest of its work as a function that may run on another thread (the
// thread that asked, when the request came through runFromAnyThread, so a
// long render holds up neither the message thread nor other callers).
struct CommandWork {
  // Set: the command is done and `work` is empty.
  std::optional<CommandResult> finished;
  // The slow part. Runs once, on the calling thread; it must reach the
  // message thread itself for anything that needs it (see
  // CommandRegistry::callOnMessageThread).
  std::function<CommandResult()> work;

  static CommandWork done(CommandResult result) {
    CommandWork w;
    w.finished = std::move(result);
    return w;
  }
  static CommandWork later(std::function<CommandResult()> rest) {
    CommandWork w;
    w.work = std::move(rest);
    return w;
  }
};

// A command's handler: either kind of function, told apart by what it
// returns, so a registration is written the same way for both.
//
//   quick: CommandResult(const juce::var& args), the whole command, on the
//          message thread.
//   slow:  CommandWork(const juce::var& args), for commands that take long (a
//          render, waiting for loads): the quick message-thread part, which
//          returns the rest as CommandWork.
//
// Either receives the validated arguments: always an object, every required
// argument present with the declared type and within its bounds, integers as
// int, defaults filled in, no unknown names.
struct CommandHandler {
  CommandHandler() = default;

  template <typename Fn>
    requires std::is_invocable_v<Fn&, const juce::var&>
  CommandHandler(Fn fn) {  // implicit on purpose: registrations pass a lambda
    if constexpr (std::is_same_v<std::invoke_result_t<Fn&, const juce::var&>, CommandWork>)
      slow = std::move(fn);
    else
      quick = std::move(fn);
  }

  bool isSet() const { return quick != nullptr || slow != nullptr; }
  bool isSlow() const { return slow != nullptr; }

  std::function<CommandResult(const juce::var& args)> quick;
  std::function<CommandWork(const juce::var& args)> slow;
};

struct Command {
  // Dotted, lower camel case after the dot: "block.setParam".
  juce::String name;
  // One sentence, shown in describe() and the generated reference.
  juce::String summary;
  std::vector<CommandParam> params;
  // False for commands that only read. Transports use it to tell queries
  // from edits (confirmation prompts, read-only clients).
  bool mutates = false;
  // What the command touches (see CommandPermission). Placed before the
  // handler so a registration cannot leave it out by accident.
  CommandPermission permission = CommandPermission::unclassified;
  // A function of the validated arguments (see CommandHandler).
  CommandHandler handler;
};

class CommandRegistry {
public:
  CommandRegistry();
  ~CommandRegistry();

  // Registers a command. Names are unique; registering one twice is a
  // programming error (asserts, and the later one wins).
  void add(Command command);
  bool contains(const juce::String& name) const;

  // Validates `args` against the command's spec, then runs its handler.
  // Unknown command, missing required argument, wrong type, value outside an
  // enum or bounds, and unknown argument names never reach a handler: they
  // come back as unknown_command / invalid_args with a message naming the
  // offending argument. `args` may be void (no arguments) or an object.
  //
  // Message thread only: handlers call into the processor's mutators, which
  // are message-thread-only by contract.
  CommandResult run(const juce::String& name, const juce::var& args = {});

  // The first half of run(): validates and runs the handler, or for a slow
  // command its message-thread part, handing back what is left to do. Message
  // thread only. run() is begin() followed by the work, inline.
  CommandWork begin(const juce::String& name, const juce::var& args = {});

  // The validation alone: the failure run() would answer with before any
  // handler ran (unknown command, bad arguments), or nothing when the
  // arguments are acceptable. For callers that have to decide something
  // about a request before running it.
  std::optional<CommandResult> check(const juce::String& name, const juce::var& args) const;

  // The registered command, or null.
  const Command* find(const juce::String& name) const;
  // Every registered name, sorted.
  juce::StringArray names() const;

  // For callers on other threads (a socket or stdio transport): posts the
  // command to the message thread and waits for its result. `timeoutMs`
  // bounds how long the command may wait to *start*; a command that never
  // got picked up returns `unavailable` and is guaranteed not to run later.
  // Once a command has started it is always waited to completion, so a
  // result is never reported for an edit that is still happening.
  //
  // Never call this from the audio thread (it blocks, allocates and locks).
  // On the message thread it simply runs the command. The registry must
  // outlive the call and be destroyed on the message thread.
  //
  // A slow command's work runs on the calling thread, after its
  // message-thread part has returned.
  CommandResult runFromAnyThread(const juce::String& name, const juce::var& args, int timeoutMs);

  // Runs `call` on the message thread and waits for it: inline when already
  // there, else posted. False when it did not start within `timeoutMs` (it is
  // then guaranteed never to run) or no message thread exists; once started
  // it is always waited for. What runFromAnyThread is built on, and what a
  // slow command's work uses to get back to the message thread. A caller
  // that must be able to give up early (a server thread at shutdown, when the
  // message thread is busy joining it) passes `cancel`: once it reads true, a
  // call that has not started yet is dropped and false returned.
  static bool callOnMessageThread(std::function<void()> call, int timeoutMs,
                                  const std::atomic<bool>* cancel = nullptr);

  // Transport entry point: takes {"command":"...","args":{...}} and returns
  // the result envelope as one line of JSON. Other top-level keys are
  // ignored, so a transport can carry its own fields (a request id) through
  // the same object. Message thread only, like run().
  juce::String runJson(const juce::String& requestJson);

  // Every command as { name, summary, mutates, permission, params: [{ name,
  // type, required, description, enum?, min?, max?, items?, default?,
  // path? }] }, sorted by name.
  juce::var describe() const;
  // One command in the same shape; void when there is none of that name.
  juce::var describe(const juce::String& name) const;
  // The same information as a markdown reference: a short preamble on the
  // result envelope, an index, then one section per command.
  // `howToRegenerate` is one line for the preamble telling a reader how the
  // file is produced (the registry does not know which tool prints it).
  juce::String describeMarkdown(const juce::String& title,
                                const juce::String& howToRegenerate = {}) const;

  // Called on the message thread before a command's handler, once its
  // arguments have passed validation. For work the owner wants done ahead
  // of every command without each handler remembering it (the engine brings
  // hosted plugins' cached state up to date, so whatever snapshot the
  // command takes is current).
  std::function<void()> beforeRun;
  // Whether the command beforeRun is being called for edits the rig (its
  // `mutates`). A poll or a refresh takes no snapshot, so the hook can do
  // less for it.
  bool runningCommandMutates() const noexcept { return currentCommandMutates; }

private:
  bool currentCommandMutates{true};
  std::optional<CommandResult> validate(const juce::String& name, const juce::var& args,
                                        juce::var& validatedArgs,
                                        const Command*& commandOut) const;

  // Sorted by name, so describe() needs no extra pass.
  std::map<juce::String, Command> commands;

  // Lets a command posted by runFromAnyThread find out, on the message
  // thread, that the registry it was meant for is gone.
  std::shared_ptr<bool> lifetime;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CommandRegistry)
};

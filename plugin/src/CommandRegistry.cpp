#include "CommandRegistry.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>

namespace {

const char* typeName(CommandParamType type) {
  switch (type) {
    case CommandParamType::string: return "string";
    case CommandParamType::number: return "number";
    case CommandParamType::integer: return "integer";
    case CommandParamType::boolean: return "boolean";
    case CommandParamType::object: return "object";
    case CommandParamType::array: break;
  }
  return "array";
}

// "a number", "an integer": for messages that name the expected type.
juce::String typePhrase(CommandParamType type) {
  const bool vowel = type == CommandParamType::integer || type == CommandParamType::object ||
                     type == CommandParamType::array;
  return juce::String(vowel ? "an " : "a ") + typeName(type);
}

bool isFiniteNumber(const juce::var& v) {
  if (v.isInt() || v.isInt64())
    return true;
  return v.isDouble() && std::isfinite(static_cast<double>(v));
}

// JSON has one number type, so 3 and 3.0 are both integers here; 3.5 is not,
// and neither is anything an int cannot hold.
bool isIntegral(const juce::var& v) {
  if (!isFiniteNumber(v))
    return false;
  const double d = static_cast<double>(v);
  return std::floor(d) == d && d >= std::numeric_limits<int>::min() &&
         d <= std::numeric_limits<int>::max();
}

bool matchesType(const juce::var& v, CommandParamType type) {
  switch (type) {
    case CommandParamType::string: return v.isString();
    case CommandParamType::number: return isFiniteNumber(v);
    case CommandParamType::integer: return isIntegral(v);
    case CommandParamType::boolean: return v.isBool();
    // var::isObject is true for any reference-counted payload; only a
    // property bag is a JSON object.
    case CommandParamType::object: return v.getDynamicObject() != nullptr;
    case CommandParamType::array: return v.isArray();
  }
  return false;
}

// Whole numbers print without a fraction (bounds like 0, 1, 127 are the
// common case); everything else as the shortest text that reads back equal.
juce::String numberText(double value) {
  if (std::floor(value) == value && std::abs(value) < 1.0e15)
    return juce::String(static_cast<juce::int64>(value));
  return juce::String(value);
}

juce::var numberVar(double value, CommandParamType type) {
  if (type == CommandParamType::integer)
    return static_cast<int>(value);
  return value;
}

juce::String argError(const Command& command, const CommandParam& param, const juce::String& what) {
  return command.name + ": argument '" + param.name + "' " + what;
}

// Checks one present argument against its spec. Returns an empty string when
// it is acceptable, else the message for invalid_args.
juce::String checkArgument(const Command& command, const CommandParam& param,
                           const juce::var& value) {
  if (!matchesType(value, param.type))
    return argError(command, param, "must be " + typePhrase(param.type));

  if (param.type == CommandParamType::string && !param.enumValues.isEmpty() &&
      !param.enumValues.contains(value.toString()))
    return argError(command, param,
                    "must be one of: " + param.enumValues.joinIntoString(", ") + " (got '" +
                        value.toString() + "')");

  if (param.type == CommandParamType::number || param.type == CommandParamType::integer) {
    const double number = static_cast<double>(value);
    if (param.min.has_value() && number < *param.min)
      return argError(command, param,
                      "must be at least " + numberText(*param.min) + " (got " +
                          numberText(number) + ")");
    if (param.max.has_value() && number > *param.max)
      return argError(command, param,
                      "must be at most " + numberText(*param.max) + " (got " +
                          numberText(number) + ")");
  }

  if (param.type == CommandParamType::array && param.itemType.has_value()) {
    const auto* items = value.getArray();
    for (int i = 0; i < items->size(); ++i)
      if (!matchesType(items->getReference(i), *param.itemType))
        return argError(command, param,
                        "must be an array of " + juce::String(typeName(*param.itemType)) +
                            " values (element " + juce::String(i) + " is not)");
  }
  return {};
}

juce::var describeCommand(const Command& command) {
  juce::Array<juce::var> params;
  for (const auto& param : command.params) {
    juce::DynamicObject::Ptr p = new juce::DynamicObject();
    p->setProperty("name", param.name);
    p->setProperty("type", typeName(param.type));
    p->setProperty("required", param.isRequired);
    p->setProperty("description", param.description);
    if (!param.enumValues.isEmpty()) {
      juce::Array<juce::var> values;
      for (const auto& value : param.enumValues)
        values.add(value);
      p->setProperty("enum", values);
    }
    if (param.min.has_value())
      p->setProperty("min", numberVar(*param.min, param.type));
    if (param.max.has_value())
      p->setProperty("max", numberVar(*param.max, param.type));
    if (param.itemType.has_value())
      p->setProperty("items", typeName(*param.itemType));
    if (!param.defaultValue.isVoid())
      p->setProperty("default", param.defaultValue);
    if (param.isPath)
      p->setProperty("path", true);
    params.add(juce::var(p.get()));
  }

  juce::DynamicObject::Ptr obj = new juce::DynamicObject();
  obj->setProperty("name", command.name);
  obj->setProperty("summary", command.summary);
  obj->setProperty("mutates", command.mutates);
  obj->setProperty("permission", commandPermissionName(command.permission));
  obj->setProperty("params", params);
  return juce::var(obj.get());
}

// Table cells cannot hold a raw pipe or a line break.
juce::String markdownCell(const juce::String& text) {
  return text.replace("|", "\\|").replace("\n", " ");
}

juce::String markdownConstraints(const CommandParam& param) {
  juce::StringArray parts;
  if (!param.enumValues.isEmpty()) {
    juce::StringArray quoted;
    for (const auto& value : param.enumValues)
      quoted.add("`" + value + "`");
    parts.add("one of " + quoted.joinIntoString(", "));
  }
  if (param.min.has_value() && param.max.has_value())
    parts.add(numberText(*param.min) + " to " + numberText(*param.max));
  else if (param.min.has_value())
    parts.add("at least " + numberText(*param.min));
  else if (param.max.has_value())
    parts.add("at most " + numberText(*param.max));
  if (!param.defaultValue.isVoid())
    parts.add("default `" + juce::JSON::toString(param.defaultValue, true) + "`");
  return parts.joinIntoString("; ");
}

}  // namespace

// ####################
// RESULT / PARAM SPEC
// ####################
const char* commandPermissionName(CommandPermission permission) {
  switch (permission) {
    case CommandPermission::read: return "read";
    case CommandPermission::edit: return "edit";
    case CommandPermission::render: return "render";
    case CommandPermission::live: return "live";
    case CommandPermission::destroy: return "delete";
    case CommandPermission::settings: return "settings";
    case CommandPermission::local: return "local";
    case CommandPermission::unclassified: break;
  }
  return "unclassified";
}

CommandResult CommandResult::success(juce::var result) {
  CommandResult r;
  r.value = std::move(result);
  return r;
}

CommandResult CommandResult::failure(const char* code, const juce::String& message) {
  CommandResult r;
  r.ok = false;
  r.errorCode = code;
  r.errorMessage = message;
  return r;
}

juce::var CommandResult::toVar() const {
  juce::DynamicObject::Ptr obj = new juce::DynamicObject();
  obj->setProperty("ok", ok);
  if (ok) {
    obj->setProperty("result", value);
  } else {
    juce::DynamicObject::Ptr error = new juce::DynamicObject();
    error->setProperty("code", errorCode);
    error->setProperty("message", errorMessage);
    if (const auto* details = errorDetails.getDynamicObject())
      for (const auto& property : details->getProperties())
        error->setProperty(property.name, property.value);
    obj->setProperty("error", juce::var(error.get()));
  }
  return juce::var(obj.get());
}

CommandParam CommandParam::required(const juce::String& name, CommandParamType type,
                                    const juce::String& description) {
  CommandParam p;
  p.name = name;
  p.type = type;
  p.isRequired = true;
  p.description = description;
  return p;
}

CommandParam CommandParam::optional(const juce::String& name, CommandParamType type,
                                    const juce::String& description) {
  CommandParam p = required(name, type, description);
  p.isRequired = false;
  return p;
}

CommandParam CommandParam::oneOf(const juce::StringArray& values) const {
  CommandParam p = *this;
  p.enumValues = values;
  return p;
}

CommandParam CommandParam::range(double minimum, double maximum) const {
  CommandParam p = *this;
  p.min = minimum;
  p.max = maximum;
  return p;
}

CommandParam CommandParam::atLeast(double minimum) const {
  CommandParam p = *this;
  p.min = minimum;
  return p;
}

CommandParam CommandParam::items(CommandParamType elementType) const {
  CommandParam p = *this;
  p.itemType = elementType;
  return p;
}

CommandParam CommandParam::withDefault(const juce::var& value) const {
  CommandParam p = *this;
  p.defaultValue = value;
  return p;
}

CommandParam CommandParam::path(bool mayBeName) const {
  CommandParam p = *this;
  p.isPath = true;
  p.pathMayBeName = mayBeName;
  return p;
}

CommandParam CommandParam::withSibling(const juce::String& folderName) const {
  CommandParam p = *this;
  p.pathSibling = folderName;
  return p;
}

// ####################
// REGISTRY
// ####################
CommandRegistry::CommandRegistry() : lifetime(std::make_shared<bool>(true)) {}

CommandRegistry::~CommandRegistry() = default;

void CommandRegistry::add(Command command) {
  jassert(command.name.isNotEmpty() && command.handler.isSet());
  jassert(!contains(command.name));
  const juce::String name = command.name;
  commands.insert_or_assign(name, std::move(command));
}

bool CommandRegistry::contains(const juce::String& name) const {
  return commands.find(name) != commands.end();
}

const Command* CommandRegistry::find(const juce::String& name) const {
  const auto found = commands.find(name);
  return found != commands.end() ? &found->second : nullptr;
}

juce::StringArray CommandRegistry::names() const {
  juce::StringArray all;
  for (const auto& [name, command] : commands)
    all.add(name);
  return all;
}

CommandResult CommandRegistry::run(const juce::String& name, const juce::var& args) {
  CommandWork work = begin(name, args);
  if (work.finished.has_value())
    return *work.finished;
  return work.work();
}

CommandWork CommandRegistry::begin(const juce::String& name, const juce::var& args) {
  JUCE_ASSERT_MESSAGE_THREAD

  juce::var validated;
  const Command* command = nullptr;
  if (auto problem = validate(name, args, validated, command))
    return CommandWork::done(*problem);

  if (beforeRun != nullptr)
    beforeRun();
  if (command->handler.isSlow())
    return command->handler.slow(validated);
  return CommandWork::done(command->handler.quick(validated));
}

std::optional<CommandResult> CommandRegistry::check(const juce::String& name,
                                                    const juce::var& args) const {
  juce::var validated;
  const Command* command = nullptr;
  return validate(name, args, validated, command);
}

std::optional<CommandResult> CommandRegistry::validate(const juce::String& name,
                                                       const juce::var& args,
                                                       juce::var& validatedArgs,
                                                       const Command*& commandOut) const {
  const auto refuse = [](const char* code, const juce::String& message) {
    return CommandResult::failure(code, message);
  };

  const auto found = commands.find(name);
  if (found == commands.end())
    return refuse(CommandError::unknownCommand, "unknown command '" + name + "'");
  const Command& command = found->second;

  // JSON null and "no args at all" both mean an empty argument object.
  const juce::DynamicObject* given = args.getDynamicObject();
  if (given == nullptr && !args.isVoid() && !args.isUndefined())
    return refuse(CommandError::invalidArgs,
                  name + ": args must be an object of named arguments");

  if (given != nullptr) {
    for (const auto& property : given->getProperties()) {
      const juce::String argName = property.name.toString();
      const bool known =
          std::any_of(command.params.begin(), command.params.end(),
                      [&](const CommandParam& p) { return p.name == argName; });
      if (!known)
        return refuse(CommandError::invalidArgs, name + ": unknown argument '" + argName + "'");
    }
  }

  // Handlers get their own object: the caller's is never modified, integers
  // arrive as int whatever JSON spelling carried them, and defaults are in.
  juce::DynamicObject::Ptr validated = new juce::DynamicObject();
  for (const auto& param : command.params) {
    const juce::Identifier id(param.name);
    // An explicit null reads as "not given", which is what JSON writers that
    // always emit every key produce for an optional argument.
    const bool present = given != nullptr && given->hasProperty(id) &&
                         !given->getProperty(id).isVoid() && !given->getProperty(id).isUndefined();
    if (!present) {
      if (param.isRequired)
        return refuse(
            CommandError::invalidArgs,
            argError(command, param, "is required (" + juce::String(typeName(param.type)) + ")"));
      if (!param.defaultValue.isVoid())
        validated->setProperty(id, param.defaultValue);
      continue;
    }

    const juce::var value = given->getProperty(id);
    const juce::String problem = checkArgument(command, param, value);
    if (problem.isNotEmpty())
      return refuse(CommandError::invalidArgs, problem);

    if (param.type == CommandParamType::integer)
      validated->setProperty(id, static_cast<int>(static_cast<double>(value)));
    else
      validated->setProperty(id, value);
  }

  validatedArgs = juce::var(validated.get());
  commandOut = &command;
  return std::nullopt;
}

bool CommandRegistry::callOnMessageThread(std::function<void()> call, int timeoutMs,
                                          const std::atomic<bool>* cancel) {
  auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
  if (messageManager == nullptr)
    return false;
  if (messageManager->isThisTheMessageThread()) {
    call();
    return true;
  }

  // Shared with the posted call, which may outlive this stack frame when the
  // wait below gives up.
  struct Pending {
    std::mutex mutex;
    std::condition_variable finishedSignal;
    bool started = false, finished = false, abandoned = false;
    std::function<void()> call;
  };
  auto pending = std::make_shared<Pending>();
  pending->call = std::move(call);

  const bool posted = juce::MessageManager::callAsync([pending] {
    {
      const std::lock_guard<std::mutex> lock(pending->mutex);
      // The caller already gave up; running now would make an edit happen
      // behind a response that said it did not.
      if (pending->abandoned)
        return;
      pending->started = true;
    }
    pending->call();
    {
      const std::lock_guard<std::mutex> lock(pending->mutex);
      pending->finished = true;
    }
    pending->finishedSignal.notify_all();
  });
  if (!posted)
    return false;

  std::unique_lock<std::mutex> lock(pending->mutex);
  const auto finished = [&] { return pending->finished; };
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(juce::jmax(0, timeoutMs));
  // Waited in slices so a cancelled caller is not held for the whole timeout.
  while (!pending->finishedSignal.wait_for(lock, std::chrono::milliseconds(25), finished)) {
    if (pending->started) {
      // Already running: its outcome is the only truthful answer.
      pending->finishedSignal.wait(lock, finished);
      break;
    }
    if ((cancel != nullptr && cancel->load()) || std::chrono::steady_clock::now() >= deadline) {
      pending->abandoned = true;
      return false;
    }
  }
  return true;
}

CommandResult CommandRegistry::runFromAnyThread(const juce::String& name, const juce::var& args,
                                                int timeoutMs) {
  auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
  if (messageManager == nullptr)
    return CommandResult::failure(CommandError::unavailable, "no message thread is running");
  if (messageManager->isThisTheMessageThread())
    return run(name, args);

  // The registry dies on the message thread, so the check inside the call
  // cannot race its destructor.
  std::weak_ptr<bool> alive = lifetime;
  auto work = std::make_shared<CommandWork>();
  const bool ran = callOnMessageThread(
      [this, alive, work, name, args] {
        *work = alive.expired()
                    ? CommandWork::done(CommandResult::failure(CommandError::unavailable,
                                                               "the command layer has shut down"))
                    : begin(name, args);
      },
      timeoutMs);
  if (!ran)
    return CommandResult::failure(CommandError::unavailable,
                                  "timed out after " + juce::String(timeoutMs) +
                                      " ms waiting for the message thread; '" + name +
                                      "' was not run");
  if (work->finished.has_value())
    return *work->finished;
  return work->work();
}

juce::String CommandRegistry::runJson(const juce::String& requestJson) {
  const auto reply = [](const CommandResult& result) {
    return juce::JSON::toString(result.toVar(), true);
  };

  juce::var request;
  const juce::Result parsed = juce::JSON::parse(requestJson, request);
  if (parsed.failed())
    return reply(CommandResult::failure(CommandError::invalidArgs,
                                        "request is not valid JSON: " + parsed.getErrorMessage()));
  if (request.getDynamicObject() == nullptr || !request["command"].isString())
    return reply(CommandResult::failure(
        CommandError::invalidArgs,
        "request must be an object like {\"command\":\"name\",\"args\":{}}"));

  return reply(run(request["command"].toString(), request["args"]));
}

juce::var CommandRegistry::describe() const {
  juce::Array<juce::var> all;
  for (const auto& [name, command] : commands)
    all.add(describeCommand(command));
  return all;
}

juce::var CommandRegistry::describe(const juce::String& name) const {
  const auto found = commands.find(name);
  return found != commands.end() ? describeCommand(found->second) : juce::var();
}

juce::String CommandRegistry::describeMarkdown(const juce::String& title,
                                               const juce::String& howToRegenerate) const {
  juce::StringArray lines;
  lines.add("# " + title);
  lines.add({});
  lines.add("Generated from the command registry. Do not edit by hand: a test compares this file");
  lines.add("with the registry and fails when they differ.");
  if (howToRegenerate.isNotEmpty())
    lines.add(howToRegenerate);
  lines.add({});
  lines.add("A request names a command and gives its arguments as a JSON object:");
  lines.add({});
  lines.add("```json");
  lines.add("{\"command\": \"block.setParam\", \"args\": {\"blockId\": \"...\", \"param\": \"mix\", \"value\": 0.5}}");
  lines.add("```");
  lines.add({});
  lines.add("Every command answers with one of two envelopes:");
  lines.add({});
  lines.add("```json");
  lines.add("{\"ok\": true, \"result\": {}}");
  lines.add("{\"ok\": false, \"error\": {\"code\": \"invalid_args\", \"message\": \"...\"}}");
  lines.add("```");
  lines.add({});
  lines.add("Error codes:");
  lines.add({});
  lines.add("| Code | Meaning |");
  lines.add("|---|---|");
  lines.add("| `unknown_command` | No command of that name exists. |");
  lines.add("| `invalid_args` | An argument is missing, unknown, of the wrong type or outside its allowed values. Nothing was changed. |");
  lines.add("| `not_found` | A block, preset, mapping or file named by the arguments does not exist. |");
  lines.add("| `failed` | The arguments were valid but the engine refused or could not do it. |");
  lines.add("| `unavailable` | The command could not start or finish in time. Trying again later can succeed. |");
  lines.add("| `permission_denied` | Command server clients only: the command's permission class is switched off, the command is local only, or a path is outside the allowed folders. |");
  lines.add("| `needs_approval` | Command server clients only: the command changes the rig being played and waits for the user. The error carries `approvalId`; poll `approvals.status`. |");
  lines.add({});
  lines.add("Each command has a permission class, which decides whether a command server client (an");
  lines.add("agent) may run it: `read`, `edit`, `render`, `live`, `delete`, `settings` or `local` (never");
  lines.add("available to clients). See docs/agents.md.");
  lines.add({});
  lines.add("Arguments are validated before a command runs. An optional argument may be left out or");
  lines.add("given as `null`.");
  lines.add({});
  lines.add("## Commands");
  lines.add({});
  lines.add("| Command | Mutates | Permission | Summary |");
  lines.add("|---|---|---|---|");
  for (const auto& [name, command] : commands)
    lines.add("| [`" + name + "`](#" + name.toLowerCase().removeCharacters(".") + ") | " +
              (command.mutates ? "yes" : "no") + " | " +
              commandPermissionName(command.permission) + " | " + markdownCell(command.summary) +
              " |");

  for (const auto& [name, command] : commands) {
    lines.add({});
    lines.add("## " + name);
    lines.add({});
    lines.add(command.summary);
    lines.add({});
    lines.add(juce::String("Mutates state: ") + (command.mutates ? "yes" : "no") +
              ". Permission: " + commandPermissionName(command.permission) + ".");
    lines.add({});
    if (command.params.empty()) {
      lines.add("No parameters.");
      continue;
    }
    lines.add("| Parameter | Type | Required | Allowed | Description |");
    lines.add("|---|---|---|---|---|");
    for (const auto& param : command.params) {
      juce::String type = typeName(param.type);
      if (param.itemType.has_value())
        type += " of " + juce::String(typeName(*param.itemType));
      if (param.isPath)
        type += " (path)";
      lines.add("| `" + param.name + "` | " + type + " | " + (param.isRequired ? "yes" : "no") +
                " | " + markdownCell(markdownConstraints(param)) + " | " +
                markdownCell(param.description) + " |");
    }
  }
  return lines.joinIntoString("\n") + "\n";
}

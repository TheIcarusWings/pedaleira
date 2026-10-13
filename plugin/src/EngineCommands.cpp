#include "EngineCommands.h"

// Processor.h first: it pulls in the JUCE module headers, which must come
// before anything that drags in <windows.h> (its `small` macro breaks
// juce_gui_extra on MSVC).
#include "Processor.h"

#include "AgentHub.h"
#include "BlockEq.h"
#include "BlockKinds.h"
#include "ChainDomain.h"
#include "HostedPluginBlockProcessor.h"
#include "PluginScanner.h"
#include "Scenes.h"

#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using P = CommandParam;
using T = CommandParamType;

// The EQ band bounds are spelled out as literals in the command specs so the
// reference shows round numbers; this keeps them tied to the engine's.
// (Compared as scaled integers: the bounds are floats.)
static_assert(BlockEq::kNumBands == 6 && static_cast<int>(BlockEq::kMinFreqHz) == 20 &&
              static_cast<int>(BlockEq::kMaxFreqHz) == 20000 &&
              static_cast<int>(BlockEq::kMaxAbsGainDb) == 15 &&
              static_cast<int>(BlockEq::kMinQ * 100.0f + 0.5f) == 10 &&
              static_cast<int>(BlockEq::kMaxQ) == 10,
              "block.setEqBand's argument ranges no longer match BlockEq");

// The standalone, plugin and headless builds each learn the version a
// different way; all of them come from the repo's VERSION file.
#if defined(JucePlugin_VersionString)
constexpr const char* kVersion = JucePlugin_VersionString;
#elif defined(T3K_VERSION_STRING)
constexpr const char* kVersion = T3K_VERSION_STRING;
#else
constexpr const char* kVersion = "unknown";
#endif

using Properties = std::initializer_list<std::pair<const char*, juce::var>>;

juce::var object(Properties properties) {
  juce::DynamicObject::Ptr obj = new juce::DynamicObject();
  for (const auto& [name, value] : properties)
    obj->setProperty(name, value);
  return juce::var(obj.get());
}

// Copy of `base` (an object var, or void) with `properties` set on top.
juce::var withProperties(const juce::var& base, Properties properties) {
  juce::DynamicObject::Ptr obj = new juce::DynamicObject();
  if (const auto* source = base.getDynamicObject())
    for (const auto& property : source->getProperties())
      obj->setProperty(property.name, property.value);
  for (const auto& [name, value] : properties)
    obj->setProperty(name, value);
  return juce::var(obj.get());
}

CommandResult ok(Properties properties) { return CommandResult::success(object(properties)); }

CommandResult fail(const char* code, const juce::String& message) {
  return CommandResult::failure(code, message);
}

constexpr std::pair<const char*, const char*> kLanes[] = {{"chain", "left"},
                                                          {"chainRight", "right"}};

// Where a block id sits in a getChainState payload. Blocks of a dormant
// right lane (mono mode) are not in the payload and so are not found, which
// matches what every other caller of the chain state can see.
struct BlockRef {
  juce::var item;
  juce::String side;
  int index = -1;
  bool isTone = false;
  int row = 0;  // 0 and 1 are the left and right lanes
};

// Every list of blocks in a getChainState payload with the lane it is: the
// two lanes of a legacy rig, or all four rows of a grid (rows 0 and 1 answer
// to the lane names, whether or not row 1 is heard; rows 2 and 3 have none).
std::vector<std::pair<juce::var, juce::String>> blockLists(const juce::var& chainState) {
  std::vector<std::pair<juce::var, juce::String>> lists;
  if (const auto* rows = chainState["grid"]["rows"].getArray()) {
    for (int row = 0; row < rows->size(); ++row)
      lists.emplace_back(rows->getReference(row)["blocks"],
                         row == 0 ? "left" : row == 1 ? "right" : "");
    return lists;
  }
  for (const auto& [key, side] : kLanes)
    lists.emplace_back(chainState[key], side);
  return lists;
}

std::optional<BlockRef> findBlock(const juce::var& chainState, const juce::String& blockId) {
  int row = 0;
  for (const auto& [list, side] : blockLists(chainState)) {
    if (const auto* lane = list.getArray())
      for (int i = 0; i < lane->size(); ++i) {
        const juce::var& item = lane->getReference(i);
        if (item["blockId"].toString() == blockId)
          return BlockRef{item, side, i, item["kind"].toString() == "tone", row};
      }
    ++row;
  }
  return std::nullopt;
}

juce::var laneIds(const juce::var& chainState, const char* key) {
  juce::Array<juce::var> ids;
  if (const auto* lane = chainState[key].getArray())
    for (const auto& item : *lane)
      ids.add(item["blockId"]);
  return ids;
}

CommandResult blockNotFound(const juce::String& blockId) {
  return fail(CommandError::notFound, "no block with id '" + blockId + "'");
}

// The error for a command that needs a tone block, or nullopt when `blockId`
// names one (then `found`, when given, receives it).
std::optional<CommandResult> toneBlockError(const juce::var& chainState,
                                            const juce::String& blockId,
                                            BlockRef* found = nullptr) {
  const auto ref = findBlock(chainState, blockId);
  if (!ref.has_value())
    return blockNotFound(blockId);
  if (!ref->isTone)
    return fail(CommandError::failed,
                "'" + blockId + "' is an empty insert slot, not a tone block");
  if (found != nullptr)
    *found = *ref;
  return std::nullopt;
}

bool presetExists(const juce::var& presetList, const juce::String& presetId) {
  if (const auto* presets = presetList["presets"].getArray())
    for (const auto& preset : *presets)
      if (preset["id"].toString() == presetId)
        return true;
  return false;
}

CommandResult presetNotFound(const juce::String& presetId) {
  return fail(CommandError::notFound, "no preset with id '" + presetId + "'");
}

// One APVTS parameter in real units: what state.get's `params` and the
// param.* commands ship.
juce::var describeParameter(juce::RangedAudioParameter& param) {
  const auto& range = param.getNormalisableRange();
  juce::DynamicObject::Ptr obj = new juce::DynamicObject();
  obj->setProperty("id", param.getParameterID());
  obj->setProperty("name", param.getName(256));
  obj->setProperty("value", static_cast<double>(param.convertFrom0to1(param.getValue())));
  obj->setProperty("normalized", static_cast<double>(param.getValue()));
  obj->setProperty("min", static_cast<double>(range.start));
  obj->setProperty("max", static_cast<double>(range.end));
  obj->setProperty("default", static_cast<double>(param.convertFrom0to1(param.getDefaultValue())));
  if (param.getLabel().isNotEmpty())
    obj->setProperty("unit", param.getLabel());

  // A choice parameter's value is an index; without the labels a caller
  // could not know what 0, 1, 2 select.
  if (auto* choice = dynamic_cast<juce::AudioParameterChoice*>(&param)) {
    obj->setProperty("type", "choice");
    juce::Array<juce::var> choices;
    for (const auto& label : choice->choices)
      choices.add(label);
    obj->setProperty("choices", choices);
  } else if (dynamic_cast<juce::AudioParameterBool*>(&param) != nullptr) {
    obj->setProperty("type", "boolean");
  } else if (dynamic_cast<juce::AudioParameterInt*>(&param) != nullptr) {
    obj->setProperty("type", "integer");
  } else {
    obj->setProperty("type", "float");
  }
  return juce::var(obj.get());
}

// Lets the processor's deferred work run for `ms`. Headless hosts (tests, the
// render tool) have no running message loop, so the oversampling re-rate and
// the MIDI mapper's notifications only happen while something pumps it.
// Builds without modal loops (the plugin itself) cannot pump from inside a
// call, so they only wait; their own loop does the delivering once the
// command returns, and model loads finish on the background pool either way.
// (Hosted plugins are created on the message thread; a waiter there runs
// those builds itself, see TONE3000Processor::serviceHostedPlugins.)
void letPendingWorkRun(int ms) {
#if JUCE_MODAL_LOOPS_PERMITTED
  juce::MessageManager::getInstance()->runDispatchLoopUntil(ms);
#else
  juce::Thread::sleep(ms);
#endif
}

}  // namespace

EngineCommands::~EngineCommands() = default;

EngineCommands::EngineCommands(TONE3000Processor& p, Scope s) : processor(p), scope(s) {
  // Every command starts from current plugin state: an edit's undo snapshot
  // and a duplicate's copy read hosted plugins from a cache that only the
  // message thread, off the chain lock, can refresh.
  // A command that only reads takes no snapshot: a plugin that keeps flagging
  // changes is not read again for every poll of the UI.
  commands.beforeRun = [this] {
    processor.serviceHostedPlugins(false, /*throttled=*/!commands.runningCommandMutates());
  };

  registerStateCommands();
  registerParameterCommands();
  registerChainCommands();
  registerBlockCommands();
  registerPluginCommands();
  registerPresetCommands();
  registerMidiCommands();
  registerLiveCommands();
  registerSceneCommands();
  registerFootswitchCommands();
  registerGridCommands();
  registerLibraryCommands();

  // Last, so the hub can tell the rig commands (everything registered so
  // far) from its own.
  if (scope == Scope::app) {
    hub = std::make_unique<AgentHub>(processor, commands);
    hub->registerCommands();
    // The rig being played is waited on like a sandbox is: a client inside
    // state.waitUntilLoaded lets go when its connection or the application
    // closes, instead of holding a quit for the length of its timeout.
    slowWorkCancel = [this] { return &hub->slowWorkCancel(); };
  }
}

EngineCommands::LoadState EngineCommands::loadState(TONE3000Processor& processor) {
  // What a parameter edit left for its gesture to settle (tail, latency) and
  // any latency a background install changed: a caller without a message
  // loop has no timer to do this for it.
  processor.flushDeferredBlockRefresh();

  LoadState state;
  const juce::var chain = processor.getChainState(-1);
  for (const auto& [list, side] : blockLists(chain)) {
    const auto* lane = list.getArray();
    if (lane == nullptr)
      continue;
    for (const auto& item : *lane) {
      if (item["kind"].toString() != "tone")
        continue;
      // A placeholder for a kind this build lacks will never load; it passes
      // audio through and must not hold a render up.
      if (static_cast<bool>(item["missing"]))
        continue;
      // A block whose processor failed while running is out of processing
      // for good: as final as a failed load.
      if (static_cast<bool>(item["loadFailed"]) || static_cast<bool>(item["processingFailed"]))
        state.failedBlockIds.add(item["blockId"]);
      else if (!static_cast<bool>(item["loaded"]) || static_cast<bool>(item["modelLoading"]))
        state.pendingBlockIds.add(item["blockId"]);
    }
  }
  // A restore keeps the chain muted until its loads have landed (see
  // releaseChainEditFadeWhenLoadsSettle); rendering before that captures
  // silence, so "loaded" includes the mute having let go.
  state.settled = state.pendingBlockIds.isEmpty() && !processor.isChainEditFadeHeld();
  state.revision = chain["revision"];
  return state;
}

CommandResult EngineCommands::waitUntilLoaded(TONE3000Processor& processor, int timeoutMs,
                                              const std::atomic<bool>* cancel) {
  const bool onMessageThread = juce::MessageManager::existsAndIsCurrentThread();
  const double deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
  for (;;) {
    // Asked every turn, not only when a look could not be had: a caller whose
    // connection is closing must not sit out the timeout because the rig
    // happens to keep answering.
    if (cancel != nullptr && cancel->load())
      return fail(CommandError::unavailable, "cancelled while waiting for the rig to load");
    // Before the first look as well: an edit made just before this call may
    // have work queued on the message loop that starts a load.
    if (onMessageThread)
      letPendingWorkRun(10);
    else
      juce::Thread::sleep(10);

    LoadState state;
    if (!CommandRegistry::callOnMessageThread([&] { state = loadState(processor); }, 2000, cancel)) {
      if (cancel != nullptr && cancel->load())
        return fail(CommandError::unavailable, "cancelled while waiting for the rig to load");
      // The message thread was busy for this look; the deadline decides.
    } else if (state.settled) {
      return ok({
          {"loaded", state.failedBlockIds.isEmpty()},
          {"failedBlockIds", state.failedBlockIds},
          {"revision", state.revision},
      });
    }

    if (juce::Time::getMillisecondCounterHiRes() >= deadline)
      return fail(CommandError::unavailable,
                  "timed out after " + juce::String(timeoutMs) + " ms with " +
                      juce::String(state.pendingBlockIds.size()) + " block(s) still loading");
  }
}

std::optional<CommandResult> EngineCommands::leaveGridFor(const char* command) {
  if (!processor.isGridMode())
    return std::nullopt;
  const juce::var converted = processor.convertRouting(false);
  if (static_cast<bool>(converted["ok"]))
    return std::nullopt;
  juce::StringArray reasons;
  if (const auto* list = converted["reasons"].getArray())
    for (const auto& reason : *list)
      reasons.add(reason.toString());
  return fail(CommandError::failed,
              juce::String(command) +
                  " is a legacy routing command, and this grid cannot be turned back into a "
                  "legacy rig: " +
                  reasons.joinIntoString("; "));
}

std::optional<CommandResult> EngineCommands::slotTarget(const juce::var& args, const char* command,
                                                        juce::String& target, int* rowOut,
                                                        int* slotOut) {
  const bool hasRow = !args["row"].isVoid();
  const bool hasSlot = !args["slot"].isVoid();
  if (!hasRow && !hasSlot)
    return std::nullopt;
  const juce::String name(command);
  if (!hasRow || !hasSlot)
    return fail(CommandError::invalidArgs, name + ": 'row' and 'slot' go together");
  if (!args["target"].isVoid() || !args["lane"].isVoid() || !args["side"].isVoid() ||
      !args["index"].isVoid())
    return fail(CommandError::invalidArgs,
                name + ": give 'row' and 'slot', or the lane arguments, not both");
  if (!processor.isGridMode())
    return fail(CommandError::failed,
                name + ": 'row' and 'slot' address the grid; convert the rig first with "
                       "grid.convert");
  const int row = args["row"];
  const int slot = args["slot"];
  const std::string id = processor.gridSlotBlockId(row, slot);
  if (id.empty())
    return fail(CommandError::notFound, name + ": no slot " + juce::String(slot) + " in row " +
                                            juce::String(row));
  target = juce::String(id);
  if (rowOut != nullptr)
    *rowOut = row;
  if (slotOut != nullptr)
    *slotOut = slot;
  return std::nullopt;
}

juce::String EngineCommands::markdownReference() const {
  return commands.describeMarkdown(
      "Pedaleira command reference",
      "Regenerate with `PedaleiraRender --describe-markdown > docs/commands.md`.");
}

// ####################
// SHARED PAYLOADS
// ####################
juce::var EngineCommands::parameterList() const {
  juce::DynamicObject::Ptr params = new juce::DynamicObject();
  // AudioProcessor's own list (host order), not the processor's APVTS
  // accessor of the same name.
  for (auto* param : processor.juce::AudioProcessor::getParameters())
    if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(param))
      params->setProperty(ranged->getParameterID(), describeParameter(*ranged));
  return juce::var(params.get());
}

juce::var EngineCommands::engineInfo(const juce::var& chainState) const {
  // The factor actually running, read off the chain rate: the osEnabled /
  // osFactor parameters lead it by one message-loop turn after a change.
  const double chainRate = static_cast<double>(chainState["sampleRate"]);
  return object({
      {"sampleRate", processor.getSampleRate()},
      {"blockSize", processor.getBlockSize()},
      {"chainSampleRate", chainRate},
      {"oversampling", juce::jmax(1, juce::roundToInt(chainRate / kChainBaseSampleRate))},
      {"latencySamples", processor.getLatencySamples()},
      {"inputChannels", processor.getTotalNumInputChannels()},
      {"outputChannels", processor.getTotalNumOutputChannels()},
      {"multiCore", processor.getMultiCoreEnabled()},
      {"version", kVersion},
  });
}

juce::var EngineCommands::fullState() const {
  const juce::var chain = processor.getChainState(-1);
  return object({
      {"chain", chain},
      {"grid", processor.getGridState()},
      {"params", parameterList()},
      {"presets", processor.getPresetList()["presets"]},
      {"library", processor.getLibraryState()},
      {"midi", processor.midiMapper.getState()},
      {"scenes", processor.getScenesState()},
      {"stomps", processor.getStompsState()["stomps"]},
      {"footswitches", processor.getFootswitchState()},
      {"engine", engineInfo(chain)},
  });
}

CommandResult EngineCommands::layoutResult(const juce::var& extra) const {
  const juce::var chain = processor.getChainState(-1);
  juce::var result = withProperties(extra, {
      {"revision", chain["revision"]},
      {"stereoEnabled", chain["stereoEnabled"]},
      {"left", laneIds(chain, "chain")},
      {"right", laneIds(chain, "chainRight")},
  });
  // A grid also lists every row's slots (rows 0 and 1 are left and right).
  if (const auto* rows = chain["grid"]["rows"].getArray()) {
    juce::Array<juce::var> rowIds;
    for (const auto& row : *rows)
      rowIds.add(row["slots"]);
    result = withProperties(result, {{"rows", rowIds}});
  }
  return CommandResult::success(result);
}

CommandResult EngineCommands::blockResult(const juce::String& blockId) const {
  const juce::var chain = processor.getChainState(-1);
  const auto ref = findBlock(chain, blockId);
  return ok({
      {"revision", chain["revision"]},
      {"blockId", blockId},
      {"block", ref.has_value() ? ref->item : juce::var()},
  });
}

// ####################
// STATE
// ####################
void EngineCommands::registerStateCommands() {
  commands.add({
      "state.get",
      "Returns the whole rig as one object: chain (blocks, lanes, branch, active preset; each "
      "block lists its scene-assigned parameters in sceneParams and those set aside for the "
      "expression pedal in expressionParams), params (every global parameter by id), presets "
      "(the current setlist's, as preset.list), library (as library.get), "
      "grid (as grid.get), midi (the MIDI map and the fixed default controls), scenes (as scene.get), stomps (as "
      "stomp.get), footswitches (as footswitch.getState) and engine (sample rate, block size, "
      "latency, oversampling, multi-core, version).",
      {},
      false,
      CommandPermission::read,
      [this](const juce::var&) { return CommandResult::success(fullState()); },
  });

  commands.add({
      "state.revision",
      "Returns the chain revision, a counter that goes up when the chain changes, so a client "
      "can poll this and refetch state.get only on change (a run of knob-rate edits counts "
      "once, about 0.4 s after the last one).",
      {},
      false,
      CommandPermission::read,
      [this](const juce::var&) {
        return ok({{"revision", static_cast<int>(processor.getCurrentChainRevision())}});
      },
  });

  commands.add({
      "state.waitUntilLoaded",
      "Blocks until every block has finished loading (or failed to, or stopped with a "
      "processing failure) and the rig is audible again, and applies what a parameter edit "
      "defers (the reported tail, a latency change), for headless callers that edit and then "
      "render; answers unavailable if that takes longer than timeoutMs.",
      {P::optional("timeoutMs", T::integer, "How long to wait, in milliseconds.")
           .range(0, 600000)
           .withDefault(30000)},
      false,
      CommandPermission::read,
      // Slow: over the command server the waiting happens on the caller's
      // thread, so other clients and the message loop carry on.
      [this](const juce::var& args) {
        const int timeoutMs = args["timeoutMs"];
        const std::atomic<bool>* cancel = slowWorkCancel != nullptr ? slowWorkCancel() : nullptr;
        return CommandWork::later(
            [this, timeoutMs, cancel] { return waitUntilLoaded(processor, timeoutMs, cancel); });
      },
  });

  // The full list belongs to the application's command set; a sandbox rig's
  // own registry holds only the rig commands and is never asked.
  if (scope == Scope::app)
    commands.add({
      "commands.describe",
      "Lists every command with its summary, parameters and whether it mutates state, or "
      "describes the one named.",
      {P::optional("name", T::string, "A command name; leave out for the full list.")},
      false,
      CommandPermission::read,
      [this](const juce::var& args) {
        if (args["name"].isVoid())
          return CommandResult::success(commands.describe());
        const juce::var one = commands.describe(args["name"].toString());
        if (one.isVoid())
          return fail(CommandError::notFound,
                      "no command named '" + args["name"].toString() + "'");
        return CommandResult::success(one);
      },
  });
}

// ####################
// GLOBAL PARAMETERS
// ####################
void EngineCommands::registerParameterCommands() {
  const auto unknownParameter = [](const juce::String& id) {
    return fail(CommandError::invalidArgs,
                "unknown parameter id '" + id + "' (see param.list)");
  };

  commands.add({
      "param.list",
      "Returns every global (faceplate and settings) parameter by id with its value in real "
      "units, normalized value, range, default, name, type and unit.",
      {},
      false,
      CommandPermission::read,
      [this](const juce::var&) { return CommandResult::success(parameterList()); },
  });

  commands.add({
      "param.get",
      "Returns one global parameter in the same shape as param.list.",
      {P::required("id", T::string, "Parameter id, for example gateThreshold.")},
      false,
      CommandPermission::read,
      [this, unknownParameter](const juce::var& args) {
        const juce::String id = args["id"].toString();
        auto* param = processor.parameters.getParameter(id);
        if (param == nullptr)
          return unknownParameter(id);
        return CommandResult::success(describeParameter(*param));
      },
  });

  commands.add({
      "param.set",
      "Sets one global parameter to a value in real units (dB, Hz, a choice index, 0 or 1 for "
      "a switch), clamped to the parameter's range, and returns the parameter as it now is.",
      {P::required("id", T::string, "Parameter id, for example gateThreshold."),
       P::required("value", T::number,
                   "The new value in the parameter's own units; out-of-range values are clamped.")},
      true,
      CommandPermission::edit,
      [this, unknownParameter](const juce::var& args) {
        const juce::String id = args["id"].toString();
        auto* param = processor.parameters.getParameter(id);
        if (param == nullptr)
          return unknownParameter(id);

        const auto& range = param->getNormalisableRange();
        const double requested = static_cast<double>(args["value"]);
        const float clamped = static_cast<float>(
            juce::jlimit(static_cast<double>(range.start), static_cast<double>(range.end),
                         requested));

        // Gestured, so a host records the jump as a user edit (the same
        // framing loadPreset and resetToDefault give their parameter moves).
        param->beginChangeGesture();
        param->setValueNotifyingHost(param->convertTo0to1(clamped));
        param->endChangeGesture();

        return CommandResult::success(withProperties(
            describeParameter(*param),
            {{"clamped", requested < range.start || requested > range.end}}));
      },
  });
}

// ####################
// CHAIN
// ####################
void EngineCommands::registerChainCommands() {
  commands.add({
      "chain.addToneFromFiles",
      "Adds ONE tone block from local files and starts loading it in the background: a single "
      ".nam or .wav (IR) file, a folder of them, or several files of the same kind that become "
      "the selectable models of that one block (call once per block to add an amp and a cab).",
      {P::required("paths", T::array,
                   "Absolute paths. One entry may be a file or a folder; several entries must "
                   "all be .nam files or all be .wav files.")
           .items(T::string)
           .path(),
       P::optional("target", T::string,
                   "Block id to load into: an empty insert slot (the tone takes that position) "
                   "or an existing tone block (its tone is replaced in place, keeping its id and "
                   "settings). Default: the first empty slot of the lane."),
       P::optional("lane", T::string,
                   "Lane to add to when no target is given; right needs stereo mode. Default: "
                   "the lane last added to (chain.activeSide).")
           .oneOf({"left", "right"}),
       P::optional("row", T::integer,
                   "Grid mode: the row of the slot to put it in, with slot, instead of target "
                   "or lane. They name the slot as target would: an empty slot takes the tone, "
                   "a block already in it has its tone replaced in place.")
           .range(0, 3),
       P::optional("slot", T::integer, "Grid mode: the slot in that row.").range(0, 7)},
      true,
      CommandPermission::edit,
      [this](const juce::var& args) {
        const auto* paths = args["paths"].getArray();
        if (paths->isEmpty())
          return fail(CommandError::invalidArgs,
                      "chain.addToneFromFiles: argument 'paths' must name at least one file");

        juce::Array<juce::File> files;
        for (const auto& path : *paths) {
          const juce::String text = path.toString();
          if (!juce::File::isAbsolutePath(text))
            return fail(CommandError::invalidArgs,
                        "chain.addToneFromFiles: argument 'paths' must hold absolute paths (got '" +
                            text + "')");
          const juce::File file(text);
          if (!file.exists())
            return fail(CommandError::notFound, "file not found: " + text);
          files.add(file);
        }

        // Several files make one multi-model tone, and a tone has one format
        // (the engine reads it off the first file), so a mixed list would
        // load the cab as an amp or the reverse.
        if (files.size() > 1) {
          const juce::String extension = files.getReference(0).getFileExtension().toLowerCase();
          for (const auto& file : files)
            if (file.isDirectory() || file.getFileExtension().toLowerCase() != extension ||
                (extension != ".nam" && extension != ".wav"))
              return fail(CommandError::invalidArgs,
                          "chain.addToneFromFiles: several 'paths' must all be .nam files or all "
                          "be .wav files, since together they form one block; add different "
                          "kinds with one call each");
        }

        juce::String target = args["target"].toString();
        const juce::String lane = args["lane"].toString();
        if (auto error = slotTarget(args, "chain.addToneFromFiles", target))
          return *error;
        const juce::var before = processor.getChainState(-1);
        if (target.isNotEmpty() && lane.isNotEmpty())
          return fail(CommandError::invalidArgs,
                      "chain.addToneFromFiles: give 'target' or 'lane', not both");
        // The engine falls back to the first free slot for a stale target (a
        // UI flow can outlive its slot); a command names it on purpose.
        if (target.isNotEmpty() && !findBlock(before, target).has_value())
          return blockNotFound(target);
        if (lane == "right" && !static_cast<bool>(before["stereoEnabled"]))
          return fail(CommandError::failed, "the right lane only exists in stereo mode");
        if (lane.isNotEmpty())
          processor.setActiveEditChain(lane);

        juce::var loaded;
        if (files.size() == 1) {
          loaded = processor.loadLocalTonePath(files.getReference(0), target.toStdString());
        } else {
          juce::Array<juce::URL> urls;
          for (const auto& file : files)
            urls.add(juce::URL(file));
          loaded = processor.loadLocalToneUrls(urls, target.toStdString());
        }

        const juce::String blockId = loaded["blockId"].toString();
        if (blockId.isEmpty())
          return fail(CommandError::failed, loaded["error"].toString().isNotEmpty()
                                                ? loaded["error"].toString()
                                                : juce::String("could not add the block"));
        return blockResult(blockId);
      },
  });

  commands.add({
      "kinds.list",
      "Lists every block kind this build knows: id, name, category, whether chain.addBlock can "
      "create it (nam and ir blocks come from chain.addToneFromFiles instead), the mix a new "
      "block of it starts at (defaultMix), how its mix knob blends dry and wet (mixLaw: "
      "crossfade is dry x (1 - mix) + wet x mix; dryPreserving keeps the dry signal at "
      "unity up to mix 0.5, where the wet signal reaches unity too, and fades it above "
      "that), whether its tail rings out after a bypass "
      "(spillover: the kind then has a spillover switch among its parameters, on by default) "
      "its own parameters with their ranges, units and defaults, and its readings (read-only "
      "values it reports while it runs, such as a compressor's gain reduction; meters.get "
      "returns them). categories names every category a kind can be filed under.",
      {},
      false,
      CommandPermission::read,
      [](const juce::var&) {
        juce::Array<juce::var> kinds;
        for (const BlockKindInfo* info : BlockKindRegistry::instance().all()) {
          juce::Array<juce::var> params;
          for (const auto& spec : info->params)
            params.add(spec.toVar());
          juce::Array<juce::var> readings;
          for (const auto& spec : info->readings)
            readings.add(spec.toVar());
          kinds.add(object({
              {"id", info->id},
              {"name", info->name},
              {"category", blockCategoryToString(info->category)},
              {"addable", info->addable},
              // Rounded: a float 0.35 would otherwise print its binary tail.
              {"defaultMix", std::round(static_cast<double>(info->defaultMix) * 1000.0) / 1000.0},
              {"mixLaw", blockMixLawToString(info->mixLaw)},
              {"spillover", info->spillover},
              {"params", params},
              {"readings", readings},
          }));
        }
        juce::Array<juce::var> categories;
        for (const BlockCategory category : allBlockCategories())
          categories.add(blockCategoryToString(category));
        juce::Array<juce::var> common;
        for (const auto& spec : commonBlockParams())
          common.add(spec.toVar());
        return ok({{"kinds", kinds}, {"categories", categories}, {"commonParams", common}});
      },
  });

  commands.add({
      "chain.addBlock",
      "Adds a block of a registered kind (see kinds.list), such as utility.gain, at its "
      "default settings; unlike a tone it has no model to load and is live when the command "
      "returns.",
      {P::required("kind", T::string, "The kind id, from kinds.list."),
       P::optional("target", T::string,
                   "Block id to put it in: an empty insert slot (the block takes that position) "
                   "or an existing tone block (replaced in place, keeping its id, gains, mix "
                   "and EQ). Default: the first empty slot of the lane."),
       P::optional("lane", T::string,
                   "Lane to add to when no target is given; right needs stereo mode. Default: "
                   "the lane last added to (chain.activeSide).")
           .oneOf({"left", "right"}),
       P::optional("row", T::integer,
                   "Grid mode: the row of the slot to put it in, with slot, instead of target "
                   "or lane.")
           .range(0, 3),
       P::optional("slot", T::integer, "Grid mode: the slot in that row.").range(0, 7)},
      true,
      CommandPermission::edit,
      [this](const juce::var& args) {
        const juce::String kind = args["kind"].toString();
        const BlockKindInfo* info = BlockKindRegistry::instance().find(kind);
        if (info == nullptr)
          return fail(CommandError::invalidArgs,
                      "chain.addBlock: no block kind '" + kind + "' (see kinds.list)");
        if (kind == HostedPluginBlockProcessor::kKindId)
          return fail(CommandError::invalidArgs,
                      "chain.addBlock: a 'plugin' block hosts one particular plugin; add it "
                      "with chain.addPlugin");
        if (!info->addable)
          return fail(CommandError::invalidArgs,
                      "chain.addBlock: a '" + kind +
                          "' block needs a model; add it with chain.addToneFromFiles");

        juce::String target = args["target"].toString();
        const juce::String lane = args["lane"].toString();
        if (auto error = slotTarget(args, "chain.addBlock", target))
          return *error;
        const juce::var before = processor.getChainState(-1);
        if (target.isNotEmpty() && lane.isNotEmpty())
          return fail(CommandError::invalidArgs, "chain.addBlock: give 'target' or 'lane', not both");
        if (target.isNotEmpty() && !findBlock(before, target).has_value())
          return blockNotFound(target);
        if (lane == "right" && !static_cast<bool>(before["stereoEnabled"]))
          return fail(CommandError::failed, "the right lane only exists in stereo mode");
        if (lane.isNotEmpty())
          processor.setActiveEditChain(lane);

        const juce::var added = processor.addKindBlock(kind, target.toStdString());
        const juce::String blockId = added["blockId"].toString();
        if (blockId.isEmpty())
          return fail(CommandError::failed, added["error"].toString().isNotEmpty()
                                                ? added["error"].toString()
                                                : juce::String("could not add the block"));
        return blockResult(blockId);
      },
  });

  commands.add({
      "chain.removeBlock",
      "Removes a tone block from the chain.",
      {P::required("blockId", T::string, "The tone block to remove.")},
      true,
      CommandPermission::edit,
      [this](const juce::var& args) {
        const juce::String blockId = args["blockId"].toString();
        if (auto error = toneBlockError(processor.getChainState(-1), blockId))
          return *error;
        if (!processor.removeChainBlock(blockId.toStdString()))
          return fail(CommandError::failed, "could not remove block '" + blockId + "'");
        return layoutResult(object({{"removedBlockId", blockId}}));
      },
  });

  commands.add({
      "chain.reorder",
      "Reorders the blocks of one lane; signal flows through them in the order given.",
      {P::required("order", T::array,
                   "Every block id of exactly one lane, empty insert slots included, in the new "
                   "order (the lane is inferred from the ids; see left / right in any chain "
                   "command's result).")
           .items(T::string)},
      true,
      CommandPermission::edit,
      [this](const juce::var& args) {
        const juce::var before = processor.getChainState(-1);
        std::vector<std::string> order;
        const juce::var ids = args["order"];
        for (const auto& id : *ids.getArray()) {
          if (!findBlock(before, id.toString()).has_value())
            return blockNotFound(id.toString());
          order.push_back(id.toString().toStdString());
        }
        if (!processor.reorderChainBlocks(order))
          return fail(CommandError::failed,
                      "'order' must list every block id of exactly one lane once, insert slots "
                      "included");
        return layoutResult();
      },
  });

  commands.add({
      "chain.moveBlock",
      "Moves a tone block across to the other lane at the given position (stereo mode only; "
      "use chain.reorder to move within a lane).",
      {P::required("blockId", T::string, "The tone block to move."),
       P::required("side", T::string, "The lane to move it into.").oneOf({"left", "right"}),
       P::required("index", T::integer,
                   "Position in the destination lane, counting insert slots; clamped to its "
                   "length.")
           .atLeast(0)},
      true,
      CommandPermission::edit,
      [this](const juce::var& args) {
        const juce::String blockId = args["blockId"].toString();
        const juce::String side = args["side"].toString();
        const juce::var before = processor.getChainState(-1);
        BlockRef ref;
        if (auto error = toneBlockError(before, blockId, &ref))
          return *error;
        if (processor.isGridMode())
          return fail(CommandError::failed,
                      "chain.moveBlock moves between the two legacy lanes; in grid mode use "
                      "grid.moveBlock");
        if (!static_cast<bool>(before["stereoEnabled"]))
          return fail(CommandError::failed, "blocks only move between lanes in stereo mode");
        if (ref.side == side)
          return fail(CommandError::failed,
                      "block '" + blockId + "' is already in the " + side +
                          " lane; use chain.reorder to move it within a lane");
        if (!processor.moveBlockToChain(blockId.toStdString(), side, args["index"]))
          return fail(CommandError::failed, "could not move block '" + blockId + "'");
        return layoutResult(object({{"blockId", blockId}}));
      },
  });

  commands.add({
      "chain.duplicateBlock",
      "Clones a tone block with its model and every setting, and returns the new block's id.",
      {P::required("blockId", T::string, "The tone block to clone."),
       P::optional("side", T::string, "Lane the clone lands in. Default: the source's lane.")
           .oneOf({"left", "right"}),
       P::optional("index", T::integer,
                   "Position in that lane, counting insert slots; an empty slot there is "
                   "filled, anything else is pushed along. Default: right after the source.")
           .atLeast(0),
       P::optional("row", T::integer,
                   "Grid mode: the row the clone lands in, with slot, instead of side and index "
                   "(an empty slot there is filled, anything else is pushed toward the row's "
                   "nearest empty slot).")
           .range(0, 3),
       P::optional("slot", T::integer, "Grid mode: the slot in that row.").range(0, 7)},
      true,
      CommandPermission::edit,
      [this](const juce::var& args) {
        const juce::String blockId = args["blockId"].toString();
        const juce::var before = processor.getChainState(-1);
        BlockRef ref;
        if (auto error = toneBlockError(before, blockId, &ref))
          return *error;
        juce::String slotId;
        int gridRow = -1, gridSlot = -1;
        if (auto error = slotTarget(args, "chain.duplicateBlock", slotId, &gridRow, &gridSlot))
          return *error;
        const juce::String side = args["side"].isVoid() ? ref.side : args["side"].toString();
        // In a grid every row takes blocks, heard or not; the default lane
        // is the source's row.
        const bool gridMode = processor.isGridMode();
        if (gridRow < 0 && side == "right" && !gridMode &&
            !static_cast<bool>(before["stereoEnabled"]))
          return fail(CommandError::failed, "the right lane only exists in stereo mode");
        const int row = gridRow >= 0 ? gridRow
                            : args["side"].isVoid() ? ref.row : side == "right" ? 1 : 0;
        const int index = gridRow >= 0 ? gridSlot
                              : args["index"].isVoid() ? (row == ref.row ? ref.index + 1 : 0)
                                                       : static_cast<int>(args["index"]);

        const std::string newId =
            processor.duplicateChainBlockToRow(blockId.toStdString(), row, index);
        if (newId.empty())
          return fail(CommandError::failed,
                      "could not duplicate block '" + blockId + "'" +
                          (gridMode ? " (a grid row holds eight blocks)" : ""));
        const juce::var after = processor.getChainState(-1);
        const auto clone = findBlock(after, juce::String(newId));
        return layoutResult(object({
            {"blockId", juce::String(newId)},
            {"block", clone.has_value() ? clone->item : juce::var()},
        }));
      },
  });

  commands.add({
      "chain.setStereo",
      "Switches between one mono chain and two independent left / right lanes.",
      {P::required("enabled", T::boolean, "True for two lanes, false for one chain.")},
      true,
      CommandPermission::edit,
      [this](const juce::var& args) {
        if (auto error = leaveGridFor("chain.setStereo"))
          return *error;
        processor.setStereoMode(static_cast<bool>(args["enabled"]));
        return layoutResult();
      },
  });

  commands.add({
      "chain.setInputMode",
      "Chooses how a stereo source feeds the chain: stereo (the natural routing), left or "
      "right (one channel onto both), or dual (a mono chain run once per channel).",
      {P::required("mode", T::string, "The input mode.")
           .oneOf({"stereo", "left", "right", "dual"})},
      true,
      CommandPermission::edit,
      [this](const juce::var& args) {
        if (auto error = leaveGridFor("chain.setInputMode"))
          return *error;
        const auto mode = TONE3000Processor::inputModeFromString(args["mode"].toString());
        processor.setInputMode(mode);
        // The engine keeps a branched chain on a single source and ignores
        // the request without saying so.
        if (processor.getInputMode() != mode)
          return fail(CommandError::failed,
                      "stereo and dual are not available while the chain is branched");
        const juce::var chain = processor.getChainState(-1);
        return ok({{"revision", chain["revision"]}, {"inputMode", chain["inputMode"]}});
      },
  });

  commands.add({
      "chain.setBranch",
      "Feeds one lane from the other lane's signal after a chosen tone block instead of from "
      "its own input (stereo mode only); calling it again moves the tap.",
      {P::required("side", T::string, "The trunk lane, which keeps its own input.")
           .oneOf({"left", "right"}),
       P::required("afterBlockId", T::string,
                   "A tone block in that lane (the trunk); the other lane taps its output.")},
      true,
      CommandPermission::edit,
      [this](const juce::var& args) {
        if (auto error = leaveGridFor("chain.setBranch"))
          return *error;
        const juce::String side = args["side"].toString();
        const juce::String blockId = args["afterBlockId"].toString();
        const juce::var before = processor.getChainState(-1);
        BlockRef ref;
        if (auto error = toneBlockError(before, blockId, &ref))
          return *error;
        if (!static_cast<bool>(before["stereoEnabled"]))
          return fail(CommandError::failed, "the chain only branches in stereo mode");
        if (ref.side != side)
          return fail(CommandError::failed,
                      "block '" + blockId + "' is in the " + ref.side + " lane, not the " + side +
                          " lane");
        if (!processor.setChainBranch(side, blockId.toStdString()))
          return fail(CommandError::failed, "could not branch after block '" + blockId + "'");
        const juce::var chain = processor.getChainState(-1);
        return layoutResult(object({{"branch", chain["branch"]}, {"inputMode", chain["inputMode"]}}));
      },
  });

  commands.add({
      "chain.clearBranch",
      "Removes the branch so both lanes take their own input again.",
      {},
      true,
      CommandPermission::edit,
      [this](const juce::var&) {
        if (auto error = leaveGridFor("chain.clearBranch"))
          return *error;
        const bool changed = processor.clearChainBranch();
        return layoutResult(object({{"changed", changed}}));
      },
  });

  commands.add({
      "chain.swapLanes",
      "Swaps the left and right lanes wholesale (stereo mode only).",
      {},
      true,
      CommandPermission::edit,
      [this](const juce::var&) {
        if (auto error = leaveGridFor("chain.swapLanes"))
          return *error;
        if (!processor.swapChains())
          return fail(CommandError::failed, "lanes only swap in stereo mode");
        return layoutResult();
      },
  });

  const auto historyResult = [this] {
    const juce::var chain = processor.getChainState(-1);
    return layoutResult(object({{"canUndo", chain["canUndo"]}, {"canRedo", chain["canRedo"]}}));
  };

  commands.add({
      "chain.undo",
      "Undoes the last chain edit (structure, tones, block settings, EQ, stereo mode, preset "
      "load); global parameters are not part of the history.",
      {},
      true,
      CommandPermission::edit,
      [this, historyResult](const juce::var&) {
        if (!processor.undoChain())
          return fail(CommandError::failed, "nothing to undo");
        return historyResult();
      },
  });

  commands.add({
      "chain.redo",
      "Redoes the chain edit most recently undone.",
      {},
      true,
      CommandPermission::edit,
      [this, historyResult](const juce::var&) {
        if (!processor.redoChain())
          return fail(CommandError::failed, "nothing to redo");
        return historyResult();
      },
  });

  commands.add({
      "chain.reset",
      "Returns to the factory default: an empty mono chain, preset-managed parameters at their "
      "defaults and no active preset, as one undoable step.",
      {},
      true,
      CommandPermission::edit,
      [this](const juce::var&) {
        const bool changed = processor.resetToDefault();
        return layoutResult(object({{"changed", changed}}));
      },
  });
}

// ####################
// BLOCKS
// ####################
void EngineCommands::registerBlockCommands() {
  const P blockIdParam = P::required("blockId", T::string, "The tone block.");

  // Runs `edit` on a tone block and answers with the block as it now is.
  const auto editBlock = [this](const juce::String& blockId, const juce::String& what,
                                const std::function<bool(const std::string&)>& edit) {
    if (auto error = toneBlockError(processor.getChainState(-1), blockId))
      return *error;
    if (!edit(blockId.toStdString()))
      return fail(CommandError::failed, "could not " + what + " on block '" + blockId + "'");
    return blockResult(blockId);
  };

  commands.add({
      "block.listParams",
      "Lists everything block.setParam can set on a block, with current values: the five "
      "common settings (scope common, knob positions from 0 to 1) and then the parameters of "
      "the block's kind (scope kind, real units), each with its range, unit, default and flags. "
      "A hosted plugin block lists the plugin's own automatable parameters (the first 256) as "
      "normalised 0 to 1 values under the plugin's parameter ids, with the plugin's display "
      "text for the current value in text.",
      {blockIdParam},
      false,
      CommandPermission::read,
      [this](const juce::var& args) {
        const juce::String blockId = args["blockId"].toString();
        const juce::var chain = processor.getChainState(-1);
        BlockRef ref;
        if (auto error = toneBlockError(chain, blockId, &ref))
          return *error;
        return ok({
            {"revision", chain["revision"]},
            {"blockId", blockId},
            {"kindId", ref.item["kindId"]},
            {"category", ref.item["category"]},
            {"params", processor.getBlockParams(blockId.toStdString())},
        });
      },
  });

  commands.add({
      "block.setParam",
      "Sets one of a block's settings. The common ones are knob positions: inputGain, "
      "outputGain and mix from 0 to 1 (0.5 is unity for the gains), enabled and normalize as "
      "switches (0 or 1). Any other id is a parameter of the block's kind (see "
      "block.listParams) and takes a value in that parameter's own unit, clamped to its range "
      "(0 to 1 for a hosted plugin's parameters). The blocks carry the active scene: enabled "
      "and a scene-assigned parameter (scene.assignParam) change in the active scene only. "
      "While the preset has more than one scene (scene.add), a parameter that is not assigned "
      "yet is scene-assigned by its first set, so it changes in the active scene only and the "
      "others keep the value they had; this is one undo step. With one scene, and for "
      "normalize, a parameter on the expression pedal or one scene.assignParam refuses, the "
      "setting has one value shared by all scenes.",
      {blockIdParam,
       P::required("param", T::string,
                   "Which setting: enabled, normalize, inputGain, outputGain, mix, or a kind "
                   "parameter id. A newly added IR block takes its default mix when its first "
                   "load lands, so set mix after state.waitUntilLoaded."),
       P::required("value", T::number,
                   "For the common settings a knob position from 0 to 1 (switches: 1 is on, 0 "
                   "is off); for a kind parameter its real value (dB, ms, a choice index).")},
      true,
      CommandPermission::edit,
      [this, editBlock](const juce::var& args) {
        const juce::String blockId = args["blockId"].toString();
        const juce::String param = args["param"].toString();
        const double value = static_cast<double>(args["value"]);

        // What can be checked without the block is checked before the
        // lookup, like any argument outside a command's spec: the common
        // settings' range, and an id that is neither common nor a parameter
        // of any kind.
        if (findBlockParam(commonBlockParams(), param) >= 0) {
          if (value < 0.0 || value > 1.0)
            return fail(CommandError::invalidArgs,
                        "block.setParam: argument 'value' must be between 0 and 1 for '" +
                            param + "'");
          return editBlock(blockId, "set " + param, [&](const std::string& id) {
            return processor.setBlockParam(id, param, value);
          });
        }

        // The block's own list, not the registry's table for its kind: a
        // hosted plugin's parameters belong to the instance, so two blocks
        // of the kind "plugin" have different ones.
        const auto ownSpec = [&]() -> juce::var {
          const juce::var all = processor.getBlockParams(blockId.toStdString());
          if (const auto* listed = all.getArray())
            for (const auto& entry : *listed)
              if (entry["scope"].toString() == "kind" && entry["id"].toString() == param)
                return entry;
          return {};
        };

        const juce::var before = processor.getChainState(-1);
        const auto found = findBlock(before, blockId);
        const juce::var spec = found.has_value() && found->isTone ? ownSpec() : juce::var();

        bool anyKindHasIt = spec.isObject();
        for (const BlockKindInfo* info : BlockKindRegistry::instance().all())
          anyKindHasIt = anyKindHasIt || findBlockParam(info->params, param) >= 0;
        if (!anyKindHasIt)
          return fail(CommandError::invalidArgs,
                      "block.setParam: no block has a parameter '" + param + "'");

        BlockRef ref;
        if (auto error = toneBlockError(before, blockId, &ref))
          return *error;
        const juce::String kindId = ref.item["kindId"].toString();
        if (!spec.isObject())
          return fail(CommandError::invalidArgs,
                      "block.setParam: a '" + kindId + "' block has no parameter '" + param +
                          "' (see block.listParams)");

        if (!processor.setBlockParam(blockId.toStdString(), param, value))
          return fail(CommandError::failed,
                      "could not set " + param + " on block '" + blockId + "'");
        const CommandResult result = blockResult(blockId);
        return CommandResult::success(withProperties(
            result.value,
            {{"param", param},
             {"value", result.value["block"]["kindParams"][juce::Identifier(param)]},
             {"clamped", value < static_cast<double>(spec["min"]) ||
                             value > static_cast<double>(spec["max"])}}));
      },
  });

  commands.add({
      "block.setEnabled",
      "Turns a block on or off (off passes the signal through untouched). A block of a kind "
      "with spillover (see kinds.list) whose spillover parameter is on keeps ringing after it "
      "is turned off: its input closes and its tail plays out over the dry signal, for as "
      "long as the block's reported tail (at most 30 seconds); with the parameter off the "
      "tail is cut. The state belongs to the active scene: a block is in the same state in "
      "every scene until this is first used on it, and from then on each scene has its own.",
      {blockIdParam, P::required("enabled", T::boolean, "True to process, false to bypass.")},
      true,
      CommandPermission::edit,
      [this, editBlock](const juce::var& args) {
        return editBlock(args["blockId"].toString(), "set enabled", [&](const std::string& id) {
          return processor.setBlockParam(id, "enabled",
                                         static_cast<bool>(args["enabled"]) ? 1.0 : 0.0);
        });
      },
  });

  commands.add({
      "block.setSlimSize",
      "Sets a NAM block's model size, trading CPU for accuracy: 0 is the lightest, 1 the full "
      "model (only A2 slimmable captures change; IR blocks refuse).",
      {blockIdParam,
       P::required("slimSize", T::number, "Model size from 0 (lite) to 1 (full).")
           .range(0.0, 1.0)},
      true,
      CommandPermission::edit,
      [this, editBlock](const juce::var& args) {
        const juce::String blockId = args["blockId"].toString();
        BlockRef ref;
        if (auto error = toneBlockError(processor.getChainState(-1), blockId, &ref))
          return *error;
        if (ref.item["tone"]["format"].toString() != "nam")
          return fail(CommandError::failed, "block '" + blockId + "' is not a NAM block");
        return editBlock(blockId, "set the model size", [&](const std::string& id) {
          return processor.setBlockSlimSize(id, static_cast<double>(args["slimSize"]));
        });
      },
  });

  commands.add({
      "block.setEqBand",
      "Changes one band of a block's six-band EQ; fields left out keep their current value.",
      {blockIdParam,
       P::required("band", T::integer,
                   "Band index: 0 is the low band, 5 the high band, 1 to 4 are bells.")
           .range(0, BlockEq::kNumBands - 1),
       P::optional("type", T::string,
                   "Filter type. Band 0 takes lowcut or lowshelf, band 5 highcut or highshelf, "
                   "the others are always bell; a type the band cannot take is replaced by the "
                   "band's shelf or bell.")
           .oneOf({"lowcut", "lowshelf", "bell", "highshelf", "highcut"}),
       P::optional("freqHz", T::number, "Centre or corner frequency in Hz.").range(20.0, 20000.0),
       P::optional("gainDb", T::number, "Gain in dB (ignored by the cut types).")
           .range(-15.0, 15.0),
       P::optional("q", T::number, "Q (bandwidth or slope).").range(0.1, 10.0)},
      true,
      CommandPermission::edit,
      [this](const juce::var& args) {
        const juce::String blockId = args["blockId"].toString();
        const int bandIndex = args["band"];
        BlockRef ref;
        if (auto error = toneBlockError(processor.getChainState(-1), blockId, &ref))
          return *error;

        // The engine takes a whole band; start from the live one so a caller
        // can move just the gain.
        const juce::var current = ref.item["params"]["eq"]["bands"][bandIndex];
        juce::DynamicObject::Ptr band = new juce::DynamicObject();
        for (const char* field : {"type", "freqHz", "gainDb", "q"})
          band->setProperty(field, args[field].isVoid() ? current[field] : args[field]);

        if (!processor.setBlockEqBand(blockId.toStdString(), bandIndex, juce::var(band.get())))
          return fail(CommandError::failed, "could not set the EQ band on block '" + blockId + "'");
        return blockResult(blockId);
      },
  });

  commands.add({
      "block.setEqEnabled",
      "Turns a block's EQ on or off, keeping its band settings.",
      {blockIdParam, P::required("enabled", T::boolean, "True to apply the EQ.")},
      true,
      CommandPermission::edit,
      [this, editBlock](const juce::var& args) {
        return editBlock(args["blockId"].toString(), "switch the EQ", [&](const std::string& id) {
          return processor.setBlockEqEnabled(id, static_cast<bool>(args["enabled"]));
        });
      },
  });

  commands.add({
      "block.setEqPre",
      "Places a block's EQ before its model (true) or after it (false, the default).",
      {blockIdParam, P::required("pre", T::boolean, "True for pre-model, false for post-model.")},
      true,
      CommandPermission::edit,
      [this, editBlock](const juce::var& args) {
        return editBlock(args["blockId"].toString(), "move the EQ", [&](const std::string& id) {
          return processor.setBlockEqPre(id, static_cast<bool>(args["pre"]));
        });
      },
  });

  commands.add({
      "block.resetEq",
      "Returns a block's EQ to its flat defaults, switched on and post-model.",
      {blockIdParam},
      true,
      CommandPermission::edit,
      [this, editBlock](const juce::var& args) {
        return editBlock(args["blockId"].toString(), "reset the EQ", [&](const std::string& id) {
          return processor.resetBlockEq(id);
        });
      },
  });
}

// ####################
// PRESETS
// ####################
void EngineCommands::registerPresetCommands() {
  const P presetIdParam = P::required("id", T::string, "Preset id, as preset.list returns it.");

  const auto listResult = [this](Properties extra) {
    return CommandResult::success(withProperties(processor.getPresetList(), extra));
  };

  commands.add({
      "preset.list",
      "Lists the presets of the current setlist in slot order as { presets: [{ id, name, "
      "factory, slot }], setlist: { id, name, readOnly } }. Empty slots are left out; a "
      "preset's slot is also its MIDI program number plus one and its place on the "
      "footswitches' preset banks.",
      {},
      false,
      CommandPermission::read,
      [this](const juce::var&) { return CommandResult::success(processor.getPresetList()); },
  });

  commands.add({
      "preset.save",
      "Saves the current chain and preset-managed parameters as a preset of the current "
      "setlist, makes it the active preset and returns its id, setlist and slot. Saving under "
      "the name of a preset of the setlist replaces that preset; otherwise it goes into the "
      "first empty slot. With the read-only factory setlist current it goes to the first "
      "setlist of the user's own (\"My Presets\" is made when there is none), which becomes "
      "current.",
      {P::required("name", T::string, "Preset name; must not be blank.")},
      true,
      CommandPermission::edit,
      [this](const juce::var& args) {
        if (args["name"].toString().trim().isEmpty())
          return fail(CommandError::invalidArgs,
                      "preset.save: argument 'name' must not be blank");
        juce::String whyNot;
        const juce::var saved = processor.savePreset(args["name"].toString(), &whyNot);
        if (!saved.isObject())
          return fail(CommandError::failed,
                      whyNot.isNotEmpty() ? whyNot : juce::String("could not write the preset file"));
        return CommandResult::success(withProperties(
            saved, {{"revision", static_cast<int>(processor.getCurrentChainRevision())}}));
      },
  });

  commands.add({
      "preset.load",
      "Loads a preset, replacing the chain and the preset-managed parameters, and starts "
      "loading its models in the background (one undoable step for the chain). The id may be "
      "of any setlist's preset (a favourite, a recent); the current setlist only changes "
      "through setlist.select and preset.loadSlot. A block whose model file is missing from "
      "the library comes up failed under its name and the rest of the rig loads.",
      {presetIdParam},
      true,
      CommandPermission::edit,
      [this](const juce::var& args) {
        const juce::String id = args["id"].toString();
        if (!processor.presetExists(id))
          return presetNotFound(id);
        if (!processor.loadPreset(id))
          return fail(CommandError::failed, "preset '" + id + "' could not be read");
        const juce::var chain = processor.getChainState(-1);
        return layoutResult(object({{"preset", chain["preset"]}}));
      },
  });

  // The rest reorganise the library and have nothing of a rig in them: the
  // application's, not a sandbox rig's (see registerLibraryCommands).
  if (scope != Scope::app)
    return;

  commands.add({
      "preset.rename",
      "Renames a preset of the user's and its file; its id and slot stay (factory presets "
      "cannot be renamed).",
      {presetIdParam, P::required("name", T::string, "The new name; must not be blank.")},
      true,
      CommandPermission::edit,
      [this, listResult](const juce::var& args) {
        const juce::String id = args["id"].toString();
        if (!processor.presetExists(id))
          return presetNotFound(id);
        if (!processor.renamePreset(id, args["name"].toString()))
          return fail(CommandError::failed,
                      "could not rename preset '" + id + "' (factory preset, or a blank name)");
        return listResult({});
      },
  });

  commands.add({
      "preset.delete",
      "Moves a preset's file to the system trash and leaves its slot empty (factory presets "
      "cannot be deleted); the loaded chain is left as it is.",
      {presetIdParam},
      true,
      CommandPermission::destroy,
      [this, listResult](const juce::var& args) {
        const juce::String id = args["id"].toString();
        if (!processor.presetExists(id))
          return presetNotFound(id);
        if (!processor.deletePreset(id))
          return fail(CommandError::failed,
                      "could not delete preset '" + id + "' (factory presets cannot be deleted)");
        return listResult({{"deletedId", id}});
      },
  });

  commands.add({
      "preset.move",
      "Moves a preset up or down among the presets of the current setlist: the slots in use "
      "stay the same and the presets trade places in them, which also sets the order MIDI "
      "program changes follow. preset.moveSlot moves one to a slot by number.",
      {presetIdParam,
       P::required("delta", T::integer, "Steps to move: negative is earlier, positive later.")
           .range(-1000, 1000)},
      true,
      CommandPermission::edit,
      [this, listResult](const juce::var& args) {
        const juce::String id = args["id"].toString();
        if (!presetExists(processor.getPresetList(), id))
          return presetNotFound(id);
        if (!processor.movePreset(id, args["delta"]))
          return fail(CommandError::failed,
                      "preset '" + id + "' cannot move that way (already at the end of the "
                      "setlist, a zero delta, or a factory preset)");
        return listResult({});
      },
  });
}

// ####################
// MIDI MAP
// ####################
void EngineCommands::registerMidiCommands() {
  const P targetParam = P::required(
      "targetId", T::string,
      "What the control drives: a global parameter id, block1Power to block64Power (the Nth "
      "tone block of the left lane), rightBlock1Power and up (right lane), stereoEnabled, "
      "presetPrevious or presetNext.");

  commands.add({
      "midi.getMap",
      "Returns the MIDI map: the channel filter (0 is omni) and each mapping's target, source "
      "(cc or note) and number.",
      {},
      false,
      CommandPermission::read,
      [this](const juce::var&) {
        return CommandResult::success(processor.midiMapper.getState());
      },
  });

  commands.add({
      "midi.setCc",
      "Maps a MIDI CC number to a target, replacing any mapping that target already had.",
      {targetParam, P::required("cc", T::integer, "Controller number.").range(0, 127)},
      true,
      CommandPermission::edit,
      [this](const juce::var& args) {
        const juce::String target = args["targetId"].toString();
        if (!processor.midiMapper.setCcMapping(target, args["cc"]))
          return fail(CommandError::invalidArgs,
                      "midi.setCc: argument 'targetId' is not a mappable target ('" + target +
                          "')");
        return CommandResult::success(processor.midiMapper.getState());
      },
  });

  commands.add({
      "midi.remove",
      "Removes the mapping of one target.",
      {targetParam},
      true,
      CommandPermission::edit,
      [this](const juce::var& args) {
        const juce::String target = args["targetId"].toString();
        if (!processor.midiMapper.removeMapping(target))
          return fail(CommandError::notFound, "no MIDI mapping for target '" + target + "'");
        return CommandResult::success(processor.midiMapper.getState());
      },
  });

  commands.add({
      "midi.setChannelFilter",
      "Limits MIDI control (mappings and program changes) to one channel, or to all of them.",
      {P::required("channel", T::integer, "1 to 16 for one channel, 0 for omni.").range(0, 16)},
      true,
      CommandPermission::edit,
      [this](const juce::var& args) {
        processor.midiMapper.setChannelFilter(args["channel"]);
        return CommandResult::success(processor.midiMapper.getState());
      },
  });
}

// ####################
// PLUGINS
// ####################
// True when one of the path's steps is "..".
static bool hasParentStep(const juce::String& path) {
  juce::StringArray steps;
  steps.addTokens(path, "/\\", {});
  return steps.contains("..");
}

void EngineCommands::registerPluginCommands() {
  const auto scanStatus = [] {
    const PluginScanner& scanner = PluginScanner::instance();
    const PluginScanner::ScanStatus status = scanner.status();
    // A scanner that was never asked still knows whether it could scan.
    const bool available = scanner.isAvailable();
    return object({
        {"state", !available ? "unavailable" : PluginScanner::stateToString(status.state)},
        {"available", available},
        {"currentFile", status.currentFile},
        {"done", status.done},
        {"total", status.total},
        {"scanned", status.scanned},
        {"failed", status.failed},
    });
  };

  const auto folderList = [] {
    juce::Array<juce::var> folders;
    for (const auto& folder : PluginScanner::instance().userFolders())
      folders.add(folder);
    return juce::var(folders);
  };

  // What this build can scan. The reference text below spells the names out
  // instead of listing these, so docs/commands.md reads the same whichever
  // platform generated it.
  juce::StringArray formatNames;
  for (auto* format : hostedPluginFormats().getFormats())
    formatNames.add(format->getName());

  commands.add({
      "plugins.scan",
      "Starts a scan for VST3 and Audio Unit plugins in the background, replacing one that is "
      "running, and returns the scan status; each plugin is scanned in a helper process, so one "
      "that crashes or hangs is blacklisted with the reason instead of taking the app down, and "
      "files that have not changed since the last scan are not scanned again. Answers "
      "unavailable when the helper executable is not installed.",
      {P::optional("folders", T::array,
                   "Scan these folders only (absolute paths) instead of the formats' default "
                   "locations plus the added folders; Audio Units, which have no folders, are "
                   "then skipped unless formats names them.")
           .items(T::string)
           .path(),
       P::optional("files", T::array,
                   "Scan exactly these plugin files and search nowhere: absolute paths (a "
                   ".vst3) with no .. in them, which for a command server client must lie "
                   "inside the allowed folders. A local caller may also give Audio Unit "
                   "identifiers (AudioUnit:Effects/aufx,dely,appl). Takes precedence over "
                   "folders.")
           .items(T::string)
           .path(),
       P::optional("formats", T::array,
                   "Formats to scan, by name: VST3, and AudioUnit on macOS. Default: every "
                   "format this build hosts.")
           .items(T::string),
       P::optional("rescanAll", T::boolean,
                   "Scan unchanged files again as well, the blacklisted ones included.")
           .withDefault(false),
       P::optional("waitMs", T::integer,
                   "Wait up to this long for the scan to finish before answering (for scripts); "
                   "the scan carries on in the background if it takes longer.")
           .range(0, 3600000)
           .withDefault(0)},
      true,
      CommandPermission::settings,
      [scanStatus, formatNames](const juce::var& args) {
        PluginScanner::ScanOptions options;
        if (const auto* folders = args["folders"].getArray())
          for (const auto& folder : *folders) {
            const juce::String path = folder.toString();
            if (!juce::File::isAbsolutePath(path))
              return fail(CommandError::invalidArgs,
                          "plugins.scan: argument 'folders' must hold absolute paths (got '" +
                              path + "')");
            options.folders.add(path);
          }
        if (const auto* files = args["files"].getArray())
          for (const auto& file : *files) {
            const juce::String entry = file.toString();
            // An Audio Unit is named, not located: its identifier is the one
            // thing here that is not a path. (A command server client never
            // gets this far with one: the agent layer lets only absolute
            // paths inside the allowed folders through.)
            const bool identifier = entry.startsWith("AudioUnit:");
            // What is handed to the scan helper is a file, said in full:
            // nothing relative to wherever the application happens to run,
            // and nothing that climbs.
            if (!identifier && !juce::File::isAbsolutePath(entry))
              return fail(CommandError::invalidArgs,
                          "plugins.scan: argument 'files' must hold absolute paths (got '" +
                              entry + "')");
            if (!identifier && hasParentStep(entry))
              return fail(CommandError::invalidArgs,
                          "plugins.scan: argument 'files' must not hold paths with '..' in "
                          "them (got '" + entry + "')");
            options.files.add(entry);
          }
        if (const auto* formats = args["formats"].getArray())
          for (const auto& format : *formats) {
            if (!formatNames.contains(format.toString()))
              return fail(CommandError::invalidArgs,
                          "plugins.scan: this build does not host the format '" +
                              format.toString() + "'");
            options.formats.add(format.toString());
          }
        options.rescanAll = static_cast<bool>(args["rescanAll"]);

        PluginScanner& scanner = PluginScanner::instance();
        if (!scanner.startScan(options))
          return fail(CommandError::unavailable,
                      "the plugin scan helper (" + scanner.helperFile().getFileName() +
                          ") is not installed next to this binary; plugins cannot be scanned");
        if (const int waitMs = static_cast<int>(args["waitMs"]); waitMs > 0)
          scanner.waitForScan(waitMs);
        return CommandResult::success(scanStatus());
      },
  });

  commands.add({
      "plugins.scanStatus",
      "Returns the plugin scan's progress: state (idle, scanning, finished, cancelled or "
      "unavailable), the file being scanned, done and total candidates, and how many needed "
      "scanning and failed.",
      {},
      false,
      CommandPermission::read,
      [scanStatus](const juce::var&) { return CommandResult::success(scanStatus()); },
  });

  commands.add({
      "plugins.cancelScan",
      "Stops a running plugin scan after the plugin it is on and returns the scan status; "
      "what was scanned so far is kept.",
      {},
      true,
      CommandPermission::settings,
      [scanStatus](const juce::var&) {
        PluginScanner::instance().cancelScan();
        return CommandResult::success(scanStatus());
      },
  });

  commands.add({
      "plugins.list",
      "Lists every plugin the scanner knows with its name, vendor, format, category, version, "
      "channels, file and the identifier chain.addPlugin takes, and a status: ok (an effect "
      "that can be added), failed scan, not an effect (instruments and MIDI-only plugins) or "
      "missing (no longer found); also the added folders and the scan status.",
      {},
      false,
      CommandPermission::read,
      [scanStatus, folderList](const juce::var&) {
        juce::Array<juce::var> plugins;
        for (const auto& listed : PluginScanner::instance().list()) {
          const juce::PluginDescription& d = listed.description;
          juce::var entry = object({
              {"identifier", d.createIdentifierString()},
              {"name", d.name},
              {"vendor", d.manufacturerName},
              {"format", d.pluginFormatName},
              {"category", d.category},
              {"version", d.version},
              {"inputChannels", d.numInputChannels},
              {"outputChannels", d.numOutputChannels},
              {"file", d.fileOrIdentifier},
              {"status", PluginScanner::statusToString(listed.status)},
          });
          if (listed.reason.isNotEmpty())
            entry.getDynamicObject()->setProperty("reason", listed.reason);
          plugins.add(entry);
        }
        return ok({{"plugins", plugins}, {"folders", folderList()}, {"scan", scanStatus()}});
      },
  });

  const P folderParam =
      P::required("path", T::string, "The folder, as an absolute path.").path();
  const auto folderCommand = [folderList](const juce::var& args, bool add) {
    const juce::String path = args["path"].toString();
    const juce::String name = add ? "plugins.addFolder" : "plugins.removeFolder";
    if (!juce::File::isAbsolutePath(path))
      return fail(CommandError::invalidArgs,
                  name + ": argument 'path' must be an absolute path (got '" + path + "')");
    const juce::File folder(path);
    if (add && !folder.isDirectory())
      return fail(CommandError::notFound, "folder not found: " + path);
    const bool changed = add ? PluginScanner::instance().addFolder(folder)
                             : PluginScanner::instance().removeFolder(folder);
    if (!add && !changed)
      return fail(CommandError::notFound, "'" + path + "' is not one of the added folders");
    return ok({{"folders", folderList()}, {"changed", changed}});
  };

  commands.add({
      "plugins.addFolder",
      "Adds a folder to search for plugins on top of the formats' default locations (kept "
      "across sessions) and returns the added folders; the next plugins.scan looks there.",
      {folderParam},
      true,
      CommandPermission::settings,
      [folderCommand](const juce::var& args) { return folderCommand(args, true); },
  });

  commands.add({
      "plugins.removeFolder",
      "Removes a folder added with plugins.addFolder and returns the added folders; plugins "
      "that were only found there show as missing after the next plugins.scan.",
      {folderParam},
      true,
      CommandPermission::settings,
      [folderCommand](const juce::var& args) { return folderCommand(args, false); },
  });

  commands.add({
      "chain.addPlugin",
      "Adds a block that hosts one of the scanned effect plugins and returns it; the plugin is "
      "created on the message thread after the command returns, so the block shows as loading "
      "until state.waitUntilLoaded (or a poll of state.get) sees it loaded, and its parameters "
      "(block.listParams) exist from then on. A plugin that will not load leaves a bypassed "
      "placeholder under its name.",
      {P::required("identifier", T::string,
                   "The plugin: an identifier from plugins.list (status ok), or the plugin's "
                   "file path or exact name when that names exactly one effect.")
           .path(/*mayBeName=*/true),
       P::optional("target", T::string,
                   "An empty insert slot's block id (the block takes that position). Default: "
                   "the first empty slot of the lane."),
       P::optional("lane", T::string,
                   "Lane to add to when no target is given; right needs stereo mode. Default: "
                   "the lane last added to (chain.activeSide).")
           .oneOf({"left", "right"}),
       P::optional("row", T::integer,
                   "Grid mode: the row of the slot to put it in, with slot, instead of target "
                   "or lane.")
           .range(0, 3),
       P::optional("slot", T::integer, "Grid mode: the slot in that row.").range(0, 7)},
      true,
      CommandPermission::edit,
      [this](const juce::var& args) {
        juce::String target = args["target"].toString();
        const juce::String lane = args["lane"].toString();
        if (auto error = slotTarget(args, "chain.addPlugin", target))
          return *error;
        const juce::var before = processor.getChainState(-1);
        if (target.isNotEmpty() && lane.isNotEmpty())
          return fail(CommandError::invalidArgs, "chain.addPlugin: give 'target' or 'lane', not both");
        if (target.isNotEmpty()) {
          const auto ref = findBlock(before, target);
          if (!ref.has_value())
            return blockNotFound(target);
          if (ref->isTone)
            return fail(CommandError::failed,
                        "'" + target + "' is not an empty insert slot; a plugin is added to an "
                                       "empty slot");
        }
        if (lane == "right" && !static_cast<bool>(before["stereoEnabled"]))
          return fail(CommandError::failed, "the right lane only exists in stereo mode");

        juce::String lookupError;
        if (!PluginScanner::instance().findEffect(args["identifier"].toString(), &lookupError))
          return fail(CommandError::notFound, lookupError);
        if (lane.isNotEmpty())
          processor.setActiveEditChain(lane);

        const juce::var added =
            processor.addPluginBlock(args["identifier"].toString(), target.toStdString());
        const juce::String blockId = added["blockId"].toString();
        if (blockId.isEmpty())
          return fail(CommandError::failed, added["error"].toString().isNotEmpty()
                                                ? added["error"].toString()
                                                : juce::String("could not add the plugin"));
        return blockResult(blockId);
      },
  });
}

// ####################
// LIVE READINGS
// ####################
void EngineCommands::registerLiveCommands() {
  commands.add({
      "meters.get",
      "Returns the current levels in dB (floor -60): input and output as [left, right], each "
      "block's in and out by block id (with readings, by reading id, for a kind that has "
      "them: see kinds.list), the audio thread's CPU load (0 to 1) and the output's "
      "stereo correlation (-1 to 1). Also rows (in grid mode, per row what entered it and what "
      "it sent, as [left, right]), splits (in grid mode, per split what the splitter sent down "
      "paths A and B as split and what reached the mixer from each as mix), outputs (per output pair in use, its level after the output "
      "strip) and planCpu (total, critical and rows: what the rig's blocks are estimated to "
      "cost, as fractions of one core; critical is the most one thread has to carry with "
      "multi-core on).",
      {},
      false,
      CommandPermission::read,
      [this](const juce::var&) { return CommandResult::success(processor.getMeterLevels()); },
  });

  const auto tempoResult = [this] {
    return ok({
        {"bpm", processor.getTempo()},
        {"effectiveBpm", processor.getEffectiveTempo()},
        {"hostSync", processor.isTempoFromHost()},
    });
  };

  commands.add({
      "tempo.set",
      "Sets the rig's own tempo, which tempo-synced blocks follow whenever the host reports "
      "none (the standalone app, a host without a tempo); a host's tempo always wins.",
      {P::required("bpm", T::number, "Beats per minute.")
           .range(TONE3000Processor::kMinTempoBpm, TONE3000Processor::kMaxTempoBpm)},
      true,
      CommandPermission::edit,
      [this, tempoResult](const juce::var& args) {
        processor.setTempo(static_cast<double>(args["bpm"]));
        return tempoResult();
      },
  });

  commands.add({
      "tempo.tap",
      "Counts one tap of the tempo switch and sets the rig's tempo once the taps give one: the "
      "average of the last intervals (up to four), within 20 to 300 BPM. A pause longer than "
      "3 seconds starts a new count, a tap within 0.2 seconds of the last is dropped as a "
      "bounce, and a single interval more than 30% off the average is left out. tapped is "
      "true when this tap set the tempo (the first tap of a count and a tap that was left "
      "out do not).",
      {P::optional("timeMs", T::number,
                   "When the tap happened, in milliseconds on any clock that moves forward (a "
                   "remote's own timestamp, so network jitter does not blur the tempo). "
                   "Default: now.")
           .atLeast(0.0)},
      true,
      CommandPermission::edit,
      [this, tempoResult](const juce::var& args) {
        const bool tapped = processor.tapTempo(
            args["timeMs"].isVoid() ? -1.0 : static_cast<double>(args["timeMs"]));
        return CommandResult::success(withProperties(tempoResult().value, {{"tapped", tapped}}));
      },
  });

  commands.add({
      "tempo.get",
      "Returns the rig's own tempo as bpm (default 120), plus what the last audio callback "
      "actually ran with: effectiveBpm, and hostSync when that came from the host.",
      {},
      false,
      CommandPermission::read,
      [tempoResult](const juce::var&) { return tempoResult(); },
  });

  commands.add({
      "tuner.setEnabled",
      "Starts or stops feeding the tuner from the raw input; leave it off when not tuning.",
      {P::required("enabled", T::boolean, "True to run the pitch detector.")},
      true,
      CommandPermission::edit,
      [this](const juce::var& args) {
        const bool enabled = static_cast<bool>(args["enabled"]);
        processor.setTunerEnabled(enabled);
        return ok({{"enabled", enabled}});
      },
  });

  commands.add({
      "tuner.get",
      "Returns the tuner's latest reading as { frequency (Hz), confidence (0 to 1), level "
      "(dB) }; meaningful only while the tuner is enabled and audio is running.",
      {},
      false,
      CommandPermission::read,
      [this](const juce::var&) { return CommandResult::success(processor.getTunerReading()); },
  });
}

// ####################
// SCENES
// ####################
void EngineCommands::registerSceneCommands() {
  const juce::StringArray sceneIds = {"A", "B", "C", "D", "E", "F", "G", "H"};
  const auto sceneParam = [&sceneIds](const char* name, const char* description) {
    return P::required(name, T::string, description).oneOf(sceneIds);
  };
  const P blockIdParam = P::required("blockId", T::string, "The tone block.");
  const P paramIdParam = P::required(
      "param", T::string,
      "The parameter: inputGain, outputGain, mix, or a parameter of the block's kind (see "
      "block.listParams).");

  // scene.get's payload plus the revision: what every scene edit answers with.
  const auto scenesResult = [this] {
    return CommandResult::success(withProperties(
        processor.getScenesState(),
        {{"revision", static_cast<int>(processor.getCurrentChainRevision())}}));
  };
  // A scene the preset does not use cannot be edited or heard: failed, with
  // the reason.
  const auto notInUse = [this](const juce::String& letter) -> std::optional<CommandResult> {
    const int index = sceneIndexFromLetter(letter);
    if (index >= processor.getSceneCount())
      return fail(CommandError::failed,
                  "scene " + letter + " is not in use (the preset has " +
                      juce::String(processor.getSceneCount()) + "; see scene.add)");
    return std::nullopt;
  };
  // A scene edit on one block's parameter: the block has to be a tone block
  // (not_found otherwise), and what the engine refuses comes back as failed
  // with its reason.
  const auto paramEdit = [this, scenesResult](const juce::String& blockId,
                                              const std::function<juce::Result()>& edit) {
    if (auto error = toneBlockError(processor.getChainState(-1), blockId))
      return *error;
    const juce::Result result = edit();
    if (result.failed())
      return fail(CommandError::failed, result.getErrorMessage());
    return scenesResult();
  };

  commands.add({
      "scene.get",
      "Returns the rig's scenes: count (how many the preset uses, 1 to 8), active (index 0 "
      "to 7) and activeId (A to H), and for each of the eight places its name, whether it is "
      "in use (inUse), whether it is the active one, blocks (the on/off state of every "
      "tone block by block id) and values (the value of each scene-assigned parameter: a knob "
      "position from 0 to 1 for inputGain, outputGain and mix, the parameter's own unit for a "
      "kind parameter). assigned lists the scene-assigned parameters and expression those set "
      "aside for the expression pedal. A parameter that is not assigned has one value shared "
      "by all scenes.",
      {},
      false,
      CommandPermission::read,
      [scenesResult](const juce::var&) { return scenesResult(); },
  });

  commands.add({
      "scene.select",
      "Makes a scene the active one: its block states and assigned values are put on the "
      "blocks without a click and without muting the chain (blocks fade, a block with "
      "spillover that the scene turns off rings out, values glide), so it is safe at "
      "footswitch speed. Refused for a scene that is not in use. Not an undo step. The scene "
      "active when a rig or preset is saved is the one it opens with.",
      {sceneParam("scene", "The scene to select, A to H.")},
      true,
      CommandPermission::edit,
      [this, scenesResult, notInUse](const juce::var& args) {
        if (auto error = notInUse(args["scene"].toString()))
          return *error;
        processor.selectScene(sceneIndexFromLetter(args["scene"].toString()));
        return scenesResult();
      },
  });

  commands.add({
      "scene.add",
      "Adds the next scene (a preset starts with one and can have up to eight). The new scene "
      "starts as a copy of the active scene's block states and assigned values, with its "
      "default name; the active scene stays the same. Returns the scenes (as scene.get) and "
      "added, the id of the new scene (A to H). Refused when all eight are in use.",
      {},
      true,
      CommandPermission::edit,
      [this, scenesResult](const juce::var&) {
        int added = -1;
        if (const juce::Result result = processor.addScene(&added); result.failed())
          return fail(CommandError::failed, result.getErrorMessage());
        return CommandResult::success(withProperties(
            scenesResult().value, {{"added", sceneLetter(added)}}));
      },
  });

  commands.add({
      "scene.remove",
      "Removes a scene that is in use. The scenes after it move down one place and keep their "
      "names, block states and assigned values; the last place goes back to a fresh scene and "
      "the count goes down by one. The sound does not change: the active scene stays active "
      "wherever it moves to. If the active scene is the one removed, the scene that takes its "
      "place is selected (the new last scene when it was the last). Refused for a scene that "
      "is not in use and for the only scene.",
      {sceneParam("scene", "The scene to remove, A to H.")},
      true,
      CommandPermission::edit,
      [this, scenesResult, notInUse](const juce::var& args) {
        if (auto error = notInUse(args["scene"].toString()))
          return *error;
        if (const juce::Result result =
                processor.removeScene(sceneIndexFromLetter(args["scene"].toString()));
            result.failed())
          return fail(CommandError::failed, result.getErrorMessage());
        return scenesResult();
      },
  });

  commands.add({
      "scene.rename",
      "Names a scene (the default names are Scene A to Scene H).",
      {sceneParam("scene", "The scene to name, A to H."),
       P::required("name", T::string, "The new name; empty restores the default.")},
      true,
      CommandPermission::edit,
      [this, scenesResult, notInUse](const juce::var& args) {
        if (auto error = notInUse(args["scene"].toString()))
          return *error;
        processor.renameScene(sceneIndexFromLetter(args["scene"].toString()),
                              args["name"].toString());
        return scenesResult();
      },
  });

  commands.add({
      "scene.copy",
      "Copies one scene's block states and assigned values onto another, which keeps its "
      "name; copying onto the active scene is heard at once, like a scene switch.",
      {sceneParam("from", "The scene to copy, A to H."),
       sceneParam("to", "The scene to overwrite, A to H.")},
      true,
      CommandPermission::edit,
      [this, scenesResult, notInUse](const juce::var& args) {
        for (const char* name : {"from", "to"})
          if (auto error = notInUse(args[name].toString()))
            return *error;
        processor.copyScene(sceneIndexFromLetter(args["from"].toString()),
                            sceneIndexFromLetter(args["to"].toString()));
        return scenesResult();
      },
  });

  commands.add({
      "scene.reset",
      "Puts a scene back to its starting point: default name, every block on, and each "
      "scene-assigned parameter at the value the active scene has (resetting the active scene "
      "keeps its values).",
      {sceneParam("scene", "The scene to reset, A to H.")},
      true,
      CommandPermission::edit,
      [this, scenesResult, notInUse](const juce::var& args) {
        if (auto error = notInUse(args["scene"].toString()))
          return *error;
        processor.resetScene(sceneIndexFromLetter(args["scene"].toString()));
        return scenesResult();
      },
  });

  commands.add({
      "scene.assignParam",
      "Gives a block parameter a value per scene, starting with its current value in all "
      "eight places; from then on block.setParam changes it in the active scene only and "
      "scene.setParamValue in any scene. With more than one scene in use block.setParam does "
      "this by itself on a parameter's first set, so calling it is only needed to assign "
      "without changing the value. Works for inputGain, outputGain, mix and the "
      "parameters of the block's kind, a hosted plugin's included. Refused for enabled (a "
      "block's on/off state is per scene already), normalize, a parameter that changes the "
      "block's latency (affectsLatency in block.listParams) and one assigned to the "
      "expression pedal.",
      {blockIdParam, paramIdParam},
      true,
      CommandPermission::edit,
      [this, paramEdit](const juce::var& args) {
        const juce::String blockId = args["blockId"].toString();
        return paramEdit(blockId, [&] {
          return processor.assignSceneParam(blockId.toStdString(), args["param"].toString());
        });
      },
  });

  commands.add({
      "scene.unassignParam",
      "Takes a parameter out of the scenes again: the value the active scene has becomes the "
      "one value all scenes share.",
      {blockIdParam, paramIdParam},
      true,
      CommandPermission::edit,
      [this, paramEdit](const juce::var& args) {
        const juce::String blockId = args["blockId"].toString();
        return paramEdit(blockId, [&] {
          return processor.unassignSceneParam(blockId.toStdString(), args["param"].toString());
        });
      },
  });

  commands.add({
      "scene.setParamValue",
      "Sets a scene-assigned parameter's value in one scene without selecting that scene (for "
      "the active scene this is block.setParam). The value is clamped to the parameter's "
      "range.",
      {sceneParam("scene", "The scene to change, A to H."), blockIdParam, paramIdParam,
       P::required("value", T::number,
                   "A knob position from 0 to 1 for inputGain, outputGain and mix; a kind "
                   "parameter's own unit otherwise.")},
      true,
      CommandPermission::edit,
      [this, paramEdit, notInUse](const juce::var& args) {
        if (auto error = notInUse(args["scene"].toString()))
          return *error;
        const juce::String blockId = args["blockId"].toString();
        return paramEdit(blockId, [&] {
          return processor.setSceneParamValue(sceneIndexFromLetter(args["scene"].toString()),
                                              blockId.toStdString(), args["param"].toString(),
                                              static_cast<double>(args["value"]));
        });
      },
  });

  commands.add({
      "expression.assignParam",
      "Sets a block parameter aside for the expression pedal. The pedal itself is not there "
      "yet; what the flag does today is keep the parameter out of the scenes: it is "
      "unassigned from them (the active scene's value stays as the shared one) and "
      "scene.assignParam refuses it, so a scene switch never pulls a pedal-driven value away.",
      {blockIdParam, paramIdParam},
      true,
      CommandPermission::edit,
      [this, paramEdit](const juce::var& args) {
        const juce::String blockId = args["blockId"].toString();
        return paramEdit(blockId, [&] {
          return processor.setParamExpressionAssigned(blockId.toStdString(),
                                                      args["param"].toString(), true);
        });
      },
  });

  commands.add({
      "expression.unassignParam",
      "Clears a parameter's expression pedal flag, so it can be scene-assigned again.",
      {blockIdParam, paramIdParam},
      true,
      CommandPermission::edit,
      [this, paramEdit](const juce::var& args) {
        const juce::String blockId = args["blockId"].toString();
        return paramEdit(blockId, [&] {
          return processor.setParamExpressionAssigned(blockId.toStdString(),
                                                      args["param"].toString(), false);
        });
      },
  });
}

// ####################
// STOMPS AND FOOTSWITCHES
// ####################
void EngineCommands::registerFootswitchCommands() {
  const juce::StringArray stompSwitchIds = {"A", "B", "C", "D", "E", "F", "G", "H"};
  const juce::StringArray allSwitchIds = {"A", "B", "C",      "D",        "E",    "F",
                                          "G", "H", "bankUp", "bankDown", "tempo"};
  const P stompSwitchParam =
      P::required("switch", T::string, "The footswitch, A to H.").oneOf(stompSwitchIds);
  const P anySwitchParam =
      P::required("switch", T::string, "The footswitch: A to H, bankUp, bankDown or tempo.")
          .oneOf(allSwitchIds);

  const auto stompsResult = [this] {
    return CommandResult::success(withProperties(
        processor.getStompsState(),
        {{"revision", static_cast<int>(processor.getCurrentChainRevision())}}));
  };
  const auto stompEdit = [stompsResult](const juce::Result& result) {
    if (result.failed())
      return fail(CommandError::failed, result.getErrorMessage());
    return stompsResult();
  };
  const auto footswitchResult = [this] {
    return CommandResult::success(processor.getFootswitchState());
  };

  commands.add({
      "stomp.get",
      "Returns the rig's stomp assignments, one per footswitch A to H: the blocks the switch "
      "flips (blockIds), its label (empty: named after the first block) and whether it is "
      "momentary. Assignments are part of the rig and are saved with it and in presets.",
      {},
      false,
      CommandPermission::read,
      [stompsResult](const juce::var&) { return stompsResult(); },
  });

  commands.add({
      "stomp.assign",
      "Assigns one or more blocks to a footswitch, replacing what it had; in Stomp mode a "
      "press flips each of them (so one switch can turn a block off and another on).",
      {stompSwitchParam,
       P::required("blockIds", T::array, "The tone blocks the switch flips, at least one.")
           .items(T::string),
       P::optional("label", T::string, "What to call the switch; see stomp.setOptions."),
       P::optional("momentary", T::boolean, "On while held; see stomp.setOptions.")},
      true,
      CommandPermission::edit,
      [this, stompEdit](const juce::var& args) {
        const int footswitch = footswitchIdFromString(args["switch"].toString());
        const juce::var chain = processor.getChainState(-1);
        std::vector<std::string> blockIds;
        for (const auto& id : *args["blockIds"].getArray()) {
          if (auto error = toneBlockError(chain, id.toString()))
            return *error;
          blockIds.push_back(id.toString().toStdString());
        }
        if (blockIds.empty())
          return fail(CommandError::invalidArgs,
                      "stomp.assign: argument 'blockIds' must name at least one block");
        const juce::Result assigned = processor.assignStomp(footswitch, blockIds);
        if (assigned.failed())
          return stompEdit(assigned);
        const juce::String label = args["label"].toString();
        const bool momentary = static_cast<bool>(args["momentary"]);
        return stompEdit(processor.setStompOptions(
            footswitch, args["label"].isVoid() ? nullptr : &label,
            args["momentary"].isVoid() ? nullptr : &momentary));
      },
  });

  commands.add({
      "stomp.unassign",
      "Takes one block off a footswitch, or without blockId clears the switch entirely (its "
      "label and momentary flag too).",
      {stompSwitchParam,
       P::optional("blockId", T::string, "The block to take off; leave out to clear the switch.")},
      true,
      CommandPermission::edit,
      [this, stompEdit](const juce::var& args) {
        return stompEdit(processor.unassignStomp(footswitchIdFromString(args["switch"].toString()),
                                                 args["blockId"].toString().toStdString()));
      },
  });

  commands.add({
      "stomp.setOptions",
      "Sets a footswitch's stomp label and whether it is momentary: a momentary switch flips "
      "its blocks on the press and flips them back on the release (on while held).",
      {stompSwitchParam,
       P::optional("label", T::string,
                   "What to call the switch; empty names it after its first block."),
       P::optional("momentary", T::boolean, "True for on while held, false to latch.")},
      true,
      CommandPermission::edit,
      [this, stompEdit](const juce::var& args) {
        const juce::String label = args["label"].toString();
        const bool momentary = static_cast<bool>(args["momentary"]);
        return stompEdit(processor.setStompOptions(
            footswitchIdFromString(args["switch"].toString()),
            args["label"].isVoid() ? nullptr : &label,
            args["momentary"].isVoid() ? nullptr : &momentary));
      },
  });

  commands.add({
      "footswitch.getState",
      "Returns what a pedalboard view draws: mode (preset, scene, stomp or hybrid), "
      "hybridRows (what the top row A to D and the bottom row E to H do in hybrid mode), bank "
      "and bankCount (preset banks), scenePage, activeScene, sceneCount (scenes in use), bpm, and the eleven switches (A "
      "to H, bankUp, bankDown, tempo), each with its role in the current mode (preset, scene, "
      "stomp, bankUp, bankDown, presetNext, presetPrevious or tapTempo), label, colour (a "
      "hint: the category of a stomp's first block, else the role), lit, held, empty (nothing "
      "behind the switch, also a scene the preset does not use) and its target (scene, presetId and presetIndex, or blockIds and "
      "momentary).",
      {},
      false,
      CommandPermission::read,
      [footswitchResult](const juce::var&) { return footswitchResult(); },
  });

  commands.add({
      "footswitch.press",
      "Presses a footswitch. A to H act by mode: Preset loads preset bank x 8 + switch of the "
      "preset list, Scene selects scene A to H, Stomp flips the switch's assigned blocks, "
      "Hybrid does per row what footswitch.setHybridRows says. bankUp and bankDown move the "
      "preset bank (and, in a hybrid with one scene row, flip that row between scenes A to D "
      "and E to H); where there is nothing to page (Scene and Stomp mode) they step to the "
      "next or previous preset. tempo taps the tempo (see tempo.tap). Follow it with "
      "footswitch.release.",
      {anySwitchParam},
      true,
      CommandPermission::edit,
      [this, footswitchResult](const juce::var& args) {
        processor.footswitchPress(footswitchIdFromString(args["switch"].toString()));
        return footswitchResult();
      },
  });

  commands.add({
      "footswitch.release",
      "Releases a footswitch; a momentary stomp flips its blocks back, nothing else acts on "
      "the release.",
      {anySwitchParam},
      true,
      CommandPermission::edit,
      [this, footswitchResult](const juce::var& args) {
        processor.footswitchRelease(footswitchIdFromString(args["switch"].toString()));
        return footswitchResult();
      },
  });

  commands.add({
      "footswitch.setMode",
      "Sets what the eight footswitches do: preset, scene, stomp or hybrid. A machine-wide "
      "setting, kept across rigs, presets and restarts.",
      {P::required("mode", T::string, "The footswitch mode.")
           .oneOf({"preset", "scene", "stomp", "hybrid"})},
      true,
      CommandPermission::settings,
      [this, footswitchResult](const juce::var& args) {
        processor.setFootswitchMode(footswitchModeFromString(args["mode"].toString()));
        return footswitchResult();
      },
  });

  commands.add({
      "footswitch.setHybridRows",
      "Sets what each row does in hybrid mode: scene, stomp or preset. A stomp row uses its own "
      "switches' assignments. With both rows on scenes the top row is scenes A to D and the "
      "bottom E to H; a single scene row shows A to D and bankUp / bankDown flip it to E to H. "
      "A preset row shows four presets, two show eight, and the bank switches move by that "
      "many. Machine-wide, like the mode.",
      {P::required("top", T::string, "Switches A to D.").oneOf({"scene", "stomp", "preset"}),
       P::required("bottom", T::string, "Switches E to H.").oneOf({"scene", "stomp", "preset"})},
      true,
      CommandPermission::settings,
      [this, footswitchResult](const juce::var& args) {
        processor.setFootswitchHybridRows(
            footswitchRowRoleFromString(args["top"].toString(), FootswitchRowRole::Scene),
            footswitchRowRoleFromString(args["bottom"].toString(), FootswitchRowRole::Stomp));
        return footswitchResult();
      },
  });
}

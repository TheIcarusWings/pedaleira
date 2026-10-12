#pragma once

#include "CommandRegistry.h"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

class AgentHub;
class TONE3000Processor;

// The engine's command set: every way to change or read the rig, as named
// commands over one processor. This is the door for anything that is not the
// native UI (an MCP server, the remote, the headless render tool, tests), and
// the rule is that they add nothing of their own: a transport translates its
// wire format to registry().run and back.
//
// Each command is a thin call into the processor's existing public methods
// (the same ones the UI's ProcessorBackend forwards to), so a rig built
// through commands is the rig the UI would have built. What the commands add
// is what a remote caller cannot do for itself: argument checking, telling
// "no such block" from "the engine refused", and answering each edit with
// the object it changed.
//
// Left out on purpose: OS helpers (clipboard, logs, file browsers), the
// TONE3000 account and tone browsing, audio device selection, and anything
// that needs a person in the loop (MIDI learn, auto balance / align).
//
// Threading follows the processor: construct, use and destroy on the message
// thread. Other threads go through registry().runFromAnyThread.
class EngineCommands {
public:
  // `app` is the command set of the rig the application plays: the rig
  // commands plus the agent layer's (sessions, permissions, sandbox rigs,
  // guitar profiles, renders; see AgentHub.h). `rig` is the rig commands
  // alone, which is what a sandbox rig gets: everything that acts on one
  // processor and nothing that belongs to the application as a whole. The
  // commands that only reorganise the setlist library (rename, move, copy,
  // delete, import, favourites) are the application's too: a sandbox reads
  // the library, loads from it and adds presets to it, no more (see
  // LibraryCommands.cpp and docs/agents.md, "Sandbox rigs and the library").
  enum class Scope { app, rig };
  explicit EngineCommands(TONE3000Processor& processor, Scope scope = Scope::app);
  ~EngineCommands();

  // The agent layer over this command set. Only an app-scope instance has
  // one.
  bool hasAgents() const { return hub != nullptr; }
  AgentHub& agents() { return *hub; }

  // How far a rig's blocks are with loading. One look, message thread.
  struct LoadState {
    // Nothing is loading any more and the rig is audible again.
    bool settled = false;
    juce::Array<juce::var> pendingBlockIds, failedBlockIds;
    juce::var revision;
  };
  static LoadState loadState(TONE3000Processor& processor);
  // What state.waitUntilLoaded does, for any rig and from any thread: looks
  // (on the message thread) every 10 ms until the rig has settled or
  // `timeoutMs` has passed. `cancel`, when given, ends the wait early with
  // unavailable.
  static CommandResult waitUntilLoaded(TONE3000Processor& processor, int timeoutMs,
                                       const std::atomic<bool>* cancel = nullptr);

  CommandRegistry& registry() { return commands; }
  const CommandRegistry& registry() const { return commands; }

  // Asked on the message thread when one of this command set's slow commands
  // starts, for the flag its work reads between steps: once that reads true
  // the work gives up with `unavailable`. The agent layer answers with the
  // calling session's flag (AgentHub::slowWorkCancel), for the rig being
  // played as for a sandbox, so a server thread that is waiting for loads
  // lets go at once when its connection or the application closes. Unset, or
  // answering null: never cancelled.
  std::function<const std::atomic<bool>*()> slowWorkCancel;

  // The markdown reference checked in as docs/commands.md.
  juce::String markdownReference() const;

private:
  void registerStateCommands();
  void registerParameterCommands();
  void registerChainCommands();
  void registerBlockCommands();
  void registerPluginCommands();
  void registerPresetCommands();
  void registerMidiCommands();
  void registerLiveCommands();
  void registerSceneCommands();
  void registerFootswitchCommands();
  // GridCommands.cpp: grid.* and row.*.
  void registerGridCommands();
  // LibraryCommands.cpp: setlist.*, the slot commands of preset.*,
  // library.*, favourite.set and recent.list.
  void registerLibraryCommands();

  // For the legacy routing commands: nothing in a legacy rig; in a grid,
  // converts it back to the legacy rig when it has that shape, else the
  // failure to return, with the reasons.
  std::optional<CommandResult> leaveGridFor(const char* command);
  // The optional `row` and `slot` of the add commands: the id of the block
  // or empty slot they name into `target`, or the failure to return.
  // Nothing happens when neither is given.
  std::optional<CommandResult> slotTarget(const juce::var& args, const char* command,
                                          juce::String& target, int* rowOut = nullptr,
                                          int* slotOut = nullptr);

  juce::var fullState() const;
  juce::var engineInfo(const juce::var& chainState) const;
  juce::var parameterList() const;

  // { revision, stereoEnabled, left: [ids], right: [ids] } plus `extra`'s
  // properties: what a structural edit answers with, so the caller has the
  // new lane orders (insert slots included) without asking again.
  CommandResult layoutResult(const juce::var& extra = {}) const;
  // { revision, blockId, block }: what a per-block edit answers with.
  CommandResult blockResult(const juce::String& blockId) const;

  TONE3000Processor& processor;
  const Scope scope;
  CommandRegistry commands;
  // After the registry: it registers into it and is destroyed first.
  std::unique_ptr<AgentHub> hub;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EngineCommands)
};

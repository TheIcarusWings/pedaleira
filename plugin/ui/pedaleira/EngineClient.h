// The UI's only door to the engine: the command layer, as the local session.
// Every page reads and
// changes the rig through commands, the same ones an agent uses, so nothing
// the UI does is out of an agent's reach and the UI never touches the
// processor.
//
// Message thread only.
#pragma once

#include <juce_core/juce_core.h>

#include <initializer_list>
#include <utility>

class AgentHub;

namespace pedaleira::ui {

struct EngineReply {
  bool ok = false;
  juce::var value;
  juce::String errorCode;
  juce::String errorMessage;
};

class EngineClient {
public:
  virtual ~EngineClient() = default;

  // Runs a command and returns its reply. A slow command's work runs here
  // too, so a page calls those from an action, never from paint.
  virtual EngineReply run(const juce::String& command, const juce::var& args = {}) = 0;

  // The value of a command that cannot fail in normal use (a read), or an
  // empty var when it did; the failure is logged.
  juce::var read(const juce::String& command, const juce::var& args = {});

  // Changes when the rig changes; pages poll it to know when to read again.
  virtual juce::uint32 revision() = 0;
};

// The client the application and the testbed use: the engine's agent hub,
// in process, as its local session (the user: never gated, never queued).
class LocalEngineClient final : public EngineClient {
public:
  explicit LocalEngineClient(AgentHub& hub);

  EngineReply run(const juce::String& command, const juce::var& args = {}) override;
  juce::uint32 revision() override;

private:
  AgentHub& hub;
};

// Builds an arguments object: object({{"blockId", id}, {"value", 0.5}}).
juce::var object(std::initializer_list<std::pair<const char*, juce::var>> properties);

}  // namespace pedaleira::ui

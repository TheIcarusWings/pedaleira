#include "EngineClient.h"

#include "AgentHub.h"

namespace pedaleira::ui {

juce::var EngineClient::read(const juce::String& command, const juce::var& arguments) {
  const EngineReply reply = run(command, arguments);
  if (!reply.ok) {
    DBG("Pedaleira UI: " << command << " failed: " << reply.errorCode << ": " << reply.errorMessage);
    return {};
  }
  return reply.value;
}

LocalEngineClient::LocalEngineClient(AgentHub& agentHub) : hub(agentHub) {}

EngineReply LocalEngineClient::run(const juce::String& command, const juce::var& arguments) {
  const CommandResult result =
      hub.run(hub.localSession(), command, arguments.isVoid() ? juce::var(new juce::DynamicObject()) : arguments);
  return {result.ok, result.value, result.errorCode, result.errorMessage};
}

juce::uint32 LocalEngineClient::revision() { return hub.revisionFor(hub.localSession()).revision; }

juce::var object(std::initializer_list<std::pair<const char*, juce::var>> properties) {
  juce::DynamicObject::Ptr obj = new juce::DynamicObject();
  for (const auto& [key, value] : properties)
    obj->setProperty(key, value);
  return juce::var(obj.get());
}

}  // namespace pedaleira::ui

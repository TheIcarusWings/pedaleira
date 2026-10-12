// Settings, "AI and MCP" (the board "Settings, MCP"): the command server an
// agent connects to (server.status, server.setEnabled), its address and the
// setup snippet for an agent, the clients connected (agents.list,
// agents.disconnect), what agents may do (permissions.get and set), how far
// the captures and IRs are tagged (assets.tagSummary) and the guitar profiles
// (guitar.list). Adding a guitar is the wizard of UI step 9. The connection
// token is never shown: it stays in the connection file.
#pragma once

#include "SettingsParts.h"

#include <memory>
#include <vector>

namespace pedaleira::ui {

class SettingsAgents : public SettingsSection {
public:
  explicit SettingsAgents(EngineClient& engine);

  void refresh() override;
  void tick() override;
  int heightFor(int width) const override;

  // What the Copy buttons put on the clipboard.
  juce::String endpointText() const;
  juce::String setupText() const;
  // The switch and the buttons, for the testbed and tests.
  void setServerEnabled(bool enabled);
  void setPermission(const juce::String& key, bool allowed);
  void disconnect(int clientId);
  int clientCount() const { return static_cast<int>(clients.size()); }

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  struct Client {
    int id = 0;
    juce::String name, connected, lastCommand, lastTime;
  };
  struct Guitar {
    juce::String name, line, summary;
    bool ready = false;
  };
  struct Layout {
    juce::Rectangle<int> server, agents, tagging, permissions, guitars;
    juce::Rectangle<int> serverSwitch, status, endpointLabel, endpoint, copy, setup, serverNote;
    juce::Rectangle<int> agentHead, agentEmpty;
    std::vector<juce::Rectangle<int>> agentRows, guitarRows, permissionRows;
    juce::Rectangle<int> taggingNote, captures, irs, tagList, guitarNote, addGuitar, guitarEmpty;
    int height = 0;
  };

  Layout layoutFor(int width) const;
  void readServer();
  void rebuildClients();
  void copyToClipboard(const juce::String& text, PushButton& button);
  bool running() const { return static_cast<bool>(server["running"]); }
  juce::String tagListText() const;

  juce::var server, tags;
  std::vector<Client> clients;
  std::vector<Guitar> guitars;
  juce::StringArray permissionKeys;
  Layout lay;
  int ticks = 0;
  juce::int64 copiedAt = 0;
  PushButton* copiedButton = nullptr;

  SettingToggle enableServer{"Enable MCP server"};
  juce::TextEditor endpoint;
  PushButton copyEndpoint{"Copy"};
  PushButton copySetup{"Copy setup for my agent"};
  std::vector<std::unique_ptr<PushButton>> disconnectButtons;
  std::vector<std::unique_ptr<SettingToggle>> permissionToggles;
  PushButton addGuitar{"Add a guitar"};
};

}  // namespace pedaleira::ui

// Settings, "Capture folders": the folders Pedaleira looks in for NAM captures
// and IRs on top of the library (roots.list, roots.add, roots.remove; kept
// across restarts by the engine). What is found shows in the block library,
// by name; the captures downloaded from TONE3000 are in the library itself.
#pragma once

#include "SettingsParts.h"

#include <memory>
#include <vector>

namespace pedaleira::ui {

class SettingsCaptures : public SettingsSection {
public:
  explicit SettingsCaptures(EngineClient& engine);

  void refresh() override;
  int heightFor(int width) const override;

  // Testbed and tests: what the buttons do.
  void addFolderPath(const juce::String& path);
  void removeFolderPath(const juce::String& path);
  int folderCount() const { return folders.size(); }

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  struct Layout {
    juce::Rectangle<int> mine, addFolder, note;
    std::vector<juce::Rectangle<int>> folderRows;
    int height = 0;
  };

  Layout layoutFor(int width) const;
  void rebuildControls();
  void chooseFolder();

  juce::StringArray folders;
  Layout lay;

  PushButton addFolderButton{"Add folder"};
  std::vector<std::unique_ptr<IconButton>> folderButtons;
  std::unique_ptr<juce::FileChooser> chooser;
};

}  // namespace pedaleira::ui

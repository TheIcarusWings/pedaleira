// Settings, "Plugins" (the board "Settings, plugins"): the scan of VST3 and
// Audio Unit plugins (plugins.scan, cancelScan, scanStatus), the folders that
// are searched on top of the defaults, and the plugins found with their
// status. The page polls the scan from the Settings page's timer, which only
// runs while Settings is on screen. "In picker" and a plugin's latency are not
// in the engine, so the table leaves them out.
#pragma once

#include "SettingsParts.h"

#include <memory>
#include <vector>

namespace pedaleira::ui {

class SettingsPlugins : public SettingsSection, private juce::TextEditor::Listener {
public:
  explicit SettingsPlugins(EngineClient& engine);
  ~SettingsPlugins() override;

  void refresh() override;
  // Follows a running scan; does nothing when none is running.
  void tick() override;
  int heightFor(int width) const override;

  // Testbed and tests: what the buttons and the box do.
  void startScan(bool rescanAll);
  void stopScan();
  void addFolderPath(const juce::String& path);
  void removeFolderPath(const juce::String& path);
  void setSearch(const juce::String& text);
  // Shows a scan status as if the engine had just reported it.
  void showScan(const juce::var& status);
  int shownCount() const { return static_cast<int>(shown.size()); }
  int foundCount() const { return static_cast<int>(found.size()); }

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  struct Entry {
    juce::String name, vendor, format, channels, status, reason;
  };
  struct Layout {
    juce::Rectangle<int> scan, statusColumn, formatsColumn, foldersColumn, bar, buttonRow;
    juce::Rectangle<int> plugins, searchBox, tableHead, footer;
    std::vector<juce::Rectangle<int>> formatRows, folderRows;
    juce::Rectangle<int> addFolder, folderNote;
    int tableTop = 0;
    int height = 0;
  };

  void textEditorTextChanged(juce::TextEditor&) override { applyFilter(); }

  Layout layoutFor(int width) const;
  bool scanning() const { return scan["state"].toString() == "scanning"; }
  bool available() const { return scan["state"].toString() != "unavailable"; }
  void applyFilter();
  void rebuildControls();
  void chooseFolder();
  void paintRow(juce::Graphics& g, const Entry& entry, juce::Rectangle<int> row);

  std::vector<Entry> found;
  std::vector<int> shown;
  juce::StringArray folders;
  juce::StringArray formatNames, enabledFormats;
  juce::var scan;
  Layout lay;

  juce::TextEditor search;
  PushButton scanButton{"Scan for plugins", PushButton::Kind::primary};
  PushButton rescanButton{"Rescan all"};
  PushButton addFolderButton{"Add folder"};
  std::vector<std::unique_ptr<SettingToggle>> formatToggles;
  std::vector<std::unique_ptr<IconButton>> folderButtons;
  std::unique_ptr<juce::FileChooser> chooser;
};

}  // namespace pedaleira::ui

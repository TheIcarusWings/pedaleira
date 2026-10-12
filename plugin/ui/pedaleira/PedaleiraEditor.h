// The standalone application's window: the Pedaleira UI
// over the processor's own command layer. Laid out in the design's
// 1440 x 900 space and scaled as one piece, so the window can be any size
// with the board's proportions.
//
// The plugin formats keep upstream's editor until the Pedaleira one covers
// what they need (standalone first); PEDALEIRA_CLASSIC_UI=1 in
// the environment brings upstream's editor back in the standalone app too,
// for comparing.
#pragma once

#include "DeviceSettings.h"
#include "EngineClient.h"
#include "pages/ComingSoon.h"
#include "pages/CapturePage.h"
#include "pages/FootswitchModesPage.h"
#include "pages/GigView.h"
#include "pages/GridPage.h"
#include "pages/ImportPage.h"
#include "pages/LibraryPage.h"
#include "pages/SettingsPage.h"
#include "PluginWindow.h"
#include "shell/AppShell.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>
#include <vector>

class TONE3000Processor;

namespace pedaleira::ui {

class PedaleiraEditor : public juce::AudioProcessorEditor {
public:
  explicit PedaleiraEditor(TONE3000Processor& processor);
  ~PedaleiraEditor() override;

  // Whether the standalone application should open this editor.
  static bool wanted(const juce::AudioProcessor& processor);

  // Opens a hosted plugin block's own editor window (or brings it to the
  // front). False when the block has none.
  bool showPluginWindow(const juce::String& blockId, const juce::String& title);

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  std::unique_ptr<LocalEngineClient> client;
  // The audio device of the standalone application; null when there is none.
  std::unique_ptr<DeviceSettings> device;
  std::unique_ptr<AppShell> shell;
  std::unique_ptr<GridPage> grid;
  std::unique_ptr<LibraryPage> library;
  std::unique_ptr<ImportPage> importer;
  std::unique_ptr<CapturePage> capturePage;
  std::unique_ptr<SettingsPage> settingsPage;
  std::unique_ptr<FootswitchModesPage> modes;
  std::unique_ptr<GigView> gig;
  std::vector<std::unique_ptr<juce::Component>> comingSoon;
  PluginWindows pluginWindows;
};

}  // namespace pedaleira::ui

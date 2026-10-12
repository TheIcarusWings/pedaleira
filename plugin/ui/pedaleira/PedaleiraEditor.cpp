#include "PedaleiraEditor.h"

#include "AgentHub.h"
#include "EngineCommands.h"
#include "Processor.h"

#if PEDALEIRA_STANDALONE_DEVICE
#include "standalone/StandaloneDevice.h"
#endif

namespace pedaleira::ui {

namespace {
constexpr int kMinWidth = 1120;
constexpr double kAspect = static_cast<double>(size::windowWidth) / size::windowHeight;
}  // namespace

bool PedaleiraEditor::wanted(const juce::AudioProcessor& processor) {
  if (processor.wrapperType != juce::AudioProcessor::wrapperType_Standalone)
    return false;
  return juce::SystemStats::getEnvironmentVariable("PEDALEIRA_CLASSIC_UI", {}) != "1";
}

PedaleiraEditor::PedaleiraEditor(TONE3000Processor& processor)
    : juce::AudioProcessorEditor(processor), pluginWindows(processor) {
  client = std::make_unique<LocalEngineClient>(processor.commandLayer().agents());
  shell = std::make_unique<AppShell>(*client);
  grid = std::make_unique<GridPage>(*client);
  shell->setPageComponent(Page::grid, grid.get());
  library = std::make_unique<LibraryPage>(*client);
  importer = std::make_unique<ImportPage>(*client);
  shell->setPageComponent(Page::library, library.get());
  shell->setPageComponent(Page::import, importer.get());
#if PEDALEIRA_STANDALONE_DEVICE
  device = StandaloneDevice::create(processor);
#endif
  settingsPage = std::make_unique<SettingsPage>(*client, device.get());
  shell->setPageComponent(Page::settings, settingsPage.get());
  const std::pair<Page, const char*> pending[] = {
      {Page::capture, "Capture"},
      {Page::live, "Live"},
  };
  for (const auto& [page, name] : pending) {
    comingSoon.push_back(std::make_unique<ComingSoon>(name, "This page comes in a later build."));
    shell->setPageComponent(page, comingSoon.back().get());
  }
  modes = std::make_unique<FootswitchModesPage>(*client);
  gig = std::make_unique<GigView>(*client);
  shell->setModesView(modes.get());
  shell->setGigView(gig.get());
  grid->onOpenModes = [this] {
    modes->refresh();
    shell->showModesView(true);
    modes->grabKeyboardFocus();
  };
  modes->onBack = [this] { shell->showModesView(false); };
  gig->onExit = [this] { shell->showGigView(false); };
  shell->onRigChanged = [this] {
    grid->rigChanged();
    if (shell->isModesViewShown())
      modes->refresh();
    if (shell->currentPage() == Page::library)
      library->refresh();
    else if (shell->currentPage() == Page::settings)
      settingsPage->rigChanged();
  };
  shell->onPageShown = [this](Page page) {
    if (page == Page::library)
      library->refresh();
    else if (page == Page::import)
      importer->refresh();
    else if (page == Page::settings)
      settingsPage->refresh();
  };
  // A preset was loaded, saved or moved: the header and the grid read it again.
  library->onChanged = [this] {
    shell->header().refresh();
    grid->rigChanged();
  };
  importer->onChanged = [this] {
    shell->header().refresh();
    grid->rigChanged();
  };
  importer->onOpenGrid = [this] { shell->showPage(Page::grid); };
  importer->onPickCapture = [this](int row, int slot) {
    shell->showPage(Page::grid);
    grid->openPicker(row, slot);
  };
  grid->onOpenPluginWindow = [this](const juce::String& blockId, const juce::String& title) {
    return showPluginWindow(blockId, title);
  };
  shell->showPage(Page::grid);
  shell->setSize(size::windowWidth, size::windowHeight);
  addAndMakeVisible(*shell);

  setResizable(true, true);
  setResizeLimits(kMinWidth, juce::roundToInt(kMinWidth / kAspect), size::windowWidth * 2, size::windowHeight * 2);
  getConstrainer()->setFixedAspectRatio(kAspect);
  setSize(size::windowWidth, size::windowHeight);
}

PedaleiraEditor::~PedaleiraEditor() {
  pluginWindows.closeAll();
  // The pages go before the shell that shows them.
  shell->onRigChanged = nullptr;
  shell->onPageShown = nullptr;
  library->onChanged = nullptr;
  importer->onChanged = nullptr;
  importer->onOpenGrid = nullptr;
  importer->onPickCapture = nullptr;
  grid->onOpenPluginWindow = nullptr;
  grid->onOpenModes = nullptr;
  // The tuner stops listening with the Gig View.
  shell->showGigView(false);
  removeAllChildren();
  gig.reset();
  modes.reset();
  importer.reset();
  // The page lets go of the device before the device goes.
  settingsPage.reset();
  device.reset();
  library.reset();
  grid.reset();
  comingSoon.clear();
  shell.reset();
}

bool PedaleiraEditor::showPluginWindow(const juce::String& blockId, const juce::String& title) {
  return pluginWindows.show(blockId, title);
}

void PedaleiraEditor::paint(juce::Graphics& g) { g.fillAll(colour::ground); }

void PedaleiraEditor::resized() {
  // One scale for the whole UI, from the design's 1440 x 900.
  const float scale = getWidth() / static_cast<float>(size::windowWidth);
  shell->setTransform(juce::AffineTransform::scale(scale));
}

}  // namespace pedaleira::ui

#include "PluginWindow.h"

#include "HostedPluginBlockProcessor.h"
#include "Processor.h"
#include "Tokens.h"

namespace pedaleira::ui {

namespace {
// Where each block's window was last, for this session.
std::map<juce::String, juce::Point<int>>& lastPositions() {
  static std::map<juce::String, juce::Point<int>> positions;
  return positions;
}

constexpr int kWatchMs = 500;
}  // namespace

std::unique_ptr<PluginWindow> PluginWindow::open(TONE3000Processor& processor, const juce::String& blockId,
                                                 const juce::String& title) {
  auto instance = processor.hostedPluginInstance(blockId.toStdString());
  if (instance == nullptr)
    return nullptr;
  auto editor = instance->createEditor();
  if (editor == nullptr)
    return nullptr;
  return std::unique_ptr<PluginWindow>(
      new PluginWindow(processor, blockId, title, std::move(instance), std::move(editor)));
}

PluginWindow::PluginWindow(TONE3000Processor& owner, const juce::String& id, const juce::String& title,
                           std::shared_ptr<HostedPluginInstance> hosted,
                           std::unique_ptr<juce::AudioProcessorEditor> editor)
    : juce::DocumentWindow(title, colour::surface, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
      processor(owner), blockId(id), instance(std::move(hosted)) {
  setUsingNativeTitleBar(true);
  const bool resizable = editor->isResizable();
  setContentOwned(editor.release(), true);
  setResizable(resizable, false);

  const auto& positions = lastPositions();
  if (const auto found = positions.find(blockId); found != positions.end())
    setTopLeftPosition(found->second);
  else
    centreWithSize(getWidth(), getHeight());
  setVisible(true);
  toFront(true);
  startTimer(kWatchMs);
}

PluginWindow::~PluginWindow() {
  stopTimer();
  // The editor goes while the instance it draws is still held (the base
  // class would delete it after this class's members).
  clearContentComponent();
}

void PluginWindow::closeButtonPressed() {
  if (closing)
    return;
  closing = true;
  stopTimer();
  setVisible(false);
  if (onClose)
    onClose(*this);
}

void PluginWindow::moved() {
  juce::DocumentWindow::moved();
  if (isVisible())
    lastPositions()[blockId] = getPosition();
}

void PluginWindow::timerCallback() {
  // The block was removed or no longer runs this instance (replaced, a
  // preset or an undo rebuilt it): this editor shows nothing that plays.
  if (processor.hostedPluginInstance(blockId.toStdString()) != instance)
    closeButtonPressed();
}

PluginWindows::PluginWindows(TONE3000Processor& owner) : processor(owner) {}

PluginWindows::~PluginWindows() { closeAll(); }

bool PluginWindows::show(const juce::String& blockId, const juce::String& title) {
  if (const auto found = windows.find(blockId); found != windows.end()) {
    if (!found->second.window->isClosing()) {
      found->second.window->setVisible(true);
      found->second.window->toFront(true);
      return true;
    }
    // On its way out (its delete is queued): it goes now, this is not one
    // of its own callbacks.
    windows.erase(found);
  }
  auto window = PluginWindow::open(processor, blockId, title);
  if (window == nullptr)
    return false;
  // Deleted on the next message loop turn: the request to close comes from
  // inside the window's own callbacks. The serial tells that window from a
  // later one of the same block.
  const int serial = ++lastSerial;
  window->onClose = [this, alive = std::weak_ptr<bool>(token), serial](PluginWindow& closed) {
    juce::MessageManager::callAsync([this, alive, serial, key = closed.block()] {
      if (alive.lock() == nullptr)
        return;
      if (const auto found = windows.find(key); found != windows.end() && found->second.serial == serial)
        windows.erase(found);
    });
  };
  windows[blockId] = Open{serial, std::move(window)};
  return true;
}

void PluginWindows::closeAll() { windows.clear(); }

}  // namespace pedaleira::ui

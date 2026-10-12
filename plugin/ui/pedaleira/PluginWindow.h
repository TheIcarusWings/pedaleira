// A hosted plugin's own editor in a window of its own (the board "Block
// editor, plugin", right). The one part of the UI that is not a command: an
// editor is the plugin's code drawing itself, so the window holds the plugin
// instance (HostedPluginInstance) for as long as it is open, and closes by
// itself once the block no longer runs that instance (removed, replaced, a
// preset loaded). Its position is remembered per block for the session.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <map>
#include <memory>

class HostedPluginInstance;
class TONE3000Processor;

namespace pedaleira::ui {

class PluginWindow : public juce::DocumentWindow, private juce::Timer {
public:
  // Null when the block is not a running plugin or the plugin has no editor
  // (or one is open already).
  static std::unique_ptr<PluginWindow> open(TONE3000Processor& processor, const juce::String& blockId,
                                            const juce::String& title);
  ~PluginWindow() override;

  const juce::String& block() const noexcept { return blockId; }
  bool isClosing() const noexcept { return closing; }

  // Asked to close (its close button, or the block went away); the owner
  // deletes the window, later than this call.
  std::function<void(PluginWindow&)> onClose;

  void closeButtonPressed() override;
  void moved() override;

private:
  PluginWindow(TONE3000Processor& processor, const juce::String& blockId, const juce::String& title,
               std::shared_ptr<HostedPluginInstance> instance,
               std::unique_ptr<juce::AudioProcessorEditor> editor);
  void timerCallback() override;

  TONE3000Processor& processor;
  const juce::String blockId;
  // Declared before the editor so it is released after it.
  std::shared_ptr<HostedPluginInstance> instance;
  bool closing = false;
};

// The windows open at a time, one per block.
class PluginWindows {
public:
  explicit PluginWindows(TONE3000Processor& processor);
  ~PluginWindows();

  // Opens the block's plugin window, or brings it to the front when it is
  // open. False when the block has no plugin editor to show.
  bool show(const juce::String& blockId, const juce::String& title);
  bool isOpen(const juce::String& blockId) const { return windows.count(blockId) > 0; }
  void closeAll();

private:
  TONE3000Processor& processor;
  struct Open {
    int serial = 0;
    std::unique_ptr<PluginWindow> window;
  };
  std::map<juce::String, Open> windows;
  int lastSerial = 0;
  // Lets a deferred delete see that this object is gone.
  std::shared_ptr<bool> token = std::make_shared<bool>(true);
};

}  // namespace pedaleira::ui

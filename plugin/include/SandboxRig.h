#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <atomic>
#include <memory>

class EngineCommands;
class TONE3000Processor;

// A second rig, for work that must not be heard: a whole processor of its own
// with the engine's command set over it, never connected to an audio device.
// An agent builds and edits a tone here, renders clips through it offline to
// measure the result, and only the finished rig is copied onto the one being
// played (SandboxRig::applyTo), as a single undoable step.
//
// A second processor rather than a flag on the live one because nothing short
// of that keeps the live audio bit-identical: the sandbox has its own blocks,
// models, parameters, scenes and undo history, and shares only what is global
// anyway (the library, the model stash, the plugin scanner).
//
// Message thread for everything except OfflineRender::render's block loop,
// which stands in for the audio thread of a rig that has none.
class SandboxRig {
public:
  // An empty rig that is a guest of `live`'s library: it loads from it and
  // adds presets to it (TONE3000Processor::sharePresetStoreWith).
  SandboxRig(const juce::String& rigId, TONE3000Processor& live);
  ~SandboxRig();

  TONE3000Processor& processor() { return *rig; }
  EngineCommands& commands() { return *rigCommands; }

  // Makes this rig a copy of `source`: its whole saved state (blocks with
  // their models, scenes, parameters, input mode, oversampling), the way a
  // host project restores it.
  void copyStateFrom(TONE3000Processor& source);
  // Replaces `target`'s rig with this one, as loading a preset would: the
  // chain and the preset-managed parameters, one undo step on the target.
  // Session settings (input mode, oversampling, MIDI map) stay the target's.
  bool applyTo(TONE3000Processor& target);

  // The rig as applyTo would carry it over, taken now and independent of
  // this sandbox from then on: what a request to apply the sandbox is
  // approved as, so that edits made after the user was asked (or the sandbox
  // being discarded) change nothing about what gets applied.
  struct Snapshot {
    juce::ValueTree tree;
    juce::String presetId, presetName;
    // The sandbox had been edited since it loaded or saved that preset.
    bool edited = false;
    juce::uint32 revision = 0;
  };
  Snapshot capture();
  static bool applySnapshot(const Snapshot& snapshot, TONE3000Processor& target);

  const juce::String id;
  juce::String name;
  // The session that made it (0: a local caller); its rigs go when it does.
  int ownerSession = 0;
  juce::Time created;
  // Made by a render of the live rig for that one render, then discarded;
  // does not count against the limit and is not listed.
  bool temporary = false;

  // Set while an offline render runs through the rig: one at a time, and no
  // edits meanwhile, or two calls with the same arguments could differ.
  std::atomic<bool> rendering{false};
  // Slow commands in flight that hold a reference to this rig from another
  // thread; it is not discarded while this is above zero.
  std::atomic<int> activeUses{0};
  // Its owner has gone while it was in use: whoever ends the last use has it
  // discarded (AgentHub::discardSandboxSoon).
  std::atomic<bool> orphaned{false};

private:
  std::unique_ptr<TONE3000Processor> rig;
  std::unique_ptr<EngineCommands> rigCommands;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SandboxRig)
};

// Runs audio through a rig that no device is driving. The caller may be any
// thread: preparing and waiting for loads hop to the message thread, the
// block loop runs on the calling thread.
namespace OfflineRender {

struct Result {
  // Two channels, the input's length plus the tail, lined up with the input
  // (the rig's reported latency is rendered extra and dropped).
  juce::AudioBuffer<float> audio;
  int latencySamples = 0;
  // Non-empty when the render did not happen.
  juce::String error;
};

// `input` has one or two channels. `cancel` is checked between blocks.
Result render(TONE3000Processor& rig, const juce::AudioBuffer<float>& input, double sampleRate,
              double tailSeconds, const std::atomic<bool>& cancel);

constexpr int kBlockSize = 128;
constexpr int kLoadTimeoutMs = 60000;

}  // namespace OfflineRender

#include "EngineCommands.h"

// Processor.h first: it pulls in the JUCE module headers, which must come
// before anything that drags in <windows.h> (see EngineCommands.cpp).
#include "Processor.h"

// ####################
// LOOPER
// ####################
// looper.*: the loop at the end of the main output (Looper.h, docs/looper.md).
// Every action command posts its action to the looper's lock-free ring and
// answers with the looper's state as the audio thread last published it. The
// audio thread applies the action at the start of its next block, so the
// state in the answer does not show it yet (`queued` counts what is waiting);
// poll looper.get. With no audio running (a headless rig between renders) the
// action waits for the next block.

namespace {

using P = CommandParam;
using T = CommandParamType;

CommandResult fail(const char* code, const juce::String& message) {
  return CommandResult::failure(code, message);
}

}  // namespace

void EngineCommands::registerLooperCommands() {
  const auto stateResult = [this] { return CommandResult::success(processor.getLooper().describe()); };

  // The checks every action command shares: the looper has memory (it has
  // not been prepared before the audio device started, or the allocation was
  // refused), and the ring took the action.
  const auto post = [this, stateResult](const char* name, Looper::Action action) {
    Looper& looper = processor.getLooper();
    if (!looper.isReady())
      return fail(CommandError::unavailable,
                  juce::String(name) + ": the looper has no memory yet (audio has not started)");
    if (!looper.request(action))
      return fail(CommandError::unavailable,
                  juce::String(name) + ": too many looper actions are waiting for the audio thread");
    return stateResult();
  };

  commands.add({
      "looper.get",
      "Returns the looper's state as the audio thread last published it: state (empty, "
      "recording, playing, overdubbing or stopped), closing (the first recording is running on "
      "to the next beat, with quantise on), queued (actions waiting for the next audio block), "
      "channels (1 or 2), lengthFrames and lengthSeconds (of the loop as recorded), "
      "playbackLengthSeconds (twice that at half speed), recordedSeconds (while recording), "
      "positionFrames, positionSeconds and position (0 to 1) in the loop, layers (the "
      "recording and the overdubs in the sound now; the one being recorded counts), "
      "canUndo, redoLayers, canRedo, layersFull, maxLayers, maxSeconds, capacitySeconds, and "
      "options: halfSpeed, reverse, oneShot, quantise and levelDb. The loop is not saved with "
      "a preset or the plugin state.",
      {},
      false,
      CommandPermission::read,
      [stateResult](const juce::var&) { return stateResult(); },
  });

  commands.add({
      "looper.record",
      "Records the loop from the main output (after the tone stack, before the output level). "
      "Empty: starts the first recording, which sets the loop's length when it ends. "
      "Recording: ends it and starts playing (with quantise on, at the nearest whole number of "
      "beats). With a loop already there: throws it away and starts a new recording. Up to 60 "
      "seconds; at the limit the loop closes by itself. Takes effect at the start of the next "
      "audio block. Answers with looper.get's state, which does not show the action yet.",
      {},
      true,
      CommandPermission::edit,
      [post](const juce::var&) { return post("looper.record", Looper::Action::record); },
  });

  commands.add({
      "looper.overdub",
      "Layers new playing on top of the loop (up to four layers after the first recording). "
      "Recording: ends the first recording and starts overdubbing from the top of the loop. "
      "Playing or stopped: starts overdubbing. Overdubbing: ends the layer and keeps playing. "
      "Nothing is recorded in half speed or reverse: overdubbing runs at normal speed, "
      "forwards. Ignored with no loop, and when all layers are used (layersFull).",
      {},
      true,
      CommandPermission::edit,
      [post](const juce::var&) { return post("looper.overdub", Looper::Action::overdub); },
  });

  commands.add({
      "looper.play",
      "Plays the loop. Stopped: from the start (or, in reverse, from the end). Recording: ends "
      "the first recording and plays it. Overdubbing: ends the layer and keeps playing.",
      {},
      true,
      CommandPermission::edit,
      [post](const juce::var&) { return post("looper.play", Looper::Action::play); },
  });

  commands.add({
      "looper.stop",
      "Stops the loop and puts it back at the start, keeping what was recorded. Recording: ends "
      "the first recording and stops. Overdubbing: ends the layer first.",
      {},
      true,
      CommandPermission::edit,
      [post](const juce::var&) { return post("looper.stop", Looper::Action::stop); },
  });

  commands.add({
      "looper.undo",
      "Takes the last overdub layer out of the sound (the first recording stays; use "
      "looper.clear for that). While overdubbing it throws away the layer being recorded and "
      "keeps playing. The layer is kept for looper.redo until a new overdub starts.",
      {},
      true,
      CommandPermission::edit,
      [post](const juce::var&) { return post("looper.undo", Looper::Action::undo); },
  });

  commands.add({
      "looper.redo",
      "Puts back the overdub layer looper.undo took out, if no new overdub has started since.",
      {},
      true,
      CommandPermission::edit,
      [post](const juce::var&) { return post("looper.redo", Looper::Action::redo); },
  });

  commands.add({
      "looper.clear",
      "Erases the loop and all its layers and goes back to empty.",
      {},
      true,
      CommandPermission::edit,
      [post](const juce::var&) { return post("looper.clear", Looper::Action::clear); },
  });

  commands.add({
      "looper.setOptions",
      "Sets how the loop plays; give at least one. halfSpeed plays at half speed, an octave "
      "down and twice as long (playbackLengthSeconds); reverse plays backwards; oneShot plays "
      "the loop once and stops instead of repeating; quantise rounds the length of the first "
      "recording to a whole number of beats at the tempo of the moment it ends; levelDb is the "
      "loop's playback level (the rig's own sound is not touched). They take effect within a "
      "block and can change while playing. Answers with looper.get's state.",
      {P::optional("halfSpeed", T::boolean, "Half speed playback."),
       P::optional("reverse", T::boolean, "Reverse playback."),
       P::optional("oneShot", T::boolean, "Play once, then stop."),
       P::optional("quantise", T::boolean,
                   "Round the first recording's length to whole beats (applies to the next one "
                   "that ends)."),
       P::optional("levelDb", T::number, "Playback level in dB, -60 (silent) to 12.")
           .range(Looper::kMinLevelDb, Looper::kMaxLevelDb)},
      true,
      CommandPermission::edit,
      [this, stateResult](const juce::var& args) {
        bool any = false;
        Looper& looper = processor.getLooper();
        if (!args["halfSpeed"].isVoid()) {
          looper.setHalfSpeed(static_cast<bool>(args["halfSpeed"]));
          any = true;
        }
        if (!args["reverse"].isVoid()) {
          looper.setReverse(static_cast<bool>(args["reverse"]));
          any = true;
        }
        if (!args["oneShot"].isVoid()) {
          looper.setOneShot(static_cast<bool>(args["oneShot"]));
          any = true;
        }
        if (!args["quantise"].isVoid()) {
          looper.setQuantise(static_cast<bool>(args["quantise"]));
          any = true;
        }
        if (!args["levelDb"].isVoid()) {
          looper.setLevelDb(static_cast<float>(static_cast<double>(args["levelDb"])));
          any = true;
        }
        if (!any)
          return fail(CommandError::invalidArgs,
                      "looper.setOptions: give at least one of halfSpeed, reverse, oneShot, "
                      "quantise or levelDb");
        return stateResult();
      },
  });
}

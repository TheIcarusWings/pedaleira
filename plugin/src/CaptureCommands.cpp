// The agent layer's capture.* commands: the capture wizard's engine side
// (docs/capture.md). Thin calls into CaptureStudio. The user's side is local
// because it drives audio hardware and a long child process; only capture.get
// and capture.list are open to a command server client, so an agent can read
// where a capture is and what came of it.
#include "AgentHub.h"

// Processor.h first: it pulls in the JUCE module headers, which must come
// before anything that drags in <windows.h>.
#include "Processor.h"

#include "CaptureStudio.h"

namespace {

using P = CommandParam;
using T = CommandParamType;

const juce::StringArray& sources() {
  static const juce::StringArray values{"pedal", "ampHead", "ampCab", "modeller"};
  return values;
}
const juce::StringArray& methods() {
  static const juce::StringArray values{"reamp", "dryWet"};
  return values;
}
const juce::StringArray& gainTags() {
  static const juce::StringArray values{"clean", "crunch", "highGain"};
  return values;
}
const juce::StringArray& sizes() {
  static const juce::StringArray values{"full", "lite"};
  return values;
}
const juce::StringArray& steps() {
  static const juce::StringArray values{"source", "wiring", "levels", "record",
                                        "check",  "train",  "review"};
  return values;
}

CommandParam idParam() { return P::required("id", T::string, "The capture's id, from capture.create or capture.list."); }

// The fields describing what is captured, shared by create and setSource.
std::vector<CommandParam> describingParams(bool forCreate) {
  std::vector<CommandParam> params;
  params.push_back(forCreate ? P::required("name", T::string, "The capture's name, shown in the library.")
                             : P::optional("name", T::string, "The capture's name."));
  params.push_back(P::optional("source", T::string,
                               "What is captured: a pedal, an amp head (through a load box), an "
                               "amp and cab (mic'd or through a cab sim) or a modeller.")
                       .oneOf(sources()));
  params.push_back(P::optional("method", T::string,
                               "reamp plays the trainer's input file through the gear; dryWet "
                               "records the guitar before and after the gear while you play "
                               "(no re-amp box needed).")
                       .oneOf(methods()));
  params.push_back(P::optional("gainTag", T::string, "How much gain the capture has.").oneOf(gainTags()));
  params.push_back(P::optional("modelSize", T::string,
                               "Full or lite. One capture file holds both; this is the size the "
                               "wizard offers first.")
                       .oneOf(sizes()));
  params.push_back(P::optional("gearType", T::string,
                               "TONE3000's gear type: ampHead, ampCab, cabinet, pedal, outboard, "
                               "spaces or experimental. Default: from the source.")
                       .oneOf({"ampHead", "ampCab", "cabinet", "pedal", "outboard", "spaces",
                               "experimental"}));
  params.push_back(P::optional("gearMake", T::string, "The gear's make."));
  params.push_back(P::optional("gearModel", T::string, "The gear's model."));
  params.push_back(P::optional("notes", T::string, "Notes kept with the capture (knob settings, load box)."));
  return params;
}

}  // namespace

void AgentHub::registerCaptureCommands() {
  // ---- Settings and the trainer ----
  liveCommands.add({
      "capture.getSettings",
      "Returns the capture settings: the Python executable the trainer runs under (empty: "
      "python3 on the path), the trainer's standard input file and whether it is the NAM "
      "project's version 3 file (by checksum), and the level target. The standard input "
      "file is published by the NAM project and is not shipped with Pedaleira.",
      {},
      false,
      CommandPermission::local,
      [this](const juce::var&) { return captures().getSettings(); },
  });

  liveCommands.add({
      "capture.setSettings",
      "Sets any of the capture settings and returns them all (capture.getSettings).",
      {P::optional("pythonExecutable", T::string,
                   "The Python that has neural-amp-modeler 0.13.0 or newer installed; empty to "
                   "use python3 on the path.")
           .path(true),
       P::optional("standardInputFile", T::string,
                   "Absolute path of the NAM standard input file (v3_0_0.wav, also called "
                   "input.wav); empty to clear.")
           .path(),
       P::optional("levelTargetDb", T::number,
                   "The peak level a capture should reach, in dBFS; new captures start with it. "
                   "TONE3000 recommends -8, the design -12 (the default).")
           .range(-30.0, -1.0)},
      true,
      CommandPermission::local,
      [this](const juce::var& args) { return captures().setSettings(args); },
  });

  liveCommands.add({
      "capture.checkTrainer",
      "Finds out whether the trainer can be run: starts Python and imports neural-amp-modeler, "
      "which takes a few seconds. Answers pythonFound, packageFound, versionOk (version 0.13.0 "
      "or newer, the first whose standard training makes NAM A2 files), the version and a "
      "message saying what to do when it is not usable.",
      {},
      false,
      CommandPermission::local,
      [this](const juce::var&) {
        const std::atomic<bool>* cancel = &slowWorkCancel();
        return CommandWork::later(
            [this, cancel] { return CommandResult::success(captures().checkTrainer(cancel)); });
      },
  });

  // ---- Sessions ----
  liveCommands.add({
      "capture.create",
      "Starts a capture session and returns it, at the source step. Its folder is "
      "<library>/Captures/<id>; the session survives a restart.",
      describingParams(true),
      true,
      CommandPermission::local,
      [this](const juce::var& args) { return captures().create(args); },
  });

  {
    std::vector<CommandParam> params = describingParams(false);
    params.insert(params.begin(), idParam());
    liveCommands.add({
        "capture.setSource",
        "Changes what the capture is: its name, source, method, gain tag, model size, gear and "
        "notes. The source and the method can only change up to the wiring step. Returns the "
        "session.",
        params,
        true,
        CommandPermission::local,
        [this](const juce::var& args) { return captures().setSource(args); },
    });
  }

  liveCommands.add({
      "capture.get",
      "Returns one capture session: its settings, the step it is at (and what stops it from "
      "going on, blocks), levels, recording, checks, training and files. During training it "
      "carries the trainer's progress too.",
      {idParam()},
      false,
      CommandPermission::read,
      [this](const juce::var& args) { return captures().get(args["id"].toString()); },
  });

  liveCommands.add({
      "capture.list",
      "Lists the capture sessions, newest first: id, name, source, method, step, training "
      "state, ESR and the saved file when there is one.",
      {},
      false,
      CommandPermission::read,
      [this](const juce::var&) { return captures().list(); },
  });

  liveCommands.add({
      "capture.setChannels",
      "Sets which device channels the capture uses, counted from 0: for a re-amp the send "
      "output and the return input, for dry/wet the dry and wet inputs. Also the calibration: "
      "how many dBu 0 dBFS at the return input is (saved in the capture as output_level_dbu; "
      "the Volt 876 is +12.5) and, when known, at the send output (input_level_dbu). The "
      "processor has the main stereo pair only for now, so 0 and 1 are the usable channels; "
      "the answer warns about a channel the device does not have.",
      {idParam(),
       P::optional("send", T::integer, "Re-amp: the output channel the signal goes out on.").range(0, 7),
       P::optional("returnIn", T::integer, "Re-amp: the input channel the gear comes back on.").range(0, 7),
       P::optional("dry", T::integer, "Dry/wet: the input with the guitar before the gear.").range(0, 7),
       P::optional("wet", T::integer, "Dry/wet: the input with the gear.").range(0, 7),
       P::optional("calibrationDbu", T::number, "dBu at 0 dBFS on the return input.").range(-40.0, 60.0),
       P::optional("sendCalibrationDbu", T::number, "dBu at 0 dBFS on the send output.")
           .range(-40.0, 60.0)},
      true,
      CommandPermission::local,
      [this](const juce::var& args) { return captures().setChannels(args); },
  });

  liveCommands.add({
      "capture.setLevels",
      "Sets the send level of the test tone and of the signal that is recorded (its peak, in "
      "dBFS), the level target and band the return has to reach, and for dry/wet how long to "
      "record. The target defaults to the capture settings' level target, the band to 3 dB "
      "either side. Changing the send level or the target clears the peak readings.",
      {idParam(),
       P::optional("sendDb", T::number, "The send level, dBFS.").range(-80.0, 0.0),
       P::optional("targetDb", T::number, "The peak the return should reach, dBFS.").range(-30.0, -1.0),
       P::optional("bandDb", T::number, "How far either side of the target counts as in range.")
           .range(0.5, 6.0),
       P::optional("recordSeconds", T::number, "Dry/wet: how long to record (default 180).")
           .range(10.0, 600.0)},
      true,
      CommandPermission::local,
      [this](const juce::var& args) { return captures().setLevels(args); },
  });

  liveCommands.add({
      "capture.setStep",
      "Moves the wizard to a step: one forward, when what the current step needs holds (the "
      "answer of a refused move says what is missing), or any step back, which throws away "
      "what the steps left behind had made (going back to record discards the recording). "
      "Steps: source, wiring, levels, record, check, train, review.",
      {idParam(),
       P::required("step", T::string, "The step to go to.").oneOf(steps()),
       P::optional("overrideChecks", T::boolean,
                   "Going on from the check step although the checks failed.")
           .withDefault(false)},
      true,
      CommandPermission::local,
      [this](const juce::var& args) { return captures().setStep(args); },
  });

  // ---- Levels ----
  liveCommands.add({
      "capture.startTone",
      "Starts the levels run in the levels step: the rig is muted, a 1 kHz test tone plays on "
      "the send channel at the send level and the return is metered (for dry/wet there is no "
      "tone: the two inputs are metered while the guitar is played). Poll capture.levelStatus. "
      "Standalone application only.",
      {idParam(), P::optional("sendDb", T::number, "The send level, dBFS; default: the session's.")
                      .range(-80.0, 0.0)},
      true,
      CommandPermission::local,
      [this](const juce::var& args) { return captures().startTone(args); },
  });

  liveCommands.add({
      "capture.stopTone",
      "Ends the levels run (the readings are kept in the session) and unmutes the rig.",
      {idParam()},
      true,
      CommandPermission::local,
      [this](const juce::var& args) { return captures().stopTone(args["id"].toString()); },
  });

  liveCommands.add({
      "capture.levelStatus",
      "Returns the levels: the return's current peak, peak hold, RMS and clipped samples "
      "(dry/wet: both inputs), the target band and whether the hold is inside it "
      "(state silent, low, high, clipping or inRange), and the noise floor when measured. "
      "Each call also records the readings in the session, which is what lets the wizard "
      "leave the levels step.",
      {idParam(),
       P::optional("resetHold", T::boolean, "Clear the peak hold and the clip count first.")
           .withDefault(false)},
      false,
      CommandPermission::local,
      [this](const juce::var& args) { return captures().levelStatus(args); },
  });

  liveCommands.add({
      "capture.measureNoiseFloor",
      "Mutes the tone for a moment and measures the return's noise (RMS) over the given time. "
      "The result shows in capture.levelStatus as noiseFloor.",
      {idParam(), P::optional("seconds", T::number, "How long to measure.").range(0.5, 10.0).withDefault(2.0)},
      true,
      CommandPermission::local,
      [this](const juce::var& args) { return captures().measureNoise(args); },
  });

  // ---- Recording ----
  liveCommands.add({
      "capture.startRecording",
      "Starts the recording in the record step. Re-amp: the trainer's standard input file "
      "(capture.setSettings) plays on the send channel at the send level and the return is "
      "recorded, sample for sample with what is played; it takes about three minutes. "
      "Dry/wet: both inputs are recorded together while you play, for the recording time. "
      "The audio device must run at 48 kHz. Poll capture.recordingStatus; the files are "
      "written when it ends.",
      {idParam(),
       P::optional("allowUnrecognisedInput", T::boolean,
                   "Use a standard input file whose checksum is not the NAM project's version 3 "
                   "(the trainer is likely to refuse it).")
           .withDefault(false)},
      true,
      CommandPermission::local,
      [this](const juce::var& args) { return captures().startRecording(args); },
  });

  liveCommands.add({
      "capture.cancelRecording",
      "Stops a recording that is going and throws away what was recorded; also discards a "
      "finished recording so that it can be made again (in the record or check step).",
      {idParam()},
      true,
      CommandPermission::local,
      [this](const juce::var& args) { return captures().cancelRecording(args["id"].toString()); },
  });

  liveCommands.add({
      "capture.recordingStatus",
      "Returns the recording's state (none, recording, writing, complete, failed), progress "
      "from 0 to 1, seconds recorded and left, the return's peak so far, clipped samples and, "
      "for a re-amp, the latency measured once enough has come in.",
      {idParam()},
      false,
      CommandPermission::local,
      [this](const juce::var& args) { return captures().recordingStatus(args["id"].toString()); },
  });

  // ---- Checks ----
  liveCommands.add({
      "capture.runChecks",
      "Runs the data checks on the finished recording and returns the session with the report "
      "(checks): length, peak level against the target band, clipping, noise floor, latency, "
      "DC offset and, for dry/wet, that both inputs have signal and the wet is not a copy of the "
      "dry. Each check is pass, warn or fail; the verdict is the worst. Moves a capture at the "
      "record step on to the check step.",
      {idParam()},
      true,
      CommandPermission::local,
      [this](const juce::var& args) { return captures().runChecks(args); },
  });

  // ---- Training ----
  liveCommands.add({
      "capture.startTraining",
      "Starts the trainer (neural-amp-modeler 0.13.0 or newer, whose standard training makes "
      "NAM A2 files) as a child process and returns at once. From the check step, the checks "
      "must not have failed unless overrideChecks is set. Poll capture.trainingStatus. "
      "Training an already trained capture again (from review) starts over.",
      {idParam(),
       P::optional("epochs", T::integer, "Training epochs (the trainer's default is 100).")
           .range(1, 2000),
       P::optional("overrideChecks", T::boolean, "Train although our data checks failed.")
           .withDefault(false),
       P::optional("ignoreTrainerChecks", T::boolean,
                   "Tell the trainer to go on although its own data checks fail (at your own risk).")
           .withDefault(false),
       P::optional("latency", T::integer,
                   "Re-amp: the delay in samples, instead of the trainer's own calibration.")
           .range(-10000, 10000)},
      true,
      CommandPermission::local,
      [this](const juce::var& args) { return captures().startTraining(args); },
  });

  liveCommands.add({
      "capture.cancelTraining",
      "Stops the trainer (the child process is killed) and returns the training status.",
      {idParam()},
      true,
      CommandPermission::local,
      [this](const juce::var& args) { return captures().cancelTraining(args["id"].toString()); },
  });

  liveCommands.add({
      "capture.trainingStatus",
      "Returns the training state (none, checking, running, done, failed, cancelled), the "
      "epoch and progress, the time left, the validation ESR with its band (great, notBad, "
      "mightBeOkay, poor, failed), the latency the trainer used, whether its own data checks "
      "passed, the last lines of its output, and an error message when it failed. A finished "
      "training whose file is not NAM A2 is a failure. Done moves the capture to review.",
      {idParam()},
      false,
      CommandPermission::local,
      [this](const juce::var& args) { return captures().trainingStatus(args["id"].toString()); },
  });

  // ---- Review ----
  liveCommands.add({
      "capture.review",
      "Returns what the review step shows: the trained file, the ESR and its band (great under "
      "0.01, not bad under 0.035, might be okay under 0.1, poor under 0.3, else failed), the "
      "calibration and the metadata the saved file will carry.",
      {idParam()},
      false,
      CommandPermission::local,
      [this](const juce::var& args) { return captures().review(args["id"].toString()); },
  });

  liveCommands.add({
      "capture.addToRig",
      "Adds the trained capture to the rig being played as a NAM block (chain.addToneFromFiles), "
      "so it can be compared with the real device. Returns what that command returns.",
      {idParam()},
      true,
      CommandPermission::local,
      [this](const juce::var& args) { return captures().addToRig(args["id"].toString()); },
  });

  liveCommands.add({
      "capture.save",
      "Saves the capture into the library: the trained file moves to <library>/Captures/<name>.nam "
      "(a copy with the name, gear, gain tag and calibration set in its metadata when the trainer "
      "did not write them). Returns the file.",
      {idParam()},
      true,
      CommandPermission::local,
      [this](const juce::var& args) { return captures().save(args); },
  });

  liveCommands.add({
      "capture.delete",
      "Deletes a capture session's folder (the recordings and an unsaved model); a capture "
      "already saved to the library is a file of its own and stays. Stops a run that belongs "
      "to it.",
      {idParam()},
      true,
      CommandPermission::local,
      [this](const juce::var& args) { return captures().remove(args["id"].toString()); },
  });
}

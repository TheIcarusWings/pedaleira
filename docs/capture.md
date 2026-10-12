# Capture: record a device, train a NAM A2 model, review it

The engine side of the capture wizard (Phase 4). A capture session is the state of one
capture; the audio, the data checks, the trainer and the review hang off it. The wizard
screens (the boards in the design canvas, "Capture 1" to "Capture 7") are not built: they
drive the `capture.*` commands below. Everything here is tested without hardware, against a
simulated device and a stand-in trainer (`test/src/capture_tests.cpp`).

| Piece | File | What it is |
|---|---|---|
| Session | `CaptureSession` | The data of one capture, the steps, what each needs, its JSON. |
| Audio | `CaptureAudio` | The audio-thread half: test tone, meters, re-amp play and record, dry/wet record. |
| Checks | `CaptureChecks` | Our fast data checks on a recording. Pure functions. |
| Studio | `CaptureStudio` | Runs a session: sessions on disk, the audio run, checks, training, review. |
| Trainer | `TrainerRunner` | The NAM trainer as a child process; reads the model it makes. |
| Commands | `CaptureCommands.cpp` | `capture.*`, registered by the agent layer. |

## The session

A session lives in a folder under the library and survives a restart:

```
<library>/Captures/<id>/session.json    the session
<library>/Captures/<id>/return.wav      re-amp: what came back, as long as the input file
<library>/Captures/<id>/dry.wav, wet.wav   dry/wet: the two inputs
<library>/Captures/<id>/training/       the trainer's work, removed when it is done
<library>/Captures/<id>/model.nam      the trained model, until it is saved
<library>/Captures/<Readable name>.nam   a saved capture
```

Recordings are 24-bit WAV, 48 kHz, mono (what the trainer reads and what TONE3000 asks for).
`assets.list` and `assets.tagSummary` skip the working folders, so the recordings are not
listed as IRs; a saved capture is a file in `Captures/` itself and is listed.

`session.json` holds: `id`, `name`, `notes`, `source` (`pedal`, `ampHead`, `ampCab`,
`modeller`), `method` (`reamp`, `dryWet`), `gainTag` (`clean`, `crunch`, `highGain`),
`modelSize` (`full`, `lite`; one file holds both, this is the size offered first), `gear`
(`type` from TONE3000's list: `ampHead`, `ampCab`, `cabinet`, `pedal`, `outboard`, `spaces`,
`experimental`; `make`, `model`), `channels`, `calibration`, `levels`, `step`, `recording`,
`checks` (the report), `checksOverridden`, `training`, `files`.

Channels are numbers counted from 0: `send` and `returnIn` for a re-amp, `dry` and `wet` for
dry/wet. **Limit:** the processor has the host's main stereo pair only (grid stage 8, the
device channels, is not done), so 0 and 1 are the usable values; a channel beyond the buffer
falls back to the last one, and `capture.setChannels` says so in `warnings`. When stage 8 gives
the audio callback a wider buffer, nothing here changes: the fields already carry the numbers.

Calibration: `calibrationDbu` is what 0 dBFS at the return input is in dBu (the Volt 876:
+12.5, the default), saved in the capture as `output_level_dbu`; `sendCalibrationDbu`, when
known, as `input_level_dbu`.

### The steps

The steps are `source`, `wiring`, `levels`, `record`, `check`, `train`, `review`. A step can
only advance, one at a time, when its condition holds; going back is always allowed and throws
away what the steps left behind had made (back to `record` discards the recording, its checks
and the training).

| Leaving | Needs |
|---|---|
| source | a name and a gear type |
| wiring | for dry/wet, different dry and wet channels |
| levels | the levels measured and in range: the peak hold of the return (dry/wet: of both inputs) inside target plus or minus band, and no clipped sample |
| record | a finished recording |
| check | a report whose verdict is not `fail`, or `overrideChecks` |
| train | a finished training (the trained file is NAM A2) |

`capture.setStep` moves; some commands move on their own because their result is the
condition: `capture.runChecks` takes a capture at `record` to `check`, `capture.startTraining`
takes one at `check` to `train` (checking the same rule), and a finished training takes the
capture to `review`.

The level target is a setting (`capture.setSettings`, `levelTargetDb`), **default -12 dB
peak**, with an in-range band of plus or minus 3 dB; both are per capture too
(`capture.setLevels`). TONE3000's guide says -8; Pedaleira uses -12. It is a setting, not a constant, so a
player who prefers -8 can set it.

## The audio

`CaptureAudio` is a member of the processor, next to `InputRecorder`. `processBlock` holds a
`CaptureAudio::BlockScope` on it: on entry it reads the input channels before anything
touches them and hands the rig silence, and on every way out of the callback it replaces the
output with what the run plays (all channels silent but the send channel). Idle, that is one
atomic read. While a run is active the rig is muted ("Audio from other rows is muted while
recording").

Modes:

- **levels** A 1 kHz sine on the send channel at the send level (a smoothed gain, about
  10 ms, so a change does not click); the return (channel A) and for dry/wet the second
  input (channel B) are metered: block peak, peak hold, RMS (about 300 ms) and a count of
  samples at or above -0.1 dBFS. `measureNoiseFloor` holds the tone off for a moment and
  reads the return's RMS after 0.3 s of settling. Dry/wet plays nothing: it only meters the
  two inputs while the guitar is played.
- **play** (re-amp) The standard input file, scaled so that its peak is the send level, goes
  out on the send channel after 0.5 s of silence; the return is recorded from the first
  sample of that pre-roll for pre-roll, file and 0.25 s more. The saved `return.wav` is cut
  from the first played sample to exactly the length of the file, which is what the trainer
  wants (it takes an output as long as the input, or up to a second longer, never shorter).
  Position `i` of the recording is the device's answer to position `i - delay` of what was
  played, where the delay is the device's own latency, which the checks measure. The wizard's
  live "latency measured" reading is taken once five seconds plus the search range have come
  in.
- **listen** (dry/wet) Both inputs are recorded together for the planned time (default
  3 minutes, 10 s to 10 min), nothing is played.

Real-time rules: `beginBlock` and `endBlock` take no lock, allocate nothing, do no I/O and
throw nothing. Every buffer is prepared by the message thread before the run starts, while
the audio thread is known to be out (the same handshake as `InputRecorder`: leave the mode,
wait for `busy` to clear, then replace). Progress, meters and states are atomics, readable
from any thread. A finished recording is written to disk by a thread of its own, never by the
message thread or the audio thread.

Only the standalone application captures (and the headless host and tests, whose wrapper is
undefined); a plugin instance inside a DAW refuses to start a run. Recording needs the audio
device at 48 kHz; the tone and the meters work at any rate. One audio run at a time.

## The standard input file

The re-amp plays the file the trainer recognises. Findings, from the trainer's source
(neural-amp-modeler 0.13.0, `nam/train/core.py`):

- The trainer identifies its input file by the MD5 of the whole file (the "strong match") and,
  failing that, by hashes of stretches of the decoded samples (the "weak match"). Version 3
  (`v3_0_0.wav`, also published as `input.wav`; 3:10 at 48 kHz, MD5
  `36cd1af62985c2fac3e654333e36431e`) is the only one 0.13.0 trains from without failing its
  own checks; versions 1, 2 and Proteus are deprecated there.
- The file is not in the pip package (`nam/train/_resources` holds the model configuration
  only). The project publishes it as a download link in its documentation (Google Drive).
- No licence is stated for the file, and it is large (about 27 MB).

**Decision:** it is not committed and not downloaded by the application. The path is a
setting, asked for once (`capture.setSettings`, `standardInputFile`). `capture.getSettings`
reports whether the file is there, readable and the known version 3 by checksum.
`capture.startRecording` refuses a file whose checksum is not a known one unless
`allowUnrecognisedInput` is set, because the trainer would refuse it after the three minutes
are spent. (Only the strong match is checked here; a re-encoded copy that the trainer would
weak-match is held back too, and the override covers it.)

## The data checks

`capture.runChecks` runs on the finished recording (off the message thread: it reads the
files) and stores the report in the session. Each check is `pass`, `warn` or `fail` with a
value and a message; the verdict is the worst. Re-amp:

| id | Pass | Warn | Fail |
|---|---|---|---|
| `length` | as long as the input file | up to 1 s longer | shorter, or over 1 s longer |
| `level` | peak within target plus or minus band | up to one band width outside | further out, or almost no signal |
| `clipping` | no sample at or above -0.1 dBFS | 1 or 2 places | 3 places or more |
| `noise` | quietest 100 ms at least 45 dB under the peak | 35 to 45 dB | under 35 dB |
| `latency` | found, up to 9000 samples (what the trainer's calibration can find), match over 0.2 | weak match | no match, or longer than 9000 samples |
| `dc` | under -50 dBFS | -50 to -40 | over -40 |

The latency is a cross-correlation (FFT) of what was played and what came back over the first
ten seconds, reported in samples and milliseconds with its confidence. For dry/wet the same
checks run on each input (`level.dry`, `level.wet`, `clipping.dry`, ...; a noise failure is
only a warning because playing may leave no quiet stretch), plus `length` (120 s or more
passes, under 20 s fails), `latency` (the wet behind the dry, negative if before) and `copy`
(the wet differs from the dry once the best gain and delay are taken out: a residual under
-33 dB fails, under -23 dB warns).

The trainer runs its own checks as well (a replicate comparison of the validation signal);
its verdict is shown after training (`trainerChecksPassed`, and a failed run says so).

## The trainer

`TrainerRunner` starts Python as a child process with a small helper script
(`train_helper.py`, written into the capture folder together with `job.json`; both can be run
by hand). It never links or copies the trainer. The Python is the `pythonExecutable` setting,
else `python3` (`python` on Windows). Before every run it checks
`python -c "import nam; print(nam.__version__)"` and fails with a message saying what to do if
Python or the package is missing or too old.

**Version 0.13.0 or newer is required.** That is the first release whose standard training
(`nam.train.core.train`, the same code as the `nam` GUI) uses a packed WaveNet and exports
`"architecture": "SlimmableContainer"`: the Full and Lite models in one file, which is NAM A2,
the only format the engine loads. Older trainers make single-model files that do not load. After
training the produced file is read and its `architecture` must be `SlimmableContainer`; anything
else fails the run with a message naming the architecture and the version needed.

- **Re-amp** calls `nam.train.core.train` on the standard input file and `return.wav`, exports
  with `net.export` like the GUI, and passes the capture's metadata as the trainer's own
  `UserMetadata` (name, gear type, make, model, tone type, `input_level_dbu`,
  `output_level_dbu`). The trainer calibrates the delay itself from the file's blips.
- **Dry/wet** cannot use the standard path (the dry take is not a recognised input file), so the
  helper builds the same configuration the standard path does (`core._get_configs`), changes the
  data split (validation is the last tenth, 9 to 30 s, no required pre-silence) and runs the full
  trainer (`nam.train.full.main`), with the delay our check measured. The validation ESR comes
  from the best checkpoint's name. This path touches private names of the trainer and is the
  part most likely to need adjusting against a real installation.
- Progress: a Lightning callback added to the trainer's callbacks prints
  `PEDALEIRA_EPOCH {...}` after each epoch; the progress bar's "Epoch N" is the fallback. The
  result is one `PEDALEIRA_RESULT {...}` line (ok, ESR, delay, whether the trainer's checks
  passed, the model file).
- Cancel kills the child process. The runner never blocks the message thread; `capture.trainingStatus`
  reads its state.
- Epochs default to the trainer's own 100.

ESR bands: under 0.01 great, under 0.035 not bad, under 0.1 might be okay, under 0.3 poor,
above that failed (`great`, `notBad`, `mightBeOkay`, `poor`, `failed` in the commands).

### Metadata

The trainer writes the metadata it is given. On save, the model is moved into the library
as it is when its `metadata` object already says what the wizard knows (name, gear type,
make, model, tone type, calibration). When it does not (an edit in the review step, or a
trainer that left fields out), the file is **copied** with those keys set in the `metadata`
object, and nothing else of the file is changed (the weights are the same bytes). The model
that was trained stays as it was until the copy is whole.

## Review and save

`capture.review` returns the file, the ESR and its band, the calibration and the metadata that
will be saved. `capture.addToRig` adds the file to the rig being played with
`chain.addToneFromFiles`, so the user can A/B it against the real device (the level matching
and the blind test of the board are UI). `capture.save` moves the model to
`<library>/Captures/<name>.nam` (a readable name, numbered if it exists), `capture.delete`
removes the working folder and keeps a saved capture.

## Commands

All are `local` (they drive audio hardware and a long process) except `capture.get` and
`capture.list`, which are `read`. `docs/commands.md` is the reference.

| Command | What it does |
|---|---|
| `capture.getSettings`, `capture.setSettings` | Python, standard input file, level target |
| `capture.checkTrainer` | Python and the trainer's version (slow: imports the package) |
| `capture.create`, `capture.setSource`, `capture.setChannels`, `capture.setLevels` | The session's fields |
| `capture.get`, `capture.list` | Read sessions |
| `capture.setStep` | Move a step on (conditions) or back |
| `capture.startTone`, `capture.stopTone`, `capture.levelStatus`, `capture.measureNoiseFloor` | The levels run |
| `capture.startRecording`, `capture.cancelRecording`, `capture.recordingStatus` | The recording |
| `capture.runChecks` | The data checks (slow) |
| `capture.startTraining`, `capture.cancelTraining`, `capture.trainingStatus` | The trainer |
| `capture.review`, `capture.addToRig`, `capture.save`, `capture.delete` | The end |

## What needs hardware, and João

Not verified, because it needs the Volt 876 or a real trainer:

- Actual device I/O: the tone on a real output, the return on a real input, the real round trip
  latency, the 48 kHz device setting, the 0.5 s pre-roll against a real amp, and the first
  recording through `capture.startRecording` on the Volt. The simulated device checks the logic
  and the sample alignment of the recording, not the converters.
- A real training run. The helper script is written against the 0.13.0 source and untested
  against an installed trainer; the dry/wet path especially. A first run on João's machine
  (`pip install neural-amp-modeler`, the v3 input file, one capture of a pedal or an amp) is the
  test. `capture.checkTrainer` says whether Python and the package are found.
- Multi-channel device routing (grid stage 8).
